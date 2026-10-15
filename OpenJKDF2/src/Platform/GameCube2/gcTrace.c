// Diagnostic build helper (GC_TRACE=1 at configure time): the engine is compiled
// with -finstrument-functions, these hooks keep a shadow call stack of the main
// thread, and a high-priority watchdog thread prints it every few seconds. Feed the
// addresses to powerpc-eabi-addr2line -f -e openjkdf2.elf to see where the game is
// spending its time or hanging.

#ifdef GC_TRACE

#include "gcOgc.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define GCTRACE_MAX_DEPTH 256
#define GCTRACE_PERIOD_US (10 * 1000 * 1000)

#define NO_INSTRUMENT __attribute__((no_instrument_function))

static void* volatile gcTrace_aStack[GCTRACE_MAX_DEPTH];
static volatile int   gcTrace_depth = 0;
static volatile lwp_t gcTrace_mainThread = LWP_THREAD_NULL;
static lwp_t          gcTrace_watchdog;
static uint8_t        gcTrace_watchdogStack[16 * 1024] __attribute__((aligned(32)));

void __cyg_profile_func_enter(void* fn, void* callSite) NO_INSTRUMENT;
void __cyg_profile_func_exit(void* fn, void* callSite) NO_INSTRUMENT;

void __cyg_profile_func_enter(void* fn, void* callSite)
{
    if (LWP_GetSelf() != gcTrace_mainThread) return;
    int d = gcTrace_depth;
    if (d < GCTRACE_MAX_DEPTH) gcTrace_aStack[d] = fn;
    gcTrace_depth = d + 1;
}

void __cyg_profile_func_exit(void* fn, void* callSite)
{
    if (LWP_GetSelf() != gcTrace_mainThread) return;
    if (gcTrace_depth > 0) gcTrace_depth--;
}

// Sampling profile: every GCTRACE_SAMPLE_US the innermost instrumented function is
// counted; the hottest ones are printed with each stack dump ("GCPROF addr count",
// symbolize with addr2line). Counts reset after every report.
#define GCTRACE_SAMPLE_US  (10 * 1000)
#define GCTRACE_PROF_SLOTS (1024)
#define GCTRACE_PROF_TOP   (20)

static uint32_t gcTrace_aProfAddr[GCTRACE_PROF_SLOTS];
static uint32_t gcTrace_aProfCount[GCTRACE_PROF_SLOTS];

static void gcTrace_Sample(void) NO_INSTRUMENT;
static void gcTrace_Sample(void)
{
    int depth = gcTrace_depth;
    if (depth <= 0) return;
    if (depth > GCTRACE_MAX_DEPTH) depth = GCTRACE_MAX_DEPTH;
    uint32_t addr = (uint32_t)(uintptr_t)gcTrace_aStack[depth - 1];
    uint32_t h = (addr >> 2) & (GCTRACE_PROF_SLOTS - 1);
    for (int i = 0; i < GCTRACE_PROF_SLOTS; i++, h = (h + 1) & (GCTRACE_PROF_SLOTS - 1)) {
        if (gcTrace_aProfAddr[h] == addr || !gcTrace_aProfAddr[h]) {
            gcTrace_aProfAddr[h] = addr;
            gcTrace_aProfCount[h]++;
            return;
        }
    }
}

static void gcTrace_ReportProfile(void) NO_INSTRUMENT;
static void gcTrace_ReportProfile(void)
{
    char line[1024];
    int n = snprintf(line, sizeof(line), "GCPROF");
    for (int k = 0; k < GCTRACE_PROF_TOP; k++) {
        int best = -1;
        for (int i = 0; i < GCTRACE_PROF_SLOTS; i++)
            if (gcTrace_aProfCount[i] && (best < 0 || gcTrace_aProfCount[i] > gcTrace_aProfCount[best])) best = i;
        if (best < 0) break;
        n += snprintf(line + n, sizeof(line) - n, " %08x:%u", gcTrace_aProfAddr[best], gcTrace_aProfCount[best]);
        gcTrace_aProfCount[best] = 0;
    }
    memset(gcTrace_aProfAddr, 0, sizeof(gcTrace_aProfAddr));
    memset(gcTrace_aProfCount, 0, sizeof(gcTrace_aProfCount));
    printf("%s\n", line);
}

static void* gcTrace_WatchdogMain(void* arg) NO_INSTRUMENT;
static void* gcTrace_WatchdogMain(void* arg)
{
    uint32_t elapsed = 0;
    while (1)
    {
        usleep(GCTRACE_SAMPLE_US);
        gcTrace_Sample();
        elapsed += GCTRACE_SAMPLE_US;
        if (elapsed < GCTRACE_PERIOD_US) continue;
        elapsed = 0;

        int depth = gcTrace_depth;
        if (depth > GCTRACE_MAX_DEPTH) depth = GCTRACE_MAX_DEPTH;
        char line[2048];
        int n = snprintf(line, sizeof(line), "GCTRACE depth %d:", depth);
        for (int i = 0; i < depth && n < (int)sizeof(line) - 12; i++)
            n += snprintf(line + n, sizeof(line) - n, " %08x", (unsigned)(uintptr_t)gcTrace_aStack[i]);
        printf("%s\n", line);
        gcTrace_ReportProfile();
    }
    return NULL;
}

void gcTrace_Start(void) NO_INSTRUMENT;
void gcTrace_Start(void)
{
    gcTrace_mainThread = LWP_GetSelf();
    LWP_CreateThread(&gcTrace_watchdog, gcTrace_WatchdogMain, NULL,
                     gcTrace_watchdogStack, sizeof(gcTrace_watchdogStack), 120);
}

#else

void gcTrace_Start(void) {}

#endif // GC_TRACE
