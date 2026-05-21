/*
 * app_layered.c — Translucent (layered) window and AnimateWindow
 *
 * Demonstrates:
 *   - WS_EX_LAYERED extended style on the app frame
 *   - SetLayeredWindowAttributes for whole-window alpha and color-key
 *   - AnimateWindow for slide-in/out effects
 *
 * A slider controls the window alpha live; buttons demonstrate the
 * AnimateWindow effects (slide, blend, center).
 */

#include "shell.h"
#include <commctrl.h>
#include <stdlib.h>

#pragma comment(lib, "comctl32.lib")

#define LY_PROP    L"MS_LY_STATE"
#define ID_LY_TRK  24001
#define ID_LY_LBL  24002
#define ID_LY_SLIDE 24003
#define ID_LY_BLEND 24004
#define ID_LY_HIDE  24005

typedef struct {
    HWND track, label;
} LyState;

static WNDPROC g_origLyFrame = NULL;

static void Ly_UpdateAlpha(HWND frame, LyState *st)
{
    int v = (int)SendMessageW(st->track, TBM_GETPOS, 0, 0);
    wchar_t buf[32];
    SetLayeredWindowAttributes(frame, 0, (BYTE)v, LWA_ALPHA);
    swprintf_s(buf, 32, L"Alpha: %d / 255", v);
    SetWindowTextW(st->label, buf);
}

static LRESULT CALLBACK Ly_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    LyState *st = (LyState *)GetPropW(hwnd, LY_PROP);

    if (msg == WM_HSCROLL && st) { Ly_UpdateAlpha(hwnd, st); return 0; }
    if (msg == WM_COMMAND && st) {
        switch (LOWORD(wp)) {
        case ID_LY_SLIDE:
            ShowWindow(hwnd, SW_HIDE);
            AnimateWindow(hwnd, 350, AW_SLIDE | AW_HOR_POSITIVE);
            return 0;
        case ID_LY_BLEND:
            ShowWindow(hwnd, SW_HIDE);
            AnimateWindow(hwnd, 350, AW_BLEND);
            return 0;
        case ID_LY_HIDE:
            AnimateWindow(hwnd, 250, AW_HIDE | AW_BLEND);
            /* Re-show after 500 ms so the user can see it return */
            SetTimer(hwnd, 1, 500, NULL);
            return 0;
        }
    }
    if (msg == WM_TIMER) {
        KillTimer(hwnd, 1);
        AnimateWindow(hwnd, 300, AW_BLEND);
        return 0;
    }
    if (msg == WM_DESTROY && st) {
        free(st);
        RemovePropW(hwnd, LY_PROP);
    }
    return CallWindowProcW(g_origLyFrame, hwnd, msg, wp, lp);
}

static HWND Layered_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    LyState *st;
    INITCOMMONCONTROLSEX icc;
    (void)self;

    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        MS_CLASS_APPFRAME, L"Layered",
        WS_POPUP | WS_BORDER,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (LyState *)calloc(1, sizeof(LyState));
    if (!st) { DestroyWindow(frame); return NULL; }

    /* Start at ~80% opacity */
    SetLayeredWindowAttributes(frame, 0, 200, LWA_ALPHA);

    st->label = CreateWindowExW(0, L"STATIC", L"Alpha: 200 / 255",
        WS_CHILD | WS_VISIBLE,
        12, 40, w - 24, 18, frame, (HMENU)(LONG_PTR)ID_LY_LBL, hInstance, NULL);

    st->track = CreateWindowExW(0, TRACKBAR_CLASSW, NULL,
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
        12, 60, w - 24, 30, frame, (HMENU)(LONG_PTR)ID_LY_TRK, hInstance, NULL);
    SendMessageW(st->track, TBM_SETRANGE, TRUE, MAKELPARAM(40, 255));
    SendMessageW(st->track, TBM_SETPOS,   TRUE, 200);

    CreateWindowExW(0, L"BUTTON", L"Animate slide",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        12, 100, 130, 28, frame, (HMENU)(LONG_PTR)ID_LY_SLIDE, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Animate blend",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        152, 100, 130, 28, frame, (HMENU)(LONG_PTR)ID_LY_BLEND, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Hide & restore",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        12, 136, 270, 28, frame, (HMENU)(LONG_PTR)ID_LY_HIDE, hInstance, NULL);

    SetPropW(frame, LY_PROP, (HANDLE)st);
    if (!g_origLyFrame) g_origLyFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Ly_FrameProc);
    return frame;
}

MsApp g_AppLayered = {
    L"Layered",
    Layered_Create,
    320, 200
};
