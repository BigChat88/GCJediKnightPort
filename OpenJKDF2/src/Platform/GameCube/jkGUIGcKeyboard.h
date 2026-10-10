#ifndef _JKGUIGCKEYBOARD_H
#define _JKGUIGCKEYBOARD_H

#include "types.h"

// On-screen keyboard for text fields (GameCube: no keyboard). Opened with A on a
// focused ELEMENT_TEXTBOX; edits a copy of its text and writes it back on OK.
void jkGuiGcKeyboard_EditTextBox(jkGuiElement* pTextBox, jkGuiMenu* pMenu);

#endif // _JKGUIGCKEYBOARD_H
