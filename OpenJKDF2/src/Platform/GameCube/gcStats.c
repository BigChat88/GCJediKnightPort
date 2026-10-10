// Frame rate counter: frames are counted over GCSTATS_WINDOW frames and the
// rate can be shown in the top-left corner (cheats menu: "Show FPS").

#include "gcStats.h"
#include "gcSystem.h"
#include "Platform/wuRegistry.h"

#include "Win95/Video.h"
#include "Win95/stdDisplay.h"
#include "jk.h"

#include "gcOgc.h"
#include <ogc/lwp_watchdog.h>
#include <stdio.h>
#include <string.h>

#define GCSTATS_WINDOW 30

// Where the heap goes, in KiB: GPU texture copies, sounds, files kept in RAM (the
// overlay) and the rest (the level, menus, libogc). For level load errors and the
// GC_MEMHUD panel.
extern uint32_t std3D_gcTexBytes;
uint32_t gcDvd_OverlayRamBytes(void);
uint32_t gcHeap_UsedBytes(void);
// "heap ok (N checks)" or "heap E<code> @<offset> since load N".
void gcStats_HeapCheckText(char* out, size_t len)
{
    extern int gcHeap_checkCode, gcHeap_checkCount, gcHeap_checkBadAt;
    extern uint32_t gcHeap_checkOff;
    if (!gcHeap_checkCode) snprintf(out, len, "heap ok %d", gcHeap_checkCount);
    else snprintf(out, len, "heap E%d @%x load %d/%d", gcHeap_checkCode, (unsigned)gcHeap_checkOff,
                  gcHeap_checkBadAt, gcHeap_checkCount);
}

void gcStats_MemBreakdown(char* out, size_t len)
{
    uint32_t used = gcHeap_UsedBytes(), tex = std3D_gcTexBytes, snd = (uint32_t)sithSound_curDataLoaded,
             files = gcDvd_OverlayRamBytes();
    extern uint32_t gcHeap_SndArenaSize(void);
    uint32_t arena = gcHeap_SndArenaSize(); // sounds live in it (the rest of it is free)
    uint32_t known = tex + (snd > arena ? snd : arena) + files;
    snprintf(out, len, "tex %u snd %u files %u other %u KB", (unsigned)(tex / 1024), (unsigned)(snd / 1024),
             (unsigned)(files / 1024), (unsigned)((used > known ? used - known : 0) / 1024));
}

static u64      gcStats_windowStart = 0;
static uint32_t gcStats_frames = 0;
static uint32_t gcStats_fps = 0;          // frames per second over the last window

void gcStats_GameFrame(uint64_t drawUs)
{
    u64 now = gettime();
    if (!gcStats_windowStart) gcStats_windowStart = now;
    if (++gcStats_frames < GCSTATS_WINDOW) return;
    uint64_t wall = ticks_to_microsecs(diff_ticks(gcStats_windowStart, now));
    gcStats_fps = (uint32_t)(gcStats_frames * 1000000ull / (wall ? wall : 1));
    gcStats_frames = 0;
    gcStats_windowStart = now;
}

// "Show FPS" (secret cheats menu), kept in the settings; off by default.
static int gcStats_bVisible = -1; // -1: not read from the settings yet
static char gcStats_szFps[16] = "";

int gcStats_IsVisible(void)
{
#ifdef GC_MEMHUD
    return 1;
#endif
    if (gcStats_bVisible < 0)
        gcStats_bVisible = wuRegistry_GetBool("gcShowFps", 0) ? 1 : 0;
    return gcStats_bVisible;
}

void gcStats_SetVisible(int bVisible)
{
    gcStats_bVisible = bVisible ? 1 : 0;
    wuRegistry_SaveBool("gcShowFps", gcStats_bVisible);
}

// The frame rate in the top-left corner (cheats menu: "Show FPS").
void gcStats_DrawHud(void)
{
    if (!gcStats_IsVisible() || !gcStats_fps) return;
    if (!Video_pMenuBuffer || !Video_pMenuBuffer->surface_lock_alloc) return;

    int best = 0, bestL = -1, dark = 0, darkL = 1 << 30;
    for (int i = 1; i < 256; i++) {
        int l = stdDisplay_masterPalette[i].r * 3 + stdDisplay_masterPalette[i].g * 6 + stdDisplay_masterPalette[i].b;
        if (l > bestL) { bestL = l; best = i; }
        if (l < darkL) { darkL = l; dark = i; }
    }
    snprintf(gcStats_szFps, sizeof(gcStats_szFps), "%u fps", (unsigned)gcStats_fps);
    uint8_t* pix = (uint8_t*)Video_pMenuBuffer->surface_lock_alloc;
    // Inside the TV-safe area.
    gcSystem_DrawText8(pix, Video_pMenuBuffer->format.rowSize, Video_pMenuBuffer->format.height,
                       32, 24, gcStats_szFps, (uint8_t)best, (uint8_t)dark);
    stdDisplay_GcMarkRect(32, 24, (int)strlen(gcStats_szFps) * 8, 16); // only this row changed
#ifdef GC_MEMHUD
    {
        // Diagnostic builds: heap in use / free / largest block, what the level
        // started with, and where it goes. Refreshed every 2 s (probing the
        // largest block takes a few ms).
        static char szLine1[64] = "", szLine2[64] = "", szLine3[48] = "";
        static uint32_t lastMs = 0, startUsed = 0;
        static void* pLastWorld = NULL;
        uint32_t now = (uint32_t)(ticks_to_millisecs(gettime()));
        if ((void*)sithWorld_g_pCurrentWorld != pLastWorld) {
            pLastWorld = sithWorld_g_pCurrentWorld;
            startUsed = gcHeap_UsedBytes();
        }
        if (!szLine1[0] || now - lastMs > 2000) {
            lastMs = now;
            snprintf(szLine1, sizeof(szLine1), "used %u free %u max %u start %u KB",
                     (unsigned)(gcHeap_UsedBytes() / 1024), (unsigned)(gcSystem_FreeBytes() / 1024),
                     (unsigned)(gcSystem_LargestFreeBytes() / 1024), (unsigned)(startUsed / 1024));
            gcStats_MemBreakdown(szLine2, sizeof(szLine2));
            gcStats_HeapCheckText(szLine3, sizeof(szLine3));
        }
        gcSystem_DrawText8(pix, Video_pMenuBuffer->format.rowSize, Video_pMenuBuffer->format.height,
                           32, 40, szLine1, (uint8_t)best, (uint8_t)dark);
        gcSystem_DrawText8(pix, Video_pMenuBuffer->format.rowSize, Video_pMenuBuffer->format.height,
                           32, 56, szLine2, (uint8_t)best, (uint8_t)dark);
        gcSystem_DrawText8(pix, Video_pMenuBuffer->format.rowSize, Video_pMenuBuffer->format.height,
                           32, 72, szLine3, (uint8_t)best, (uint8_t)dark);
        stdDisplay_GcMarkRect(32, 40, 64 * 8, 48);
    }
#endif
}
