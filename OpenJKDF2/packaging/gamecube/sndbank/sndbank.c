/* sndbank: builds resource/sndbank.bin, the GameCube sound bank.
 *
 * Every WAV under sound\ and voice\ in the given GOB/GOO files is encoded to
 * Nintendo DSP-ADPCM (the GameCube DSP's own format, ~29% of 16-bit PCM). At
 * level load the engine copies a level's sounds from this file into ARAM, and
 * decodes one into main RAM when it plays -- so no sound is ever read from the
 * disc/SD during play (src/Platform/GameCube/gcSndBank.c).
 *
 *   sndbank out.bin order.txt game1.gob [game2.gob ...]
 *
 * GOBs are searched in the order given (the first copy of a name wins, like the
 * engine's search order). order.txt lists sound names (e.g. "sound\expl_01.wav")
 * in the order levels first use them, so a level's sounds sit together in the
 * file; the rest follow alphabetically. Unreadable or non-PCM WAVs are left out
 * (the engine then plays them from the GOB as before).
 *
 * File layout, all big-endian:
 *   header  "SBNK" u32 version, u32 count, u32 dataStart, 16 bytes reserved
 *   index   count entries of GCSB_ENTRY bytes, sorted by name (binary search):
 *             char name[48]   lowercase, '\' separators
 *             u32 sampleRate
 *             u16 channels    1 or 2
 *             u16 reserved
 *             u32 numSamples  per channel
 *             u32 dataOffset  from the start of the file, 32-byte aligned
 *             u32 chBytes     ADPCM bytes per channel, a multiple of 32
 *             s16 coefs[2][16]
 *             u8  pad[12]
 *   data    per sound: channel 0's frames, then channel 1's (each padded to 32)
 *           8-byte frames: ps byte (predictor<<4 | scale), 7 bytes = 14 nibbles
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <math.h>

void DSPCorrelateCoefs(const short* source, int samples, short* coefsOut);
void DSPEncodeFrame(short pcmInOut[16], int sampleCount, unsigned char adpcmOut[8], const short coefsIn[8][2]);

#define GCSB_VERSION 1
#define GCSB_HEADER  32
#define GCSB_ENTRY   144
#define GCSB_NAME    48

typedef struct {
    char name[GCSB_NAME];
    const char* gob;
    long off, size;
} Wav;

static Wav* g_wavs;
static int g_numWavs, g_capWavs;

static uint32_t rd32le(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16le(const uint8_t* p) { return p[0] | (p[1] << 8); }
static void put32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void put16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t)v; }

static int find_wav(const char* name)
{
    for (int i = 0; i < g_numWavs; i++)
        if (!strcmp(g_wavs[i].name, name)) return i;
    return -1;
}

static void scan_gob(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "sndbank: can't open %s\n", path); exit(1); }
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "GOB ", 4)) { fprintf(stderr, "sndbank: %s is not a GOB\n", path); exit(1); }
    fseek(f, rd32le(h + 8), SEEK_SET);
    uint8_t c[4];
    fread(c, 1, 4, f);
    uint32_t n = rd32le(c);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t e[136];
        if (fread(e, 1, 136, f) != 136) break;
        char name[129];
        memcpy(name, e + 8, 128);
        name[128] = 0;
        for (char* p = name; *p; p++) *p = (char)tolower((unsigned char)*p);
        size_t len = strlen(name);
        if (len < 4 || strcmp(name + len - 4, ".wav")) continue;
        if (strncmp(name, "sound\\", 6) && strncmp(name, "voice\\", 6)) continue;
        if (len >= GCSB_NAME || find_wav(name) >= 0) continue;
        if (g_numWavs == g_capWavs) {
            g_capWavs = g_capWavs ? g_capWavs * 2 : 1024;
            g_wavs = realloc(g_wavs, g_capWavs * sizeof(Wav));
        }
        Wav* w = &g_wavs[g_numWavs++];
        strcpy(w->name, name);
        w->gob = path;
        w->off = rd32le(e);
        w->size = rd32le(e + 4);
    }
    fclose(f);
}

static int cmp_name(const void* a, const void* b)
{
    return strcmp(((const Wav*)a)->name, ((const Wav*)b)->name);
}

/* Reads a WAV's PCM as 16-bit samples per channel. Returns 0 if unusable. */
static int load_pcm(const Wav* w, int* pRate, int* pCh, int* pN, short** pChans)
{
    FILE* f = fopen(w->gob, "rb");
    if (!f) return 0;
    uint8_t* buf = malloc(w->size);
    fseek(f, w->off, SEEK_SET);
    long got = (long)fread(buf, 1, w->size, f);
    fclose(f);
    if (got != w->size || got < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) { free(buf); return 0; }

    int fmt = 0, ch = 0, rate = 0, bits = 0;
    const uint8_t* data = NULL;
    uint32_t dlen = 0;
    for (long p = 12; p + 8 <= got; ) {
        uint32_t cl = rd32le(buf + p + 4);
        if (!memcmp(buf + p, "fmt ", 4) && p + 24 <= got) {
            fmt = rd16le(buf + p + 8); ch = rd16le(buf + p + 10);
            rate = rd32le(buf + p + 12); bits = rd16le(buf + p + 22);
        }
        else if (!memcmp(buf + p, "data", 4)) {
            data = buf + p + 8;
            dlen = cl;
            if (p + 8 + (long)dlen > got) dlen = got - p - 8; /* truncated files: what's there */
            break;
        }
        p += 8 + cl + (cl & 1);
    }
    if (fmt != 1 || (ch != 1 && ch != 2) || (bits != 8 && bits != 16) || !data || !rate) { free(buf); return 0; }

    int bps = bits / 8;
    int n = dlen / (bps * ch);
    if (n <= 0) { free(buf); return 0; }
    for (int c = 0; c < ch; c++) {
        pChans[c] = calloc(n + 16, sizeof(short));
        for (int i = 0; i < n; i++) {
            const uint8_t* s = data + ((long)i * ch + c) * bps;
            pChans[c][i] = bits == 8 ? (short)((s[0] - 128) << 8) : (short)rd16le(s);
        }
    }
    free(buf);
    *pRate = rate; *pCh = ch; *pN = n;
    return 1;
}

/* Hardware decode of one channel (as the DSP / the engine's gcSndBank_Decode). */
static void decode_channel(const uint8_t* adpcm, int n, const short coefs[16], short* out)
{
    int h1 = 0, h2 = 0;
    for (int fi = 0; fi < (n + 13) / 14; fi++) {
        const uint8_t* fr = adpcm + fi * 8;
        int pred = fr[0] >> 4, scale = 1 << (fr[0] & 0xF);
        for (int s = 0; s < 14 && fi * 14 + s < n; s++) {
            int nib = (s & 1) ? (fr[1 + s / 2] & 0xF) : (fr[1 + s / 2] >> 4);
            if (nib >= 8) nib -= 16;
            int v = (((nib * scale) << 11) + 1024 + coefs[pred * 2] * h1 + coefs[pred * 2 + 1] * h2) >> 11;
            if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
            out[fi * 14 + s] = (short)v;
            h2 = h1; h1 = v;
        }
    }
}

/* Encodes one channel; returns the ADPCM bytes (padded to 32) and fills coefs. */
static uint8_t* encode_channel(const short* pcm, int n, short coefs[16], uint32_t* pBytes)
{
    DSPCorrelateCoefs(pcm, n, coefs);
    int frames = (n + 13) / 14;
    uint32_t bytes = ((uint32_t)frames * 8 + 31) & ~31u;
    uint8_t* out = calloc(bytes, 1);
    short conv[16] = {0};
    for (int fi = 0; fi < frames; fi++) {
        int cnt = n - fi * 14 < 14 ? n - fi * 14 : 14;
        memcpy(conv + 2, pcm + fi * 14, cnt * sizeof(short));
        for (int k = cnt; k < 14; k++) conv[2 + k] = 0;
        DSPEncodeFrame(conv, 14, out + fi * 8, (const short (*)[2])coefs);
        conv[0] = conv[14];
        conv[1] = conv[15];
    }
    *pBytes = bytes;
    return out;
}

int main(int argc, char** argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: sndbank out.bin order.txt game.gob [more.gob ...]\n");
        return 1;
    }
    for (int i = 3; i < argc; i++) scan_gob(argv[i]);
    if (!g_numWavs) { fprintf(stderr, "sndbank: no sounds found\n"); return 1; }

    /* Data order: order.txt first, then the rest by name. */
    int* order = malloc(g_numWavs * sizeof(int));
    char* used = calloc(g_numWavs, 1);
    int numOrder = 0;
    FILE* fo = fopen(argv[2], "r");
    if (fo) {
        char line[256];
        while (fgets(line, sizeof(line), fo)) {
            char* e = line + strlen(line);
            while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
            for (char* p = line; *p; p++) *p = (char)(*p == '/' ? '\\' : tolower((unsigned char)*p));
            int idx = find_wav(line);
            if (idx >= 0 && !used[idx]) { used[idx] = 1; order[numOrder++] = idx; }
        }
        fclose(fo);
    }
    {
        /* the rest, alphabetically */
        Wav* sorted = malloc(g_numWavs * sizeof(Wav));
        memcpy(sorted, g_wavs, g_numWavs * sizeof(Wav));
        qsort(sorted, g_numWavs, sizeof(Wav), cmp_name);
        for (int i = 0; i < g_numWavs; i++) {
            int idx = find_wav(sorted[i].name);
            if (!used[idx]) { used[idx] = 1; order[numOrder++] = idx; }
        }
        free(sorted);
    }

    /* Encode in data order; index entries are written sorted by name later. */
    uint8_t* entries = calloc(g_numWavs, GCSB_ENTRY);
    int count = 0;
    FILE* out = fopen(argv[1], "wb");
    if (!out) { fprintf(stderr, "sndbank: can't write %s\n", argv[1]); return 1; }
    uint32_t dataStart = (GCSB_HEADER + (uint32_t)g_numWavs * GCSB_ENTRY + 31) & ~31u;
    fseek(out, dataStart, SEEK_SET);
    uint32_t pos = dataStart;
    uint64_t pcmTotal = 0;
    int skipped = 0;

    for (int oi = 0; oi < numOrder; oi++) {
        const Wav* w = &g_wavs[order[oi]];
        int rate, ch, n;
        short* chans[2] = {0};
        if (!load_pcm(w, &rate, &ch, &n, chans)) {
            fprintf(stderr, "sndbank: skipped %s (not 8/16-bit PCM)\n", w->name);
            skipped++;
            continue;
        }
        uint8_t* e = entries + (size_t)count * GCSB_ENTRY;
        memset(e, 0, GCSB_ENTRY);
        strcpy((char*)e, w->name);
        put32(e + 48, rate);
        put16(e + 52, (uint16_t)ch);
        put32(e + 56, n);
        put32(e + 60, pos);
        uint32_t chBytes = 0;
        for (int c = 0; c < ch; c++) {
            short coefs[16];
            uint8_t* adpcm = encode_channel(chans[c], n, coefs, &chBytes);
            if (getenv("SNDBANK_REPORT")) {
                /* Quality report: SNR of the decoded sound against the original. */
                short* dec = malloc(n * sizeof(short));
                decode_channel(adpcm, n, coefs, dec);
                double sig = 0, err = 0; int peak = 0, clip = 0;
                for (int i = 0; i < n; i++) {
                    double a = chans[c][i], d = a - dec[i];
                    sig += a * a; err += d * d;
                    if (abs(chans[c][i]) > peak) peak = abs(chans[c][i]);
                    if (abs(chans[c][i]) >= 32700) clip++;
                }
                printf("REPORT %s ch%d rate %d n %d snr %.1f peak %d clipped %d\n", w->name, c, rate, n,
                       err > 0 ? 10 * log10(sig / err) : 99.0, peak, clip);
                free(dec);
            }
            fwrite(adpcm, 1, chBytes, out);
            pos += chBytes;
            for (int k = 0; k < 16; k++) put16(e + 68 + c * 32 + k * 2, (uint16_t)coefs[k]);
            free(adpcm);
            free(chans[c]);
        }
        put32(e + 64, chBytes);
        pcmTotal += (uint64_t)n * ch * 2;
        count++;
    }

    /* Index sorted by name, so the engine can binary-search it. */
    qsort(entries, count, GCSB_ENTRY, (int (*)(const void*, const void*))strcmp);
    uint8_t hdr[GCSB_HEADER] = {0};
    memcpy(hdr, "SBNK", 4);
    put32(hdr + 4, GCSB_VERSION);
    put32(hdr + 8, count);
    put32(hdr + 12, dataStart);
    fseek(out, 0, SEEK_SET);
    fwrite(hdr, 1, GCSB_HEADER, out);
    fwrite(entries, GCSB_ENTRY, count, out);
    fclose(out);

    printf("sndbank: %d sounds (%d skipped), %u KB of 16-bit PCM -> %u KB\n",
           count, skipped, (unsigned)(pcmTotal / 1024), (unsigned)((pos - dataStart) / 1024));
    return 0;
}
