// Sound bank: DSP-ADPCM sounds held in ARAM (see gcSndBank.h).
//
// Why: sounds used to be read from the disc/SD the first time they played (and
// again whenever the 2 MiB sound cache had dropped them): on the SD that is
// 30-340 ms with the game stopped, the frame drops seen on hardware. A level's
// sounds are 4-14 MiB of 16-bit PCM -- too much to keep -- but ~1-4 MiB as
// DSP-ADPCM, the GameCube's own sound format (4 bits a sample; checked by ear
// against the originals). So at level load each sound's ADPCM goes to ARAM, and
// playing one only costs an ARAM copy and a decode on the CPU.
//
// In ARAM a sound is: its coefficients (2 x 16 s16, 64 bytes), then channel 0's
// frames, then channel 1's. The bank's index is kept in main RAM in compact form
// (name hash + numbers, ~24 bytes a sound); names are matched by hash.

#include "gcSndBank.h"
#include "gcAram.h"

#include "stdPlatform.h"
#include "General/stdString.h"
#include "jk.h"

#include "Platform/GameCube/gcOgc.h"
#include <ogc/lwp_watchdog.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#define GCSB_HEADER   32
#define GCSB_ENTRY    144
#define GCSB_COEFS    64
#define GCSB_STAGE    (32 * 1024)

typedef struct gcSndBankEntry
{
    uint32_t hash;
    uint32_t sampleRate;
    uint32_t numSamples;
    uint32_t dataOffset;
    uint32_t chBytes;
    uint16_t channels;
    uint16_t bValid;      // 0: hash shared with another name (not used)
    uint32_t fileIdx;     // position in the bank's on-disc index (for the coefficients)
} gcSndBankEntry;

static int              gcSndBank_bTried = 0;
static stdFile_t        gcSndBank_file = 0;
static gcSndBankEntry*  gcSndBank_aEntries = NULL; // sorted by hash
static int              gcSndBank_count = 0;
static uint8_t          gcSndBank_stage[GCSB_STAGE] __attribute__((aligned(32)));

static uint32_t rd32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static uint16_t rd16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

static uint32_t gcSndBank_Hash(const char* s)
{
    uint32_t h = 2166136261u; // FNV-1a
    for (; *s; s++) {
        h ^= (uint8_t)tolower((unsigned char)*s);
        h *= 16777619u;
    }
    return h;
}

static int gcSndBank_CmpHash(const void* a, const void* b)
{
    uint32_t x = ((const gcSndBankEntry*)a)->hash, y = ((const gcSndBankEntry*)b)->hash;
    return x < y ? -1 : x > y;
}

static void gcSndBank_Open(void)
{
    gcSndBank_bTried = 1;
    gcSndBank_file = std_g_pHS->fileOpen("resource\\sndbank.bin", "rb");
    if (!gcSndBank_file) {
        stdPlatform_Printf("gcSndBank: no sound bank, sounds play from the WAVs\n");
        return;
    }
    uint8_t hdr[GCSB_HEADER];
    if (std_g_pHS->fileRead(gcSndBank_file, hdr, GCSB_HEADER) != GCSB_HEADER || memcmp(hdr, "SBNK", 4) || rd32(hdr + 4) != 1) {
        stdPlatform_Printf("gcSndBank: bad sound bank\n");
        std_g_pHS->fileClose(gcSndBank_file);
        gcSndBank_file = 0;
        return;
    }
    int count = (int)rd32(hdr + 8);
    gcSndBank_aEntries = (gcSndBankEntry*)std_g_pHS->alloc(sizeof(gcSndBankEntry) * count);
    if (!gcSndBank_aEntries) return;

    // Read the index in pieces through the stage buffer, keeping the compact form.
    int done = 0;
    while (done < count) {
        int n = count - done;
        if (n > GCSB_STAGE / GCSB_ENTRY) n = GCSB_STAGE / GCSB_ENTRY;
        if (std_g_pHS->fileRead(gcSndBank_file, gcSndBank_stage, n * GCSB_ENTRY) != n * GCSB_ENTRY) break;
        for (int i = 0; i < n; i++) {
            const uint8_t* e = gcSndBank_stage + i * GCSB_ENTRY;
            gcSndBankEntry* d = &gcSndBank_aEntries[done + i];
            char name[49];
            memcpy(name, e, 48);
            name[48] = 0;
            d->hash = gcSndBank_Hash(name);
            d->sampleRate = rd32(e + 48);
            d->channels = rd16(e + 52);
            d->numSamples = rd32(e + 56);
            d->dataOffset = rd32(e + 60);
            d->chBytes = rd32(e + 64);
            d->bValid = (d->channels == 1 || d->channels == 2) && d->numSamples;
            d->fileIdx = done + i;
        }
        done += n;
    }
    gcSndBank_count = done;
    qsort(gcSndBank_aEntries, gcSndBank_count, sizeof(gcSndBankEntry), gcSndBank_CmpHash);
    for (int i = 1; i < gcSndBank_count; i++)
        if (gcSndBank_aEntries[i].hash == gcSndBank_aEntries[i - 1].hash)
            gcSndBank_aEntries[i].bValid = gcSndBank_aEntries[i - 1].bValid = 0;
    stdPlatform_Printf("gcSndBank: %d sounds in the bank\n", gcSndBank_count);
}

static int gcSndBank_Lookup(const char* key)
{
    uint32_t h = gcSndBank_Hash(key);
    int lo = 0, hi = gcSndBank_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t m = gcSndBank_aEntries[mid].hash;
        if (m == h) return gcSndBank_aEntries[mid].bValid ? mid : -1;
        if (m < h) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

int gcSndBank_Find(const char* fname, gcSndBankInfo* pInfo)
{
    if (!gcSndBank_bTried) gcSndBank_Open();
    if (!gcSndBank_count) return 0;
    static const char* aDirs[] = { "sound\\", "voice\\" };
    char key[64];
    for (int d = 0; d < 2; d++) {
        stdString_snprintf(key, sizeof(key), "%s%s", aDirs[d], fname);
        int idx = gcSndBank_Lookup(key);
        if (idx < 0) continue;
        const gcSndBankEntry* e = &gcSndBank_aEntries[idx];
        pInfo->idx = idx;
        pInfo->sampleRate = e->sampleRate;
        pInfo->channels = e->channels;
        pInfo->numSamples = e->numSamples;
        pInfo->pcmBytes = e->numSamples * e->channels * 2;
        return 1;
    }
    return 0;
}

// The entry's coefficients, from the bank's on-disc index.
static int gcSndBank_ReadCoefs(const gcSndBankEntry* e, uint8_t* pOut)
{
    std_g_pHS->fseek(gcSndBank_file, GCSB_HEADER + e->fileIdx * GCSB_ENTRY + 68, 0);
    return std_g_pHS->fileRead(gcSndBank_file, pOut, GCSB_COEFS) == GCSB_COEFS;
}

uint32_t gcSndBank_LoadToAram(const gcSndBankInfo* pInfo, uint32_t* pAramBytes)
{
    const gcSndBankEntry* e = &gcSndBank_aEntries[pInfo->idx];
    uint32_t dataBytes = e->chBytes * e->channels;
    uint32_t total = GCSB_COEFS + dataBytes;
    uint32_t aram = gcAram_Alloc(total);
    if (!aram) return 0;

    // Coefficients first (stage buffer is reused below).
    static uint8_t coefs[GCSB_COEFS] __attribute__((aligned(32)));
    if (!gcSndBank_ReadCoefs(e, coefs)) {
        gcAram_Free(aram, total);
        return 0;
    }
    gcAram_Write(aram, coefs, GCSB_COEFS);

    std_g_pHS->fseek(gcSndBank_file, e->dataOffset, 0);
    for (uint32_t done = 0; done < dataBytes; ) {
        uint32_t n = dataBytes - done < GCSB_STAGE ? dataBytes - done : GCSB_STAGE;
        if ((uint32_t)std_g_pHS->fileRead(gcSndBank_file, gcSndBank_stage, n) != n) {
            gcAram_Free(aram, total);
            return 0;
        }
        gcAram_Write(aram + GCSB_COEFS + done, gcSndBank_stage, n); // n: multiple of 32
        done += n;
    }
    *pAramBytes = total;
    return aram;
}

void gcSndBank_Decode(uint32_t aram, uint32_t channels, uint32_t numSamples, int16_t* pOut)
{
    static int16_t coefs[2][16] __attribute__((aligned(32)));
    gcAram_Read(coefs, aram, GCSB_COEFS);
    uint32_t frames = (numSamples + 13) / 14;
    uint32_t chBytes = (frames * 8 + 31) & ~31u;

    for (uint32_t c = 0; c < channels; c++) {
        int hist1 = 0, hist2 = 0;
        uint32_t frame = 0;
        for (uint32_t off = 0; off < chBytes; off += GCSB_STAGE) {
            uint32_t n = chBytes - off < GCSB_STAGE ? chBytes - off : GCSB_STAGE;
            gcAram_Read(gcSndBank_stage, aram + GCSB_COEFS + c * chBytes + off, n);
            const uint8_t* fr = gcSndBank_stage;
            for (uint32_t f = 0; f < n / 8 && frame < frames; f++, frame++, fr += 8) {
                int pred = fr[0] >> 4, scale = 1 << (fr[0] & 0xF);
                int c1 = coefs[c][pred * 2], c2 = coefs[c][pred * 2 + 1];
                uint32_t s0 = frame * 14;
                for (int s = 0; s < 14 && s0 + s < numSamples; s++) {
                    int nib = (s & 1) ? (fr[1 + s / 2] & 0xF) : (fr[1 + s / 2] >> 4);
                    if (nib >= 8) nib -= 16;
                    int v = (((nib * scale) << 11) + 1024 + c1 * hist1 + c2 * hist2) >> 11;
                    if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
                    pOut[(s0 + s) * channels + c] = (int16_t)v;
                    hist2 = hist1;
                    hist1 = v;
                }
            }
        }
    }
}
