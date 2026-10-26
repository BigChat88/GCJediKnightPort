// Diagnostics (GC_MEM_TRACK builds only): every malloc/calloc/realloc/memalign/
// free in the program goes through these --wrap'ed functions, which keep the
// live bytes per calling function (return address). gcMemWrap_Checkpoint, called
// at each level loading screen, logs which callers hold more than at the first
// one -- memory left behind by earlier levels, cutscenes or menus, whoever
// allocated it (engine macros or plain malloc).
#ifdef GC_MEM_TRACK

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ogc/irq.h>


#define GCMW_CALLERS 1024
#define GCMW_SLOTS   (64 * 1024)

typedef struct { uintptr_t pc; int32_t live; int32_t base; uint32_t count; } gcMwCaller;
typedef struct { void* p; uint32_t size; uint16_t caller; } gcMwSlot;

static gcMwCaller gcMw_aCallers[GCMW_CALLERS];
static int        gcMw_numCallers = 0;
static gcMwSlot   gcMw_aSlots[GCMW_SLOTS];
static int        gcMw_bOverflow = 0;
static uint32_t   gcMw_numUsed = 0; // kept below 3/4 full (a full table never ends a probe)
static int        gcMw_numCheckpoints = 0;

static int gcMw_Caller(uintptr_t pc)
{
    for (int i = 0; i < gcMw_numCallers; i++)
        if (gcMw_aCallers[i].pc == pc) return i;
    if (gcMw_numCallers >= GCMW_CALLERS) return GCMW_CALLERS - 1;
    gcMw_aCallers[gcMw_numCallers].pc = pc;
    return gcMw_numCallers++;
}

static uint32_t gcMw_Hash(void* p) { return (uint32_t)(((uintptr_t)p >> 3) * 2654435761u) % GCMW_SLOTS; }

// Small blocks (<= 256 bytes) in use, and their peak: what a separate arena for
// them would have to hold.
static uint32_t gcMw_smallBytes = 0, gcMw_smallPeak = 0, gcMw_smallCount = 0, gcMw_smallCountPeak = 0;

static void gcMw_Put(void* p, uint32_t size, uintptr_t pc)
{
    if (!p) return;
    if (size <= 256) {
        gcMw_smallBytes += size + 8;
        gcMw_smallCount++;
        if (gcMw_smallBytes > gcMw_smallPeak) gcMw_smallPeak = gcMw_smallBytes;
        if (gcMw_smallCount > gcMw_smallCountPeak) gcMw_smallCountPeak = gcMw_smallCount;
    }
    if (gcMw_numUsed >= GCMW_SLOTS / 4 * 3) { gcMw_bOverflow = 1; return; }
    int c = gcMw_Caller(pc);
    uint32_t h = gcMw_Hash(p);
    for (uint32_t n = 0; n < GCMW_SLOTS; n++, h = (h + 1) % GCMW_SLOTS) {
        if (!gcMw_aSlots[h].p) {
            gcMw_aSlots[h].p = p;
            gcMw_aSlots[h].size = size;
            gcMw_aSlots[h].caller = (uint16_t)c;
            gcMw_numUsed++;
            gcMw_aCallers[c].live += (int32_t)size;
            gcMw_aCallers[c].count++;
            return;
        }
    }
    gcMw_bOverflow = 1;
}

static void gcMw_Remove(void* p)
{
    if (!p) return;
    uint32_t i = gcMw_Hash(p);
    for (uint32_t n = 0; n < GCMW_SLOTS; n++, i = (i + 1) % GCMW_SLOTS) {
        if (!gcMw_aSlots[i].p) return;
        if (gcMw_aSlots[i].p == p) break;
    }
    if (gcMw_aSlots[i].p != p) return;
    gcMw_aCallers[gcMw_aSlots[i].caller].live -= (int32_t)gcMw_aSlots[i].size;
    if (gcMw_aSlots[i].size <= 256) {
        gcMw_smallBytes -= gcMw_aSlots[i].size + 8;
        gcMw_smallCount--;
    }
    // backward-shift delete
    uint32_t j = i;
    for (;;) {
        j = (j + 1) % GCMW_SLOTS;
        if (!gcMw_aSlots[j].p) break;
        uint32_t k = gcMw_Hash(gcMw_aSlots[j].p);
        int bInRange = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
        if (!bInRange) { gcMw_aSlots[i] = gcMw_aSlots[j]; i = j; }
    }
    gcMw_aSlots[i].p = NULL;
    gcMw_numUsed--;
}

// Called by gcHeap.c (interrupts already off).
void gcMw_Track(void* p, uint32_t size, uintptr_t pc) { gcMw_Put(p, size, pc); }
void gcMw_Untrack(void* p) { gcMw_Remove(p); }

// The heap's layout at a checkpoint: the free stretches of 64 KiB or more and the
// blocks in use between them (a run of neighbours as one line, with the caller of
// its first block) -- what splits the free memory up. Collected first, printed
// after: printf may allocate, and the walk holds malloc's lock.
typedef struct { uintptr_t start, end; uint32_t used, n; uintptr_t pc; void* aP[6]; } gcMwRun;
const char* stdPlatform_GcOwnerFile(void* p);
#define GCMW_RUNS 2048
static gcMwRun gcMw_aRuns[GCMW_RUNS];
static int     gcMw_numRuns = 0;
void dlmalloc_inspect_all(void (*handler)(void*, void*, size_t, void*), void* arg);

static uintptr_t gcMw_PcOf(void* p)
{
    uint32_t i = gcMw_Hash(p);
    for (uint32_t n = 0; n < GCMW_SLOTS; n++, i = (i + 1) % GCMW_SLOTS) {
        if (!gcMw_aSlots[i].p) return 0;
        if (gcMw_aSlots[i].p == p) return gcMw_aCallers[gcMw_aSlots[i].caller].pc;
    }
    return 0;
}

static void gcMw_Chunk(void* start, void* end, size_t used, void* arg)
{
    uintptr_t s = (uintptr_t)start, e = (uintptr_t)end;
    gcMwRun* r = gcMw_numRuns ? &gcMw_aRuns[gcMw_numRuns - 1] : NULL;
    if (!used) {
        if (e - s < 64 * 1024) return; // small holes: part of the run around them
        if (gcMw_numRuns < GCMW_RUNS) gcMw_aRuns[gcMw_numRuns++] = (gcMwRun){ s, e, 0, 0, 0, { 0 } };
        return;
    }
    if (r && r->used && s - r->end < 64 * 1024) {
        if (r->n < 6) r->aP[r->n] = start;
        r->end = e; r->used += (uint32_t)used; r->n++;
        return;
    }
    if (gcMw_numRuns < GCMW_RUNS) gcMw_aRuns[gcMw_numRuns++] = (gcMwRun){ s, e, (uint32_t)used, 1, gcMw_PcOf(start), { start } };
}

static void gcMw_PrintLayout(void)
{
    gcMw_numRuns = 0;
    dlmalloc_inspect_all(gcMw_Chunk, NULL);
    printf("memwrap layout (%d runs): FREE = free stretch, USED = blocks in use (first block's caller)\n", gcMw_numRuns);
    for (int i = 0; i < gcMw_numRuns; i++) {
        gcMwRun* r = &gcMw_aRuns[i];
        if (!r->used)
            printf("  FREE %08x-%08x %6u KB\n", (unsigned)r->start, (unsigned)r->end, (unsigned)((r->end - r->start) / 1024));
        else
        {
            printf("  USED %08x-%08x %6u KB in %u blocks, pc 0x%08x\n", (unsigned)r->start, (unsigned)r->end,
                   (unsigned)(r->used / 1024), (unsigned)r->n, (unsigned)r->pc);
            // Small islands: who allocated their first blocks.
            if (r->used < 64 * 1024)
                for (uint32_t k = 0; k < r->n && k < 6; k++) {
                    const char* f = stdPlatform_GcOwnerFile(r->aP[k]);
                    printf("       %08x pc 0x%08x %s\n", (unsigned)(uintptr_t)r->aP[k], (unsigned)gcMw_PcOf(r->aP[k]),
                           f ? f : "?");
                }
        }
    }
}

void gcMemWrap_Checkpoint(void)
{
    extern uint32_t gcHeap_smallOverflow;
    printf("memwrap small blocks: now %u KB in %u, peak %u KB in %u, %u past the full arena\n",
           (unsigned)(gcMw_smallBytes / 1024), (unsigned)gcMw_smallCount, (unsigned)(gcMw_smallPeak / 1024),
           (unsigned)gcMw_smallCountPeak, (unsigned)gcHeap_smallOverflow);
    gcMw_PrintLayout();
    gcMw_numCheckpoints++;
    if (gcMw_numCheckpoints == 1) {
        for (int i = 0; i < gcMw_numCallers; i++) gcMw_aCallers[i].base = gcMw_aCallers[i].live;
        printf("memwrap #1: reference (%d callers)%s\n", gcMw_numCallers, gcMw_bOverflow ? " OVERFLOW" : "");
        return;
    }
    int32_t total = 0;
    for (int i = 0; i < gcMw_numCallers; i++) total += gcMw_aCallers[i].live - gcMw_aCallers[i].base;
    printf("memwrap #%d: %+d KB vs reference%s\n", gcMw_numCheckpoints, (int)(total / 1024), gcMw_bOverflow ? " OVERFLOW" : "");
    for (int i = 0; i < gcMw_numCallers; i++) {
        int32_t d = gcMw_aCallers[i].live - gcMw_aCallers[i].base;
        if (d > 8 * 1024 || d < -8 * 1024)
            printf("  memwrap %+7d KB  pc 0x%08x  live %d KB\n", (int)(d / 1024), (unsigned)gcMw_aCallers[i].pc,
                   (int)(gcMw_aCallers[i].live / 1024));
    }
}

#endif // GC_MEM_TRACK
