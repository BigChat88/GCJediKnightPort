// Crash screen.
//
// libogc2's own screen walks the stack through the back chain without checking
// it: when the crash came from a damaged stack (the cases worth diagnosing) the
// walk faults too, and the second screen hides the first. This one replaces it
// (--wrap=c_default_exceptionhandler): registers, a stack walk that only follows
// pointers into RAM, and the words at the top of the crashed frame.

#include <stdint.h>
#include <gccore.h>
#include <ogc/context.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>
#include "gcOgc.h"

extern void __dsp_shutdown(void);

static const char* const gcCrash_aNames[] = {
    "System Reset", "Machine Check", "DSI", "ISI", "Interrupt", "Alignment", "Program",
    "FP Unavailable", "Decrementer", "System Call", "Trace", "Performance", "IABR",
    "Reserved", "Thermal"
};

static volatile int gcCrash_depth = 0;

static int gcCrash_IsRam(uint32_t p)
{
    return p >= 0x80003000 && p < 0x81800000 && !(p & 3);
}

void __wrap_c_default_exceptionhandler(frame_context* c)
{
    if (gcCrash_depth++) {
        // A fault while showing the first one: keep that screen.
        while (1);
    }
    __dsp_shutdown();
    GX_AbortFrame();
    u16 xstart, ystart, xres, yres, stride;
    VIDEO_GetFrameBufferPan(&xstart, &ystart, &xres, &yres, &stride);
    if (CON_Init(VIDEO_GetCurrentFramebuffer(), xstart, ystart, xres, yres, stride * VI_DISPLAY_PIX_SZ))
        while (1);

    const char* name = c->nExcept < sizeof(gcCrash_aNames) / sizeof(gcCrash_aNames[0])
                     ? gcCrash_aNames[c->nExcept] : "?";
    kprintf("\n\n\tException (%s) occurred! (OpenJKDF2 crash screen)", name);
    for (int i = 0; i < 8; i++)
        kprintf("\n\tGPR%02d %08X GPR%02d %08X GPR%02d %08X GPR%02d %08X", i, (unsigned)c->gpr[i],
                i + 8, (unsigned)c->gpr[i + 8], i + 16, (unsigned)c->gpr[i + 16], i + 24, (unsigned)c->gpr[i + 24]);
    kprintf("\n\tLR %08X SRR0 %08X SRR1 %08X", (unsigned)c->lr, (unsigned)c->srr0, (unsigned)c->srr1);
    kprintf("\n\tDAR %08X DSISR %08X", (unsigned)mfspr(19), (unsigned)mfspr(18));
    // libogc2 guards the bottom of each thread's stack with a data address
    // breakpoint (DABR): a hit is a stack overflow.
    if (c->nExcept == EX_DSI && (mfspr(18) & 0x00400000))
        kprintf("\n\n\tSTACK OVERFLOW (DABR): write to %08X by the instruction at %08X (LR %08X)",
                (unsigned)mfspr(19), (unsigned)c->srr0, (unsigned)c->lr);

    uint32_t sp = c->gpr[1];
    kprintf("\n\n\tSP %08X", (unsigned)sp);

    // Back chain, only through RAM: frame[0] = caller's frame, frame[1] = saved LR.
    kprintf("\n\n\tSTACK: %08X %08X", (unsigned)c->srr0, (unsigned)c->lr);
    uint32_t p = sp;
    for (int i = 0; i < 12 && gcCrash_IsRam(p); i++) {
        uint32_t up = *(uint32_t*)p;
        if (!gcCrash_IsRam(up) || up <= p) {
            kprintf(" | chain ends: %08X", (unsigned)up);
            break;
        }
        kprintf("%s%08X", (i % 6 == 5) ? "\n\t" : " ", (unsigned)*(uint32_t*)(up + 4));
        p = up;
    }

    // The words at the top of the stack frame (damage shows up here).
    if (gcCrash_IsRam(sp)) {
        kprintf("\n\n\tAT SP:");
        for (int i = 0; i < 8; i++)
            kprintf(" %08X", (unsigned)((uint32_t*)sp)[i]);
    }
    if (c->nExcept == EX_DSI && gcCrash_IsRam(c->srr0)) {
        uint32_t* code = (uint32_t*)c->srr0;
        kprintf("\n\tCODE: %08X %08X %08X %08X", (unsigned)code[0], (unsigned)code[1],
                (unsigned)code[2], (unsigned)code[3]);
    }
    kprintf("\n\n\tPress RESET to restart.\n");
    while (1) {
        if (SYS_ResetButtonDown())
            SYS_ResetSystem(SYS_HOTRESET, 0, 0);
        udelay(20000);
    }
}
