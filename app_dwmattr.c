/*
 * app_dwmattr.c — Desktop Window Manager attributes
 *
 * Demonstrates the dwmapi.dll modern window-chrome controls (Win 10 1809+
 * for immersive dark mode; Win 11 22000+ for caption colors and corners):
 *   - DwmExtendFrameIntoClientArea with negative margins for full-glass effect
 *   - DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE) to swap the title
 *     bar to dark theme
 *   - DwmSetWindowAttribute(DWMWA_CAPTION_COLOR) custom title bar color
 *     (Win 11)
 *   - DwmSetWindowAttribute(DWMWA_WINDOW_CORNER_PREFERENCE) for rounded
 *     corners (Win 11)
 *   - DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS) reading the real
 *     visible bounds (includes shadow)
 *
 * Several effects only apply on Windows 11. On earlier versions the calls
 * return success but have no visible effect, which is fine; the app prints
 * each call's HRESULT to make this visible.
 */

#include "shell.h"
#include <dwmapi.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "dwmapi.lib")

/* DWMWA values may be missing in older SDKs; define defensively. */
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_DEFAULT
#define DWMWCP_DEFAULT     0
#define DWMWCP_DONOTROUND  1
#define DWMWCP_ROUND       2
#define DWMWCP_ROUNDSMALL  3
#endif
#ifndef DWMWA_EXTENDED_FRAME_BOUNDS
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#endif

#define DA_PROP    L"MS_DA_STATE"
#define ID_DA_DARK 63001
#define ID_DA_CAP  63002
#define ID_DA_RND  63003
#define ID_DA_GLASS 63004
#define ID_DA_RST  63005
#define ID_DA_OUT  63006

typedef struct {
    HWND output;
    BOOL darkOn;
    BOOL capOn;
} DaState;

static WNDPROC g_origDaFrame = NULL;

static void Da_Append(DaState *st, const wchar_t *t)
{
    int len = GetWindowTextLengthW(st->output);
    SendMessageW(st->output, EM_SETSEL, len, len);
    SendMessageW(st->output, EM_REPLACESEL, FALSE, (LPARAM)t);
    SendMessageW(st->output, EM_SCROLLCARET, 0, 0);
}

static void Da_Report(DaState *st, const wchar_t *what, HRESULT hr)
{
    wchar_t buf[200];
    swprintf_s(buf, 200, L"  %-32s → hr=0x%08lX %s\r\n",
               what, (long)hr,
               SUCCEEDED(hr) ? L"(applied or ignored)" : L"(failed)");
    Da_Append(st, buf);
}

static void Da_ToggleDark(HWND hwnd, DaState *st)
{
    BOOL value = !st->darkOn;
    HRESULT hr;
    hr = DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                                &value, sizeof(value));
    if (SUCCEEDED(hr)) st->darkOn = value;
    Da_Report(st, L"immersive dark mode", hr);
}

static void Da_ToggleCap(HWND hwnd, DaState *st)
{
    COLORREF value = st->capOn ? DWMWA_COLOR_DEFAULT : RGB(64, 32, 96);
    HRESULT hr;
    hr = DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &value, sizeof(value));
    if (SUCCEEDED(hr)) st->capOn = !st->capOn;
    Da_Report(st, L"caption color (Win 11)", hr);
}

static void Da_Rounded(HWND hwnd, DaState *st)
{
    UINT pref = DWMWCP_ROUND;
    HRESULT hr;
    hr = DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                                &pref, sizeof(pref));
    Da_Report(st, L"rounded corners (Win 11)", hr);
}

static void Da_Glass(HWND hwnd, DaState *st)
{
    MARGINS m = { -1, -1, -1, -1 };
    HRESULT hr = DwmExtendFrameIntoClientArea(hwnd, &m);
    Da_Report(st, L"extend frame into client", hr);
}

static void Da_Reset(HWND hwnd, DaState *st)
{
    BOOL off = FALSE;
    COLORREF def = DWMWA_COLOR_DEFAULT;
    UINT pref = DWMWCP_DEFAULT;
    MARGINS m = { 0, 0, 0, 0 };
    Da_Report(st, L"reset: dark mode off",
              DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &off, sizeof(off)));
    Da_Report(st, L"reset: caption color default",
              DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &def, sizeof(def)));
    Da_Report(st, L"reset: corner default",
              DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref)));
    Da_Report(st, L"reset: extend frame = none",
              DwmExtendFrameIntoClientArea(hwnd, &m));
    st->darkOn = st->capOn = FALSE;
}

static void Da_ShowBounds(HWND hwnd, DaState *st)
{
    RECT rcWin, rcExt;
    HRESULT hr;
    wchar_t buf[200];
    GetWindowRect(hwnd, &rcWin);
    hr = DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                &rcExt, sizeof(rcExt));
    if (SUCCEEDED(hr)) {
        swprintf_s(buf, 200,
            L"  Window:   %ld,%ld → %ld,%ld\r\n"
            L"  Ext.frame:%ld,%ld → %ld,%ld\r\n",
            rcWin.left, rcWin.top, rcWin.right, rcWin.bottom,
            rcExt.left, rcExt.top, rcExt.right, rcExt.bottom);
        Da_Append(st, buf);
    }
}

static LRESULT CALLBACK Da_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    DaState *st = (DaState *)GetPropW(hwnd, DA_PROP);
    if (msg == WM_COMMAND && st) {
        switch (LOWORD(wp)) {
        case ID_DA_DARK:  Da_ToggleDark(hwnd, st);  Da_ShowBounds(hwnd, st); return 0;
        case ID_DA_CAP:   Da_ToggleCap(hwnd, st);   Da_ShowBounds(hwnd, st); return 0;
        case ID_DA_RND:   Da_Rounded(hwnd, st);     return 0;
        case ID_DA_GLASS: Da_Glass(hwnd, st);       return 0;
        case ID_DA_RST:   Da_Reset(hwnd, st);       return 0;
        }
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 80, w - 16, h - 88, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, DA_PROP); }
    return CallWindowProcW(g_origDaFrame, hwnd, msg, wp, lp);
}

static HWND DwmAttr_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    DaState *st;
    HFONT mono;
    (void)self;

    /* DwmAttr needs its own top-level window to demonstrate caption effects,
       so we still use the app frame class but the user will see Mr. SHell's
       frame, not the system's. The DWM calls still work against the HWND. */
    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"DwmAttr",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (DaState *)calloc(1, sizeof(DaState));
    if (!st) { DestroyWindow(frame); return NULL; }

    CreateWindowExW(0, L"STATIC",
        L"DWM attributes. Effects on title bar are most visible on Windows 11.\n"
        L"On Windows 10, dark mode applies; caption color/rounded corners are\n"
        L"silently accepted but not rendered.",
        WS_CHILD | WS_VISIBLE,
        12, 30, w - 24, 48, frame, NULL, hInstance, NULL);

    CreateWindowExW(0, L"BUTTON", L"Dark",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 78, 70, 26, frame, (HMENU)(LONG_PTR)ID_DA_DARK, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Caption",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        90, 78, 86, 26, frame, (HMENU)(LONG_PTR)ID_DA_CAP, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Rounded",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        184, 78, 86, 26, frame, (HMENU)(LONG_PTR)ID_DA_RND, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Extend frame",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        278, 78, 110, 26, frame, (HMENU)(LONG_PTR)ID_DA_GLASS, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Reset",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        396, 78, 70, 26, frame, (HMENU)(LONG_PTR)ID_DA_RST, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 116, w - 16, h - 124, frame, (HMENU)(LONG_PTR)ID_DA_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, DA_PROP, (HANDLE)st);
    if (!g_origDaFrame) g_origDaFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Da_FrameProc);

    Da_ShowBounds(frame, st);
    SendMessageW(frame, WM_SIZE, 0, MAKELPARAM(w, h));
    return frame;
}

MsApp g_AppDwmAttr = {
    L"DwmAttr",
    DwmAttr_Create,
    560, 400
};
