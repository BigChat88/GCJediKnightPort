// Which game is on the disc (see gcGame.h).

#include "gcGame.h"

#include <gccore.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int gcGame_bMots = 0;

// Which game this image holds, stamped by make_gcm.py: the same .dol goes into
// both images, and make_gcm.py --mots rewrites "JK1" to "JKM" in this tag inside
// the copy it puts in the Mysteries of the Sith image. Volatile so the compiler
// can't fold the comparison away.
__attribute__((used)) volatile const char gcGame_szImageTag[16] = "GCGAMETAG:JK1:::";

void gcGame_DetectEarly(void)
{
    // boot.bin's first bytes (game ID + maker) sit at the start of MEM1 after a
    // disc boot -- but libogc's startup clears low memory before main(), so on
    // hardware this is usually gone and the stamped tag decides (it picks which
    // image to look for on the SD card).
    const char* id = (const char*)0x80000000;
    if (!memcmp(id, "OJMEOJ", 6))
        gcGame_bMots = 1;
    else if (!memcmp(id, "OJKEOJ", 6))
        gcGame_bMots = 0;
    else
        gcGame_bMots = gcGame_szImageTag[10] == 'J' && gcGame_szImageTag[11] == 'K'
                    && gcGame_szImageTag[12] == 'M';
}

void gcGame_DetectDisc(void)
{
    struct stat st;
    int bJk = !stat("dvd:/jk1/episode/jk1.gob", &st);
    int bMots = !stat("dvd:/mots/episode/jkm.goo", &st);
    if (bMots && !bJk)
        gcGame_bMots = 1;
    else if (bJk && !bMots)
        gcGame_bMots = 0;
    printf("gcGame: %s\n", gcGame_bMots ? "Mysteries of the Sith" : "Jedi Knight");
}

const char* gcGame_DiscId(void)     { return gcGame_bMots ? "OJMEOJ" : "OJKEOJ"; }
const char* gcGame_Root(void)       { return gcGame_bMots ? "mots" : "jk1"; }
const char* gcGame_CardPrefix(void) { return gcGame_bMots ? "openjkm" : "openjkdf2"; }
const char* gcGame_Title(void)      { return gcGame_bMots ? "Mysteries of the Sith" : "Jedi Knight"; }
const char* gcGame_Player(void)     { return gcGame_bMots ? "Mara" : "Kyle"; }
const char* gcGame_Episode(void)    { return gcGame_bMots ? "JKM" : "JK1"; }
