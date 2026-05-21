/*
 * app_dibclip.c — Bitmap clipboard: copy and paste CF_DIB
 *
 * Demonstrates the bitmap clipboard format (CF_DIB), which complements
 * app_clipboard.c which only handles CF_TEXT/CF_UNICODETEXT:
 *   - GetDIBits / CreateCompatibleBitmap to materialize a drawn canvas
 *   - GlobalAlloc + memcpy + SetClipboardData(CF_DIB, h)
 *   - GetClipboardData(CF_DIB) → BITMAPINFOHEADER + pixel bytes
 *   - StretchDIBits to paint the pasted DIB into the canvas
 *
 * The window has a small drawing canvas (click-drag with mouse to scribble),
 * a Copy button (CF_DIB to clipboard), and a Paste button (CF_DIB into the
 * canvas).
 */

#include "shell.h"
#include <stdlib.h>
#include <stdio.h>

#define DC_PROP    L"MS_DC_STATE"
#define ID_DC_COPY 52001
#define ID_DC_PASTE 52002
#define ID_DC_CLR  52003
#define ID_DC_STAT 52004

#define CANVAS_W 320
#define CANVAS_H 200

typedef struct {
    HBITMAP  canvas;
    HBITMAP  oldBmp;
    HDC      memDC;
    HWND     status;
    BOOL     dragging;
    POINT    last;
} DcState;

static WNDPROC g_origDcFrame = NULL;

static void Dc_InitCanvas(HWND hwnd, DcState *st)
{
    HDC hdc = GetDC(hwnd);
    RECT rc = { 0, 0, CANVAS_W, CANVAS_H };
    HBRUSH white;

    st->memDC  = CreateCompatibleDC(hdc);
    st->canvas = CreateCompatibleBitmap(hdc, CANVAS_W, CANVAS_H);
    st->oldBmp = (HBITMAP)SelectObject(st->memDC, st->canvas);

    white = (HBRUSH)GetStockObject(WHITE_BRUSH);
    FillRect(st->memDC, &rc, white);

    ReleaseDC(hwnd, hdc);
}

static void Dc_PaintCanvas(HWND hwnd, DcState *st)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    HBRUSH border = CreateSolidBrush(RGB(100, 100, 100));
    RECT rc;
    GetClientRect(hwnd, &rc);

    /* Clear surround */
    {
        HBRUSH bg = (HBRUSH)(COLOR_BTNFACE + 1);
        FillRect(hdc, &rc, bg);
    }
    /* Frame */
    {
        RECT b = { 7, 75, 9 + CANVAS_W, 77 + CANVAS_H };
        FrameRect(hdc, &b, border);
    }
    BitBlt(hdc, 8, 76, CANVAS_W, CANVAS_H, st->memDC, 0, 0, SRCCOPY);
    DeleteObject(border);
    EndPaint(hwnd, &ps);
}

static BOOL Dc_PointInCanvas(int x, int y, POINT *out)
{
    if (x >= 8 && x < 8 + CANVAS_W && y >= 76 && y < 76 + CANVAS_H) {
        out->x = x - 8;
        out->y = y - 76;
        return TRUE;
    }
    return FALSE;
}

static void Dc_Copy(HWND hwnd, DcState *st)
{
    BITMAPINFO bi;
    DWORD imgSize;
    HGLOBAL hMem;
    BYTE *p;
    BITMAPINFOHEADER *bih;
    BYTE *pixels;
    HDC hdc;

    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = CANVAS_W;
    bi.bmiHeader.biHeight      = CANVAS_H;        /* positive = bottom-up */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    imgSize = CANVAS_W * CANVAS_H * 4;

    hMem = GlobalAlloc(GHND, sizeof(BITMAPINFOHEADER) + imgSize);
    if (!hMem) { SetWindowTextW(st->status, L"GlobalAlloc failed."); return; }
    p = (BYTE *)GlobalLock(hMem);
    bih = (BITMAPINFOHEADER *)p;
    *bih = bi.bmiHeader;
    bih->biSizeImage = imgSize;
    pixels = p + sizeof(BITMAPINFOHEADER);

    hdc = GetDC(hwnd);
    /* Pull pixels from our memDC */
    GetDIBits(st->memDC, st->canvas, 0, CANVAS_H, pixels, &bi, DIB_RGB_COLORS);
    ReleaseDC(hwnd, hdc);
    GlobalUnlock(hMem);

    if (!OpenClipboard(hwnd)) {
        GlobalFree(hMem);
        SetWindowTextW(st->status, L"OpenClipboard failed.");
        return;
    }
    EmptyClipboard();
    if (SetClipboardData(CF_DIB, hMem)) {
        SetWindowTextW(st->status, L"Canvas copied as CF_DIB.");
    } else {
        GlobalFree(hMem);
        SetWindowTextW(st->status, L"SetClipboardData failed.");
    }
    CloseClipboard();
}

static void Dc_Paste(HWND hwnd, DcState *st)
{
    HGLOBAL h;
    BYTE *p;
    BITMAPINFOHEADER *bih;
    BYTE *pixels;
    int srcW, srcH;

    if (!IsClipboardFormatAvailable(CF_DIB)) {
        SetWindowTextW(st->status, L"No CF_DIB on the clipboard.");
        return;
    }
    if (!OpenClipboard(hwnd)) return;
    h = GetClipboardData(CF_DIB);
    if (!h) { CloseClipboard(); SetWindowTextW(st->status, L"GetClipboardData failed."); return; }

    p = (BYTE *)GlobalLock(h);
    bih = (BITMAPINFOHEADER *)p;

    srcW = bih->biWidth;
    srcH = bih->biHeight < 0 ? -bih->biHeight : bih->biHeight;

    /* Color table for paletted DIBs follows the BITMAPINFOHEADER.
       For 16/24/32-bit RGB sources biClrUsed is typically 0. */
    {
        DWORD ctEntries = bih->biClrUsed;
        DWORD ctBytes;
        if (ctEntries == 0) {
            if (bih->biBitCount == 1)       ctEntries = 2;
            else if (bih->biBitCount == 4)  ctEntries = 16;
            else if (bih->biBitCount == 8)  ctEntries = 256;
            else                            ctEntries = 0;
        }
        ctBytes = ctEntries * sizeof(RGBQUAD);
        if (bih->biCompression == BI_BITFIELDS) ctBytes += 12;
        pixels = p + bih->biSize + ctBytes;
    }

    /* Blit into memDC, fitting within the canvas */
    StretchDIBits(st->memDC,
        0, 0, CANVAS_W, CANVAS_H,
        0, 0, srcW, srcH,
        pixels, (BITMAPINFO *)bih, DIB_RGB_COLORS, SRCCOPY);

    GlobalUnlock(h);
    CloseClipboard();

    {
        wchar_t buf[80];
        swprintf_s(buf, 80, L"Pasted %dx%d DIB (%d bpp).",
                   srcW, srcH, bih->biBitCount);
        SetWindowTextW(st->status, buf);
    }
    InvalidateRect(hwnd, NULL, FALSE);
}

static void Dc_Clear(HWND hwnd, DcState *st)
{
    RECT rc = { 0, 0, CANVAS_W, CANVAS_H };
    HBRUSH white = (HBRUSH)GetStockObject(WHITE_BRUSH);
    FillRect(st->memDC, &rc, white);
    InvalidateRect(hwnd, NULL, FALSE);
    SetWindowTextW(st->status, L"Canvas cleared.");
}

static LRESULT CALLBACK Dc_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    DcState *st = (DcState *)GetPropW(hwnd, DC_PROP);

    if (msg == WM_COMMAND && st) {
        switch (LOWORD(wp)) {
        case ID_DC_COPY:  Dc_Copy(hwnd, st);  return 0;
        case ID_DC_PASTE: Dc_Paste(hwnd, st); return 0;
        case ID_DC_CLR:   Dc_Clear(hwnd, st); return 0;
        }
    }
    if (msg == WM_PAINT && st) { Dc_PaintCanvas(hwnd, st); return 0; }
    if (msg == WM_LBUTTONDOWN && st) {
        POINT p;
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (Dc_PointInCanvas(x, y, &p)) {
            st->dragging = TRUE;
            st->last = p;
            SetCapture(hwnd);
        }
        return 0;
    }
    if (msg == WM_MOUSEMOVE && st && st->dragging) {
        POINT p;
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (Dc_PointInCanvas(x, y, &p)) {
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(40, 60, 120));
            HPEN old = (HPEN)SelectObject(st->memDC, pen);
            MoveToEx(st->memDC, st->last.x, st->last.y, NULL);
            LineTo(st->memDC, p.x, p.y);
            SelectObject(st->memDC, old);
            DeleteObject(pen);
            st->last = p;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    if (msg == WM_LBUTTONUP && st) {
        if (st->dragging) { st->dragging = FALSE; ReleaseCapture(); }
        return 0;
    }
    if (msg == WM_DESTROY && st) {
        if (st->memDC) {
            SelectObject(st->memDC, st->oldBmp);
            DeleteDC(st->memDC);
        }
        if (st->canvas) DeleteObject(st->canvas);
        free(st);
        RemovePropW(hwnd, DC_PROP);
    }
    return CallWindowProcW(g_origDcFrame, hwnd, msg, wp, lp);
}

static HWND DibClip_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    DcState *st;
    (void)self; (void)w; (void)h;

    /* Pin window to fit canvas */
    w = CANVAS_W + 24;
    h = CANVAS_H + 120;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"DibClip",
        WS_POPUP | WS_BORDER,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (DcState *)calloc(1, sizeof(DcState));
    if (!st) { DestroyWindow(frame); return NULL; }

    CreateWindowExW(0, L"BUTTON", L"Copy",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        8, 38, 70, 26, frame, (HMENU)(LONG_PTR)ID_DC_COPY, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Paste",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        86, 38, 70, 26, frame, (HMENU)(LONG_PTR)ID_DC_PASTE, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Clear",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        164, 38, 70, 26, frame, (HMENU)(LONG_PTR)ID_DC_CLR, hInstance, NULL);

    st->status = CreateWindowExW(0, L"STATIC",
        L"Drag the mouse over the canvas to draw; then Copy/Paste CF_DIB.",
        WS_CHILD | WS_VISIBLE,
        8, h - 28, w - 16, 22, frame, (HMENU)(LONG_PTR)ID_DC_STAT, hInstance, NULL);

    SetPropW(frame, DC_PROP, (HANDLE)st);
    Dc_InitCanvas(frame, st);
    if (!g_origDcFrame) g_origDcFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Dc_FrameProc);
    return frame;
}

MsApp g_AppDibClip = {
    L"DibClip",
    DibClip_Create,
    344, 320
};
