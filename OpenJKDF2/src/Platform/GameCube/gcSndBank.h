#ifndef _GCSNDBANK_H
#define _GCSNDBANK_H

#include <stdint.h>

// The sound bank (resource/sndbank.bin, DSP-ADPCM, built by
// packaging/gamecube/make_sndbank.py): a level's sounds are copied into ARAM when
// it loads, and decoded into main RAM when they play -- no disc/SD reads in play.

typedef struct gcSndBankInfo
{
    int      idx;         // entry in the bank
    uint32_t sampleRate;
    uint32_t channels;    // 1 or 2
    uint32_t numSamples;  // per channel
    uint32_t pcmBytes;    // decoded size (16-bit, interleaved)
} gcSndBankInfo;

// Looks a sound up by its file name ("expl_01.wav": tried under sound\ and voice\).
int gcSndBank_Find(const char* fname, gcSndBankInfo* pInfo);

// Copies the sound's ADPCM (and its coefficients) into the ARAM pool. Returns the
// ARAM address (0: no room -- play it from the WAV as before) and the block size.
uint32_t gcSndBank_LoadToAram(const gcSndBankInfo* pInfo, uint32_t* pAramBytes);

// Decodes a sound loaded by gcSndBank_LoadToAram into 16-bit native-endian PCM
// (interleaved for stereo); pOut holds channels * numSamples samples.
void gcSndBank_Decode(uint32_t aram, uint32_t channels, uint32_t numSamples, int16_t* pOut);


#endif // _GCSNDBANK_H
