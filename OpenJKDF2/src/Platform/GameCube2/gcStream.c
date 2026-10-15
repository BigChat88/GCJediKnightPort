// PCM streams on libansnd (see gcStream.h).

#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <ansndlib.h>
#include "gcStream.h"
#include "gcAram.h"

#ifdef STDSOUND_GAMECUBE

// Fill buffer i and send it to ARAM (DSP interrupt, or the caller at start).
#define GCSTREAM_MAX 4
static ARQRequest gcStream_aReq[GCSTREAM_MAX][3];
static int        gcStream_numIds = 0;

static void gcStream_Prepare(gcStream* s, int i, int bWait)
{
    uint8_t* stage = s->aStage[i];
    s->fill(s->ctx, stage, s->chunkBytes);
    DCFlushRange(stage, s->chunkBytes);
    ARQRequest* req = &gcStream_aReq[s->id - 1][i];
    if (bWait)
        ARQ_PostRequest(req, 0x4753, ARQ_MRAMTOARAM, ARQ_PRIO_HI, s->aAram[i],
                        (u32)MEM_VIRTUAL_TO_PHYSICAL(stage), s->chunkBytes);
    else
        ARQ_PostRequestAsync(req, 0x4753, ARQ_MRAMTOARAM, ARQ_PRIO_HI, s->aAram[i],
                             (u32)MEM_VIRTUAL_TO_PHYSICAL(stage), s->chunkBytes, NULL);
}

// libansnd asks for the buffer to play after the current one (DSP interrupt):
// hand over the one filled earlier and fill the one after it (the buffer played
// before the current one, done by now).
static void gcStream_Callback(void* user, ansnd_pcm_data_buffer_t* out)
{
    gcStream* s = (gcStream*)user;
    if (s->voice < 0) return;
    int i = s->next;
    out->frame_data_ptr = s->aAram[i];
    out->frame_count = s->chunkBytes / s->frameBytes;
    s->next = (i + 1) % 3;
    gcStream_Prepare(s, s->next, 0);
    s->chunksFed++;
    s->lastFeedTicks = gettime();
}

int gcStream_Start(gcStream* s, uint32_t rate, int bStereo, float vol, uint32_t chunkBytes,
                   gcStreamFill fill, void* ctx)
{
    if (!s->id) {
        // (A zeroed gcStream has voice 0, a valid libansnd id: not ours yet.)
        if (gcStream_numIds >= GCSTREAM_MAX) return 0;
        s->id = ++gcStream_numIds;
        s->voice = -1;
    }
    gcStream_Stop(s);
    chunkBytes = (chunkBytes + 31) & ~31u;
    if (s->chunkBytes != chunkBytes) {
        for (int i = 0; i < 3; i++) {
            if (s->aAram[i]) gcAram_Free(s->aAram[i], s->chunkBytes);
            free(s->aStage[i]);
            s->aAram[i] = 0;
            s->aStage[i] = NULL;
        }
        s->chunkBytes = chunkBytes;
    }
    for (int i = 0; i < 3; i++) {
        if (!s->aAram[i]) s->aAram[i] = gcAram_Alloc(chunkBytes);
        if (!s->aStage[i]) s->aStage[i] = (uint8_t*)memalign(32, chunkBytes);
        if (!s->aAram[i] || !s->aStage[i]) return 0;
    }
    s->frameBytes = bStereo ? 4 : 2;
    s->fill = fill;
    s->ctx = ctx;
    s->chunksFed = 1;
    s->lastFeedTicks = gettime();

    // The first two buffers before the voice starts: it plays 0, asks for 1.
    gcStream_Prepare(s, 0, 1);
    gcStream_Prepare(s, 1, 1);
    s->next = 1;

    s32 voice = ansnd_allocate_voice();
    if (voice < 0) return 0;
    ansnd_pcm_voice_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.samplerate = rate;
    cfg.format = ANSND_VOICE_PCM_FORMAT_SIGNED_16_PCM;
    cfg.channels = bStereo ? 2 : 1;
    cfg.pitch = 1.0f;
    cfg.left_volume = vol;
    cfg.right_volume = vol;
    cfg.frame_data_ptr = s->aAram[0];
    cfg.frame_count = chunkBytes / s->frameBytes;
    cfg.stream_callback = gcStream_Callback;
    cfg.user_pointer = s;
    s->voice = voice; // before the voice can call back
    if (ansnd_configure_pcm_voice(voice, &cfg) < 0 || ansnd_start_voice(voice) < 0) {
        s->voice = -1;
        ansnd_deallocate_voice(voice);
        return 0;
    }
    return 1;
}

void gcStream_Stop(gcStream* s)
{
    s32 voice = s->voice;
    if (!s->id || voice < 0) return;
    s->voice = -1; // its callback does nothing from now on
    ansnd_stop_voice(voice);
    ansnd_deallocate_voice(voice);
}

void gcStream_SetVolume(gcStream* s, float vol)
{
    if (s->id && s->voice >= 0)
        ansnd_set_voice_volume(s->voice, vol, vol);
}

int gcStream_IsPlaying(const gcStream* s)
{
    return s->id && s->voice >= 0;
}

#endif // STDSOUND_GAMECUBE
