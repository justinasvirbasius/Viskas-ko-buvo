/*
 * app_sesshook.c — Session lock/unlock and console-switch events
 *
 * Demonstrates WTSRegisterSessionNotification + WM_WTSSESSION_CHANGE,
 * which lets a GUI app respond to:
 *   - WTS_SESSION_LOCK    (workstation locked, e.g. Win+L)
 *   - WTS_SESSION_UNLOCK  (workstation unlocked)
 *   - WTS_CONSOLE_CONNECT / WTS_CONSOLE_DISCONNECT (fast user switch)
 *   - WTS_REMOTE_CONNECT / WTS_REMOTE_DISCONNECT (RDP session attach)
 *   - WTS_SESSION_LOGON / WTS_SESSION_LOGOFF
 *
 * The lParam of WM_WTSSESSION_CHANGE is the session ID; wParam is the event
 * code above. NOTIFY_FOR_THIS_SESSION filters to our session; THIS_SESSION
 * is the typical choice for a per-user notifier.
 *
 * UnregisterSessionNotification on shutdown.
 */

#include "shell.h"
#include <wtsapi32.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "wtsapi32.lib")

#ifndef WM_WTSSESSION_CHANGE
#define WM_WTSSESSION_CHANGE 0x02B1
#endif

#define SH_PROP    L"MS_SHK_STATE"
#define ID_SH_GO   80001
#define ID_SH_STOP 80002
#define ID_SH_OUT  80003

typedef struct {
    HWND output;
    BOOL registered;
} ShState;

static WNDPROC g_origShFrame = NULL;

static const wchar_t *Sh_EventName(WPARAM wp)
{
    switch (wp) {
    case WTS_CONSOLE_CONNECT:        return L"console-connect";
    case WTS_CONSOLE_DISCONNECT:     return L"console-disconnect";
    case WTS_REMOTE_CONNECT:         return L"remote-connect";
    case WTS_REMOTE_DISCONNECT:      return L"remote-disconnect";
    case WTS_SESSION_LOGON:          return L"logon";
    case WTS_SESSION_LOGOFF:         return L"logoff";
    case WTS_SESSION_LOCK:           return L"LOCK";
    case WTS_SESSION_UNLOCK:         return L"UNLOCK";
    case WTS_SESSION_REMOTE_CONTROL: return L"remote-control";
    }
    return L"?";
}

static void Sh_Append(ShState *st, const wchar_t *t)
{
    int len = GetWindowTextLengthW(st->output);
    SendMessageW(st->output, EM_SETSEL, len, len);
    SendMessageW(st->output, EM_REPLACESEL, FALSE, (LPARAM)t);
    SendMessageW(st->output, EM_SCROLLCARET, 0, 0);
}

static void Sh_Start(HWND frame, ShState *st)
{
    if (st->registered) return;
    if (WTSRegisterSessionNotification(frame, NOTIFY_FOR_THIS_SESSION)) {
        st->registered = TRUE;
        Sh_Append(st,
            L"Registered for session notifications.\r\n"
            L"Press Win+L to lock, then unlock, to see events.\r\n\r\n");
    } else {
        Sh_Append(st, L"WTSRegisterSessionNotification failed.\r\n");
    }
}

static void Sh_Stop(HWND frame, ShState *st)
{
    if (!st->registered) return;
    WTSUnRegisterSessionNotification(frame);
    st->registered = FALSE;
    Sh_Append(st, L"Unregistered.\r\n");
}

static LRESULT CALLBACK Sh_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    ShState *st = (ShState *)GetPropW(hwnd, SH_PROP);
    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_SH_GO)   { Sh_Start(hwnd, st); return 0; }
        if (LOWORD(wp) == ID_SH_STOP) { Sh_Stop(hwnd, st);  return 0; }
    }
    if (msg == WM_WTSSESSION_CHANGE && st) {
        wchar_t buf[160];
        SYSTEMTIME t; GetLocalTime(&t);
        swprintf_s(buf, 160,
            L"  [%02u:%02u:%02u]  %s  (session %lu)\r\n",
            t.wHour, t.wMinute, t.wSecond,
            Sh_EventName(wp), (DWORD)lp);
        Sh_Append(st, buf);
        return 0;
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        Sh_Stop(hwnd, st);
        free(st); RemovePropW(hwnd, SH_PROP);
    }
    return CallWindowProcW(g_origShFrame, hwnd, msg, wp, lp);
}

static HWND SessHook_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    ShState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"SessHook",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (ShState *)calloc(1, sizeof(ShState));
    if (!st) { DestroyWindow(frame); return NULL; }

    CreateWindowExW(0, L"BUTTON", L"Register",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 110, 26, frame, (HMENU)(LONG_PTR)ID_SH_GO, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Unregister",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        130, 38, 110, 26, frame, (HMENU)(LONG_PTR)ID_SH_STOP, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_SH_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, SH_PROP, (HANDLE)st);
    if (!g_origShFrame) g_origShFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Sh_FrameProc);
    return frame;
}

MsApp g_AppSessHook = { L"SessHook", SessHook_Create, 600, 400 };
