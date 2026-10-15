// Safe release of AESND voices.
//
// libogc's AESND (3.1) mixes one voice at a time: the DSP interrupt copies the
// voice into a single "current" parameter block, hands it to the DSP, and when the
// DSP is done calls the voice's callback from that copy and writes the copy back
// over the voice. Anything done to a voice while it is in flight is undone by that
// write-back: AESND_FreeVoice + AESND_RegisterVoiceCallbackWithArg(NULL) on a
// playing voice can bring it back to life with its old callback and argument, which
// then runs on freed memory (a DSI on real hardware; Dolphin's DSP timing hides it).
//
// No voice is in flight while the audio DMA interrupt calls the audio callback (it
// runs before the next mixing pass starts), so voices are freed from there.

#include "gcVoice.h"

#include <gccore.h>

#define GCVOICE_MAX_PENDING 64

typedef struct gcVoicePending {
    AESNDPB*      pb;
    volatile int* pDone;
} gcVoicePending;

static gcVoicePending gcVoice_aPending[GCVOICE_MAX_PENDING];
static volatile int   gcVoice_numPending = 0;

static void gcVoice_AudioCallback(void* pAudioBuffer, u32 len, void* arg)
{
    // Interrupt context, no voice in flight: these stick.
    for (int i = 0; i < gcVoice_numPending; i++) {
        AESNDPB* pb = gcVoice_aPending[i].pb;
        AESND_SetVoiceStop(pb, true);
        AESND_RegisterVoiceCallbackWithArg(pb, NULL, NULL);
        AESND_FreeVoice(pb);
        if (gcVoice_aPending[i].pDone)
            *gcVoice_aPending[i].pDone = 1;
    }
    gcVoice_numPending = 0;
}

void gcVoice_Init(void)
{
    AESND_RegisterAudioCallbackWithArg(gcVoice_AudioCallback, NULL);
}

void gcVoice_Free(AESNDPB* pb, volatile int* pDone)
{
    if (!pb) {
        if (pDone) *pDone = 1;
        return;
    }
    // Stop it now (usually sticks; if the voice is in flight its callback still
    // runs once more, and gets dropped there by the owner's token check).
    AESND_SetVoiceStop(pb, true);

    u32 level;
    for (;;) {
        level = IRQ_Disable();
        if (gcVoice_numPending < GCVOICE_MAX_PENDING) {
            gcVoice_aPending[gcVoice_numPending].pb = pb;
            gcVoice_aPending[gcVoice_numPending].pDone = pDone;
            gcVoice_numPending++;
            IRQ_Restore(level);
            return;
        }
        IRQ_Restore(level);
        VIDEO_WaitVSync(); // queue full: the audio interrupt drains it every few ms
    }
}
