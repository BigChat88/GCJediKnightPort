#ifndef _PLATFORM_GAMECUBE_GCNOTICE_H
#define _PLATFORM_GAMECUBE_GCNOTICE_H

#ifdef __cplusplus
extern "C" {
#endif

// Show a one-line notice in a box over the menu/loading screen and present it at
// once (for work that is about to block, e.g. a memory card write). Only one at a
// time; gcNotice_Hide puts the screen back.
void gcNotice_Show(const char* text);
void gcNotice_Hide(void);

// Boot screen: "Loading..." until the boot's loading is done (see gcSystem).

#ifdef __cplusplus
}
#endif

#endif // _PLATFORM_GAMECUBE_GCNOTICE_H
