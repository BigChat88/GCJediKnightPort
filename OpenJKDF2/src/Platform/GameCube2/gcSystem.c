// GameCube system bring-up: VI (double-buffered XFB), GX, controllers, and stdio
// routed to Dolphin's OSReport UART so engine logs show up in Dolphin's log window.

#include "gcSystem.h"
#include "gcCard.h"
#include "gcInput.h"
#include "gcGame.h"

#include <ansndlib.h>

#include <sys/iosupport.h>
#include <ogc/lwp_watchdog.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define GC_FIFO_SIZE (256 * 1024)

GXRModeObj* gcSystem_pRmode = NULL;

static void* gcSystem_aXfb[2] = {NULL, NULL};
static int   gcSystem_curXfb = 0;
static void* gcSystem_pFifo = NULL;

static int gcSystem_bootDone = 0;

// Boot console: until the game presents its first frame, stdout/stderr go both to
// OSReport and to the screen, so a hang during startup on real hardware (where
// OSReport goes nowhere) shows how far it got instead of a black screen.
static const devoptab_t* gcSystem_pConsoleDev = NULL;
static const devoptab_t* gcSystem_pReportDev = NULL;
static devoptab_t gcSystem_teeDev;
static volatile int gcSystem_bBootConsole = 0;
static int gcSystem_conRow = 0;
static int gcSystem_conRows = 25;
static int gcSystem_conCol = 0;
static int gcSystem_conCols = 75;

// libogc's console wraps back to the top instead of scrolling, writing over the
// old lines; start a fresh page instead.
static void gcSystem_ConsoleWrite(struct _reent* r, void* fd, const char* ptr, size_t len)
{
    if (gcSystem_conRow >= gcSystem_conRows - 1) {
        gcSystem_pConsoleDev->write_r(r, fd, "\x1b[2J", 4);
        gcSystem_conRow = 0;
        gcSystem_conCol = 0;
    }
    gcSystem_pConsoleDev->write_r(r, fd, ptr, len);
    for (size_t i = 0; i < len; i++) {
        if (ptr[i] == '\n' || ++gcSystem_conCol >= gcSystem_conCols) {
            gcSystem_conRow++;
            gcSystem_conCol = 0;
        }
    }
}

// Set while a memory card write is in flight (gcCard): the OSReport UART sits on
// the same EXI channel as the memory card, and its writes corrupt card transfers.
volatile int gcSystem_bMuteReport = 0;

#ifdef GC_LIBOGC2
// libogc2 only writes its report UART on development consoles, and Dolphin (like
// a retail console) isn't one: all the log was lost. Written here the way
// devkitPro's libogc did: the IPL device's UART (EXI 0, device 1, address
// 0x20010000), which Dolphin shows as OSReport. (On a retail console the writes
// go nowhere.)
static void gcSystem_UartWrite(const char* p, size_t len)
{
    while (len) {
        u32 n = len > 64 ? 64 : (u32)len;
        if (!EXI_Lock(EXI_CHANNEL_0, EXI_DEVICE_1, NULL)) return;
        if (!EXI_Select(EXI_CHANNEL_0, EXI_DEVICE_1, EXI_SPEED8MHZ)) {
            EXI_Unlock(EXI_CHANNEL_0);
            return;
        }
        u32 cmd = 0xa0010000;
        EXI_ImmEx(EXI_CHANNEL_0, &cmd, sizeof(cmd), EXI_WRITE);
        EXI_ImmEx(EXI_CHANNEL_0, (void*)p, n, EXI_WRITE);
        EXI_Deselect(EXI_CHANNEL_0);
        EXI_Unlock(EXI_CHANNEL_0);
        p += n;
        len -= n;
    }
}
#endif

static ssize_t gcSystem_TeeWrite(struct _reent* r, void* fd, const char* ptr, size_t len)
{
    if (!gcSystem_bMuteReport)
#ifdef GC_LIBOGC2
        gcSystem_UartWrite(ptr, len);
#else
        gcSystem_pReportDev->write_r(r, fd, ptr, len);
#endif
    if (gcSystem_bBootConsole)
        gcSystem_ConsoleWrite(r, fd, ptr, len);
    return len;
}

void gcSystem_BootNotice(const char* text, int ms)
{
    if (!gcSystem_bBootConsole) return;
    gcSystem_pConsoleDev->write_r(_REENT, NULL, "\x1b[2J", 4);
    gcSystem_conRow = 0;
    gcSystem_conCol = 0;
    printf("%s", text);
    for (int t = 0; t < ms; t += 16)
        VIDEO_WaitVSync();
}

static void gcSystem_StartBootConsole(GXRModeObj* rmode)
{
#ifdef GC_BOOT_CONSOLE
    // Dev builds (GC_BOOT_CONSOLE=1): show the startup log on screen too.
    CON_Init(gcSystem_aXfb[0], 20, 20, rmode->fbWidth, rmode->xfbHeight,
             rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    gcSystem_pConsoleDev = devoptab_list[STD_OUT];
    gcSystem_conRows = (rmode->xfbHeight - 40) / 16;   // 8x16 font
    gcSystem_conCols = (rmode->fbWidth - 40) / 8;
#endif

    // stdout + stderr -> Dolphin OSReport.
    SYS_STDIO_Report(true);
    gcSystem_pReportDev = devoptab_list[STD_OUT];

    gcSystem_teeDev = *gcSystem_pReportDev;
    gcSystem_teeDev.write_r = gcSystem_TeeWrite;
    devoptab_list[STD_OUT] = &gcSystem_teeDev;
    devoptab_list[STD_ERR] = &gcSystem_teeDev;
#ifdef GC_BOOT_CONSOLE
    gcSystem_bBootConsole = 1;
#endif

    printf("OpenJKDF2 for GameCube v" GC_PORT_VERSION ", built " __DATE__ " " __TIME__ "\n");
}

// libogc's preferred mode, and its progressive/interlaced variants (progressive
// NULL without a component/digital cable).
static void gcSystem_VideoModes(GXRModeObj** ppProg, GXRModeObj** ppInt)
{
    GXRModeObj* pref = VIDEO_GetPreferredMode(NULL);
    *ppProg = pref;
    if (pref == &TVNtsc480Prog)           *ppInt = &TVNtsc480IntDf;
    else if (pref == &TVEurgb60Hz480Prog) *ppInt = &TVEurgb60Hz480IntDf;
    else if (pref == &TVMpal480Prog)      *ppInt = &TVMpal480IntDf;
    else if (pref == &TVPal576ProgScale) {
        // PAL consoles only do progressive as 480p at 60 Hz.
        *ppProg = &TVEurgb60Hz480Prog;
        *ppInt = VIDEO_GetCurrentTvMode() == VI_EURGB60 ? &TVEurgb60Hz480IntDf : &TVPal576IntDfScale;
    }
    else {
        *ppProg = NULL;
        *ppInt = pref;
    }
    // Added: progressive only with a component/digital cable actually plugged in
    // (the preferred mode follows the console's/Swiss's progressive setting too).
    if (*ppProg && !VIDEO_HaveComponentCable()) {
        if (pref == *ppProg) *ppInt = (*ppProg == &TVEurgb60Hz480Prog) ? &TVEurgb60Hz480IntDf
                                    : (*ppProg == &TVMpal480Prog) ? &TVMpal480IntDf : &TVNtsc480IntDf;
        *ppProg = NULL;
    }
}

// Pick 480i or 480p. With a component/digital cable it's 480p, unless B is held
// at boot (for screens that can't show 480p) or 480i was chosen before (Display
// menu, or an earlier boot with B); the choice is kept in the card settings.
static GXRModeObj* gcSystem_pProgMode = NULL; // NULL: no component/digital cable
static GXRModeObj* gcSystem_pIntMode = NULL;

static GXRModeObj* gcSystem_ChooseVideoMode(void)
{
    gcSystem_VideoModes(&gcSystem_pProgMode, &gcSystem_pIntMode);
    GXRModeObj* progressive = gcSystem_pProgMode;
    GXRModeObj* interlaced = gcSystem_pIntMode;
    if (!progressive) // no component cable
        return interlaced;

    int bHoldB = 0;
    for (int i = 0; i < 20 && !bHoldB; i++) { // ~1/3 s for the pad to report
        VIDEO_WaitVSync();
        bHoldB = (gcInput_ButtonsHeld() & PAD_BUTTON_B) != 0;
    }
    if (bHoldB) {
        gcCard_SetVideoPref(0);
        return interlaced;
    }
    return gcCard_GetVideoPref() == 0 ? interlaced : progressive;
}

// EFB -> XFB copy settings for a video mode.
static void gcSystem_SetupDisplayCopy(GXRModeObj* rmode)
{
    f32 yscale = GX_GetYScaleFactor(rmode->efbHeight, rmode->xfbHeight);
    u32 xfbHeight = GX_SetDispCopyYScale(yscale);
    GX_SetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
    GX_SetDispCopyDst(rmode->fbWidth, xfbHeight);
    GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter);
    GX_SetFieldMode(rmode->field_rendering,
                    (rmode->viHeight == 2 * rmode->xfbHeight) ? GX_ENABLE : GX_DISABLE);
}

int gcSystem_CanProgressive(void)
{
    return gcSystem_pProgMode != NULL;
}

int gcSystem_IsProgressive(void)
{
    return gcSystem_pProgMode && gcSystem_pRmode == gcSystem_pProgMode;
}

void gcSystem_SetProgressive(int bOn)
{
    GXRModeObj* rmode = bOn ? gcSystem_pProgMode : gcSystem_pIntMode;
    if (!rmode || rmode == gcSystem_pRmode) return;

    // Both modes render to the same 640x480 EFB; only the scan-out changes.
    GX_DrawDone();
    VIDEO_SetBlack(true);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    gcSystem_pRmode = rmode;
    VIDEO_Configure(rmode);
    for (int i = 0; i < 2; i++)
        VIDEO_ClearFrameBuffer(rmode, gcSystem_aXfb[i], COLOR_BLACK);
    VIDEO_SetNextFramebuffer(gcSystem_aXfb[gcSystem_curXfb]);
    gcSystem_SetupDisplayCopy(rmode);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE)
        VIDEO_WaitVSync();

    gcCard_SetVideoPref(bOn);
    printf("gcSystem: switched to %s\n", bOn ? "480p" : "interlaced");
}

static lwp_t gcSystem_mainThread = LWP_THREAD_NULL;

// Added: bytes malloc can still hand out (free heap chunks + unused arena).
uint32_t gcSystem_FreeBytes(void)
{
    extern uint32_t gcHeap_FreeBytes(void);
    return gcHeap_FreeBytes();
}

// Added: dlmalloc found the heap damaged (or a bad free): stop with a message
// rather than run on with corrupted memory.
void gcHeap_Corrupt(const char* what, void* p)
{
    gcSystem_Panic("Memory error: %s (%p, from %p).", what, p, __builtin_return_address(0));
}

// Largest block malloc can hand out right now. (It used to be probed with
// malloc+free of decreasing sizes, but the compiler drops an unused malloc/free
// pair, so it always "succeeded" and reported the total free memory instead: the
// "8 MB contiguous" that hid the real fragmentation.)
uint32_t gcSystem_LargestFreeBytes(void)
{
    extern uint32_t gcHeap_LargestFree(void);
    return gcHeap_LargestFree();
}

// Bytes the caches still need to give back so that `incoming` more bytes leave
// GC_MEM_RESERVE free (menu backgrounds, dialogs, level cutscenes, next level).
uint32_t gcSystem_MemShortfall(uint32_t incoming)
{
    uint32_t freeBytes = gcSystem_FreeBytes();
    uint32_t want = GC_MEM_RESERVE + incoming;
    return freeBytes >= want ? 0 : want - freeBytes;
}

int gcSystem_IsMainThread(void)
{
    return LWP_GetSelf() == gcSystem_mainThread;
}

void gcSystem_Init(void)
{
    // Fixed so the background readers (music 50, disc read-ahead 40) rank below it.
    gcSystem_mainThread = LWP_GetSelf();
    LWP_SetThreadPriority(LWP_GetSelf(), GCSYSTEM_MAIN_PRIO);

    gcGame_DetectEarly(); // which game's card files and disc image to use
    VIDEO_Init();
    PAD_Init();

    // Memory cards are unlocked through a DSP task, which can't run once AESND owns
    // the DSP (below): mount the card first. The video mode choice reads it too.
    gcCard_EarlyMount();

    gcSystem_pRmode = gcSystem_ChooseVideoMode();
    GXRModeObj* rmode = gcSystem_pRmode;

    // Big enough for either mode, so the Display menu can switch between them.
    GXRModeObj* bigger = rmode;
    if (gcSystem_pProgMode && gcSystem_pProgMode->xfbHeight > bigger->xfbHeight) bigger = gcSystem_pProgMode;
    if (gcSystem_pIntMode && gcSystem_pIntMode->xfbHeight > bigger->xfbHeight) bigger = gcSystem_pIntMode;
    for (int i = 0; i < 2; i++) {
        gcSystem_aXfb[i] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(bigger));
        VIDEO_ClearFrameBuffer(rmode, gcSystem_aXfb[i], COLOR_BLACK);
    }

    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(gcSystem_aXfb[0]);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE)
        VIDEO_WaitVSync();

    gcSystem_pFifo = memalign(32, GC_FIFO_SIZE);
    memset(gcSystem_pFifo, 0, GC_FIFO_SIZE);
    GX_Init(gcSystem_pFifo, GC_FIFO_SIZE);

    GX_SetCopyClear((GXColor){0, 0, 0, 0xFF}, GX_MAX_Z24);
    GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
    GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
    gcSystem_SetupDisplayCopy(rmode);
    GX_SetPixelFmt(rmode->aa ? GX_PF_RGB565_Z16 : GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetDispCopyGamma(GX_GM_1_0);
    GX_CopyDisp(gcSystem_aXfb[0], GX_TRUE);
    GX_DrawDone();

    gcSystem_StartBootConsole(rmode);
    printf("gcSystem: video %ux%u %s (efb %u, xfb %u)\n",
           rmode->fbWidth, rmode->viHeight,
           (rmode->viTVMode & 3) == VI_PROGRESSIVE ? "progressive" : "interlaced",
           rmode->efbHeight, rmode->xfbHeight);

    // The DSP must be brought up before the ARAM controller is configured (gcDvd's
    // read cache): initializing the DSP later resets ARAM DMA and any transfer in
    // flight never completes. libansnd mixes at 32 kHz (the music's rate).
    printf("gcSystem: starting the DSP (libansnd)\n");
    ansnd_initialize_samplerate(ANSND_OUTPUT_SAMPLERATE_32KHZ);
}

volatile uint64_t gcSystem_statPresentUs = 0;

extern u8 console_font_8x16[];

void gcSystem_DrawText8(uint8_t* pBuf, int pitch, int bufH, int x, int y, const char* text, uint8_t color, uint8_t bg)
{
    for (; *text; text++, x += 8) {
        if (x + 8 > pitch) break;
        const u8* glyph = &console_font_8x16[(u8)*text * 16];
        for (int r = 0; r < 16 && y + r < bufH; r++) {
            uint8_t* row = pBuf + (y + r) * pitch + x;
            u8 bits = glyph[r];
            for (int c = 0; c < 8; c++)
                row[c] = (bits & (0x80 >> c)) ? color : bg;
        }
    }
}

// Boot "Loading..." screen: from power-on to the intro the game loads for
// several seconds with nothing on screen, which looks like a hang. Once the fonts
// can be read, gcNotice_BootLoading renders the text and hands it here; it is
// drawn with GX like any frame (writing the XFB from the CPU shows garbage
// around it in Dolphin) and stays up until the next frame is presented.
void gcSystem_BootOverlay(const uint16_t* pRgb565, int w, int h)
{
    GXRModeObj* rmode = gcSystem_pRmode;
    if (!rmode || gcSystem_bootDone || (w & 3) || (h & 3) || w > rmode->fbWidth || h > rmode->efbHeight) return;

    // RGB565 texture: 4x4 pixel tiles.
    uint16_t* tex = (uint16_t*)memalign(32, w * h * 2);
    if (!tex) return;
    uint16_t* out = tex;
    for (int ty = 0; ty < h; ty += 4)
        for (int tx = 0; tx < w; tx += 4)
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++)
                    *out++ = pRgb565[(ty + y) * w + tx + x];
    DCFlushRange(tex, w * h * 2);
    GX_InvalidateTexAll();

    GXTexObj texObj;
    GX_InitTexObj(&texObj, tex, w, h, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjLOD(&texObj, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
    GX_LoadTexObj(&texObj, GX_TEXMAP0);

    Mtx44 proj;
    Mtx view;
    guOrtho(proj, 0, rmode->efbHeight, 0, rmode->fbWidth, 0.0f, 1.0f);
    GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
    guMtxIdentity(view);
    GX_LoadPosMtxImm(view, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);

    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT7, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT7, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GX_SetNumChans(1);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
    GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetColorUpdate(GX_TRUE);

    float x0 = (float)((rmode->fbWidth - w) / 2);
    float y0 = (float)(rmode->efbHeight * 7 / 8 - h / 2);
    float x1 = x0 + w, y1 = y0 + h;
    GX_Begin(GX_QUADS, GX_VTXFMT7, 4);
    GX_Position3f32(x0, y0, -0.5f); GX_TexCoord2f32(0.0f, 0.0f);
    GX_Position3f32(x1, y0, -0.5f); GX_TexCoord2f32(1.0f, 0.0f);
    GX_Position3f32(x1, y1, -0.5f); GX_TexCoord2f32(1.0f, 1.0f);
    GX_Position3f32(x0, y1, -0.5f); GX_TexCoord2f32(0.0f, 1.0f);
    GX_End();

    // Copy (clearing the EFB back to black) and show it.
    gcSystem_curXfb ^= 1;
    GX_CopyDisp(gcSystem_aXfb[gcSystem_curXfb], GX_TRUE);
    GX_DrawDone();
    VIDEO_SetNextFramebuffer(gcSystem_aXfb[gcSystem_curXfb]);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    free(tex);
}

void gcSystem_BootOverlayEnd(void)
{
    gcSystem_bootDone = 1;
}

void gcSystem_Present(void)
{
    gcSystem_bBootConsole = 0;
    gcSystem_curXfb ^= 1;

    // Z/color writes must be on for the EFB clear that rides along with the copy.
    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_TRUE);
    GX_CopyDisp(gcSystem_aXfb[gcSystem_curXfb], GX_TRUE);
    GX_DrawDone();

    VIDEO_SetNextFramebuffer(gcSystem_aXfb[gcSystem_curXfb]);
    VIDEO_Flush();
    VIDEO_WaitVSync();

}

void gcSystem_Panic(const char* fmt, ...)
{
    char msg[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    SYS_Report("PANIC: %s\n", msg);

    GXRModeObj* rmode = gcSystem_pRmode ? gcSystem_pRmode : VIDEO_GetPreferredMode(NULL);
    void* xfb = gcSystem_aXfb[0];
    if (!xfb) {
        VIDEO_Init();
        xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
        VIDEO_Configure(rmode);
    }
    VIDEO_ClearFrameBuffer(rmode, xfb, COLOR_BLACK);
    CON_Init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(false);
    VIDEO_Flush();

    printf("\n\n  OpenJKDF2 (GameCube) fatal error:\n\n  %s\n", msg);
    while (1) VIDEO_WaitVSync();
}
