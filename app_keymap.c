/*
 * app_keymap.c — Keyboard scan-code / virtual-key / character mapping
 *
 * Demonstrates the keyboard translation APIs:
 *   - WM_KEYDOWN receives a virtual-key code in wParam and a scan code
 *     packed into lParam (HIWORD bits 16-23, plus extended-key bit 24)
 *   - MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) — vkey → scan code
 *   - MapVirtualKeyW(vsc, MAPVK_VSC_TO_VK) — scan → vkey
 *   - MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) — printable char without modifiers
 *   - ToUnicodeEx(vk, scan, keyboardState, buf, cb, 0, layout) translates
 *     with modifiers (Shift, AltGr, dead keys); requires GetKeyboardState
 *   - GetKeyboardLayoutList enumerates loaded HKLs
 *   - GetKeyNameTextW gives a localized key name ("Caps Lock", "Tab")
 *
 * The frame edit consumes WM_KEYDOWN before the normal text path so every
 * keypress is reported regardless of whether it'd actually produce text.
 */

#include "shell.h"
#include <stdlib.h>
#include <stdio.h>

#define KM_PROP    L"MS_KM_STATE"
#define ID_KM_OUT  77001
#define ID_KM_LAY  77002

typedef struct {
    HWND output, layouts;
} KmState;

static WNDPROC g_origKmFrame = NULL;

static void Km_Append(KmState *st, const wchar_t *t)
{
    int len = GetWindowTextLengthW(st->output);
    SendMessageW(st->output, EM_SETSEL, len, len);
    SendMessageW(st->output, EM_REPLACESEL, FALSE, (LPARAM)t);
    SendMessageW(st->output, EM_SCROLLCARET, 0, 0);
}

static void Km_FillLayouts(KmState *st)
{
    UINT n = GetKeyboardLayoutList(0, NULL);
    HKL *list;
    UINT i;
    wchar_t cur[16];
    HKL active = GetKeyboardLayout(0);

    SendMessageW(st->layouts, CB_RESETCONTENT, 0, 0);
    if (n == 0) return;
    list = (HKL *)malloc(n * sizeof(HKL));
    GetKeyboardLayoutList(n, list);
    for (i = 0; i < n; ++i) {
        wchar_t label[80];
        DWORD lid = (DWORD)(DWORD_PTR)list[i];
        swprintf_s(label, 80, L"HKL 0x%08lx (lang 0x%04x)",
                   lid, LOWORD(lid));
        SendMessageW(st->layouts, CB_ADDSTRING, 0, (LPARAM)label);
        if (list[i] == active)
            SendMessageW(st->layouts, CB_SETCURSEL, i, 0);
    }
    (void)cur;
    free(list);
}

static void Km_Report(KmState *st, WPARAM vk, LPARAM lp)
{
    UINT scan      = (UINT)((lp >> 16) & 0xFF);
    BOOL extended  = (lp & (1 << 24)) ? TRUE : FALSE;
    UINT scanFull  = scan | (extended ? 0xE000 : 0);
    BYTE kbState[256];
    wchar_t keyName[64] = L"";
    wchar_t char1[16] = L"";
    int     toUni;
    UINT    mapVsc;
    UINT    mapChar;
    HKL     layout = GetKeyboardLayout(0);
    wchar_t line[400];

    GetKeyNameTextW((LONG)((scan << 16) | (extended ? (1 << 24) : 0)),
                     keyName, 64);
    GetKeyboardState(kbState);
    toUni = ToUnicodeEx((UINT)vk, scan, kbState, char1, 15, 0, layout);
    if (toUni <= 0) wcscpy_s(char1, 16, L"(none)");
    else            char1[toUni] = 0;

    mapVsc  = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    mapChar = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_CHAR);

    swprintf_s(line, 400,
        L"vk=0x%02X (%-12s)  scan=0x%02X ext=%d  full=0x%04X\r\n"
        L"   MapVirtualKey: vsc=0x%02X chr=U+%04X    ToUnicode: \"%s\"\r\n\r\n",
        (UINT)vk, keyName[0] ? keyName : L"?",
        scan, extended ? 1 : 0, scanFull,
        mapVsc, mapChar, char1);
    Km_Append(st, line);
    (void)layout;
}

static LRESULT CALLBACK Km_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    KmState *st = (KmState *)GetPropW(hwnd, KM_PROP);
    if (msg == WM_KEYDOWN && st) {
        Km_Report(st, wp, lp);
        return 0;
    }
    if (msg == WM_SETFOCUS && st) {
        Km_Append(st, L"[focused — press any key]\r\n");
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->layouts, 12, 38, w - 24, 24, TRUE);
        MoveWindow(st->output,  8, 74, w - 16, h - 82, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, KM_PROP); }
    return CallWindowProcW(g_origKmFrame, hwnd, msg, wp, lp);
}

static HWND KeyMap_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    KmState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"KeyMap",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (KmState *)calloc(1, sizeof(KmState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->layouts = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        12, 38, w - 24, 200, frame, (HMENU)(LONG_PTR)ID_KM_LAY, hInstance, NULL);
    Km_FillLayouts(st);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"Press any key here to see scan code / vk / char mapping...\r\n\r\n",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 74, w - 16, h - 82, frame, (HMENU)(LONG_PTR)ID_KM_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, KM_PROP, (HANDLE)st);
    if (!g_origKmFrame) g_origKmFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Km_FrameProc);
    SetFocus(frame);
    return frame;
}

MsApp g_AppKeyMap = { L"KeyMap", KeyMap_Create, 620, 440 };
