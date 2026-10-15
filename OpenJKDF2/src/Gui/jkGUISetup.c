#include "jkGUISetup.h"

#include "General/Darray.h"
#include "General/stdBitmap.h"
#include "General/stdFont.h"
#include "General/stdStrTable.h"
#include "General/stdFileUtil.h"
#include "Engine/rdMaterial.h" // TODO move tVBuffer
#include "stdPlatform.h"
#include "jk.h"
#include "Gui/jkGUIRend.h"
#include "Gui/jkGUI.h"
#include "Gui/jkGUIGameplay.h"
#include "Gui/jkGUIDisplay.h"
#include "Gui/jkGUISound.h"
#include "Gui/jkGUIKeyboard.h"
#include "Gui/jkGUIMouse.h"
#include "Gui/jkGUIJoystick.h"
#include "Gui/jkGUIGeneral.h"
#include "Gui/jkGUIControlOptions.h"
#ifdef TARGET_GAMECUBE
#include "Gui/jkGUIDialog.h"
#include "Main/jkStrings.h"
#include "World/jkPlayer.h"
#include "Devices/sithSoundMixer.h"
#include "Win95/stdSound.h"
#include "gcCard.h"
#include "gcDvd.h"
#include "gcSystem.h"
#endif

static jkGuiElement jkGuiSetup_buttons[9] = {
    {ELEMENT_TEXT, 0, 0, 0, 3, {0, 410, 640, 20}, 1, 0, 0, 0, 0, 0, {0}, 0},
    {ELEMENT_TEXT, 0, 6, "GUI_SETUP", 3, {20, 20, 600, 40}, 1, 0, 0, 0, 0, 0, {0}, 0},
    {ELEMENT_TEXTBUTTON, 100, 2, "GUI_GENERAL", 3, {20, 80, 120, 40},  1, 0, "GUI_GENERAL_HINT", 0, 0, 0, {0}, 0},
    {ELEMENT_TEXTBUTTON, 101, 2, "GUI_GAMEPLAY", 3, {140, 80, 120, 40}, 1, 0, "GUI_GAMEPLAY_HINT", 0, 0, 0, {0}, 0},
    {ELEMENT_TEXTBUTTON, 102, 2, "GUI_DISPLAY", 3, {260, 80, 120, 40},  1, 0, "GUI_DISPLAY_HINT", 0, 0, 0, {0}, 0},
    {ELEMENT_TEXTBUTTON, 103, 2, "GUI_SOUND", 3, {380, 80, 120, 40}, 1, 0, "GUI_SOUND_HINT", 0, 0, 0, {0}, 0},
    {ELEMENT_TEXTBUTTON, 104, 2, "GUI_CONTROLS", 3, {500, 80, 120, 40}, 1, 0, "GUI_CONTROLS_HINT", 0, 0, 0, {0}, 0},
    {ELEMENT_TEXTBUTTON, 1, 2, "GUI_OK", 3, {440, 430, 200, 40}, 1, 0, 0, 0, 0, 0, {0}, 0},
    {ELEMENT_END, 0, 0, 0, 0, {0}, 0, 0, 0, 0, 0, 0, {0}, 0},
};

static jkGuiMenu jkGuiSetup_menu = {jkGuiSetup_buttons, 0, 0xFF, 0xE1, 0xF, 0, 0, jkGui_stdBitmaps, jkGui_stdFonts, 0, 0, "thermloop01.wav", "thrmlpu2.wav", 0, 0, 0, 0, 0, 0};

static jkGuiElement jkGuiSetupControls_buttons[13] = {
    {ELEMENT_TEXT, 0, 0, 0, 3, {0, 410, 640, 20}, 1, 0, 0, 0, 0, 0, {0}, 0},
    {ELEMENT_TEXT, 0, 6, "GUI_SETUP", 3, {20, 20, 600, 40}, 1, 0, 0, 0, 0, 0, {0}, 0},
    {ELEMENT_TEXTBUTTON, 100, 2, "GUI_GENERAL", 3, {20, 80, 120, 40},  1, 0, "GUI_GENERAL_HINT", 0, 0, 0, {0}, 0},
    {ELEMENT_TEXTBUTTON, 101, 2, "GUI_GAMEPLAY", 3, {140, 80, 120, 40}, 1, 0, "GUI_GAMEPLAY_HINT", 0, 0, 0, {0}, 0},
    {ELEMENT_TEXTBUTTON, 102, 2, "GUI_DISPLAY", 3, {260, 80, 120, 40},  1, 0, "GUI_DISPLAY_HINT", 0, 0, 0, {0}, 0},
    {ELEMENT_TEXTBUTTON, 103, 2, "GUI_SOUND", 3, {380, 80, 120, 40}, 1, 0, "GUI_SOUND_HINT", 0, 0, 0, {0}, 0},
    {ELEMENT_TEXTBUTTON, 104, 2, "GUI_CONTROLS", 3, {500, 80, 120, 40}, 1, 0, "GUI_CONTROLS_HINT", 0, 0, 0, {0}, 0},
    {ELEMENT_TEXTBUTTON, 105, 2, "GUI_KEYBOARD", 3, {40, 120, 140, 40}, 1, 0, "GUI_KEYBOARD_HINT", 0, 0, 0, {0}, 0},
    {ELEMENT_TEXTBUTTON, 106, 2, "GUI_MOUSE", 3, {180, 120, 140, 40},  1, 0, "GUI_MOUSE_HINT", 0, 0, 0, {0}, 0},
    {ELEMENT_TEXTBUTTON, 107, 2, "GUI_JOYSTICK", 3, {320, 120, 140, 40}, 1, 0, "GUI_JOYSTICK_HINT", 0, 0, 0, {0}, 0},
    {ELEMENT_TEXTBUTTON, 108, 2, "GUI_CONTROLOPTIONS", 3, {460, 120, 140,  40}, 1, 0, "GUI_CONTROLOPTIONS_HINT", 0, 0, 0, {0}, 0},
    {ELEMENT_TEXTBUTTON, 1, 2, "GUI_OK", 3, {440, 430, 200, 40}, 1, 0, 0, 0, 0, 0, {0}, 0},
    {ELEMENT_END, 0, 0, 0, 0, {0}, 0, 0, 0, 0, 0, 0, {0}, 0},
};

static jkGuiMenu jkGuiSetupControls_menu = {jkGuiSetupControls_buttons, 0, 0xFF, 0xE1, 0xF, 0, 0, jkGui_stdBitmaps, jkGui_stdFonts, 0, 0, "thermloop01.wav", "thrmlpu2.wav", 0, 0, 0, 0, 0, 0};

void jkGuiSetup_sub_412EF0(jkGuiMenu *menu, int a2)
{
    jkGuiElement *paElements; // eax

    paElements = menu->paElements;
    paElements[2].enableHover = 1;
    paElements[3].enableHover = 1;
    paElements[4].enableHover = 1;
    paElements[5].enableHover = 1;
    paElements[6].enableHover = 1;
    if ( a2 )
    {
        paElements[7].enableHover = 1;
        paElements[8].enableHover = 1;
        paElements[9].enableHover = 1;
        paElements[10].enableHover = 1;
    }
}

#ifdef TARGET_GAMECUBE
// Added: Setup changes go to the memory card only if the player says so on the
// way out (a card write takes a moment); "No" puts the old settings back. The
// settings are files in the dvd: overlay (profile, options) plus a few globals
// that the pages apply right away (video mode, volumes).
static uint8_t* jkGuiSetup_pGcSnapshot = NULL;
static uint32_t jkGuiSetup_gcSnapshotLen = 0;
static int      jkGuiSetup_bGcWasProgressive;
static flex_t   jkGuiSetup_gcMusicVolume, jkGuiSetup_gcSfxVolume;
static int      jkGuiSetup_gcNumChannels;

static uint8_t* jkGuiSetup_GcSerialize(uint32_t* pLen)
{
    uint32_t cap = gcDvd_OverlaySerializedSize();
    uint8_t* p = (uint8_t*)malloc(cap ? cap : 1);
    if (p) *pLen = gcDvd_OverlaySerialize(p, cap);
    return p;
}

static void jkGuiSetup_GcBegin(void)
{
    free(jkGuiSetup_pGcSnapshot);
    jkGuiSetup_pGcSnapshot = jkGuiSetup_GcSerialize(&jkGuiSetup_gcSnapshotLen);
    jkGuiSetup_bGcWasProgressive = gcSystem_IsProgressive();
    jkGuiSetup_gcMusicVolume = jkGuiSound_musicVolume;
    jkGuiSetup_gcSfxVolume = jkGuiSound_sfxVolume;
    jkGuiSetup_gcNumChannels = jkGuiSound_numChannels;
}

static void jkGuiSetup_GcEnd(void)
{
    if (!jkGuiSetup_pGcSnapshot) return;
    uint32_t len = 0;
    uint8_t* now = jkGuiSetup_GcSerialize(&len);
    int bChanged = !now || len != jkGuiSetup_gcSnapshotLen
                   || memcmp(now, jkGuiSetup_pGcSnapshot, len)
                   || gcSystem_IsProgressive() != jkGuiSetup_bGcWasProgressive;
    free(now);

    if (bChanged) {
        char16_t* title = jkStrings_GetUniStringWithFallback("GUI_SETUP");
        if (jkGuiDialog_YesNoDialog(title, (char16_t*)u"Save the changes to the memory card?")) {
            if (!gcCard_Save()) {
                char16_t msg[256];
                jk_snwprintf(msg, 256, u"The settings could not be written to the memory card (%S).",
                             gcCard_GetLastError());
                jkGuiDialog_ErrorDialog(title, msg);
            }
        }
        else {
            gcDvd_OverlayDeserialize(jkGuiSetup_pGcSnapshot, jkGuiSetup_gcSnapshotLen);
            jkPlayer_ReadConf(jkPlayer_playerShortName);
            if (gcSystem_IsProgressive() != jkGuiSetup_bGcWasProgressive)
                gcSystem_SetProgressive(jkGuiSetup_bGcWasProgressive);
            jkGuiSound_musicVolume = jkGuiSetup_gcMusicVolume;
            jkGuiSound_sfxVolume = jkGuiSetup_gcSfxVolume;
            jkGuiSound_numChannels = jkGuiSetup_gcNumChannels;
            sithSoundMixer_UpdateMusicVolume(jkGuiSound_musicVolume);
            stdSound_SetMenuVolume(jkGuiSound_sfxVolume);
        }
    }
    free(jkGuiSetup_pGcSnapshot);
    jkGuiSetup_pGcSnapshot = NULL;
}
#endif

void jkGuiSetup_Show()
{
    int i; // esi
    int v1; // edi
    int v2; // eax
;
#ifdef TARGET_GAMECUBE
    jkGuiSetup_GcBegin();
#endif
    jkGuiRend_MenuSetReturnKeyShortcutElement(&jkGuiSetup_menu, &jkGuiSetup_buttons[7]);
    jkGuiRend_MenuSetEscapeKeyShortcutElement(&jkGuiSetup_menu, &jkGuiSetup_buttons[7]);
    for ( i = jkGuiRend_DisplayAndReturnClicked(&jkGuiSetup_menu); i != -1; i = jkGuiRend_DisplayAndReturnClicked(&jkGuiSetup_menu) )
    {
        if ( i == 1 )
            break;
        if ( i >= 100 )
        {
            while ( 2 )
            {
                if ( i <= 104 )
                {
                    switch ( i )
                    {
                        case 100:
                            i = jkGuiGeneral_Show();
                            goto LABEL_23;
                        case 101:
                            i = jkGuiGameplay_Show();
                            goto LABEL_23;
                        case 102:
                            i = jkGuiDisplay_Show();
                            goto LABEL_23;
                        case 103:
                            i = jkGuiSound_Show();
                            goto LABEL_23;
                        case 104:
#ifdef TARGET_GAMECUBE
                            // Added: the controller layout is fixed, so Controls goes
                            // straight to the general options (no keyboard/mouse/joystick).
                            i = jkGuiControlOptions_Show();
                            goto LABEL_23;
#endif
                            do
                            {
                                jkGui_sub_412E20(&jkGuiSetupControls_menu, 105, 108, 0);
                                jkGui_sub_412E20(&jkGuiSetupControls_menu, 102, 107, 104);
                                jkGuiRend_MenuSetReturnKeyShortcutElement(&jkGuiSetupControls_menu, &jkGuiSetupControls_buttons[11]);
                                jkGuiRend_MenuSetEscapeKeyShortcutElement(&jkGuiSetupControls_menu, &jkGuiSetupControls_buttons[11]);
                                i = jkGuiRend_DisplayAndReturnClicked(&jkGuiSetupControls_menu);
                                v1 = 0;
                                while ( i >= 105 )
                                {
                                    if ( i > 108 )
                                        break;
                                    switch ( i )
                                    {
                                        case 105:
                                            v2 = jkGuiKeyboard_Show();
                                            goto LABEL_17;
                                        case 106:
                                            v2 = jkGuiMouse_Show();
                                            goto LABEL_17;
                                        case 107:
                                            v2 = jkGuiJoystick_Show();
                                            goto LABEL_17;
                                        case 108:
                                            v2 = jkGuiControlOptions_Show();
LABEL_17:
                                            i = v2;
                                            v1 = 1;
                                            break;
                                        default:
                                            break;
                                    }
                                    if ( !v1 )
                                    {
                                        jkGui_sub_412E20(&jkGuiSetup_menu, 105, 108, i);
                                        jkGuiSetup_menu.paElements[jkGuiSetup_menu.clickableIdxIdk].wstr = 0; // MOTS added
                                        jkGuiRend_Paint(&jkGuiSetup_menu);
                                    }
                                }
                            }
                            while ( v1 );
                            if ( i != -1 )
                                goto LABEL_23;
#ifdef TARGET_GAMECUBE
                            jkGuiSetup_GcEnd();
#endif
                            return;
                        default:
LABEL_23:
                            jkGui_sub_412E20(&jkGuiSetup_menu, 100, 104, i);
                            jkGuiSetup_menu.paElements[jkGuiSetup_menu.clickableIdxIdk].wstr = 0; // MOTS added
                            jkGuiRend_Paint(&jkGuiSetup_menu);
                            if ( i < 100 )
                                break;
                            continue;
                    }
                }
                break;
            }
        }
        jkGuiRend_MenuSetReturnKeyShortcutElement(&jkGuiSetup_menu, &jkGuiSetup_buttons[7]);
        jkGuiRend_MenuSetEscapeKeyShortcutElement(&jkGuiSetup_menu, &jkGuiSetup_buttons[7]);
    }
#ifdef TARGET_GAMECUBE
    jkGuiSetup_GcEnd();
#endif
}

void jkGuiSetup_Startup()
{
    stdPlatform_Printf("OpenJKDF2: %s\n", __func__); // Added

    jkGui_InitMenu(&jkGuiSetup_menu, jkGui_stdBitmaps[JKGUI_BM_BK_SETUP]);
    jkGui_InitMenu(&jkGuiSetupControls_menu, jkGui_stdBitmaps[JKGUI_BM_BK_SETUP]);
}

void jkGuiSetup_Shutdown()
{
    stdPlatform_Printf("OpenJKDF2: %s\n", __func__); // Added
}
