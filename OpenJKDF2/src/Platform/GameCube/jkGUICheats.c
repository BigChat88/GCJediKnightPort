// Secret cheats menu for the GameCube (no keyboard to type the cheat codes).
//
// Opened from the pause menu by holding L + D-pad left + X (see jkGUIEsc.c).
// The toggles reflect the current state and are applied on the way out; the
// other entries act right away. It also switches the on-screen stats panel
// (gcStats), which is saved in the settings and on by default.

#include "jkGUICheats.h"

#include "Gui/jkGUIRend.h"
#include "Gui/jkGUI.h"
#include "Main/jkDev.h"
#include "General/stdString.h"
#include <string.h>
#include "Main/jkMain.h"
#include "World/sithWorld.h"
#include "Platform/GameCube/gcStats.h"
#include "jk.h"

enum
{
    CHEAT_FIRST_ACTION = 200,
    CHEAT_WEAPONS = CHEAT_FIRST_ACTION,
    CHEAT_ITEMS,
    CHEAT_HEAL,
    CHEAT_MANA,
    CHEAT_FORCE,
    CHEAT_LEVELUP,
    CHEAT_MAP,
    CHEAT_SKIP,
};

enum
{
    ELMT_TITLE = 0,
    ELMT_INVUL,
    ELMT_FLY,
    ELMT_NOAI,
    ELMT_SLOWMO,
    ELMT_FPS,
    ELMT_STATUS = 14,
    ELMT_OK,
};

static int jkGuiGcCheats_Action(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, int bRedraw);

// Text that isn't a string table key is given as UTF-16 (u"..."), which the GUI
// draws as is when the lookup fails.
static jkGuiElement jkGuiGcCheats_aElements[17] = {
    { ELEMENT_TEXT,       0,              6, (const char*)u"Cheats",               3, {20, 20, 600, 40},   1, 0, NULL, 0, 0, 0, {0}, 0},
    // toggles (applied on OK / back)
    { ELEMENT_CHECKBOX,   0,              0, (const char*)u"Invulnerability",      0, {40, 100, 280, 30},  1, 0, NULL, 0, 0, 0, {0}, 0},
    { ELEMENT_CHECKBOX,   0,              0, (const char*)u"Fly",                  0, {40, 140, 280, 30},  1, 0, NULL, 0, 0, 0, {0}, 0},
    { ELEMENT_CHECKBOX,   0,              0, (const char*)u"Freeze enemies",       0, {40, 180, 280, 30},  1, 0, NULL, 0, 0, 0, {0}, 0},
    { ELEMENT_CHECKBOX,   0,              0, (const char*)u"Slow motion",          0, {40, 220, 280, 30},  1, 0, NULL, 0, 0, 0, {0}, 0},
    { ELEMENT_CHECKBOX,   0,              0, (const char*)u"Show FPS",             0, {40, 290, 280, 30},  1, 0, NULL, 0, 0, 0, {0}, 0},
    // actions (right away)
    { ELEMENT_TEXTBUTTON, CHEAT_WEAPONS,  2, (const char*)u"All weapons",          3, {340, 90, 280, 34},  1, 0, NULL, 0, jkGuiGcCheats_Action, 0, {0}, 0},
    { ELEMENT_TEXTBUTTON, CHEAT_ITEMS,    2, (const char*)u"All items",            3, {340, 125, 280, 34}, 1, 0, NULL, 0, jkGuiGcCheats_Action, 0, {0}, 0},
    { ELEMENT_TEXTBUTTON, CHEAT_HEAL,     2, (const char*)u"Full health",          3, {340, 160, 280, 34}, 1, 0, NULL, 0, jkGuiGcCheats_Action, 0, {0}, 0},
    { ELEMENT_TEXTBUTTON, CHEAT_MANA,     2, (const char*)u"Full Force mana",      3, {340, 195, 280, 34}, 1, 0, NULL, 0, jkGuiGcCheats_Action, 0, {0}, 0},
    { ELEMENT_TEXTBUTTON, CHEAT_FORCE,    2, (const char*)u"All Force powers",     3, {340, 230, 280, 34}, 1, 0, NULL, 0, jkGuiGcCheats_Action, 0, {0}, 0},
    { ELEMENT_TEXTBUTTON, CHEAT_LEVELUP,  2, (const char*)u"Force rank up",        3, {340, 265, 280, 34}, 1, 0, NULL, 0, jkGuiGcCheats_Action, 0, {0}, 0},
    { ELEMENT_TEXTBUTTON, CHEAT_MAP,      2, (const char*)u"Reveal map",           3, {340, 300, 280, 34}, 1, 0, NULL, 0, jkGuiGcCheats_Action, 0, {0}, 0},
    { ELEMENT_TEXTBUTTON, CHEAT_SKIP,     2, (const char*)u"Skip level",           3, {340, 335, 280, 34}, 1, 0, NULL, 0, jkGuiGcCheats_Action, 0, {0}, 0},
    // 14: status line
    { ELEMENT_TEXT,       0,              0, (const char*)u"",                     3, {0, 395, 640, 20},   1, 0, NULL, 0, 0, 0, {0}, 0},
    // 15
    { ELEMENT_TEXTBUTTON, 1,              2, "GUI_OK",                             3, {440, 430, 200, 40}, 1, 0, NULL, 0, 0, 0, {0}, 0},
    { ELEMENT_END,        0,              0, NULL,                                 0, {0},                 0, 0, NULL, 0, 0, 0, {0}, 0},
};

static jkGuiMenu jkGuiGcCheats_menu = { jkGuiGcCheats_aElements, -1, 0xFFFF, 0xFFFF, 0x0F, 0, 0, jkGui_stdBitmaps, jkGui_stdFonts, 0, 0, "thermloop01.wav", "thrmlpu2.wav", 0, 0, 0, 0, 0, 0 };
static int jkGuiGcCheats_bInitted = 0;
static int jkGuiGcCheats_bSkipPending = 0;
static char16_t jkGuiGcCheats_wStatus[64];

// The engine's cheat handlers, called as the console would (extra = which flag).
static void jkGuiGcCheats_Run(int (*fn)(stdDebugConsoleCmd*, const char*), uint32_t extra, const char* arg)
{
    stdDebugConsoleCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    stdString_SafeStrCopy(cmd.cmdStr, "cheat", sizeof(cmd.cmdStr));
    cmd.extra = extra;
    fn(&cmd, arg);
}

static SithThing* jkGuiGcCheats_Player(void)
{
    return sithWorld_g_pCurrentWorld ? sithWorld_g_pCurrentWorld->pLocalPlayer : NULL;
}

static int jkGuiGcCheats_IsInvul(void)  { SithThing* p = jkGuiGcCheats_Player(); return p && (p->actorParams.flags & 8); }
static int jkGuiGcCheats_IsFlying(void) { SithThing* p = jkGuiGcCheats_Player(); return p && p->moveType == SITH_MT_PHYSICS && (p->physicsParams.flags & SITH_PF_FLY); }
static int jkGuiGcCheats_IsNoAi(void)   { return (g_debugmodeFlags & 1) != 0; }
static int jkGuiGcCheats_IsSlowmo(void) { return (g_debugmodeFlags & 0x400) != 0; }

static int jkGuiGcCheats_Action(jkGuiElement *pElement, jkGuiMenu *pMenu, int32_t mouseX, int32_t mouseY, int bRedraw)
{
    jkGuiRend_PlayWav(pMenu->soundClick);
    switch (pElement->hoverId)
    {
        case CHEAT_WEAPONS: jkGuiGcCheats_Run(jkDev_CmdAllWeapons, 0, NULL); break;
        case CHEAT_ITEMS:   jkGuiGcCheats_Run(jkDev_CmdAllItems, 0, NULL); break;
        case CHEAT_HEAL:    jkGuiGcCheats_Run(jkDev_CmdHeal, 0, NULL); break;
        case CHEAT_MANA:    jkGuiGcCheats_Run(jkDev_CmdMana, 0, NULL); break;
        case CHEAT_FORCE:   jkGuiGcCheats_Run(jkDev_CmdUberJedi, 0, NULL); break;
        case CHEAT_LEVELUP: jkGuiGcCheats_Run(jkDev_CmdLevelUp, 0, NULL); break;
        case CHEAT_MAP:     jkGuiGcCheats_Run(jkDev_CmdAllMap, 0, NULL); break;
        case CHEAT_SKIP:
            // Ending the level from inside the pause menu would be undone by the
            // return to the game; do it on the first frame back instead.
            jkGuiGcCheats_bSkipPending = 1;
            return JKGUIGCCHEATS_LEAVE_PAUSE;
        default: return 0;
    }
    jk_snwprintf(jkGuiGcCheats_wStatus, 63, u"%ls: done", pElement->wstr);
    jkGuiGcCheats_aElements[ELMT_STATUS].wstr = jkGuiGcCheats_wStatus;
    jkGuiRend_UpdateAndDrawClickable(&jkGuiGcCheats_aElements[ELMT_STATUS], pMenu, 1);
    return 0;
}

int jkGuiGcCheats_Show(void)
{
    if (!jkGuiGcCheats_bInitted) {
        jkGui_InitMenu(&jkGuiGcCheats_menu, jkGui_stdBitmaps[JKGUI_BM_BK_ESC]);
        jkGuiGcCheats_bInitted = 1;
    }

    int bInvul = jkGuiGcCheats_IsInvul(), bFly = jkGuiGcCheats_IsFlying();
    int bNoAi = jkGuiGcCheats_IsNoAi(), bSlowmo = jkGuiGcCheats_IsSlowmo();
    jkGuiGcCheats_aElements[ELMT_INVUL].selectedTextEntry = bInvul;
    jkGuiGcCheats_aElements[ELMT_FLY].selectedTextEntry = bFly;
    jkGuiGcCheats_aElements[ELMT_NOAI].selectedTextEntry = bNoAi;
    jkGuiGcCheats_aElements[ELMT_SLOWMO].selectedTextEntry = bSlowmo;
    jkGuiGcCheats_aElements[ELMT_FPS].selectedTextEntry = gcStats_IsVisible();
    jkGuiGcCheats_wStatus[0] = 0;
    jkGuiGcCheats_aElements[ELMT_STATUS].wstr = jkGuiGcCheats_wStatus;

    jkGuiRend_MenuSetReturnKeyShortcutElement(&jkGuiGcCheats_menu, &jkGuiGcCheats_aElements[ELMT_OK]);
    jkGuiRend_MenuSetEscapeKeyShortcutElement(&jkGuiGcCheats_menu, &jkGuiGcCheats_aElements[ELMT_OK]);
    jkGuiRend_GcSetInitialFocus(&jkGuiGcCheats_menu, &jkGuiGcCheats_aElements[ELMT_INVUL]);
    int ret = jkGuiRend_DisplayAndReturnClicked(&jkGuiGcCheats_menu);

    // Toggles: only touch what changed (the engine's handlers flip or set them).
    if (!!jkGuiGcCheats_aElements[ELMT_INVUL].selectedTextEntry != bInvul)
        jkGuiGcCheats_Run(jkDev_CmdDebugFlags2, 5, bInvul ? "off" : "on");
    if (!!jkGuiGcCheats_aElements[ELMT_FLY].selectedTextEntry != bFly)
        jkGuiGcCheats_Run(jkDev_CmdFly, 0, NULL);
    if (!!jkGuiGcCheats_aElements[ELMT_NOAI].selectedTextEntry != bNoAi)
        jkGuiGcCheats_Run(jkDev_CmdDebugFlags, 0, bNoAi ? "off" : "on");
    if (!!jkGuiGcCheats_aElements[ELMT_SLOWMO].selectedTextEntry != bSlowmo)
        jkGuiGcCheats_Run(jkDev_CmdDebugFlags3, 7, bSlowmo ? "off" : "on");
    if (!!jkGuiGcCheats_aElements[ELMT_FPS].selectedTextEntry != gcStats_IsVisible())
        gcStats_SetVisible(jkGuiGcCheats_aElements[ELMT_FPS].selectedTextEntry);

    return ret == JKGUIGCCHEATS_LEAVE_PAUSE ? JKGUIGCCHEATS_LEAVE_PAUSE : 0;
}

void jkGuiGcCheats_GameFrame(void)
{
    if (jkGuiGcCheats_bSkipPending) {
        jkGuiGcCheats_bSkipPending = 0;
        jkGuiGcCheats_Run(jkDev_CmdEndLevel2, 0, NULL);
    }
}
