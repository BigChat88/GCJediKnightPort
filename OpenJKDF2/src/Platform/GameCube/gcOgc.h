#ifndef _PLATFORM_GAMECUBE_GCOGC_H
#define _PLATFORM_GAMECUBE_GCOGC_H

// libogc's gcbool.h typedefs BOOL as unsigned int, which clashes with the engine's
// Win32-style BOOL. Include libogc through here so its BOOL gets a private name.
#define BOOL gcBOOL
#include <gccore.h>
#undef BOOL

#endif // _PLATFORM_GAMECUBE_GCOGC_H
