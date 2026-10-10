// Minimal GameCube apploader: loads the disc's main DOL.
//
// The IPL (or Dolphin's HLE BS2) copies this code to 0x81200000 and calls the entry
// point, which hands back three callbacks. It then calls main() repeatedly; each
// call returns a disc read request (destination, length, disc offset) that the IPL
// performs before calling again, until main() returns 0. close() returns the DOL
// entry point, which the IPL jumps to.
//
// Freestanding: no libc/libogc, linked at 0x81200000 (see apploader.ld).

typedef unsigned int u32;
typedef unsigned char u8;

typedef void (*report_t)(const char* fmt, ...);

typedef struct {
    u32 textOffset[7];
    u32 dataOffset[11];
    u32 textAddress[7];
    u32 dataAddress[11];
    u32 textSize[7];
    u32 dataSize[11];
    u32 bssAddress;
    u32 bssSize;
    u32 entry;
    u32 pad[7];
} dolHeader;

// Scratch buffers live past the apploader image, at addresses the DOL never uses
// before it starts (and 32-byte aligned for the DVD DMA).
#define DISC_HDR_BUF ((volatile u8*)0x81300000)
#define DOL_HDR_BUF  ((dolHeader*)0x81300100)

static int state;
static int section;
static u32 dolOffset;
static report_t report;

static void flushRange(u32 addr, u32 size)
{
    u32 end = addr + size;
    for (addr &= ~31u; addr < end; addr += 32) {
        __asm__ volatile("dcbf 0,%0; icbi 0,%0" :: "r"(addr) : "memory");
    }
    __asm__ volatile("sync; isync" ::: "memory");
}

static void invalidateRange(u32 addr, u32 size)
{
    u32 end = addr + size;
    for (addr &= ~31u; addr < end; addr += 32) {
        __asm__ volatile("dcbi 0,%0" :: "r"(addr) : "memory");
    }
    __asm__ volatile("sync" ::: "memory");
}

static void apploaderInit(report_t r)
{
    report = r;
    state = 0;
    section = 0;
    dolOffset = 0;
}

static int apploaderMain(void** dst, int* size, int* offset)
{
    dolHeader* dol = DOL_HDR_BUF;

    if (state == 0) {
        // The disc header field with the main DOL offset lives at 0x420.
        invalidateRange((u32)DISC_HDR_BUF, 0x20);
        *dst = (void*)DISC_HDR_BUF;
        *size = 0x20;
        *offset = 0x420;
        state = 1;
        return 1;
    }
    if (state == 1) {
        invalidateRange((u32)DISC_HDR_BUF, 0x20);
        dolOffset = *(volatile u32*)DISC_HDR_BUF;
        invalidateRange((u32)dol, 0x100);
        *dst = (void*)dol;
        *size = 0x100;
        *offset = (int)dolOffset;
        state = 2;
        return 1;
    }

    // Sections: 7 text then 11 data, skipping empty ones.
    invalidateRange((u32)dol, 0x100);
    while (section < 18) {
        int i = section++;
        u32 addr, len, off;
        if (i < 7) { addr = dol->textAddress[i]; len = dol->textSize[i]; off = dol->textOffset[i]; }
        else       { addr = dol->dataAddress[i - 7]; len = dol->dataSize[i - 7]; off = dol->dataOffset[i - 7]; }
        if (!len) continue;
        invalidateRange(addr, len);
        *dst = (void*)addr;
        *size = (int)((len + 31) & ~31u);
        *offset = (int)(dolOffset + off);
        return 1;
    }
    return 0;
}

static void* apploaderClose(void)
{
    dolHeader* dol = DOL_HDR_BUF;

    // Zero .bss, then make the freshly DMA'd code visible to the instruction cache.
    volatile u8* bss = (volatile u8*)dol->bssAddress;
    for (u32 i = 0; i < dol->bssSize; i++) bss[i] = 0;
    flushRange(dol->bssAddress, dol->bssSize);

    for (int i = 0; i < 7; i++)
        if (dol->textSize[i]) flushRange(dol->textAddress[i], dol->textSize[i]);
    for (int i = 0; i < 11; i++)
        if (dol->dataSize[i]) flushRange(dol->dataAddress[i], dol->dataSize[i]);

    return (void*)dol->entry;
}

__attribute__((section(".text.entry")))
void apploaderEntry(void (**init)(report_t), int (**main)(void**, int*, int*), void* (**close)(void))
{
    *init = apploaderInit;
    *main = apploaderMain;
    *close = apploaderClose;
}
