/*
 * app_dpiaware.c — DPI awareness and per-monitor scaling
 *
 * Demonstrates:
 *   - GetDpiForWindow (Win10+) to read the current effective DPI
 *   - WM_DPICHANGED: re-laying out child controls and resizing fonts
 *     when the window is dragged between monitors with different scaling
 *   - GetSystemMetricsForDpi for DPI-aware metrics
 *
 * The shell process itself should be made per-monitor DPI aware via
 * SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2),
 * which we attempt dynamically (the symbol is Win10 1703+).
 */

#include "shell.h"
#include <stdlib.h>
#include <stdio.h>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

/* Dynamic-load SetProcessDpiAwarenessContext so we don't fail on older Windows */
typedef HANDLE DPI_AWARENESS_CONTEXT_LOCAL;
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2_LOCAL ((DPI_AWARENESS_CONTEXT_LOCAL)-4)

typedef BOOL (WINAPI *SetProcessDpiAwarenessContextFn)(DPI_AWARENESS_CONTEXT_LOCAL);
typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
typedef int  (WINAPI *GetSystemMetricsForDpiFn)(int, UINT);

static UINT Dpi_GetForWindow(HWND hwnd)
{
    HMODULE user = GetModuleHandleW(L"user32.dll");
    GetDpiForWindowFn fn = user ?
        (GetDpiForWindowFn)GetProcAddress(user, "GetDpiForWindow") : NULL;
    return fn ? fn(hwnd) : 96;
}

#define DPI_PROP   L"MS_DPI_STATE"
#define ID_DPI_LBL 30001

typedef struct {
    HWND  info, dpiLbl, scaleLbl, metricsLbl, sampleBtn;
    HFONT scaledFont;
} DpiState;

static WNDPROC g_origDpiFrame = NULL;

static void Dpi_BuildFont(DpiState *st, UINT dpi)
{
    int pt = MulDiv(11, (int)dpi, 96);
    if (st->scaledFont) DeleteObject(st->scaledFont);
    st->scaledFont = CreateFontW(-MulDiv(pt, 1, 1), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

static void Dpi_UpdateLabels(HWND hwnd, DpiState *st)
{
    UINT dpi = Dpi_GetForWindow(hwnd);
    wchar_t buf[120];

    swprintf_s(buf, 120, L"Current DPI: %u (%u%% scaling)",
               dpi, (dpi * 100) / 96);
    SetWindowTextW(st->dpiLbl, buf);

    swprintf_s(buf, 120, L"Effective scale factor: %.2fx", (double)dpi / 96.0);
    SetWindowTextW(st->scaleLbl, buf);

    {
        HMODULE user = GetModuleHandleW(L"user32.dll");
        GetSystemMetricsForDpiFn fn = user ?
            (GetSystemMetricsForDpiFn)GetProcAddress(user, "GetSystemMetricsForDpi") : NULL;
        int caption = fn ? fn(SM_CYCAPTION, dpi) : GetSystemMetrics(SM_CYCAPTION);
        int frame   = fn ? fn(SM_CXFRAME,   dpi) : GetSystemMetrics(SM_CXFRAME);
        swprintf_s(buf, 120, L"DPI-aware metrics: caption=%dpx, frame=%dpx",
                   caption, frame);
        SetWindowTextW(st->metricsLbl, buf);
    }

    Dpi_BuildFont(st, dpi);
    SendMessageW(st->sampleBtn, WM_SETFONT, (WPARAM)st->scaledFont, TRUE);
}

static LRESULT CALLBACK Dpi_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    DpiState *st = (DpiState *)GetPropW(hwnd, DPI_PROP);

    if (msg == WM_DPICHANGED && st) {
        /* lparam = suggested new RECT for the window on the new monitor */
        RECT *r = (RECT *)lp;
        SetWindowPos(hwnd, NULL,
                     r->left, r->top,
                     r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        Dpi_UpdateLabels(hwnd, st);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    if (msg == WM_DESTROY && st) {
        if (st->scaledFont) DeleteObject(st->scaledFont);
        free(st);
        RemovePropW(hwnd, DPI_PROP);
    }
    return CallWindowProcW(g_origDpiFrame, hwnd, msg, wp, lp);
}

static HWND DpiAware_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    DpiState *st;
    HMODULE user;
    SetProcessDpiAwarenessContextFn setCtx;
    (void)self;

    /* Try to upgrade the whole process to per-monitor DPI v2 if available.
     * Idempotent — safe to call multiple times. */
    user = GetModuleHandleW(L"user32.dll");
    setCtx = user ? (SetProcessDpiAwarenessContextFn)GetProcAddress(
                user, "SetProcessDpiAwarenessContext") : NULL;
    if (setCtx) setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2_LOCAL);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"DpiAware",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (DpiState *)calloc(1, sizeof(DpiState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->info = CreateWindowExW(0, L"STATIC",
        L"Drag this window between monitors with different scaling to see\n"
        L"WM_DPICHANGED in action.",
        WS_CHILD | WS_VISIBLE,
        12, 36, w - 24, 36, frame, NULL, hInstance, NULL);

    st->dpiLbl = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE,
        12, 80, w - 24, 20, frame, (HMENU)(LONG_PTR)ID_DPI_LBL, hInstance, NULL);
    st->scaleLbl = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE,
        12, 102, w - 24, 20, frame, NULL, hInstance, NULL);
    st->metricsLbl = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE,
        12, 124, w - 24, 20, frame, NULL, hInstance, NULL);
    st->sampleBtn = CreateWindowExW(0, L"BUTTON", L"DPI-scaled text",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        12, 160, 200, 36, frame, NULL, hInstance, NULL);

    SetPropW(frame, DPI_PROP, (HANDLE)st);
    if (!g_origDpiFrame)
        g_origDpiFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Dpi_FrameProc);

    Dpi_UpdateLabels(frame, st);
    return frame;
}

MsApp g_AppDpiAware = {
    L"DpiAware",
    DpiAware_Create,
    420, 240
};
