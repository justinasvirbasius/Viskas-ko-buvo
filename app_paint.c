/*
 * app_paint.c — Tiny paint program
 *
 * A canvas child window backed by a DIB-section bitmap so strokes persist
 * across WM_PAINT. Left-drag draws with the current color; right-click clears.
 * A row of color swatches sits along the top of the canvas.
 */

#include "shell.h"
#include <stdlib.h>

#define PAINT_CANVAS_CLASS L"MiniShell_PaintCanvas"
#define SWATCH_HEIGHT 24
#define SWATCH_SIZE   20

typedef struct {
    HBITMAP    bmp;
    HDC        memDC;
    HBITMAP    oldBmp;
    int        w, h;
    BOOL       drawing;
    POINT      last;
    COLORREF   color;
    HPEN       pen;
} PaintState;

static const COLORREF kSwatches[] = {
    RGB(0,0,0), RGB(255,255,255), RGB(220,40,40), RGB(40,160,40),
    RGB(40,80,200), RGB(240,200,40), RGB(180,80,200), RGB(40,180,200)
};
#define SWATCH_COUNT (sizeof(kSwatches)/sizeof(kSwatches[0]))

static void Paint_EnsureBitmap(HWND hwnd, PaintState *st)
{
    RECT rc;
    HDC  screen;

    GetClientRect(hwnd, &rc);
    if (st->bmp && st->w == rc.right && st->h == rc.bottom) return;

    if (st->memDC) {
        SelectObject(st->memDC, st->oldBmp);
        DeleteDC(st->memDC);
        DeleteObject(st->bmp);
    }

    screen = GetDC(hwnd);
    st->memDC = CreateCompatibleDC(screen);
    st->bmp   = CreateCompatibleBitmap(screen, rc.right, rc.bottom);
    st->oldBmp = (HBITMAP)SelectObject(st->memDC, st->bmp);
    st->w = rc.right;
    st->h = rc.bottom;

    /* White background */
    {
        HBRUSH white = (HBRUSH)GetStockObject(WHITE_BRUSH);
        FillRect(st->memDC, &rc, white);
    }
    ReleaseDC(hwnd, screen);
}

static LRESULT CALLBACK Paint_CanvasProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    PaintState *st = (PaintState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        st = (PaintState *)calloc(1, sizeof(PaintState));
        if (!st) return -1;
        st->color = RGB(0, 0, 0);
        st->pen = CreatePen(PS_SOLID, 3, st->color);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        return 0;
    }

    case WM_SIZE:
        Paint_EnsureBitmap(hwnd, st);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        int i;
        Paint_EnsureBitmap(hwnd, st);

        /* Blit the persistent canvas */
        BitBlt(hdc, 0, SWATCH_HEIGHT, st->w, st->h - SWATCH_HEIGHT,
               st->memDC, 0, SWATCH_HEIGHT, SRCCOPY);

        /* Swatches across the top */
        for (i = 0; i < (int)SWATCH_COUNT; ++i) {
            RECT r;
            HBRUSH br;
            r.left = 4 + i * (SWATCH_SIZE + 4);
            r.top  = 2;
            r.right = r.left + SWATCH_SIZE;
            r.bottom = r.top + SWATCH_SIZE;
            br = CreateSolidBrush(kSwatches[i]);
            FillRect(hdc, &r, br);
            DeleteObject(br);
            if (kSwatches[i] == st->color) {
                FrameRect(hdc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
                InflateRect(&r, -1, -1);
                FrameRect(hdc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
            }
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int x = LOWORD(lp), y = HIWORD(lp);
        if (y < SWATCH_HEIGHT) {
            int idx = (x - 4) / (SWATCH_SIZE + 4);
            if (idx >= 0 && idx < (int)SWATCH_COUNT) {
                st->color = kSwatches[idx];
                DeleteObject(st->pen);
                st->pen = CreatePen(PS_SOLID, 3, st->color);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        st->drawing = TRUE;
        st->last.x = x;
        st->last.y = y;
        SetCapture(hwnd);
        return 0;
    }

    case WM_MOUSEMOVE:
        if (st->drawing) {
            int x = LOWORD(lp), y = HIWORD(lp);
            HGDIOBJ oldPen = SelectObject(st->memDC, st->pen);
            MoveToEx(st->memDC, st->last.x, st->last.y, NULL);
            LineTo(st->memDC, x, y);
            SelectObject(st->memDC, oldPen);
            st->last.x = x;
            st->last.y = y;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_LBUTTONUP:
        if (st->drawing) {
            st->drawing = FALSE;
            ReleaseCapture();
        }
        return 0;

    case WM_RBUTTONDOWN: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(st->memDC, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_DESTROY:
        if (st) {
            if (st->memDC) {
                SelectObject(st->memDC, st->oldBmp);
                DeleteDC(st->memDC);
                DeleteObject(st->bmp);
            }
            if (st->pen) DeleteObject(st->pen);
            free(st);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void EnsurePaintClass(HINSTANCE hInstance)
{
    static BOOL registered = FALSE;
    WNDCLASSEXW wc;
    if (registered) return;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = Paint_CanvasProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_CROSS);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = PAINT_CANVAS_CLASS;
    RegisterClassExW(&wc);
    registered = TRUE;
}

/* Frame subclass to keep the canvas sized to the client area */
static WNDPROC g_origPaintFrame = NULL;

static LRESULT CALLBACK Paint_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_SIZE) {
        HWND canvas = FindWindowExW(hwnd, NULL, PAINT_CANVAS_CLASS, NULL);
        if (canvas) {
            int w = LOWORD(lp), h = HIWORD(lp);
            MoveWindow(canvas, 4, 32, w - 8, h - 36, TRUE);
        }
    }
    return CallWindowProcW(g_origPaintFrame, hwnd, msg, wp, lp);
}

static HWND Paint_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    (void)self;

    EnsurePaintClass(hInstance);
    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Paint",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    CreateWindowExW(0, PAINT_CANVAS_CLASS, L"",
        WS_CHILD | WS_VISIBLE,
        4, 32, w - 8, h - 36, frame, NULL, hInstance, NULL);

    if (!g_origPaintFrame)
        g_origPaintFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Paint_FrameProc);
    return frame;
}

MsApp g_AppPaint = {
    L"Paint",
    Paint_Create,
    520, 420
};
