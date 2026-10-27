// std3D backend for the Nintendo GameCube (libogc GX).
//
// Implements the non-SDL2 std3D contract (the `#else` branch of std3D.h), modeled on
// the Dreamcast backend. rdCache hands us screen-space vertices plus nx = (1/z)/32,
// so each vertex is lifted back to view space (x*z, y*z, -z) and drawn with a
// perspective projection that lands it exactly on its screen position. That gives
// GX real depth and perspective-correct texturing without touching the engine.
//
//  - 8-bit world/model textures upload once as CI8 and look up the level colormap
//    through a TLUT (two variants: index 0 transparent for colorkeyed textures,
//    opaque otherwise). Lighting is the modulated vertex color.
//  - The 8-bit menu/HUD buffer is uploaded every frame as a 640x480 CI8 texture
//    with its own TLUT and composited over the scene (index 0 transparent).

#include "Platform/std3D.h"

#include "Main/Main.h"
#include "Win95/Video.h"
#include "Win95/stdDisplay.h"
#include "Engine/rdColormap.h"
#include "Engine/rdroid.h"
#include "Raster/rdCache.h"
#include "World/sithSurface.h"
#include "Platform/wuRegistry.h"
#include <math.h>
#include "World/sithWorld.h"
#include "jk.h"

#include "gcSystem.h"
#include "gcAram.h"
#include "gcHandIcon.h"
#include <ogc/lwp_watchdog.h>
#include <malloc.h>
#include <string.h>

#define STD3D_SCREEN_W 640.0f
#define STD3D_SCREEN_H 480.0f
#define STD3D_ZNEAR    (1.0f / 128.0f)
#define STD3D_ZFAR     (256.0f)



#define TLUT_WORLD_CK     GX_TLUT0
#define TLUT_WORLD_OPAQUE GX_TLUT1
#define TLUT_MENU         GX_TLUT2

int std3D_bReinitHudElements = 0;
static int std3D_bHasInitted = 0;
static int std3D_bGxReady    = 0;
static int std3D_bInScene    = 0;
static int std3D_bMenuPending = 0;

// --- Render list (points straight at rdCache's buffers, like the DC backend) ---
static D3DVERTEX* GL_tmpVertices = NULL;
static size_t     GL_tmpVerticesAmt = 0;
static rdTri*     GL_tmpTris = NULL;
static size_t     GL_tmpTrisAmt = 0;
static void std3D_DrawRenderListReal(void);
static void std3D_ApplyFog(void);
static void std3D_DrawUIRects(void);

// --- Texture cache -----------------------------------------------------------
typedef struct gcTexEntry {
    rdDDrawSurface* surf;
    void*           data;
    GXTexObj        obj;
    int             bCI8;       // world-colormap CI8: TLUT picked per draw
    int             bColorkey;  // loaded as an alpha (colorkeyed) material
    int             bOwnsData;  // data allocated here (always, today)
    uint32_t        bytes;      // size of data (std3D_gcTexBytes)
} gcTexEntry;

// GX copies of the textures live in main RAM next to the engine's own copies.
// Unbounded, the cache grew through a level until other allocations (menu
// backgrounds, new textures) failed: keep it under a budget, evicting the least
// recently drawn textures (they are converted again if they come back).
// World textures are also copies now (their pixels live in ARAM), so the budget is
// the working set of textures in view, not just the extras.
#define STD3D_TEX_BUDGET (3072 * 1024)
uint32_t std3D_gcTexBytes = 0;

// Texture copies are freed once the GPU is done with the frame (see
// std3D_FlushPendingFrees), not mid-frame with a GX_DrawDone stall.
#define STD3D_MAX_PENDING_FREES 256
static void* std3D_aPendingFree[STD3D_MAX_PENDING_FREES];
static int   std3D_numPendingFree = 0;

static void std3D_FlushPendingFrees(int bWaitGpu)
{
    if (!std3D_numPendingFree) return;
    if (bWaitGpu)
        GX_DrawDone();
    for (int i = 0; i < std3D_numPendingFree; i++)
        free(std3D_aPendingFree[i]);
    std3D_numPendingFree = 0;
}

// How a triangle blends, from its rdCache flags (mirrors the GL renderer):
//   0x200 translucent: src*(1-a) + dst*a, a = vertex alpha, no depth write
//   0x400 alpha/colorkey (sprites, blaster flashes): index 0 cut out, rest opaque
// Both cut out palette index 0 regardless of how the texture was loaded.
#define STD3D_BLEND_OPAQUE      0
#define STD3D_BLEND_ALPHA       1
#define STD3D_BLEND_TRANSLUCENT 2
// Added (effects, see rdCache's RDCACHE_GC_FLAG_*): particles are soft round dots;
// fully lit particles and sprites (sparks, explosions, flashes) add their light.
#define STD3D_BLEND_SOFT        3   // src*a + dst*(1-a), no depth write
#define STD3D_BLEND_GLOW        4   // dst + src*a, no depth write
// Added: textured translucent faces (saber blades, force fields, glass). The
// TEV makes alpha = texture alpha * (1 - vertex alpha) and this blends src*a +
// dst*(1-a): the same opacity as STD3D_BLEND_TRANSLUCENT where the texture is
// solid, but its see-through texels stay see-through. The untextured blend,
// src*(1-a) + dst*a with the texture's alpha in it, turned the half-transparent
// edge texels (bilinear filtering) nearly black: a dark rim on the saber's tip.
#define STD3D_BLEND_TRANSLUCENT_TEX 5

static inline int std3D_BlendClass(int flags)
{
    if (flags & RDCACHE_GC_FLAG_GLOW) return STD3D_BLEND_GLOW;
    if (flags & RDCACHE_GC_FLAG_PARTICLE) return STD3D_BLEND_SOFT;
    if (flags & 0x200) return STD3D_BLEND_TRANSLUCENT;
    if (flags & 0x400) return STD3D_BLEND_ALPHA;
    return STD3D_BLEND_OPAQUE;
}
static gcTexEntry std3D_aTex[STD3D_MAX_TEXTURES];
static size_t     std3D_numTex = 0;

static rdColor24 std3D_currentPalette[256];

static uint16_t* std3D_pTlutWorldCK = NULL;
static uint16_t* std3D_pTlutWorldOpaque = NULL;
static uint16_t* std3D_pTlutMenu = NULL;
static void*     std3D_loadedColormap = NULL;

static uint8_t*  std3D_pMenuTex = NULL;
static GXTexObj  std3D_menuTexObj;

// Bound GX state, to avoid redundant state changes per triangle.
static int std3D_boundMode = -1;           // 0 = untextured, 1 = textured
static rdDDrawSurface* std3D_boundTex = NULL;
static int std3D_boundTlut = -1;
static int std3D_boundTevTrans = -1;       // textured TEV: 0 modulate, 1 translucent, -1 unknown
static int std3D_boundBlend = -1;

static size_t std3D_numDeferTris = 0; // translucent tris waiting (std3D_DeferTris)
static void std3D_FlushDeferred(void);

static inline uint16_t std3D_Rgb5a3Opaque(uint8_t r, uint8_t g, uint8_t b)
{
    return 0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
}

static inline int std3D_IsPow2(uint32_t n) { return n && !(n & (n - 1)); }

// --- Texture swizzling ---------------------------------------------------------
// tVBuffer.gap8 value marking a material buffer already tiled in place.
#define STD3D_TILED_MAGIC 0x54494C45u // 'TILE'

// GX textures are stored in tiles: 8x4 texels for 8-bit formats, 4x4 for 16-bit.
// Dimensions are padded up to whole tiles.

static void std3D_SwizzleCI8(uint8_t* dst, const uint8_t* src, uint32_t w, uint32_t h, uint32_t pitch)
{
    // Whole tiles with word-aligned rows (menu/cutscene overlay, most textures):
    // each tile row is 8 bytes, so move it as two words.
    if (!(w & 7) && !(h & 3) && !(pitch & 3) && !((uintptr_t)src & 3) && !((uintptr_t)dst & 3)) {
        uint32_t* d = (uint32_t*)dst;
        for (uint32_t ty = 0; ty < h; ty += 4) {
            const uint8_t* rows = src + ty * pitch;
            for (uint32_t tx = 0; tx < w; tx += 8) {
                const uint32_t* r0 = (const uint32_t*)(rows + tx);
                const uint32_t* r1 = (const uint32_t*)(rows + pitch + tx);
                const uint32_t* r2 = (const uint32_t*)(rows + 2 * pitch + tx);
                const uint32_t* r3 = (const uint32_t*)(rows + 3 * pitch + tx);
                d[0] = r0[0]; d[1] = r0[1];
                d[2] = r1[0]; d[3] = r1[1];
                d[4] = r2[0]; d[5] = r2[1];
                d[6] = r3[0]; d[7] = r3[1];
                d += 8;
            }
        }
        return;
    }

    uint32_t pw = (w + 7) & ~7u, ph = (h + 3) & ~3u;
    for (uint32_t ty = 0; ty < ph; ty += 4) {
        for (uint32_t tx = 0; tx < pw; tx += 8) {
            for (uint32_t y = ty; y < ty + 4; y++) {
                for (uint32_t x = tx; x < tx + 8; x++) {
                    *dst++ = (x < w && y < h) ? src[y * pitch + x] : 0;
                }
            }
        }
    }
}

static void std3D_Swizzle16(uint16_t* dst, const uint16_t* src, uint32_t w, uint32_t h, uint32_t pitch)
{
    uint32_t pw = (w + 3) & ~3u, ph = (h + 3) & ~3u;
    for (uint32_t ty = 0; ty < ph; ty += 4) {
        for (uint32_t tx = 0; tx < pw; tx += 4) {
            for (uint32_t y = ty; y < ty + 4; y++) {
                for (uint32_t x = tx; x < tx + 4; x++) {
                    *dst++ = (x < w && y < h) ? src[y * pitch + x] : 0;
                }
            }
        }
    }
}

// --- ARAM-backed material textures -----------------------------------------------
//
// A level's material pixels used to stay in main RAM for the whole level (3-5 MiB,
// ~a third of what a level needs). Now the material loader hands whole-tile 8-bit
// textures (nearly all of them) to std3D_GcLoadPixelsToAram: they are read in
// pieces, tiled for GX and stored in the ARAM texture pool (gcAram.c). The texture
// cache below copies them back into main RAM when they are drawn and evicts the
// least recently drawn ones, so only the textures in view take main RAM.

// Whether a texture of this format can live in ARAM (CI8 whole tiles).
int std3D_GcCanUseAram(const tRasterInfo* fmt)
{
    uint32_t w = fmt->width, h = fmt->height;
    return !fmt->format.is16bit && fmt->format.bpp != 16 && w && h
        && !(w & 7) && !(h & 3) && w <= 1024 && h <= 1024;
}

// Reads the texture's pixels from the file straight into the ARAM pool. Returns 0
// (nothing read) when the pool is full: the caller then loads it into main RAM.
int std3D_GcLoadPixelsToAram(tVBuffer* vbuf, stdFile_t f, size_t (*fileRead)(stdFile_t, void*, size_t))
{
    // Pieces of whole 4-row bands (a band stays in the same bytes when tiled).
    static uint8_t stage[32 * 1024] __attribute__((aligned(32)));
    static uint8_t strip[1024 * 4] __attribute__((aligned(32)));
    uint32_t w = vbuf->format.width, h = vbuf->format.height;
    uint32_t size = w * h;

    uint32_t aram = gcAram_Alloc(size);
    if (!aram) return 0;

    uint32_t rowsPerPiece = (sizeof(stage) / (w * 4)) * 4;
    for (uint32_t y = 0; y < h; y += rowsPerPiece) {
        uint32_t rows = h - y < rowsPerPiece ? h - y : rowsPerPiece;
        uint32_t len = rows * w;
        uint32_t got = 0;
        while (got < len) {
            size_t r = fileRead(f, stage + got, len - got);
            if (!r) break;
            got += (uint32_t)r;
        }
        if (got < len) memset(stage + got, 0, len - got); // short read: black, not garbage
        for (uint32_t b = 0; b < rows; b += 4) {
            memcpy(strip, stage + b * w, w * 4);
            std3D_SwizzleCI8(stage + b * w, strip, w, 4, w);
        }
        gcAram_Write(aram + y * w, stage, len); // len: rows%4==0, w%8==0 -> multiple of 32
    }
    vbuf->gcAram = aram;
    vbuf->gap8 = STD3D_TILED_MAGIC;
    return 1;
}

// --- GX state helpers ----------------------------------------------------------

static void std3D_LoadSceneProjection(void)
{
    // Vertices arrive as (sx*z, sy*z, -z); this matrix maps them back to sx, sy in a
    // 640x480 screen and keeps GX's standard [near,far] -> [-1,0] depth mapping.
    Mtx44 p;
    memset(p, 0, sizeof(p));
    p[0][0] = 2.0f / STD3D_SCREEN_W;
    p[0][2] = 1.0f;
    p[1][1] = -2.0f / STD3D_SCREEN_H;
    p[1][2] = -1.0f;
    p[2][2] = -STD3D_ZNEAR / (STD3D_ZFAR - STD3D_ZNEAR);
    p[2][3] = -(STD3D_ZFAR * STD3D_ZNEAR) / (STD3D_ZFAR - STD3D_ZNEAR);
    p[3][2] = -1.0f;
    GX_LoadProjectionMtx(p, GX_PERSPECTIVE);
}

static void std3D_LoadOrthoProjection(void)
{
    Mtx44 p;
    guOrtho(p, 0.0f, STD3D_SCREEN_H, 0.0f, STD3D_SCREEN_W, 0.0f, 1.0f);
    GX_LoadProjectionMtx(p, GX_ORTHOGRAPHIC);
}

static void std3D_SetVtxFormat(void)
{
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
}

static void std3D_BindUntextured(void)
{
    if (std3D_boundMode == 0) return;
    GX_SetNumTexGens(0);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    std3D_boundTevTrans = -1;
    std3D_boundMode = 0;
    std3D_boundTex = NULL;
}

// tlut: GX_TLUTx to (re)point a CI8 texture at, or -1 for non-paletted textures.
static void std3D_BindTexture(GXTexObj* obj, rdDDrawSurface* key, int tlut)
{
    if (std3D_boundMode != 1) {
        GX_SetNumTexGens(1);
        GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
        GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
        std3D_boundTevTrans = 0;
        std3D_boundMode = 1;
        std3D_boundTex = NULL;
    }
    if (!key || key != std3D_boundTex || tlut != std3D_boundTlut) {
        if (tlut >= 0)
            GX_InitTexObjTlut(obj, tlut);
        GX_LoadTexObj(obj, GX_TEXMAP0);
        std3D_boundTex = key;
        std3D_boundTlut = tlut;
    }
}

// Textured faces' TEV: plain modulate, or for translucent faces alpha = texture
// alpha * (1 - vertex alpha) (see STD3D_BLEND_TRANSLUCENT_TEX).
static void std3D_SetTexTev(int bTrans)
{
    if (bTrans == std3D_boundTevTrans) return;
    if (bTrans) {
        GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
        GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_TEXA, GX_CA_ZERO, GX_CA_RASA, GX_CA_ZERO);
        GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    }
    else
        GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    std3D_boundTevTrans = bTrans;
}

static void std3D_SetBlend(int blendClass)
{
    if (std3D_boundBlend == blendClass) return;
    switch (blendClass)
    {
        case STD3D_BLEND_TRANSLUCENT:
            GX_SetBlendMode(GX_BM_BLEND, GX_BL_INVSRCALPHA, GX_BL_SRCALPHA, GX_LO_CLEAR);
            GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
            break;
        case STD3D_BLEND_ALPHA:
            GX_SetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
            GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
            break;
        case STD3D_BLEND_SOFT:
            GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
            GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
            break;
        case STD3D_BLEND_TRANSLUCENT_TEX:
            GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
            GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
            break;
        case STD3D_BLEND_GLOW:
            GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR);
            GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
            break;
        default:
            GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
            GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
            break;
    }
    std3D_boundBlend = blendClass;
}

// Added: the sky is drawn at the far end of the depth range, whatever the depth
// of its faces (its texture is projected on the screen anyway). It's then the
// background: the fog sees it at one fixed distance. Only things in front of the sky are drawn over it,
// as before.
static int std3D_bSkyDepthBound = 0;

static void std3D_SetSkyDepth(int bSky)
{
    if (bSky == std3D_bSkyDepthBound) return;
    GX_SetViewport(0, 0, gcSystem_pRmode->fbWidth, gcSystem_pRmode->efbHeight, bSky ? 1 : 0, 1);
    std3D_bSkyDepthBound = bSky;
}

static void std3D_SetSceneState(void)
{
    GX_SetViewport(0, 0, gcSystem_pRmode->fbWidth, gcSystem_pRmode->efbHeight, 0, 1);
    std3D_bSkyDepthBound = 0;
    GX_SetScissor(0, 0, gcSystem_pRmode->fbWidth, gcSystem_pRmode->efbHeight);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetNumTevStages(1);
    // Colorkeyed texels (alpha 0) are discarded; depth is tested after texturing so
    // the discarded texels don't write Z.
    GX_SetAlphaCompare(GX_GREATER, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetZCompLoc(GX_FALSE);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_FALSE);

    Mtx ident;
    guMtxIdentity(ident);
    GX_LoadPosMtxImm(ident, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);

    std3D_SetVtxFormat();
    std3D_LoadSceneProjection();

    std3D_boundMode = -1;
    std3D_boundTex = NULL;
    std3D_boundBlend = -1;
    std3D_BindUntextured();
    std3D_SetBlend(0);
}

// --- Palettes ----------------------------------------------------------------

static void std3D_UploadTlut(uint16_t* pTlut, uint32_t tlutName)
{
    GXTlutObj obj;
    DCFlushRange(pTlut, 256 * sizeof(uint16_t));
    GX_InitTlutObj(&obj, pTlut, GX_TL_RGB5A3, 256);
    GX_LoadTlut(&obj, tlutName);
}

// Refresh the world TLUTs if the level colormap changed. Compared by content, not
// just by pointer: the next level's colormap can be allocated at the same address.
// The colormap 8-bit textures are drawn through: the level's, or in the menus the
// one the multiplayer character preview loads (as the PC renderer does).
extern int jkGuiBuildMulti_bRendering;
static rdColormap* std3D_GcColormap(void)
{
    if (sithWorld_g_pCurrentWorld && sithWorld_g_pCurrentWorld->colormaps)
        return sithWorld_g_pCurrentWorld->colormaps;
    if (jkGuiBuildMulti_bRendering && rdColormap_pCurMap)
        return rdColormap_pCurMap;
    return NULL;
}

static void std3D_UpdateWorldTluts(void)
{
    rdColormap* pCmp = std3D_GcColormap();
    if (!pCmp) return;
    static rdColor24 loadedColors[256];
    if ((void*)pCmp == std3D_loadedColormap && !memcmp(loadedColors, pCmp->colors, sizeof(loadedColors)))
        return;
    std3D_loadedColormap = (void*)pCmp;
    memcpy(loadedColors, pCmp->colors, sizeof(loadedColors));

    for (int i = 0; i < 256; i++) {
        rdColor24* c = &pCmp->colors[i];
        uint16_t v = std3D_Rgb5a3Opaque(c->r, c->g, c->b);
        std3D_pTlutWorldCK[i] = (i == 0) ? 0x0000 : v;
        std3D_pTlutWorldOpaque[i] = v;
    }
    std3D_UploadTlut(std3D_pTlutWorldCK, TLUT_WORLD_CK);
    std3D_UploadTlut(std3D_pTlutWorldOpaque, TLUT_WORLD_OPAQUE);
}

// --- Lifecycle -----------------------------------------------------------------

// Added: the particles' texture, a 32x32 dot fading out towards its edge (IA8,
// 2 KiB). Intensity and alpha hold the same value.
#define STD3D_DOT_SIZE 32
static GXTexObj  std3D_dotTexObj;
static uint16_t* std3D_pDotTex = NULL;

static void std3D_MakeDotTexture(void)
{
    static uint16_t lin[STD3D_DOT_SIZE * STD3D_DOT_SIZE];
    const float half = STD3D_DOT_SIZE * 0.5f;
    for (int y = 0; y < STD3D_DOT_SIZE; y++) {
        for (int x = 0; x < STD3D_DOT_SIZE; x++) {
            float dx = (x + 0.5f - half) / half, dy = (y + 0.5f - half) / half;
            float r = sqrtf(dx * dx + dy * dy);
            // Solid core, smooth falloff over the outer half.
            float t = r <= 0.45f ? 1.0f : (r >= 1.0f ? 0.0f : 1.0f - (r - 0.45f) / 0.55f);
            t = t * t * (3.0f - 2.0f * t);
            uint16_t v = (uint16_t)(t * 255.0f + 0.5f);
            lin[y * STD3D_DOT_SIZE + x] = (uint16_t)((v << 8) | v);
        }
    }
    std3D_pDotTex = (uint16_t*)memalign(32, sizeof(lin));
    if (!std3D_pDotTex) return;
    std3D_Swizzle16(std3D_pDotTex, lin, STD3D_DOT_SIZE, STD3D_DOT_SIZE, STD3D_DOT_SIZE);
    DCFlushRange(std3D_pDotTex, sizeof(lin));
    GX_InitTexObj(&std3D_dotTexObj, std3D_pDotTex, STD3D_DOT_SIZE, STD3D_DOT_SIZE, GX_TF_IA8,
                  GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&std3D_dotTexObj, GX_LINEAR, GX_LINEAR);
}

static void std3D_GxInit(void)
{
    if (std3D_bGxReady) return;

    std3D_pTlutWorldCK     = (uint16_t*)memalign(32, 256 * sizeof(uint16_t));
    std3D_pTlutWorldOpaque = (uint16_t*)memalign(32, 256 * sizeof(uint16_t));
    std3D_pTlutMenu        = (uint16_t*)memalign(32, 256 * sizeof(uint16_t));
    std3D_pMenuTex         = (uint8_t*)memalign(32, 640 * 480);
    if (!std3D_pTlutWorldCK || !std3D_pTlutWorldOpaque || !std3D_pTlutMenu || !std3D_pMenuTex)
        gcSystem_Panic("std3D: out of memory for GX buffers");

    memset(std3D_pMenuTex, 0, 640 * 480);
    DCFlushRange(std3D_pMenuTex, 640 * 480);
    GX_InitTexObjCI(&std3D_menuTexObj, std3D_pMenuTex, 640, 480, GX_TF_CI8,
                    GX_CLAMP, GX_CLAMP, GX_FALSE, TLUT_MENU);
    GX_InitTexObjFilterMode(&std3D_menuTexObj, GX_NEAR, GX_NEAR);
    std3D_MakeDotTexture();

    std3D_bGxReady = 1;
}

int std3D_Startup()
{
    if (Main_bHeadless) { std3D_bHasInitted = 1; return 1; }
    std3D_GxInit();
    std3D_bHasInitted = 1;
    return 1;
}

void std3D_Shutdown()
{
    // Soft shutdown: the engine cycles Startup/Shutdown around GUI transitions; GX
    // and its resources stay alive for the whole session.
    std3D_bHasInitted = 0;
}

int std3D_IsReady()        { return std3D_bHasInitted; }
void std3D_FreeResources() {}
void std3D_UpdateSettings() {}

// The first-person weapon is drawn after the depth is reset (std3D_ClearZBuffer):
// everything but the sky is set to this depth, just short of the far end, so the
// weapon is drawn in front of the world (walls no longer cut through it).
#define STD3D_POV_CLEAR_DEPTH ((float)0xFEFEFE / (float)0xFFFFFF)

int std3D_StartScene()
{
    if (Main_bHeadless) return 0;

    // The material LRU gates its per-frame load budget on this counter.
    ++std3D_frameCount;

    if (!std3D_bHasInitted) std3D_Startup();
    if (std3D_bInScene) std3D_EndScene();

    std3D_UpdateWorldTluts();
    std3D_SetSceneState();
    std3D_ApplyFog();
    std3D_bInScene = 1;
    return 0;
}

// --- Enlarged HUD ------------------------------------------------------------------
// The status panels (health/shield, ammo/force/battery) and the item bar are drawn
// into the 640x480 overlay at their original size, which reads small on a TV.
// jkHud registers their rectangles every frame; on upload each one is copied into
// its own small texture and cleared from the overlay, then drawn STD3D_HUD_SCALE
// times larger, anchored to its screen edge. The 3D view keeps its resolution.
// The pickup/message lines at the top are enlarged the same way, from the top.
#define STD3D_HUD_SCALE   1.5f
#define STD3D_HUD_TEX_W   640
#define STD3D_HUD_TEX_H   128

typedef struct std3DHudRegion {
    int      x, y, w, h;        // in the overlay
    int      anchorX, anchorY;  // screen point the region scales away from
    f32      scale;
    uint32_t frame;             // std3D_frameCount when last registered
    int      bUploaded;         // cut out of the overlay at its last upload
    uint8_t* pTex;
    uint32_t texCap;            // bytes allocated at pTex
    GXTexObj obj;
} std3DHudRegion;
static std3DHudRegion std3D_aHud[STD3D_HUD_REGIONS];

void std3D_GcSetHudRegionEx(int i, int x, int y, int w, int h, int anchorX, int anchorY, float scale)
{
    if (i < 0 || i >= STD3D_HUD_REGIONS) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > 640) w = 640 - x;
    if (y + h > 480) h = 480 - y;
    if (w > STD3D_HUD_TEX_W) w = STD3D_HUD_TEX_W;
    if (h > STD3D_HUD_TEX_H) h = STD3D_HUD_TEX_H;
    std3DHudRegion* r = &std3D_aHud[i];
    if (r->x != x || r->y != y || r->w != w || r->h != h)
        stdDisplay_gcMenuDirty = 1;
    r->x = x; r->y = y; r->w = w; r->h = h;
    r->anchorX = anchorX;
    r->anchorY = anchorY;
    r->scale = scale;
    r->frame = std3D_frameCount;
}

void std3D_GcSetHudRegion(int i, int x, int y, int w, int h, int anchorX)
{
    std3D_GcSetHudRegionEx(i, x, y, w, h, anchorX, 480, STD3D_HUD_SCALE);
}

// Called on overlay upload, after the overlay was tiled into std3D_pMenuTex.
static void std3D_CutHudRegions(const uint8_t* src, uint32_t pitch)
{
    for (int i = 0; i < STD3D_HUD_REGIONS; i++)
    {
        std3DHudRegion* r = &std3D_aHud[i];
        r->bUploaded = 0;
        if (r->frame != std3D_frameCount || r->w <= 0 || r->h <= 0) continue;
        uint32_t pw = (r->w + 7) & ~7u, ph = (r->h + 3) & ~3u;
        if (r->texCap < pw * ph) {
            free(r->pTex);
            r->pTex = (uint8_t*)memalign(32, pw * ph);
            r->texCap = r->pTex ? pw * ph : 0;
            if (!r->pTex) continue;
        }
        std3D_SwizzleCI8(r->pTex, src + r->y * pitch + r->x, r->w, r->h, pitch);
        DCFlushRange(r->pTex, pw * ph);
        GX_InitTexObjCI(&r->obj, r->pTex, pw, ph, GX_TF_CI8, GX_CLAMP, GX_CLAMP, GX_FALSE, TLUT_MENU);
        GX_InitTexObjFilterMode(&r->obj, GX_LINEAR, GX_LINEAR);

        // Clear it from the overlay (index 0 = transparent). The overlay is tiled:
        // 8x4 texel tiles, 640 / 8 = 80 tiles per row.
        for (int y = r->y; y < r->y + r->h; y++)
            for (int x = r->x; x < r->x + r->w; x++)
                std3D_pMenuTex[((y >> 2) * 80 + (x >> 3)) * 32 + (y & 3) * 8 + (x & 7)] = 0;
        for (int ty = r->y >> 2; ty <= (r->y + r->h - 1) >> 2; ty++) {
            int tx0 = r->x >> 3, tx1 = (r->x + r->w + 7) >> 3;
            DCFlushRange(std3D_pMenuTex + (ty * 80 + tx0) * 32, (tx1 - tx0) * 32);
        }
        r->bUploaded = 1;
    }
}

static void std3D_DrawHudRegions(void)
{
    for (int i = 0; i < STD3D_HUD_REGIONS; i++)
    {
        std3DHudRegion* r = &std3D_aHud[i];
        if (!r->bUploaded) continue;
        const f32 s = r->scale, ax = (f32)r->anchorX, ay = (f32)r->anchorY;
        f32 x0 = ax + (r->x - ax) * s, y0 = ay + (r->y - ay) * s;
        f32 x1 = ax + (r->x + r->w - ax) * s, y1 = ay + (r->y + r->h - ay) * s;
        f32 u1 = (f32)r->w / ((r->w + 7) & ~7), v1 = (f32)r->h / ((r->h + 3) & ~3);
        std3D_BindTexture(&r->obj, NULL, -1);
        GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
            GX_Position3f32(x0, y0, 0.0f); GX_Color1u32(0xFFFFFFFF); GX_TexCoord2f32(0.0f, 0.0f);
            GX_Position3f32(x1, y0, 0.0f); GX_Color1u32(0xFFFFFFFF); GX_TexCoord2f32(u1, 0.0f);
            GX_Position3f32(x1, y1, 0.0f); GX_Color1u32(0xFFFFFFFF); GX_TexCoord2f32(u1, v1);
            GX_Position3f32(x0, y1, 0.0f); GX_Color1u32(0xFFFFFFFF); GX_TexCoord2f32(0.0f, v1);
        GX_End();
    }
}

// Added: the "add" part of the screen color effects (rdroid_curColorEffects, set
// each frame from stdPalEffects): Force Blinding whitens the view (Yun's attack in
// level 7: add 180/260/230), pickups flash. The PC renderer adds it to every 3D
// pixel in its shader; here an additive full-screen quad over the 3D scene, under
// the HUD. (Tint, filter and fade are applied per vertex by rdCache already.)
static int std3D_bDrew3D = 0; // a 3D render list was drawn in this scene

static void std3D_DrawColorEffectAdd(void)
{
    int r = rdroid_curColorEffects.add.x, g = rdroid_curColorEffects.add.y, b = rdroid_curColorEffects.add.z;
    if (r <= 0 && g <= 0 && b <= 0) return;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    u32 c = ((u32)r << 24) | ((u32)g << 16) | ((u32)b << 8) | 0xFF;

    std3D_LoadOrthoProjection();
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR); // dst + color, saturating
    std3D_boundBlend = -1;
    std3D_BindUntextured();
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
        GX_Position3f32(0.0f, 0.0f, 0.0f);     GX_Color1u32(c); GX_TexCoord2f32(0.0f, 0.0f);
        GX_Position3f32(640.0f, 0.0f, 0.0f);   GX_Color1u32(c); GX_TexCoord2f32(0.0f, 0.0f);
        GX_Position3f32(640.0f, 480.0f, 0.0f); GX_Color1u32(c); GX_TexCoord2f32(0.0f, 0.0f);
        GX_Position3f32(0.0f, 480.0f, 0.0f);   GX_Color1u32(c); GX_TexCoord2f32(0.0f, 0.0f);
    GX_End();
    std3D_LoadSceneProjection();
}

// Added: the "use" hand above the health display while the B button would
// activate something (a switch, console, door...), as the console Jedi Knight II
// games show it. Set for each frame by jkHud_Draw; drawn over the HUD.
int std3D_gcShowHand = 0;
static GXTexObj std3D_handTexObj;
static int std3D_bHandTexInit = 0;

#define STD3D_HAND_DRAW_SIZE 32.0f

extern int jkGuiBuildMulti_bRendering;

// --- Display options (Setup > Display), kept in the settings ---------------------
// Distance fog (on by default; the game never had fog, so it can be turned off),
// and the action hand icon (on by default).
static int std3D_gcbFog = -1, std3D_gcbHand = -1; // -1: not read from the settings yet

int std3D_GcGetFog(void)
{
    if (std3D_gcbFog < 0) std3D_gcbFog = wuRegistry_GetBool("gcFog", 1) ? 1 : 0;
    return std3D_gcbFog;
}

void std3D_GcSetFog(int b)
{
    std3D_gcbFog = b ? 1 : 0;
    wuRegistry_SaveBool("gcFog", std3D_gcbFog);
}

int std3D_GcGetHandIcon(void)
{
    if (std3D_gcbHand < 0) std3D_gcbHand = wuRegistry_GetBool("gcHandIcon", 1) ? 1 : 0;
    return std3D_gcbHand;
}

void std3D_GcSetHandIcon(int b)
{
    std3D_gcbHand = b ? 1 : 0;
    wuRegistry_SaveBool("gcHandIcon", std3D_gcbHand);
}

// Linear fog for the 3D scene, in the colour of the level's sky (dark grey
// without one). JK units: the player is ~0.18 tall, so it starts ~25 m away and
// is at its most at ~180 m.
//
// Done in the TEV, not with the hardware fog: that one always ends at 100%,
// which turned far mountains and walls into flat silhouettes against a textured
// sky. Here it stops at STD3D_FOG_MAX, and the sky gets that same amount, so
// far things and the sky blend alike. The amount is worked out per vertex (from
// its depth, in std3D_EmitVertex) and goes in the alpha of a second vertex
// colour; a second TEV stage mixes towards the fog colour by it (additive
// things fade towards black instead, so they don't add grey).
#define STD3D_FOG_START 2.5f
#define STD3D_FOG_END   18.0f
#define STD3D_FOG_MAX   0.6f
#define STD3D_FOG_OFF   0
#define STD3D_FOG_WORLD 1
#define STD3D_FOG_SKY   2
static int     std3D_bSceneFog = 0;   // fog on for this scene
static int     std3D_bFogBound = -1;  // what the TEV has now (-1 unknown)
static int     std3D_vtxFogMode = STD3D_FOG_OFF; // what std3D_EmitVertex writes
static GXColor std3D_fogColor;

// Per batch: world, sky (the fixed amount) or none.
static void std3D_SetFogMode(int mode, int bAdditive)
{
    if (!std3D_bSceneFog) mode = STD3D_FOG_OFF;
    std3D_vtxFogMode = mode;
    int key = mode == STD3D_FOG_OFF ? 0 : (bAdditive ? 2 : 1);
    if (key == std3D_bFogBound) return;
    if (!key) {
        GX_SetNumTevStages(1);
    }
    else {
        GX_SetNumTevStages(2);
        GX_SetTevKColor(GX_KCOLOR3, std3D_fogColor);
        GX_SetTevKColor(GX_KCOLOR2, (GXColor){ 0, 0, 0, 0xFF });
        GX_SetTevKColorSel(GX_TEVSTAGE1, key == 2 ? GX_TEV_KCSEL_K2 : GX_TEV_KCSEL_K3);
        GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR1A1);
        GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_CPREV, GX_CC_KONST, GX_CC_RASA, GX_CC_ZERO);
        GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
        GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    }
    std3D_bFogBound = key;
}

static void std3D_SetFogEnabled(int bOn)
{
    std3D_SetFogMode(bOn ? STD3D_FOG_WORLD : STD3D_FOG_OFF, 0);
}

// The fog amount for a vertex at view depth z (0-255).
static inline uint32_t std3D_VtxFog(float z)
{
    if (std3D_vtxFogMode == STD3D_FOG_SKY)
        return (uint32_t)(STD3D_FOG_MAX * 255.0f);
    if (std3D_vtxFogMode != STD3D_FOG_WORLD || z <= STD3D_FOG_START)
        return 0;
    if (z >= STD3D_FOG_END)
        return (uint32_t)(STD3D_FOG_MAX * 255.0f);
    return (uint32_t)((z - STD3D_FOG_START) * (STD3D_FOG_MAX * 255.0f / (STD3D_FOG_END - STD3D_FOG_START)));
}

// The fog's colour: the average of the sky texture as drawn (the horizon sky's
// if the level has one, else the ceiling's), worked out the first time it's
// drawn in a level. The material's "solid colour" used before often isn't what
// the sky looks like (grey fog under a blue sky, or the other of two skies).
static int     std3D_skyFogKind = 0;     // 0 none yet, 1 ceiling sky, 2 horizon sky
static rdColor24 std3D_skyFogAvg;

void std3D_GcResetSkyFog(void)
{
    std3D_skyFogKind = 0;
}

static void std3D_TrackSkyFog(gcTexEntry* e, int bHorizon)
{
    int kind = bHorizon ? 2 : 1;
    if (kind <= std3D_skyFogKind || !e->bCI8) return; // the first one of a kind
    rdColormap* pCmp = std3D_GcColormap();
    if (!pCmp) return;
    uint32_t n = (uint32_t)GX_GetTexObjWidth(&e->obj) * GX_GetTexObjHeight(&e->obj);
    if (!n) return;
    const uint8_t* p = (const uint8_t*)e->data;
    uint32_t r = 0, g = 0, b = 0;
    for (uint32_t i = 0; i < n; i++) {
        rdColor24 c = pCmp->colors[p[i]];
        r += c.r; g += c.g; b += c.b;
    }
    std3D_skyFogAvg.r = (uint8_t)(r / n);
    std3D_skyFogAvg.g = (uint8_t)(g / n);
    std3D_skyFogAvg.b = (uint8_t)(b / n);
    std3D_skyFogKind = kind;
}

static void std3D_ApplyFog(void)
{
    GXColor c = { 0x30, 0x30, 0x34, 0xFF };
    int bOn = std3D_GcGetFog() && jkGame_isDDraw && sithWorld_g_pCurrentWorld && !jkGuiBuildMulti_bRendering;
    if (bOn && std3D_skyFogKind) {
        c.r = std3D_skyFogAvg.r; c.g = std3D_skyFogAvg.g; c.b = std3D_skyFogAvg.b;
    }
    else if (bOn && sithSurface_gcbSkyColorKnown && sithWorld_g_pCurrentWorld->colormaps) {
        rdColor24 sky = sithWorld_g_pCurrentWorld->colormaps->colors[sithSurface_skyColorGuess];
        c.r = sky.r; c.g = sky.g; c.b = sky.b;
    }
    std3D_fogColor = c;
    std3D_bSceneFog = bOn;
    std3D_bFogBound = -1;
    GX_SetFog(GX_FOG_NONE, 0.0f, 1.0f, 0.1f, 1.0f, c); // see std3D_SetFogMode
}

static void std3D_FogOff(void)
{
    std3D_SetFogEnabled(0);
    std3D_bSceneFog = 0;
}

// EFB region -> texture (half: 2x2 box filtered).
static void std3D_EfbCopy(int x, int y, int w, int h, u8 fmt, int bHalf, void* pDst)
{
    GX_SetTexCopySrc(x, y, w, h);
    GX_SetTexCopyDst(bHalf ? w / 2 : w, bHalf ? h / 2 : h, fmt, bHalf ? GX_TRUE : GX_FALSE);
    GX_CopyTex(pDst, GX_FALSE);
    GX_PixModeSync();
    GX_InvalidateTexAll();
}

// --- Underwater ripple ---------------------------------------------------------------
// While the camera is in an underwater sector the 3D view wobbles: the scene is
// copied out of the EFB at half size (soft, which suits being under water) and
// drawn back over the screen on a grid whose texture coordinates sway with time.
// 150 KiB, allocated the first time it's needed while the option is on (default).
#define STD3D_WATER_W     320
#define STD3D_WATER_H     240
#define STD3D_WATER_COLS  16
#define STD3D_WATER_ROWS  12
#define STD3D_WATER_AMP   0.006f   // sway, in texture units (~2 px on screen)
static int      std3D_gcbWater = -1;
static int      std3D_bUnderwater = 0;
static void*    std3D_pWater = NULL;
static GXTexObj std3D_waterObj;

int std3D_GcGetWater(void)
{
    if (std3D_gcbWater < 0) std3D_gcbWater = wuRegistry_GetBool("gcWater", 1) ? 1 : 0;
    return std3D_gcbWater;
}

void std3D_GcSetWater(int b)
{
    std3D_gcbWater = b ? 1 : 0;
    wuRegistry_SaveBool("gcWater", std3D_gcbWater);
    if (!std3D_gcbWater && std3D_pWater) {
        GX_DrawDone(); // the GPU may still read it
        free(std3D_pWater);
        std3D_pWater = NULL;
    }
}

// From sithRender, every frame: is the camera under water?
void std3D_GcSetUnderwater(int b)
{
    std3D_bUnderwater = b;
}

static void std3D_DrawUnderwater(void)
{
    if (!std3D_bUnderwater || !std3D_GcGetWater() || jkGuiBuildMulti_bRendering) return;
    if (!std3D_pWater) {
        std3D_pWater = memalign(32, STD3D_WATER_W * STD3D_WATER_H * 2);
        if (!std3D_pWater) return;
        GX_InitTexObj(&std3D_waterObj, std3D_pWater, STD3D_WATER_W, STD3D_WATER_H, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
        GX_InitTexObjFilterMode(&std3D_waterObj, GX_LINEAR, GX_LINEAR);
    }
    int W = gcSystem_pRmode->fbWidth, H = gcSystem_pRmode->efbHeight;
    std3D_EfbCopy(0, 0, W, H, GX_TF_RGB565, 1, std3D_pWater);

    std3D_LoadOrthoProjection();
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_LoadTexObj(&std3D_waterObj, GX_TEXMAP0);

    // Two slow waves across each other; the edges stay put (no smear from clamping).
    float t = (float)(stdPlatform_GetTimeMsec() % 100000u) * 0.001f;
    static float aU[STD3D_WATER_ROWS + 1][STD3D_WATER_COLS + 1];
    static float aV[STD3D_WATER_ROWS + 1][STD3D_WATER_COLS + 1];
    for (int r = 0; r <= STD3D_WATER_ROWS; r++)
        for (int c = 0; c <= STD3D_WATER_COLS; c++) {
            float u = (float)c / STD3D_WATER_COLS, v = (float)r / STD3D_WATER_ROWS;
            int bEdge = !r || !c || r == STD3D_WATER_ROWS || c == STD3D_WATER_COLS;
            aU[r][c] = u + (bEdge ? 0.0f : STD3D_WATER_AMP * sinf(v * 12.0f + t * 2.1f));
            aV[r][c] = v + (bEdge ? 0.0f : STD3D_WATER_AMP * cosf(u * 9.0f + t * 1.7f));
        }
    const float cw = (float)STD3D_SCREEN_W / STD3D_WATER_COLS, ch = (float)STD3D_SCREEN_H / STD3D_WATER_ROWS;
    GX_Begin(GX_QUADS, GX_VTXFMT0, STD3D_WATER_ROWS * STD3D_WATER_COLS * 4);
    for (int r = 0; r < STD3D_WATER_ROWS; r++)
        for (int c = 0; c < STD3D_WATER_COLS; c++) {
            float x0 = c * cw, x1 = (c + 1) * cw, y0 = r * ch, y1 = (r + 1) * ch;
            GX_Position3f32(x0, y0, 0.0f); GX_Color1u32(0xFFFFFFFF); GX_TexCoord2f32(aU[r][c],         aV[r][c]);
            GX_Position3f32(x1, y0, 0.0f); GX_Color1u32(0xFFFFFFFF); GX_TexCoord2f32(aU[r][c + 1],     aV[r][c + 1]);
            GX_Position3f32(x1, y1, 0.0f); GX_Color1u32(0xFFFFFFFF); GX_TexCoord2f32(aU[r + 1][c + 1], aV[r + 1][c + 1]);
            GX_Position3f32(x0, y1, 0.0f); GX_Color1u32(0xFFFFFFFF); GX_TexCoord2f32(aU[r + 1][c],     aV[r + 1][c]);
        }
    GX_End();

    std3D_SetSceneState();
}

static void std3D_DrawHandIcon(void)
{
    if (!std3D_bHandTexInit) {
        DCFlushRange((void*)gcHandIcon_rgb5a3, sizeof(gcHandIcon_rgb5a3));
        GX_InitTexObj(&std3D_handTexObj, (void*)gcHandIcon_rgb5a3, GC_HAND_ICON_SIZE, GC_HAND_ICON_SIZE,
                      GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
        GX_InitTexObjLOD(&std3D_handTexObj, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
        std3D_bHandTexInit = 1;
    }
    // Centred above the health display as drawn on the TV: HUD region 0, enlarged
    // from its anchor (see std3D_DrawHudRegions).
    std3DHudRegion* r = &std3D_aHud[0];
    float cx = 70.0f, top = 380.0f;
    if (r->w && r->h) {
        cx = r->anchorX + (r->x + r->w * 0.5f - r->anchorX) * r->scale;
        top = r->anchorY + (r->y - r->anchorY) * r->scale;
    }
    // Nudged a little left and up from centred, as the user asked.
    float x0 = cx - STD3D_HAND_DRAW_SIZE / 2 - 6.0f, y0 = top - STD3D_HAND_DRAW_SIZE - 8.0f;
    float x1 = x0 + STD3D_HAND_DRAW_SIZE, y1 = y0 + STD3D_HAND_DRAW_SIZE;

    std3D_LoadOrthoProjection();
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    std3D_boundBlend = -1;
    std3D_BindTexture(&std3D_handTexObj, NULL, -1);
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
        GX_Position3f32(x0, y0, 0.0f); GX_Color1u32(0xFFFFFFFF); GX_TexCoord2f32(0.0f, 0.0f);
        GX_Position3f32(x1, y0, 0.0f); GX_Color1u32(0xFFFFFFFF); GX_TexCoord2f32(1.0f, 0.0f);
        GX_Position3f32(x1, y1, 0.0f); GX_Color1u32(0xFFFFFFFF); GX_TexCoord2f32(1.0f, 1.0f);
        GX_Position3f32(x0, y1, 0.0f); GX_Color1u32(0xFFFFFFFF); GX_TexCoord2f32(0.0f, 1.0f);
    GX_End();
    std3D_LoadSceneProjection();
}

static void std3D_SubmitMenuOverlay(void)
{
    std3D_LoadOrthoProjection();
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    std3D_boundBlend = -1;
    std3D_BindTexture(&std3D_menuTexObj, NULL, -1);

    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
        GX_Position3f32(0.0f, 0.0f, 0.0f);     GX_Color1u32(0xFFFFFFFF); GX_TexCoord2f32(0.0f, 0.0f);
        GX_Position3f32(640.0f, 0.0f, 0.0f);   GX_Color1u32(0xFFFFFFFF); GX_TexCoord2f32(1.0f, 0.0f);
        GX_Position3f32(640.0f, 480.0f, 0.0f); GX_Color1u32(0xFFFFFFFF); GX_TexCoord2f32(1.0f, 1.0f);
        GX_Position3f32(0.0f, 480.0f, 0.0f);   GX_Color1u32(0xFFFFFFFF); GX_TexCoord2f32(0.0f, 1.0f);
    GX_End();
    std3D_DrawHudRegions();
}

// Added: close a scene without presenting it. The in-game map screen calls
// rdAdvanceFrame (which opens a scene) but draws in software; the menu flip's
// own StartScene then ended that empty scene by presenting it: a black frame
// every time the map moved.
void std3D_GcDropScene(void)
{
    std3D_numDeferTris = 0;
    std3D_bInScene = 0;
}

int std3D_EndScene()
{
    if (Main_bHeadless || !std3D_bInScene) return 0;
    std3D_FlushDeferred(); // translucent things, over everything opaque
    std3D_FogOff(); // the overlays below are not part of the scene
    // Added: the overlays (HUD, crosshair) from a known GX state, not what the
    // scene's last batch left: in third person, once the sky came into view the
    // whole HUD stopped showing (drawn, but not on screen) until it went away.
    std3D_SetSceneState();
    std3D_bFogBound = -1;

    if (std3D_bDrew3D && jkGame_isDDraw) {
        std3D_DrawUnderwater();
        std3D_DrawColorEffectAdd();
        std3D_bDrew3D = 0;
    }
    std3D_DrawUIRects(); // the crosshair (std3D_DrawUIClearedRect)
    if (std3D_bMenuPending) {
        std3D_SubmitMenuOverlay();
        std3D_bMenuPending = 0;
    }
    if (std3D_gcShowHand) {
        if (jkGame_isDDraw && std3D_GcGetHandIcon())
            std3D_DrawHandIcon();
        std3D_gcShowHand = 0;
    }

    gcSystem_Present();
    std3D_FlushPendingFrees(0); // Present waited for the GPU (GX_DrawDone)
    std3D_bInScene = 0;
    return 0;
}

// --- Render list ---------------------------------------------------------------

void std3D_ResetRenderList()
{
    GL_tmpVerticesAmt = 0;
    GL_tmpTrisAmt = 0;
}

int std3D_RenderListVerticesFinish() { return 0; }

int std3D_AddRenderListVertices(D3DVERTEX* aVertices, int count)
{
    if (Main_bHeadless) return 1;
    GL_tmpVertices    = aVertices;
    GL_tmpVerticesAmt = (size_t)count;
    return 1;
}

void std3D_AddRenderListTris(rdTri* tris, unsigned int num_tris)
{
    if (Main_bHeadless) return;
    GL_tmpTris    = tris;
    GL_tmpTrisAmt = num_tris;
    std3D_DrawRenderListReal();
}

void std3D_AddRenderListLines(rdLine* lines, uint32_t num_lines) {}

// Added: the multiplayer character preview (jkGUIBuildMulti) renders through a
// 259x259 canvas at the screen's origin; the menu shows it in a box at (315, 115).
extern int jkGuiBuildMulti_bRendering;
static float std3D_offsetX = 0.0f, std3D_offsetY = 0.0f;

static inline void std3D_EmitVertex(const D3DVERTEX* v, int keepAlpha)
{
    // nx = (1/z)/32 (rdCache.c); rebuild view-space depth from it.
    float invz = v->nx * 32.0f;
    float z = (invz > 1.0f / STD3D_ZFAR) ? (1.0f / invz) : (STD3D_ZFAR * 0.999f);
    if (z < STD3D_ZNEAR) z = STD3D_ZNEAR;

    uint32_t argb = v->color;
    uint32_t rgba = (argb << 8) | (keepAlpha ? (argb >> 24) : 0xFF);

    GX_Position3f32((v->x + std3D_offsetX) * z, (v->y + std3D_offsetY) * z, -z);
    GX_Color1u32(rgba);
    GX_Color1u32(0xFFFFFF00u | std3D_VtxFog(z)); // fog amount (std3D_SetFogMode)
    GX_TexCoord2f32(v->tu, v->tv);
}

static void std3D_EmitTris(rdTri* tris, D3DVERTEX* verts, size_t j, size_t end, int keepAlpha)
{
    GX_Begin(GX_TRIANGLES, GX_VTXFMT0, (uint16_t)((end - j) * 3));
    for (size_t k = j; k < end; k++) {
        std3D_EmitVertex(&verts[tris[k].v1], keepAlpha);
        std3D_EmitVertex(&verts[tris[k].v2], keepAlpha);
        std3D_EmitVertex(&verts[tris[k].v3], keepAlpha);
    }
    GX_End();
}

// Added: translucent things (explosions, particles, bolts, saber blades) don't
// write depth, so whatever opaque was drawn after them painted over them: a
// character behind an explosion showed whole, in front of it. The game flushes
// its face cache several times a frame (the character can come in a later
// flush), so they're kept here and drawn after everything opaque: before the
// depth is reset for the first-person weapon, and at the end of the scene.
#define STD3D_MAX_DEFERRED_TRIS 1536
static rdTri*     std3D_aDeferTris = NULL;
static D3DVERTEX* std3D_aDeferVerts = NULL;

static void std3D_DrawTriList(rdTri* tris, D3DVERTEX* verts, size_t numTris, int bDefer);

static int std3D_DeferTris(rdTri* tris, D3DVERTEX* verts, size_t j, size_t end)
{
    if (!std3D_aDeferTris) {
        std3D_aDeferTris = (rdTri*)malloc(STD3D_MAX_DEFERRED_TRIS * sizeof(rdTri));
        std3D_aDeferVerts = (D3DVERTEX*)malloc(STD3D_MAX_DEFERRED_TRIS * 3 * sizeof(D3DVERTEX));
        if (!std3D_aDeferTris || !std3D_aDeferVerts) {
            free(std3D_aDeferTris); free(std3D_aDeferVerts);
            std3D_aDeferTris = NULL; std3D_aDeferVerts = NULL;
            return 0;
        }
    }
    if (std3D_numDeferTris + (end - j) > STD3D_MAX_DEFERRED_TRIS)
        return 0; // full: drawn now, as before
    for (size_t k = j; k < end; k++) {
        size_t n = std3D_numDeferTris++;
        rdTri* t = &std3D_aDeferTris[n];
        *t = tris[k];
        std3D_aDeferVerts[n * 3 + 0] = verts[tris[k].v1];
        std3D_aDeferVerts[n * 3 + 1] = verts[tris[k].v2];
        std3D_aDeferVerts[n * 3 + 2] = verts[tris[k].v3];
        t->v1 = (int32_t)(n * 3); t->v2 = (int32_t)(n * 3 + 1); t->v3 = (int32_t)(n * 3 + 2);
    }
    // Kept in the texture cache until drawn. Only a texture that is in it (as the
    // draw loop checks): putting any other on the cache list corrupted the list.
    rdDDrawSurface* tex = tris[j].texture;
    if (tex && tex->texture_loaded && (size_t)tex->texture_id < std3D_numTex && std3D_aTex[tex->texture_id].data)
        std3D_UpdateFrameCount(tex);
    return 1;
}

static void std3D_FlushDeferred(void)
{
    if (!std3D_numDeferTris) return;
    size_t n = std3D_numDeferTris;
    std3D_numDeferTris = 0;
    if (std3D_bInScene && !Main_bHeadless) {
        std3D_LoadSceneProjection();
        std3D_DrawTriList(std3D_aDeferTris, std3D_aDeferVerts, n, 0);
    }
}

static void std3D_DrawRenderListReal(void)
{
    if (Main_bHeadless || !std3D_bInScene || !GL_tmpTrisAmt || !GL_tmpVertices)
        return;
    std3D_DrawTriList(GL_tmpTris, GL_tmpVertices, GL_tmpTrisAmt, !jkGuiBuildMulti_bRendering);
}

static void std3D_DrawTriList(rdTri* tris, D3DVERTEX* verts, size_t numTris, int bDefer)
{
    std3D_bDrew3D = 1;
    std3D_offsetX = jkGuiBuildMulti_bRendering ? 315.0f : 0.0f;
    std3D_offsetY = jkGuiBuildMulti_bRendering ? 115.0f : 0.0f;

    // The scene's vertices carry a second colour, the fog amount (only here: the
    // overlays drawn with the same format don't send it).
    GX_SetVtxDesc(GX_VA_CLR1, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR1, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetNumChans(2);
    GX_SetChanCtrl(GX_COLOR1A1, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    std3D_bFogBound = -1; // the TEV stages may have been changed since (underwater)

    size_t j = 0;
    while (j < numTris)
    {
        // Batch consecutive tris sharing a texture and blend mode into one GX_Begin.
        rdDDrawSurface* tex = tris[j].texture;
        int isTex = tex && tex->texture_loaded && (size_t)tex->texture_id < std3D_numTex
                    && std3D_aTex[tex->texture_id].data;
        int blendClass = std3D_BlendClass(tris[j].flags);

        int bParticle = !isTex && (tris[j].flags & RDCACHE_GC_FLAG_PARTICLE) && std3D_pDotTex;

        size_t end = j + 1;
        while (end < numTris
               && tris[end].texture == tris[j].texture
               && std3D_BlendClass(tris[end].flags) == blendClass
               && ((tris[end].flags ^ tris[j].flags) & (RDCACHE_GC_FLAG_PARTICLE | RDCACHE_GC_FLAG_NOFOG | RDCACHE_GC_FLAG_HORIZON)) == 0)
            end++;
        int bSky = (tris[j].flags & RDCACHE_GC_FLAG_NOFOG) != 0;
        if (bDefer && !bSky && blendClass != STD3D_BLEND_OPAQUE && blendClass != STD3D_BLEND_ALPHA
            && std3D_DeferTris(tris, verts, j, end)) {
            j = end;
            continue;
        }
        std3D_SetFogMode(bSky ? STD3D_FOG_SKY : STD3D_FOG_WORLD,
                         blendClass == STD3D_BLEND_GLOW || blendClass == STD3D_BLEND_SOFT);
        std3D_SetSkyDepth(bSky);

        if (isTex) {
            gcTexEntry* e = &std3D_aTex[tex->texture_id];
            if (bSky)
                std3D_TrackSkyFog(e, (tris[j].flags & RDCACHE_GC_FLAG_HORIZON) != 0);
            int tlut = -1;
            if (e->bCI8)
                tlut = (blendClass != STD3D_BLEND_OPAQUE || e->bColorkey) ? TLUT_WORLD_CK : TLUT_WORLD_OPAQUE;
            std3D_BindTexture(&e->obj, tex, tlut);
            std3D_UpdateFrameCount(tex);
            if (blendClass == STD3D_BLEND_TRANSLUCENT)
                blendClass = STD3D_BLEND_TRANSLUCENT_TEX;
            std3D_SetTexTev(blendClass == STD3D_BLEND_TRANSLUCENT_TEX);
        }
        else if (bParticle) {
            std3D_BindTexture(&std3D_dotTexObj, (rdDDrawSurface*)&std3D_dotTexObj, -1);
            std3D_SetTexTev(0);
        }
        else {
            std3D_BindUntextured();
        }
        std3D_SetBlend(blendClass);

        int keepAlpha = blendClass != STD3D_BLEND_OPAQUE;
        std3D_EmitTris(tris, verts, j, end, keepAlpha);

        j = end;
    }
    std3D_SetSkyDepth(0);
    std3D_SetFogEnabled(0);
    GX_SetVtxDesc(GX_VA_CLR1, GX_NONE);
    GX_SetNumChans(1);
}

void std3D_DrawRenderList()
{
    std3D_ResetRenderList();
}

// --- Palette / dimensions ------------------------------------------------------

int std3D_SetCurrentPalette(rdColor24* a1, int a2)
{
    if (a1) memcpy(std3D_currentPalette, a1, sizeof(std3D_currentPalette));
    return 1;
}

void std3D_GetValidDimension(unsigned int inW, unsigned int inH, unsigned int* outW, unsigned int* outH)
{
    if (outW) *outW = inW;
    if (outH) *outH = inH;
}
int std3D_GetValidDimensions(int a1, int a2, int a3, int a4) { return 1; }

// --- Texture cache -------------------------------------------------------------

void std3D_PurgeSurfaceRefs(rdDDrawSurface* texture);

static size_t std3D_FindFreeEntry(void)
{
    for (size_t i = 0; i < std3D_numTex; i++) {
        if (!std3D_aTex[i].surf && !std3D_aTex[i].data) return i;
    }
    return std3D_numTex;
}

// Free the least recently drawn texture (the cache list is kept oldest first),
// never one drawn this frame.
static int std3D_EvictOldest(void)
{
    rdDDrawSurface* pTex = std3D_pFirstTexCache;
    if (!pTex || pTex->frameNum == std3D_frameCount) return 0;
    std3D_PurgeSurfaceRefs(pTex);
    return 1;
}

static void std3D_EnforceTexBudget(uint32_t incoming)
{
    // Also shrink below the budget when the heap runs low (GC_MEM_RESERVE), but
    // keep a minimum working set so the current view doesn't thrash.
    uint32_t limit = STD3D_TEX_BUDGET;
    uint32_t shortfall = gcSystem_MemShortfall(incoming);
    if (shortfall) {
        uint32_t floor = 512 * 1024;
        limit = std3D_gcTexBytes > shortfall + floor ? std3D_gcTexBytes - shortfall : floor;
        if (limit > STD3D_TEX_BUDGET) limit = STD3D_TEX_BUDGET;
    }
    while (std3D_gcTexBytes + incoming > limit && std3D_EvictOldest())
        ;
}

// The heap ran dry (stdPlatform's allocator calls this before giving up): drop
// every texture copy not drawn this frame, and free them now (waits for the GPU).
void std3D_GcReleaseMemory(void)
{
    while (std3D_EvictOldest())
        ;
    std3D_FlushPendingFrees(1);
}

static int std3D_AddToTextureCacheReal(tVBuffer* vbuf, rdDDrawSurface* texture, int is_alpha_tex, int no_alpha);

int std3D_AddToTextureCache(tVBuffer* vbuf, rdDDrawSurface* texture, int is_alpha_tex, int no_alpha)
{
    return std3D_AddToTextureCacheReal(vbuf, texture, is_alpha_tex, no_alpha);
}

static int std3D_AddToTextureCacheReal(tVBuffer* vbuf, rdDDrawSurface* texture, int is_alpha_tex, int no_alpha)
{
    if (Main_bHeadless) return 1;
    if (!vbuf || !texture || (!vbuf->surface_lock_alloc && !vbuf->gcAram)) return 1;
    if (texture->texture_loaded) return 1;

    uint32_t w = vbuf->format.width;
    uint32_t h = vbuf->format.height;
    if (!w || !h || w > 1024 || h > 1024) return 1;

    std3D_GxInit();
    std3D_UpdateWorldTluts();

    size_t idx = std3D_FindFreeEntry();
    if (idx == std3D_numTex && std3D_numTex >= STD3D_MAX_TEXTURES) {
        // All slots taken (big levels use more materials than that): evict the
        // least recently drawn texture instead of leaving this one untextured.
        if (!std3D_EvictOldest()) return 1;
        idx = std3D_FindFreeEntry();
        if (idx == std3D_numTex) return 1;
    }
    if (idx == std3D_numTex)
        std3D_numTex++;
    gcTexEntry* e = &std3D_aTex[idx];

    uint8_t wrap = (std3D_IsPow2(w) && std3D_IsPow2(h)) ? GX_REPEAT : GX_CLAMP;
    int hasColormap = std3D_GcColormap() != NULL;
    uint32_t bytes;

    if (vbuf->gcAram)
    {
        // Pixels in ARAM, already tiled: copy them into a cache entry.
        if (vbuf->palette || !hasColormap) { if (idx == std3D_numTex - 1) std3D_numTex--; return 1; }
        bytes = w * h;
        std3D_EnforceTexBudget(bytes);
        e->data = memalign(32, bytes);
        if (!e->data) { std3D_PurgeTextureCache(bytes); std3D_FlushPendingFrees(1); e->data = memalign(32, bytes); }
        if (!e->data) { if (idx == std3D_numTex - 1) std3D_numTex--; return 1; }
        gcAram_Read(e->data, vbuf->gcAram, bytes);
        e->bOwnsData = 1;
        e->bCI8 = 1;
        e->bColorkey = is_alpha_tex && !no_alpha;
        GX_InitTexObjCI(&e->obj, e->data, w, h, GX_TF_CI8, wrap, wrap, GX_FALSE,
                        e->bColorkey ? TLUT_WORLD_CK : TLUT_WORLD_OPAQUE);
        e->bytes = bytes;
        std3D_gcTexBytes += bytes;
        goto added; // the DMA wrote memory directly: nothing to flush
    }

    if (!vbuf->format.format.is16bit && !vbuf->palette && hasColormap)
    {
        // CI8 through the world colormap TLUT.
        bytes = ((w + 7) & ~7u) * ((h + 3) & ~3u);
        uint8_t* src = (uint8_t*)vbuf->surface_lock_alloc;

        // Added: whole-tile textures (nearly all of them) are tiled in place and
        // sampled straight from the material's own buffer: no second copy per
        // texture (that cache was up to 2 MiB, allocated mid-level, and when the
        // fragmented heap refused one, things like the level 1 TIE fighter drew
        // without their texture or not at all). Nothing reads material texels on
        // the CPU here (no software rasterizer). The buffer is marked so a re-add
        // after a cache purge doesn't tile it twice, and std3D never frees it (see
        // std3D_UnloadAllTextures: an earlier attempt corrupted the weapon after
        // the pause menu because that freed the material's own buffer).
        if (!(w & 7) && !(h & 3) && vbuf->format.rowSize == w && w <= 1024
            && !((uintptr_t)src & 31))
        {
            if (vbuf->gap8 != STD3D_TILED_MAGIC) {
                static uint8_t strip[1024 * 4] __attribute__((aligned(32)));
                // In CI8 tiling a band of 4 rows stays in the same bytes.
                for (uint32_t y = 0; y < h; y += 4) {
                    memcpy(strip, src + y * w, w * 4);
                    std3D_SwizzleCI8(src + y * w, strip, w, 4, w);
                }
                DCFlushRange(src, w * h);
                vbuf->gap8 = STD3D_TILED_MAGIC;
            }
            e->data = src;
            e->bytes = 0;
            e->bOwnsData = 0;
            e->bCI8 = 1;
            e->bColorkey = is_alpha_tex && !no_alpha;
            GX_InitTexObjCI(&e->obj, e->data, w, h, GX_TF_CI8, wrap, wrap, GX_FALSE,
                            e->bColorkey ? TLUT_WORLD_CK : TLUT_WORLD_OPAQUE);
            goto added;
        }

        std3D_EnforceTexBudget(bytes);
        e->data = memalign(32, bytes);
        if (!e->data) { std3D_PurgeTextureCache(bytes); std3D_FlushPendingFrees(1); e->data = memalign(32, bytes); }
        if (!e->data) { if (idx == std3D_numTex - 1) std3D_numTex--; return 1; }
        std3D_SwizzleCI8((uint8_t*)e->data, src, w, h, vbuf->format.rowSize);
        e->bOwnsData = 1;
        e->bCI8 = 1;
        e->bColorkey = is_alpha_tex && !no_alpha;
        GX_InitTexObjCI(&e->obj, e->data, w, h, GX_TF_CI8, wrap, wrap, GX_FALSE,
                        e->bColorkey ? TLUT_WORLD_CK : TLUT_WORLD_OPAQUE);
    }
    else
    {
        // 16-bit texels: native 565/1555 surfaces, or 8-bit expanded through an
        // explicit palette (bitmaps carrying their own palette, no world loaded).
        uint16_t* tmp = (uint16_t*)malloc(w * h * sizeof(uint16_t));
        if (!tmp) { if (idx == std3D_numTex - 1) std3D_numTex--; return 1; }
        int fmt;

        if (vbuf->format.format.is16bit)
        {
            const uint16_t* src16 = (const uint16_t*)vbuf->surface_lock_alloc;
            uint32_t rowpx = vbuf->format.rowSize / 2;
            // Engine 16-bit surfaces are RGB565, or ARGB1555 for alpha textures.
            int is565 = !is_alpha_tex;
            for (uint32_t y = 0; y < h; y++) {
                for (uint32_t x = 0; x < w; x++) {
                    uint16_t p = src16[y * rowpx + x];
                    if (is565) tmp[y * w + x] = p;
                    else tmp[y * w + x] = (p & 0x8000) || !is_alpha_tex ? (0x8000 | (p & 0x7FFF)) : 0x0000;
                }
            }
            fmt = is565 ? GX_TF_RGB565 : GX_TF_RGB5A3;
        }
        else
        {
            const uint8_t* src8 = (const uint8_t*)vbuf->surface_lock_alloc;
            const uint8_t* pal = (const uint8_t*)vbuf->palette;
            const rdColor24* cmp = hasColormap ? std3D_GcColormap()->colors : std3D_currentPalette;
            for (uint32_t y = 0; y < h; y++) {
                for (uint32_t x = 0; x < w; x++) {
                    uint8_t i = src8[y * vbuf->format.rowSize + x];
                    uint8_t r, g, b;
                    if (pal) { r = pal[i*3+0]; g = pal[i*3+1]; b = pal[i*3+2]; }
                    else     { r = cmp[i].r;   g = cmp[i].g;   b = cmp[i].b; }
                    tmp[y * w + x] = (is_alpha_tex && !no_alpha && i == 0) ? 0x0000 : std3D_Rgb5a3Opaque(r, g, b);
                }
            }
            fmt = GX_TF_RGB5A3;
        }

        bytes = ((w + 3) & ~3u) * ((h + 3) & ~3u) * 2;
        std3D_EnforceTexBudget(bytes);
        e->data = memalign(32, bytes);
        if (!e->data) { std3D_PurgeTextureCache(bytes); std3D_FlushPendingFrees(1); e->data = memalign(32, bytes); }
        if (!e->data) { free(tmp); if (idx == std3D_numTex - 1) std3D_numTex--; return 1; }
        std3D_Swizzle16((uint16_t*)e->data, tmp, w, h, w);
        free(tmp);
        e->bOwnsData = 1;
        e->bCI8 = 0;
        e->bColorkey = 0;
        GX_InitTexObj(&e->obj, e->data, w, h, fmt, wrap, wrap, GX_FALSE);
    }

    e->bytes = bytes;
    std3D_gcTexBytes += bytes;
    DCFlushRange(e->data, bytes);
added:
    GX_InitTexObjFilterMode(&e->obj, GX_LINEAR, GX_LINEAR);
    e->surf = texture;

    texture->texture_id     = (int)idx;
    texture->texture_loaded = 1;
    texture->is_16bit       = vbuf->format.format.is16bit ? 1 : 0;
    texture->textureSize    = bytes;
    std3D_AddTextureToCacheList(texture);
    texture->frameNum       = std3D_frameCount;
    return 1;
}

static void std3D_FreeEntry(gcTexEntry* e)
{
    if (e->data && e->bOwnsData) {
        // The GPU may still be sampling it from this frame's command stream.
        if (std3D_numPendingFree == STD3D_MAX_PENDING_FREES)
            std3D_FlushPendingFrees(1);
        std3D_aPendingFree[std3D_numPendingFree++] = e->data;
        std3D_gcTexBytes -= e->bytes;
    }
    e->bytes = 0;
    e->data = NULL;
    e->surf = NULL;
    e->bOwnsData = 0;
}

void std3D_UnloadAllTextures()
{
    GX_DrawDone();
    for (size_t i = 0; i < std3D_numTex; i++)
    {
        if (std3D_aTex[i].surf) {
            std3D_aTex[i].surf->texture_loaded = 0;
            std3D_aTex[i].surf->pNextCachedTexture = NULL;
            std3D_aTex[i].surf->pPrevCachedTexture = NULL;
        }
        // Only our own copies: textures sampled in place belong to their material
        // (freeing those let jkHud_Open's allocations overwrite the weapon after
        // the pause menu, and the material later freed them a second time).
        if (std3D_aTex[i].data && std3D_aTex[i].bOwnsData) free(std3D_aTex[i].data);
        std3D_aTex[i].data = NULL;
        std3D_aTex[i].bOwnsData = 0;
        std3D_aTex[i].surf = NULL;
        std3D_aTex[i].bytes = 0;
    }
    std3D_gcTexBytes = 0;
    std3D_numTex = 0;
    std3D_pFirstTexCache = NULL;
    std3D_pLastTexCache  = NULL;
    std3D_numCachedTextures = 0;
    std3D_boundTex = NULL;
    GX_InvalidateTexAll();
}

void std3D_PurgeEntireTextureCache()
{
    std3D_UnloadAllTextures();
    // A reloaded level often gets the same colormap address; force a TLUT refresh.
    std3D_loadedColormap = NULL;
}

// --- LRU cache list (same as the DC/original std3D) ------------------------------

void std3D_RemoveTextureFromCacheList(rdDDrawSurface* pCacheTexture)
{
    if (!pCacheTexture) return;

    if (pCacheTexture == std3D_pFirstTexCache)
    {
        std3D_pFirstTexCache = pCacheTexture->pNextCachedTexture;
        if (std3D_pFirstTexCache)
        {
            std3D_pFirstTexCache->pPrevCachedTexture = NULL;
            if (!std3D_pFirstTexCache->pNextCachedTexture)
                std3D_pLastTexCache = std3D_pFirstTexCache;
        }
        else
            std3D_pLastTexCache = NULL;
    }
    else if (pCacheTexture == std3D_pLastTexCache)
    {
        std3D_pLastTexCache = pCacheTexture->pPrevCachedTexture;
        if (pCacheTexture->pPrevCachedTexture)
            pCacheTexture->pPrevCachedTexture->pNextCachedTexture = NULL;
        else
            std3D_pLastTexCache = std3D_pFirstTexCache;
    }
    else
    {
        if (pCacheTexture->pPrevCachedTexture)
            pCacheTexture->pPrevCachedTexture->pNextCachedTexture = pCacheTexture->pNextCachedTexture;
        if (pCacheTexture->pNextCachedTexture)
            pCacheTexture->pNextCachedTexture->pPrevCachedTexture = pCacheTexture->pPrevCachedTexture;
    }

    pCacheTexture->pNextCachedTexture = NULL;
    pCacheTexture->pPrevCachedTexture = NULL;
    pCacheTexture->frameNum = 0;
    --std3D_numCachedTextures;
}

void std3D_AddTextureToCacheList(rdDDrawSurface* pTexture)
{
    if (!pTexture) return;

    if (std3D_pFirstTexCache)
    {
        std3D_pLastTexCache->pNextCachedTexture = pTexture;
        pTexture->pPrevCachedTexture            = std3D_pLastTexCache;
        pTexture->pNextCachedTexture            = NULL;
        std3D_pLastTexCache                     = pTexture;
    }
    else
    {
        std3D_pLastTexCache          = pTexture;
        std3D_pFirstTexCache         = pTexture;
        pTexture->pPrevCachedTexture = NULL;
        pTexture->pNextCachedTexture = NULL;
    }
    ++std3D_numCachedTextures;
}

void std3D_UpdateFrameCount(rdDDrawSurface* pTexture)
{
    if (!pTexture || pTexture->frameNum == std3D_frameCount) return;
    std3D_RemoveTextureFromCacheList(pTexture);
    std3D_AddTextureToCacheList(pTexture);
    pTexture->frameNum = std3D_frameCount;
}

void std3D_PurgeSurfaceRefs(rdDDrawSurface* texture)
{
    if (!texture) return;
    for (size_t i = 0; i < std3D_numTex; i++) {
        if (std3D_aTex[i].surf == texture)
            std3D_FreeEntry(&std3D_aTex[i]);
    }
    if (std3D_boundTex == texture) std3D_boundTex = NULL;
    std3D_RemoveTextureFromCacheList(texture);
    texture->texture_loaded = 0;
}

void std3D_PurgeTextureEntry(int i)
{
    if (i < 0 || (size_t)i >= std3D_numTex) return;
    if (std3D_aTex[i].surf) std3D_PurgeSurfaceRefs(std3D_aTex[i].surf);
}

// Evict cached textures (oldest first, never this frame's) to free `size` bytes.
int std3D_PurgeTextureCache(size_t size)
{
    size_t purgedBytes = 0;

    for (rdDDrawSurface* pTex = std3D_pFirstTexCache;
         pTex && pTex->frameNum != std3D_frameCount;
         pTex = pTex->pNextCachedTexture)
    {
        if (pTex->textureSize == size) {
            std3D_PurgeSurfaceRefs(pTex);
            return 1;
        }
    }

    rdDDrawSurface* pNext = NULL;
    for (rdDDrawSurface* pTex = std3D_pFirstTexCache; pTex && purgedBytes < size; pTex = pNext)
    {
        pNext = pTex->pNextCachedTexture;
        if (pTex->frameNum == std3D_frameCount)
            continue;
        purgedBytes += pTex->textureSize;
        std3D_PurgeSurfaceRefs(pTex);
    }
    return purgedBytes != 0;
}

void std3D_PurgeUIEntry(int i, int idx)        {}
void std3D_PurgeBitmapRefs(stdBitmap* pBitmap) {}

// --- Z buffer / viewport / device ------------------------------------------------

// Clear depth mid-frame (first-person weapon, etc.) by drawing a far-plane quad
// with color writes off.
int std3D_ClearZBuffer()
{
    if (!std3D_bInScene) return 1;
    std3D_FlushDeferred(); // the world's translucent things, with the world's depth

    std3D_LoadOrthoProjection();
    GX_SetColorUpdate(GX_FALSE);
    // Not to the far end: to STD3D_POV_CLEAR_DEPTH, everywhere nearer than it
    // (GREATER: the sky, at the far end, keeps its depth) -- see there.
    GX_SetZMode(GX_TRUE, GX_GREATER, GX_TRUE);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    std3D_BindUntextured();

    // Ortho maps z in [0,-1] -> [near, far] (depth = -z). Opaque alpha: the
    // alpha test (GREATER 0) runs before the depth write and dropped the whole
    // quad with alpha 0, so the depth was never cleared and walls cut through
    // the first-person weapon (the long bowcaster most visibly).
    const float z = -STD3D_POV_CLEAR_DEPTH;
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
        GX_Position3f32(0.0f, 0.0f, z);     GX_Color1u32(0x000000FF); GX_TexCoord2f32(0, 0);
        GX_Position3f32(640.0f, 0.0f, z);   GX_Color1u32(0x000000FF); GX_TexCoord2f32(0, 0);
        GX_Position3f32(640.0f, 480.0f, z); GX_Color1u32(0x000000FF); GX_TexCoord2f32(0, 0);
        GX_Position3f32(0.0f, 480.0f, z);   GX_Color1u32(0x000000FF); GX_TexCoord2f32(0, 0);
    GX_End();

    GX_SetColorUpdate(GX_TRUE);
    std3D_LoadSceneProjection();
    std3D_boundBlend = -1;
    std3D_SetBlend(0);
    return 1;
}

void std3D_InitializeViewport(rdRect* viewRect) {}
int  std3D_DrawOverlay()                  { return 1; }
int  std3D_FindClosestDevice(uint32_t index, int a2) { return 0; }
int  std3D_SetRenderList(intptr_t a1)     { return 0; }
intptr_t std3D_GetRenderList()            { return 0; }
int  std3D_CreateExecuteBuffer()          { return 1; }
void std3D_DrawSceneFbo()                 {}
void std3D_Screenshot(const char* pFpath) {}

// Colorkey and modulated-alpha support (see the DC backend for what rdCache does
// with these).
int std3D_HasAlpha()             { return 1; }
int std3D_HasModulateAlpha()     { return 1; }
int std3D_HasAlphaFlatStippled() { return 1; }

// --- Menu / HUD overlay ------------------------------------------------------------

void std3D_DrawMenu()
{
    if (Main_bHeadless) return;
    if (!Video_menuBuffer.surface_lock_alloc) return;

    std3D_GxInit();

    int w = Video_menuBuffer.format.width;
    int h = Video_menuBuffer.format.height;
    if (w <= 0 || h <= 0) return;
    if (w > 640) w = 640;
    if (h > 480) h = 480;

    // Rebuild the menu TLUT only when the master palette changes.
    static rdColor24 menuPalCache[256];
    static int menuPalCacheValid = 0;
    if (!menuPalCacheValid || memcmp(menuPalCache, stdDisplay_masterPalette, sizeof(menuPalCache)))
    {
        memcpy(menuPalCache, stdDisplay_masterPalette, sizeof(menuPalCache));
        menuPalCacheValid = 1;
        std3D_pTlutMenu[0] = 0x0000; // index 0 -> transparent (world shows through the HUD)
        for (int i = 1; i < 256; i++) {
            const rdColor24* c = &stdDisplay_masterPalette[i];
            std3D_pTlutMenu[i] = std3D_Rgb5a3Opaque(c->r, c->g, c->b);
        }
        std3D_UploadTlut(std3D_pTlutMenu, TLUT_MENU);
    }

    // Re-tile only when the buffer may have changed: in a cutscene the loop runs at
    // 60 Hz but the video at 15 fps, and on the menus most frames are identical.
    // A periodic refresh covers any writes that bypass stdDisplay.
    static int framesSinceUpload = 0;
    if (!stdDisplay_gcMenuDirty && ++framesSinceUpload < 30) {
        std3D_bMenuPending = 1;
        return;
    }
    const uint8_t* src = (const uint8_t*)Video_menuBuffer.surface_lock_alloc;
    uint32_t pitch = Video_menuBuffer.format.rowSize;
    // Only the bands that changed (stdDisplay_GameCube.c tracks them), unless the
    // extent of the changes is unknown, the buffer isn't a plain 640x480, or the
    // periodic refresh is due.
    int bFull = stdDisplay_gcDirtyAll || framesSinceUpload >= 30 || w != 640 || h != 480
                || (pitch & 3) || ((uintptr_t)src & 3);
    stdDisplay_gcMenuDirty = 0;
    framesSinceUpload = 0;

    // The previous frame finished (gcSystem_Present waits on GX_DrawDone), so the
    // texture can be rewritten in place.
    if (bFull) {
        std3D_SwizzleCI8(std3D_pMenuTex, src, w, h, pitch);
    }
    else {
        for (int r = 0; r < STDDISPLAY_GC_ROWS; r++) {
            int x0 = stdDisplay_gcRowDirtyX0[r], x1 = stdDisplay_gcRowDirtyX1[r];
            if (x0 >= x1) continue;
            int tx0 = x0 >> 3, tx1 = (x1 + 7) >> 3; // 8-pixel tile columns
            uint8_t* dst = std3D_pMenuTex + (r * 80 + tx0) * 32;
            std3D_SwizzleCI8(dst, src + r * 4 * pitch + tx0 * 8, (tx1 - tx0) * 8, 4, pitch);
            DCFlushRange(dst, (tx1 - tx0) * 32);
        }
    }
    stdDisplay_GcClearDirty();
    std3D_CutHudRegions(src, pitch);
    if (bFull)
        DCFlushRange(std3D_pMenuTex, 640 * 480);
    GX_InvalidateTexAll();
    std3D_boundTex = NULL;

    std3D_bMenuPending = 1;
}

void std3D_ResetUIRenderList() {}
int  std3D_AddBitmapToTextureCache(stdBitmap* texture, int mipIdx, int is_alpha_tex, int no_alpha) { return 0; }
void std3D_DrawUIBitmapRGBA(stdBitmap* pBmp, int mipIdx, flex_t dstX, flex_t dstY, rdRect* srcRect, flex_t scaleX, flex_t scaleY, int bAlphaOverwrite, uint8_t color_r, uint8_t color_g, uint8_t color_b, uint8_t color_a) {}
void std3D_DrawUIBitmap(stdBitmap* pBmp, int mipIdx, flex_t dstX, flex_t dstY, rdRect* srcRect, flex_t scale, int bAlphaOverwrite) {}
// Added: solid rectangles the HUD asks for (the crosshair: jkHud_DrawGPU). It
// was a stub, and the HUD's own software lines land in the middle of the menu
// buffer, which only shows its status-bar regions here: no crosshair at all.
// Queued and drawn over the scene at the end of the frame (std3D_DrawUIRects).
#define STD3D_MAX_UI_RECTS 16
typedef struct { float x0, y0, x1, y1; uint32_t rgba; } std3DUIRect;
static std3DUIRect std3D_aUIRects[STD3D_MAX_UI_RECTS];
static int std3D_numUIRects = 0;

void std3D_DrawUIClearedRectRGBA(uint8_t color_r, uint8_t color_g, uint8_t color_b, uint8_t color_a, rdRect* dstRect)
{
    if (!dstRect || std3D_numUIRects >= STD3D_MAX_UI_RECTS) return;
    float w = Video_menuBuffer.format.width, h = Video_menuBuffer.format.height;
    if (!w || !h || jkGuiBuildMulti_bRendering) { w = STD3D_SCREEN_W; h = STD3D_SCREEN_H; }
    float sx = STD3D_SCREEN_W / w, sy = STD3D_SCREEN_H / h;
    std3DUIRect* r = &std3D_aUIRects[std3D_numUIRects++];
    r->x0 = dstRect->x * sx;
    r->y0 = dstRect->y * sy;
    r->x1 = (dstRect->x + dstRect->width) * sx;
    r->y1 = (dstRect->y + dstRect->height) * sy;
    r->rgba = ((uint32_t)color_r << 24) | ((uint32_t)color_g << 16) | ((uint32_t)color_b << 8) | color_a;
}

void std3D_DrawUIClearedRect(uint8_t palIdx, rdRect* dstRect)
{
    // The display palette, else the level's (the HUD draws in the game's palette).
    rdColor24 c = std3D_currentPalette[palIdx];
    if (!c.r && !c.g && !c.b && std3D_GcColormap())
        c = std3D_GcColormap()->colors[palIdx];
    std3D_DrawUIClearedRectRGBA(c.r, c.g, c.b, 0xFF, dstRect);
}

static void std3D_DrawUIRects(void)
{
    if (!std3D_numUIRects) return;
    std3D_LoadOrthoProjection();
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    std3D_boundBlend = -1;
    std3D_BindUntextured();
    GX_Begin(GX_QUADS, GX_VTXFMT0, std3D_numUIRects * 4);
    for (int i = 0; i < std3D_numUIRects; i++) {
        const std3DUIRect* r = &std3D_aUIRects[i];
        GX_Position3f32(r->x0, r->y0, 0.0f); GX_Color1u32(r->rgba); GX_TexCoord2f32(0.0f, 0.0f);
        GX_Position3f32(r->x1, r->y0, 0.0f); GX_Color1u32(r->rgba); GX_TexCoord2f32(0.0f, 0.0f);
        GX_Position3f32(r->x1, r->y1, 0.0f); GX_Color1u32(r->rgba); GX_TexCoord2f32(0.0f, 0.0f);
        GX_Position3f32(r->x0, r->y1, 0.0f); GX_Color1u32(r->rgba); GX_TexCoord2f32(0.0f, 0.0f);
    }
    GX_End();
    std3D_numUIRects = 0;
    std3D_LoadSceneProjection();
}
