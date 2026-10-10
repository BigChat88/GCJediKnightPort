#ifndef __DLMALLOC_CONFIG_H__
#define __DLMALLOC_CONFIG_H__

#define INSECURE		0
#ifdef TARGET_GAMECUBE
// GameCube (gcHeap.c): report heap damage and bad frees instead of carrying on.
#define FOOTERS			1
#define PROCEED_ON_ERROR	0
void gcHeap_Corrupt(const char* what, void* p);
#define CORRUPTION_ERROR_ACTION(m)  gcHeap_Corrupt("heap corrupted", (void*)(m))
#define USAGE_ERROR_ACTION(m, p)    gcHeap_Corrupt("bad free", (void*)(p))
#endif
#define ONLY_MSPACES    	1
#define HAVE_MORECORE		0
#define HAVE_MMAP		0
#define MSPACES			1
#define LACKS_SYS_MMAN_H 	1
#define LACKS_SCHED_H		1
#ifndef FOOTERS
#define FOOTERS			0
#endif
#define NO_MALLOC_STATS		1
#ifndef PROCEED_ON_ERROR
#define PROCEED_ON_ERROR	1
#endif
#define malloc_getpagesize	128
#define DEFAULT_GRANULARITY (32)

#endif
