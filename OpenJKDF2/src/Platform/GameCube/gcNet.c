// Network setup (libogc2).
//
// libogc2 finds the network adapter itself: if_config tries the Broadband
// Adapter, then the WIZnet W6100/W6300 and W5500 based ones (ETH2GC and the
// like), then the ENC28J60, and returns -1 when there is none. (devkitPro's
// libogc only knows the official BBA.) Which of them may be set up: see
// gcNet_AdapterPresent. It runs on a
// low-priority thread with DHCP -- that can take seconds, and fails without a
// cable or a DHCP server -- so the game never waits for it. Multiplayer is
// offered only once this succeeded.

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <gccore.h>
#include <network.h>
#include "gcNet.h"
#include "gcDvd.h"

static volatile int gcNet_state = GCNET_NONE;
static char gcNet_szIp[16] = "";
static char gcNet_szAdapter[16] = "";
static u32 gcNet_broadcast = 0; // subnet broadcast address (network order)
static lwp_t gcNet_thread = LWP_THREAD_NULL;
// (32 KiB: DHCP, lwIP and printf.)
static u8 gcNet_stack[32 * 1024] __attribute__((aligned(32)));

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
        // lwIP keeps asking for an address in the background (if_config gives up
        // after a few DHCP tries, and can't be called again): a cable plugged in
        // later, or a slow DHCP server, still gets the game online. The main menu
        // shows Multiplayer as soon as the state turns ready.
        while (1) {
            usleep(1000 * 1000);
            u32 addr = net_gethostip();
            if (!addr) continue;
            struct in_addr in = { .s_addr = addr };
            strncpy(gcNet_szIp, inet_ntoa(in), sizeof(gcNet_szIp) - 1);
            gcNet_broadcast = 0; // (netmask unknown here: limited broadcast only)
            gcNet_state = GCNET_READY;
            printf("gcNet: network up late on adapter '%s', ip %s\n", gcNet_szAdapter, gcNet_szIp);
            break;
        }
    }
    return NULL;
}

// The adapter, found by EXI ID without touching serial port 2. if_config itself
// probes serial port 2 (EXI channel 2) for WIZnet and ENC28J60 adapters -- ID and
// reset commands sent to whatever sits there -- once the Broadband Adapter isn't
// found (the W6x00 driver tries serial port 1, then serial port 2, then the slots,
// before the W5500 one does the same). Run on its thread while the game reads an
// SD2SP2, those commands land on the card mid-read. So: the Broadband Adapter
// always; a WIZnet adapter only when serial port 2 isn't serving the game.
// ENC28J60 adapters don't answer the ID command and aren't looked for.
static int gcNet_AdapterPresent(void)
{
    static const struct { s32 chan, dev; const char* name; } aWhere[] = {
        { EXI_CHANNEL_0, EXI_DEVICE_2, "serial port 1" },
        { EXI_CHANNEL_1, EXI_DEVICE_0, "slot B" },
        { EXI_CHANNEL_0, EXI_DEVICE_0, "slot A" },
    };
    for (size_t i = 0; i < sizeof(aWhere) / sizeof(aWhere[0]); i++) {
        u32 id = 0;
        if (!EXI_GetID(aWhere[i].chan, aWhere[i].dev, &id)) continue;
        if (id == 0x04020200) { // Broadband Adapter
            printf("gcNet: Broadband Adapter in %s\n", aWhere[i].name);
            return 1;
        }
        if (id == 0x03000000 || id == 0xFF800000) { // WIZnet W5500/W6100, W6300
            if (gcDvd_UsesSerialPort2()) {
                printf("gcNet: WIZnet adapter in %s not used: setting it up probes serial port 2,\n"
                       "  the SD card the game is read from\n", aWhere[i].name);
                return 0;
            }
            printf("gcNet: WIZnet adapter in %s (EXI id 0x%08x)\n", aWhere[i].name, (unsigned)id);
            return 1;
        }
    }
    return 0;
}

void gcNet_Start(void)
{
    printf("gcNet: looking for a network adapter\n");
    if (!gcNet_AdapterPresent()) {
        gcNet_state = GCNET_NONE;
        printf("gcNet: no network adapter in serial port 1 or the memory card slots\n");
        return;
    }
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
