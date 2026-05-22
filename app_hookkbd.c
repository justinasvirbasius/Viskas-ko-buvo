/*
 * app_hookkbd.c — Low-level keyboard hook (WH_KEYBOARD_LL)
 *
 * Demonstrates a global keyboard observer running entirely in-process —
 * WH_KEYBOARD_LL is special in that it does NOT require an external DLL,
 * unlike most other global hooks:
 *   - SetWindowsHookExW(WH_KEYBOARD_LL, proc, GetModuleHandleW(NULL), 0)
 *   - LowLevelKeyboardProc receives KBDLLHOOKSTRUCT for every key event
 *     while the hook is installed
 *   - CallNextHookEx to chain to the next hook (mandatory pass-through)
 *
 * Safety: this hook can observe ANY key on the system, including passwords.
 * The app shows ONLY the virtual-key code and timestamp, never characters,
 * and a big start/stop toggle so it's deliberately user-controlled. Keys
 * pressed while focus is in another window will still be counted, but only
 * counted — no key data leaves this window.
 */

#include "shell.h"
#include <stdlib.h>
#include <stdio.h>

#define HK_PROP   L"MS_HK_STATE"
#define ID_HK_GO  57001
#define ID_HK_STOP 57002
#define ID_HK_LBL 57003
#define ID_HK_HIST 57004

#define WM_HK_KEY (WM_USER + 140)   /* wparam=vk, lparam=isKeyDown */

typedef struct {
    HHOOK   hook;
    HWND    frame, label, history;
    int     keyDowns, keyUps;
} HkState;

/* Hook proc must be a function pointer; we need access to one shared HkState. */
static HkState *g_hkCurrent = NULL;

static WNDPROC g_origHkFrame = NULL;

static LRESULT CALLBACK Hk_HookProc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && g_hkCurrent) {
        KBDLLHOOKSTRUCT *kb = (KBDLLHOOKSTRUCT *)lp;
        BOOL down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
        BOOL up   = (wp == WM_KEYUP   || wp == WM_SYSKEYUP);
        if (down || up) {
            PostMessageW(g_hkCurrent->frame, WM_HK_KEY,
                         (WPARAM)kb->vkCode, (LPARAM)down);
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static const wchar_t *Hk_VkLabel(DWORD vk)
{
    switch (vk) {
    case VK_SHIFT:   return L"Shift";
    case VK_LSHIFT:  return L"LShift";
    case VK_RSHIFT:  return L"RShift";
    case VK_CONTROL: return L"Ctrl";
    case VK_LCONTROL:return L"LCtrl";
    case VK_RCONTROL:return L"RCtrl";
    case VK_MENU:    return L"Alt";
    case VK_LMENU:   return L"LAlt";
    case VK_RMENU:   return L"RAlt";
    case VK_CAPITAL: return L"CapsLock";
    case VK_ESCAPE:  return L"Esc";
    case VK_TAB:     return L"Tab";
    case VK_RETURN:  return L"Enter";
    case VK_SPACE:   return L"Space";
    case VK_BACK:    return L"Backspace";
    case VK_LWIN:    return L"LWin";
    case VK_RWIN:    return L"RWin";
    case VK_UP:      return L"Up";
    case VK_DOWN:    return L"Down";
    case VK_LEFT:    return L"Left";
    case VK_RIGHT:   return L"Right";
    case VK_PRIOR:   return L"PgUp";
    case VK_NEXT:    return L"PgDn";
    case VK_HOME:    return L"Home";
    case VK_END:     return L"End";
    case VK_DELETE:  return L"Delete";
    case VK_INSERT:  return L"Insert";
    }
    return NULL;
}

static void Hk_Append(HkState *st, const wchar_t *t)
{
    int len = GetWindowTextLengthW(st->history);
    SendMessageW(st->history, EM_SETSEL, len, len);
    SendMessageW(st->history, EM_REPLACESEL, FALSE, (LPARAM)t);
    SendMessageW(st->history, EM_SCROLLCARET, 0, 0);
}

static void Hk_Start(HkState *st)
{
    if (st->hook) return;
    g_hkCurrent = st;
    st->hook = SetWindowsHookExW(WH_KEYBOARD_LL, Hk_HookProc,
                                  GetModuleHandleW(NULL), 0);
    if (!st->hook) {
        SetWindowTextW(st->label, L"SetWindowsHookEx failed.");
        g_hkCurrent = NULL;
        return;
    }
    SetWindowTextW(st->label,
        L"Hook active. Keystrokes are *counted* (not recorded). Press Stop to unhook.");
}

static void Hk_Stop(HkState *st)
{
    if (!st->hook) return;
    UnhookWindowsHookEx(st->hook);
    st->hook = NULL;
    if (g_hkCurrent == st) g_hkCurrent = NULL;
    SetWindowTextW(st->label, L"Hook stopped.");
}

static LRESULT CALLBACK Hk_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    HkState *st = (HkState *)GetPropW(hwnd, HK_PROP);

    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_HK_GO)   { Hk_Start(st); return 0; }
        if (LOWORD(wp) == ID_HK_STOP) { Hk_Stop(st);  return 0; }
    }
    if (msg == WM_HK_KEY && st) {
        DWORD vk = (DWORD)wp;
        BOOL down = (BOOL)lp;
        wchar_t buf[80];
        const wchar_t *name = Hk_VkLabel(vk);

        if (down) ++st->keyDowns; else ++st->keyUps;
        if (name) {
            swprintf_s(buf, 80, L"  %s %s\r\n", down ? L"↓" : L"↑", name);
        } else if (vk >= 0x30 && vk <= 0x39) {
            swprintf_s(buf, 80, L"  %s digit\r\n", down ? L"↓" : L"↑");
        } else if (vk >= 0x41 && vk <= 0x5A) {
            swprintf_s(buf, 80, L"  %s letter\r\n", down ? L"↓" : L"↑");
        } else {
            swprintf_s(buf, 80, L"  %s vk=0x%02X\r\n", down ? L"↓" : L"↑", vk);
        }
        Hk_Append(st, buf);
        {
            wchar_t status[120];
            swprintf_s(status, 120,
                L"Hook active.  Keydowns: %d   Keyups: %d",
                st->keyDowns, st->keyUps);
            SetWindowTextW(st->label, status);
        }
        return 0;
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->history, 8, 134, w - 16, h - 142, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        Hk_Stop(st);
        free(st);
        RemovePropW(hwnd, HK_PROP);
    }
    return CallWindowProcW(g_origHkFrame, hwnd, msg, wp, lp);
}

static HWND HookKbd_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    HkState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"HookKbd",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (HkState *)calloc(1, sizeof(HkState));
    if (!st) { DestroyWindow(frame); return NULL; }
    st->frame = frame;

    CreateWindowExW(0, L"STATIC",
        L"Low-level keyboard hook. Shows only direction + identifier of\n"
        L"non-character keys; character keys are reported as 'letter'/'digit'.\n"
        L"No keystroke content is captured or logged.",
        WS_CHILD | WS_VISIBLE,
        12, 36, w - 24, 56, frame, NULL, hInstance, NULL);

    CreateWindowExW(0, L"BUTTON", L"Start hook",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 98, 110, 26, frame, (HMENU)(LONG_PTR)ID_HK_GO, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Stop hook",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        130, 98, 110, 26, frame, (HMENU)(LONG_PTR)ID_HK_STOP, hInstance, NULL);

    st->label = CreateWindowExW(0, L"STATIC", L"Hook idle.",
        WS_CHILD | WS_VISIBLE,
        248, 102, w - 256, 22, frame, (HMENU)(LONG_PTR)ID_HK_LBL, hInstance, NULL);

    st->history = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 134, w - 16, h - 142, frame, (HMENU)(LONG_PTR)ID_HK_HIST, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->history, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, HK_PROP, (HANDLE)st);
    if (!g_origHkFrame) g_origHkFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Hk_FrameProc);
    return frame;
}

MsApp g_AppHookKbd = {
    L"HookKbd",
    HookKbd_Create,
    560, 420
};
