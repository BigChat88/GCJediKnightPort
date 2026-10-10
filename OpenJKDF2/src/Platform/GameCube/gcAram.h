#ifndef _GCARAM_H
#define _GCARAM_H

#include <stdint.h>

// Texture pool in ARAM: material pixels live here instead of main RAM (MEM1),
// and the GX texture cache copies them into MEM1 when they are drawn. Addresses
// are ARAM addresses (0 = none). Sizes are rounded up to 32 bytes.

void     gcAram_InitPool(uint32_t base, uint32_t size);
uint32_t gcAram_Alloc(uint32_t size);
void     gcAram_Free(uint32_t addr, uint32_t size);

// Synchronous transfers. mem must be 32-byte aligned, aram and len multiples of 32.
void     gcAram_Write(uint32_t aram, void* mem, uint32_t len);
void     gcAram_Read(void* mem, uint32_t aram, uint32_t len);

extern uint32_t gcAram_poolSize;
extern uint32_t gcAram_poolUsed;
extern uint32_t gcAram_poolFails; // allocations that fell back to main RAM

#endif // _GCARAM_H
