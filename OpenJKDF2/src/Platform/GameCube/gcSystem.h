#ifndef _PLATFORM_GAMECUBE_GCSYSTEM_H
#define _PLATFORM_GAMECUBE_GCSYSTEM_H

#include "gcOgc.h"

#ifdef __cplusplus
extern "C" {
#endif

extern GXRModeObj* gcSystem_pRmode;

// Main thread priority (libogc: higher runs first).
#define GCSYSTEM_MAIN_PRIO 64

// Video, GX, controllers and the Dolphin OSReport log. Call first thing in main().
void gcSystem_Init(void);

// 480p: possible only with a component/digital cable. Switching takes effect at
// once and is remembered in the memory card settings.
int gcSystem_CanProgressive(void);
int gcSystem_IsProgressive(void);
void gcSystem_SetProgressive(int bOn);

// While the boot console is up: clear it, show `text` and hold it for `ms`.
void gcSystem_BootNotice(const char* text, int ms);

// While set, stdout/stderr skip the OSReport UART, which shares the EXI channel
// with the memory card (gcCard sets it during card writes).
extern volatile int gcSystem_bMuteReport;

// Draw text into an 8-bit surface with the console's 8x16 font.
void gcSystem_DrawText8(uint8_t* pBuf, int pitch, int bufH, int x, int y, const char* text, uint8_t color, uint8_t bg);

// Boot "Loading..." screen: draw a `w` x `h` RGB565 image (sizes multiples of
// 4) near the bottom of an otherwise black frame and show it at once. Ignored
// after gcSystem_BootOverlayEnd (the boot's loading is done).
void gcSystem_BootOverlay(const uint16_t* pRgb565, int w, int h);
void gcSystem_BootOverlayEnd(void);

// Copy the EFB to the back XFB and flip on the next vblank.
void gcSystem_Present(void);

// Free memory the texture and sound caches must leave untouched: late in a
// level they filled the heap and menu backgrounds / the next level failed.
#define GC_MEM_RESERVE (1536 * 1024)
uint32_t gcSystem_FreeBytes(void);
uint32_t gcSystem_LargestFreeBytes(void);
int gcSystem_IsMainThread(void);
uint32_t gcSystem_MemShortfall(uint32_t incoming);

// Diagnostic call-stack watchdog (no-op unless built with GC_TRACE).
void gcTrace_Start(void);

// Fatal error screen (text console on the XFB); never returns.
void gcSystem_Panic(const char* fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif // _PLATFORM_GAMECUBE_GCSYSTEM_H
