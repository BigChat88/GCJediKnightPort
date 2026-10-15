#ifndef _PLATFORM_GAMECUBE_GCCARD_H
#define _PLATFORM_GAMECUBE_GCCARD_H

#ifdef __cplusplus
extern "C" {
#endif

// Saved-game slots on the memory card (slot N = the profile's save000N.jks).
#define GCCARD_MAX_SLOTS 3

// Mount the memory card. Must run before the DSP is started (see gcCard.c).
void gcCard_EarlyMount(void);

// Mount the memory card (if gcCard_EarlyMount didn't), restore the settings into
// the dvd: overlay and attach the save slots. Call after gcDvd_Mount.
void gcCard_Init(void);

// Write pending changes (settings, changed save slots) now. Called at points
// where a short pause goes unnoticed: loading screens and the main menu.
void gcCard_FlushPending(void);

// Remembered 480i/480p choice: 1 progressive, 0 interlaced, -1 never asked (or no
// card). Usable right after gcCard_EarlyMount.
int gcCard_GetVideoPref(void);
void gcCard_SetVideoPref(int bProgressive);

// Write pending changes (settings and save slots) to the card now. Returns 0 if
// something could not be written (no card, card full).
int gcCard_Save(void);

// Why the last gcCard_Save failed (empty if it didn't).
const char* gcCard_GetLastError(void);

// Whether save slot n (1..GCCARD_MAX_SLOTS) holds a game.
int gcCard_GetSlotInfo(int n, int* pbUsed);

// Whether the card has room for one more saved game.
int gcCard_CanCreateSave(void);

// Free blocks on the card and about how many a saved game takes. Returns 0 if
// there is no usable memory card.
int gcCard_GetSpace(unsigned int* pFreeBlocks, unsigned int* pNeedBlocks);

#ifdef __cplusplus
}
#endif

#endif // _PLATFORM_GAMECUBE_GCCARD_H
