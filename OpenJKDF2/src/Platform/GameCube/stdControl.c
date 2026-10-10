// GameCube controller input backend (libogc PAD). Derived from the Dreamcast one;
// the shared key/axis logic lives in src/Platform/Common/stdControl.c.
//
// Port 0 is presented to the engine as joystick 0:
//   A/B/X/Z        -> KEY_JOY1_B1/B2/B3/B5 (A/B also accept/back in menus)
//   Y              -> tap KEY_JOY1_B4, held 1 s KEY_JOY1_B6
//   L/R (digital)  -> KEY_JOY1_B10/B11
//   START          -> KEY_JOY1_B8 (and Escape, see Window_GameCube.c)
//   D-pad          -> joystick hat
//   Main stick     -> AXIS_JOY1_X/Y
//   C-stick        -> AXIS_JOY1_Z/R, and mouse-look deltas while in game
//
// In-game bindings (sithControl_MapDefaultsJoystick): main stick moves/strafes,
// C-stick aims, R fire, Z secondary fire, L use Force power, A jump, X crouch,
// B activate (or use item when nothing is in front), Y lightsaber (hold 1 s:
// camera view), D-pad left/right weapon, up Force power, down item.

#include "Platform/stdControl.h"

#include "Devices/sithControl.h"
#include "Win95/Window.h"
#include "Main/jkGame.h"
#include "stdPlatform.h"

#include "jk.h"
#include <math.h>

#include "Platform/GameCube/gcOgc.h"
#include "Platform/GameCube/gcInput.h"

// Pad sticks report roughly +-100 at full deflection (s8 range).
#define GC_STICK_RANGE      (100)
#define GC_STICK_DEADZONE   (12)

// C-stick aiming: it has a short throw, so a linear response felt like an on/off
// button. Quadratic curve (fine control near the centre, full speed at the edge),
// in mouse-delta units per frame, with fractions carried over between frames.
#define GC_CSTICK_RANGE     (80)
#define GC_CSTICK_DEADZONE  (10)
#define GC_CSTICK_MAX_YAW   (11.0f)
#define GC_CSTICK_MAX_PITCH (6.5f)

uint8_t stdControl_aDebounce[JK_NUM_KEYS];
int stdControl_bControllerEscapeKey = 0;
int stdControl_bControllerEscapeKey_last = 0;
static int stdControl_aJoystickNumAxes[JK_NUM_JOYSTICKS] = {0};
static int stdControl_bKeyboardBeingShown = 0;

void stdControl_SetSDLKeydown(int keyNum, int bDown, uint32_t readTime)
{
}

void stdControl_FreeSdlJoysticks()
{
    for (int i = 0; i < JK_NUM_JOYSTICKS; i++) {
        stdControl_aJoystickExists[i] = 0;
        stdControl_aJoystickMaxButtons[i] = 0;
        stdControl_aJoystickEnabled[i] = 0;
        stdControl_aJoystickNumAxes[i] = 0;
    }
}

// The pad's stick axes. They must be registered before any binding is made:
// sithControl_BindAxis only binds a registered axis (stdControl_EnableAxis) and
// silently drops the binding otherwise. They used to be registered only when the
// pad was first read, so controls set up before that (some ways into a game,
// often a multiplayer host) lost the main stick: no walking, while the buttons
// (jump, fire, weapons) still worked.
static void stdControl_GcRegisterStickAxes(void)
{
    // The main stick's dead zone is applied (and rescaled) in stdControl_GcMoveStick:
    // the engine's own 20% one made movement start with a jump.
    stdControl_RegisterAxis(AXIS_JOY1_X, -0x7FFF, 0x7FFF, 0.0);
    stdControl_RegisterAxis(AXIS_JOY1_Y, -0x7FFF, 0x7FFF, 0.0);
    stdControl_RegisterAxis(AXIS_JOY1_Z, -0x7FFF, 0x7FFF, 0.2);
    stdControl_RegisterAxis(AXIS_JOY1_R, -0x7FFF, 0x7FFF, 0.2);
}

int stdControl_Startup()
{
    _memset(stdControl_aKeyIdleTimes, 0, sizeof(int) * JK_NUM_KEYS);
    _memset(stdControl_aKeyInfo, 0, sizeof(int) * JK_NUM_KEYS);
    _memset(stdControl_aAxes, 0, sizeof(stdControlJoystickEntry) * JK_NUM_AXES);
    _memset(stdControl_aAxisStates, 0, sizeof(int) * JK_NUM_AXES);
    _memset(stdControl_aDebounce, 0, sizeof(stdControl_aDebounce));

    stdControl_FreeSdlJoysticks();

    // Mouse axes (matches the SDL2/TWL/DC ranges); fed by the C-stick in game.
    stdControl_RegisterAxis(AXIS_MOUSE_X, -250, 250, 0.0);
    stdControl_RegisterAxis(AXIS_MOUSE_Y, -200, 200, 0.0);
    stdControl_RegisterAxis(AXIS_MOUSE_Z, -20, 20, 0.0);
    stdControl_GcRegisterStickAxes();

    stdControl_Reset();
    stdControl_bStartup = 1;
    return 1;
}

void stdControl_Shutdown()
{
    stdControl_FreeSdlJoysticks();
    stdControl_bStartup = 0;
}

int stdControl_Open()
{
    stdControl_bOpen = 1;
    stdControl_bControlsActive = 1;
    return 1;
}

int stdControl_Close()
{
    if (!stdControl_bOpen)
        return 0;

    stdControl_bControlsActive = 0;
    stdControl_bOpen = 0;
    return 1;
}

void stdControl_Flush()
{
    stdControl_curReadTime = stdPlatform_GetTimeMsec();
}

void stdControl_SetActivation(int a)
{
    if (stdControl_bOpen)
        stdControl_bControlsActive = !!a;
}

static int _cursorState = 0;

int stdControl_ShowMouseCursor(int a)
{
    if (a)
        _cursorState++;
    else
        _cursorState--;
    return _cursorState;
}

void stdControl_ToggleMouse()
{
    stdControl_bReadMouse = !stdControl_bReadMouse;
}

void stdControl_InitSdlJoysticks() {}
void stdControl_ShowSystemKeyboard() {}
void stdControl_HideSystemKeyboard() {}

static float stdControl_GcLookCurve(int v)
{
    int mag = v < 0 ? -v : v;
    if (mag <= GC_CSTICK_DEADZONE) return 0.0f;
    float n = (float)(mag - GC_CSTICK_DEADZONE) / (GC_CSTICK_RANGE - GC_CSTICK_DEADZONE);
    if (n > 1.0f) n = 1.0f;
    n *= n;
    return v < 0 ? -n : n;
}

// Main stick (movement): radial dead zone, then a linear ramp that reaches full
// speed at the stick's real travel (~80 on a GameCube pad, less on the diagonals
// of its octagonal gate, hence the radial normalisation), in every direction.
#define GC_MSTICK_DEADZONE  (10)
#define GC_MSTICK_RANGE     (78)

static void stdControl_GcMoveStick(int sx, int sy, int* pOutX, int* pOutY)
{
    float fx = (float)sx, fy = (float)sy;
    float mag = sqrtf(fx * fx + fy * fy);
    if (mag <= GC_MSTICK_DEADZONE) { *pOutX = *pOutY = 0; return; }
    float n = (mag - GC_MSTICK_DEADZONE) / (GC_MSTICK_RANGE - GC_MSTICK_DEADZONE);
    if (n > 1.0f) n = 1.0f;
    float scale = n * 0x7FFF / mag;
    *pOutX = (int)(fx * scale);
    *pOutY = (int)(fy * scale);
}

static int stdControl_GcAxis(int v)
{
    if (v > -GC_STICK_DEADZONE && v < GC_STICK_DEADZONE)
        return 0;
    int out = (v * 0x7FFF) / GC_STICK_RANGE;
    if (out > 0x7FFF) out = 0x7FFF;
    if (out < -0x7FFF) out = -0x7FFF;
    return out;
}

void stdControl_ReadControls()
{
    flex_d_t khz;

    if (!stdControl_bControlsActive)
        return;

    stdControl_bReadJoysticks = 1;
    stdControl_aJoystickNumAxes[0] = 2;
    stdControl_aJoystickMaxButtons[0] = 8;
    stdControl_aAxisEnabled[0] = 1;
    stdControl_aAxisEnabled[1] = 1;
    stdControl_aJoystickExists[0] = 1;
    sithWeapon_controlOptions &= ~(1 << 5); // Enable joystick

    stdControl_GcRegisterStickAxes();

    stdControl_bControlsIdle = 1;
    stdControl_curReadTime = stdPlatform_GetTimeMsec();
    stdControl_readDeltaTime = stdControl_curReadTime - stdControl_lastReadTime;
    khz = (stdControl_readDeltaTime != 0) ? (1.0 / (flex_d_t)(stdControl_readDeltaTime)) : (flex_d_t)1.0;
    stdControl_updateKHz = khz;
    stdControl_updateHz = khz * 1000.0;

    uint32_t b = gcInput_ButtonsHeld();
    uint32_t t = stdControl_curReadTime;

    stdControl_UpdateKeyState(KEY_JOY1_B1,  !!(b & PAD_BUTTON_A), t);
    stdControl_UpdateKeyState(KEY_JOY1_B2,  !!(b & PAD_BUTTON_B), t);
    stdControl_UpdateKeyState(KEY_JOY1_B3,  !!(b & PAD_BUTTON_X), t);
    // Y is two virtual buttons: a tap (released within 1 s) is KEY_JOY1_B4 (draw
    // the lightsaber), holding it 1 s is KEY_JOY1_B6 (switch 1st/3rd person). Each
    // is down for one read, so the game sees one press.
    {
        static int yState = 0; // 0 up, 1 down, 2 down and the hold already fired
        static uint32_t yDownAt = 0;
        int yTap = 0, yHold = 0;
        if (b & PAD_BUTTON_Y) {
            if (!yState) { yState = 1; yDownAt = t; }
            else if (yState == 1 && t - yDownAt >= 1000) { yHold = 1; yState = 2; }
        }
        else {
            if (yState == 1) yTap = 1;
            yState = 0;
        }
        stdControl_UpdateKeyState(KEY_JOY1_B4, yTap, t);
        stdControl_UpdateKeyState(KEY_JOY1_B6, yHold, t);
    }
    stdControl_UpdateKeyState(KEY_JOY1_B5,  !!(b & PAD_TRIGGER_Z), t);
    stdControl_UpdateKeyState(KEY_JOY1_B8,  !!(b & PAD_BUTTON_START), t);
    stdControl_UpdateKeyState(KEY_JOY1_B10, !!(b & PAD_TRIGGER_L) || PAD_TriggerL(0) > 100, t);
    stdControl_UpdateKeyState(KEY_JOY1_B11, !!(b & PAD_TRIGGER_R) || PAD_TriggerR(0) > 100, t);

    stdControl_UpdateKeyState(KEY_JOY1_HLEFT,  !!(b & PAD_BUTTON_LEFT),  t);
    stdControl_UpdateKeyState(KEY_JOY1_HUP,    !!(b & PAD_BUTTON_UP),    t);
    stdControl_UpdateKeyState(KEY_JOY1_HRIGHT, !!(b & PAD_BUTTON_RIGHT), t);
    stdControl_UpdateKeyState(KEY_JOY1_HDOWN,  !!(b & PAD_BUTTON_DOWN),  t);

    // Pad Y is up-positive; the engine's joystick Y is down-positive.
    int sx = PAD_StickX(0), sy = PAD_StickY(0);
    int cx = PAD_SubStickX(0), cy = PAD_SubStickY(0);
#ifdef GC_INPUT_SCRIPT
    if (b & GCINPUT_SCRIPT_CLEFT)  cx = -80;
    if (b & GCINPUT_SCRIPT_CRIGHT) cx = 80;
    if (b & GCINPUT_SCRIPT_CUP)    cy = 80;
    if (b & GCINPUT_SCRIPT_CDOWN)  cy = -80;
    if (b & GCINPUT_SCRIPT_SLEFT)  sx = -80;
    if (b & GCINPUT_SCRIPT_SRIGHT) sx = 80;
    if (b & GCINPUT_SCRIPT_SUP)    sy = 80;
    if (b & GCINPUT_SCRIPT_SDOWN)  sy = -80;
#endif
    int moveX, moveY;
    stdControl_GcMoveStick(sx, -sy, &moveX, &moveY);
    stdControl_aAxisStates[AXIS_JOY1_X] = moveX;
    stdControl_aAxisStates[AXIS_JOY1_Y] = moveY;
    stdControl_aAxisStates[AXIS_JOY1_Z] = stdControl_GcAxis(cx);
    stdControl_aAxisStates[AXIS_JOY1_R] = stdControl_GcAxis(-cy);

    if (b || stdControl_aAxisStates[AXIS_JOY1_X] || stdControl_aAxisStates[AXIS_JOY1_Y])
        stdControl_bControlsIdle = 0;

    // C-stick drives the mouse-look axes in game.
    if (jkGame_isDDraw) {
        static float accX = 0.0f, accY = 0.0f;
        accX += stdControl_GcLookCurve(cx) * GC_CSTICK_MAX_YAW;
        accY -= stdControl_GcLookCurve(cy) * GC_CSTICK_MAX_PITCH;
        int dx = (int)accX, dy = (int)accY;   // truncates toward zero
        accX -= dx;
        accY -= dy;
        if (!cx || stdControl_GcLookCurve(cx) == 0.0f) accX = 0.0f;
        if (!cy || stdControl_GcLookCurve(cy) == 0.0f) accY = 0.0f;
        Window_lastXRel += dx;
        Window_lastYRel += dy;
    }

    stdControl_ReadMouse();
    stdControl_lastReadTime = stdControl_curReadTime;
}

void stdControl_ReadMouse()
{
    stdControl_aAxisStates[AXIS_MOUSE_X] = Window_lastXRel;
    stdControl_aAxisStates[AXIS_MOUSE_Y] = Window_lastYRel;
    stdControl_aAxisStates[AXIS_MOUSE_Z] = Window_mouseWheelY;

    if (Window_lastXRel || Window_lastYRel)
        stdControl_bControlsIdle = 0;

    stdControl_dwLastMouseX = stdControl_aAxisStates[AXIS_MOUSE_X];
    stdControl_dwLastMouseY = stdControl_aAxisStates[AXIS_MOUSE_Y];

    Window_lastXRel = 0;
    Window_lastYRel = 0;
    Window_mouseWheelX = 0;
    Window_mouseWheelY = 0;

    stdControl_UpdateKeyState(KEY_MOUSE_B1, Window_bMouseLeft,  stdControl_curReadTime);
    stdControl_UpdateKeyState(KEY_MOUSE_B2, Window_bMouseRight, stdControl_curReadTime);
}

BOOL stdControl_IsSystemKeyboardShowing()
{
    return stdControl_bKeyboardBeingShown;
}
