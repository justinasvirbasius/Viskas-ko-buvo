/*
 * app_sendin.c — Synthesize input events with SendInput
 *
 * Demonstrates the SendInput API for posting virtual input from a process:
 *   - SendInput with an array of INPUT structures
 *   - Filling KEYBDINPUT with virtual key codes and KEYEVENTF_KEYUP for the
 *     paired up event
 *   - Pre-focusing our own edit control via SetFocus so the synthetic
 *     keystrokes land in our window (we don't redirect them at other apps)
 *
 * The "Type" button focuses the edit box and synthesizes a short greeting
 * letter by letter so you can see SendInput working. The "Hotkey" button
 * synthesizes Ctrl+A to select all currently-typed text in the box.
 */

#include "shell.h"
#include <stdlib.h>

#define SI_PROP    L"MS_SI_STATE"
#define ID_SI_TYPE 39001
#define ID_SI_ALL  39002
#define ID_SI_EDIT 39003
#define ID_SI_CLR  39004

typedef struct {
    HWND edit;
} SiState;

static WNDPROC g_origSiFrame = NULL;

static void Si_PressKey(WORD vk, BOOL shift)
{
    INPUT in[4];
    int n = 0;
    ZeroMemory(in, sizeof(in));

    if (shift) {
        in[n].type = INPUT_KEYBOARD;
        in[n].ki.wVk = VK_SHIFT;
        ++n;
    }
    in[n].type = INPUT_KEYBOARD;
    in[n].ki.wVk = vk;
    ++n;
    in[n].type = INPUT_KEYBOARD;
    in[n].ki.wVk = vk;
    in[n].ki.dwFlags = KEYEVENTF_KEYUP;
    ++n;
    if (shift) {
        in[n].type = INPUT_KEYBOARD;
        in[n].ki.wVk = VK_SHIFT;
        in[n].ki.dwFlags = KEYEVENTF_KEYUP;
        ++n;
    }
    SendInput(n, in, sizeof(INPUT));
}

static void Si_TypeString(SiState *st, const wchar_t *s)
{
    SetFocus(st->edit);
    while (*s) {
        wchar_t c = *s;
        BOOL shift = FALSE;
        WORD vk = 0;
        if (c == L' ') { vk = VK_SPACE; }
        else if (c >= L'a' && c <= L'z') { vk = (WORD)(L'A' + (c - L'a')); }
        else if (c >= L'A' && c <= L'Z') { vk = (WORD)c; shift = TRUE; }
        else if (c >= L'0' && c <= L'9') { vk = (WORD)c; }
        else if (c == L'!') { vk = '1'; shift = TRUE; }
        else if (c == L',') { vk = VK_OEM_COMMA; }
        else if (c == L'.') { vk = VK_OEM_PERIOD; }
        else { ++s; continue; }
        Si_PressKey(vk, shift);
        Sleep(40);   /* tiny pause so the user perceives typing */
        ++s;
    }
}

static void Si_SelectAll(SiState *st)
{
    INPUT in[4];
    SetFocus(st->edit);
    ZeroMemory(in, sizeof(in));
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_CONTROL;
    in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = 'A';
    in[2].type = INPUT_KEYBOARD; in[2].ki.wVk = 'A';        in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].type = INPUT_KEYBOARD; in[3].ki.wVk = VK_CONTROL; in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

static LRESULT CALLBACK Si_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    SiState *st = (SiState *)GetPropW(hwnd, SI_PROP);

    if (msg == WM_COMMAND && st) {
        switch (LOWORD(wp)) {
        case ID_SI_TYPE:
            Si_TypeString(st, L"Hello from SendInput!");
            return 0;
        case ID_SI_ALL:
            Si_SelectAll(st);
            return 0;
        case ID_SI_CLR:
            SetWindowTextW(st->edit, L"");
            SetFocus(st->edit);
            return 0;
        }
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->edit, 8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, SI_PROP); }
    return CallWindowProcW(g_origSiFrame, hwnd, msg, wp, lp);
}

static HWND SendIn_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    SiState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"SendIn",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (SiState *)calloc(1, sizeof(SiState));
    if (!st) { DestroyWindow(frame); return NULL; }

    CreateWindowExW(0, L"STATIC",
        L"Click Type to have SendInput synthesize keystrokes into the edit box below.\n"
        L"Hotkey demonstrates Ctrl+A. Input is scoped to this window.",
        WS_CHILD | WS_VISIBLE,
        12, 36, w - 24, 36, frame, NULL, hInstance, NULL);

    CreateWindowExW(0, L"BUTTON", L"Type",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, h - 36, 80, 26, frame, (HMENU)(LONG_PTR)ID_SI_TYPE, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Hotkey",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        100, h - 36, 80, 26, frame, (HMENU)(LONG_PTR)ID_SI_ALL, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Clear",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        188, h - 36, 80, 26, frame, (HMENU)(LONG_PTR)ID_SI_CLR, hInstance, NULL);

    st->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL,
        8, 76, w - 16, h - 120, frame, (HMENU)(LONG_PTR)ID_SI_EDIT, hInstance, NULL);
    mono = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    SendMessageW(st->edit, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, SI_PROP, (HANDLE)st);
    if (!g_origSiFrame) g_origSiFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Si_FrameProc);
    /* SetSize calls MoveWindow on the edit; re-do once more so initial geometry is right */
    SendMessageW(frame, WM_SIZE, 0, MAKELPARAM(w, h));
    return frame;
}

MsApp g_AppSendIn = {
    L"SendIn",
    SendIn_Create,
    480, 320
};
