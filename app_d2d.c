/*
 * app_d2d.c — Direct2D + DirectWrite canvas
 *
 * Demonstrates the modern hardware-accelerated 2D path:
 *   - D2D1CreateFactory → ID2D1Factory
 *   - ID2D1HwndRenderTarget bound to a child window's HWND
 *   - DWriteCreateFactory → IDWriteFactory → IDWriteTextFormat
 *   - Animated content (rotating gradient ring + text) on a timer
 *   - BeginDraw / EndDraw with D2DERR_RECREATE_TARGET handling
 *
 * Plain C bindings via COBJMACROS + CINTERFACE (lpVtbl-> form).
 */

#define COBJMACROS
#define CINTERFACE

#include "shell.h"
#include <d2d1.h>
#include <dwrite.h>
#include <math.h>
#include <stdlib.h>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

#define D2D_CLASS L"MiniShell_D2DCanvas"
#define D2D_TIMER 1

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    ID2D1Factory       *factory;
    ID2D1HwndRenderTarget *rt;
    IDWriteFactory     *dwrite;
    IDWriteTextFormat  *textFormat;
    ID2D1SolidColorBrush *brush;
    float angle;
} D2dState;

static void D2d_DiscardTarget(D2dState *st)
{
    if (st->brush) { ID2D1SolidColorBrush_Release(st->brush); st->brush = NULL; }
    if (st->rt)    { ID2D1HwndRenderTarget_Release(st->rt);   st->rt = NULL; }
}

static BOOL D2d_EnsureTarget(HWND hwnd, D2dState *st)
{
    RECT rc;
    D2D1_RENDER_TARGET_PROPERTIES props;
    D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps;
    D2D1_SIZE_U size;
    D2D1_COLOR_F color;
    HRESULT hr;

    if (st->rt) return TRUE;
    GetClientRect(hwnd, &rc);
    size.width  = (UINT32)(rc.right);
    size.height = (UINT32)(rc.bottom);
    if (size.width == 0 || size.height == 0) return FALSE;

    ZeroMemory(&props, sizeof(props));
    props.type        = D2D1_RENDER_TARGET_TYPE_DEFAULT;
    props.pixelFormat.format    = DXGI_FORMAT_UNKNOWN;
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    props.dpiX = 96; props.dpiY = 96;

    ZeroMemory(&hwndProps, sizeof(hwndProps));
    hwndProps.hwnd        = hwnd;
    hwndProps.pixelSize   = size;
    hwndProps.presentOptions = D2D1_PRESENT_OPTIONS_NONE;

    hr = ID2D1Factory_CreateHwndRenderTarget(st->factory, &props, &hwndProps, &st->rt);
    if (FAILED(hr)) return FALSE;

    color.r = 1.0f; color.g = 1.0f; color.b = 1.0f; color.a = 1.0f;
    ID2D1HwndRenderTarget_CreateSolidColorBrush(st->rt, &color, NULL, &st->brush);
    return TRUE;
}

static void D2d_Render(HWND hwnd, D2dState *st)
{
    D2D1_COLOR_F bg;
    D2D1_SIZE_F  rtSize;
    HRESULT hr;
    int i;

    if (!D2d_EnsureTarget(hwnd, st)) return;

    ID2D1HwndRenderTarget_BeginDraw(st->rt);

    bg.r = 0.10f; bg.g = 0.12f; bg.b = 0.18f; bg.a = 1.0f;
    ID2D1HwndRenderTarget_Clear(st->rt, &bg);

    rtSize = ID2D1HwndRenderTarget_GetSize(st->rt);
    {
        float cx = rtSize.width * 0.5f;
        float cy = rtSize.height * 0.5f;
        float r  = (rtSize.width < rtSize.height ? rtSize.width : rtSize.height) * 0.35f;

        /* Animated colored arcs around a ring */
        for (i = 0; i < 24; ++i) {
            float t = (float)i / 24.0f;
            float a = (t * 2.0f * (float)M_PI) + (st->angle * (float)M_PI / 180.0f);
            float ex = cx + (float)cos(a) * r;
            float ey = cy + (float)sin(a) * r;
            D2D1_COLOR_F col;
            D2D1_ELLIPSE el;
            col.r = 0.4f + 0.5f * (float)sin(a);
            col.g = 0.4f + 0.5f * (float)sin(a + 2.0f);
            col.b = 0.4f + 0.5f * (float)sin(a + 4.0f);
            col.a = 1.0f;
            ID2D1SolidColorBrush_SetColor(st->brush, &col);
            el.point.x = ex; el.point.y = ey;
            el.radiusX = 10.0f + 4.0f * (float)sin(a * 3);
            el.radiusY = el.radiusX;
            ID2D1HwndRenderTarget_FillEllipse(st->rt, &el, (ID2D1Brush *)st->brush);
        }

        /* Center text */
        if (st->textFormat) {
            D2D1_RECT_F textRc;
            D2D1_COLOR_F white = { 1.0f, 1.0f, 1.0f, 1.0f };
            const wchar_t *msg = L"Direct2D + DirectWrite";
            textRc.left = 0; textRc.top = cy - 12;
            textRc.right = rtSize.width; textRc.bottom = cy + 12;
            ID2D1SolidColorBrush_SetColor(st->brush, &white);
            ID2D1HwndRenderTarget_DrawTextW(st->rt, msg, (UINT32)wcslen(msg),
                st->textFormat, &textRc, (ID2D1Brush *)st->brush,
                D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
        }
    }

    hr = ID2D1HwndRenderTarget_EndDraw(st->rt, NULL, NULL);
    if (hr == D2DERR_RECREATE_TARGET) {
        D2d_DiscardTarget(st);
    }
}

static LRESULT CALLBACK D2d_CanvasProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    D2dState *st = (D2dState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        HRESULT hr;
        DWRITE_TEXT_ALIGNMENT ta = DWRITE_TEXT_ALIGNMENT_CENTER;
        st = (D2dState *)calloc(1, sizeof(D2dState));
        if (!st) return -1;

        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                               &IID_ID2D1Factory, NULL, (void **)&st->factory);
        if (FAILED(hr)) { free(st); return -1; }

        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                 &IID_IDWriteFactory, (IUnknown **)&st->dwrite);
        if (SUCCEEDED(hr)) {
            IDWriteFactory_CreateTextFormat(st->dwrite,
                L"Segoe UI", NULL,
                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"en-us",
                &st->textFormat);
            if (st->textFormat) {
                IDWriteTextFormat_SetTextAlignment(st->textFormat, ta);
            }
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        SetTimer(hwnd, D2D_TIMER, 16, NULL);
        return 0;
    }

    case WM_TIMER:
        st->angle += 1.5f;
        if (st->angle > 360.0f) st->angle -= 360.0f;
        D2d_Render(hwnd, st);
        return 0;

    case WM_SIZE:
        if (st && st->rt) {
            D2D1_SIZE_U sz;
            sz.width  = LOWORD(lp);
            sz.height = HIWORD(lp);
            if (sz.width > 0 && sz.height > 0) {
                ID2D1HwndRenderTarget_Resize(st->rt, &sz);
            }
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        D2d_Render(hwnd, st);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, D2D_TIMER);
        if (st) {
            D2d_DiscardTarget(st);
            if (st->textFormat) IDWriteTextFormat_Release(st->textFormat);
            if (st->dwrite)     IDWriteFactory_Release(st->dwrite);
            if (st->factory)    ID2D1Factory_Release(st->factory);
            free(st);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void EnsureD2dClass(HINSTANCE hInstance)
{
    static BOOL registered = FALSE;
    WNDCLASSEXW wc;
    if (registered) return;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = D2d_CanvasProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = D2D_CLASS;
    RegisterClassExW(&wc);
    registered = TRUE;
}

static WNDPROC g_origD2dFrame = NULL;

static LRESULT CALLBACK D2d_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_SIZE) {
        HWND canvas = FindWindowExW(hwnd, NULL, D2D_CLASS, NULL);
        if (canvas) {
            int w = LOWORD(lp), h = HIWORD(lp);
            MoveWindow(canvas, 4, 32, w - 8, h - 36, TRUE);
        }
    }
    return CallWindowProcW(g_origD2dFrame, hwnd, msg, wp, lp);
}

static HWND D2D_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    (void)self;

    EnsureD2dClass(hInstance);
    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"D2D",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    CreateWindowExW(0, D2D_CLASS, L"",
        WS_CHILD | WS_VISIBLE,
        4, 32, w - 8, h - 36, frame, NULL, hInstance, NULL);

    if (!g_origD2dFrame)
        g_origD2dFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)D2d_FrameProc);
    return frame;
}

MsApp g_AppD2D = {
    L"D2D",
    D2D_Create,
    440, 360
};
