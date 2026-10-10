// On-screen keyboard for text fields (see jkGUIGcKeyboard.h).
//
// The game asks for text in a few places (multiplayer character name, game name,
// password) that a controller can't fill. This is a regular jkGui menu: a grid of
// character buttons moved through with the D-pad / stick, A types the focused key,
// B deletes the last character, X (or OK) accepts.

#include "jkGUIGcKeyboard.h"

#include "Gui/jkGUIRend.h"
#include "Gui/jkGUI.h"
#include "General/stdString.h"
#include "jk.h"

#include <string.h>

#define KB_COLS     10
#define KB_ROWS     4
#define KB_KEYS     (KB_COLS * KB_ROWS)
#define KB_MAXTEXT  64

enum {
    KB_ID_SHIFT = 1001,
    KB_ID_SPACE,
    KB_ID_DELETE,
    KB_ID_OK,       // closes the menu (returned by DisplayAndReturnClicked)
    KB_ID_CANCEL,
};

static const char kb_aUpper[KB_KEYS + 1] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.-_?0123456789";
static const char kb_aLower[KB_KEYS + 1] = "abcdefghijklmnopqrstuvwxyz.-_?0123456789";

static char16_t  kb_aKeyText[KB_KEYS][2];
static char16_t  kb_text[KB_MAXTEXT + 2];   // the text being edited, plus a cursor mark
static char16_t  kb_display[KB_MAXTEXT + 2];
static int       kb_maxLen = 0;              // characters allowed (field size - 1)
static int       kb_bLower = 0;
static int       kb_bInitted = 0;

static int jkGuiGcKeyboard_Key(jkGuiElement* pElement, jkGuiMenu* pMenu, int32_t mouseX, int32_t mouseY, int bRedraw);

// 0: title, 1: text, 2..41: keys, 42..46: shift/space/delete/ok/cancel, 47: end
static jkGuiElement kb_aElements[KB_KEYS + 8];
static jkGuiMenu kb_menu = { kb_aElements, -1, 0xFFFF, 0xFFFF, 0x0F, 0, 0, jkGui_stdBitmaps, jkGui_stdFonts, 0, 0, "thermloop01.wav", "thrmlpu2.wav", 0, 0, 0, 0, 0, 0 };

static void jkGuiGcKeyboard_SetElement(jkGuiElement* e, int type, int id, int textType, const char16_t* str, int x, int y, int w, int h)
{
    memset(e, 0, sizeof(*e));
    e->type = type;
    e->hoverId = id;
    e->textType = textType;
    e->orig_wstr = str;   // jkGui_InitMenu copies it to wstr (not a string table key: drawn as is)
    e->wstr = str;
    e->selectedTextEntry = 3; // centred
    e->rect.x = x; e->rect.y = y; e->rect.width = w; e->rect.height = h;
    e->bIsVisible = 1;
    if (type == ELEMENT_TEXTBUTTON) e->clickHandlerFunc = jkGuiGcKeyboard_Key;
}

static void jkGuiGcKeyboard_Build(void)
{
    jkGuiElement* e = kb_aElements;
    jkGuiGcKeyboard_SetElement(e++, ELEMENT_TEXT, 0, 6, u"Enter text", 20, 20, 600, 40);
    jkGuiGcKeyboard_SetElement(e++, ELEMENT_TEXT, 0, 2, kb_display, 20, 95, 600, 30);
    for (int i = 0; i < KB_KEYS; i++) {
        int col = i % KB_COLS, row = i / KB_COLS;
        jkGuiGcKeyboard_SetElement(e++, ELEMENT_TEXTBUTTON, 2000 + i, 2, kb_aKeyText[i],
                                   70 + col * 51, 150 + row * 48, 46, 40);
    }
    jkGuiGcKeyboard_SetElement(e++, ELEMENT_TEXTBUTTON, KB_ID_SHIFT,  2, u"a / A",  40,  355, 110, 36);
    jkGuiGcKeyboard_SetElement(e++, ELEMENT_TEXTBUTTON, KB_ID_SPACE,  2, u"Space",  165, 355, 110, 36);
    jkGuiGcKeyboard_SetElement(e++, ELEMENT_TEXTBUTTON, KB_ID_DELETE, 2, u"Delete", 290, 355, 110, 36);
    jkGuiGcKeyboard_SetElement(e++, ELEMENT_TEXTBUTTON, KB_ID_CANCEL, 2, u"Cancel", 40,  420, 200, 40);
    jkGuiGcKeyboard_SetElement(e++, ELEMENT_TEXTBUTTON, KB_ID_OK,     2, u"OK",     440, 420, 160, 40);
    (e - 1)->clickHandlerFunc = NULL;     // OK: plain button, ends the menu
    (e - 2)->clickHandlerFunc = NULL;     // Cancel too
    memset(e, 0, sizeof(*e));
    e->type = ELEMENT_END;
}

static void jkGuiGcKeyboard_UpdateKeys(void)
{
    for (int i = 0; i < KB_KEYS; i++) {
        kb_aKeyText[i][0] = (char16_t)(kb_bLower ? kb_aLower[i] : kb_aUpper[i]);
        kb_aKeyText[i][1] = 0;
    }
}

static void jkGuiGcKeyboard_UpdateDisplay(void)
{
    int n = (int)_wcslen(kb_text);
    memcpy(kb_display, kb_text, n * sizeof(char16_t));
    kb_display[n] = n < kb_maxLen ? u'_' : 0; // cursor while there's room
    kb_display[n + 1] = 0;
}

static int jkGuiGcKeyboard_Key(jkGuiElement* pElement, jkGuiMenu* pMenu, int32_t mouseX, int32_t mouseY, int bRedraw)
{
    int n = (int)_wcslen(kb_text);
    jkGuiRend_PlayWav(pMenu->soundClick);
    switch (pElement->hoverId) {
        case KB_ID_SHIFT:
            kb_bLower = !kb_bLower;
            jkGuiGcKeyboard_UpdateKeys();
            jkGuiRend_Paint(pMenu);
            return 0;
        case KB_ID_SPACE:
            if (n < kb_maxLen) { kb_text[n] = u' '; kb_text[n + 1] = 0; }
            break;
        case KB_ID_DELETE:
            if (n > 0) kb_text[n - 1] = 0;
            break;
        default:
            if (pElement->hoverId >= 2000 && pElement->hoverId < 2000 + KB_KEYS && n < kb_maxLen) {
                kb_text[n] = pElement->wstr[0];
                kb_text[n + 1] = 0;
            }
            break;
    }
    jkGuiGcKeyboard_UpdateDisplay();
    jkGuiRend_UpdateAndDrawClickable(&kb_aElements[1], pMenu, 1);
    return 0;
}

void jkGuiGcKeyboard_EditTextBox(jkGuiElement* pTextBox, jkGuiMenu* pMenu)
{
    if (!pTextBox || !pTextBox->wstr) return;
    if (!kb_bInitted) {
        jkGuiGcKeyboard_Build();
        kb_bInitted = 1;
    }
    // Over the same background (and palette) as the menu it was opened from.
    jkGui_InitMenu(&kb_menu, (pMenu && pMenu->pBgBitmap) ? pMenu->pBgBitmap : jkGui_stdBitmaps[JKGUI_BM_BK_ESC]);
    kb_maxLen = pTextBox->selectedTextEntry - 1;
    if (kb_maxLen > KB_MAXTEXT) kb_maxLen = KB_MAXTEXT;
    if (kb_maxLen < 1) return;
    stdString_SafeWStrCopy(kb_text, pTextBox->wstr, kb_maxLen + 1);
    kb_bLower = 0;
    jkGuiGcKeyboard_UpdateKeys();
    jkGuiGcKeyboard_UpdateDisplay();

    // B deletes, X accepts (the menu's escape / return shortcuts).
    jkGuiRend_MenuSetEscapeKeyShortcutElement(&kb_menu, &kb_aElements[2 + KB_KEYS + 2]); // Delete
    jkGuiRend_MenuSetReturnKeyShortcutElement(&kb_menu, &kb_aElements[2 + KB_KEYS + 4]); // OK
    jkGuiRend_GcSetInitialFocus(&kb_menu, &kb_aElements[2]);
    int ret = jkGuiRend_DisplayAndReturnClicked(&kb_menu);

    if (ret == KB_ID_OK) {
        stdString_SafeWStrCopy((char16_t*)pTextBox->wstr, kb_text, pTextBox->selectedTextEntry);
        pTextBox->texInfo.textHeight = (int)_wcslen(pTextBox->wstr); // cursor at the end
    }
    if (pMenu) jkGuiRend_UpdateAndDrawClickable(pTextBox, pMenu, 1);
}
