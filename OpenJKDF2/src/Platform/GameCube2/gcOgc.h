#ifndef _PLATFORM_GAMECUBE_GCOGC_H
#define _PLATFORM_GAMECUBE_GCOGC_H

// libogc's gcbool.h typedefs BOOL as unsigned int, which clashes with the engine's
// Win32-style BOOL. Include libogc through here so its BOOL gets a private name.
// (Declared here too: if gctypes.h was already included elsewhere, its BOOL
// typedef is done and the headers below would name an undeclared gcBOOL.)
typedef unsigned int gcBOOL;
#define BOOL gcBOOL
#include <gccore.h>
#undef BOOL

#endif // _PLATFORM_GAMECUBE_GCOGC_H
