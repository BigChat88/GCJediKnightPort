// "dvd:" devoptab: the GameCube disc's FST (GCM layout) plus a writable RAM overlay.
//
// Game assets ship on the disc as ordinary files (jk1/resource/*.gob, ...), so the
// engine can keep using fopen/stat/opendir. Works on real hardware (Swiss, ODEs)
// and in Dolphin. When the game was started from an SD card (Swiss), the disc image
// is read from the card instead of the drive.
//
// GCM header: 0x424 = FST offset, 0x428 = FST size. FST entries are 12 bytes:
//   u8 isDir, u24 nameOffset, u32 fileOffset|parentIdx, u32 fileLength|nextIdx
// followed by the name string table. Root is entry 0 (its "next" = entry count).
//
// The engine also writes next to its assets (player/<name>/..., saves,
// registry.json). Those writes land in an in-memory overlay that shadows the
// disc: lookups check the overlay first, directory listings merge both. The
// overlay is serialized to the memory card by gcCard (see gcDvd_SetWriteCallback).
//
// Lookups are case-insensitive (the engine mixes cases freely, e.g. "JK1.GOB" vs
// "jk1.gob"), which also makes fcaseopen unnecessary on this platform.

#include "gcDvd.h"
#include "gcGame.h"
#include "gcSystem.h"
#include "gcAram.h"

#include <gccore.h>
#include <ogc/dvd.h>
#include <sys/iosupport.h>
#include <ogc/lwp_watchdog.h>
#include <sys/dirent.h>
#include <dirent.h>
#include <dvm.h>
#include <sdcard/gcsd.h>
#include <sdcard/card_cmn.h>
#include <sdcard/card_io.h>
#include <ogc/semaphore.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef GC_LIBOGC2
#define DVD_ReadPrio(block, buf, len, offset, prio) DVD_ReadAbsPrio(block, buf, len, offset, prio)
#endif

#define GCDVD_MAX_PATH 256

// boot.bin game ID + maker code written by packaging/gamecube/make_gcm.py; used to
// recognize our own image on an SD card (one per game, see gcGame.c).
#define GCDVD_DISC_ID          gcGame_DiscId()
#define GCDVD_MOUNT_TIMEOUT_MS (15 * 1000)

// Small LRU cache of 32-byte aligned disc blocks. Engine reads are small (text
// parsing, GOB sub-file reads) and often interleave two streams -- e.g. a GOB's
// line reader seeking back and forth -- so a single cached block thrashes, and
// every miss is a real drive seek.
#define GCDVD_BLOCK_SIZE  (32 * 1024)
#define GCDVD_NUM_BLOCKS  (8)
#define GCDVD_DIRECT_MIN  (128 * 1024)

// Second-level cache in ARAM (the 16 MiB auxiliary RAM nothing else uses). Seeks
// dominate load times (~100 ms each), so misses fetch large 256 KiB slices: a
// level's resources sit close together in the GOBs (e.g. its materials sort
// together by name), so one slice usually serves many subsequent files.
#define GCDVD_L2_BLOCK    (256 * 1024)
#define GCDVD_L2_COUNT    (20)          // 5 MiB (+1 guard slice, see gcDvd_Mount)
// Below the read cache: the ARAM pool (gcAram.c, 8 MiB) for material pixels (up to
// ~2.6 MiB a level) and the sound bank (gcSndBank.c, ~1-4 MiB of DSP-ADPCM a
// level); what doesn't fit falls back to main RAM / the WAVs -- material pixels live there
// instead of main RAM. Together they keep the old 52+1 slice footprint: at 58 slices
// the cache overlapped ARAM that audio uses (choppy cutscene audio).
#define GCDVD_TEXPOOL_SIZE ((52 - GCDVD_L2_COUNT) * GCDVD_L2_BLOCK)

typedef struct gcDvdBlock {
    uint8_t* data;
    int64_t  offset;     // -1 = empty
    uint32_t lastUse;
} gcDvdBlock;

typedef struct gcDvdL2Slot {
    int64_t  offset;     // disc offset of the slice, -1 = empty
    uint32_t len;        // valid bytes (short at the end of the disc)
    uint32_t lastUse;
    uint32_t mask;       // which GCDVD_BLOCK_SIZE blocks of the slice are loaded
    uint32_t busy;       // transfers in flight to/from its ARAM (never evicted then)
    uint32_t aHash[GCDVD_L2_BLOCK / GCDVD_BLOCK_SIZE]; // checksum of each loaded block
} gcDvdL2Slot;

#define GCDVD_L2_FULL ((1u << (GCDVD_L2_BLOCK / GCDVD_BLOCK_SIZE)) - 1)

typedef struct gcDvdFstEntry {
    uint32_t typeName;   // [31:24] isDir, [23:0] name offset
    uint32_t offset;     // file: disc offset, dir: parent index
    uint32_t length;     // file: byte length, dir: index one past the last child
} gcDvdFstEntry;

// Overlay entry: a file or directory created at runtime. Paths are normalized
// (case preserved, compared case-insensitively, '/'-separated, relative to the disc
// root, e.g. "jk1/player/Kyle").
typedef struct gcOvNode {
    struct gcOvNode* next;
    char     path[GCDVD_MAX_PATH];
    int      isDir;
    // Contents, in pieces of GCDVD_DATA_CHUNK bytes (the last one may be smaller):
    // a save is ~380 KiB, and mid-level the heap had no single block that big.
    uint8_t** chunks;
    uint32_t numChunks;
    uint32_t lastCap;    // bytes allocated for the last piece
    uint32_t size;
    uint32_t cap;        // bytes allocated in all
    int      slot;       // >0: a save slot kept in its own memory card file (gcCard)
    int      bLazy;      // slot file whose data is still on the card (loaded on open)
    int      bChanged;   // contents changed since gcCard last persisted them
    uint32_t cardSize;   // slot: size of the copy on the card (to fall back to it)
} gcOvNode;

typedef struct gcDvdFile {
    gcOvNode* node;      // overlay file, or NULL for a disc file
    uint32_t  entry;     // FST index for disc files
    uint32_t  pos;
    int       bWritable;
    int       bAppend;
    int       bDirty;
} gcDvdFile;

typedef struct gcDvdDir {
    char      path[GCDVD_MAX_PATH];
    int       fstEntry;  // -1 if the directory only exists in the overlay
    uint32_t  fstCur;
    gcOvNode* ovCur;
    uint32_t  dots;      // "." and ".." are listed first (scandir users skip them)
} gcDvdDir;

static dvdcmdblk      gcDvd_cmd;
static gcDvdFstEntry* gcDvd_pFst = NULL;
static const char*    gcDvd_pStrings = NULL;
static uint32_t       gcDvd_numEntries = 0;
static gcDvdBlock     gcDvd_aBlocks[GCDVD_NUM_BLOCKS];
static uint32_t       gcDvd_useCounter = 0;
static char           gcDvd_cwd[GCDVD_MAX_PATH] = "";
static int            gcDvd_bMounted = 0;

static gcDvdL2Slot    gcDvd_aL2[GCDVD_L2_COUNT];
static uint32_t       gcDvd_l2Aram = 0;     // ARAM address of slot 0; 0 = no L2
static uint8_t*       gcDvd_pL2Staging = NULL;
static uint32_t       gcDvd_l2UseCounter = 0;
static uint32_t       gcDvd_discEnd = 0;    // end of the last file (reads stop here)
static u32            gcDvd_aArStack[4];

static gcOvNode*      gcOv_pHead = NULL;
static gcOvNode*      gcOv_pTail = NULL;
static void         (*gcDvd_writeCallback)(void) = NULL;

static inline int gcDvd_IsDir(uint32_t idx)
{
    return (gcDvd_pFst[idx].typeName >> 24) != 0;
}

static inline const char* gcDvd_Name(uint32_t idx)
{
    return gcDvd_pStrings + (gcDvd_pFst[idx].typeName & 0xFFFFFF);
}

// --- Disc reads ------------------------------------------------------------------

// The "disc" is either the real drive or, when booted from an SD card through
// Swiss, the disc image file itself (Swiss's DVD emulation only hooks the official
// SDK's drive routines, not libogc's). libogc2 core: the card is mounted with
// libdvm, FAT or exFAT alike (the libogc core has its own exFAT reader, gcExfat),
// and the image is read as a file. The main and music threads share it under a
// mutex.
enum { GCDVD_SRC_DRIVE, GCDVD_SRC_SD };
static int     gcDvd_source = GCDVD_SRC_DRIVE;
static FILE*   gcDvd_pImage = NULL;
static char    gcDvd_imagePath[256] = "";
static mutex_t gcDvd_imageMutex;

static int     gcDvd_sdDrive = -1;     // sdgecko channel of the card holding the image
static int     gcDvd_bSdFast = 0;      // running it at 32 MHz
static char    gcDvd_szSdSpeed[96] = "not measured";

const char* gcDvd_GetSdInfo(char* buf, int len)
{
    if (gcDvd_source == GCDVD_SRC_DRIVE) return NULL;
    snprintf(buf, len, "Game image: %s\nSD speed:   %s\n", gcDvd_imagePath, gcDvd_szSdSpeed);
    return buf;
}

void gcDvd_PauseReads(void)
{
    LWP_MutexLock(gcDvd_imageMutex);
}

void gcDvd_ResumeReads(void)
{
    LWP_MutexUnlock(gcDvd_imageMutex);
}

volatile uint32_t gcDvd_statReadErrors = 0; // SD read errors (each retry counts)

static int gcDvd_ReadFile(void* dst, uint32_t len, int64_t offset)
{
    clearerr(gcDvd_pImage);
    int ok = !fseeko(gcDvd_pImage, (off_t)offset, SEEK_SET);
    size_t got = ok ? fread(dst, 1, len, gcDvd_pImage) : 0;
    // Reads are rounded up for the drive; past the end of the image is just zeroes.
    if (got < len) memset((uint8_t*)dst + got, 0, len - got);
    return ok && (got > 0 || len == 0) && !ferror(gcDvd_pImage);
}

static int gcDvd_ReadImage(void* dst, uint32_t len, int64_t offset)
{
    LWP_MutexLock(gcDvd_imageMutex);
    int ok = gcDvd_ReadFile(dst, len, offset);
    if (!ok && gcDvd_bSdFast) {
        // Not every card copes with 32 MHz for long; drop back and retry.
        gcDvd_bSdFast = 0;
        sdgecko_setSpeed(gcDvd_sdDrive, EXI_SPEED16MHZ);
        printf("gcDvd: SD read error at 32 MHz, back to 16 MHz\n");
        ok = gcDvd_ReadFile(dst, len, offset);
    }
    // A failed read used to end up as an early end of file in the engine (a
    // level failing to parse at a valid line, a bitmap not loading). Transient
    // SD errors happen: give the card a moment and try again.
    for (int attempt = 0; !ok && attempt < 4; attempt++) {
        gcDvd_statReadErrors++;
        printf("gcDvd: SD read error at 0x%llx (+%u), retrying\n", (long long)offset, len);
        usleep(20 * 1000);
        ok = gcDvd_ReadFile(dst, len, offset);
    }
    if (!ok) gcDvd_statReadErrors++;
    LWP_MutexUnlock(gcDvd_imageMutex);
    return ok;
}

// Raw read. For the drive, DVD_ReadPrio wants a 32-byte aligned buffer, a length
// that is a multiple of 32 and a 4-byte aligned disc offset.
static volatile uint64_t gcDvd_statMissUsInternal = 0;
static int gcDvd_ReadRaw(void* dst, uint32_t len, int64_t offset)
{
    u64 t0 = gettime();
    int ok;
    if (gcDvd_source != GCDVD_SRC_DRIVE)
        ok = gcDvd_ReadImage(dst, len, offset);
    else {
        DCInvalidateRange(dst, len);
        ok = DVD_ReadPrio(&gcDvd_cmd, dst, len, offset, 2) >= 0;
    }
    gcDvd_statMissUsInternal += ticks_to_microsecs(diff_ticks(t0, gettime()));
    return ok;
}

static void gcDvd_AramDma(u32 dir, void* mem, u32 aram, u32 len)
{
    ARQRequest req;
    if (dir == ARQ_MRAMTOARAM) DCFlushRange(mem, len);
    else DCInvalidateRange(mem, len);
    // Synchronous: returns once the transfer is done. Low priority: ARQ moves those
    // in chunks and serves AESND's small high-priority refills in between. At high
    // priority a 256 KiB transfer is one DMA that holds the audio up for
    // milliseconds, and AESND (which stages every voice through one shared buffer)
    // then plays stale or another voice's data.
    ARQ_PostRequest(&req, 0x4A4B, dir, ARQ_PRIO_LO, aram, (u32)MEM_VIRTUAL_TO_PHYSICAL(mem), len);
}

// --- Read-ahead ----------------------------------------------------------------------
//
// Cutscenes (and level loads, thanks to the GOB ordering) read the disc
// sequentially. Reading from an SD card is slow and keeps the CPU busy (the EXI SD
// driver is polled), so a miss stalls the caller for a long time: cutscene audio
// runs dry and the video falls behind. When the main thread moves on to the next
// slice, a low-priority thread loads the following GCDVD_PREFETCH_AHEAD slices
// into ARAM while the main thread waits for vsync.

#define GCDVD_PREFETCH_AHEAD  2
#define GCDVD_PREFETCH_PIECE  (32 * 1024)   // bounds how long the game waits for the SD (~17 ms)
#define GCDVD_PREFETCH_PRIO   40            // below the main thread and the music

static mutex_t           gcDvd_l2Mutex = LWP_MUTEX_NULL;   // gcDvd_aL2 + use counter
static sem_t             gcDvd_prefetchSem;
static lwp_t             gcDvd_prefetchThread = LWP_THREAD_NULL;
static uint8_t           gcDvd_prefetchStack[16 * 1024] __attribute__((aligned(32)));
static uint8_t*          gcDvd_pPrefetchStaging = NULL;
static dvdcmdblk         gcDvd_prefetchCmd;
static volatile int64_t  gcDvd_prefetchFrom = -1;   // first slice wanted ahead
static volatile int64_t  gcDvd_prefetchBusy = -1;   // slice being loaded
static int64_t           gcDvd_lastSlice = -1;

static int gcDvd_ReadRawCmd(void* dst, uint32_t len, int64_t offset, dvdcmdblk* pCmd)
{
    if (gcDvd_source != GCDVD_SRC_DRIVE)
        return gcDvd_ReadImage(dst, len, offset);
    DCInvalidateRange(dst, len);
    return DVD_ReadPrio(pCmd, dst, len, offset, 2) >= 0;
}

static uint32_t gcDvd_SliceLen(int64_t slice)
{
    uint32_t len = GCDVD_L2_BLOCK;
    if (gcDvd_discEnd && slice + len > gcDvd_discEnd)
        len = (uint32_t)(((gcDvd_discEnd - slice) + GCDVD_BLOCK_SIZE - 1) & ~(GCDVD_BLOCK_SIZE - 1));
    return len;
}

// Callers hold gcDvd_l2Mutex.
// Every block put in ARAM gets a checksum, checked when it comes back: on real
// hardware, after a level or two, some ARAM reads returned wrong data (levels that
// failed to parse at a valid line, menu backgrounds that didn't load) while
// Dolphin never showed it. A mismatching block is dropped and read again from the
// medium, and counted (stats panel: "aram bad").
volatile uint32_t gcDvd_statAramBad = 0;
volatile int32_t  gcDvd_statAramBadSlot = -1;

static uint32_t gcDvd_Hash(const void* p)
{
    const uint32_t* w = (const uint32_t*)p;
    uint32_t a = 0x9E3779B9u, b = 0x7F4A7C15u;
    for (uint32_t i = 0; i < GCDVD_BLOCK_SIZE / 4; i += 2) {
        a = (a ^ w[i]) * 0x01000193u;
        b = (b + w[i + 1]) * 0x85EBCA6Bu;
    }
    return a ^ (b >> 7);
}

// The victim is the least recently used slot with no transfer in flight: the
// main thread and the read-ahead thread fill slots outside the mutex (SD reads are
// slow), and a slot reclaimed meanwhile for another slice would get the first
// transfer's data under the second slice's valid mask (corrupt level data on
// hardware once the cache had filled up). NULL if every slot is busy.
static gcDvdL2Slot* gcDvd_L2Find(int64_t slice, gcDvdL2Slot** ppVictim)
{
    gcDvdL2Slot* victim = NULL;
    for (int i = 0; i < GCDVD_L2_COUNT; i++) {
        if (gcDvd_aL2[i].offset == slice) return &gcDvd_aL2[i];
        if (!gcDvd_aL2[i].busy && (!victim || gcDvd_aL2[i].lastUse < victim->lastUse)) victim = &gcDvd_aL2[i];
    }
    if (ppVictim) *ppVictim = victim;
    return NULL;
}

static void gcDvd_L2Release(gcDvdL2Slot* slot)
{
    LWP_MutexLock(gcDvd_l2Mutex);
    slot->busy--;
    LWP_MutexUnlock(gcDvd_l2Mutex);
}

// Main thread touched `slice`: if it is reading forward, queue the next ones.
static const devoptab_t gcDvd_devoptab;
static volatile int gcDvd_bSeqPending = 0;

static void gcDvd_NoteSlice(int64_t slice)
{
    if (!gcDvd_pPrefetchStaging) return;
    if (slice == gcDvd_lastSlice + GCDVD_L2_BLOCK) {
        gcDvd_prefetchFrom = slice + GCDVD_L2_BLOCK;
        gcDvd_bSeqPending = 1;
        LWP_SemPost(gcDvd_prefetchSem);
    }
    gcDvd_lastSlice = slice;
}

// Hints: disc ranges the game will want soon (e.g. the current level's sounds,
// which the engine only reads the first time they play). One byte per ARAM slice,
// one bit per block in it; the read-ahead thread loads the wanted blocks of each
// slice with a single read, in disc order, whenever it has nothing else to do, so
// the first gunshot doesn't stall the game on the disc.
static uint8_t*          gcDvd_pHintMap = NULL;
static uint32_t          gcDvd_hintMapLen = 0;
static uint32_t*         gcDvd_pHintOrder = NULL;  // slices in the order first hinted
static volatile uint32_t gcDvd_hintOrderW = 0, gcDvd_hintOrderR = 0;
#define GCDVD_HINT_ORDER_MAX 4096

static void gcDvd_PrefetchSequential(int64_t from)
{
    for (int k = 0; k < GCDVD_PREFETCH_AHEAD; k++)
    {
        int64_t slice = from + (int64_t)k * GCDVD_L2_BLOCK;
        if (gcDvd_discEnd && slice >= gcDvd_discEnd) break;

        LWP_MutexLock(gcDvd_l2Mutex);
        gcDvdL2Slot* victim = NULL;
        if (gcDvd_L2Find(slice, &victim) || !victim) {
            LWP_MutexUnlock(gcDvd_l2Mutex);
            continue;
        }
        // Reserve the slot: invalid while loading, and busy so it can't be evicted.
        victim->offset = -1;
        victim->lastUse = ++gcDvd_l2UseCounter;
        victim->busy++;
        gcDvd_prefetchBusy = slice;
        LWP_MutexUnlock(gcDvd_l2Mutex);

        uint32_t len = gcDvd_SliceLen(slice);
        int ok = 1;
        for (uint32_t done = 0; ok && done < len; done += GCDVD_PREFETCH_PIECE) {
            uint32_t n = len - done < GCDVD_PREFETCH_PIECE ? len - done : GCDVD_PREFETCH_PIECE;
            ok = gcDvd_ReadRawCmd(gcDvd_pPrefetchStaging + done, n, slice + done, &gcDvd_prefetchCmd);
        }
        uint32_t aHash[GCDVD_L2_BLOCK / GCDVD_BLOCK_SIZE];
        if (ok) {
            for (uint32_t b = 0; b * GCDVD_BLOCK_SIZE < len; b++)
                aHash[b] = gcDvd_Hash(gcDvd_pPrefetchStaging + b * GCDVD_BLOCK_SIZE);
            gcDvd_AramDma(ARQ_MRAMTOARAM, gcDvd_pPrefetchStaging,
                          gcDvd_l2Aram + (u32)(victim - gcDvd_aL2) * GCDVD_L2_BLOCK, len);
        }

        LWP_MutexLock(gcDvd_l2Mutex);
        if (ok) {
            victim->offset = slice;
            victim->len = len;
            for (uint32_t b = 0; b * GCDVD_BLOCK_SIZE < len; b++)
                victim->aHash[b] = aHash[b];
            victim->mask = GCDVD_L2_FULL;
        }
        victim->busy--;
        gcDvd_prefetchBusy = -1;
        LWP_MutexUnlock(gcDvd_l2Mutex);

        // The reader jumped elsewhere: follow it instead.
        if (gcDvd_bSeqPending) break;
    }
}

// Load the blocks of slice `idx` flagged in `want` that aren't in ARAM yet, with
// one read from the first to the last of them.
static void gcDvd_PrefetchHinted(uint32_t idx, uint32_t want)
{
    int64_t slice = (int64_t)idx * GCDVD_L2_BLOCK;
    if (gcDvd_discEnd && slice >= gcDvd_discEnd) return;

    LWP_MutexLock(gcDvd_l2Mutex);
    gcDvdL2Slot* victim = NULL;
    gcDvdL2Slot* slot = gcDvd_L2Find(slice, &victim);
    if (slot) want &= ~slot->mask;
    if (!want || (!slot && !victim)) {
        LWP_MutexUnlock(gcDvd_l2Mutex);
        return;
    }
    if (!slot) {
        slot = victim;
        slot->offset = slice;
        slot->len = gcDvd_SliceLen(slice);
        slot->mask = 0;
    }
    slot->lastUse = ++gcDvd_l2UseCounter;
    slot->busy++;
    LWP_MutexUnlock(gcDvd_l2Mutex);

    int first = __builtin_ctz(want);
    int last = 31 - __builtin_clz(want);
    uint32_t off = first * GCDVD_BLOCK_SIZE;
    uint32_t len = (last - first + 1) * GCDVD_BLOCK_SIZE;
    if (off >= slot->len) { gcDvd_L2Release(slot); return; }
    if (off + len > slot->len) len = slot->len - off;
    for (uint32_t done = 0; done < len; done += GCDVD_PREFETCH_PIECE) {
        uint32_t n = len - done < GCDVD_PREFETCH_PIECE ? len - done : GCDVD_PREFETCH_PIECE;
        if (!gcDvd_ReadRawCmd(gcDvd_pPrefetchStaging + done, n, slice + off + done, &gcDvd_prefetchCmd)) {
            gcDvd_L2Release(slot);
            return;
        }
    }
    // Only whole blocks are marked loaded (a short last block, at the end of the
    // disc, is simply not cached).
    uint32_t aHash[GCDVD_L2_BLOCK / GCDVD_BLOCK_SIZE];
    uint32_t got = 0;
    for (int b = first; b <= last; b++) {
        uint32_t at = (uint32_t)(b - first) * GCDVD_BLOCK_SIZE;
        if (at + GCDVD_BLOCK_SIZE > len) break;
        aHash[b] = gcDvd_Hash(gcDvd_pPrefetchStaging + at);
        got |= 1u << b;
    }
    gcDvd_AramDma(ARQ_MRAMTOARAM, gcDvd_pPrefetchStaging,
                  gcDvd_l2Aram + (u32)(slot - gcDvd_aL2) * GCDVD_L2_BLOCK + off, len);
    LWP_MutexLock(gcDvd_l2Mutex);
    if (slot->offset == slice) {
        for (int b = first; b <= last; b++)
            if (got & (1u << b)) slot->aHash[b] = aHash[b];
        slot->mask |= got;
    }
    slot->busy--;
    LWP_MutexUnlock(gcDvd_l2Mutex);
}

// Next slice with hinted blocks, in the order they were first hinted (a level's
// common sounds, weapons and impacts, come first); -1 if none.
// Hints are background work: stay out of the game's way while it is reading.
static volatile u64 gcDvd_lastGameReadTicks = 0;
#define GCDVD_HINT_IDLE_MS 150

static void gcDvd_WaitGameIdle(void)
{
    while (diff_msec(gcDvd_lastGameReadTicks, gettime()) < GCDVD_HINT_IDLE_MS && !gcDvd_bSeqPending)
        usleep(20 * 1000);
}

static int gcDvd_NextHint(uint32_t* pWant)
{
    while (gcDvd_pHintOrder && gcDvd_hintOrderR != gcDvd_hintOrderW) {
        uint32_t i = gcDvd_pHintOrder[gcDvd_hintOrderR % GCDVD_HINT_ORDER_MAX];
        gcDvd_hintOrderR++;
        u32 level = IRQ_Disable();
        *pWant = gcDvd_pHintMap[i];
        gcDvd_pHintMap[i] = 0;
        IRQ_Restore(level);
        if (*pWant) return (int)i;
    }
    return -1;
}

static void* gcDvd_PrefetchMain(void* arg)
{
    while (1)
    {
        LWP_SemWait(gcDvd_prefetchSem);
        while (1)
        {
            if (gcDvd_bSeqPending) {
                gcDvd_bSeqPending = 0;
                gcDvd_PrefetchSequential(gcDvd_prefetchFrom);
                continue;
            }
            uint32_t want;
            if (gcDvd_hintOrderR != gcDvd_hintOrderW) {
                gcDvd_WaitGameIdle();
                if (gcDvd_bSeqPending) continue;
            }
            int idx = gcDvd_NextHint(&want);
            if (idx >= 0) {
                gcDvd_PrefetchHinted((uint32_t)idx, want);
                continue;
            }
            break;
        }
    }
    return NULL;
}

void gcDvd_HintFileRange(FILE* fp, uint32_t offset, uint32_t len)
{
    // SD cards only: with a disc drive these reads would compete with the game's
    // own for the drive (seeks), and cause the very stalls they are meant to avoid.
    if (!fp || !len || !gcDvd_pHintMap   || gcDvd_source == GCDVD_SRC_DRIVE) return;
    int fd = fileno(fp);
    __handle* h = fd >= 0 ? __get_handle(fd) : NULL;
    if (!h || devoptab_list[h->device] != &gcDvd_devoptab) return;
    gcDvdFile* f = (gcDvdFile*)h->fileStruct;
    if (!f || f->node) return; // overlay files live in RAM already
    const gcDvdFstEntry* e = &gcDvd_pFst[f->entry];
    if (offset >= e->length) return;
    if (len > e->length - offset) len = e->length - offset;

    uint32_t firstBlock = (e->offset + offset) / GCDVD_BLOCK_SIZE;
    uint32_t lastBlock = (e->offset + offset + len - 1) / GCDVD_BLOCK_SIZE;
    const uint32_t perSlice = GCDVD_L2_BLOCK / GCDVD_BLOCK_SIZE;
    for (uint32_t blk = firstBlock; blk <= lastBlock; blk++) {
        uint32_t idx = blk / perSlice;
        if (idx >= gcDvd_hintMapLen) break;
        u32 level = IRQ_Disable();
        if (!gcDvd_pHintMap[idx] && gcDvd_hintOrderW - gcDvd_hintOrderR < GCDVD_HINT_ORDER_MAX)
            gcDvd_pHintOrder[gcDvd_hintOrderW++ % GCDVD_HINT_ORDER_MAX] = idx;
        gcDvd_pHintMap[idx] |= (uint8_t)(1u << (blk % perSlice));
        IRQ_Restore(level);
    }
    LWP_SemPost(gcDvd_prefetchSem);
}

void gcDvd_DrainHints(uint32_t maxMs)
{
    if (gcDvd_source == GCDVD_SRC_DRIVE || !gcDvd_pHintOrder) return;
    u64 t0 = gettime();
    uint32_t pending = gcDvd_hintOrderW - gcDvd_hintOrderR;
    LWP_SemPost(gcDvd_prefetchSem);
    // The prefetch thread only works while the game isn't reading (see
    // gcDvd_WaitGameIdle); sleeping here lets it run flat out.
    while (gcDvd_hintOrderR != gcDvd_hintOrderW && diff_msec(t0, gettime()) < maxMs)
        usleep(10 * 1000);
    gcDvd_prefDone = pending - (gcDvd_hintOrderW - gcDvd_hintOrderR);
    gcDvd_prefLeft = gcDvd_hintOrderW - gcDvd_hintOrderR;
    printf("gcDvd: prefetched %u hinted slices in %u ms (%u left)\n",
           pending - (gcDvd_hintOrderW - gcDvd_hintOrderR), (unsigned)diff_msec(t0, gettime()),
           gcDvd_hintOrderW - gcDvd_hintOrderR);
}

static void gcDvd_StartPrefetch(void)
{
    gcDvd_pPrefetchStaging = (uint8_t*)memalign(32, GCDVD_L2_BLOCK);
    if (!gcDvd_pPrefetchStaging) return;
    gcDvd_hintMapLen = gcDvd_discEnd / GCDVD_L2_BLOCK + 1;
    gcDvd_pHintMap = (uint8_t*)calloc(gcDvd_hintMapLen, 1);
    gcDvd_pHintOrder = (uint32_t*)malloc(GCDVD_HINT_ORDER_MAX * sizeof(uint32_t));
    if (!gcDvd_pHintOrder) { free(gcDvd_pHintMap); gcDvd_pHintMap = NULL; }
    LWP_SemInit(&gcDvd_prefetchSem, 0, 1);
    LWP_CreateThread(&gcDvd_prefetchThread, gcDvd_PrefetchMain, NULL, gcDvd_prefetchStack,
                     sizeof(gcDvd_prefetchStack), GCDVD_PREFETCH_PRIO);
}

// Fill one L1 block (GCDVD_BLOCK_SIZE at blockStart) through the ARAM cache.
static int gcDvd_FillBlock(uint8_t* pDst, int64_t blockStart)
{
    if (!gcDvd_l2Aram)
        return gcDvd_ReadRaw(pDst, GCDVD_BLOCK_SIZE, blockStart);

    int64_t slice = blockStart & ~(int64_t)(GCDVD_L2_BLOCK - 1);
    uint32_t sub = (uint32_t)(blockStart - slice);

    LWP_MutexLock(gcDvd_l2Mutex);
    // Being read ahead right now: wait for it rather than reading it twice.
    while (gcDvd_prefetchBusy == slice) {
        LWP_MutexUnlock(gcDvd_l2Mutex);
        usleep(1000);
        LWP_MutexLock(gcDvd_l2Mutex);
    }
    gcDvdL2Slot* victim = NULL;
    gcDvdL2Slot* slot = gcDvd_L2Find(slice, &victim);

    uint32_t bit = 1u << (sub / GCDVD_BLOCK_SIZE);
    if (slot && sub < slot->len && (slot->mask & bit)) {
        slot->lastUse = ++gcDvd_l2UseCounter;
        slot->busy++;
        LWP_MutexUnlock(gcDvd_l2Mutex);
        gcDvd_aStatCount[GCDVD_STAT_L2HIT]++;
        gcDvd_NoteSlice(slice);
        u32 aram = gcDvd_l2Aram + (u32)(slot - gcDvd_aL2) * GCDVD_L2_BLOCK + sub;
        gcDvd_AramDma(ARQ_ARAMTOMRAM, pDst, aram, GCDVD_BLOCK_SIZE);
        uint32_t want = slot->aHash[sub / GCDVD_BLOCK_SIZE];
        if (gcDvd_Hash(pDst) != want) {
            gcDvd_statAramBad++;
            gcDvd_statAramBadSlot = (int32_t)(slot - gcDvd_aL2);
            printf("gcDvd: ARAM block check failed at 0x%llx (slot %d), rereading\n",
                   (long long)blockStart, (int)(slot - gcDvd_aL2));
            LWP_MutexLock(gcDvd_l2Mutex);
            if (slot->offset == slice) slot->mask &= ~bit;
            slot->busy--;
            LWP_MutexUnlock(gcDvd_l2Mutex);
            return gcDvd_ReadRaw(pDst, GCDVD_BLOCK_SIZE, blockStart);
        }
        gcDvd_L2Release(slot);
#ifdef GC_DVD_VERIFY
        // Dev builds: cross-check ARAM hits against the disc.
        if (gcDvd_ReadRaw(gcDvd_pL2Staging, GCDVD_BLOCK_SIZE, blockStart)
            && memcmp(gcDvd_pL2Staging, pDst, GCDVD_BLOCK_SIZE)) {
            printf("gcDvd: ARAM MISMATCH at 0x%llx (slice 0x%llx slot %d)\n",
                   (long long)blockStart, (long long)slice, (int)(slot - gcDvd_aL2));
            slot->offset = -1; // staging was clobbered by the check; drop the slice
        }
#endif
        return 1;
    }

    uint32_t len = gcDvd_SliceLen(slice);
    if (len <= sub || (!slot && !victim)) {
        LWP_MutexUnlock(gcDvd_l2Mutex);
        return gcDvd_ReadRaw(pDst, GCDVD_BLOCK_SIZE, blockStart);
    }

    // SD card: no seek cost, but it only reads ~2 MB/s, so a whole slice would
    // stall the game for >100 ms to fetch a small file (a sound on the first shot,
    // the textures of a new area). Read just this block and remember it in the
    // slice; sequential reads still get whole slices from the read-ahead thread.
    if (gcDvd_source != GCDVD_SRC_DRIVE) {
        if (!slot) {
            slot = victim;
            slot->offset = slice;
            slot->len = len;
            slot->mask = 0;
        }
        slot->lastUse = ++gcDvd_l2UseCounter;
        slot->busy++;
        LWP_MutexUnlock(gcDvd_l2Mutex);

        if (!gcDvd_ReadRaw(pDst, GCDVD_BLOCK_SIZE, blockStart)) {
            gcDvd_L2Release(slot);
            return 0;
        }
        uint32_t hash = gcDvd_Hash(pDst);
        gcDvd_AramDma(ARQ_MRAMTOARAM, pDst,
                      gcDvd_l2Aram + (u32)(slot - gcDvd_aL2) * GCDVD_L2_BLOCK + sub, GCDVD_BLOCK_SIZE);
        LWP_MutexLock(gcDvd_l2Mutex);
        if (slot->offset == slice) {
            slot->aHash[sub / GCDVD_BLOCK_SIZE] = hash;
            slot->mask |= bit;
        }
        slot->busy--;
        LWP_MutexUnlock(gcDvd_l2Mutex);
        gcDvd_NoteSlice(slice);
        return 1;
    }

    // Disc drive: seeks dominate, so read the whole slice and keep it in ARAM.
    if (!victim) { // partly loaded here and nothing else is free: read past the cache
        LWP_MutexUnlock(gcDvd_l2Mutex);
        return gcDvd_ReadRaw(pDst, GCDVD_BLOCK_SIZE, blockStart);
    }
    slot = victim;
    slot->offset = -1;                        // reserved while it loads
    slot->lastUse = ++gcDvd_l2UseCounter;
    slot->busy++;
    LWP_MutexUnlock(gcDvd_l2Mutex);

    if (!gcDvd_ReadRaw(gcDvd_pL2Staging, len, slice)) {
        gcDvd_L2Release(slot);
        return 0;
    }
    uint32_t aHash[GCDVD_L2_BLOCK / GCDVD_BLOCK_SIZE];
    for (uint32_t b = 0; b * GCDVD_BLOCK_SIZE < len; b++)
        aHash[b] = gcDvd_Hash(gcDvd_pL2Staging + b * GCDVD_BLOCK_SIZE);
    gcDvd_AramDma(ARQ_MRAMTOARAM, gcDvd_pL2Staging,
                  gcDvd_l2Aram + (u32)(slot - gcDvd_aL2) * GCDVD_L2_BLOCK, len);

    LWP_MutexLock(gcDvd_l2Mutex);
    slot->offset = slice;
    slot->len = len;
    for (uint32_t b = 0; b * GCDVD_BLOCK_SIZE < len; b++)
        slot->aHash[b] = aHash[b];
    slot->mask = GCDVD_L2_FULL;
    slot->busy--;
    LWP_MutexUnlock(gcDvd_l2Mutex);

    gcDvd_NoteSlice(slice);
    memcpy(pDst, gcDvd_pL2Staging + sub, GCDVD_BLOCK_SIZE);
    return 1;
}

volatile uint32_t gcDvd_aStatCount[GCDVD_STAT_NUM];
volatile uint64_t gcDvd_statReadUs = 0;   // time the engine spent in disc reads
volatile uint64_t gcDvd_statMissUs = 0;   // ... of which actually reading the medium
char gcDvd_szLastOpen[40] = "";
char gcDvd_szSlow[40] = "";
volatile uint32_t gcDvd_slowMs = 0;
uint32_t gcDvd_prefDone = 0, gcDvd_prefLeft = 0;

static int gcDvd_ReadUntimed(void* dst, uint32_t len, int64_t offset);
static int gcDvd_Read(void* dst, uint32_t len, int64_t offset)
{
    u64 t0 = gettime();
    gcDvd_aStatCount[GCDVD_STAT_READS]++;
    gcDvd_aStatCount[GCDVD_STAT_KB] += len >> 10;
    int ok = gcDvd_ReadUntimed(dst, len, offset);
    gcDvd_lastGameReadTicks = gettime();
    uint64_t us = ticks_to_microsecs(diff_ticks(t0, gettime()));
    gcDvd_statReadUs += us;
    gcDvd_statMissUs = gcDvd_statMissUsInternal;
    if (us > 20000 && gcSystem_IsMainThread()) {
        memcpy(gcDvd_szSlow, gcDvd_szLastOpen, sizeof(gcDvd_szSlow));
        gcDvd_slowMs = (uint32_t)(us / 1000);
    }
    return ok;
}

static int gcDvd_ReadUntimed(void* dst, uint32_t len, int64_t offset)
{
    uint8_t* out = (uint8_t*)dst;
    while (len)
    {
        // Large aligned spans go straight into the destination buffer. The bar is
        // well above a block: newlib refills its FILE buffers in 32 KiB chunks, and
        // those must go through the cache (the engine seeks back and forth a lot).
        if (!((uintptr_t)out & 31) && !(offset & 3) && len >= GCDVD_DIRECT_MIN)
        {
            uint32_t direct = len & ~31u;
            gcDvd_aStatCount[GCDVD_STAT_DIRECT]++;
            if (!gcDvd_ReadRaw(out, direct, offset)) return 0;
            out += direct; offset += direct; len -= direct;
            continue;
        }

        int64_t blockStart = offset & ~(int64_t)(GCDVD_BLOCK_SIZE - 1);
        gcDvdBlock* blk = NULL;
        gcDvdBlock* victim = &gcDvd_aBlocks[0];
        for (int i = 0; i < GCDVD_NUM_BLOCKS; i++) {
            if (gcDvd_aBlocks[i].offset == blockStart) { blk = &gcDvd_aBlocks[i]; break; }
            if (gcDvd_aBlocks[i].lastUse < victim->lastUse) victim = &gcDvd_aBlocks[i];
        }
        if (!blk)
        {
            gcDvd_aStatCount[GCDVD_STAT_L1MISS]++;
            blk = victim;
            if (!gcDvd_FillBlock(blk->data, blockStart)) {
                blk->offset = -1;
                return 0;
            }
            blk->offset = blockStart;
        }
        blk->lastUse = ++gcDvd_useCounter;

        uint32_t inBlock = (uint32_t)(offset - blockStart);
        uint32_t n = GCDVD_BLOCK_SIZE - inBlock;
        if (n > len) n = len;
        memcpy(out, blk->data + inBlock, n);
        out += n; offset += n; len -= n;
    }
    return 1;
}

// --- Paths -----------------------------------------------------------------------

// Normalize "dvd:/a/B", "/a/b", "b\\c" (relative to the cwd), "." and ".." into
// "a/b/c" form: no device, no leading slash. Returns 0 if too long.
static int gcDvd_Normalize(const char* path, char* out, size_t outSz)
{
    const char* colon = strchr(path, ':');
    if (colon) path = colon + 1;

    char joined[GCDVD_MAX_PATH * 2];
    if (*path == '/' || *path == '\\' || !gcDvd_cwd[0])
        snprintf(joined, sizeof(joined), "%s", path);
    else
        snprintf(joined, sizeof(joined), "%s/%s", gcDvd_cwd, path);

    size_t len = 0;
    out[0] = 0;
    const char* p = joined;
    while (*p)
    {
        while (*p == '/' || *p == '\\') p++;
        if (!*p) break;
        size_t n = 0;
        while (p[n] && p[n] != '/' && p[n] != '\\') n++;

        if (n == 1 && p[0] == '.') {
            // skip
        }
        else if (n == 2 && p[0] == '.' && p[1] == '.') {
            char* slash = strrchr(out, '/');
            len = slash ? (size_t)(slash - out) : 0;
            out[len] = 0;
        }
        else {
            if (len + n + 2 > outSz) return 0;
            if (len) out[len++] = '/';
            for (size_t i = 0; i < n; i++) out[len++] = p[i];
            out[len] = 0;
        }
        p += n;
    }
    return 1;
}

// FST entry for a normalized path, or -1.
static int gcDvd_FstLookup(const char* norm)
{
    if (!gcDvd_bMounted) return -1;

    uint32_t cur = 0;
    const char* p = norm;
    char comp[GCDVD_MAX_PATH];
    while (*p)
    {
        size_t n = 0;
        while (p[n] && p[n] != '/') n++;
        memcpy(comp, p, n);
        comp[n] = 0;
        p += n;
        if (*p == '/') p++;

        if (!gcDvd_IsDir(cur)) return -1;

        // Children of a directory occupy [cur+1, next); subdirectories are skipped
        // over as a whole by jumping to their own "next" index.
        uint32_t end = gcDvd_pFst[cur].length;
        uint32_t i = cur + 1;
        int found = -1;
        while (i < end)
        {
            if (!strcasecmp(gcDvd_Name(i), comp)) { found = (int)i; break; }
            i = gcDvd_IsDir(i) ? gcDvd_pFst[i].length : i + 1;
        }
        if (found < 0) return -1;
        cur = (uint32_t)found;
    }
    return (int)cur;
}

static void gcDvd_FillFstStat(uint32_t idx, struct stat* st)
{
    memset(st, 0, sizeof(*st));
    st->st_ino = idx;
    if (gcDvd_IsDir(idx)) {
        st->st_mode = S_IFDIR | 0555;
        st->st_nlink = 2;
    }
    else {
        st->st_mode = S_IFREG | 0444;
        st->st_nlink = 1;
        st->st_size = gcDvd_pFst[idx].length;
        st->st_blksize = 2048;
        st->st_blocks = (st->st_size + 511) / 512;
    }
}

// --- Overlay -----------------------------------------------------------------------

static gcOvNode* gcDvd_pFailedNode = NULL; // see gcDvd_GcTakeWriteFailure

static void gcOv_FreeData(gcOvNode* n)
{
    for (uint32_t i = 0; i < n->numChunks; i++)
        free(n->chunks[i]);
    free(n->chunks);
    n->chunks = NULL;
    n->numChunks = 0;
    n->lastCap = 0;
    n->cap = 0;
}

static gcOvNode* gcOv_Find(const char* norm)
{
    for (gcOvNode* n = gcOv_pHead; n; n = n->next)
        if (!strcasecmp(n->path, norm)) return n;
    return NULL;
}

static gcOvNode* gcOv_Create(const char* norm, int isDir)
{
    gcOvNode* n = (gcOvNode*)calloc(1, sizeof(gcOvNode));
    if (!n) return NULL;
    strncpy(n->path, norm, sizeof(n->path) - 1);
    n->isDir = isDir;
    // Append, so parents always precede their children (serialization relies on it).
    if (gcOv_pTail) gcOv_pTail->next = n; else gcOv_pHead = n;
    gcOv_pTail = n;
    return n;
}

static void gcOv_Remove(gcOvNode* node)
{
    gcOvNode* prev = NULL;
    if (gcDvd_pFailedNode == node) gcDvd_pFailedNode = NULL;
    for (gcOvNode* n = gcOv_pHead; n; prev = n, n = n->next) {
        if (n != node) continue;
        if (prev) prev->next = n->next; else gcOv_pHead = n->next;
        if (gcOv_pTail == n) gcOv_pTail = prev;
        gcOv_FreeData(n);
        free(n);
        return;
    }
}

// Room for size bytes: the last piece grows (doubling, up to a whole piece),
// then whole new pieces are added. No block is ever bigger than a piece.
static int gcOv_Reserve(gcOvNode* n, uint32_t size)
{
    while (n->cap < size) {
        uint32_t want = size - n->cap;
        if (n->numChunks && n->lastCap < GCDVD_DATA_CHUNK) {
            uint32_t nc = n->lastCap * 2;
            while (nc < n->lastCap + want && nc < GCDVD_DATA_CHUNK) nc *= 2;
            if (nc > GCDVD_DATA_CHUNK) nc = GCDVD_DATA_CHUNK;
            uint8_t* c = (uint8_t*)realloc(n->chunks[n->numChunks - 1], nc);
            if (!c) return 0;
            n->chunks[n->numChunks - 1] = c;
            n->cap += nc - n->lastCap;
            n->lastCap = nc;
        }
        else {
            uint32_t nc = 1024;
            while (nc < want && nc < GCDVD_DATA_CHUNK) nc *= 2;
            uint8_t** arr = (uint8_t**)realloc(n->chunks, (n->numChunks + 1) * sizeof(uint8_t*));
            if (!arr) return 0;
            n->chunks = arr;
            uint8_t* c = (uint8_t*)malloc(nc);
            if (!c) return 0;
            n->chunks[n->numChunks++] = c;
            n->cap += nc;
            n->lastCap = nc;
        }
    }
    return 1;
}

// Copies between a node's pieces and a flat buffer (within cap).
static void gcOv_CopyIn(gcOvNode* n, uint32_t off, const uint8_t* src, uint32_t len)
{
    while (len) {
        uint32_t io = off % GCDVD_DATA_CHUNK;
        uint32_t k = GCDVD_DATA_CHUNK - io;
        if (k > len) k = len;
        if (src) memcpy(n->chunks[off / GCDVD_DATA_CHUNK] + io, src, k), src += k;
        else memset(n->chunks[off / GCDVD_DATA_CHUNK] + io, 0, k);
        off += k;
        len -= k;
    }
}

static void gcOv_CopyOut(const gcOvNode* n, uint32_t off, uint8_t* dst, uint32_t len)
{
    while (len) {
        uint32_t io = off % GCDVD_DATA_CHUNK;
        uint32_t k = GCDVD_DATA_CHUNK - io;
        if (k > len) k = len;
        memcpy(dst, n->chunks[off / GCDVD_DATA_CHUNK] + io, k);
        dst += k;
        off += k;
        len -= k;
    }
}

static int gcDvd_IsDirPath(const char* norm)
{
    if (!norm[0]) return 1;
    gcOvNode* n = gcOv_Find(norm);
    if (n) return n->isDir;
    int idx = gcDvd_FstLookup(norm);
    return idx >= 0 && gcDvd_IsDir(idx);
}

static int gcDvd_Exists(const char* norm)
{
    return !norm[0] || gcOv_Find(norm) || gcDvd_FstLookup(norm) >= 0;
}

static int gcDvd_ParentIsDir(const char* norm)
{
    char parent[GCDVD_MAX_PATH];
    strcpy(parent, norm);
    char* slash = strrchr(parent, '/');
    if (!slash) return 1; // root
    *slash = 0;
    return gcDvd_IsDirPath(parent);
}

static void gcDvd_NotifyWrite(void)
{
    if (gcDvd_writeCallback) gcDvd_writeCallback();
}

// Added: a write that ran out of memory. The file was left cut short, and a cut
// short save went on to the memory card: "could not load level" from then on
// (saving in a level with little memory free, e.g. level 4). The game checks
// this after writing a save (gcDvd_GcTakeWriteFailure) and throws the write away.
static int gcOv_IsVolatile(const gcOvNode* n);

int gcDvd_GcTakeWriteFailure(void)
{
    gcOvNode* n = gcDvd_pFailedNode;
    if (!n) return 0;
    gcDvd_pFailedNode = NULL;
    if (n->slot) {
        // Back to the copy on the card, as it was before this write.
        gcOv_FreeData(n);
        n->size = n->cardSize;
        n->bChanged = 0;
        n->bLazy = 1;
    }
    else if (gcOv_IsVolatile(n))
        gcOv_Remove(n); // a cut-short autosave / quicksave: none is better
    else
        // Settings or the player profile: never thrown away (it would lose them
        // all); what was written stays, and the next write replaces it.
        printf("gcDvd: %s was cut short (out of memory), kept\n", n->path);
    return 1;
}

// --- Save slots ------------------------------------------------------------------
// Saved games are kept out of RAM: gcCard attaches each memory card save slot as
// an overlay file whose data is fetched (and decompressed) only while it is open.

static gcDvdSlotLoader gcDvd_slotLoader = NULL;

void gcDvd_SetSlotLoader(gcDvdSlotLoader fn)
{
    gcDvd_slotLoader = fn;
}

static void gcOv_EnsureParentDirs(const char* norm)
{
    char dir[GCDVD_MAX_PATH];
    for (const char* p = strchr(norm, '/'); p; p = strchr(p + 1, '/')) {
        size_t n = (size_t)(p - norm);
        memcpy(dir, norm, n);
        dir[n] = 0;
        if (!gcDvd_IsDirPath(dir) && !gcOv_Find(dir))
            gcOv_Create(dir, 1);
    }
}

int gcDvd_SlotAttach(int slot, const char* norm, uint32_t size)
{
    gcOvNode* n = gcOv_Find(norm);
    if (n) { n->slot = slot; return 1; } // already in RAM (e.g. carried over in an old config)
    gcOv_EnsureParentDirs(norm);
    n = gcOv_Create(norm, 0);
    if (!n) return 0;
    n->slot = slot;
    n->bLazy = 1;
    n->size = size;
    n->cardSize = size;
    return 1;
}

int gcDvd_SlotState(const char* norm, gcDvdData* pData)
{
    gcOvNode* n = gcOv_Find(norm);
    if (!n || n->isDir) return GCDVD_SLOT_ABSENT;
    // Never persisted to a slot yet (e.g. carried over from an old config) counts
    // as changed.
    if (n->bLazy || (!n->bChanged && n->slot)) return GCDVD_SLOT_SAVED;
    if (pData) { pData->chunks = n->chunks; pData->size = n->size; }
    return GCDVD_SLOT_CHANGED;
}

void gcDvd_SlotPersisted(const char* norm, int slot)
{
    gcOvNode* n = gcOv_Find(norm);
    if (!n) return;
    n->slot = slot;
    n->bChanged = 0;
    n->cardSize = n->size;
    // Back on the card: drop the RAM copy (a save is ~380 KiB).
    gcOv_FreeData(n);
    n->bLazy = 1;
}

static int gcOv_Sink(void* ctx, const uint8_t* p, uint32_t len)
{
    gcOvNode* n = (gcOvNode*)ctx;
    if (!gcOv_Reserve(n, n->size + len)) return 0;
    gcOv_CopyIn(n, n->size, p, len);
    n->size += len;
    return 1;
}

static int gcOv_LoadLazy(gcOvNode* n)
{
    if (!n->bLazy) return 1;
    uint32_t expect = n->size;
    n->size = 0;
    gcOv_Reserve(n, expect); // all at once if it can; the sink grows it otherwise
    if (!gcDvd_slotLoader || !gcDvd_slotLoader(n->slot, gcOv_Sink, n)) {
        gcOv_FreeData(n);
        n->size = expect;
        return 0;
    }
    n->bLazy = 0;
    return 1;
}

int gcDvd_SlotLoadData(const char* norm, gcDvdData* pData)
{
    gcOvNode* n = gcOv_Find(norm);
    if (!n || n->isDir || !gcOv_LoadLazy(n)) return 0;
    pData->chunks = n->chunks;
    pData->size = n->size;
    return 1;
}

void gcDvd_SlotDropData(const char* norm)
{
    gcOvNode* n = gcOv_Find(norm);
    if (n && n->slot && !n->bChanged && !n->bLazy) {
        gcOv_FreeData(n);
        n->bLazy = 1;
    }
}

// --- devoptab ----------------------------------------------------------------

static int gcDvd_open_r(struct _reent* r, void* fileStruct, const char* path, int flags, int mode)
{
    char norm[GCDVD_MAX_PATH];
    if (!gcDvd_Normalize(path, norm, sizeof(norm))) { r->_errno = ENAMETOOLONG; return -1; }

    gcDvdFile* f = (gcDvdFile*)fileStruct;
    memset(f, 0, sizeof(*f));
    int wantWrite = (flags & O_ACCMODE) != O_RDONLY;

    // Added: level-start autosaves are named per map (_JKAUTO_<map>.jks) and live
    // in this RAM overlay; nothing ever deleted the old ones, so each level left
    // ~0.5 MiB behind (the memory that made later levels fail to load). Only the
    // current level's autosave is used (restart after dying): drop the others
    // when a new one is written.
    if (wantWrite && (flags & (O_CREAT | O_TRUNC))) {
        const char* base = strrchr(norm, '/');
        base = base ? base + 1 : norm;
        if (!strncasecmp(base, "_jkauto_", 8)) {
            for (gcOvNode* n = gcOv_pHead; n; ) {
                gcOvNode* next = n->next;
                const char* nb = strrchr(n->path, '/');
                nb = nb ? nb + 1 : n->path;
                if (!n->isDir && n != gcOv_Find(norm) && !strncasecmp(nb, "_jkauto_", 8))
                    gcOv_Remove(n);
                n = next;
            }
        }
    }

    gcOvNode* node = gcOv_Find(norm);
    if (node)
    {
        if (node->isDir) { r->_errno = EISDIR; return -1; }
        if ((flags & O_CREAT) && (flags & O_EXCL)) { r->_errno = EEXIST; return -1; }
        if (wantWrite && (flags & O_TRUNC)) {
            uint32_t oldSize = node->size;
            if (node->bLazy) { node->bLazy = 0; gcOv_FreeData(node); }
            node->size = 0;
            node->bChanged = 1;
            // Added: room for the whole file up front (the size it had, a bit
            // more), so it isn't grown by doubling as it's written: that needed
            // up to twice its size in one block, mid-level.
            if (oldSize > node->cap)
                gcOv_Reserve(node, oldSize + oldSize / 8);
        }
        else if (!gcOv_LoadLazy(node)) { r->_errno = EIO; return -1; }
    }
    else
    {
        int idx = gcDvd_FstLookup(norm);
        if (idx >= 0)
        {
            if (gcDvd_IsDir(idx)) { r->_errno = EISDIR; return -1; }
            if (!wantWrite) {
                f->entry = (uint32_t)idx;
                return (int)(intptr_t)f;
            }
            // Writing to a disc file: copy-on-write into the overlay.
            node = gcOv_Create(norm, 0);
            if (!node) { r->_errno = ENOMEM; return -1; }
            if (!(flags & O_TRUNC)) {
                uint32_t len = gcDvd_pFst[idx].length;
                int okCopy = gcOv_Reserve(node, len);
                for (uint32_t off = 0; okCopy && off < len; off += GCDVD_DATA_CHUNK) {
                    uint32_t k = len - off < GCDVD_DATA_CHUNK ? len - off : GCDVD_DATA_CHUNK;
                    okCopy = gcDvd_Read(node->chunks[off / GCDVD_DATA_CHUNK], k,
                                        (int64_t)gcDvd_pFst[idx].offset + off);
                }
                if (!okCopy) {
                    gcOv_Remove(node);
                    r->_errno = EIO;
                    return -1;
                }
                node->size = len;
            }
        }
        else
        {
            if (!(flags & O_CREAT)) { r->_errno = ENOENT; return -1; }
            if (!gcDvd_ParentIsDir(norm)) { r->_errno = ENOENT; return -1; }
            node = gcOv_Create(norm, 0);
            if (!node) { r->_errno = ENOMEM; return -1; }
        }
    }

    if (wantWrite && (flags & (O_TRUNC | O_CREAT))) node->bChanged = 1;
    if (wantWrite && gcDvd_pFailedNode == node) gcDvd_pFailedNode = NULL; // written anew
    f->node = node;
    f->bWritable = wantWrite;
    f->bAppend = !!(flags & O_APPEND);
    f->bDirty = wantWrite && (flags & O_TRUNC);
    f->pos = f->bAppend ? node->size : 0;
    return (int)(intptr_t)f;
}

static int gcDvd_close_r(struct _reent* r, void* fd)
{
    gcDvdFile* f = (gcDvdFile*)fd;
    if (f->bDirty) gcDvd_NotifyWrite();
    // A save slot that was only read goes back to living on the card.
    gcOvNode* n = f->node;
    if (n && n->slot && !n->bChanged && !n->bLazy && !f->bDirty) {
        gcOv_FreeData(n);
        n->bLazy = 1;
    }
    return 0;
}

static ssize_t gcDvd_read_r(struct _reent* r, void* fd, char* ptr, size_t len)
{
    gcDvdFile* f = (gcDvdFile*)fd;
    if (f->node)
    {
        if (f->pos >= f->node->size) return 0;
        if (len > f->node->size - f->pos) len = f->node->size - f->pos;
        gcOv_CopyOut(f->node, f->pos, (uint8_t*)ptr, len);
        f->pos += len;
        return (ssize_t)len;
    }

    const gcDvdFstEntry* e = &gcDvd_pFst[f->entry];
    if (f->pos >= e->length) return 0;
    if (len > e->length - f->pos) len = e->length - f->pos;
    if (!gcDvd_Read(ptr, len, (int64_t)e->offset + f->pos)) { r->_errno = EIO; return -1; }
    f->pos += len;
    return (ssize_t)len;
}

static ssize_t gcDvd_write_r(struct _reent* r, void* fd, const char* ptr, size_t len)
{
    gcDvdFile* f = (gcDvdFile*)fd;
    if (!f->node || !f->bWritable) { r->_errno = EBADF; return -1; }
    if (f->bAppend) f->pos = f->node->size;

    uint32_t end = f->pos + len;
    if (!gcOv_Reserve(f->node, end)) {
        printf("gcDvd: out of memory writing %s (%u bytes)\n", f->node->path, end);
        gcDvd_pFailedNode = f->node;
        r->_errno = ENOSPC;
        return -1;
    }
    if (f->pos > f->node->size) // seeked past the end: zero-fill the gap
        gcOv_CopyIn(f->node, f->node->size, NULL, f->pos - f->node->size);
    gcOv_CopyIn(f->node, f->pos, (const uint8_t*)ptr, len);
    f->pos = end;
    if (end > f->node->size) f->node->size = end;
    f->bDirty = 1;
    f->node->bChanged = 1;
    return (ssize_t)len;
}

static off_t gcDvd_seek_r(struct _reent* r, void* fd, off_t pos, int dir)
{
    gcDvdFile* f = (gcDvdFile*)fd;
    off_t base;
    switch (dir) {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = f->pos; break;
        case SEEK_END: base = f->node ? f->node->size : gcDvd_pFst[f->entry].length; break;
        default: r->_errno = EINVAL; return -1;
    }
    if (base + pos < 0) { r->_errno = EINVAL; return -1; }
    f->pos = (uint32_t)(base + pos);
    return f->pos;
}

static void gcDvd_FillOvStat(gcOvNode* n, struct stat* st)
{
    memset(st, 0, sizeof(*st));
    st->st_mode = n->isDir ? (S_IFDIR | 0777) : (S_IFREG | 0666);
    st->st_nlink = n->isDir ? 2 : 1;
    st->st_size = n->size;
    st->st_blksize = 2048;
    st->st_blocks = (st->st_size + 511) / 512;
}

static int gcDvd_fstat_r(struct _reent* r, void* fd, struct stat* st)
{
    gcDvdFile* f = (gcDvdFile*)fd;
    if (f->node) gcDvd_FillOvStat(f->node, st);
    else gcDvd_FillFstStat(f->entry, st);
    return 0;
}

static int gcDvd_stat_r(struct _reent* r, const char* path, struct stat* st)
{
    char norm[GCDVD_MAX_PATH];
    if (!gcDvd_Normalize(path, norm, sizeof(norm))) { r->_errno = ENAMETOOLONG; return -1; }
    gcOvNode* n = gcOv_Find(norm);
    if (n) { gcDvd_FillOvStat(n, st); return 0; }
    int idx = gcDvd_FstLookup(norm);
    if (idx < 0) { r->_errno = ENOENT; return -1; }
    gcDvd_FillFstStat((uint32_t)idx, st);
    return 0;
}

static int gcDvd_chdir_r(struct _reent* r, const char* path)
{
    char norm[GCDVD_MAX_PATH];
    if (!gcDvd_Normalize(path, norm, sizeof(norm))) { r->_errno = ENAMETOOLONG; return -1; }
    if (!gcDvd_Exists(norm)) { r->_errno = ENOENT; return -1; }
    if (!gcDvd_IsDirPath(norm)) { r->_errno = ENOTDIR; return -1; }
    strcpy(gcDvd_cwd, norm);
    return 0;
}

static int gcDvd_mkdir_r(struct _reent* r, const char* path, int mode)
{
    char norm[GCDVD_MAX_PATH];
    if (!gcDvd_Normalize(path, norm, sizeof(norm))) { r->_errno = ENAMETOOLONG; return -1; }
    if (gcDvd_Exists(norm)) { r->_errno = EEXIST; return -1; }
    if (!gcDvd_ParentIsDir(norm)) { r->_errno = ENOENT; return -1; }
    if (!gcOv_Create(norm, 1)) { r->_errno = ENOMEM; return -1; }
    gcDvd_NotifyWrite();
    return 0;
}

static int gcDvd_unlink_r(struct _reent* r, const char* path)
{
    char norm[GCDVD_MAX_PATH];
    if (!gcDvd_Normalize(path, norm, sizeof(norm))) { r->_errno = ENAMETOOLONG; return -1; }
    gcOvNode* n = gcOv_Find(norm);
    if (!n) { r->_errno = gcDvd_FstLookup(norm) >= 0 ? EROFS : ENOENT; return -1; }
    if (n->isDir) { r->_errno = EISDIR; return -1; }
    gcOv_Remove(n);
    gcDvd_NotifyWrite();
    return 0;
}

static int gcDvd_HasOvChildren(const char* norm)
{
    size_t len = strlen(norm);
    for (gcOvNode* n = gcOv_pHead; n; n = n->next)
        if (!strncasecmp(n->path, norm, len) && n->path[len] == '/') return 1;
    return 0;
}

static int gcDvd_rmdir_r(struct _reent* r, const char* path)
{
    char norm[GCDVD_MAX_PATH];
    if (!gcDvd_Normalize(path, norm, sizeof(norm))) { r->_errno = ENAMETOOLONG; return -1; }
    gcOvNode* n = gcOv_Find(norm);
    if (!n) { r->_errno = gcDvd_FstLookup(norm) >= 0 ? EROFS : ENOENT; return -1; }
    if (!n->isDir) { r->_errno = ENOTDIR; return -1; }
    if (gcDvd_HasOvChildren(norm)) { r->_errno = ENOTEMPTY; return -1; }
    gcOv_Remove(n);
    gcDvd_NotifyWrite();
    return 0;
}

static int gcDvd_rename_r(struct _reent* r, const char* oldName, const char* newName)
{
    char from[GCDVD_MAX_PATH], to[GCDVD_MAX_PATH];
    if (!gcDvd_Normalize(oldName, from, sizeof(from)) || !gcDvd_Normalize(newName, to, sizeof(to))) {
        r->_errno = ENAMETOOLONG;
        return -1;
    }
    gcOvNode* n = gcOv_Find(from);
    if (!n) { r->_errno = gcDvd_FstLookup(from) >= 0 ? EROFS : ENOENT; return -1; }
    if (!gcDvd_ParentIsDir(to)) { r->_errno = ENOENT; return -1; }

    gcOvNode* existing = gcOv_Find(to);
    if (existing && existing != n) {
        if (existing->isDir) { r->_errno = EISDIR; return -1; }
        gcOv_Remove(existing);
    }

    // Move the node and, for directories, everything below it.
    size_t fromLen = strlen(from);
    for (gcOvNode* c = gcOv_pHead; c; c = c->next) {
        if (c == n || (!strncasecmp(c->path, from, fromLen) && c->path[fromLen] == '/')) {
            char moved[GCDVD_MAX_PATH];
            snprintf(moved, sizeof(moved), "%s%s", to, c->path + fromLen);
            strcpy(c->path, moved);
        }
    }
    gcDvd_NotifyWrite();
    return 0;
}

static DIR_ITER* gcDvd_diropen_r(struct _reent* r, DIR_ITER* dirState, const char* path)
{
    gcDvdDir* d = (gcDvdDir*)dirState->dirStruct;
    if (!gcDvd_Normalize(path, d->path, sizeof(d->path))) { r->_errno = ENAMETOOLONG; return NULL; }
    if (!gcDvd_Exists(d->path)) { r->_errno = ENOENT; return NULL; }
    if (!gcDvd_IsDirPath(d->path)) { r->_errno = ENOTDIR; return NULL; }

    d->fstEntry = gcDvd_FstLookup(d->path);
    if (d->fstEntry >= 0 && !gcDvd_IsDir(d->fstEntry)) d->fstEntry = -1;
    d->fstCur = d->fstEntry >= 0 ? (uint32_t)d->fstEntry + 1 : 0;
    d->ovCur = gcOv_pHead;
    d->dots = 0;
    return dirState;
}

static int gcDvd_dirreset_r(struct _reent* r, DIR_ITER* dirState)
{
    gcDvdDir* d = (gcDvdDir*)dirState->dirStruct;
    d->fstCur = d->fstEntry >= 0 ? (uint32_t)d->fstEntry + 1 : 0;
    d->ovCur = gcOv_pHead;
    d->dots = 0;
    return 0;
}

// Is `n` a direct child of the directory `dir` (both normalized)?
static const char* gcDvd_ChildName(const char* dir, const gcOvNode* n)
{
    size_t len = strlen(dir);
    const char* rest;
    if (!len) rest = n->path;
    else if (!strncasecmp(n->path, dir, len) && n->path[len] == '/') rest = n->path + len + 1;
    else return NULL;
    return strchr(rest, '/') ? NULL : rest;
}

static int gcDvd_dirnext_r(struct _reent* r, DIR_ITER* dirState, char* filename, struct stat* st)
{
    gcDvdDir* d = (gcDvdDir*)dirState->dirStruct;
    if (d->dots < 2) {
        strcpy(filename, d->dots ? ".." : ".");
        if (st) {
            memset(st, 0, sizeof(*st));
            st->st_mode = S_IFDIR | 0555;
        }
        d->dots++;
        return 0;
    }

    // Disc entries first...
    if (d->fstEntry >= 0 && d->fstCur < gcDvd_pFst[d->fstEntry].length) {
        uint32_t idx = d->fstCur;
        strncpy(filename, gcDvd_Name(idx), NAME_MAX);
        filename[NAME_MAX] = 0;
        if (st) gcDvd_FillFstStat(idx, st);
        d->fstCur = gcDvd_IsDir(idx) ? gcDvd_pFst[idx].length : idx + 1;
        return 0;
    }

    // ...then overlay entries, skipping copy-on-write shadows of disc files.
    for (; d->ovCur; d->ovCur = d->ovCur->next) {
        gcOvNode* n = d->ovCur;
        const char* name = gcDvd_ChildName(d->path, n);
        if (!name || gcDvd_FstLookup(n->path) >= 0) continue;
        strncpy(filename, name, NAME_MAX);
        filename[NAME_MAX] = 0;
        if (st) gcDvd_FillOvStat(n, st);
        d->ovCur = n->next;
        return 0;
    }

    r->_errno = ENOENT;
    return -1;
}

static int gcDvd_dirclose_r(struct _reent* r, DIR_ITER* dirState)
{
    return 0;
}

static int gcDvd_ftruncate_r(struct _reent* r, void* fd, off_t len)
{
    gcDvdFile* f = (gcDvdFile*)fd;
    if (!f->node || !f->bWritable) { r->_errno = EBADF; return -1; }
    if (!gcOv_Reserve(f->node, (uint32_t)len)) { r->_errno = ENOSPC; return -1; }
    if ((uint32_t)len > f->node->size)
        gcOv_CopyIn(f->node, f->node->size, NULL, (uint32_t)len - f->node->size);
    f->node->size = (uint32_t)len;
    f->bDirty = 1;
    f->node->bChanged = 1;
    return 0;
}

static int gcDvd_fsync_r(struct _reent* r, void* fd)
{
    return 0;
}

static const devoptab_t gcDvd_devoptab = {
    .name         = "dvd",
    .structSize   = sizeof(gcDvdFile),
    .open_r       = gcDvd_open_r,
    .close_r      = gcDvd_close_r,
    .write_r      = gcDvd_write_r,
    .read_r       = gcDvd_read_r,
    .seek_r       = gcDvd_seek_r,
    .fstat_r      = gcDvd_fstat_r,
    .stat_r       = gcDvd_stat_r,
    .unlink_r     = gcDvd_unlink_r,
    .chdir_r      = gcDvd_chdir_r,
    .rename_r     = gcDvd_rename_r,
    .mkdir_r      = gcDvd_mkdir_r,
    .dirStateSize = sizeof(gcDvdDir),
    .diropen_r    = gcDvd_diropen_r,
    .dirreset_r   = gcDvd_dirreset_r,
    .dirnext_r    = gcDvd_dirnext_r,
    .dirclose_r   = gcDvd_dirclose_r,
    .ftruncate_r  = gcDvd_ftruncate_r,
    .fsync_r      = gcDvd_fsync_r,
    .rmdir_r      = gcDvd_rmdir_r,
    .lstat_r      = gcDvd_stat_r,
};

// --- Overlay persistence -------------------------------------------------------------
//
// Blob: "OJKS" u32 version, u32 count, then per node:
//   u8 isDir, u8 pad, u16 pathLen, u32 size, path bytes, data bytes
// Big-endian (native); only ever read back on the same platform.

#define GCOV_MAGIC   0x4F4A4B53 // "OJKS"
#define GCOV_VERSION 1

void gcDvd_SetWriteCallback(void (*callback)(void))
{
    gcDvd_writeCallback = callback;
}

// Saved games (*.jks) never go into the config blob: save slots have memory card
// files of their own (gcCard), and the level-start autosave (_JKAUTO_<map>.jks,
// the restart point after dying) lives in RAM only, as on the Dreamcast port.
static int gcOv_IsVolatile(const gcOvNode* n)
{
    size_t len = strlen(n->path);
    return !n->isDir && len > 4 && !strcasecmp(n->path + len - 4, ".jks");
}

// Bytes of RAM the overlay's files hold (saves being written or read, the level's
// autosave, settings); shown in level load errors.
uint32_t gcDvd_OverlayRamBytes(void)
{
    uint32_t total = 0;
    for (gcOvNode* n = gcOv_pHead; n; n = n->next)
        total += n->cap;
    return total;
}

uint32_t gcDvd_OverlaySerializedSize(void)
{
    uint32_t size = 12;
    for (gcOvNode* n = gcOv_pHead; n; n = n->next)
        if (!gcOv_IsVolatile(n))
            size += 8 + (uint32_t)strlen(n->path) + (n->isDir ? 0 : n->size);
    return size;
}

uint32_t gcDvd_OverlaySerialize(uint8_t* pOut, uint32_t cap)
{
    uint32_t need = gcDvd_OverlaySerializedSize();
    if (need > cap) return 0;

    uint8_t* p = pOut;
    uint32_t count = 0;
    for (gcOvNode* n = gcOv_pHead; n; n = n->next)
        if (!gcOv_IsVolatile(n)) count++;
    uint32_t hdr[3] = { GCOV_MAGIC, GCOV_VERSION, count };
    memcpy(p, hdr, 12); p += 12;

    for (gcOvNode* n = gcOv_pHead; n; n = n->next) {
        if (gcOv_IsVolatile(n)) continue;
        uint16_t pathLen = (uint16_t)strlen(n->path);
        uint32_t size = n->isDir ? 0 : n->size;
        p[0] = (uint8_t)n->isDir; p[1] = 0;
        memcpy(p + 2, &pathLen, 2);
        memcpy(p + 4, &size, 4);
        p += 8;
        memcpy(p, n->path, pathLen); p += pathLen;
        if (size) { gcOv_CopyOut(n, 0, p, size); p += size; }
    }
    return (uint32_t)(p - pOut);
}

int gcDvd_OverlayDeserialize(const uint8_t* pIn, uint32_t len)
{
    if (len < 12) return 0;
    uint32_t hdr[3];
    memcpy(hdr, pIn, 12);
    if (hdr[0] != GCOV_MAGIC || hdr[1] != GCOV_VERSION) return 0;

    const uint8_t* p = pIn + 12;
    const uint8_t* end = pIn + len;
    for (uint32_t i = 0; i < hdr[2]; i++) {
        if (p + 8 > end) return 0;
        uint16_t pathLen;
        uint32_t size;
        int isDir = p[0];
        memcpy(&pathLen, p + 2, 2);
        memcpy(&size, p + 4, 4);
        p += 8;
        if (pathLen >= GCDVD_MAX_PATH || p + pathLen + size > end) return 0;

        char path[GCDVD_MAX_PATH];
        memcpy(path, p, pathLen);
        path[pathLen] = 0;
        p += pathLen;

        gcOvNode* n = gcOv_Find(path);
        if (!n) n = gcOv_Create(path, isDir);
        if (!n) return 0;
        n->isDir = isDir;
        n->size = 0;
        if (size) {
            if (!gcOv_Reserve(n, size)) return 0;
            gcOv_CopyIn(n, 0, p, size);
            n->size = size;
            p += size;
        }
    }
    return 1;
}

// --- Direct reads for other threads ----------------------------------------------------

int gcDvd_FindDiscFile(const char* path, uint32_t* pOffset, uint32_t* pSize)
{
    char norm[GCDVD_MAX_PATH];
    if (!gcDvd_Normalize(path, norm, sizeof(norm))) return 0;
    int idx = gcDvd_FstLookup(norm);
    if (idx < 0 || gcDvd_IsDir(idx)) return 0;
    *pOffset = gcDvd_pFst[idx].offset;
    *pSize = gcDvd_pFst[idx].length;
    return 1;
}

// Added: forget everything cached in RAM (L1) and ARAM (L2) so the next reads
// come from the medium again (retrying a level load that read bad data).
void gcDvd_DropCaches(void)
{
    for (int i = 0; i < GCDVD_NUM_BLOCKS; i++)
        gcDvd_aBlocks[i].offset = -1;
    if (!gcDvd_l2Aram) return;
    LWP_MutexLock(gcDvd_l2Mutex);
    for (int i = 0; i < GCDVD_L2_COUNT; i++)
        if (!gcDvd_aL2[i].busy) {
            gcDvd_aL2[i].offset = -1;
            gcDvd_aL2[i].mask = 0;
        }
    LWP_MutexUnlock(gcDvd_l2Mutex);
}

int gcDvd_ReadAt(void* pDst, uint32_t len, uint32_t offset, void* pCmdBlock)
{
    if (gcDvd_source != GCDVD_SRC_DRIVE)
        return gcDvd_ReadImage(pDst, len, offset);
    DCInvalidateRange(pDst, len);
    return DVD_ReadPrio((dvdcmdblk*)pCmdBlock, pDst, len, offset, 2) >= 0;
}

// --- Disc image on an SD card ---------------------------------------------------------

static int gcDvd_IsOurImage(const char* path)
{
    char id[6];
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    int ok = fread(id, 1, sizeof(id), f) == sizeof(id) && !memcmp(id, GCDVD_DISC_ID, sizeof(id));
    fclose(f);
    return ok;
}

// Look for the image in a directory and (depth > 0) its subdirectories.
static int gcDvd_ScanForImage(const char* dir, int depth)
{
    DIR* d = opendir(dir);
    if (!d) return 0;
    struct dirent* ent;
    int found = 0;
    while (!found && (ent = readdir(d)))
    {
        if (ent->d_name[0] == '.') continue;
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        const char* ext = strrchr(ent->d_name, '.');
        int isDir = ent->d_type == DT_DIR;
        if (ent->d_type == DT_UNKNOWN) { // not every FAT driver fills d_type in
            struct stat st;
            isDir = !stat(path, &st) && S_ISDIR(st.st_mode);
        }
        if (isDir) {
            if (depth > 0) found = gcDvd_ScanForImage(path, depth - 1);
        }
        else if (ext && (!strcasecmp(ext, ".iso") || !strcasecmp(ext, ".gcm"))
                 && gcDvd_IsOurImage(path)) {
            strncpy(gcDvd_imagePath, path, sizeof(gcDvd_imagePath) - 1);
            found = 1;
        }
    }
    closedir(d);
    return found;
}

static void gcDvd_TuneSdSpeed(void);

// libdvm with the FAT and exFAT drivers of libogc2's libfat, registered here
// rather than through dvmInit: that one also mounts a GC Loader as "dvd", the
// name this file uses for the disc.
extern const DvmFsDriver g_vfatFsDriver, g_exfatFsDriver;
extern unsigned g_dvmDefaultCachePages, g_dvmDefaultSectorsPerPage;

static int gcDvd_FindImageOnSd(void)
{
    static int bDrivers = 0;
    if (!bDrivers) {
        bDrivers = 1;
        dvmRegisterFsDriver(&g_vfatFsDriver);
        dvmRegisterFsDriver(&g_exfatFsDriver);
    }
    // SD2SP2 (serial port 2), then SD Gecko style adapters in slot B and slot A.
    static const DISC_INTERFACE* const aIfaces[] = { &__io_gcsd2, &__io_gcsdb, &__io_gcsda };
    static const char* const aNames[] = { "SD2SP2", "slot B", "slot A" };
    static const int aCardChan[] = { -1, CARD_SLOTB, CARD_SLOTA };
    static const int aSdDrive[] = { 2, 1, 0 };   // sdgecko drive = EXI channel
    for (size_t i = 0; i < sizeof(aIfaces) / sizeof(aIfaces[0]); i++)
    {
        // Don't send SD commands to a memory card.
        if (aCardChan[i] >= 0 && CARD_ProbeEx(aCardChan[i], NULL, NULL) == CARD_ERROR_READY) {
            printf("gcDvd: %s holds a memory card\n", aNames[i]);
            continue;
        }
        printf("gcDvd: looking for an SD card in %s...\n", aNames[i]);
        unsigned numVolumes = dvmProbeMountDiscIface("sd", (DISC_INTERFACE*)aIfaces[i],
                                                     g_dvmDefaultCachePages, g_dvmDefaultSectorsPerPage);
        if (numVolumes) {
            printf("gcDvd: SD card mounted (%u volume%s), searching for openjkdf2.iso\n",
                   numVolumes, numVolumes == 1 ? "" : "s");
            if (gcDvd_IsOurImage("sd:/openjkdf2.iso"))
                strcpy(gcDvd_imagePath, "sd:/openjkdf2.iso");
            if (gcDvd_imagePath[0] || gcDvd_ScanForImage("sd:/games", 2) || gcDvd_ScanForImage("sd:", 0)) {
                gcDvd_pImage = fopen(gcDvd_imagePath, "rb");
                if (gcDvd_pImage) {
                    printf("gcDvd: game image %s\n", gcDvd_imagePath);
                    gcDvd_source = GCDVD_SRC_SD;
                    gcDvd_sdDrive = aSdDrive[i];
                    gcDvd_TuneSdSpeed();
                    return 1;
                }
            }
            dvmUnmountVolume("sd");
        }
        printf("gcDvd: no disc image found in %s\n", aNames[i]);
        gcDvd_imagePath[0] = 0;
    }
    return 0;
}

static uint32_t gcDvd_TimedRead(uint8_t* buf, uint32_t len, int* pOk)
{
    u64 start = gettime();
    *pOk = gcDvd_ReadImage(buf, len, 0);
    uint32_t us = ticks_to_microsecs(diff_ticks(start, gettime()));
    return us ? us : 1;
}

// libogc talks to SD cards at 16 MHz, polling every 4 bytes, which is barely
// enough for the cutscenes. Most cards (and the SD2SP2) handle 32 MHz: switch to it
// if the same data reads back identically, and report both speeds on screen.
static void gcDvd_TuneSdSpeed(void)
{
    const uint32_t len = 256 * 1024;
    uint8_t* a = (uint8_t*)memalign(32, len);
    uint8_t* b = (uint8_t*)memalign(32, len);
    if (a && b)
    {
        int okSlow, okFast;
        uint32_t usSlow = gcDvd_TimedRead(a, len, &okSlow);
        sdgecko_setSpeed(gcDvd_sdDrive, EXI_SPEED32MHZ);
        uint32_t usFast = gcDvd_TimedRead(b, len, &okFast);
        int ok = okSlow && okFast && !memcmp(a, b, len);
        if (ok) {
            memset(b, 0, len);
            ok = gcDvd_ReadImage(b, len, 0) && !memcmp(a, b, len);
        }
        gcDvd_bSdFast = ok;
        if (!ok) sdgecko_setSpeed(gcDvd_sdDrive, EXI_SPEED16MHZ);
        snprintf(gcDvd_szSdSpeed, sizeof(gcDvd_szSdSpeed),
                 "%u KiB/s at 16 MHz, %u KiB/s at 32 MHz%s",
                 (uint32_t)((uint64_t)len * 1000000 / 1024 / usSlow),
                 (uint32_t)((uint64_t)len * 1000000 / 1024 / usFast),
                 ok ? " (using 32 MHz)" : " (32 MHz unreliable, using 16 MHz)");
        printf("gcDvd: SD reads %s\n", gcDvd_szSdSpeed);
    }
    free(a);
    free(b);
}

// DVD_Mount blocks forever without a disc; give the drive a bounded time instead.
static int gcDvd_MountDrive(void)
{
    static dvdcmdblk mountBlock;
    printf("gcDvd: waiting for the disc drive...\n");
    DVD_Init();
    if (DVD_MountAsync(&mountBlock, NULL) < 0) return 0;
    u64 start = gettime();
    while (DVD_GetCmdBlockStatus(&mountBlock) == DVD_STATE_BUSY
           || DVD_GetCmdBlockStatus(&mountBlock) == DVD_STATE_WAITING) {
        if (diff_msec(start, gettime()) > GCDVD_MOUNT_TIMEOUT_MS) {
            printf("gcDvd: no disc in the drive\n");
            return 0;
        }
        usleep(10 * 1000);
    }
    return DVD_GetCmdBlockStatus(&mountBlock) == DVD_STATE_END;
}

// --- Mount ---------------------------------------------------------------------------

int gcDvd_Mount(void)
{
    if (gcDvd_bMounted) return 1;

    LWP_MutexInit(&gcDvd_imageMutex, false);
    if (gcDvd_FindImageOnSd())
        printf("gcDvd: reading the game from %s\n", gcDvd_imagePath);
    else if (!gcDvd_MountDrive()) {
        printf("gcDvd: no game disc and no disc image on an SD card\n");
        return 0;
    }

    for (int i = 0; i < GCDVD_NUM_BLOCKS; i++) {
        gcDvd_aBlocks[i].data = (uint8_t*)memalign(32, GCDVD_BLOCK_SIZE);
        gcDvd_aBlocks[i].offset = -1;
        gcDvd_aBlocks[i].lastUse = 0;
        if (!gcDvd_aBlocks[i].data) return 0;
    }

    uint8_t* hdr = (uint8_t*)memalign(32, 0x40);
    if (!hdr) return 0;
    if (!gcDvd_ReadRaw(hdr, 0x40, 0x420)) {
        printf("gcDvd: failed to read the disc header\n");
        free(hdr);
        return 0;
    }
    uint32_t fstOffset = *(uint32_t*)(hdr + 0x04);
    uint32_t fstSize   = *(uint32_t*)(hdr + 0x08);
    free(hdr);

    uint32_t fstAlloc = (fstSize + 31) & ~31u;
    gcDvd_pFst = (gcDvdFstEntry*)memalign(32, fstAlloc);
    if (!gcDvd_pFst || !gcDvd_ReadRaw(gcDvd_pFst, fstAlloc, fstOffset)) {
        printf("gcDvd: failed to read the FST (off 0x%x size 0x%x)\n", fstOffset, fstSize);
        return 0;
    }

    gcDvd_numEntries = gcDvd_pFst[0].length;
    gcDvd_pStrings = (const char*)&gcDvd_pFst[gcDvd_numEntries];
    gcDvd_cwd[0] = 0;
    gcDvd_bMounted = 1;

    for (uint32_t i = 1; i < gcDvd_numEntries; i++) {
        if (gcDvd_IsDir(i)) continue;
        uint32_t end = gcDvd_pFst[i].offset + gcDvd_pFst[i].length;
        if (end > gcDvd_discEnd) gcDvd_discEnd = end;
    }

    // ARAM read cache.
    printf("gcDvd: disc filesystem ok (%u entries), setting up ARAM\n", gcDvd_numEntries);
    AR_Init(gcDvd_aArStack, 4);
    ARQ_Init();
    gcDvd_pL2Staging = (uint8_t*)memalign(32, GCDVD_L2_BLOCK);
    // One spare slice past the end as a guard: something else in the process
    // writes just below the top of the last ARAM allocation (seen with
    // GC_DVD_VERIFY as corruption in the final slice, ~64-96 KiB from its end).
    u32 aramSize = GCDVD_TEXPOOL_SIZE + (GCDVD_L2_COUNT + 1) * GCDVD_L2_BLOCK;
    u32 aram = gcDvd_pL2Staging ? AR_Alloc(aramSize) : 0;
    if (aram && aram + aramSize <= AR_GetSize()) {
        gcAram_InitPool(aram, GCDVD_TEXPOOL_SIZE);
        printf("gcDvd: %u KiB ARAM texture pool at 0x%x\n", GCDVD_TEXPOOL_SIZE / 1024, aram);
        aram += GCDVD_TEXPOOL_SIZE;
        gcDvd_l2Aram = aram;
        for (int i = 0; i < GCDVD_L2_COUNT; i++) {
            gcDvd_aL2[i].offset = -1;
            gcDvd_aL2[i].len = 0;
            gcDvd_aL2[i].lastUse = 0;
            gcDvd_aL2[i].mask = 0;
            gcDvd_aL2[i].busy = 0;
        }
        printf("gcDvd: %u KiB ARAM read cache at 0x%x\n", (GCDVD_L2_COUNT * GCDVD_L2_BLOCK) / 1024, aram);
        LWP_MutexInit(&gcDvd_l2Mutex, false);
        gcDvd_StartPrefetch();
    }
    else {
        printf("gcDvd: no ARAM read cache\n");
    }

    if (AddDevice(&gcDvd_devoptab) < 0) {
        printf("gcDvd: AddDevice failed\n");
        return 0;
    }
    printf("gcDvd: mounted, %u FST entries\n", gcDvd_numEntries);
    return 1;
}
