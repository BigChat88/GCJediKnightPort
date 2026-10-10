// Controller state shared by the GUI poll and stdControl, plus an optional scripted
// input source for automated tests (GC_INPUT_SCRIPT at configure time):
//
//   "25000:A,27500:DOWN,28000:A"   press A at 25.0s after boot, DOWN at 27.5s, ...
//
// Each scripted press is held for GCINPUT_SCRIPT_HOLD_MS, or "+ms" after the name
// ("30000:CRIGHT+800"). CLEFT/CRIGHT/CUP/CDOWN push the C-stick (camera),
// SLEFT/SRIGHT/SUP/SDOWN the main stick (movement).

#include "gcInput.h"

#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GCINPUT_SCRIPT_HOLD_MS 150

#ifdef GC_INPUT_SCRIPT
static u64 gcInput_startTicks = 0;

static uint32_t gcInput_ButtonFromName(const char* name, size_t len)
{
    static const struct { const char* name; uint32_t mask; } names[] = {
        { "A", PAD_BUTTON_A }, { "B", PAD_BUTTON_B }, { "X", PAD_BUTTON_X },
        { "Y", PAD_BUTTON_Y }, { "Z", PAD_TRIGGER_Z }, { "L", PAD_TRIGGER_L },
        { "R", PAD_TRIGGER_R }, { "START", PAD_BUTTON_START },
        { "UP", PAD_BUTTON_UP }, { "DOWN", PAD_BUTTON_DOWN },
        { "LEFT", PAD_BUTTON_LEFT }, { "RIGHT", PAD_BUTTON_RIGHT },
        { "CLEFT", GCINPUT_SCRIPT_CLEFT }, { "CRIGHT", GCINPUT_SCRIPT_CRIGHT },
        { "CUP", GCINPUT_SCRIPT_CUP }, { "CDOWN", GCINPUT_SCRIPT_CDOWN },
        { "SLEFT", GCINPUT_SCRIPT_SLEFT }, { "SRIGHT", GCINPUT_SCRIPT_SRIGHT },
        { "SUP", GCINPUT_SCRIPT_SUP }, { "SDOWN", GCINPUT_SCRIPT_SDOWN },
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strlen(names[i].name) == len && !strncmp(names[i].name, name, len))
            return names[i].mask;
    return 0;
}

static uint32_t gcInput_ScriptButtons(void)
{
    if (!gcInput_startTicks) gcInput_startTicks = gettime();
    uint32_t now = diff_msec(gcInput_startTicks, gettime());
    uint32_t buttons = 0;

    const char* p = GC_INPUT_SCRIPT;
    while (*p) {
        uint32_t at = (uint32_t)strtoul(p, (char**)&p, 10);
        if (*p != ':') break;
        p++;
        const char* name = p;
        while (*p && *p != ',' && *p != '+') p++;
        size_t nameLen = (size_t)(p - name);
        uint32_t hold = GCINPUT_SCRIPT_HOLD_MS;
        if (*p == '+')
            hold = (uint32_t)strtoul(p + 1, (char**)&p, 10);
        if (now >= at && now < at + hold) {
            buttons |= gcInput_ButtonFromName(name, nameLen);
            static uint32_t lastLogged = ~0u;
            if (lastLogged != at) {
                printf("gcInput: scripted %.*s at %u ms\n", (int)nameLen, name, now);
                lastLogged = at;
            }
        }
        if (*p == ',') p++;
    }
    return buttons;
}
#endif // GC_INPUT_SCRIPT

// A button counts as let go only after it has read up for this long. It's read
// many times a frame, and a read can come back with nothing held while the button
// still is: in the menus a held A or B then acted again and again (a checkbox
// flipping on and off, back going through two menus).
#define GCINPUT_RELEASE_MS 50

uint32_t gcInput_ButtonsHeld(void)
{
    static uint32_t held = 0;
    static u64 aLastDown[32];

    PAD_ScanPads();
    uint32_t raw = PAD_ButtonsHeld(0);
#ifdef GC_INPUT_SCRIPT
    raw |= gcInput_ScriptButtons();
#endif
    u64 now = gettime();
    for (int i = 0; i < 32; i++) {
        uint32_t bit = 1u << i;
        if (raw & bit) {
            held |= bit;
            aLastDown[i] = now;
        }
        else if ((held & bit) && diff_msec(aLastDown[i], now) >= GCINPUT_RELEASE_MS)
            held &= ~bit;
    }
    return held;
}

int gcInput_CheatComboPressed(void)
{
    static int bWasHeld = 1;
    const uint32_t combo = PAD_TRIGGER_L | PAD_BUTTON_LEFT | PAD_BUTTON_X;
    int bHeld = (gcInput_ButtonsHeld() & combo) == combo;
    int bPressed = bHeld && !bWasHeld;
    bWasHeld = bHeld;
    return bPressed;
}
