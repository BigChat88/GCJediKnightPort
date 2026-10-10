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
#include <aesndlib.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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

static void gcCard_SlotFilename(int n, char* buf, size_t len)
{
    snprintf(buf, len, "%s_s%d.sav", gcGame_CardPrefix(), n);
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

static s32 gcCard_WriteAll(const char* name, u8* buf, u32 fileLen)
{
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
    for (; off < fileLen && res >= 0; off += gcCard_sectorSize) {
        for (int attempt = 0; attempt < 3; attempt++) {
            u64 w0 = gettime();
            res = CARD_Write(&file, buf + off, gcCard_sectorSize, off);
            if (res >= 0) break;
            failMs = diff_msec(w0, gettime());
        }
    }
    gcSystem_bMuteReport = 0;
    if (res < 0) {
        u32 cardBlocks = 0;
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

static int gcCard_WriteSlot(int n, const uint8_t* pData, uint32_t size)
{
    uLongf compLen = compressBound(size);
    u32 fileLen = gcCard_RoundUp(GCCARD_DATA_OFF + compLen);
    u8* buf = (u8*)memalign(32, fileLen);
    if (!buf) {
        gcCard_SetError("out of memory for save %d (%u KiB)", n, fileLen / 1024);
        return 0;
    }
    memset(buf, 0, fileLen);
    int zres = compress2(buf + GCCARD_DATA_OFF, &compLen, pData, size, Z_BEST_SPEED);
    if (zres != Z_OK) {
        free(buf);
        gcCard_SetError("compressing save %d failed (zlib %d)", n, zres);
        return 0;
    }
    fileLen = gcCard_RoundUp(GCCARD_DATA_OFF + compLen);

    char comment[64];
    gcCard_SlotComment(n, pData, size, comment, sizeof(comment));
    gcCard_SetHeader(buf, gcGame_Title(), comment);
    u32 hdr[4] = { GCCARD_SLOT_MAGIC, size, (u32)compLen,
                   gcCard_Checksum(buf + GCCARD_DATA_OFF, (u32)compLen, 0) };
    memcpy(buf + 0x40, hdr, sizeof(hdr));

    char name[32];
    gcCard_SlotFilename(n, name, sizeof(name));
    s32 res = gcCard_WriteAll(name, buf, fileLen);
    free(buf);
    if (res < 0) {
        printf("gcCard: writing %s failed (%d)\n", name, (int)res);
        return 0;
    }
    u32 blocks = fileLen / gcCard_sectorSize;
    if (blocks > gcCard_saveBlocksEstimate) gcCard_saveBlocksEstimate = blocks;
    printf("gcCard: %s saved (%u -> %u bytes, %u blocks)\n", name, size, (u32)compLen, blocks);
    return 1;
}

// gcDvd slot loader: fetch and decompress save slot n.
static int gcCard_LoadSlot(int n, uint8_t** ppData, uint32_t* pSize)
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
        uint8_t* out = (uint8_t*)malloc(hdr[1] ? hdr[1] : 1);
        uLongf outLen = hdr[1];
        if (out && uncompress(out, &outLen, buf + dataOff, hdr[2]) == Z_OK && outLen == hdr[1]) {
            *ppData = out;
            *pSize = hdr[1];
            ok = 1;
        }
        else free(out);
    }
    if (!ok) printf("gcCard: %s is damaged\n", name);
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
        const uint8_t* data = NULL;
        uint32_t size = 0;
        switch (gcDvd_SlotState(path, &data, &size))
        {
            case GCDVD_SLOT_ABSENT:
                if (gcCard_aSlotOnCard[n]) {
                    gcCard_NoticeBegin();
                    CARD_Delete(gcCard_slot, name);
                    gcCard_aSlotOnCard[n] = 0;
                    printf("gcCard: %s deleted\n", name);
                }
                break;
            case GCDVD_SLOT_CHANGED:
                if (gcCard_WriteSlot(n, data, size)) {
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
                    uint8_t* old = NULL;
                    uint32_t oldSize = 0;
                    if (gcCard_LoadSlot(n, &old, &oldSize))
                        gcCard_WriteSlot(n, old, oldSize);
                    free(old);
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
    *pbUsed = gcDvd_SlotState(path, NULL, NULL) != GCDVD_SLOT_ABSENT;
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
