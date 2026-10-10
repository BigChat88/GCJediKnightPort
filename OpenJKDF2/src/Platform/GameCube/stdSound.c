// stdSound backend for the Nintendo GameCube: libogc AESND (DSP mixer, 32 voices).
//
// Selected by STDSOUND_GAMECUBE. The engine fills PCM buffers through
// stdSound_BufferSetData/Unlock (sithSound swaps file PCM to big-endian on load);
// each playing buffer gets an AESND voice, released when it stops. Positional
// audio is done by the engine's 2D path (jkGuiSound_b3DSound = 0): it sets volume
// and pan per buffer, which map straight onto the voice's left/right volume.
//
// Cutscenes stream audio by queueing filled buffers one after another
// (stdSound_BufferQueueAfterAnother); those are copied into a ring that a single
// streaming voice drains in real time.

#include <unistd.h>
#include "Win95/stdSound.h"

#include "Gui/jkGUISound.h"
#include "General/stdMath.h"
#include "stdPlatform.h"

#include "jk.h"

#ifdef STDSOUND_GAMECUBE

#include "Platform/GameCube/gcOgc.h"
#include "Platform/GameCube/gcVoice.h"
#include <aesndlib.h>
#include <ogc/lwp_watchdog.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#define GCSND_VOLUME_MAX 255

// --- Refcounted samples --------------------------------------------------------
// DirectSound semantics: duplicated buffers share the sample memory. The PCM
// lives here and every buffer (original or duplicate) holds a reference.
typedef struct gcSample {
    int   refs;
    void* data;      // 32-byte aligned, flushed to RAM for the DSP
    int   bytes;
} gcSample;

static int stdSound_bReady = 0;

void* gcHeap_SndAlloc(size_t n); // the sound arena (gcHeap.c)
void gcHeap_SndFree(void* p);

static gcSample* gcSample_New(int bytes)
{
    gcSample* s = (gcSample*)malloc(sizeof(gcSample));
    if (!s) return NULL;
    s->refs = 1;
    s->bytes = bytes;
    s->data = gcHeap_SndAlloc((bytes + 31) & ~31);
    if (!s->data) { free(s); return NULL; }
    memset(s->data, 0, bytes);
    return s;
}

static void gcSample_Unref(gcSample* s)
{
    if (!s || --s->refs > 0) return;
    gcHeap_SndFree(s->data);
    free(s);
}

static u32 stdSound_VoiceFormat(const stdSound_buffer_t* buf)
{
    if (buf->bitsPerSample == 16)
        return buf->bStereo ? VOICE_STEREO16 : VOICE_MONO16;
    // 8-bit WAV PCM is unsigned
    return buf->bStereo ? VOICE_STEREO8_UNSIGNED : VOICE_MONO8_UNSIGNED;
}

static void stdSound_ApplyVolume(stdSound_buffer_t* buf)
{
    if (!buf->pVoice) return;
    flex_t vol = stdMath_Clamp(buf->vol, 0.0, 1.0) * GCSND_VOLUME_MAX;
    flex_t pan = stdMath_Clamp(buf->pan, -1.0, 1.0);
    flex_t l = vol * (pan > 0.0 ? 1.0 - pan : 1.0);
    flex_t r = vol * (pan < 0.0 ? 1.0 + pan : 1.0);
    AESND_SetVoiceVolume((AESNDPB*)buf->pVoice, (u16)l, (u16)r);
}

// --- Voice slots -----------------------------------------------------------------
// Each playing buffer owns a slot. The voice callback gets a token (slot index +
// generation) instead of a pointer: AESND can call a voice's callback after the
// voice was freed (see gcVoice.c), and by then the buffer may be gone. A stale
// token just stops the voice. The slot keeps what the DSP reads from (the sample,
// the loop chunks) until gcVoice reports the voice gone.
//
// Looping voices: AESND's own looping (AESND_PlayVoice(..., looped)) zero-pads the
// last DSP_STREAMBUFFER_SIZE piece of the sample before jumping back to its start,
// so every loop gets up to ~26 ms of silence: an audible click in ambient loops.
// Loops are streamed instead, in chunks that are a multiple of that piece, taken
// circularly from the sample.
#define GCSND_LOOP_CHUNK (4 * DSP_STREAMBUFFER_SIZE)
#define GCSND_MAX_SLOTS  64
#define GCSND_GEN_MASK   0xFFFFFF

enum { GCSND_SLOT_FREE, GCSND_SLOT_ACTIVE, GCSND_SLOT_RELEASING };

typedef struct gcSndSlot {
    volatile int       state;
    volatile int       bGone;      // set once the released voice is really freed
    volatile uint32_t  gen;
    stdSound_buffer_t* volatile buf;
    AESNDPB*           pb;
    gcSample*          pSample;    // reference held while the DSP may read it
    uint8_t*           aChunks[2]; // looping voices
    int                cur;
    uint32_t           pos;
} gcSndSlot;

static gcSndSlot stdSound_aSlots[GCSND_MAX_SLOTS];

static void* stdSound_SlotToken(gcSndSlot* s)
{
    return (void*)(uintptr_t)((s->gen << 8) | (uint32_t)(s - stdSound_aSlots));
}

// Interrupt context: the slot behind a callback token, or NULL if it is stale.
static gcSndSlot* stdSound_SlotFromToken(void* arg)
{
    uint32_t t = (uint32_t)(uintptr_t)arg;
    gcSndSlot* s = &stdSound_aSlots[(t & 0xFF) % GCSND_MAX_SLOTS];
    return (s->state == GCSND_SLOT_ACTIVE && s->gen == (t >> 8)) ? s : NULL;
}

// Give back the resources of slots whose voices are gone.
static void stdSound_SlotCollect(void)
{
    for (int i = 0; i < GCSND_MAX_SLOTS; i++) {
        gcSndSlot* s = &stdSound_aSlots[i];
        if (s->state != GCSND_SLOT_RELEASING || !s->bGone) continue;
        gcSample_Unref(s->pSample);
        free(s->aChunks[0]);
        free(s->aChunks[1]);
        s->pSample = NULL;
        s->aChunks[0] = s->aChunks[1] = NULL;
        s->pb = NULL;
        s->state = GCSND_SLOT_FREE;
    }
}

// Added: give back now the samples of voices that have been released. A released
// sound's PCM stays referenced by its voice slot until the audio callback has
// really freed the voice and the slot is collected (normally only when a new
// sound starts), so freeing idle sounds to make room didn't return the memory
// in time for the allocation that needed it (level loads failing with megabytes
// free a moment later). Waits up to ~40 ms for the voices to go.
void stdSound_GcReclaimWait(int maxTries);
void stdSound_GcReclaim(void) { stdSound_GcReclaimWait(8); }

void stdSound_GcReclaimWait(int maxTries)
{
    for (int tries = 0; tries < maxTries; tries++) {
        stdSound_SlotCollect();
        int bPending = 0;
        for (int i = 0; i < GCSND_MAX_SLOTS; i++)
            if (stdSound_aSlots[i].state == GCSND_SLOT_RELEASING) bPending = 1;
        if (!bPending) return;
        usleep(5000);
    }
}

static gcSndSlot* stdSound_SlotAlloc(stdSound_buffer_t* buf, gcSample* sample)
{
    stdSound_SlotCollect();
    for (int i = 0; i < GCSND_MAX_SLOTS; i++) {
        gcSndSlot* s = &stdSound_aSlots[i];
        if (s->state != GCSND_SLOT_FREE) continue;
        s->gen = (s->gen + 1) & GCSND_GEN_MASK;
        s->bGone = 0;
        s->buf = buf;
        s->pb = NULL;
        s->pSample = sample;
        sample->refs++;
        s->cur = 0;
        s->pos = 0;
        s->state = GCSND_SLOT_ACTIVE;
        return s;
    }
    return NULL;
}

// Detach the slot from its buffer and let its voice go.
static void stdSound_SlotRelease(gcSndSlot* s)
{
    u32 level;
    level = IRQ_Disable();
    s->buf = NULL;
    s->gen = (s->gen + 1) & GCSND_GEN_MASK; // outstanding tokens go stale
    s->bGone = 0;
    s->state = GCSND_SLOT_RELEASING;
    IRQ_Restore(level);
    gcVoice_Free(s->pb, &s->bGone);
}

static void gcLoop_Feed(AESNDPB* pb, gcSndSlot* l)
{
    uint8_t* chunk = l->aChunks[l->cur];
    l->cur ^= 1;
    uint32_t total = (uint32_t)l->pSample->bytes & ~3u;
    const uint8_t* src = (const uint8_t*)l->pSample->data;
    for (uint32_t done = 0; done < GCSND_LOOP_CHUNK; ) {
        uint32_t n = total - l->pos;
        if (n > GCSND_LOOP_CHUNK - done) n = GCSND_LOOP_CHUNK - done;
        memcpy(chunk + done, src + l->pos, n);
        done += n;
        l->pos += n;
        if (l->pos >= total) l->pos = 0;
    }
    DCFlushRange(chunk, GCSND_LOOP_CHUNK);
    AESND_SetVoiceBuffer(pb, chunk, GCSND_LOOP_CHUNK);
}

// Voice callback (DSP interrupt context; pb may be AESND's working copy).
static void stdSound_VoiceCallback(AESNDPB* pb, u32 state, void* arg)
{
    gcSndSlot* s = stdSound_SlotFromToken(arg);
    if (!s) {
        if (state != VOICE_STATE_STOPPED) AESND_SetVoiceStop(pb, true);
        return;
    }
    if (state == VOICE_STATE_STOPPED) {
        stdSound_buffer_t* buf = s->buf;
        if (buf) buf->bPlaying = 0;
    }
    else if (state == VOICE_STATE_STREAM && s->aChunks[0]) {
        gcLoop_Feed(pb, s);
    }
}

static void stdSound_ReleaseVoice(stdSound_buffer_t* buf)
{
    gcSndSlot* s = (gcSndSlot*)buf->pSlot;
    buf->pSlot = NULL;
    buf->pVoice = NULL;
    buf->bPlaying = 0;
    if (s) stdSound_SlotRelease(s);
}

// --- Cutscene stream -------------------------------------------------------------

#define GCSND_STREAM_RING   (256 * 1024)
// AESND (GameCube) copies voice data to ARAM in DSP_STREAMBUFFER_SIZE pieces and
// zero-pads the last piece of every buffer it is given: a stream chunk that isn't a
// multiple of it gets a burst of silence after every chunk (choppy, slow audio).
#define GCSND_STREAM_CHUNK  (4 * DSP_STREAMBUFFER_SIZE)

static uint8_t*          stdSound_pRing = NULL;
static volatile uint32_t stdSound_ringW = 0;   // absolute bytes written (engine)
static volatile uint32_t stdSound_ringR = 0;   // absolute bytes consumed (DSP callback)
static uint8_t*          stdSound_aChunks[2] = { NULL, NULL };
static int               stdSound_curChunk = 0;
static AESNDPB*          stdSound_pStreamVoice = NULL;
static stdSound_buffer_t* stdSound_pStreamOwner = NULL;
// The cutscene video is paced by the wall clock, so the stream must be too: silence
// played while the ring was starved (main thread stalled on the disc) is owed back
// by skipping that much audio once data arrives, instead of shifting all later
// audio behind the picture.
static volatile uint32_t stdSound_silenceDebt = 0;
static volatile uint32_t stdSound_silenceTotal = 0;

// Playback clock of the stream (see stdSound_GetStreamClockUs): chunks handed to
// the DSP so far, when the latest was requested, and the wall time at the start.
static volatile uint32_t stdSound_chunksFed = 0;
static volatile u64      stdSound_lastFeedTicks = 0;
static uint64_t          stdSound_streamStartUs = 0;
static uint32_t          stdSound_streamBytesPerSec = 0;

// Copy the next chunk out of the ring (silence on underrun) and hand it to the DSP.
static void stdSound_StreamFeed(AESNDPB* pb)
{
    uint8_t* chunk = stdSound_aChunks[stdSound_curChunk];
    stdSound_curChunk ^= 1;

    uint32_t avail = stdSound_ringW - stdSound_ringR;
    if (stdSound_silenceDebt && avail) {
        uint32_t skip = (stdSound_silenceDebt < avail ? stdSound_silenceDebt : avail) & ~3u;
        stdSound_ringR += skip;
        stdSound_silenceDebt -= skip;
        avail -= skip;
    }
    uint32_t give = avail < GCSND_STREAM_CHUNK ? avail : GCSND_STREAM_CHUNK;
    give &= ~3u; // whole stereo 16-bit frames
    uint32_t r = stdSound_ringR % GCSND_STREAM_RING;
    uint32_t first = give < GCSND_STREAM_RING - r ? give : GCSND_STREAM_RING - r;
    if (first) memcpy(chunk, stdSound_pRing + r, first);
    if (give > first) memcpy(chunk + first, stdSound_pRing, give - first);
    if (give < GCSND_STREAM_CHUNK) {
        memset(chunk + give, 0, GCSND_STREAM_CHUNK - give);
        stdSound_silenceDebt += GCSND_STREAM_CHUNK - give;
        stdSound_silenceTotal += GCSND_STREAM_CHUNK - give;
    }
    stdSound_ringR += give;

    DCFlushRange(chunk, GCSND_STREAM_CHUNK);
    AESND_SetVoiceBuffer(pb, chunk, GCSND_STREAM_CHUNK);
    stdSound_chunksFed++;
    stdSound_lastFeedTicks = gettime();
}

// Cutscene clock driven by the DSP's actual consumption of the stream, in the same
// units as Linux_TimeUs. Video follows this instead of the wall clock, so audio
// and picture stay locked even if the DSP runs a little fast or slow.
int stdSound_GetStreamClockUs(uint64_t* pUs)
{
    if (!stdSound_pStreamVoice || !stdSound_streamBytesPerSec || !stdSound_chunksFed)
        return 0;
    uint32_t fed = stdSound_chunksFed;
    u64 last = stdSound_lastFeedTicks;
    uint64_t chunkUs = (uint64_t)GCSND_STREAM_CHUNK * 1000000 / stdSound_streamBytesPerSec;
    // The chunk handed over at the latest request is queued behind the one playing.
    uint64_t since = ticks_to_microsecs(diff_ticks(last, gettime()));
    if (since > chunkUs) since = chunkUs;
    *pUs = stdSound_streamStartUs + (uint64_t)(fed - 1) * chunkUs + since;
    return 1;
}

// Bumped when the stream voice is released; its callback ignores (and stops) a
// voice from an older generation (see gcVoice.c).
static volatile uint32_t stdSound_streamGen = 1;

static void stdSound_StreamCallback(AESNDPB* pb, u32 state, void* arg)
{
    if ((uint32_t)(uintptr_t)arg != stdSound_streamGen || !stdSound_pStreamVoice) {
        if (state != VOICE_STATE_STOPPED) AESND_SetVoiceStop(pb, true);
        return;
    }
    if (state == VOICE_STATE_STREAM)
        stdSound_StreamFeed(pb);
}

static void stdSound_StreamStop(void)
{
    if (stdSound_pStreamVoice) {
        u32 level;
        level = IRQ_Disable();
        AESNDPB* pb = stdSound_pStreamVoice;
        stdSound_pStreamVoice = NULL;
        stdSound_streamGen++;
        IRQ_Restore(level);
        gcVoice_Free(pb, NULL);
    }
    if (stdSound_silenceTotal)
        stdPlatform_Printf("stdSound: cutscene stream starved for %u bytes\n", stdSound_silenceTotal);
    stdSound_pStreamOwner = NULL;
    // The 256 KiB ring is only needed while a cutscene plays (the stale-token check
    // above keeps the old voice's callback off it). The small DSP chunks stay.
    free(stdSound_pRing);
    stdSound_pRing = NULL;
    stdSound_ringW = stdSound_ringR = 0;
    stdSound_silenceDebt = stdSound_silenceTotal = 0;
    stdSound_chunksFed = 0;
    stdSound_streamBytesPerSec = 0;
}

// Append a cutscene chunk to the stream, starting the voice on the first one.
static void stdSound_StreamQueue(stdSound_buffer_t* owner, stdSound_buffer_t* chunk)
{
    gcSample* s = (gcSample*)chunk->pSample;
    if (!s || !s->bytes) return;

    if (!stdSound_pRing) {
        stdSound_pRing = (uint8_t*)malloc(GCSND_STREAM_RING);
        if (!stdSound_aChunks[0]) stdSound_aChunks[0] = (uint8_t*)memalign(32, GCSND_STREAM_CHUNK);
        if (!stdSound_aChunks[1]) stdSound_aChunks[1] = (uint8_t*)memalign(32, GCSND_STREAM_CHUNK);
        if (!stdSound_pRing || !stdSound_aChunks[0] || !stdSound_aChunks[1]) return;
    }

    // Drop the chunk if the ring is full (the cutscene runs ahead of the audio).
    uint32_t len = (uint32_t)s->bytes;
    if (stdSound_ringW - stdSound_ringR + len > GCSND_STREAM_RING) return;
    uint32_t w = stdSound_ringW % GCSND_STREAM_RING;
    uint32_t first = len < GCSND_STREAM_RING - w ? len : GCSND_STREAM_RING - w;
    memcpy(stdSound_pRing + w, s->data, first);
    if (len > first) memcpy(stdSound_pRing, (uint8_t*)s->data + first, len - first);
    stdSound_ringW += len;

    if (!stdSound_pStreamVoice) {
        stdSound_pStreamVoice = AESND_AllocateVoiceWithArg(stdSound_StreamCallback,
                                                           (void*)(uintptr_t)stdSound_streamGen);
        if (!stdSound_pStreamVoice) return;
        stdSound_pStreamOwner = owner;
        stdSound_chunksFed = 0;
        stdSound_streamStartUs = Linux_TimeUs();
        stdSound_streamBytesPerSec = chunk->nSamplesPerSec * (chunk->bStereo ? 2 : 1) * (chunk->bitsPerSample / 8);
        AESND_SetVoiceFormat(stdSound_pStreamVoice, stdSound_VoiceFormat(chunk));
        AESND_SetVoiceFrequency(stdSound_pStreamVoice, (f32)chunk->nSamplesPerSec);
        u16 vol = (u16)(stdMath_Clamp(owner->vol, 0.0, 1.0) * GCSND_VOLUME_MAX);
        AESND_SetVoiceVolume(stdSound_pStreamVoice, vol, vol);
        AESND_SetVoiceStream(stdSound_pStreamVoice, true);
        stdSound_StreamFeed(stdSound_pStreamVoice);
        AESND_SetVoiceStop(stdSound_pStreamVoice, false);
    }
}

// --- stdSound API ----------------------------------------------------------------

int stdSound_Startup()
{
    // The engine's 2D mixer drives volume/pan; there is no 3D audio hardware path.
    jkGuiSound_b3DSound = 0;

    if (!stdSound_bReady) {
        // AESND_Init() already ran in gcSystem_Init (it must precede the ARAM setup).
        AESND_Pause(false);
        stdSound_bReady = 1;
        stdPlatform_Printf("stdSound: AESND ready\n");
    }
    return 1;
}

void stdSound_Shutdown()
{
    stdSound_StreamStop();
}

void stdSound_SetMenuVolume(flex_t a1)
{
    stdSound_fMenuVolume = a1;
}

stdSound_buffer_t* stdSound_BufferCreate(int bStereo, uint32_t nSamplesPerSec, uint16_t bitsPerSample, int bufferLen)
{
    stdSound_buffer_t* out = (stdSound_buffer_t*)STD_ALLOC(sizeof(stdSound_buffer_t));
    if (!out)
        return NULL;

    _memset(out, 0, sizeof(*out));
    out->bStereo = bStereo;
    out->bufferLen = bufferLen;
    out->nSamplesPerSec = nSamplesPerSec;
    out->bitsPerSample = bitsPerSample;
    out->refcnt = 1;
    out->vol = 1.0 * stdSound_fMenuVolume;
    return out;
}

void* stdSound_BufferSetData(stdSound_buffer_t* sound, int bufferBytes, int32_t* bufferMaxSize)
{
    stdSound_ReleaseVoice(sound);
    gcSample_Unref((gcSample*)sound->pSample);
    sound->pSample = NULL;
    sound->data = NULL;

    gcSample* s = gcSample_New(bufferBytes);
    if (!s) {
        if (bufferMaxSize) *bufferMaxSize = 0;
        return NULL;
    }
    sound->pSample = s;
    sound->data = s->data;
    sound->bufferBytes = bufferBytes;
    if (bufferMaxSize)
        *bufferMaxSize = bufferBytes;
    return s->data;
}

int stdSound_BufferUnlock(stdSound_buffer_t* sound, void* buffer, int bufferRead)
{
    gcSample* s = (gcSample*)sound->pSample;
    // A short read trims the sample; 0 means "the whole buffer" (the cutscene
    // player always passes 0).
    if (s && bufferRead > 0 && bufferRead <= s->bytes)
        s->bytes = bufferRead;
    if (s) DCFlushRange(s->data, (s->bytes + 31) & ~31);
    return 1;
}

int stdSound_BufferPlay(stdSound_buffer_t* buf, int loop)
{
    if (!stdSound_bReady || !buf) return 0;
    gcSample* s = (gcSample*)buf->pSample;
    if (!s || !s->bytes) return 0;

    stdSound_ReleaseVoice(buf);
    f32 freq = (f32)(buf->freqHz ? buf->freqHz : buf->nSamplesPerSec);

    gcSndSlot* slot = stdSound_SlotAlloc(buf, s);
    if (!slot) return 0;
    int bStream = loop && (s->bytes & ~3) > 0;
    if (bStream) {
        slot->aChunks[0] = (uint8_t*)memalign(32, GCSND_LOOP_CHUNK);
        slot->aChunks[1] = (uint8_t*)memalign(32, GCSND_LOOP_CHUNK);
    }
    AESNDPB* pb = (!bStream || (slot->aChunks[0] && slot->aChunks[1]))
                ? AESND_AllocateVoiceWithArg(stdSound_VoiceCallback, stdSound_SlotToken(slot)) : NULL;
    slot->pb = pb;
    if (!pb) {
        stdSound_SlotRelease(slot); // no voice: gcVoice marks it gone right away
        return 0;
    }
    buf->pSlot = slot;
    buf->pVoice = pb;

    // A new voice starts stopped, and AESND never mixes stopped voices, so it can
    // be set up safely before it is started.
    if (bStream)
    {
        buf->bLooping = 1;
        buf->bPlaying = 1;
        AESND_SetVoiceFormat(pb, stdSound_VoiceFormat(buf));
        AESND_SetVoiceFrequency(pb, freq);
        stdSound_ApplyVolume(buf);
        AESND_SetVoiceStream(pb, true);
        gcLoop_Feed(pb, slot);
        AESND_SetVoiceStop(pb, false);
        return 1;
    }

    buf->bLooping = loop;
    buf->bPlaying = 1;
    stdSound_ApplyVolume(buf);
    AESND_PlayVoice(pb, stdSound_VoiceFormat(buf), s->data, (u32)s->bytes & ~3u, freq, 0, loop ? true : false);
    return 1;
}

int stdSound_BufferQueueAfterAnother(stdSound_buffer_t* bufPrev, stdSound_buffer_t* bufNext)
{
    if (!stdSound_bReady || !bufPrev || !bufNext) return 0;
    stdSound_StreamQueue(bufPrev, bufNext);
    return 1;
}

void stdSound_BufferUnqueueProcessed(stdSound_buffer_t* buf)
{
}

void stdSound_BufferRelease(stdSound_buffer_t* sound)
{
    if (!sound) return;
    if (sound == stdSound_pStreamOwner)
        stdSound_StreamStop();
    stdSound_ReleaseVoice(sound);
    gcSample_Unref((gcSample*)sound->pSample);
    memset(sound, 0, sizeof(*sound));
    STD_FREE(sound);
}

int stdSound_BufferReset(stdSound_buffer_t* sound)
{
    return 1;
}

void stdSound_BufferSetPan(stdSound_buffer_t* a1, flex_t a2)
{
    if (!a1) return;
    a1->pan = a2;
    stdSound_ApplyVolume(a1);
}

void stdSound_BufferSetFrequency(stdSound_buffer_t* sound, int freq)
{
    if (!sound) return;
    sound->freqHz = freq;
    if (sound->pVoice)
        AESND_SetVoiceFrequency((AESNDPB*)sound->pVoice, (f32)freq);
}

stdSound_buffer_t* stdSound_BufferDuplicate(stdSound_buffer_t* sound)
{
    stdSound_buffer_t* out = (stdSound_buffer_t*)STD_ALLOC(sizeof(stdSound_buffer_t));
    if (!out)
        return NULL;

    _memset(out, 0, sizeof(*out));
    out->data = sound->data;
    out->bStereo = sound->bStereo;
    out->bufferLen = sound->bufferLen;
    out->nSamplesPerSec = sound->nSamplesPerSec;
    out->bitsPerSample = sound->bitsPerSample;
    out->refcnt = 1;
    out->vol = sound->vol;
    out->format = sound->format;
    out->bufferBytes = sound->bufferBytes;
    out->bIsCopy = 1;
    out->pSample = sound->pSample;
    if (out->pSample)
        ((gcSample*)out->pSample)->refs++;
    return out;
}

void stdSound_IA3D_idk(flex_t a)
{
}

int stdSound_BufferStop(stdSound_buffer_t* buf)
{
    if (!buf) return 0;
    stdSound_ReleaseVoice(buf);
    return 1;
}

void stdSound_BufferSetVolume(stdSound_buffer_t* sound, flex_t vol)
{
    if (!sound) return;
    // The SFX volume setting scales every volume the mixer sets, as in the other
    // ports (it was only applied when a buffer was created, so the mixer's own
    // volumes -- distance, every sound -- overrode it and the slider did nothing).
    sound->vol = vol * stdSound_fMenuVolume;
    stdSound_ApplyVolume(sound);
}

int stdSound_3DSetMode(stdSound_buffer_t* a1, int a2)
{
    return 1;
}

stdSound_3dBuffer_t* stdSound_BufferQueryInterface(stdSound_buffer_t* pSoundBuffer)
{
    return pSoundBuffer;
}

void stdSound_CommitDeferredSettings()
{
}

void stdSound_SetPositionOrientation(rdVector3 *pos, rdVector3 *lvec, rdVector3 *uvec)
{
}

void stdSound_SetPosition(stdSound_buffer_t* sound, rdVector3 *pos)
{
    if (sound && pos) sound->pos = *pos;
}

void stdSound_SetVelocity(stdSound_buffer_t* sound, rdVector3 *vel)
{
    if (sound && vel) sound->vel = *vel;
}

int stdSound_IsPlaying(stdSound_buffer_t* sound, rdVector3 *pos)
{
    if (!sound) return 0;
    if (pos) *pos = sound->pos;
    if (sound->pVoice && !sound->bPlaying)
        stdSound_ReleaseVoice(sound); // finished one-shot: give the voice back
    return sound->bPlaying;
}

void stdSound_3DBufferRelease(stdSound_3dBuffer_t* p3DBuffer)
{
}

#endif // STDSOUND_GAMECUBE
