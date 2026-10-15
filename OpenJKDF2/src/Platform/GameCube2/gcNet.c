// Network setup, libogc2 core.
//
// libogc2 finds the network adapter itself: if_config tries the Broadband
// Adapter, then the WIZnet W6100/W6300 and W5500 based ones (ETH2GC and the
// like), then the ENC28J60, and returns -1 when there is none. (The libogc core
// only knows the official BBA and checks its EXI ID first.) It runs on a
// low-priority thread with DHCP -- that can take seconds, and fails without a
// cable or a DHCP server -- so the game never waits for it. Multiplayer is
// offered only once this succeeded.

#include <stdio.h>
#include <string.h>
#include <gccore.h>
#include <network.h>
#include "gcNet.h"

static volatile int gcNet_state = GCNET_NONE;
static char gcNet_szIp[16] = "";
static char gcNet_szAdapter[16] = "";
static u32 gcNet_broadcast = 0; // subnet broadcast address (network order)
static lwp_t gcNet_thread = LWP_THREAD_NULL;
static u8 gcNet_stack[16 * 1024] __attribute__((aligned(32)));

static void* gcNet_ThreadMain(void* arg)
{
    char ip[16] = "", mask[16] = "", gw[16] = "";
    s32 res = if_config(ip, mask, gw, true);
    if (res == -1) {
        gcNet_state = GCNET_NONE;
        printf("gcNet: no network adapter\n");
        return NULL;
    }
    if_indextoname(1, gcNet_szAdapter);
    if (res >= 0 && ip[0] && strcmp(ip, "0.0.0.0")) {
        strncpy(gcNet_szIp, ip, sizeof(gcNet_szIp) - 1);
        gcNet_broadcast = inet_addr(ip) | ~inet_addr(mask);
        gcNet_state = GCNET_READY;
        printf("gcNet: network up on adapter '%s', ip %s mask %s gw %s\n", gcNet_szAdapter, ip, mask, gw);
    }
    else {
        gcNet_state = GCNET_FAILED;
        printf("gcNet: adapter '%s' found but no network (if_config %d: no cable or no DHCP)\n",
               gcNet_szAdapter, (int)res);
    }
    return NULL;
}

void gcNet_Start(void)
{
    printf("gcNet: looking for a network adapter\n");
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
