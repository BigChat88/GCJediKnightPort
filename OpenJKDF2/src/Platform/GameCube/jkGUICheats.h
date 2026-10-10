#ifndef _PLATFORM_GAMECUBE_JKGUICHEATS_H
#define _PLATFORM_GAMECUBE_JKGUICHEATS_H

// Secret cheats menu (pause menu, hold L + D-pad left + X). See jkGUICheats.c.

#define JKGUIGCCHEATS_LEAVE_PAUSE 2 // returned when the pause menu should close

int jkGuiGcCheats_Show(void);
void jkGuiGcCheats_GameFrame(void); // once per game frame (deferred "skip level")

#endif // _PLATFORM_GAMECUBE_JKGUICHEATS_H
