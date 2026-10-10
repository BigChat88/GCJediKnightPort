#ifndef _PLATFORM_NETWORKING_GAMECUBE_STDCOMM_GC_H
#define _PLATFORM_NETWORKING_GAMECUBE_STDCOMM_GC_H

#ifdef PLATFORM_GCSOCKETS

void stdComm_Gc_Startup();
void stdComm_Gc_Shutdown();
void stdComm_Gc_Pump(void); // empties the game socket while the game is busy (level loads)

#endif // PLATFORM_GCSOCKETS

#endif // _PLATFORM_NETWORKING_GAMECUBE_STDCOMM_GC_H
