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
#define GCMW_SLOTS   (128 * 1024)

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

static void gcMw_Put(void* p, uint32_t size, uintptr_t pc)
{
    if (!p) return;
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

void gcMemWrap_Checkpoint(void)
{
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
