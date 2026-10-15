// Window/main-loop glue for the GameCube (libogc). Derived from the Dreamcast
// backend: there is no window system, so this drives the engine's Win32-style
// message handlers directly and presents the GUI through std3D_DrawMenu.

#include "Win95/Window.h"

#include "Win95/stdGdi.h"
#include "Platform/std3D.h"
#include "Main/Main.h"
#include "Main/jkMain.h"
#include "Main/jkGame.h"
#include "Gui/jkGUI.h"
#include "Win95/stdDisplay.h"
#include "World/jkPlayer.h"
#include "Platform/stdControl.h"
#include "stdPlatform.h"
#include "Devices/sithConsole.h"
#include "Platform/wuRegistry.h"
#include "Gui/jkGUIRend.h"

#ifdef TARGET_GAMECUBE

#include "gcOgc.h"
#include "gcInput.h"

extern int jkGuiBuildMulti_bRendering;

int Window_lastXRel = 0;
int Window_lastYRel = 0;
int Window_lastSampleTime = 0;
int Window_lastSampleMs = 0;
int Window_bMouseLeft = 0;
int Window_bMouseRight = 0;
int Window_resized = 0;
int Window_mouseX = 0;
int Window_mouseY = 0;
int Window_mouseWheelX = 0;
int Window_mouseWheelY = 0;
int Window_lastMouseX = 0;
int Window_lastMouseY = 0;
int Window_xPos = 0;
int Window_yPos = 0;
int last_jkGame_isDDraw = 0;
int Window_menu_mouseX = 0;
int Window_menu_mouseY = 0;
extern int Window_needsRecreate;
int Window_bFlipRequested = 0;

void Window_Main_Loop()
{
    jkMain_GuiAdvance();
    Window_msg_main_handler(g_hWnd, WM_PAINT, 0, 0);
}

int Window_Main_Linux(int argc, char** argv)
{
    char cmdLine[1024];
    int result;

    strcpy(cmdLine, "");

    g_handler_count = 0;
    g_thing_two_some_dialog_count = 0;
    g_should_exit = 0;
    g_window_not_destroyed = 0;
    g_hInstance = 0;
    g_nShowCmd = 0;

    for (int i = 1; i < argc; i++)
    {
        strcat(cmdLine, argv[i]);
        strcat(cmdLine, " ");
    }
    stdPlatform_Printf("cmdline: %s\n", cmdLine);

    result = Main_Startup(cmdLine);

    Window_resized = 1;
    Window_xSize = 640;
    Window_ySize = 480;

    if (!result) return result;

    std3D_FreeResources();

    g_window_not_destroyed = 1;

    Window_msg_main_handler(g_hWnd, WM_CREATE, 0, 0);
    Window_msg_main_handler(g_hWnd, WM_ACTIVATE, 2, 0);
    Window_msg_main_handler(g_hWnd, WM_ACTIVATEAPP, 1, 0);
    Window_msg_main_handler(g_hWnd, WM_SHOWWINDOW, 0, 0);
    Window_msg_main_handler(g_hWnd, WM_PAINT, 0, 0);

    while (1)
    {
        Window_Main_Loop();
        if (g_should_exit) break;
    }

    if (jkPlayer_bHasLoadedSettingsOnce) {
        jkPlayer_WriteConf(jkPlayer_playerShortName);
    }

    Main_Shutdown();
    return 1;
}

int Window_DefaultHandler(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam, void* unused)
{
    return 0;
}

int Window_ShowCursorUnwindowed(int a1)
{
    return stdControl_ShowMouseCursor(a1);
}

int Window_MessageLoop()
{
    jkGuiRend_UpdateController();
    Window_SdlUpdate();

    if (Window_bFlipRequested) {
        Window_msg_main_handler(g_hWnd, WM_PAINT, 0, 0);
        Window_bFlipRequested = 0;
    }
    return 0;
}

// START acts as Escape (pause menu in game, back in menus). Menu navigation and
// A/B accept/back go through jkGuiRend_UpdateController -> stdControl.
static void Window_GameCubePollGui()
{
    static uint32_t prevButtons = 0;

    uint32_t buttons = gcInput_ButtonsHeld();
    uint32_t pressed = buttons & ~prevButtons;
    // Update the edge state BEFORE dispatching: the handlers can re-enter this poll
    // through a nested GUI message loop.
    prevButtons = buttons;

    // Secret cheats menu combo (only does something in the pause menu).
    if (gcInput_CheatComboPressed()) {
        extern void jkGuiEsc_GcCheatCombo(void);
        jkGuiEsc_GcCheatCombo();
    }

    if (pressed & PAD_BUTTON_START) {
        Window_msg_main_handler(g_hWnd, WM_KEYFIRST, VK_ESCAPE, 0);
        Window_msg_main_handler(g_hWnd, WM_CHAR, VK_ESCAPE, 0);
    }
}

void Window_SdlUpdate()
{
    if (Main_bHeadless)
        return;

    Window_GameCubePollGui();

    if (Window_resized)
    {
        jkMain_FixRes();
        if (!jkGui_SetModeMenu(0))
        {
            stdDisplay_SetMode(0, 0, 0);
        }

        jkGui_SetModeGame();

        Window_resized = 0;
    }

    Window_lastSampleTime = stdPlatform_GetTimeMsec();

    if (!jkGame_isDDraw)
    {
        if (!jkGuiBuildMulti_bRendering) {
            std3D_StartScene();
            std3D_DrawMenu();
            std3D_EndScene();
        }
        else {
            std3D_DrawMenu();
        }

        if (Window_needsRecreate) {
            std3D_PurgeEntireTextureCache();
            Window_resized = 1;
            Window_needsRecreate = 0;
        }
    }
    else
    {
        // Save mouse position for menu
        if (jkGame_isDDraw != last_jkGame_isDDraw) {
            Window_menu_mouseX = Window_mouseX;
            Window_menu_mouseY = Window_mouseY;
            Window_lastXRel = 0;
            Window_lastYRel = 0;
        }
    }

    last_jkGame_isDDraw = jkGame_isDDraw;
}

void Window_SdlVblank()
{
}

#endif // TARGET_GAMECUBE
