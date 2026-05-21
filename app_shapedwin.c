/*
 * app_shapedwin.c — Irregular (non-rectangular) window shape
 *
 * Demonstrates:
 *   - CreateRoundRectRgn / CreateEllipticRgn to build a shape
 *   - CombineRgn to subtract a punched-out interior hole
 *   - SetWindowRgn to apply the shape to the app frame
 *
 * Toggle buttons swap between three shapes: rounded rect, ellipse, and a
 * compound shape with a hole. The transparent pixels disappear: you can see
 * the desktop through them.
 */

#include "shell.h"
#include <stdlib.h>

#define SW_PROP    L"MS_SW_STATE"
#define ID_SW_ROUND 33001
#define ID_SW_OVAL  33002
#define ID_SW_HOLE  33003

typedef struct {
    HRGN currentRgn;
} ShState;

static WNDPROC g_origShFrame = NULL;

static void Sh_ApplyRound(HWND hwnd, ShState *st)
{
    RECT rc;
    HRGN rgn;
    GetWindowRect(hwnd, &rc);
    rgn = CreateRoundRectRgn(0, 0, rc.right - rc.left, rc.bottom - rc.top, 60, 60);
    SetWindowRgn(hwnd, rgn, TRUE);
    /* Windows takes ownership */
    st->currentRgn = rgn;
}

static void Sh_ApplyOval(HWND hwnd, ShState *st)
{
    RECT rc;
    HRGN rgn;
    GetWindowRect(hwnd, &rc);
    rgn = CreateEllipticRgn(0, 0, rc.right - rc.left, rc.bottom - rc.top);
    SetWindowRgn(hwnd, rgn, TRUE);
    st->currentRgn = rgn;
}

static void Sh_ApplyHole(HWND hwnd, ShState *st)
{
    RECT rc;
    HRGN outer, inner, combined;
    int w, h;

    GetWindowRect(hwnd, &rc);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;

    outer = CreateRoundRectRgn(0, 0, w, h, 40, 40);
    inner = CreateEllipticRgn(w / 2 - 50, h / 2 - 50, w / 2 + 50, h / 2 + 50);
    combined = CreateRectRgn(0, 0, 0, 0);
    CombineRgn(combined, outer, inner, RGN_DIFF);
    DeleteObject(outer);
    DeleteObject(inner);
    SetWindowRgn(hwnd, combined, TRUE);
    st->currentRgn = combined;
}

static LRESULT CALLBACK Sh_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    ShState *st = (ShState *)GetPropW(hwnd, SW_PROP);

    if (msg == WM_COMMAND && st) {
        switch (LOWORD(wp)) {
        case ID_SW_ROUND: Sh_ApplyRound(hwnd, st); return 0;
        case ID_SW_OVAL:  Sh_ApplyOval(hwnd, st);  return 0;
        case ID_SW_HOLE:  Sh_ApplyHole(hwnd, st);  return 0;
        }
    }
    if (msg == WM_DESTROY && st) {
        /* If we set a non-NULL region, the system owns it. Clearing first
         * just to be tidy. */
        SetWindowRgn(hwnd, NULL, FALSE);
        free(st);
        RemovePropW(hwnd, SW_PROP);
    }
    return CallWindowProcW(g_origShFrame, hwnd, msg, wp, lp);
}

static HWND ShapedWin_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    ShState *st;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"ShapedWin",
        WS_POPUP | WS_BORDER,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (ShState *)calloc(1, sizeof(ShState));
    if (!st) { DestroyWindow(frame); return NULL; }

    CreateWindowExW(0, L"STATIC", L"Try the buttons to reshape the window.",
        WS_CHILD | WS_VISIBLE,
        16, 40, w - 32, 20, frame, NULL, hInstance, NULL);

    CreateWindowExW(0, L"BUTTON", L"Rounded",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        16, 72, 80, 30, frame, (HMENU)(LONG_PTR)ID_SW_ROUND, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Oval",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        104, 72, 80, 30, frame, (HMENU)(LONG_PTR)ID_SW_OVAL, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"With hole",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        192, 72, 90, 30, frame, (HMENU)(LONG_PTR)ID_SW_HOLE, hInstance, NULL);

    SetPropW(frame, SW_PROP, (HANDLE)st);
    if (!g_origShFrame)
        g_origShFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Sh_FrameProc);

    /* Start with the rounded look */
    Sh_ApplyRound(frame, st);
    return frame;
}

MsApp g_AppShapedWin = {
    L"ShapedWin",
    ShapedWin_Create,
    340, 200
};
