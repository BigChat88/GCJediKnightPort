#ifndef _PLATFORM_GAMECUBE_GCINPUT_H
#define _PLATFORM_GAMECUBE_GCINPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Scan the pads and return port 0's held buttons (PAD_BUTTON_* mask), including
// scripted presses in GC_INPUT_SCRIPT test builds.
uint32_t gcInput_ButtonsHeld(void);
// L + D-pad left + X just became held together (the secret cheats menu).
int gcInput_CheatComboPressed(void);

// Scripted C-stick deflections (GC_INPUT_SCRIPT test builds only), reported in the
// gcInput_ButtonsHeld mask above the real PAD_* bits.
#define GCINPUT_SCRIPT_CLEFT  (1u << 20)
#define GCINPUT_SCRIPT_CRIGHT (1u << 21)
#define GCINPUT_SCRIPT_CUP    (1u << 22)
#define GCINPUT_SCRIPT_CDOWN  (1u << 23)
#define GCINPUT_SCRIPT_SLEFT  (1u << 24) // main stick (movement)
#define GCINPUT_SCRIPT_SRIGHT (1u << 25)
#define GCINPUT_SCRIPT_SUP    (1u << 26)
#define GCINPUT_SCRIPT_SDOWN  (1u << 27)

#ifdef __cplusplus
}
#endif

#endif // _PLATFORM_GAMECUBE_GCINPUT_H
