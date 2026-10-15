// stdMci (CD music) backend for the GameCube: streams the soundtrack from the disc.
//
// The original game plays CD audio tracks. Here each track is a headerless PCM
// file, 16-bit big-endian stereo at GCMUSIC_RATE Hz, at dvd:/music/trackNN.pcm
// (GOG numbering: track12..18 = CD 1, track22..32 = CD 2; see extract_assets.py).
//
// A reader thread pulls 64 KiB slices from the disc into a ring buffer with its own
// DVD command block (independent of the engine's file reads), and an AESND voice in
// stream mode drains the ring in real time.

#include "Win95/stdMci.h"

#include "Main/jkMain.h"
#include "stdPlatform.h"
#include "jk.h"

#ifdef STDSOUND_GAMECUBE

#include "gcOgc.h"
#include "gcDvd.h"
#include "gcVoice.h"
#include <aesndlib.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define GCMUSIC_RATE        32000
#define GCMUSIC_RING        (512 * 1024)
#define GCMUSIC_READ        (32 * 1024)   // per locked SD read: keeps the game's own reads waiting <~17 ms
#define GCMUSIC_CHUNK       (4 * DSP_STREAMBUFFER_SIZE) // see GCSND_STREAM_CHUNK in stdSound.c
#define GCMUSIC_VOLUME_MAX  255

static uint8_t*          gcMusic_pRing = NULL;
static volatile uint32_t gcMusic_ringW = 0;      // reader thread
static volatile uint32_t gcMusic_ringR = 0;      // DSP callback
static uint8_t*          gcMusic_pReadBuf = NULL;
static uint8_t*          gcMusic_aChunks[2];
static int               gcMusic_curChunk = 0;
static AESNDPB*          gcMusic_pVoice = NULL;
static dvdcmdblk         gcMusic_cmd;

static lwp_t             gcMusic_thread = LWP_THREAD_NULL;
static uint8_t           gcMusic_threadStack[16 * 1024] __attribute__((aligned(32)));
static mutex_t           gcMusic_mtx = LWP_MUTEX_NULL;

// Reader state (guarded by gcMusic_mtx)
static volatile int      gcMusic_bPlaying = 0;
static volatile int      gcMusic_bEnded = 0;
static int               gcMusic_trackCur = 0;
static int               gcMusic_trackEnd = 0;
static uint32_t          gcMusic_fileOffset = 0;
static uint32_t          gcMusic_fileSize = 0;
static uint32_t          gcMusic_filePos = 0;
static int               gcMusic_vol = GCMUSIC_VOLUME_MAX;

// Locate a track on the disc. jkCredits passes original CD track numbers (2..12),
// so map those to the GOG numbering by the episode's disc, like the other ports.
static int gcMusic_OpenTrack(int track)
{
    char path[64];
    snprintf(path, sizeof(path), "/music/track%d.pcm", track);
    if (gcDvd_FindDiscFile(path, &gcMusic_fileOffset, &gcMusic_fileSize)) {
        gcMusic_filePos = 0;
        return 1;
    }
    if (track <= 12) {
        int cdNum = 1;
        if (jkMain_pEpisodeEnt)       cdNum = jkMain_pEpisodeEnt->cdNum;
        else if (jkMain_pEpisodeEnt2) cdNum = jkMain_pEpisodeEnt2->cdNum;
        snprintf(path, sizeof(path), "/music/track%d.pcm", track + (cdNum == 2 ? 20 : 10));
        if (gcDvd_FindDiscFile(path, &gcMusic_fileOffset, &gcMusic_fileSize)) {
            gcMusic_filePos = 0;
            return 1;
        }
    }
    gcMusic_fileSize = 0;
    return 0;
}

// DSP callback: hand the voice its next chunk (silence on underrun).
static void gcMusic_Feed(AESNDPB* pb)
{
    uint8_t* chunk = gcMusic_aChunks[gcMusic_curChunk];
    gcMusic_curChunk ^= 1;

    uint32_t avail = gcMusic_ringW - gcMusic_ringR;
    uint32_t give = avail < GCMUSIC_CHUNK ? avail : GCMUSIC_CHUNK;
    give &= ~3u;
    uint32_t r = gcMusic_ringR % GCMUSIC_RING;
    uint32_t first = give < GCMUSIC_RING - r ? give : GCMUSIC_RING - r;
    memcpy(chunk, gcMusic_pRing + r, first);
    if (give > first) memcpy(chunk + first, gcMusic_pRing, give - first);
    if (give < GCMUSIC_CHUNK) memset(chunk + give, 0, GCMUSIC_CHUNK - give);
    gcMusic_ringR += give;

    DCFlushRange(chunk, GCMUSIC_CHUNK);
    AESND_SetVoiceBuffer(pb, chunk, GCMUSIC_CHUNK);
}

// Bumped when the voice is released; its callback ignores (and stops) a voice from
// an older generation (see gcVoice.c).
static volatile uint32_t gcMusic_voiceGen = 1;

static void gcMusic_VoiceCallback(AESNDPB* pb, u32 state, void* arg)
{
    if ((uint32_t)(uintptr_t)arg != gcMusic_voiceGen || !gcMusic_pVoice) {
        if (state != VOICE_STATE_STOPPED) AESND_SetVoiceStop(pb, true);
        return;
    }
    if (state == VOICE_STATE_STREAM)
        gcMusic_Feed(pb);
}

// Reader thread: keep the ring topped up from the disc, advancing through the
// requested track range.
static void* gcMusic_ThreadMain(void* arg)
{
    while (1)
    {
        LWP_MutexLock(gcMusic_mtx);
        int bWork = gcMusic_bPlaying && !gcMusic_bEnded
                    && (GCMUSIC_RING - (gcMusic_ringW - gcMusic_ringR)) >= GCMUSIC_READ;
        if (bWork)
        {
            if (gcMusic_filePos >= gcMusic_fileSize) {
                // End of this track: next in range, else done (the mixer restarts it).
                if (gcMusic_trackCur < gcMusic_trackEnd && gcMusic_OpenTrack(++gcMusic_trackCur)) {
                }
                else {
                    gcMusic_bEnded = 1;
                }
            }
            else {
                uint32_t len = gcMusic_fileSize - gcMusic_filePos;
                if (len > GCMUSIC_READ) len = GCMUSIC_READ;
                uint32_t readLen = (len + 31) & ~31u;
                if (gcDvd_ReadAt(gcMusic_pReadBuf, readLen, gcMusic_fileOffset + gcMusic_filePos, &gcMusic_cmd)) {
                    len &= ~3u;
                    uint32_t w = gcMusic_ringW % GCMUSIC_RING;
                    uint32_t first = len < GCMUSIC_RING - w ? len : GCMUSIC_RING - w;
                    memcpy(gcMusic_pRing + w, gcMusic_pReadBuf, first);
                    if (len > first) memcpy(gcMusic_pRing, gcMusic_pReadBuf + first, len - first);
                    gcMusic_ringW += len;
                }
                gcMusic_filePos += len ? len : 4;
            }
        }
        LWP_MutexUnlock(gcMusic_mtx);
        if (!bWork) usleep(20 * 1000);
    }
    return NULL;
}

static void gcMusic_StopVoice(void)
{
    if (gcMusic_pVoice) {
        u32 level;
        level = IRQ_Disable();
        AESNDPB* pb = gcMusic_pVoice;
        gcMusic_pVoice = NULL;
        gcMusic_voiceGen++;
        IRQ_Restore(level);
        gcVoice_Free(pb, NULL);
    }
}

int stdMci_Startup()
{
    stdMci_bIsGOG = 1;
    if (stdMci_bInitted) return 1;

    gcMusic_pRing = (uint8_t*)memalign(32, GCMUSIC_RING);
    gcMusic_pReadBuf = (uint8_t*)memalign(32, GCMUSIC_READ);
    gcMusic_aChunks[0] = (uint8_t*)memalign(32, GCMUSIC_CHUNK);
    gcMusic_aChunks[1] = (uint8_t*)memalign(32, GCMUSIC_CHUNK);
    if (!gcMusic_pRing || !gcMusic_pReadBuf || !gcMusic_aChunks[0] || !gcMusic_aChunks[1]) {
        stdPlatform_Printf("stdMci: out of memory, no music\n");
        return 0;
    }

    LWP_MutexInit(&gcMusic_mtx, false);
    // Below the main thread (GCSYSTEM_MAIN_PRIO): reads from an SD card keep the CPU
    // busy, so they should happen while the game waits for vsync, not mid-frame.
    // The 512 KiB ring covers several seconds.
    LWP_CreateThread(&gcMusic_thread, gcMusic_ThreadMain, NULL,
                     gcMusic_threadStack, sizeof(gcMusic_threadStack), 50);
    stdMci_bInitted = 1;
    return 1;
}

void stdMci_Shutdown()
{
    stdMci_Stop();
}

int stdMci_Play(uint8_t trackFrom, uint8_t trackTo)
{
    if (!stdMci_bInitted) return 0;
    stdMci_Stop();

    LWP_MutexLock(gcMusic_mtx);
    gcMusic_trackCur = trackFrom;
    gcMusic_trackEnd = trackTo >= trackFrom ? trackTo : trackFrom;
    int ok = gcMusic_OpenTrack(gcMusic_trackCur);
    if (ok) {
        gcMusic_ringW = gcMusic_ringR = 0;
        gcMusic_bEnded = 0;
        gcMusic_bPlaying = 1;
    }
    LWP_MutexUnlock(gcMusic_mtx);
    stdPlatform_Printf("stdMci: play tracks %d-%d%s\n", trackFrom, trackTo, ok ? "" : " (not on disc)");
    if (!ok) return 0; // track not on the disc

    gcMusic_pVoice = AESND_AllocateVoiceWithArg(gcMusic_VoiceCallback, (void*)(uintptr_t)gcMusic_voiceGen);
    if (!gcMusic_pVoice) return 0;
    AESND_SetVoiceFormat(gcMusic_pVoice, VOICE_STEREO16);
    AESND_SetVoiceFrequency(gcMusic_pVoice, GCMUSIC_RATE);
    AESND_SetVoiceVolume(gcMusic_pVoice, gcMusic_vol, gcMusic_vol);
    AESND_SetVoiceStream(gcMusic_pVoice, true);
    gcMusic_Feed(gcMusic_pVoice);
    AESND_SetVoiceStop(gcMusic_pVoice, false);
    return 1;
}

void stdMci_SetVolume(flex_t vol)
{
    int v = (int)(vol * GCMUSIC_VOLUME_MAX + 0.5);
    if (v < 0) v = 0;
    if (v > GCMUSIC_VOLUME_MAX) v = GCMUSIC_VOLUME_MAX;
    gcMusic_vol = v;
    if (gcMusic_pVoice)
        AESND_SetVoiceVolume(gcMusic_pVoice, v, v);
}

void stdMci_Stop()
{
    gcMusic_StopVoice();
    if (gcMusic_mtx != LWP_MUTEX_NULL) LWP_MutexLock(gcMusic_mtx);
    gcMusic_bPlaying = 0;
    gcMusic_bEnded = 0;
    gcMusic_ringW = gcMusic_ringR = 0;
    if (gcMusic_mtx != LWP_MUTEX_NULL) LWP_MutexUnlock(gcMusic_mtx);
}

int stdMci_CheckStatus()
{
    // Playing until the last track drained; reporting idle makes sithSoundMixer
    // re-issue Play, which is how the soundtrack loops.
    if (!gcMusic_bPlaying) return 0;
    if (gcMusic_bEnded && gcMusic_ringW == gcMusic_ringR) {
        gcMusic_StopVoice();
        gcMusic_bPlaying = 0;
        return 0;
    }
    return 1;
}

flex_d_t stdMci_GetTrackLength(int track)
{
    return 0.0;
}

#endif // STDSOUND_GAMECUBE
