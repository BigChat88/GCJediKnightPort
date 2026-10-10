#ifndef _GCNET_H
#define _GCNET_H

// Broadband Adapter (BBA) networking for multiplayer.

#define GCNET_NONE    0 // no BBA in serial port 1
#define GCNET_PROBING 1 // BBA found, asking the network for an address (DHCP)
#define GCNET_READY   2 // connected: multiplayer available
#define GCNET_FAILED  3 // BBA found but no network (no cable, no DHCP server)

void gcNet_Start(void);          // at boot: detect the BBA, configure it in the background
int  gcNet_GetState(void);
int  gcNet_IsReady(void);
const char* gcNet_GetIp(void);   // "a.b.c.d" once ready
unsigned int gcNet_GetBroadcast(void); // subnet broadcast address, network order (0 = unknown)

#endif // _GCNET_H
