// GameCube heap, libogc2 core.
//
// libogc2's malloc is dlmalloc 2.8.6: one heap for everything (newlib's own
// _malloc_r included), grown by sbrk over the MEM1 arena, chunk footers checked
// on every free (FOOTERS), mutex locks -- interrupts stay on. That covers what
// the libogc core had to build itself (Platform/GameCube/gcHeap.c: newlib's
// malloc failing big requests with plenty free, damage found late, the audio DSP
// starved by heap work with interrupts off). So nothing is allocated here:
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

// libogc2's dlmalloc (built with USE_DL_PREFIX; its <malloc.h> shares the guard
// with our own Platform/TWL/dlmalloc.h, so declared here).
typedef void* mspace;
size_t dlmalloc_usable_size(const void*);
size_t dlmalloc_footprint(void);
void   dlmalloc_inspect_all(void (*handler)(void*, void*, size_t, void*), void* arg);
mspace create_mspace_with_base(void* base, size_t capacity, int locked);
void*  mspace_memalign(mspace msp, size_t alignment, size_t bytes);
void   mspace_free(mspace msp, void* mem);

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

static inline void gcHeap_Add(void* p)
{
    if (!p) return;
    uint32_t n = gcHeap_ChunkBytes(p);
    u32 l = IRQ_Disable();
    gcHeap_usedBytes += n;
    IRQ_Restore(l);
}

static inline void gcHeap_Sub(void* p)
{
    if (!p) return;
    uint32_t n = gcHeap_ChunkBytes(p);
    u32 l = IRQ_Disable();
    gcHeap_usedBytes -= n;
    IRQ_Restore(l);
}

void* __wrap_malloc(size_t n)
{
    void* p = __real_malloc(n);
    gcHeap_Add(p);
    GCHEAP_TRACK(p, n, (uintptr_t)__builtin_return_address(0));
    if (!p && n) gcHeap_failPath = 'm';
    return p;
}

void* __wrap_calloc(size_t a, size_t b)
{
    void* p = __real_calloc(a, b);
    gcHeap_Add(p);
    GCHEAP_TRACK(p, a * b, (uintptr_t)__builtin_return_address(0));
    return p;
}

void* __wrap_memalign(size_t al, size_t n)
{
    void* p = __real_memalign(al, n);
    gcHeap_Add(p);
    GCHEAP_TRACK(p, n, (uintptr_t)__builtin_return_address(0));
    if (!p && n) gcHeap_failPath = 'a';
    return p;
}

void __wrap_free(void* p)
{
    if (!p) return;
    GCHEAP_UNTRACK(p);
    gcHeap_Sub(p);
    __real_free(p);
}

// (dlmalloc's realloc grows a block in place when it can, before moving it.)
void* __wrap_realloc(void* old, size_t n)
{
    if (!old) return __wrap_malloc(n);
    if (!n) { __wrap_free(old); return NULL; }
    uint32_t before = gcHeap_ChunkBytes(old);
    void* p = __real_realloc(old, n);
    if (!p) { gcHeap_failPath = 'R'; return NULL; }
    uint32_t after = gcHeap_ChunkBytes(p);
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
static mspace    gcHeap_sndMs = NULL;
static uintptr_t gcHeap_sndLo = 0, gcHeap_sndHi = 0;
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
            gcHeap_sndMs = create_mspace_with_base(pBlock, GCHEAP_SND_ARENA, 1);
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

void gcHeap_SndFree(void* p)
{
    if (!p) return;
    if ((uintptr_t)p >= gcHeap_sndLo && (uintptr_t)p < gcHeap_sndHi)
        mspace_free(gcHeap_sndMs, p);
    else
        __wrap_free(p);
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
