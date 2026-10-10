// Texture pool in ARAM (see gcAram.h).
//
// Why: a level's material pixels took 3-5 MiB of the 24 MiB main RAM for the
// whole level, although only the textures in view are needed at any moment. The
// Dreamcast and DSi ports solve this by streaming textures from disc on demand; on
// GameCube the 16 MiB ARAM can hold them instead, with no disc access while
// playing (an ARAM -> MEM1 copy of a 64 KiB texture takes well under a ms).
//
// The pool is a first-fit free list over a fixed ARAM range carved out by
// gcDvd_Mount. Allocations happen on the main thread only (material loads).

#include <string.h>
#include <gccore.h>
#include <ogc/irq.h>
#include "gcAram.h"

#define GCARAM_MAX_RANGES 1024

typedef struct { uint32_t addr, size; } gcAramRange;

static gcAramRange gcAram_aFree[GCARAM_MAX_RANGES]; // sorted by address, coalesced
static int         gcAram_numFree = 0;

uint32_t gcAram_poolSize = 0;
uint32_t gcAram_poolUsed = 0;
uint32_t gcAram_poolFails = 0;

void gcAram_InitPool(uint32_t base, uint32_t size)
{
    base = (base + 31) & ~31u;
    size &= ~31u;
    gcAram_aFree[0].addr = base;
    gcAram_aFree[0].size = size;
    gcAram_numFree = size ? 1 : 0;
    gcAram_poolSize = size;
    gcAram_poolUsed = 0;
}

uint32_t gcAram_Alloc(uint32_t size)
{
    size = (size + 31) & ~31u;
    if (!size) return 0;
    // Best fit: textures come in a handful of sizes, so exact fits are common and
    // the large ranges stay whole.
    int best = -1;
    for (int i = 0; i < gcAram_numFree; i++) {
        if (gcAram_aFree[i].size < size) continue;
        if (best < 0 || gcAram_aFree[i].size < gcAram_aFree[best].size) {
            best = i;
            if (gcAram_aFree[i].size == size) break;
        }
    }
    if (best < 0) {
        gcAram_poolFails++;
        return 0;
    }
    uint32_t addr = gcAram_aFree[best].addr;
    gcAram_aFree[best].addr += size;
    gcAram_aFree[best].size -= size;
    if (!gcAram_aFree[best].size) {
        memmove(&gcAram_aFree[best], &gcAram_aFree[best + 1],
                (gcAram_numFree - best - 1) * sizeof(gcAramRange));
        gcAram_numFree--;
    }
    gcAram_poolUsed += size;
    return addr;
}

void gcAram_Free(uint32_t addr, uint32_t size)
{
    size = (size + 31) & ~31u;
    if (!addr || !size) return;

    // Insertion point (first range above addr).
    int i = 0;
    while (i < gcAram_numFree && gcAram_aFree[i].addr < addr) i++;

    int mergePrev = i > 0 && gcAram_aFree[i - 1].addr + gcAram_aFree[i - 1].size == addr;
    int mergeNext = i < gcAram_numFree && addr + size == gcAram_aFree[i].addr;

    if (mergePrev && mergeNext) {
        gcAram_aFree[i - 1].size += size + gcAram_aFree[i].size;
        memmove(&gcAram_aFree[i], &gcAram_aFree[i + 1],
                (gcAram_numFree - i - 1) * sizeof(gcAramRange));
        gcAram_numFree--;
    }
    else if (mergePrev) {
        gcAram_aFree[i - 1].size += size;
    }
    else if (mergeNext) {
        gcAram_aFree[i].addr = addr;
        gcAram_aFree[i].size += size;
    }
    else {
        if (gcAram_numFree == GCARAM_MAX_RANGES) {
            // Table full: lose the range rather than corrupt the list (never seen;
            // a level has well under a thousand textures).
            gcAram_poolUsed -= size;
            gcAram_poolSize -= size;
            return;
        }
        memmove(&gcAram_aFree[i + 1], &gcAram_aFree[i],
                (gcAram_numFree - i) * sizeof(gcAramRange));
        gcAram_aFree[i].addr = addr;
        gcAram_aFree[i].size = size;
        gcAram_numFree++;
    }
    gcAram_poolUsed -= size;
}

static void gcAram_Dma(u32 dir, void* mem, u32 aram, u32 len)
{
    ARQRequest req;
    if (dir == ARQ_MRAMTOARAM) DCFlushRange(mem, len);
    else DCInvalidateRange(mem, len);
    // Synchronous, low priority: ARQ splits it into chunks and serves the audio's
    // small high-priority transfers in between (see gcDvd_AramDma).
    ARQ_PostRequest(&req, 0x4A4D, dir, ARQ_PRIO_LO, aram, (u32)MEM_VIRTUAL_TO_PHYSICAL(mem), len);
}

void gcAram_Write(uint32_t aram, void* mem, uint32_t len)
{
    gcAram_Dma(ARQ_MRAMTOARAM, mem, aram, len);
}

void gcAram_Read(void* mem, uint32_t aram, uint32_t len)
{
    gcAram_Dma(ARQ_ARAMTOMRAM, mem, aram, len);
}
