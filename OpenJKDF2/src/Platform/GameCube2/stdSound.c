// stdSound backend for the Nintendo GameCube, libogc2 core: libansnd.
//
// Selected by STDSOUND_GAMECUBE. libansnd mixes up to 48 voices on the DSP and,
// on GameCube, plays them straight from ARAM -- PCM, or DSP-ADPCM decoded by the
// DSP itself. So:
//  - sounds from the sound bank (gcSndBank: already in ARAM since the level
//    loaded) play as ADPCM from there: no decoding by the CPU, nothing in main
//    RAM (the libogc core decodes each into a 1.5 MiB arena of main RAM first);
//  - PCM the engine fills in (stdSound_BufferSetData/Unlock: WAVs outside the
//    bank) is sent to ARAM the first time it plays, and its main RAM copy freed;
//  - loops are libansnd's own (no click at the loop point);
//  - cutscene audio, queued buffer after buffer (stdSound_BufferQueueAfterAnother),
//    goes through a ring in main RAM to a gcStream voice.
//
// Positional audio is the engine's 2D path (jkGuiSound_b3DSound = 0): volume and
// pan per buffer, mapped onto the voice's left/right volume.

#include <unistd.h>
#include "Win95/stdSound.h"

#include "Gui/jkGUISound.h"
#include "General/stdMath.h"
#include "stdPlatform.h"

#include "jk.h"

#ifdef STDSOUND_GAMECUBE

#include "gcOgc.h"
#include "gcAram.h"
#include "gcStream.h"
#include <ansndlib.h>
#include <ogc/lwp_watchdog.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

// --- Refcounted samples --------------------------------------------------------
// DirectSound semantics: duplicated buffers share the sample. The engine writes
// PCM into `mem` (main RAM); it moves to ARAM when first played. ADPCM samples
// point at the sound bank's block in ARAM (not ours to free).
enum { GCSND_PCM, GCSND_ADPCM };

typedef struct gcSample {
    int      refs;
    int      kind;
    int      channels;
    int      bits;        // PCM: 8 or 16
    uint8_t* mem;         // PCM in main RAM until sent to ARAM (32-byte aligned)
    int      bytes;       // PCM bytes
    uint32_t aram;        // PCM: our ARAM copy (0 = not yet); ADPCM: the bank's data
    uint32_t aramBytes;
    // ADPCM
    uint32_t numSamples;  // per channel
    uint32_t chBytes;     // ADPCM bytes per channel
    u16      coefs[2][16];
    u16      ps[2];       // predictor/scale of each channel's first frame
} gcSample;

static int stdSound_bReady = 0;

static gcSample* gcSample_NewPcm(int bytes, int channels, int bits)
{
    gcSample* s = (gcSample*)calloc(1, sizeof(gcSample));
    if (!s) return NULL;
    s->refs = 1;
    s->kind = GCSND_PCM;
    s->channels = channels;
    s->bits = bits;
    s->bytes = bytes;
    s->mem = (uint8_t*)memalign(32, (bytes + 31) & ~31);
    if (!s->mem) { free(s); return NULL; }
    memset(s->mem, 0, (bytes + 31) & ~31);
    return s;
}

static void gcSample_Unref(gcSample* s)
{
    if (!s || --s->refs > 0) return;
    if (s->kind == GCSND_PCM && s->aram) gcAram_Free(s->aram, s->aramBytes);
    free(s->mem);
    free(s);
}

// PCM to ARAM (main thread, when first played). libansnd takes signed 8-bit
// PCM: WAV's unsigned bytes are flipped on the way.
static int gcSample_ToAram(gcSample* s)
{
    if (s->kind != GCSND_PCM || s->aram) return 1;
    if (!s->mem || s->bytes <= 0) return 0;
    uint32_t len = (s->bytes + 31) & ~31u;
    uint32_t aram = gcAram_Alloc(len);
    if (!aram) return 0;
    if (s->bits == 8)
        for (int i = 0; i < s->bytes; i++) s->mem[i] ^= 0x80;
    gcAram_Write(aram, s->mem, len);
    s->aram = aram;
    s->aramBytes = len;
    free(s->mem);
    s->mem = NULL;
    return 1;
}

// --- Voice slots -----------------------------------------------------------------
// Each playing buffer owns a slot with its voice(s) -- two linked ones for stereo
// ADPCM, one per channel. The voice callback (DSP interrupt) gets a token (slot
// index + generation), not a pointer: a stale token after the buffer is gone is
// ignored. A released slot keeps its sample for a moment, until the DSP is surely
// done reading it.
#define GCSND_MAX_SLOTS   48
#define GCSND_GEN_MASK    0xFFFFFF
#define GCSND_RELEASE_MS  30

enum { GCSND_SLOT_FREE, GCSND_SLOT_ACTIVE, GCSND_SLOT_RELEASING };

typedef struct gcSndSlot {
    volatile int       state;
    volatile uint32_t  gen;
    stdSound_buffer_t* volatile buf;
    s32                aVoice[2];
    int                numVoices;
    gcSample*          pSample;
    u64                releasedAt;
} gcSndSlot;

static gcSndSlot stdSound_aSlots[GCSND_MAX_SLOTS];

static void* stdSound_SlotToken(gcSndSlot* s)
{
    return (void*)(uintptr_t)((s->gen << 8) | (uint32_t)(s - stdSound_aSlots));
}

static void stdSound_VoiceCallback(void* user, s32 state)
{
    uint32_t t = (uint32_t)(uintptr_t)user;
    gcSndSlot* s = &stdSound_aSlots[(t & 0xFF) % GCSND_MAX_SLOTS];
    if (s->state != GCSND_SLOT_ACTIVE || s->gen != (t >> 8)) return;
    if (state == ANSND_VOICE_STATE_FINISHED || state == ANSND_VOICE_STATE_ERROR) {
        stdSound_buffer_t* buf = s->buf;
        if (buf) buf->bPlaying = 0;
    }
}

// Give back the samples of slots released long enough ago.
static void stdSound_SlotCollect(int bForce)
{
    u64 now = gettime();
    for (int i = 0; i < GCSND_MAX_SLOTS; i++) {
        gcSndSlot* s = &stdSound_aSlots[i];
        if (s->state != GCSND_SLOT_RELEASING) continue;
        if (!bForce && diff_msec(s->releasedAt, now) < GCSND_RELEASE_MS) continue;
        gcSample_Unref(s->pSample);
        s->pSample = NULL;
        s->state = GCSND_SLOT_FREE;
    }
}

// Engine hooks (sithSound): give back released sounds' memory now.
void stdSound_GcReclaimWait(int maxTries)
{
    (void)maxTries;
    usleep(GCSND_RELEASE_MS * 1000);
    stdSound_SlotCollect(1);
}
void stdSound_GcReclaim(void) { stdSound_SlotCollect(0); }

static gcSndSlot* stdSound_SlotAlloc(stdSound_buffer_t* buf, gcSample* sample)
{
    stdSound_SlotCollect(0);
    for (int i = 0; i < GCSND_MAX_SLOTS; i++) {
        gcSndSlot* s = &stdSound_aSlots[i];
        if (s->state != GCSND_SLOT_FREE) continue;
        s->gen = (s->gen + 1) & GCSND_GEN_MASK;
        s->buf = buf;
        s->numVoices = 0;
        s->pSample = sample;
        sample->refs++;
        s->state = GCSND_SLOT_ACTIVE;
        return s;
    }
    return NULL;
}

// Stop the slot's voices and keep its sample for GCSND_RELEASE_MS.
static void stdSound_SlotRelease(gcSndSlot* s)
{
    u32 level = IRQ_Disable();
    s->buf = NULL;
    s->gen = (s->gen + 1) & GCSND_GEN_MASK; // outstanding tokens go stale
    s->state = GCSND_SLOT_RELEASING;
    IRQ_Restore(level);
    for (int v = 0; v < s->numVoices; v++) {
        ansnd_stop_voice(s->aVoice[v]);
        ansnd_deallocate_voice(s->aVoice[v]);
    }
    s->numVoices = 0;
    s->releasedAt = gettime();
}

static void stdSound_ReleaseVoice(stdSound_buffer_t* buf)
{
    gcSndSlot* s = (gcSndSlot*)buf->pSlot;
    buf->pSlot = NULL;
    buf->pVoice = NULL;
    buf->bPlaying = 0;
    if (s) stdSound_SlotRelease(s);
}

static void stdSound_ApplyVolume(stdSound_buffer_t* buf)
{
    gcSndSlot* s = (gcSndSlot*)buf->pSlot;
    if (!s || !s->numVoices) return;
    float vol = (float)stdMath_Clamp(buf->vol, 0.0, 1.0);
    float pan = (float)stdMath_Clamp(buf->pan, -1.0, 1.0);
    float l = vol * (pan > 0.0f ? 1.0f - pan : 1.0f);
    float r = vol * (pan < 0.0f ? 1.0f + pan : 1.0f);
    if (s->numVoices == 2) { // stereo ADPCM: one voice per channel
        ansnd_set_voice_volume(s->aVoice[0], l, 0.0f);
        ansnd_set_voice_volume(s->aVoice[1], 0.0f, r);
    }
    else
        ansnd_set_voice_volume(s->aVoice[0], l, r);
}

static void stdSound_ApplyPitch(stdSound_buffer_t* buf)
{
    gcSndSlot* s = (gcSndSlot*)buf->pSlot;
    if (!s || !buf->freqHz || !buf->nSamplesPerSec) return;
    float pitch = (float)buf->freqHz / (float)buf->nSamplesPerSec;
    for (int v = 0; v < s->numVoices; v++)
        ansnd_set_voice_pitch(s->aVoice[v], pitch);
}

// --- Cutscene stream -------------------------------------------------------------

#define GCSND_STREAM_RING   (256 * 1024)
#define GCSND_STREAM_CHUNK  (8 * 1024)

static uint8_t*          stdSound_pRing = NULL;
static volatile uint32_t stdSound_ringW = 0;   // absolute bytes written (engine)
static volatile uint32_t stdSound_ringR = 0;   // absolute bytes consumed (DSP interrupt)
static gcStream          stdSound_stream;
static stdSound_buffer_t* stdSound_pStreamOwner = NULL;
// The cutscene video is paced by the wall clock, so the stream must be too: silence
// played while the ring was starved (main thread stalled on the disc) is owed back
// by skipping that much audio once data arrives, instead of shifting all later
// audio behind the picture.
static volatile uint32_t stdSound_silenceDebt = 0;
static volatile uint32_t stdSound_silenceTotal = 0;
static uint64_t          stdSound_streamStartUs = 0;
static uint32_t          stdSound_streamBytesPerSec = 0;

// Stream fill (DSP interrupt): the next chunk out of the ring, silence on underrun.
static void stdSound_StreamFill(void* ctx, uint8_t* chunk, uint32_t len)
{
    uint32_t avail = stdSound_ringW - stdSound_ringR;
    if (stdSound_silenceDebt && avail) {
        uint32_t skip = (stdSound_silenceDebt < avail ? stdSound_silenceDebt : avail) & ~3u;
        stdSound_ringR += skip;
        stdSound_silenceDebt -= skip;
        avail -= skip;
    }
    uint32_t give = avail < len ? avail : len;
    give &= ~3u; // whole stereo 16-bit frames
    uint32_t r = stdSound_ringR % GCSND_STREAM_RING;
    uint32_t first = give < GCSND_STREAM_RING - r ? give : GCSND_STREAM_RING - r;
    if (first) memcpy(chunk, stdSound_pRing + r, first);
    if (give > first) memcpy(chunk + first, stdSound_pRing, give - first);
    if (give < len) {
        memset(chunk + give, 0, len - give);
        // (Not owed for the two chunks primed before the voice starts.)
        if (stdSound_stream.chunksFed > 1) {
            stdSound_silenceDebt += len - give;
            stdSound_silenceTotal += len - give;
        }
    }
    stdSound_ringR += give;
}

// Cutscene clock driven by the DSP's actual consumption of the stream, in the same
// units as Linux_TimeUs. Video follows this instead of the wall clock, so audio
// and picture stay locked even if the DSP runs a little fast or slow.
int stdSound_GetStreamClockUs(uint64_t* pUs)
{
    if (!gcStream_IsPlaying(&stdSound_stream) || !stdSound_streamBytesPerSec)
        return 0;
    uint32_t fed = stdSound_stream.chunksFed;
    u64 last = stdSound_stream.lastFeedTicks;
    uint64_t chunkUs = (uint64_t)GCSND_STREAM_CHUNK * 1000000 / stdSound_streamBytesPerSec;
    // The chunk handed over at the latest request is queued behind the one playing.
    uint64_t since = ticks_to_microsecs(diff_ticks(last, gettime()));
    if (since > chunkUs) since = chunkUs;
    *pUs = stdSound_streamStartUs + (uint64_t)(fed - 1) * chunkUs + since;
    return 1;
}

static void stdSound_StreamStop(void)
{
    gcStream_Stop(&stdSound_stream);
    if (stdSound_silenceTotal)
        stdPlatform_Printf("stdSound: cutscene stream starved for %u bytes\n", stdSound_silenceTotal);
    stdSound_pStreamOwner = NULL;
    // The ring is only needed while a cutscene plays.
    free(stdSound_pRing);
    stdSound_pRing = NULL;
    stdSound_ringW = stdSound_ringR = 0;
    stdSound_silenceDebt = stdSound_silenceTotal = 0;
    stdSound_streamBytesPerSec = 0;
}

// Append a cutscene chunk to the stream (as 16-bit), starting it on the first one.
static void stdSound_StreamQueue(stdSound_buffer_t* owner, stdSound_buffer_t* chunk)
{
    gcSample* s = (gcSample*)chunk->pSample;
    if (!s || !s->mem || !s->bytes) return;
    if (!stdSound_pRing) {
        stdSound_pRing = (uint8_t*)malloc(GCSND_STREAM_RING);
        if (!stdSound_pRing) return;
    }

    // 8-bit (unsigned) chunks are widened to 16-bit big-endian.
    int wide = s->bits == 8;
    uint32_t len = (uint32_t)s->bytes * (wide ? 2 : 1);
    // Drop the chunk if the ring is full (the cutscene runs ahead of the audio).
    if (stdSound_ringW - stdSound_ringR + len > GCSND_STREAM_RING) return;
    uint32_t w = stdSound_ringW % GCSND_STREAM_RING;
    if (wide) {
        for (int i = 0; i < s->bytes; i++) {
            stdSound_pRing[(w + i * 2) % GCSND_STREAM_RING] = s->mem[i] ^ 0x80;
            stdSound_pRing[(w + i * 2 + 1) % GCSND_STREAM_RING] = 0;
        }
    }
    else {
        uint32_t first = len < GCSND_STREAM_RING - w ? len : GCSND_STREAM_RING - w;
        memcpy(stdSound_pRing + w, s->mem, first);
        if (len > first) memcpy(stdSound_pRing, s->mem + first, len - first);
    }
    stdSound_ringW += len;

    if (!gcStream_IsPlaying(&stdSound_stream)) {
        stdSound_pStreamOwner = owner;
        stdSound_streamStartUs = Linux_TimeUs();
        stdSound_streamBytesPerSec = chunk->nSamplesPerSec * (chunk->bStereo ? 2 : 1) * 2;
        float vol = (float)stdMath_Clamp(owner->vol, 0.0, 1.0);
        gcStream_Start(&stdSound_stream, chunk->nSamplesPerSec, chunk->bStereo, vol,
                       GCSND_STREAM_CHUNK, stdSound_StreamFill, NULL);
    }
}

// --- Playing a sample ------------------------------------------------------------

static int stdSound_StartPcm(stdSound_buffer_t* buf, gcSndSlot* slot, gcSample* s, int loop)
{
    s32 v = ansnd_allocate_voice();
    if (v < 0) return 0;
    slot->aVoice[0] = v;
    slot->numVoices = 1;
    int frameBytes = s->channels * (s->bits / 8);
    uint32_t frames = (uint32_t)s->bytes / frameBytes;
    ansnd_pcm_voice_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.samplerate = buf->nSamplesPerSec;
    cfg.format = s->bits == 8 ? ANSND_VOICE_PCM_FORMAT_SIGNED_8_PCM : ANSND_VOICE_PCM_FORMAT_SIGNED_16_PCM;
    cfg.channels = s->channels;
    cfg.pitch = 1.0f;
    cfg.frame_data_ptr = s->aram;
    cfg.frame_count = frames;
    if (loop && frames > 1) cfg.loop_end_offset = frames - 1;
    cfg.voice_callback = stdSound_VoiceCallback;
    cfg.user_pointer = stdSound_SlotToken(slot);
    return ansnd_configure_pcm_voice(v, &cfg) >= 0;
}

static int stdSound_StartAdpcm(stdSound_buffer_t* buf, gcSndSlot* slot, gcSample* s, int loop)
{
    for (int c = 0; c < s->channels; c++) {
        s32 v = ansnd_allocate_voice();
        if (v < 0) return 0;
        slot->aVoice[c] = v;
        slot->numVoices = c + 1;
        ansnd_adpcm_voice_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.samplerate = buf->nSamplesPerSec;
        cfg.pitch = 1.0f;
        cfg.data_ptr = s->aram + c * s->chBytes;
        cfg.sample_count = s->numSamples;
        memcpy(cfg.decode_coefficients, s->coefs[c], sizeof(cfg.decode_coefficients));
        cfg.initial_predictor_scale = s->ps[c];
        if (loop && s->numSamples > 1) {
            cfg.loop_flag = 1;
            cfg.loop_predictor_scale = s->ps[c];
            cfg.loop_start_offset = 0;
            cfg.loop_end_offset = s->numSamples - 1;
        }
        cfg.voice_callback = stdSound_VoiceCallback;
        cfg.user_pointer = stdSound_SlotToken(slot);
        if (ansnd_configure_adpcm_voice(v, &cfg) < 0) return 0;
    }
    // Both channels start on the same DSP frame.
    if (slot->numVoices == 2) ansnd_link_voices(slot->aVoice[0], slot->aVoice[1]);
    return 1;
}

// --- stdSound API ----------------------------------------------------------------

int stdSound_Startup()
{
    // The engine's 2D mixer drives volume/pan; there is no 3D audio hardware path.
    jkGuiSound_b3DSound = 0;
    if (!stdSound_bReady) {
        // ansnd_initialize already ran in gcSystem_Init (before the ARAM setup).
        stdSound_bReady = 1;
        stdPlatform_Printf("stdSound: libansnd ready\n");
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

    gcSample* s = gcSample_NewPcm(bufferBytes, sound->bStereo ? 2 : 1, sound->bitsPerSample == 8 ? 8 : 16);
    if (!s) {
        if (bufferMaxSize) *bufferMaxSize = 0;
        return NULL;
    }
    sound->pSample = s;
    sound->data = s->mem;
    sound->bufferBytes = bufferBytes;
    if (bufferMaxSize)
        *bufferMaxSize = bufferBytes;
    return s->mem;
}

int stdSound_BufferUnlock(stdSound_buffer_t* sound, void* buffer, int bufferRead)
{
    gcSample* s = (gcSample*)sound->pSample;
    // A short read trims the sample; 0 means "the whole buffer" (the cutscene
    // player always passes 0).
    if (s && s->kind == GCSND_PCM && bufferRead > 0 && bufferRead <= s->bytes)
        s->bytes = bufferRead;
    return 1;
}

// Added (libogc2 core, called by sithSound): the buffer plays a sound of the bank
// straight from its ARAM block (gcSndBank_LoadToAram: 64 bytes of coefficients,
// then each channel's ADPCM).
int stdSound_GcBufferSetAdpcm(stdSound_buffer_t* sound, uint32_t aram, int channels, uint32_t numSamples)
{
    stdSound_ReleaseVoice(sound);
    gcSample_Unref((gcSample*)sound->pSample);
    sound->pSample = NULL;
    sound->data = NULL;
    if (channels < 1 || channels > 2 || !numSamples) return 0;

    gcSample* s = (gcSample*)calloc(1, sizeof(gcSample));
    if (!s) return 0;
    s->refs = 1;
    s->kind = GCSND_ADPCM;
    s->channels = channels;
    s->numSamples = numSamples;
    s->chBytes = (((numSamples + 13) / 14) * 8 + 31) & ~31u;
    s->aram = aram + 64;
    static u8 head[64] __attribute__((aligned(32)));
    gcAram_Read(head, aram, 64);
    memcpy(s->coefs, head, sizeof(s->coefs));
    for (int c = 0; c < channels; c++) {
        gcAram_Read(head, s->aram + c * s->chBytes, 32);
        s->ps[c] = head[0];
    }
    sound->pSample = s;
    return 1;
}

int stdSound_BufferPlay(stdSound_buffer_t* buf, int loop)
{
    if (!stdSound_bReady || !buf) return 0;
    gcSample* s = (gcSample*)buf->pSample;
    if (!s || (s->kind == GCSND_PCM && !s->bytes)) return 0;
    if (!gcSample_ToAram(s)) return 0; // no room in ARAM

    stdSound_ReleaseVoice(buf);
    gcSndSlot* slot = stdSound_SlotAlloc(buf, s);
    if (!slot) return 0;
    buf->pSlot = slot;
    int ok = s->kind == GCSND_ADPCM ? stdSound_StartAdpcm(buf, slot, s, loop)
                                    : stdSound_StartPcm(buf, slot, s, loop);
    if (ok) {
        buf->pVoice = slot;
        buf->bLooping = loop;
        buf->bPlaying = 1;
        stdSound_ApplyVolume(buf);
        stdSound_ApplyPitch(buf);
        ok = ansnd_start_voice(slot->aVoice[0]) >= 0;
        if (ok && slot->numVoices == 2) ansnd_start_voice(slot->aVoice[1]);
    }
    if (!ok) {
        stdSound_ReleaseVoice(buf);
        return 0;
    }
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
    stdSound_ApplyPitch(sound);
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
    // The SFX volume setting scales every volume the mixer sets.
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
        stdSound_ReleaseVoice(sound); // finished one-shot: give the voices back
    return sound->bPlaying;
}

void stdSound_3DBufferRelease(stdSound_3dBuffer_t* p3DBuffer)
{
}

#endif // STDSOUND_GAMECUBE
