#ifndef _PLATFORM_GAMECUBE_GCDVD_H
#define _PLATFORM_GAMECUBE_GCDVD_H

#include <stdint.h>

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// Mounts the disc filesystem (plus its writable RAM overlay) as "dvd:/".
// Returns 1 on success.
int gcDvd_Mount(void);
// The game is read from an SD card in serial port 2 (SD2SP2, EXI channel 2).
int gcDvd_UsesSerialPort2(void);

// Diagnostics: total time the engine spent in dvd: reads, and the part of it
// spent actually reading the disc/SD (cache misses).
extern volatile uint64_t gcDvd_statReadUs;
extern volatile uint64_t gcDvd_statMissUs;
// Diagnostics: last file the engine opened, the last main-thread read slower than
// 20 ms (with that file's name), and the result of the last end-of-load prefetch.
extern char gcDvd_szLastOpen[40];
extern char gcDvd_szSlow[40];
extern volatile uint32_t gcDvd_slowMs;
extern uint32_t gcDvd_prefDone, gcDvd_prefLeft;
// ARAM cache blocks that failed their checksum (re-read from the medium).
extern volatile uint32_t gcDvd_statAramBad;
extern volatile int32_t gcDvd_statAramBadSlot;
// SD card read errors (retries included).
extern volatile uint32_t gcDvd_statReadErrors;
// Drop the RAM and ARAM read caches (next reads come from the medium).
void gcDvd_DropCaches(void);
enum { GCDVD_STAT_READS, GCDVD_STAT_KB, GCDVD_STAT_DIRECT, GCDVD_STAT_L1MISS, GCDVD_STAT_L2HIT, GCDVD_STAT_NUM };
extern volatile uint32_t gcDvd_aStatCount[GCDVD_STAT_NUM];

// Read-ahead hint: bytes [offset, offset+len) of the dvd: file `fp` will be needed
// soon; they are copied into the ARAM cache in the background (SD cards only).
void gcDvd_HintFileRange(FILE* fp, uint32_t offset, uint32_t len);

// Wait (up to maxMs) for the hinted ranges to reach the ARAM cache, e.g. at the
// end of a loading screen, so the level's sounds don't load from the SD card
// mid-game. SD cards only.
void gcDvd_DrainHints(uint32_t maxMs);

// When the game runs from an image on an SD card: a printable summary (path,
// filesystem, measured read speed) in buf. NULL when reading the real drive.
const char* gcDvd_GetSdInfo(char* buf, int len);

// Hold off every read of the disc image (main, music and read-ahead threads) until
// gcDvd_ResumeReads, e.g. while the memory card is written.
void gcDvd_PauseReads(void);
void gcDvd_ResumeReads(void);

// Overlay files are kept in pieces of this many bytes (the last may be shorter).
#define GCDVD_DATA_CHUNK (32 * 1024)
typedef struct gcDvdData {
    uint8_t* const* chunks; // piece i holds bytes [i * GCDVD_DATA_CHUNK, ...)
    uint32_t        size;
} gcDvdData;

// Save slots (see gcCard): an overlay file attached to a memory card file of its
// own. Its data is fetched through the loader only while the file is open; the
// loader hands it over a piece at a time through sink (0: out of memory).
typedef int (*gcDvdSlotSink)(void* ctx, const uint8_t* p, uint32_t len);
typedef int (*gcDvdSlotLoader)(int slot, gcDvdSlotSink sink, void* ctx);
void gcDvd_SetSlotLoader(gcDvdSlotLoader fn);
int  gcDvd_SlotAttach(int slot, const char* normPath, uint32_t size);
enum { GCDVD_SLOT_ABSENT, GCDVD_SLOT_SAVED, GCDVD_SLOT_CHANGED };
// ABSENT: no such file; SAVED: unchanged since persisted; CHANGED: *pData holds
// the new contents to write to the card.
int  gcDvd_SlotState(const char* normPath, gcDvdData* pData);
// A saved slot's contents, fetched from the card (gcDvd_SlotDropData lets go).
int  gcDvd_SlotLoadData(const char* normPath, gcDvdData* pData);
void gcDvd_SlotDropData(const char* normPath);
// The slot's contents are now on the card: drop the RAM copy.
void gcDvd_SlotPersisted(const char* normPath, int slot);
// A write ran out of memory since the last call: the file it cut short is thrown
// away (a save slot goes back to the copy on the card). Returns 1 if so.
int  gcDvd_GcTakeWriteFailure(void);

// Called whenever the overlay changes (a written file is closed, a directory or
// file is created/removed/renamed).
void gcDvd_SetWriteCallback(void (*callback)(void));

// Raw disc access for background threads (music streaming): locate a disc file by
// absolute path ("/music/track12.pcm") and read with the caller's own dvdcmdblk.
// gcDvd_ReadAt needs a 32-byte aligned buffer, a multiple-of-32 length and a 4-byte
// aligned offset. Neither touches the shared block cache.
int gcDvd_FindDiscFile(const char* path, uint32_t* pOffset, uint32_t* pSize);
int gcDvd_ReadAt(void* pDst, uint32_t len, uint32_t offset, void* pCmdBlock);

// Overlay (runtime-written files) persistence.
uint32_t gcDvd_OverlaySerializedSize(void);
uint32_t gcDvd_OverlayRamBytes(void);
uint32_t gcDvd_OverlaySerialize(uint8_t* pOut, uint32_t cap);
int gcDvd_OverlayDeserialize(const uint8_t* pIn, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif // _PLATFORM_GAMECUBE_GCDVD_H
