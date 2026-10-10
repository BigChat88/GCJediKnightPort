// GameCube heap: every malloc/free/realloc/calloc/memalign in the program (the
// engine, this port and libogc) is redirected here at link time (--wrap) and
// served by dlmalloc from one fixed region of MEM1.
//
// Why: on hardware, level loads failed asking newlib's malloc for 700-800 KiB
// while a 6-7 MiB block was free a moment later -- never a real shortage. Taking
// the heap over makes its behaviour ours: a single, known region; integrity checks
// (a corrupted heap is reported where it is found, instead of failing an
// allocation much later); exact free/largest-block figures.
//
// newlib's own internal allocations (_malloc_r from stdio, strdup...) don't go
// through the wrapped symbols; they keep a small sbrk heap below our region
// (GCHEAP_NEWLIB_RESERVE). Frees of their pointers are passed through.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <gccore.h>
#include <ogc/irq.h>
#include <ogc/lwp_watchdog.h>
#include "Platform/TWL/dlmalloc.h"

#define GCHEAP_NEWLIB_RESERVE (512 * 1024)

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

static mspace    gcHeap_ms = NULL;
static uintptr_t gcHeap_lo = 0, gcHeap_hi = 0;
volatile int     gcHeap_bCorrupt = 0; // dlmalloc found a damaged chunk
// Which branch last returned NULL (shown in level load errors): 'm' malloc,
// 'a' memalign, 'n' realloc of a newlib pointer, 'R' realloc's new block.
volatile char    gcHeap_failPath = 0;

static void gcHeap_Init(void)
{
    // newlib keeps [ArenaLo, ArenaLo + reserve) for its sbrk; we take the rest.
    uintptr_t lo = ((uintptr_t)SYS_GetArena1Lo() + 31) & ~(uintptr_t)31;
    uintptr_t hi = (uintptr_t)SYS_GetArena1Hi() & ~(uintptr_t)31;
    uintptr_t base = lo + GCHEAP_NEWLIB_RESERVE;
    if (base >= hi) return;
    SYS_SetArena1Hi((void*)base);
    gcHeap_ms = create_mspace_with_base((void*)base, hi - base, 0);
    if (gcHeap_ms) { gcHeap_lo = base; gcHeap_hi = hi; }
}

static inline int gcHeap_Owns(void* p)
{
    return (uintptr_t)p >= gcHeap_lo && (uintptr_t)p < gcHeap_hi;
}

// Interrupts are off only around dlmalloc's own bookkeeping, which is short. The
// audio DSP needs the CPU's answer within a few hundred microseconds at every
// voice it mixes; anything long here (clearing or copying big blocks, walking the
// whole heap for statistics) made it miss its deadline and the next audio block
// replayed stale data -- a crackle at every sound that started (loading it checks
// the free memory). So: bytes in use are counted as blocks come and go, calloc
// clears and realloc copies with interrupts on.
#define GCHEAP_CHUNK_OVERHEAD 8
static volatile uint32_t gcHeap_usedBytes = 0;

static inline void gcHeap_Count(void* p, int sign)
{
    if (p && gcHeap_Owns(p))
        gcHeap_usedBytes += sign * (int32_t)(mspace_usable_size(p) + GCHEAP_CHUNK_OVERHEAD);
}

void* __wrap_malloc(size_t n)
{
    u32 l = IRQ_Disable();
    if (!gcHeap_ms) gcHeap_Init();
    void* p = gcHeap_ms ? mspace_malloc(gcHeap_ms, n) : __real_malloc(n);
    gcHeap_Count(p, 1);
    GCHEAP_TRACK(p, n, (uintptr_t)__builtin_return_address(0));
    if (!p && n) gcHeap_failPath = 'm';
    IRQ_Restore(l);
    return p;
}

void* __wrap_calloc(size_t a, size_t b)
{
    size_t n = a * b;
    if (b && n / b != a) return NULL;
    u32 l = IRQ_Disable();
    if (!gcHeap_ms) gcHeap_Init();
    void* p = gcHeap_ms ? mspace_malloc(gcHeap_ms, n) : __real_calloc(a, b);
    gcHeap_Count(p, 1);
    GCHEAP_TRACK(p, n, (uintptr_t)__builtin_return_address(0));
    IRQ_Restore(l);
    if (p && gcHeap_Owns(p)) memset(p, 0, n); // with interrupts on
    return p;
}

void* __wrap_memalign(size_t al, size_t n)
{
    u32 l = IRQ_Disable();
    if (!gcHeap_ms) gcHeap_Init();
    void* p = gcHeap_ms ? mspace_memalign(gcHeap_ms, al, n) : __real_memalign(al, n);
    gcHeap_Count(p, 1);
    GCHEAP_TRACK(p, n, (uintptr_t)__builtin_return_address(0));
    if (!p && n) gcHeap_failPath = 'a';
    IRQ_Restore(l);
    return p;
}

void __wrap_free(void* p)
{
    if (!p) return;
    u32 l = IRQ_Disable();
    GCHEAP_UNTRACK(p);
    gcHeap_Count(p, -1);
    if (gcHeap_Owns(p)) mspace_free(gcHeap_ms, p);
    else __real_free(p);
    IRQ_Restore(l);
}

void* __wrap_realloc(void* old, size_t n)
{
    if (!old) return __wrap_malloc(n);
    if (!n) { __wrap_free(old); return NULL; }
    if (!gcHeap_Owns(old)) {
        // Allocated by newlib internally: move it into our heap.
        u32 l = IRQ_Disable();
        void* p = __real_realloc(old, n);
        if (!p) gcHeap_failPath = 'n';
        IRQ_Restore(l);
        return p;
    }
    // Still fits: keep it (dlmalloc would only shrink it in place).
    size_t have = mspace_usable_size(old);
    if (n <= have) return old;
    // Grow in place when the next chunk is free (as dlmalloc's realloc does first):
    // a growing table during a level load needs no second block then. Without
    // this the old and the new block had to fit at once and level 9 failed to
    // load (666 KB with 8 MB free).
    {
        u32 l = IRQ_Disable();
        gcHeap_Count(old, -1);
        void* q = mspace_realloc_in_place(gcHeap_ms, old, n);
        gcHeap_Count(q ? q : old, 1);
        IRQ_Restore(l);
        if (q) return q;
    }
    // New block, copy with interrupts on, free the old one.
    void* p = __wrap_malloc(n);
    if (!p) { gcHeap_failPath = 'R'; return NULL; }
    memcpy(p, old, have);
    __wrap_free(old);
    return p;
}

// Sound arena: decoded sound PCM lives in one fixed block of the heap with its
// own allocator. Sounds come and go all through a level (a busy scene loaded
// ~800 KiB of them) and, spread over the main heap, they cut its free memory
// into pieces: the largest free block fell from 3.8 MiB to 0.7 MiB in level 1's
// TIE bomber scene, and the next levels failed to load with plenty free. Kept
// in one place, their churn only fragments the arena. Full arena: the main heap.
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
            u32 l = IRQ_Disable();
            gcHeap_sndMs = create_mspace_with_base(pBlock, GCHEAP_SND_ARENA, 0);
            IRQ_Restore(l);
            if (gcHeap_sndMs) {
                gcHeap_sndLo = (uintptr_t)pBlock;
                gcHeap_sndHi = gcHeap_sndLo + GCHEAP_SND_ARENA;
            }
            else __wrap_free(pBlock);
        }
    }
    void* p = NULL;
    if (gcHeap_sndMs) {
        u32 l = IRQ_Disable();
        p = mspace_memalign(gcHeap_sndMs, 32, n);
        IRQ_Restore(l);
    }
    return p ? p : __wrap_memalign(32, n);
}

void gcHeap_SndFree(void* p)
{
    if (!p) return;
    if ((uintptr_t)p >= gcHeap_sndLo && (uintptr_t)p < gcHeap_sndHi) {
        u32 l = IRQ_Disable();
        mspace_free(gcHeap_sndMs, p);
        IRQ_Restore(l);
    }
    else __wrap_free(p);
}

// Free bytes in our heap and the bytes in use, from the running count (a
// mallinfo walk over every chunk took milliseconds with interrupts off).
uint32_t gcHeap_FreeBytes(void)
{
    if (!gcHeap_ms) return 0;
    uint32_t total = (uint32_t)(gcHeap_hi - gcHeap_lo), used = gcHeap_usedBytes;
    return used < total ? total - used : 0;
}

// Largest block malloc can hand out now, from the allocator's own bookkeeping.
size_t mspace_gc_largest_free(mspace msp);
uint32_t gcHeap_LargestFree(void)
{
    if (!gcHeap_ms) return 0;
    u32 l = IRQ_Disable();
    uint32_t n = (uint32_t)mspace_gc_largest_free(gcHeap_ms);
    IRQ_Restore(l);
    return n;
}

uint32_t gcHeap_UsedBytes(void)
{
    return gcHeap_ms ? gcHeap_usedBytes : 0;
}

// Heap consistency (diagnostics): checked at every loading screen and when a
// level fails to load. gcHeap_checkCode/Off describe the first problem found and
// gcHeap_checkBadAt is the loading screen it was first seen at (0 = never).
int mspace_gc_check(mspace msp, void** pBad);
int      gcHeap_checkCode = 0;
uint32_t gcHeap_checkOff = 0;
int      gcHeap_checkCount = 0;
int      gcHeap_checkBadAt = 0;

int gcHeap_Check(void)
{
    if (!gcHeap_ms) return 0;
    void* pBad = NULL;
    u32 l = IRQ_Disable();
    int code = mspace_gc_check(gcHeap_ms, &pBad);
    IRQ_Restore(l);
    gcHeap_checkCount++;
    if (code && !gcHeap_checkCode) {
        gcHeap_checkCode = code;
        gcHeap_checkOff = (uint32_t)((uintptr_t)pBad - gcHeap_lo);
        gcHeap_checkBadAt = gcHeap_checkCount;
    }
    return code;
}

uint32_t gcHeap_UsableSize(void* p)
{
    return gcHeap_Owns(p) ? (uint32_t)mspace_usable_size(p) : 0;
}
