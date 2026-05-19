/*
 * app_color.c — RGB color picker
 *
 * Three trackbar (slider) controls for R, G, B and a large preview area.
 * The current hex value is rendered into the preview. Uses the standard
 * TRACKBAR_CLASS from comctl32.
 */

#include "shell.h"
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>

#pragma comment(lib, "comctl32.lib")

#define COLOR_PROP L"MS_COLOR_STATE"
#define ID_R 4001
#define ID_G 4002
#define ID_B 4003

typedef struct {
    HWND     trackR, trackG, trackB;
    HWND     labelR, labelG, labelB;
    int      r, g, b;
} ColorState;

static WNDPROC g_origColorFrame = NULL;

static void Color_UpdateLabels(ColorState *st)
{
    wchar_t buf[16];
    swprintf_s(buf, 16, L"R: %3d", st->r); SetWindowTextW(st->labelR, buf);
    swprintf_s(buf, 16, L"G: %3d", st->g); SetWindowTextW(st->labelG, buf);
    swprintf_s(buf, 16, L"B: %3d", st->b); SetWindowTextW(st->labelB, buf);
}

static void Color_Paint(HWND hwnd, ColorState *st)
{
    PAINTSTRUCT ps;
    HDC hdc;
    RECT rc, swatch;
    HBRUSH br;
    HFONT font, oldFont;
    wchar_t hex[16];

    hdc = BeginPaint(hwnd, &ps);
    GetClientRect(hwnd, &rc);

    /* Background for the whole frame interior — title bar painted separately */
    {
        RECT inner = rc;
        inner.top = 32;
        br = CreateSolidBrush(RGB(245, 245, 250));
        FillRect(hdc, &inner, br);
        DeleteObject(br);
    }

    /* Big swatch on the right */
    swatch.left = rc.right - 130;
    swatch.top = 44;
    swatch.right = rc.right - 12;
    swatch.bottom = swatch.top + 130;
    br = CreateSolidBrush(RGB(st->r, st->g, st->b));
    FillRect(hdc, &swatch, br);
    DeleteObject(br);
    FrameRect(hdc, &swatch, (HBRUSH)GetStockObject(BLACK_BRUSH));

    /* Hex code below the swatch */
    swprintf_s(hex, 16, L"#%02X%02X%02X", st->r, st->g, st->b);
    font = CreateFontW(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    oldFont = (HFONT)SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(30, 30, 40));
    {
        RECT hexR = swatch;
        hexR.top = swatch.bottom + 6;
        hexR.bottom = hexR.top + 24;
        DrawTextW(hdc, hex, -1, &hexR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(hdc, oldFont);
    DeleteObject(font);

    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK Color_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    ColorState *st = (ColorState *)GetPropW(hwnd, COLOR_PROP);

    if (msg == WM_HSCROLL && st) {
        st->r = (int)SendMessageW(st->trackR, TBM_GETPOS, 0, 0);
        st->g = (int)SendMessageW(st->trackG, TBM_GETPOS, 0, 0);
        st->b = (int)SendMessageW(st->trackB, TBM_GETPOS, 0, 0);
        Color_UpdateLabels(st);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    if (msg == WM_PAINT && st) {
        Color_Paint(hwnd, st);
        return 0;
    }
    if (msg == WM_DESTROY) {
        if (st) free(st);
        RemovePropW(hwnd, COLOR_PROP);
    }
    return CallWindowProcW(g_origColorFrame, hwnd, msg, wp, lp);
}

static HWND Color_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    ColorState *st;
    INITCOMMONCONTROLSEX icc;
    int trackW, trackY;

    (void)self;
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Color",
        WS_POPUP | WS_BORDER,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (ColorState *)calloc(1, sizeof(ColorState));
    if (!st) { DestroyWindow(frame); return NULL; }
    st->r = 80; st->g = 140; st->b = 220;

    trackW = w - 180;
    trackY = 48;

    st->labelR = CreateWindowExW(0, L"STATIC", L"R: 80",
        WS_CHILD | WS_VISIBLE, 12, trackY,      52, 18, frame, NULL, hInstance, NULL);
    st->trackR = CreateWindowExW(0, TRACKBAR_CLASSW, NULL,
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
        66, trackY - 4, trackW, 28,
        frame, (HMENU)(LONG_PTR)ID_R, hInstance, NULL);

    st->labelG = CreateWindowExW(0, L"STATIC", L"G: 140",
        WS_CHILD | WS_VISIBLE, 12, trackY + 36, 52, 18, frame, NULL, hInstance, NULL);
    st->trackG = CreateWindowExW(0, TRACKBAR_CLASSW, NULL,
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
        66, trackY + 32, trackW, 28,
        frame, (HMENU)(LONG_PTR)ID_G, hInstance, NULL);

    st->labelB = CreateWindowExW(0, L"STATIC", L"B: 220",
        WS_CHILD | WS_VISIBLE, 12, trackY + 72, 52, 18, frame, NULL, hInstance, NULL);
    st->trackB = CreateWindowExW(0, TRACKBAR_CLASSW, NULL,
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
        66, trackY + 68, trackW, 28,
        frame, (HMENU)(LONG_PTR)ID_B, hInstance, NULL);

    SendMessageW(st->trackR, TBM_SETRANGE, TRUE, MAKELPARAM(0, 255));
    SendMessageW(st->trackG, TBM_SETRANGE, TRUE, MAKELPARAM(0, 255));
    SendMessageW(st->trackB, TBM_SETRANGE, TRUE, MAKELPARAM(0, 255));
    SendMessageW(st->trackR, TBM_SETPOS, TRUE, st->r);
    SendMessageW(st->trackG, TBM_SETPOS, TRUE, st->g);
    SendMessageW(st->trackB, TBM_SETPOS, TRUE, st->b);

    SetPropW(frame, COLOR_PROP, (HANDLE)st);
    if (!g_origColorFrame)
        g_origColorFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Color_FrameProc);
    return frame;
}

MsApp g_AppColor = {
    L"Color",
    Color_Create,
    420, 220
};
