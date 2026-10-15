#ifndef _GCSTREAM_H
#define _GCSTREAM_H

#include <stdint.h>

// A PCM stream played by a libansnd voice (libogc2 core): the music and the
// cutscene soundtrack. libansnd plays from ARAM only, so the stream keeps three
// chunk buffers there: when the voice asks for the next one (DSP interrupt), it
// gets a buffer filled earlier, and the one after it is filled -- `fill` copies
// the data into a staging buffer in main RAM, which is sent to ARAM by DMA while
// the current one plays.

// Fills exactly `len` bytes of 16-bit big-endian PCM (silence past the end of the
// data). Runs in the DSP interrupt: copy only.
typedef void (*gcStreamFill)(void* ctx, uint8_t* dst, uint32_t len);

typedef struct gcStream
{
    int32_t          voice;      // libansnd voice id, -1 when stopped
    int              id;         // 1.. (its DMA requests), set on the first start
    uint32_t         aAram[3];   // chunk buffers in ARAM (allocated once)
    uint8_t*         aStage[3];  // their staging copies in main RAM
    volatile int     next;       // buffer handed over at the next request
    uint32_t         chunkBytes;
    uint32_t         frameBytes; // bytes per sample frame (2 mono, 4 stereo)
    gcStreamFill     fill;
    void*            ctx;
    volatile uint32_t chunksFed; // chunks handed to the voice so far
    volatile uint64_t lastFeedTicks;
} gcStream;

// Starts the stream (allocating its buffers the first time). Returns 0 on
// failure (no ARAM or main RAM for the buffers, no free voice).
int  gcStream_Start(gcStream* s, uint32_t rate, int bStereo, float vol, uint32_t chunkBytes,
                    gcStreamFill fill, void* ctx);
void gcStream_Stop(gcStream* s);
void gcStream_SetVolume(gcStream* s, float vol);
int  gcStream_IsPlaying(const gcStream* s);

#endif // _GCSTREAM_H
