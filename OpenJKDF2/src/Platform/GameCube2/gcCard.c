// Memory card persistence for the writable overlay of the "dvd:" filesystem.
//
// Two kinds of card files:
//
//  * Settings (profile, options, controls, level progress): the overlay minus the
//    saved games, packed into one small file. Two copies are kept and written
//    alternately (A/B), each stamped with an increasing generation; loading picks
//    the newest intact one, so losing power mid-write only loses the last change.
//      [0x00..0x40)    card comment (two 32-char lines shown by the IPL / Swiss)
//      [0x40..0x50)    u32 blob length, u32 checksum, u32 generation, u32 'OJC2'
//      [0x50..0x58)    u32 'VID0', u32 video mode (0 interlaced, 1 progressive)
//      [0x60..0x2060)  banner + icon (see GCCARD_GFX_OFF)
//      [0x2060..)      gcDvd overlay blob
//
//  * Saved games: up to GCCARD_MAX_SLOTS slots, one card file each, zlib
//    compressed (a save is ~380 KiB raw, 47 blocks; compressed it fits a 59-block
//    card several times over). Slot N is the profile's save000N.jks; gcDvd keeps it
//    out of RAM and loads it through gcCard_LoadSlot while the game has it open.
//      [0x00..0x40)    card comment
//      [0x40..0x50)    u32 'OJS2', u32 raw length, u32 compressed length, u32 checksum
//      [0x60..0x2060)  banner + icon
//      [0x2060..)      zlib stream
//
// Files written before the banner/icon existed (blob at 0x4C, 'OJS1' slots with
// data at 0x50) are still read.

#include "gcCard.h"
#include "gcDvd.h"
#include "gcSystem.h"
#include "gcNotice.h"
#include "gcGame.h"

#include "types.h"
#include "jk.h"
#include "gcCardGfx.h"

#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <zlib.h>
#include "gcAesndlib.h"
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef GC_LIBOGC2
// libogc2's card API: the work area size has another name, the block count is a
// u16 and there is no CARD_GetFreeBlocks (counted here from the directory: the
// card's blocks minus the 5 system blocks minus the files').
#define CARD_WORKAREA_SIZE CARD_WORKAREA
static s32 CARD_GetFreeBlocks(s32 chn, u16* pFree)
{
    u16 total = 0;
    s32 res = CARD_GetBlockCount(chn, &total);
    if (res < 0) return res;
    u32 sector = 8192;
    CARD_GetSectorSize(chn, &sector);
    u32 used = 5;
    card_dir dir;
    for (s32 r = CARD_FindFirst(chn, &dir, true); r >= 0; r = CARD_FindNext(&dir))
        used += (dir.filelen + sector - 1) / sector;
    *pFree = total > used ? (u16)(total - used) : 0;
    return 0;
}
typedef u16 gcCardBlockCount;
#else
typedef u32 gcCardBlockCount;
#endif

#define GCCARD_GAMECODE   "OJKE"
#define GCCARD_COMPANY    "OJ"
// Card file graphics, shown by the GameCube's memory card screen and by Swiss: a
// 96x32 banner then a 32x32 icon, both RGB5A3 (packaging/gamecube/make_card_gfx.py).
#define GCCARD_GFX_OFF        0x60
#define GCCARD_GFX_SIZE       (sizeof(gcCardGfx_banner) + sizeof(gcCardGfx_icon))
#define GCCARD_DATA_OFF       (GCCARD_GFX_OFF + GCCARD_GFX_SIZE)

#define GCCARD_SETTINGS_MAGIC 0x4F4A4332   // "OJC2"
#define GCCARD_LEGACY_BLOB    0x4C         // settings blob before the graphics existed

#define GCCARD_SLOT_MAGIC     0x4F4A5332   // "OJS2"
#define GCCARD_SLOT_MAGIC_V1  0x4F4A5331   // "OJS1": zlib stream at 0x50
#define GCCARD_DEFAULT_SAVE_BLOCKS 8       // free-space estimate before the first save (they take ~6-7)

// Card file names and graphics depend on the game (gcGame.c): both games share the
// card game code, so their files only differ by name.
#define GCCARD_BANNER (gcGame_bMots ? gcCardGfx_bannerMots : gcCardGfx_banner)
#define GCCARD_ICON   (gcGame_bMots ? gcCardGfx_iconMots : gcCardGfx_icon)

static const char* gcCard_SettingsName(int which)
{
    static char aNames[2][32];
    snprintf(aNames[which], sizeof(aNames[which]), "%s_%c.sav", gcGame_CardPrefix(), 'a' + which);
    return aNames[which];
}

static u8       gcCard_workArea[CARD_WORKAREA_SIZE] __attribute__((aligned(32)));
static int      gcCard_slot = -1;
static u32      gcCard_sectorSize = 8192;
static u32      gcCard_lastChecksum = 0;
static u32      gcCard_lastLength = 0;
static u32      gcCard_generation = 0;    // of the newest valid settings file
static int      gcCard_curFile = -1;      // which of the two holds it (-1: none)
static int      gcCard_bLoading = 0;
static int      gcCard_bLegacySettings = 0; // loaded from an old-layout file: rewrite it

#define GCCARD_VIDEO_MAGIC    0x56494430   // "VID0"
static int      gcCard_videoPref = -1;        // 480i/480p choice (-1: never asked)
static int      gcCard_writtenVideoPref = -1; // as stored in the newest settings file

static int      gcCard_aSlotOnCard[GCCARD_MAX_SLOTS + 1];
static int      gcCard_aSlotOldLayout[GCCARD_MAX_SLOTS + 1]; // rewrite to get the banner/icon
// Added: each save slot has two file names (..._sN.sav, ..._sNx.sav); this says
// which one holds it. A new save is written under the other name and the old file
// deleted only once that write has worked, so a write that fails half way (card
// pulled, power cut, a card error) never destroys the save that was there.
static int      gcCard_aSlotAlt[GCCARD_MAX_SLOTS + 1];
static u32      gcCard_saveBlocksEstimate = GCCARD_DEFAULT_SAVE_BLOCKS;
static char     gcCard_szLastError[160] = "";  // why the last gcCard_Save failed

static void gcCard_SetError(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void gcCard_SetError(const char* fmt, ...)
{
    if (gcCard_szLastError[0]) return; // keep the first cause
    va_list args;
    va_start(args, fmt);
    vsnprintf(gcCard_szLastError, sizeof(gcCard_szLastError), fmt, args);
    va_end(args);
    printf("gcCard: %s\n", gcCard_szLastError);
}

const char* gcCard_GetLastError(void)
{
    return gcCard_szLastError;
}

static u32 gcCard_Checksum(const u8* p, u32 len, u32 seed)
{
    // FNV-1a
    u32 h = 2166136261u ^ seed;
    for (u32 i = 0; i < len; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static u32 gcCard_RoundUp(u32 len)
{
    return (len + gcCard_sectorSize - 1) & ~(gcCard_sectorSize - 1);
}

static void gcCard_SlotFilenameAlt(int n, int alt, char* buf, size_t len)
{
    snprintf(buf, len, alt ? "%s_s%dx.sav" : "%s_s%d.sav", gcGame_CardPrefix(), n);
}

// The file that holds slot n now.
static void gcCard_SlotFilename(int n, char* buf, size_t len)
{
    gcCard_SlotFilenameAlt(n, gcCard_aSlotAlt[n], buf, len);
}

static void gcCard_SlotPath(int n, char* buf, size_t len)
{
    snprintf(buf, len, "%s/player/%s/save%04d.jks", gcGame_Root(), gcGame_Player(), n);  // the fixed profile
}

// --- Mounting ----------------------------------------------------------------------

#define GCCARD_MOUNT_TIMEOUT_MS 5000

static int gcCard_bMountTried = 0;

static void gcCard_MountCallback(s32 chn, s32 result)
{
}

// Mounting a card that is still locked since power-on runs the unlock exchange
// as a DSP task. That only completes while the DSP is free: once AESND's microcode
// owns it, the task never runs and a synchronous CARD_Mount sleeps forever (real
// hardware only; Dolphin's cards start unlocked). So this runs from gcSystem_Init
// before the DSP is started, and polls with a timeout instead of CARD_Mount.
static s32 gcCard_MountSlot(int slot)
{
    s32 res = CARD_MountAsync(slot, gcCard_workArea, NULL, gcCard_MountCallback);
    if (res < 0) return res;
    u64 start = gettime();
    while ((res = CARD_GetErrorCode(slot)) == CARD_ERROR_BUSY) {
        if (diff_msec(start, gettime()) > GCCARD_MOUNT_TIMEOUT_MS) {
            printf("gcCard: slot %c did not answer\n", 'A' + slot);
            return CARD_ERROR_BUSY;
        }
        VIDEO_WaitVSync();
    }
    return res;
}

static int gcCard_MountAny(void)
{
    if (gcCard_slot >= 0) return 1;
    if (gcCard_bMountTried) return 0;
    gcCard_bMountTried = 1;
    for (int slot = CARD_SLOTA; slot <= CARD_SLOTB; slot++) {
        printf("gcCard: checking slot %c\n", 'A' + slot);
        s32 res = gcCard_MountSlot(slot);
        if (res >= 0 || res == CARD_ERROR_BROKEN) {
            gcCard_slot = slot;
            CARD_GetSectorSize(slot, &gcCard_sectorSize);
            printf("gcCard: memory card in slot %c (sector %u)\n", 'A' + slot, gcCard_sectorSize);
            return 1;
        }
    }
    return 0;
}

void gcCard_EarlyMount(void)
{
    CARD_Init(GCCARD_GAMECODE, GCCARD_COMPANY);
    gcCard_MountAny();
}

// --- File I/O ------------------------------------------------------------------------
// Card I/O runs on the game thread (a background writer came out truncated in
// Dolphin). While writing, stdout skips the OSReport UART, which shares the EXI
// channel with the memory card.

#define GCCARD_ERR_CREATE (-1000)

// Read a whole card file into a malloc'd, 32-byte aligned buffer.
static u8* gcCard_ReadAll(const char* name, u32* pLen)
{
    card_file file;
    if (CARD_Open(gcCard_slot, name, &file) < 0)
        return NULL;
    u32 len = (u32)file.len;
    u8* buf = (u8*)memalign(32, len);
    if (buf && CARD_Read(&file, buf, len, 0) < 0) {
        free(buf);
        buf = NULL;
    }
    CARD_Close(&file);
    if (buf && pLen) *pLen = len;
    return buf;
}

// "Saving..." on screen while a gcCard_Save writes (shown on its first write).
static int gcCard_bNoticeShown = 0;

static void gcCard_NoticeBegin(void)
{
    if (gcCard_bNoticeShown) return;
    gcNotice_Show("Saving to memory card...");
    gcCard_bNoticeShown = 1;
}

// Fills the sector at file offset off (gcCard_sectorSize bytes). Returns 0 on error.
typedef int (*gcCardFillFn)(void* ctx, u8* sector, u32 off);

static int gcCard_FillFromBuffer(void* ctx, u8* sector, u32 off)
{
    memcpy(sector, (const u8*)ctx + off, gcCard_sectorSize);
    return 1;
}

static s32 gcCard_WriteStream(const char* name, u32 fileLen, gcCardFillFn fill, void* ctx);

static s32 gcCard_WriteAll(const char* name, u8* buf, u32 fileLen)
{
    return gcCard_WriteStream(name, fileLen, gcCard_FillFromBuffer, buf);
}

// Writes a file of fileLen bytes (whole sectors), each sector produced by fill,
// so the file never has to be in memory at once.
static s32 gcCard_WriteStream(const char* name, u32 fileLen, gcCardFillFn fill, void* ctx)
{
    u8* sector = (u8*)memalign(32, gcCard_sectorSize);
    if (!sector) {
        gcCard_SetError("out of memory writing %s", name);
        return GCCARD_ERR_CREATE;
    }
    gcCard_NoticeBegin();
    card_file file;
    s32 res = CARD_Open(gcCard_slot, name, &file);
    if (res >= 0 && (u32)file.len != fileLen) {
        // Resizing means delete + create. Only delete when the new size surely
        // fits (free blocks plus the file's own); otherwise a full card would lose
        // the file. A file that is merely bigger than needed is written in place.
        u16 freeBlocks = 0;
        CARD_GetFreeBlocks(gcCard_slot, &freeBlocks);
        u32 have = freeBlocks + (u32)file.len / gcCard_sectorSize;
        u32 need = fileLen / gcCard_sectorSize;
        if ((u32)file.len > fileLen && have < need + need) {
            // keep the bigger file (shrinking is only worth it with room to spare)
        }
        else if (have >= need) {
            CARD_Close(&file);
            CARD_Delete(gcCard_slot, name);
            res = -1;
        }
        else {
            CARD_Close(&file);
            gcCard_SetError("%s: needs %u blocks, %u free", name, need, have);
            free(sector);
            return GCCARD_ERR_CREATE;
        }
    }
    if (res < 0) {
        res = CARD_Create(gcCard_slot, name, fileLen, &file);
        if (res < 0) {
            u16 freeBlocks = 0;
            CARD_GetFreeBlocks(gcCard_slot, &freeBlocks);
            gcCard_SetError("create %s (%u blocks) error %d, %u free",
                            name, fileLen / gcCard_sectorSize, (int)res, freeBlocks);
            free(sector);
            return GCCARD_ERR_CREATE;
        }
    }
    gcSystem_bMuteReport = 1;
    // One sector per call: libogc's CARD_Write stops after the first sector of a
    // longer write (its callback miscounts the remaining length and never advances
    // the buffer), which left every save bigger than 8 KiB truncated.
    // A failed sector is retried a couple of times (real cards sometimes report an
    // I/O error, -5, on a write; Dolphin never does).
    u32 off = 0, failMs = 0;
    int bFillFailed = 0;
    for (; off < fileLen && res >= 0; off += gcCard_sectorSize) {
        if (!fill(ctx, sector, off)) { bFillFailed = 1; res = GCCARD_ERR_CREATE; break; }
        DCFlushRange(sector, gcCard_sectorSize);
        for (int attempt = 0; attempt < 3; attempt++) {
            u64 w0 = gettime();
            res = CARD_Write(&file, sector, gcCard_sectorSize, off);
            if (res >= 0) break;
            failMs = diff_msec(w0, gettime());
        }
    }
    gcSystem_bMuteReport = 0;
    free(sector);
    if (bFillFailed) {
        gcCard_SetError("preparing %s failed", name);
    }
    else if (res < 0) {
        gcCardBlockCount cardBlocks = 0;
        CARD_GetBlockCount(gcCard_slot, &cardBlocks);
        gcCard_SetError("write %s error %d at block %u; card %u blocks, sector %u at %u, %u ms",
                        name, (int)res, off / gcCard_sectorSize - 1, cardBlocks,
                        gcCard_sectorSize, (u32)file.iblock, failMs);
    }
    if (res >= 0) {
        card_stat stat;
        if (CARD_GetStatus(gcCard_slot, file.filenum, &stat) >= 0) {
            stat.comment_addr = 0; // the two comment lines at the start of the file
            // Banner and one still icon. (Fields set directly: libogc's
            // CARD_SetIconSpeed macro writes the wrong field.)
            stat.banner_fmt = CARD_BANNER_RGB;
            stat.icon_addr = GCCARD_GFX_OFF;
            stat.icon_fmt = CARD_ICON_RGB;
            stat.icon_speed = CARD_SPEED_SLOW;
            CARD_SetStatus(gcCard_slot, file.filenum, &stat);
        }
    }
    CARD_Close(&file);
    return res;
}

// Comment lines and graphics at the start of a file buffer.
static void gcCard_SetHeader(u8* buf, const char* line1, const char* line2)
{
    memset(buf, 0, 0x40);
    strncpy((char*)buf, line1, 31);
    strncpy((char*)buf + 32, line2, 31);
    memcpy(buf + GCCARD_GFX_OFF, GCCARD_BANNER, sizeof(gcCardGfx_banner));
    memcpy(buf + GCCARD_GFX_OFF + sizeof(gcCardGfx_banner), GCCARD_ICON, sizeof(gcCardGfx_icon));
}

// --- Settings ------------------------------------------------------------------------

// Read one of the two settings files. Returns a malloc'd buffer (caller frees) if
// it is present and intact, filling in where its blob starts, its length, checksum
// and generation.
static u8* gcCard_ReadSettings(int which, u32* pBlobOff, u32* pBlobLen, u32* pChecksum, u32* pGeneration)
{
    u32 len = 0;
    u8* buf = gcCard_ReadAll(gcCard_SettingsName(which), &len);
    if (!buf) return NULL;
    int ok = 0;
    if (len > GCCARD_LEGACY_BLOB) {
        u32 magic;
        memcpy(pBlobLen, buf + 0x40, 4);
        memcpy(pChecksum, buf + 0x44, 4);
        memcpy(pGeneration, buf + 0x48, 4);
        memcpy(&magic, buf + 0x4C, 4);
        *pBlobOff = magic == GCCARD_SETTINGS_MAGIC ? GCCARD_DATA_OFF : GCCARD_LEGACY_BLOB;
        ok = *pBlobOff <= len && *pBlobLen <= len - *pBlobOff
             && gcCard_Checksum(buf + *pBlobOff, *pBlobLen, *pGeneration) == *pChecksum;
        if (!ok) printf("gcCard: %s is damaged, ignoring it\n", gcCard_SettingsName(which));
    }
    if (!ok) { free(buf); return NULL; }
    return buf;
}

static int gcCard_SaveSettings(void)
{
    u32 blobLen = gcDvd_OverlaySerializedSize();
    u32 fileLen = gcCard_RoundUp(GCCARD_DATA_OFF + blobLen);
    u8* buf = (u8*)memalign(32, fileLen);
    if (!buf) {
        gcCard_SetError("out of memory for the settings (%u KiB)", fileLen / 1024);
        return 0;
    }
    memset(buf, 0, fileLen);

    blobLen = gcDvd_OverlaySerialize(buf + GCCARD_DATA_OFF, fileLen - GCCARD_DATA_OFF);
    u32 generation = gcCard_generation + 1;
    u32 checksum = gcCard_Checksum(buf + GCCARD_DATA_OFF, blobLen, generation);
    if (blobLen == gcCard_lastLength && !gcCard_bLegacySettings
        && gcCard_videoPref == gcCard_writtenVideoPref
        && gcCard_Checksum(buf + GCCARD_DATA_OFF, blobLen, gcCard_generation) == gcCard_lastChecksum) {
        free(buf); // nothing changed since the last write
        return 1;
    }

    gcCard_SetHeader(buf, gcGame_Title(), "Settings");
    u32 magic = GCCARD_SETTINGS_MAGIC;
    memcpy(buf + 0x40, &blobLen, 4);
    memcpy(buf + 0x44, &checksum, 4);
    memcpy(buf + 0x48, &generation, 4);
    memcpy(buf + 0x4C, &magic, 4);
    u32 video[2] = { GCCARD_VIDEO_MAGIC, (u32)gcCard_videoPref };
    memcpy(buf + 0x50, video, sizeof(video));

    // Overwrite the older copy; the newest one stays intact until this write is done.
    int target = gcCard_curFile == 0 ? 1 : 0;
    s32 res = gcCard_WriteAll(gcCard_SettingsName(target), buf, fileLen);
    free(buf);
    if (res < 0) {
        printf("gcCard: writing %s failed (%d)\n", gcCard_SettingsName(target), (int)res);
        return 0;
    }
    gcCard_lastChecksum = checksum;
    gcCard_lastLength = blobLen;
    gcCard_generation = generation;
    gcCard_curFile = target;
    gcCard_bLegacySettings = 0;
    gcCard_writtenVideoPref = gcCard_videoPref;
    printf("gcCard: settings saved (%u bytes, %s, generation %u)\n", blobLen, gcCard_SettingsName(target), generation);
    return 1;
}

// --- Save slots ----------------------------------------------------------------------

// "Save N: <level>" for the card's file list, from the save's header (its name is
// "<level title>~<save name>").
static void gcCard_SlotComment(int n, const uint8_t* pSave, uint32_t size, char* out, size_t outLen)
{
    char title[32] = "";
    if (size >= sizeof(sithGamesave_Header)) {
        const sithGamesave_Header* h = (const sithGamesave_Header*)pSave;
        size_t i = 0;
        for (; i < sizeof(title) - 1 && h->saveName[i] && h->saveName[i] != u'~'; i++)
            title[i] = h->saveName[i] < 0x80 ? (char)h->saveName[i] : '?';
        title[i] = 0;
    }
    snprintf(out, outLen, "Save %d: %s", n, title);
}

// Added: a save is compressed and written a sector at a time. It used to be
// compressed into one buffer of its worst case size (~480 KiB) plus zlib's
// ~270 KiB, which mid-level (level 4) wasn't there: "out of memory for save".
// A small deflate window (~64 KiB in all) runs twice: once to learn the
// compressed length and checksum (they go in the header, and the card file is
// created at its final size), then again to produce the sectors. The output is
// an ordinary zlib stream (loading reads either).
#define GCCARD_DEFLATE_WBITS 13
#define GCCARD_DEFLATE_MEMLEVEL 6

// Compresses a save held in pieces (gcDvdData), a bit of output at a time.
typedef struct gcCardDeflate {
    z_stream  zs;
    gcDvdData data;
    uint32_t  nextPiece;
    uint32_t  numPieces;
    int       bEnd;
} gcCardDeflate;

static int gcCard_DeflateBegin(gcCardDeflate* d, const gcDvdData* pData)
{
    memset(d, 0, sizeof(*d));
    d->data = *pData;
    d->numPieces = (pData->size + GCDVD_DATA_CHUNK - 1) / GCDVD_DATA_CHUNK;
    return deflateInit2(&d->zs, Z_BEST_SPEED, Z_DEFLATED, GCCARD_DEFLATE_WBITS,
                        GCCARD_DEFLATE_MEMLEVEL, Z_DEFAULT_STRATEGY) == Z_OK;
}

// Fills out[0..len) as far as the stream goes; returns the bytes made, -1 on error.
static int gcCard_DeflateSome(gcCardDeflate* d, u8* out, u32 len)
{
    d->zs.next_out = out;
    d->zs.avail_out = len;
    while (d->zs.avail_out && !d->bEnd) {
        if (!d->zs.avail_in && d->nextPiece < d->numPieces) {
            uint32_t off = d->nextPiece * GCDVD_DATA_CHUNK;
            uint32_t k = d->data.size - off;
            if (k > GCDVD_DATA_CHUNK) k = GCDVD_DATA_CHUNK;
            d->zs.next_in = (Bytef*)d->data.chunks[d->nextPiece++];
            d->zs.avail_in = k;
        }
        int flush = d->nextPiece >= d->numPieces ? Z_FINISH : Z_NO_FLUSH;
        int z = deflate(&d->zs, flush);
        if (z == Z_STREAM_END) d->bEnd = 1;
        else if (z == Z_BUF_ERROR) {
            if (d->zs.avail_in || d->nextPiece >= d->numPieces) return -1; // stuck
        }
        else if (z != Z_OK) return -1;
    }
    return (int)(len - d->zs.avail_out);
}

typedef struct gcCardSlotStream {
    gcCardDeflate d;
    const u8* pHead;     // the header area, GCCARD_DATA_OFF bytes
} gcCardSlotStream;

static int gcCard_FillSlotSector(void* ctx, u8* sector, u32 off)
{
    gcCardSlotStream* st = (gcCardSlotStream*)ctx;
    u32 pos = 0;
    // The header area first.
    if (off < GCCARD_DATA_OFF) {
        u32 n = GCCARD_DATA_OFF - off;
        if (n > gcCard_sectorSize) n = gcCard_sectorSize;
        memcpy(sector, st->pHead + off, n);
        pos = n;
    }
    // Then the compressed stream, zeros after its end.
    if (pos < gcCard_sectorSize) {
        int got = gcCard_DeflateSome(&st->d, sector + pos, gcCard_sectorSize - pos);
        if (got < 0) return 0;
        pos += (u32)got;
        memset(sector + pos, 0, gcCard_sectorSize - pos);
    }
    return 1;
}

static int gcCard_WriteSlot(int n, const gcDvdData* pData)
{
    // Pass 1: compressed length and checksum.
    gcCardDeflate d;
    if (!gcCard_DeflateBegin(&d, pData)) {
        deflateEnd(&d.zs);
        gcCard_SetError("out of memory for save %d (zlib)", n);
        return 0;
    }
    static u8 scratch[4096];
    u32 compLen = 0;
    u32 sum = 2166136261u; // gcCard_Checksum(.., 0), a piece at a time
    int got;
    while ((got = gcCard_DeflateSome(&d, scratch, sizeof(scratch))) > 0) {
        for (int i = 0; i < got; i++) { sum ^= scratch[i]; sum *= 16777619u; }
        compLen += (u32)got;
    }
    int bEnd = d.bEnd;
    deflateEnd(&d.zs);
    if (got < 0 || !bEnd) {
        gcCard_SetError("compressing save %d failed", n);
        return 0;
    }
    u32 fileLen = gcCard_RoundUp(GCCARD_DATA_OFF + compLen);

    // The header area: comment lines, graphics, then the slot header.
    static u8 head[GCCARD_DATA_OFF];
    memset(head, 0, sizeof(head));
    char comment[64];
    if (pData->size)
        gcCard_SlotComment(n, pData->chunks[0], pData->size < GCDVD_DATA_CHUNK ? pData->size : GCDVD_DATA_CHUNK,
                           comment, sizeof(comment));
    else
        snprintf(comment, sizeof(comment), "Save %d", n);
    gcCard_SetHeader(head, gcGame_Title(), comment);
    u32 hdr[4] = { GCCARD_SLOT_MAGIC, pData->size, compLen, sum };
    memcpy(head + 0x40, hdr, sizeof(hdr));

    // Pass 2: the same stream, into the card's sectors.
    gcCardSlotStream st;
    st.pHead = head;
    if (!gcCard_DeflateBegin(&st.d, pData)) {
        deflateEnd(&st.d.zs);
        gcCard_SetError("out of memory for save %d (zlib)", n);
        return 0;
    }
    // Under the other name if the card has room for both files for a moment
    // (blocks free, plus those of a leftover file under that name); else in
    // place, as before.
    char name[32], oldName[32];
    int alt = gcCard_aSlotAlt[n];
    int target = alt;
    gcCard_SlotFilenameAlt(n, alt, oldName, sizeof(oldName));
    if (gcCard_aSlotOnCard[n]) {
        u16 freeBlocks = 0;
        u32 have = 0;
        if (CARD_GetFreeBlocks(gcCard_slot, &freeBlocks) >= 0) have = freeBlocks;
        char other[32];
        gcCard_SlotFilenameAlt(n, !alt, other, sizeof(other));
        card_file f;
        if (CARD_Open(gcCard_slot, other, &f) >= 0) {
            have += (u32)f.len / gcCard_sectorSize;
            CARD_Close(&f);
        }
        if (have >= fileLen / gcCard_sectorSize) target = !alt;
        else printf("gcCard: no room for a second copy of save %d, writing it in place\n", n);
    }
    gcCard_SlotFilenameAlt(n, target, name, sizeof(name));
    s32 res = gcCard_WriteStream(name, fileLen, gcCard_FillSlotSector, &st);
    bEnd = st.d.bEnd;
    deflateEnd(&st.d.zs);
    if (res >= 0 && !bEnd) {
        gcCard_SetError("save %d: compressed data didn't fit", n);
        return 0;
    }
    if (res >= 0 && target != alt) {
        // The new copy is complete: now the old one can go.
        CARD_Delete(gcCard_slot, oldName);
        gcCard_aSlotAlt[n] = target;
    }
    if (res < 0) {
        printf("gcCard: writing %s failed (%d)\n", name, (int)res);
        return 0;
    }
    u32 blocks = fileLen / gcCard_sectorSize;
    if (blocks > gcCard_saveBlocksEstimate) gcCard_saveBlocksEstimate = blocks;
    printf("gcCard: %s saved (%u -> %u bytes, %u blocks)\n", name, pData->size, compLen, blocks);
    return 1;
}

// Whether a slot file is complete: its header and the checksum of its data.
static int gcCard_SlotFileIntact(const char* name)
{
    u32 len = 0;
    u8* buf = gcCard_ReadAll(name, &len);
    if (!buf) return 0;
    u32 hdr[4];
    memcpy(hdr, buf + 0x40, sizeof(hdr));
    u32 dataOff = hdr[0] == GCCARD_SLOT_MAGIC_V1 ? 0x50 : GCCARD_DATA_OFF;
    int ok = (hdr[0] == GCCARD_SLOT_MAGIC || hdr[0] == GCCARD_SLOT_MAGIC_V1)
             && len >= dataOff && hdr[2] <= len - dataOff
             && gcCard_Checksum(buf + dataOff, hdr[2], 0) == hdr[3];
    free(buf);
    return ok;
}

static u32 gcCard_FileTime(const char* name)
{
    card_file f;
    card_stat st;
    u32 t = 0;
    if (CARD_Open(gcCard_slot, name, &f) < 0) return 0;
    if (CARD_GetStatus(gcCard_slot, f.filenum, &st) >= 0) t = st.time;
    CARD_Close(&f);
    return t;
}

// Which of slot n's two names holds it. Both are only there after a write that
// was cut off before the old file was deleted (or right after a successful one):
// keep the newest complete file and delete the other.
static int gcCard_PickSlotFile(int n)
{
    char a[32], b[32];
    gcCard_SlotFilenameAlt(n, 0, a, sizeof(a));
    gcCard_SlotFilenameAlt(n, 1, b, sizeof(b));
    card_file f;
    int hasA = CARD_Open(gcCard_slot, a, &f) >= 0;
    if (hasA) CARD_Close(&f);
    int hasB = CARD_Open(gcCard_slot, b, &f) >= 0;
    if (hasB) CARD_Close(&f);
    if (!hasA || !hasB) return hasB;

    int okA = gcCard_SlotFileIntact(a), okB = gcCard_SlotFileIntact(b);
    int pick;
    if (okA != okB) pick = okB;
    else pick = gcCard_FileTime(b) > gcCard_FileTime(a);
    printf("gcCard: save %d has two copies (%s), keeping %s\n", n,
           okA && okB ? "both intact" : "one damaged", pick ? b : a);
    CARD_Delete(gcCard_slot, pick ? a : b);
    return pick;
}

// gcDvd slot loader: fetch save slot n and hand it over decompressed, a piece at
// a time (no buffer of the whole save: mid-level there was no block that big).
static int gcCard_LoadSlot(int n, gcDvdSlotSink sink, void* ctx)
{
    if (!gcCard_MountAny()) return 0;
    char name[32];
    gcCard_SlotFilename(n, name, sizeof(name));
    u32 len = 0;
    u8* buf = gcCard_ReadAll(name, &len);
    if (!buf) return 0;

    int ok = 0;
    u32 hdr[4];
    memcpy(hdr, buf + 0x40, sizeof(hdr));
    u32 dataOff = hdr[0] == GCCARD_SLOT_MAGIC_V1 ? 0x50 : GCCARD_DATA_OFF;
    if ((hdr[0] == GCCARD_SLOT_MAGIC || hdr[0] == GCCARD_SLOT_MAGIC_V1)
        && len >= dataOff && hdr[2] <= len - dataOff
        && gcCard_Checksum(buf + dataOff, hdr[2], 0) == hdr[3])
    {
        z_stream zs;
        memset(&zs, 0, sizeof(zs));
        if (inflateInit(&zs) == Z_OK) {
            static u8 out[8192];
            u32 total = 0;
            int z;
            zs.next_in = buf + dataOff;
            zs.avail_in = hdr[2];
            do {
                zs.next_out = out;
                zs.avail_out = sizeof(out);
                z = inflate(&zs, Z_NO_FLUSH);
                u32 got = sizeof(out) - zs.avail_out;
                if (got && !sink(ctx, out, got)) { z = Z_MEM_ERROR; break; }
                total += got;
            } while (z == Z_OK);
            inflateEnd(&zs);
            ok = z == Z_STREAM_END && total == hdr[1];
            if (z == Z_MEM_ERROR) printf("gcCard: no memory to load %s\n", name);
        }
    }
    if (!ok) printf("gcCard: %s is damaged or could not be loaded\n", name);
    free(buf);
    return ok;
}

// Bring the card's save slots in line with the overlay: write changed ones,
// delete the files of slots the game deleted.
static int gcCard_SyncSlots(void)
{
    int ok = 1;
    for (int n = 1; n <= GCCARD_MAX_SLOTS; n++)
    {
        char path[64], name[32];
        gcCard_SlotPath(n, path, sizeof(path));
        gcCard_SlotFilename(n, name, sizeof(name));
        gcDvdData data;
        switch (gcDvd_SlotState(path, &data))
        {
            case GCDVD_SLOT_ABSENT:
                if (gcCard_aSlotOnCard[n]) {
                    char other[32];
                    gcCard_SlotFilenameAlt(n, !gcCard_aSlotAlt[n], other, sizeof(other));
                    gcCard_NoticeBegin();
                    CARD_Delete(gcCard_slot, name);
                    CARD_Delete(gcCard_slot, other); // a leftover copy, if any
                    gcCard_aSlotOnCard[n] = 0;
                    gcCard_aSlotAlt[n] = 0;
                    printf("gcCard: %s deleted\n", name);
                }
                break;
            case GCDVD_SLOT_CHANGED:
                if (gcCard_WriteSlot(n, &data)) {
                    gcCard_aSlotOnCard[n] = 1;
                    gcDvd_SlotPersisted(path, n);
                }
                else ok = 0;
                break;
            default:
                // Unchanged, but written before files had a banner/icon: rewrite once.
                if (gcCard_aSlotOldLayout[n]) {
                    // Once per boot, even if it fails (e.g. the card is full):
                    // it means a decompress + recompress + write.
                    gcCard_aSlotOldLayout[n] = 0;
                    if (gcDvd_SlotLoadData(path, &data)) {
                        gcCard_WriteSlot(n, &data);
                        gcDvd_SlotDropData(path);
                    }
                }
                break;
        }
    }
    return ok;
}

int gcCard_GetSlotInfo(int n, int* pbUsed)
{
    char path[64];
    gcCard_SlotPath(n, path, sizeof(path));
    *pbUsed = gcDvd_SlotState(path, NULL) != GCDVD_SLOT_ABSENT;
    return 1;
}

int gcCard_CanCreateSave(void)
{
    unsigned int freeBlocks, needBlocks;
    return gcCard_GetSpace(&freeBlocks, &needBlocks) && freeBlocks >= needBlocks;
}

int gcCard_GetSpace(unsigned int* pFreeBlocks, unsigned int* pNeedBlocks)
{
    *pFreeBlocks = 0;
    *pNeedBlocks = gcCard_saveBlocksEstimate;
    if (!gcCard_MountAny()) return 0;
    u16 freeBlocks = 0;
    if (CARD_GetFreeBlocks(gcCard_slot, &freeBlocks) < 0) return 0;
    *pFreeBlocks = freeBlocks;
    return 1;
}

// --- Overlay sync --------------------------------------------------------------------
// Card writes block the game for a moment (a save takes a few hundred ms to a couple
// of seconds), so they only happen where a pause goes unnoticed: on loading
// screens, on the main menu, and right away when the player saves a game
// (gcCard_FlushPending / gcCard_Save). Never during play or a cutscene.

static int gcCard_bDirty = 0;

static void gcCard_OnOverlayWrite(void)
{
    if (gcCard_bLoading) return;
    gcCard_bDirty = 1;
}

int gcCard_Save(void)
{
    gcCard_bDirty = 0;
    gcCard_szLastError[0] = 0;
    if (!gcCard_MountAny()) {
        gcCard_SetError("no memory card in slot A or B");
        return 0;
    }
    // On real hardware card writes timed out (the card's "erase done" interrupt
    // never came in time) while the game ran: keep the SD card reads and the DSP
    // mixer (thousands of interrupts a second) quiet while writing.
    gcDvd_PauseReads();
    AESND_Pause(true);
    int ok = gcCard_SaveSettings();
    ok = gcCard_SyncSlots() && ok;
    AESND_Pause(false);
    gcDvd_ResumeReads();
    if (gcCard_bNoticeShown) {
        gcNotice_Hide();
        gcCard_bNoticeShown = 0;
    }
    if (!ok)
        gcCard_bDirty = 1; // try again at the next safe point
    return ok;
}

int gcCard_GetVideoPref(void)
{
    if (gcCard_videoPref >= 0 || !gcCard_MountAny()) return gcCard_videoPref;
    // Newest intact settings file (gcCard_Init reads them again later).
    u32 best = 0;
    for (int i = 0; i < 2; i++) {
        u32 off, len, sum, gen;
        u8* buf = gcCard_ReadSettings(i, &off, &len, &sum, &gen);
        if (!buf) continue;
        u32 video[2];
        memcpy(video, buf + 0x50, sizeof(video));
        if (off == GCCARD_DATA_OFF && video[0] == GCCARD_VIDEO_MAGIC && gen >= best) {
            best = gen;
            gcCard_videoPref = (int)video[1];
        }
        free(buf);
    }
    return gcCard_videoPref;
}

void gcCard_SetVideoPref(int bProgressive)
{
    if (gcCard_videoPref == bProgressive) return;
    gcCard_videoPref = bProgressive;
    gcCard_bDirty = 1; // written at the next safe point
}

void gcCard_FlushPending(void)
{
    if (gcCard_bDirty)
        gcCard_Save();
}

void gcCard_Init(void)
{
    CARD_Init(GCCARD_GAMECODE, GCCARD_COMPANY);
    gcDvd_SetWriteCallback(gcCard_OnOverlayWrite);
    gcDvd_SetSlotLoader(gcCard_LoadSlot);

    if (!gcCard_MountAny()) {
        printf("gcCard: no memory card, saves will not persist\n");
        return;
    }

    u8* aBuf[2];
    u32 aOff[2], aLen[2], aSum[2], aGen[2];
    for (int i = 0; i < 2; i++)
        aBuf[i] = gcCard_ReadSettings(i, &aOff[i], &aLen[i], &aSum[i], &aGen[i]);

    int pick = -1;
    if (aBuf[0] && aBuf[1]) pick = aGen[0] >= aGen[1] ? 0 : 1;
    else if (aBuf[0]) pick = 0;
    else if (aBuf[1]) pick = 1;

    gcCard_bLoading = 1;
    if (pick < 0) {
        printf("gcCard: no settings yet\n");
    }
    else {
        int ok = gcDvd_OverlayDeserialize(aBuf[pick] + aOff[pick], aLen[pick]);
        // Old layout, or graphics from an older build: rewrite with the current ones.
        gcCard_bLegacySettings = aOff[pick] != GCCARD_DATA_OFF
            || memcmp(aBuf[pick] + GCCARD_GFX_OFF, GCCARD_BANNER, sizeof(gcCardGfx_banner))
            || memcmp(aBuf[pick] + GCCARD_GFX_OFF + sizeof(gcCardGfx_banner), GCCARD_ICON, sizeof(gcCardGfx_icon));
        gcCard_lastChecksum = aSum[pick];
        gcCard_lastLength = aLen[pick];
        u32 video[2];
        memcpy(video, aBuf[pick] + 0x50, sizeof(video));
        gcCard_writtenVideoPref = video[0] == GCCARD_VIDEO_MAGIC ? (int)video[1] : -1;
        gcCard_generation = aGen[pick];
        gcCard_curFile = pick;
        printf("gcCard: settings loaded (%u bytes, %s, generation %u, %s)\n",
               aLen[pick], gcCard_SettingsName(pick), aGen[pick], ok ? "ok" : "partial");
    }
    free(aBuf[0]);
    free(aBuf[1]);

    // Added: the video mode was picked at boot from an early read of these same
    // files; when that read failed (the card isn't always ready that early) it
    // fell back to 480p although 480i had been chosen. Follow the settings now.
    if (gcCard_videoPref < 0 && gcCard_writtenVideoPref >= 0)
        gcCard_videoPref = gcCard_writtenVideoPref;
    if (gcCard_videoPref == 0 && gcSystem_IsProgressive())
        gcSystem_SetProgressive(0);

    // Save slots: only their sizes now; the data stays on the card until opened.
    for (int n = 1; n <= GCCARD_MAX_SLOTS; n++) {
        char name[32], path[64];
        gcCard_aSlotAlt[n] = gcCard_PickSlotFile(n);
        gcCard_SlotFilename(n, name, sizeof(name));
        gcCard_SlotPath(n, path, sizeof(path));
        card_file file;
        if (CARD_Open(gcCard_slot, name, &file) < 0) continue;
        // Size first: libogc's CARD_Read clobbers file.len.
        u32 blocks = (u32)file.len / gcCard_sectorSize;
        // First sector: header, banner and (most of) the icon.
        u8* hdr = (u8*)memalign(32, gcCard_sectorSize);
        u32 h[4] = { 0, 0, 0, 0 };
        int bOldGfx = 1;
        if (hdr && CARD_Read(&file, hdr, gcCard_sectorSize, 0) >= 0) {
            memcpy(h, hdr + 0x40, sizeof(h));
            u32 n1 = sizeof(gcCardGfx_banner);
            u32 n2 = gcCard_sectorSize - GCCARD_GFX_OFF - n1;
            bOldGfx = memcmp(hdr + GCCARD_GFX_OFF, GCCARD_BANNER, n1)
                      || memcmp(hdr + GCCARD_GFX_OFF + n1, GCCARD_ICON, n2);
        }
        CARD_Close(&file);
        free(hdr);
        if (h[0] != GCCARD_SLOT_MAGIC && h[0] != GCCARD_SLOT_MAGIC_V1) continue;
        gcCard_aSlotOnCard[n] = 1;
        gcCard_aSlotOldLayout[n] = h[0] == GCCARD_SLOT_MAGIC_V1 || bOldGfx;
        if (blocks > gcCard_saveBlocksEstimate) gcCard_saveBlocksEstimate = blocks;
        gcDvd_SlotAttach(n, path, h[1]);
        printf("gcCard: save slot %d (%u bytes, %u blocks)\n", n, h[1], blocks);
    }
    gcCard_bLoading = 0;

    // Saves carried over inside an old layout get rewritten at the first safe point.
    gcCard_bDirty = 1;
}
