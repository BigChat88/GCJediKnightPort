// Display-settings GUI for the Nintendo GameCube.
//
// jkGUIDisplay.c is platform-specific (each backend ships its own, since the
// available display options differ). Options: the video mode (480p with a
// component/digital cable, else 480i), distance fog and the action hand icon.

#include "Gui/jkGUIDisplay.h"

#include "Gui/jkGUIRend.h"
#include "Gui/jkGUI.h"
#include "Gui/jkGUISetup.h"
#include "Platform/GameCube/gcSystem.h"
#include "stdPlatform.h"
#include "jk.h"

enum
{
    GUI_GENERAL = 100,
    GUI_GAMEPLAY = 101,
    GUI_DISPLAY = 102,
    GUI_SOUND = 103,
    GUI_CONTROLS = 104,
};

// Text that isn't a string table key is given as UTF-16 (u"..."), which the
// GUI draws as is when the lookup fails.
int  std3D_GcGetFog(void);
void std3D_GcSetFog(int b);
int  std3D_GcGetHandIcon(void);
void std3D_GcSetHandIcon(int b);
int  std3D_GcGetGlow(void);
void std3D_GcSetGlow(int b);

static jkGuiElement jkGuiDisplay_aElements[15] = {
    { ELEMENT_TEXT,        0,            0, NULL,                   3, {0, 410, 640, 20},   1, 0, NULL,                        0, 0, 0, {0}, 0},
    { ELEMENT_TEXT,        0,            6, "GUI_SETUP",            3, {20, 20, 600, 40},   1, 0, NULL,                        0, 0, 0, {0}, 0},
    { ELEMENT_TEXTBUTTON,  GUI_GENERAL,  2, "GUI_GENERAL",          3, {20, 80, 120, 40},   1, 0, "GUI_GENERAL_HINT",          0, 0, 0, {0}, 0},
    { ELEMENT_TEXTBUTTON,  GUI_GAMEPLAY, 2, "GUI_GAMEPLAY",         3, {140, 80, 120, 40},  1, 0, "GUI_GAMEPLAY_HINT",         0, 0, 0, {0}, 0},
    { ELEMENT_TEXTBUTTON,  GUI_DISPLAY,  2, "GUI_DISPLAY",          3, {260, 80, 120, 40},  1, 0, "GUI_DISPLAY_HINT",          0, 0, 0, {0}, 0},
    { ELEMENT_TEXTBUTTON,  GUI_SOUND,    2, "GUI_SOUND",            3, {380, 80, 120, 40},  1, 0, "GUI_SOUND_HINT",            0, 0, 0, {0}, 0},
    { ELEMENT_TEXTBUTTON,  GUI_CONTROLS, 2, "GUI_CONTROLS",         3, {500, 80, 120, 40},  1, 0, "GUI_CONTROLS_HINT",         0, 0, 0, {0}, 0},
    { ELEMENT_TEXTBUTTON,  1,            2, "GUI_OK",               3, {440, 430, 200, 40}, 1, 0, NULL,                        0, 0, 0, {0}, 0},
    { ELEMENT_TEXTBUTTON, -1,            2, "GUI_CANCEL",           3, {0, 430, 200, 40},   1, 0, NULL,                        0, 0, 0, {0}, 0},

    // 9
    // (its hint shows on the bottom line while it has the focus, like the other setup options)
    { ELEMENT_CHECKBOX,    0,            0, (const char*)u"Progressive scan (480p)", 0, {40, 170, 400, 20}, 1, 0, (const char*)u"Sharper picture with a component cable.", 0, 0, 0, {0}, 0},
    // 10: shown without the cable
    { ELEMENT_TEXT,        0,            0, (const char*)u"Progressive scan (480p) needs a component or digital AV cable.", 3, {0, 170, 640, 20}, 1, 0, NULL, 0, 0, 0, {0}, 0},
    // 11, 12
    { ELEMENT_CHECKBOX,    0,            0, (const char*)u"Distance fog", 0, {40, 210, 400, 20}, 1, 0, (const char*)u"Far away things fade into the colour of the sky.", 0, 0, 0, {0}, 0},
    { ELEMENT_CHECKBOX,    0,            0, (const char*)u"Action hand icon", 0, {40, 240, 400, 20}, 1, 0, (const char*)u"Shows a hand when something in front of you can be used.", 0, 0, 0, {0}, 0},
    // 13
    { ELEMENT_CHECKBOX,    0,            0, (const char*)u"Glow effect", 0, {40, 270, 400, 20}, 1, 0, (const char*)u"Bright lights, sabers and blasts get a soft halo.", 0, 0, 0, {0}, 0},

    { ELEMENT_END,         0,            0, NULL,                   0, {0},                 0, 0, NULL,                        0, 0, 0, {0}, 0},
};

static jkGuiMenu jkGuiDisplay_menu = { jkGuiDisplay_aElements, 0, 0xFF, 0xE1, 0x0F, 0, 0, jkGui_stdBitmaps, jkGui_stdFonts, 0, 0, "thermloop01.wav", "thrmlpu2.wav", 0, 0, 0, 0, 0, 0 };

void jkGuiDisplay_Startup()
{
    jkGui_InitMenu(&jkGuiDisplay_menu, jkGui_stdBitmaps[JKGUI_BM_BK_SETUP]);
}

void jkGuiDisplay_Shutdown()
{
}

int jkGuiDisplay_Show()
{
    int bCan = gcSystem_CanProgressive();
    jkGuiDisplay_aElements[9].bIsVisible = bCan;
    jkGuiDisplay_aElements[10].bIsVisible = !bCan;
    jkGuiDisplay_aElements[9].boxChecked = gcSystem_IsProgressive();
    jkGuiDisplay_aElements[11].boxChecked = std3D_GcGetFog();
    jkGuiDisplay_aElements[12].boxChecked = std3D_GcGetHandIcon();
    jkGuiDisplay_aElements[13].boxChecked = std3D_GcGetGlow();

    jkGui_sub_412E20(&jkGuiDisplay_menu, 102, 104, 102);
    jkGuiRend_MenuSetReturnKeyShortcutElement(&jkGuiDisplay_menu, &jkGuiDisplay_aElements[7]);
    // Going back (B) keeps the changes; no Cancel button.
    jkGuiRend_MenuSetEscapeKeyShortcutElement(&jkGuiDisplay_menu, &jkGuiDisplay_aElements[7]);
    jkGuiDisplay_aElements[8].bIsVisible = 0;
    jkGuiSetup_sub_412EF0(&jkGuiDisplay_menu, 0);
    // Start on the option itself (else the first focusable element is OK).
    jkGuiRend_GcSetInitialFocus(&jkGuiDisplay_menu, bCan ? &jkGuiDisplay_aElements[9] : &jkGuiDisplay_aElements[11]);

    int clicked = jkGuiRend_DisplayAndReturnClicked(&jkGuiDisplay_menu);
    if (clicked != -1) {
        if (bCan)
            gcSystem_SetProgressive(jkGuiDisplay_aElements[9].boxChecked != 0);
        if ((jkGuiDisplay_aElements[11].boxChecked != 0) != std3D_GcGetFog())
            std3D_GcSetFog(jkGuiDisplay_aElements[11].boxChecked != 0);
        if ((jkGuiDisplay_aElements[12].boxChecked != 0) != std3D_GcGetHandIcon())
            std3D_GcSetHandIcon(jkGuiDisplay_aElements[12].boxChecked != 0);
        if ((jkGuiDisplay_aElements[13].boxChecked != 0) != std3D_GcGetGlow())
            std3D_GcSetGlow(jkGuiDisplay_aElements[13].boxChecked != 0);
    }
    return clicked;
}

void jkGuiDisplay_sub_4149C0()
{
}
