#ifndef _PLATFORM_GAMECUBE_GCSTATS_H
#define _PLATFORM_GAMECUBE_GCSTATS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// After each game frame (drawUs is unused, kept for the callers).
void gcStats_GameFrame(uint64_t drawUs);

// Draw the frame rate into the HUD (menu buffer), before it is presented.
void gcStats_DrawHud(void);
void gcStats_MemBreakdown(char* out, size_t len); // "tex .. snd .. files .. other .. KB"

// Show or hide it (cheats menu: "Show FPS"; saved in the settings).
int gcStats_IsVisible(void);
void gcStats_SetVisible(int bVisible);

#ifdef __cplusplus
}
#endif

#endif // _PLATFORM_GAMECUBE_GCSTATS_H
