#ifndef _STDENDIAN_H
#define _STDENDIAN_H

// Added: byte order helpers for the on-disk formats.
//
// Every original asset format (GOB, BM, MAT, SFT, CMP, WAV, saves, ...) is
// little-endian, and the decompiled loaders read them straight into structs. On a
// big-endian host (GameCube/PowerPC) those fields have to be swapped after the read.
// On little-endian hosts all of this compiles away.

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define STD_BIG_ENDIAN 1
#endif

#ifdef STD_BIG_ENDIAN

static inline uint16_t stdEndian_LE16(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t stdEndian_LE32(uint32_t v) { return __builtin_bswap32(v); }

static inline float stdEndian_LEF32(float v)
{
    uint32_t u;
    memcpy(&u, &v, 4);
    u = __builtin_bswap32(u);
    memcpy(&v, &u, 4);
    return v;
}

// Swap `count` consecutive 16/32-bit little-endian values in place (structs made
// only of 4-byte fields can be swapped with the 32-bit variant in one go).
static inline void stdEndian_SwapLE16Array(void* p, size_t count)
{
    uint16_t* w = (uint16_t*)p;
    for (size_t i = 0; i < count; i++)
        w[i] = __builtin_bswap16(w[i]);
}

static inline void stdEndian_SwapLE32Array(void* p, size_t count)
{
    uint8_t* b = (uint8_t*)p;
    for (size_t i = 0; i < count; i++) {
        uint32_t v;
        memcpy(&v, b + i * 4, 4);
        v = __builtin_bswap32(v);
        memcpy(b + i * 4, &v, 4);
    }
}

#else // little-endian host: no-ops

static inline uint16_t stdEndian_LE16(uint16_t v) { return v; }
static inline uint32_t stdEndian_LE32(uint32_t v) { return v; }
static inline float stdEndian_LEF32(float v) { return v; }
static inline void stdEndian_SwapLE16Array(void* p, size_t count) { (void)p; (void)count; }
static inline void stdEndian_SwapLE32Array(void* p, size_t count) { (void)p; (void)count; }

#endif // STD_BIG_ENDIAN

// In-place helpers for single struct fields.
#define STD_LE16(x) ((x) = (__typeof__(x))stdEndian_LE16((uint16_t)(x)))
#define STD_LE32(x) ((x) = (__typeof__(x))stdEndian_LE32((uint32_t)(x)))
#define STD_LEF32(x) ((x) = stdEndian_LEF32(x))

#endif // _STDENDIAN_H
