#ifndef _PLATFORM_GAMECUBE_GCGAME_H
#define _PLATFORM_GAMECUBE_GCGAME_H

#ifdef __cplusplus
extern "C" {
#endif

// One build runs both games; the disc says which: Jedi Knight (dvd:/jk1/, disc ID
// OJKEOJ) or Mysteries of the Sith (dvd:/mots/, disc ID OJMEOJ).
extern int gcGame_bMots;

// Before anything touches the disc or the memory card: the ID of the disc we were
// booted from (left in low memory by the IPL, Swiss or Dolphin).
void gcGame_DetectEarly(void);

// After gcDvd_Mount: confirmed (or, for a bare .dol, found) by the disc's contents.
void gcGame_DetectDisc(void);

const char* gcGame_DiscId(void);      // 6-character boot.bin game ID + maker
const char* gcGame_Root(void);        // "jk1" / "mots"
const char* gcGame_CardPrefix(void);  // memory card file names: "<prefix>_a.sav", ...
const char* gcGame_Title(void);       // first line of the memory card comment
const char* gcGame_Player(void);      // the fixed profile name
const char* gcGame_Episode(void);     // single player episode (dev autoboot)

#ifdef __cplusplus
}
#endif

#endif // _PLATFORM_GAMECUBE_GCGAME_H
