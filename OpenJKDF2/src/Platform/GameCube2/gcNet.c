// Broadband Adapter (BBA) detection and network setup.
//
// The BBA sits in serial port 1 (EXI channel 0, device 2). At boot we only check
// that it is there; if it is, a low-priority thread brings the network up with
// DHCP (that can take seconds, and fails without a cable or a DHCP server), so
// the game never waits for it. Multiplayer is offered only once this succeeded.

#include <stdio.h>
#include <string.h>
#include <gccore.h>
#include <network.h>
#include "gcNet.h"

#define GCNET_BBA_EXI_ID 0x04020200

static volatile int gcNet_state = GCNET_NONE;
static char gcNet_szIp[16] = "";
static u32 gcNet_broadcast = 0; // subnet broadcast address (network order)
static lwp_t gcNet_thread = LWP_THREAD_NULL;
static u8 gcNet_stack[16 * 1024] __attribute__((aligned(32)));

static void* gcNet_ThreadMain(void* arg)
{
    char ip[16] = "", mask[16] = "", gw[16] = "";
#ifdef GC_LIBOGC2
    s32 res = if_config(ip, mask, gw, true);
#else
    s32 res = if_config(ip, mask, gw, true, 20);
#endif
    if (res >= 0 && ip[0] && strcmp(ip, "0.0.0.0")) {
        strncpy(gcNet_szIp, ip, sizeof(gcNet_szIp) - 1);
        gcNet_broadcast = inet_addr(ip) | ~inet_addr(mask);
        gcNet_state = GCNET_READY;
        printf("gcNet: network up, ip %s mask %s gw %s\n", ip, mask, gw);
    }
    else {
        gcNet_state = GCNET_FAILED;
        printf("gcNet: no network (if_config %d)\n", res);
    }
    return NULL;
}

void gcNet_Start(void)
{
    u32 id = 0;
    if (!EXI_GetID(EXI_CHANNEL_0, EXI_DEVICE_2, &id) || id != GCNET_BBA_EXI_ID) {
        printf("gcNet: no Broadband Adapter (id 0x%08x)\n", id);
        gcNet_state = GCNET_NONE;
        return;
    }
    printf("gcNet: Broadband Adapter found, starting DHCP\n");
    gcNet_state = GCNET_PROBING;
    // Below the game and the disc prefetch: DHCP is all waiting anyway.
    if (LWP_CreateThread(&gcNet_thread, gcNet_ThreadMain, NULL, gcNet_stack, sizeof(gcNet_stack), 30) < 0)
        gcNet_state = GCNET_FAILED;
}

int gcNet_GetState(void)
{
    return gcNet_state;
}

int gcNet_IsReady(void)
{
    return gcNet_state == GCNET_READY;
}

const char* gcNet_GetIp(void)
{
    return gcNet_szIp;
}

unsigned int gcNet_GetBroadcast(void)
{
    return gcNet_broadcast;
}
