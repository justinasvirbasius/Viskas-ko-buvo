/*
 * app_guiinfo.c — Cross-process focus/caret/menu state via GetGUIThreadInfo
 *
 * Demonstrates GetGUIThreadInfo, which exposes a GUI thread's *internal*
 * input state that GetFocus/GetActiveWindow cannot return cross-process:
 *   - GUITHREADINFO.hwndActive   — the active window of that thread
 *   - GUITHREADINFO.hwndFocus    — control with keyboard focus
 *   - GUITHREADINFO.hwndCapture  — window with mouse capture
 *   - GUITHREADINFO.hwndMenuOwner / hwndMoveSize — open menu / drag state
 *   - GUITHREADINFO.hwndCaret + rcCaret — visible blinking caret + bounds
 *   - GUITHREADINFO.flags — GUI_CARETBLINKING, GUI_INMENUMODE, etc.
 *
 * To inspect a window from any other process: GetWindowThreadProcessId on
 * its HWND, then GetGUIThreadInfo(tid, &info). The structure must have
 * cbSize set to sizeof().
 *
 * We poll the foreground window every 250ms and dump its GUI state.
 */

#include "shell.h"
#include <stdlib.h>
#include <stdio.h>

#define GI_PROP    L"MS_GI_STATE"
#define ID_GI_OUT  96001
#define ID_GI_GO   96002
#define ID_GI_STOP 96003
#define GI_TIMER   1

typedef struct { HWND output; UINT_PTR timer; } GiState;
static WNDPROC g_origGiFrame = NULL;

static void Gi_DescribeFlags(DWORD flags, wchar_t *out, int cch)
{
    out[0] = 0;
    if (flags & GUI_CARETBLINKING) wcscat_s(out, cch, L"CARET ");
    if (flags & GUI_INMENUMODE)    wcscat_s(out, cch, L"MENU ");
    if (flags & GUI_INMOVESIZE)    wcscat_s(out, cch, L"MOVESIZE ");
    if (flags & GUI_POPUPMENUMODE) wcscat_s(out, cch, L"POPUPMENU ");
    if (flags & GUI_SYSTEMMENUMODE)wcscat_s(out, cch, L"SYSMENU ");
    if (!out[0]) wcscpy_s(out, cch, L"(none)");
}

static void Gi_Poll(GiState *st)
{
    HWND fg = GetForegroundWindow();
    DWORD tid, pid;
    GUITHREADINFO gti;
    wchar_t buf[2048];
    wchar_t title[200] = L"", cls[100] = L"", flags[100];
    int len;

    if (!fg) {
        SetWindowTextW(st->output, L"(no foreground window)\r\n");
        return;
    }
    tid = GetWindowThreadProcessId(fg, &pid);
    GetWindowTextW(fg, title, 200);
    GetClassNameW(fg, cls, 100);

    ZeroMemory(&gti, sizeof(gti));
    gti.cbSize = sizeof(gti);
    if (!GetGUIThreadInfo(tid, &gti)) {
        swprintf_s(buf, 2048, L"GetGUIThreadInfo failed (err %lu)\r\n", GetLastError());
        SetWindowTextW(st->output, buf);
        return;
    }

    Gi_DescribeFlags(gti.flags, flags, 100);

    len = swprintf_s(buf, 2048,
        L"== Foreground window ==\r\n"
        L"  HWND       : 0x%p\r\n"
        L"  PID / TID  : %lu / %lu\r\n"
        L"  Title      : %s\r\n"
        L"  Class      : %s\r\n"
        L"\r\n== GUITHREADINFO for that thread ==\r\n"
        L"  flags      : 0x%04lx (%s)\r\n"
        L"  hwndActive : 0x%p\r\n"
        L"  hwndFocus  : 0x%p\r\n"
        L"  hwndCapture: 0x%p\r\n"
        L"  hwndMenuOwn: 0x%p\r\n"
        L"  hwndMoveSiz: 0x%p\r\n"
        L"  hwndCaret  : 0x%p\r\n"
        L"  caretRect  : (%ld,%ld)-(%ld,%ld)\r\n",
        (void *)fg, pid, tid, title, cls,
        gti.flags, flags,
        (void *)gti.hwndActive,
        (void *)gti.hwndFocus,
        (void *)gti.hwndCapture,
        (void *)gti.hwndMenuOwner,
        (void *)gti.hwndMoveSize,
        (void *)gti.hwndCaret,
        gti.rcCaret.left, gti.rcCaret.top,
        gti.rcCaret.right, gti.rcCaret.bottom);

    if (gti.hwndFocus) {
        wchar_t fTitle[200] = L"", fCls[100] = L"";
        GetWindowTextW(gti.hwndFocus, fTitle, 200);
        GetClassNameW(gti.hwndFocus, fCls, 100);
        len += swprintf_s(buf + len, 2048 - len,
            L"\r\n  focus title: %s\r\n  focus class: %s\r\n", fTitle, fCls);
    }

    SetWindowTextW(st->output, buf);
}

static LRESULT CALLBACK Gi_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    GiState *st = (GiState *)GetPropW(hwnd, GI_PROP);
    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_GI_GO) {
            if (!st->timer) st->timer = SetTimer(hwnd, GI_TIMER, 250, NULL);
            Gi_Poll(st);
            return 0;
        }
        if (LOWORD(wp) == ID_GI_STOP) {
            if (st->timer) { KillTimer(hwnd, st->timer); st->timer = 0; }
            return 0;
        }
    }
    if (msg == WM_TIMER && st) { Gi_Poll(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->timer) KillTimer(hwnd, st->timer);
        free(st); RemovePropW(hwnd, GI_PROP);
    }
    return CallWindowProcW(g_origGiFrame, hwnd, msg, wp, lp);
}

static HWND GuiInfo_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    GiState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"GuiInfo",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (GiState *)calloc(1, sizeof(GiState));
    if (!st) { DestroyWindow(frame); return NULL; }

    CreateWindowExW(0, L"BUTTON", L"Start polling",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 130, 26, frame, (HMENU)(LONG_PTR)ID_GI_GO, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Stop",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        150, 38, 90, 26, frame, (HMENU)(LONG_PTR)ID_GI_STOP, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"Click Start; then click into other apps to see their internal GUI state.",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_GI_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, GI_PROP, (HANDLE)st);
    if (!g_origGiFrame) g_origGiFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Gi_FrameProc);
    return frame;
}

MsApp g_AppGuiInfo = { L"GuiInfo", GuiInfo_Create, 680, 480 };
