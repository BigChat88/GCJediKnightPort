// Short on-screen notices drawn over the menu screen, for work that blocks the
// game for a moment (memory card writes): the frame is presented right away, and
// the pixels underneath are put back by gcNotice_Hide.

#include "gcNotice.h"
#include "gcSystem.h"

#include "Win95/Video.h"
#include "Win95/stdDisplay.h"
#include "Platform/std3D.h"
#include "General/stdFont.h"
#include "General/stdBitmap.h"
#include "Gui/jkGUI.h"
#include "jk.h"

#include <stdlib.h>
#include <string.h>

#define GCNOTICE_FONT  2    // the menus' button font
#define GCNOTICE_PAD   12

extern int jkGame_isDDraw;

static uint8_t* gcNotice_pSaved = NULL;
static int gcNotice_x, gcNotice_y, gcNotice_w, gcNotice_h;

static void gcNotice_Present(void)
{
    if (jkGame_isDDraw) return; // only over menus and loading screens
    std3D_StartScene();
    std3D_DrawMenu();
    std3D_EndScene();
}

static void gcNotice_PaletteExtremes(uint8_t* pBright, uint8_t* pDark)
{
    int bestL = -1, darkL = 1 << 30;
    *pBright = 0;
    *pDark = 0;
    for (int i = 1; i < 256; i++) {
        int l = stdDisplay_masterPalette[i].r * 3 + stdDisplay_masterPalette[i].g * 6 + stdDisplay_masterPalette[i].b;
        if (l > bestL) { bestL = l; *pBright = (uint8_t)i; }
        if (l < darkL) { darkL = l; *pDark = (uint8_t)i; }
    }
}

void gcNotice_Show(const char* text)
{
    tVBuffer* vbuf = Video_pMenuBuffer;
    if (gcNotice_pSaved || !vbuf || !vbuf->surface_lock_alloc) return;

    stdFont* font = jkGui_stdFonts[GCNOTICE_FONT];
    if (!font) return;
    char16_t wtext[64];
    int len = 0;
    for (; text[len] && len < 63; len++) wtext[len] = (unsigned char)text[len];
    wtext[len] = 0;

    int pitch = vbuf->format.rowSize;
    int bufW = vbuf->format.width, bufH = vbuf->format.height;
    gcNotice_w = stdFont_sub_4355F0(font, wtext) + 2 * GCNOTICE_PAD;
    gcNotice_h = stdFont_GetHeight(font) + 2 * GCNOTICE_PAD;
    if (gcNotice_w > bufW) gcNotice_w = bufW;
    gcNotice_x = (bufW - gcNotice_w) / 2;
    gcNotice_y = (bufH - gcNotice_h) / 2;

    gcNotice_pSaved = (uint8_t*)malloc(gcNotice_w * gcNotice_h);
    if (!gcNotice_pSaved) return;

    uint8_t bright, dark;
    gcNotice_PaletteExtremes(&bright, &dark);

    stdDisplay_VBufferLock(vbuf); // marks the menu screen for re-upload
    uint8_t* pix = (uint8_t*)vbuf->surface_lock_alloc;
    for (int r = 0; r < gcNotice_h; r++) {
        uint8_t* row = pix + (gcNotice_y + r) * pitch + gcNotice_x;
        memcpy(gcNotice_pSaved + r * gcNotice_w, row, gcNotice_w);
        int edge = r == 0 || r == gcNotice_h - 1;
        memset(row, edge ? bright : dark, gcNotice_w);
        if (!edge) row[0] = row[gcNotice_w - 1] = bright;
    }
    stdFont_Draw4(vbuf, font, gcNotice_x, gcNotice_y, gcNotice_w, gcNotice_h, 3, wtext, 1);
    stdDisplay_VBufferUnlock(vbuf);
    gcNotice_Present();
}

void gcNotice_Hide(void)
{
    tVBuffer* vbuf = Video_pMenuBuffer;
    if (!gcNotice_pSaved) return;
    if (vbuf && vbuf->surface_lock_alloc) {
        int pitch = vbuf->format.rowSize;
        stdDisplay_VBufferLock(vbuf);
        uint8_t* pix = (uint8_t*)vbuf->surface_lock_alloc;
        for (int r = 0; r < gcNotice_h; r++)
            memcpy(pix + (gcNotice_y + r) * pitch + gcNotice_x, gcNotice_pSaved + r * gcNotice_w, gcNotice_w);
        stdDisplay_VBufferUnlock(vbuf);
        gcNotice_Present();
    }
    free(gcNotice_pSaved);
    gcNotice_pSaved = NULL;
}

