/*
 * app_dibdraw.c — Direct pixel access via CreateDIBSection
 *
 * Demonstrates software rendering with full per-pixel control:
 *   - CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &pBits, NULL, 0) creates
 *     a top-down 32-bit BGRA bitmap and returns a pointer to its raw
 *     pixel memory in pBits — writes are direct, no GDI calls per pixel
 *   - SelectObject into a memory DC, draw on the bits, then BitBlt to the
 *     window for display
 *   - 32-bit pixel layout: pixel[y * width + x] = 0x00BBGGRR (little-endian
 *     means bytes are B,G,R,X in memory)
 *
 * This app renders a Mandelbrot set, which exercises millions of pixel
 * writes per repaint. Each frame is fully computed on the CPU and BitBlt'd
 * once — a useful template for any custom software renderer (game blitter,
 * shader visualizer, plotting library).
 *
 * Click to zoom in 2x at the click point; right-click resets.
 */

#include "shell.h"
#include <stdlib.h>
#include <stdio.h>

#define DD_PROP    L"MS_DD_STATE"

typedef struct {
    HBITMAP    bmp;
    DWORD     *bits;
    int        w, h;
    HDC        memDC;
    HBITMAP    oldBmp;
    double     cx, cy, scale;
    int        maxIter;
} DdState;

static WNDPROC g_origDdFrame = NULL;

static DWORD Dd_Color(int iter, int maxIter)
{
    if (iter >= maxIter) return 0x00000000;
    /* Smooth gradient: hue cycles, brightness modulated */
    double t = (double)iter / (double)maxIter;
    double r = 9.0 * (1 - t) * t * t * t * 255.0;
    double g = 15.0 * (1 - t) * (1 - t) * t * t * 255.0;
    double b = 8.5 * (1 - t) * (1 - t) * (1 - t) * t * 255.0;
    int R = r < 0 ? 0 : (r > 255 ? 255 : (int)r);
    int G = g < 0 ? 0 : (g > 255 ? 255 : (int)g);
    int B = b < 0 ? 0 : (b > 255 ? 255 : (int)b);
    return ((DWORD)R << 16) | ((DWORD)G << 8) | (DWORD)B;
}

static void Dd_Render(DdState *st)
{
    int x, y;
    if (!st->bits) return;
    for (y = 0; y < st->h; ++y) {
        DWORD *row = &st->bits[y * st->w];
        double ci = st->cy + (y - st->h / 2) * st->scale;
        for (x = 0; x < st->w; ++x) {
            double cr = st->cx + (x - st->w / 2) * st->scale;
            double zr = 0.0, zi = 0.0;
            int iter = 0;
            while (iter < st->maxIter && zr * zr + zi * zi < 4.0) {
                double zr2 = zr * zr - zi * zi + cr;
                zi = 2.0 * zr * zi + ci;
                zr = zr2;
                ++iter;
            }
            row[x] = Dd_Color(iter, st->maxIter);
        }
    }
}

static void Dd_Realloc(HWND hwnd, DdState *st, int w, int h)
{
    BITMAPINFO bi;
    HDC hdc;

    if (st->memDC) {
        SelectObject(st->memDC, st->oldBmp);
        DeleteDC(st->memDC);
        st->memDC = NULL;
    }
    if (st->bmp) { DeleteObject(st->bmp); st->bmp = NULL; }
    st->bits = NULL;

    if (w < 1 || h < 1) return;

    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;      /* negative = top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    hdc = GetDC(hwnd);
    st->bmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS,
                                (void **)&st->bits, NULL, 0);
    ReleaseDC(hwnd, hdc);
    if (!st->bmp) return;

    st->memDC = CreateCompatibleDC(NULL);
    st->oldBmp = (HBITMAP)SelectObject(st->memDC, st->bmp);
    st->w = w;
    st->h = h;
}

static void Dd_Paint(HWND hwnd, DdState *st)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    if (st->memDC && st->bmp) {
        BitBlt(hdc, 0, 30, st->w, st->h, st->memDC, 0, 0, SRCCOPY);
    }
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK Dd_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    DdState *st = (DdState *)GetPropW(hwnd, DD_PROP);

    if (msg == WM_PAINT && st) { Dd_Paint(hwnd, st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp) - 30;
        if (h < 1) h = 1;
        Dd_Realloc(hwnd, st, w, h);
        Dd_Render(st);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    if (msg == WM_LBUTTONDOWN && st) {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp) - 30;
        if (y >= 0 && y < st->h) {
            st->cx += (x - st->w / 2) * st->scale;
            st->cy += (y - st->h / 2) * st->scale;
            st->scale *= 0.5;
            Dd_Render(st);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    if (msg == WM_RBUTTONDOWN && st) {
        st->cx = -0.5; st->cy = 0.0; st->scale = 0.005;
        Dd_Render(st);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    if (msg == WM_DESTROY && st) {
        if (st->memDC) { SelectObject(st->memDC, st->oldBmp); DeleteDC(st->memDC); }
        if (st->bmp) DeleteObject(st->bmp);
        free(st); RemovePropW(hwnd, DD_PROP);
    }
    return CallWindowProcW(g_origDdFrame, hwnd, msg, wp, lp);
}

static HWND DibDraw_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    DdState *st;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"DibDraw",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (DdState *)calloc(1, sizeof(DdState));
    if (!st) { DestroyWindow(frame); return NULL; }
    st->cx = -0.5; st->cy = 0.0; st->scale = 0.005;
    st->maxIter = 96;

    SetPropW(frame, DD_PROP, (HANDLE)st);
    if (!g_origDdFrame) g_origDdFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Dd_FrameProc);
    return frame;
}

MsApp g_AppDibDraw = { L"DibDraw", DibDraw_Create, 640, 510 };
