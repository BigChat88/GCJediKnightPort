// GameCube heap (on libogc2).
//
// libogc2's malloc is dlmalloc 2.8.6: one heap for everything (newlib's own
// _malloc_r included), grown by sbrk over the MEM1 arena, chunk footers checked
// on every free (FOOTERS), mutex locks -- interrupts stay on. That covers what
// the port had to build itself on devkitPro's libogc (newlib's malloc failing
// big requests with plenty free, damage found late, the audio DSP starved by
// heap work with interrupts off). So the main heap is libogc2's:
//
//  - malloc & co are still --wrap'ed, only to count the bytes in use (the caches
//    check the free memory at every sound and texture load: it must be cheap,
//    and a walk over the heap isn't);
//  - the sound arena is an mspace of libogc2's dlmalloc in one block of the heap;
//  - the statistics come from dlmalloc's footprint and, for the largest free
//    block (diagnostics only), a walk with malloc_inspect_all.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <gccore.h>
#include <ogc/irq.h>

// libogc2's dlmalloc (built with USE_DL_PREFIX; its <malloc.h> clashes with
// the engine's headers, so declared here).
typedef void* mspace;
size_t dlmalloc_usable_size(const void*);
size_t dlmalloc_footprint(void);
void   dlmalloc_inspect_all(void (*handler)(void*, void*, size_t, void*), void* arg);
mspace create_mspace_with_base(void* base, size_t capacity, int locked);
void*  mspace_memalign(mspace msp, size_t alignment, size_t bytes);
void   mspace_free(mspace msp, void* mem);
void*  mspace_malloc(mspace msp, size_t bytes);
void*  mspace_realloc(mspace msp, void* mem, size_t newsize);
size_t mspace_footprint(mspace msp);
size_t mspace_set_footprint_limit(mspace msp, size_t bytes);

// An arena of its own: never more than the block it was given. (libogc2's
// dlmalloc otherwise grows a full mspace with sbrk -- out of the main heap's end,
// in pieces between the main heap's own, splitting it into segments that never
// join again -- instead of failing, so that the caller falls back to the heap.)
static mspace gcHeap_Arena(void* pBlock, size_t size)
{
    mspace ms = create_mspace_with_base(pBlock, size, 1);
    if (ms) mspace_set_footprint_limit(ms, mspace_footprint(ms));
    return ms;
}

void* __real_malloc(size_t);
void  __real_free(void*);
void* __real_realloc(void*, size_t);
void* __real_calloc(size_t, size_t);
void* __real_memalign(size_t, size_t);

#ifdef GC_MEM_TRACK
void gcMw_Track(void* p, uint32_t size, uintptr_t pc);
void gcMw_Untrack(void* p);
#define GCHEAP_TRACK(p, n, pc) gcMw_Track((p), (uint32_t)(n), (pc))
#define GCHEAP_UNTRACK(p)      gcMw_Untrack(p)
#else
#define GCHEAP_TRACK(p, n, pc)
#define GCHEAP_UNTRACK(p)
#endif

volatile int  gcHeap_bCorrupt = 0;  // (dlmalloc traps on damage itself)
// Which call last returned NULL (shown in level load errors): 'm' malloc,
// 'a' memalign, 'R' realloc.
volatile char gcHeap_failPath = 0;

// Bytes in use: usable size plus dlmalloc's chunk header.
#define GCHEAP_CHUNK_OVERHEAD 8
static volatile uint32_t gcHeap_usedBytes = 0;

static inline uint32_t gcHeap_ChunkBytes(void* p)
{
    return (uint32_t)dlmalloc_usable_size(p) + GCHEAP_CHUNK_OVERHEAD;
}

// (The sound and file arenas, defined further down, lend their free space when
// the main heap has none -- see gcHeap_LastResort; free() and realloc() take
// those blocks back. They are not counted as used: each arena already is, as
// one block.)
static mspace    gcHeap_sndMs, gcHeap_fileMs;
static uintptr_t gcHeap_sndLo, gcHeap_sndHi, gcHeap_fileLo, gcHeap_fileHi;

static inline int gcHeap_InSnd(const void* p)
{
    return (uintptr_t)p >= gcHeap_sndLo && (uintptr_t)p < gcHeap_sndHi;
}

static inline int gcHeap_InFileArena(const void* p)
{
    return (uintptr_t)p >= gcHeap_fileLo && (uintptr_t)p < gcHeap_fileHi;
}

// The arena that lent block p, NULL for any other block.
static inline mspace gcHeap_Lender(const void* p)
{
    return gcHeap_InSnd(p) ? gcHeap_sndMs : gcHeap_InFileArena(p) ? gcHeap_fileMs : NULL;
}

static inline void gcHeap_Add(void* p)
{
    if (!p || gcHeap_Lender(p)) return;
    uint32_t n = gcHeap_ChunkBytes(p);
    u32 l = IRQ_Disable();
    gcHeap_usedBytes += n;
    IRQ_Restore(l);
}

static inline void gcHeap_Sub(void* p)
{
    if (!p || gcHeap_Lender(p)) return;
    uint32_t n = gcHeap_ChunkBytes(p);
    u32 l = IRQ_Disable();
    gcHeap_usedBytes -= n;
    IRQ_Restore(l);
}

// Small-block arena: every block of up to GCHEAP_SMALL_MAX bytes comes from one
// region taken at the first allocation (the bottom of the heap). Small blocks
// made the first time something shows up in a level -- a HUD icon, a GUI
// bitmap's header, a cog's stack -- outlive the level, and landed among its big
// arrays: tiny islands all over the free memory once the level was gone, which
// after a few levels left no free piece of 1 MiB (level 9: 15 MiB free, the
// largest piece 811 KiB). Here they can't split the big blocks' memory. Level 1
// peaks at 1.3 MiB of small blocks (~1.7 MiB as dlmalloc chunks); once the arena
// is full, the main heap (counted in gcHeap_smallOverflow).
// Aligned blocks of up to GCHEAP_SMALL_ALIGNED_MAX too: small bitmaps' pixels
// (HUD icons, ~800 bytes) stayed behind the same way.
// (The arena isn't counted as used: its blocks are, one by one.)
#define GCHEAP_SMALL_MAX         256
#define GCHEAP_SMALL_ALIGNED_MAX 1024
#define GCHEAP_SMALL_ARENA (2048 * 1024)
static mspace    gcHeap_smallMs = NULL;
static uintptr_t gcHeap_smallLo = 0, gcHeap_smallHi = 0;
static int       gcHeap_bSmallTried = 0;
uint32_t         gcHeap_smallOverflow = 0; // small blocks the full arena sent to the main heap

static inline int gcHeap_InSmall(const void* p)
{
    return (uintptr_t)p >= gcHeap_smallLo && (uintptr_t)p < gcHeap_smallHi;
}

static void gcHeap_SmallInit(void)
{
    if (!gcHeap_bSmallTried) {
        gcHeap_bSmallTried = 1;
        void* pBlock = __real_memalign(32, GCHEAP_SMALL_ARENA);
        if (pBlock) {
            gcHeap_smallMs = gcHeap_Arena(pBlock, GCHEAP_SMALL_ARENA);
            if (gcHeap_smallMs) {
                gcHeap_smallLo = (uintptr_t)pBlock;
                gcHeap_smallHi = gcHeap_smallLo + GCHEAP_SMALL_ARENA;
            }
            else __real_free(pBlock);
        }
    }
}

static void* gcHeap_SmallAlloc(size_t n)
{
    gcHeap_SmallInit();
    return gcHeap_smallMs ? mspace_malloc(gcHeap_smallMs, n ? n : 1) : NULL;
}

// Last resort when the main heap has no room: the free space of the sound arena,
// then of the file arena, then of the small-block arena. MotS's biggest levels (s2l1_palace,
// s2l2b_katraasii) needed a few hundred KiB more than the main heap had while
// both arenas were mostly empty -- no sound is loaded during a level load. A
// level's data that lands there leaves less room for sounds, which then fall
// back to the main heap or get dropped and come back from the ARAM cache.
uint32_t gcHeap_lastResortCount = 0; // blocks handed out this way (diagnostics)

static void* gcHeap_LastResort(size_t al, size_t n)
{
    if (al < 8) al = 8;
    void* p = gcHeap_sndMs ? mspace_memalign(gcHeap_sndMs, al, n ? n : 1) : NULL;
    if (!p && gcHeap_fileMs) p = mspace_memalign(gcHeap_fileMs, al, n ? n : 1);
    if (!p && gcHeap_smallMs) p = mspace_memalign(gcHeap_smallMs, al, n ? n : 1);
    if (p) gcHeap_lastResortCount++;
    return p;
}

void* __wrap_malloc(size_t n)
{
    void* p = n <= GCHEAP_SMALL_MAX ? gcHeap_SmallAlloc(n) : NULL;
    if (!p && n <= GCHEAP_SMALL_MAX) gcHeap_smallOverflow++;
    if (!p) p = __real_malloc(n);
    if (!p) p = gcHeap_LastResort(8, n);
    gcHeap_Add(p);
    GCHEAP_TRACK(p, n, (uintptr_t)__builtin_return_address(0));
    if (!p && n) gcHeap_failPath = 'm';
    return p;
}

void* __wrap_calloc(size_t a, size_t b)
{
    void* p = NULL;
    if ((uint64_t)a * b <= GCHEAP_SMALL_MAX) {
        p = gcHeap_SmallAlloc(a * b);
        if (p) memset(p, 0, a * b);
    }
    if (!p) p = __real_calloc(a, b);
    if (!p && (uint64_t)a * b <= 0x7FFFFFFF) {
        p = gcHeap_LastResort(8, a * b);
        if (p) memset(p, 0, a * b);
    }
    gcHeap_Add(p);
    GCHEAP_TRACK(p, a * b, (uintptr_t)__builtin_return_address(0));
    return p;
}

void* __wrap_memalign(size_t al, size_t n)
{
    void* p = NULL;
    if (n <= GCHEAP_SMALL_ALIGNED_MAX && al <= 32) {
        gcHeap_SmallInit();
        if (gcHeap_smallMs) p = mspace_memalign(gcHeap_smallMs, al, n ? n : 1);
    }
    if (!p) p = __real_memalign(al, n);
    if (!p) p = gcHeap_LastResort(al, n);
    gcHeap_Add(p);
    GCHEAP_TRACK(p, n, (uintptr_t)__builtin_return_address(0));
    if (!p && n) gcHeap_failPath = 'a';
    return p;
}

// (libdvm allocates with aligned_alloc: libogc2's calls memalign, so it is counted
// by __wrap_memalign -- wrapping it too counted every libdvm block twice.)

void __wrap_free(void* p)
{
    if (!p) return;
    GCHEAP_UNTRACK(p);
    gcHeap_Sub(p);
    mspace lender = gcHeap_Lender(p);
    if (lender) mspace_free(lender, p); // (lent by an arena)
    else if (gcHeap_InSmall(p)) mspace_free(gcHeap_smallMs, p);
    else __real_free(p);
}

// (dlmalloc's realloc grows a block in place when it can, before moving it.)
void* __wrap_realloc(void* old, size_t n)
{
    if (!old) return __wrap_malloc(n);
    if (!n) { __wrap_free(old); return NULL; }
    uint32_t before = gcHeap_Lender(old) ? 0 : gcHeap_ChunkBytes(old);
    void* p;
    if (gcHeap_InSmall(old) || gcHeap_Lender(old)) {
        mspace ms = gcHeap_InSmall(old) ? gcHeap_smallMs : gcHeap_Lender(old);
        // Stays small (or was lent by an arena): in the arena; grown past it (or
        // the arena full): moved out.
        p = (n <= GCHEAP_SMALL_MAX || ms != gcHeap_smallMs) ? mspace_realloc(ms, old, n) : NULL;
        if (!p) {
            p = __wrap_malloc(n);
            if (!p) { gcHeap_failPath = 'R'; return NULL; }
            uint32_t have = (uint32_t)dlmalloc_usable_size(old);
            memcpy(p, old, have < n ? have : n);
            __wrap_free(old);
            return p;
        }
    }
    else {
        p = __real_realloc(old, n);
        if (!p) {
            // The main heap has no room: anywhere __wrap_malloc finds some.
            p = __wrap_malloc(n);
            if (!p) { gcHeap_failPath = 'R'; return NULL; }
            uint32_t have = (uint32_t)dlmalloc_usable_size(old);
            memcpy(p, old, have < n ? have : n);
            __wrap_free(old);
            return p;
        }
    }
    uint32_t after = gcHeap_Lender(p) ? 0 : gcHeap_ChunkBytes(p);
    u32 l = IRQ_Disable();
    gcHeap_usedBytes += after - before;
    IRQ_Restore(l);
#ifdef GC_MEM_TRACK
    GCHEAP_UNTRACK(old);
    GCHEAP_TRACK(p, n, (uintptr_t)__builtin_return_address(0));
#endif
    return p;
}

// Sound arena: decoded sound PCM lives in one fixed block of the heap with its
// own allocator. Sounds come and go all through a level and, spread over the
// main heap, they cut its free memory into pieces (level 1's TIE bomber scene:
// the largest free block fell from 3.8 to 0.7 MiB). Kept in one place, their
// churn only fragments the arena. Full arena: the main heap.
#define GCHEAP_SND_ARENA (1536 * 1024)
static int       gcHeap_bSndTried = 0;

uint32_t gcHeap_SndArenaSize(void)
{
    return gcHeap_sndMs ? GCHEAP_SND_ARENA : 0;
}

void* gcHeap_SndAlloc(size_t n)
{
    if (!gcHeap_bSndTried) {
        gcHeap_bSndTried = 1;
        void* pBlock = __wrap_memalign(32, GCHEAP_SND_ARENA);
        if (pBlock) {
            gcHeap_sndMs = gcHeap_Arena(pBlock, GCHEAP_SND_ARENA);
            if (gcHeap_sndMs) {
                gcHeap_sndLo = (uintptr_t)pBlock;
                gcHeap_sndHi = gcHeap_sndLo + GCHEAP_SND_ARENA;
            }
            else __wrap_free(pBlock);
        }
    }
    void* p = gcHeap_sndMs ? mspace_memalign(gcHeap_sndMs, 32, n) : NULL;
    return p ? p : __wrap_memalign(32, n);
}

// A block from the sound arena only (32-byte aligned), NULL when it has no room
// (no main-heap fallback). Memory of last resort for a cutscene's decoder or a
// dialog's backdrop when the main heap is full after a level load: idle sounds
// can be dropped (they come back from the ARAM cache), which leaves the arena
// mostly free. Freed by free() like any other block.
void* gcHeap_SndArenaAlloc(size_t n)
{
    void* p = gcHeap_sndMs ? mspace_memalign(gcHeap_sndMs, 32, n) : NULL;
    GCHEAP_TRACK(p, n, (uintptr_t)__builtin_return_address(0));
    return p;
}

void gcHeap_SndFree(void* p)
{
    if (!p) return;
    if ((uintptr_t)p >= gcHeap_sndLo && (uintptr_t)p < gcHeap_sndHi)
        mspace_free(gcHeap_sndMs, p);
    else
        __wrap_free(p);
}

// File arena: the files kept in RAM (gcDvd's overlay: settings, the player
// profile, saves -- the level autosave is ~380 KiB) live in a block of their own,
// taken once, early (the settings are read at boot). Written while a level is
// loaded, on the main heap they were left between that level's data when it was
// freed: islands that cut the free memory up -- a later level then failed to load
// with 15 MiB free and no 1 MiB in one piece. Full arena: the main heap.
#define GCHEAP_FILE_ARENA (640 * 1024) // the settings and a level autosave (~390 KiB)
static int       gcHeap_bFileTried = 0;

void* gcHeap_FileAlloc(size_t n)
{
    if (!gcHeap_bFileTried) {
        gcHeap_bFileTried = 1;
        void* pBlock = __wrap_memalign(32, GCHEAP_FILE_ARENA);
        if (pBlock) {
            gcHeap_fileMs = gcHeap_Arena(pBlock, GCHEAP_FILE_ARENA);
            if (gcHeap_fileMs) {
                gcHeap_fileLo = (uintptr_t)pBlock;
                gcHeap_fileHi = gcHeap_fileLo + GCHEAP_FILE_ARENA;
            }
            else __wrap_free(pBlock);
        }
    }
    void* p = gcHeap_fileMs ? mspace_malloc(gcHeap_fileMs, n) : NULL;
    return p ? p : __wrap_malloc(n);
}

void* gcHeap_FileRealloc(void* old, size_t n)
{
    if (!old) return gcHeap_FileAlloc(n);
    if (!gcHeap_InFileArena(old)) return __wrap_realloc(old, n);
    void* p = mspace_realloc(gcHeap_fileMs, old, n);
    if (p) return p;
    // Arena full: out to the main heap.
    p = __wrap_malloc(n);
    if (!p) return NULL;
    size_t have = dlmalloc_usable_size(old);
    memcpy(p, old, have < n ? have : n);
    mspace_free(gcHeap_fileMs, old);
    return p;
}

void gcHeap_FileFree(void* p)
{
    if (!p) return;
    if (gcHeap_InFileArena(p)) mspace_free(gcHeap_fileMs, p);
    else __wrap_free(p);
}

// The arena not yet taken by sbrk, between its current end and the arena's top.
static uint32_t gcHeap_Unclaimed(void)
{
    uintptr_t end = (uintptr_t)sbrk(0), hi = (uintptr_t)SYS_GetArena1Hi();
    return hi > end ? (uint32_t)(hi - end) : 0;
}

// Free bytes: what dlmalloc has taken from the arena and isn't in use, plus the
// arena it hasn't taken yet. Cheap (no walk).
uint32_t gcHeap_FreeBytes(void)
{
    uint32_t taken = (uint32_t)dlmalloc_footprint(), used = gcHeap_usedBytes;
    return (taken > used ? taken - used : 0) + gcHeap_Unclaimed();
}

uint32_t gcHeap_UsedBytes(void)
{
    return gcHeap_usedBytes;
}

// Largest block malloc can hand out now: the biggest free chunk, the top one
// counting the arena sbrk can still add to it. A walk over the heap: for
// diagnostics only (load errors, stats).
typedef struct { uintptr_t end; uint32_t largest, topFree; } gcHeapWalk;

static void gcHeap_WalkChunk(void* start, void* end, size_t used, void* arg)
{
    gcHeapWalk* w = (gcHeapWalk*)arg;
    if (used) return;
    uint32_t n = (uint32_t)((uintptr_t)end - (uintptr_t)start);
    if (n > w->largest) w->largest = n;
    if ((uintptr_t)end >= w->end) w->topFree = n;
}

uint32_t gcHeap_LargestFree(void)
{
    gcHeapWalk w = { (uintptr_t)sbrk(0) - 64, 0, 0 };
    dlmalloc_inspect_all(gcHeap_WalkChunk, &w);
    uint32_t top = w.topFree + gcHeap_Unclaimed();
    return top > w.largest ? top : w.largest;
}

// Consistency: dlmalloc checks chunk footers on every free and traps on damage,
// so there is no separate check; the counters stay for the diagnostics display.
int      gcHeap_checkCode = 0;
uint32_t gcHeap_checkOff = 0;
int      gcHeap_checkCount = 0;
int      gcHeap_checkBadAt = 0;

int gcHeap_Check(void)
{
    gcHeap_checkCount++;
    return 0;
}

uint32_t gcHeap_UsableSize(void* p)
{
    return p ? (uint32_t)dlmalloc_usable_size(p) : 0;
}
