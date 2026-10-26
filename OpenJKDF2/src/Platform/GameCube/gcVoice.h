#ifndef _PLATFORM_GAMECUBE_GCVOICE_H
#define _PLATFORM_GAMECUBE_GCVOICE_H

#include "gcAesndlib.h"

#ifdef __cplusplus
extern "C" {
#endif

// Safe release of AESND voices (see gcVoice.c). Call gcVoice_Init once after
// AESND_Init.
void gcVoice_Init(void);

// Stop a voice and give it back to AESND. The actual free happens a moment later
// from the audio interrupt; until then the voice's callback may still run with its
// old argument: owners pass a generation token as the argument, and their callback
// stops a voice whose token is stale. *pDone (optional) is set to 1 once the voice
// is gone and anything it played from may be freed.
void gcVoice_Free(AESNDPB* pb, volatile int* pDone);

#ifdef __cplusplus
}
#endif

#endif // _PLATFORM_GAMECUBE_GCVOICE_H
