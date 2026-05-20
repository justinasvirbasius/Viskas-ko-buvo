/*
 * app_pngview.c — Modern image viewer (PNG/JPEG/GIF/BMP) via WIC
 *
 * Demonstrates:
 *   - COM initialization with CoInitializeEx
 *   - Windows Imaging Component used from plain C (note CINTERFACE)
 *   - IWICImagingFactory::CreateDecoderFromFilename
 *   - IWICBitmapFrameDecode + IWICFormatConverter to BGRA32
 *   - Creating a DIB from the pixel buffer and StretchBlt'ing it
 *
 * Linked against windowscodecs.lib and ole32.lib. The CINTERFACE define
 * gives the C-callable vtbl form (lpVtbl->Method).
 */

#define COBJMACROS
#define CINTERFACE

#include "shell.h"
#include <objbase.h>
#include <wincodec.h>
#include <commdlg.h>
#include <stdlib.h>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comdlg32.lib")

#define PV_PROP    L"MS_PV_STATE"
#define ID_PV_OPEN 16001
#define PV_CLASS   L"MiniShell_PvCanvas"

typedef struct {
    HBITMAP bmp;
    int     w, h;
    HWND    openBtn;
    HWND    canvas;
    BOOL    comOk;
} PvState;

static BOOL Pv_LoadWIC(const wchar_t *path, HBITMAP *outBmp, int *outW, int *outH)
{
    IWICImagingFactory   *factory   = NULL;
    IWICBitmapDecoder    *decoder   = NULL;
    IWICBitmapFrameDecode *frame    = NULL;
    IWICFormatConverter  *converter = NULL;
    HRESULT hr;
    UINT w = 0, h = 0;
    BITMAPINFO bmi;
    void *pixels = NULL;
    HBITMAP dib = NULL;
    HDC hdc;
    BOOL ok = FALSE;

    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWICImagingFactory, (void **)&factory);
    if (FAILED(hr)) return FALSE;

    hr = IWICImagingFactory_CreateDecoderFromFilename(
            factory, path, NULL, GENERIC_READ,
            WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) goto cleanup;

    hr = IWICBitmapDecoder_GetFrame(decoder, 0, &frame);
    if (FAILED(hr)) goto cleanup;

    hr = IWICImagingFactory_CreateFormatConverter(factory, &converter);
    if (FAILED(hr)) goto cleanup;

    hr = IWICFormatConverter_Initialize(
            converter, (IWICBitmapSource *)frame,
            &GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, NULL, 0.0,
            WICBitmapPaletteTypeMedianCut);
    if (FAILED(hr)) goto cleanup;

    hr = IWICFormatConverter_GetSize(converter, &w, &h);
    if (FAILED(hr) || w == 0 || h == 0) goto cleanup;

    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = (LONG)w;
    bmi.bmiHeader.biHeight      = -(LONG)h;   /* top-down */
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    hdc = GetDC(NULL);
    dib = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &pixels, NULL, 0);
    ReleaseDC(NULL, hdc);
    if (!dib || !pixels) goto cleanup;

    hr = IWICFormatConverter_CopyPixels(converter, NULL, w * 4, w * h * 4,
                                        (BYTE *)pixels);
    if (FAILED(hr)) {
        DeleteObject(dib);
        goto cleanup;
    }
    *outBmp = dib; *outW = (int)w; *outH = (int)h;
    ok = TRUE;

cleanup:
    if (converter) IWICFormatConverter_Release(converter);
    if (frame)     IWICBitmapFrameDecode_Release(frame);
    if (decoder)   IWICBitmapDecoder_Release(decoder);
    if (factory)   IWICImagingFactory_Release(factory);
    return ok;
}

static LRESULT CALLBACK Pv_CanvasProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc;
        PvState *st;
        RECT rc;

        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));

        st = (PvState *)GetPropW(GetParent(hwnd), PV_PROP);
        if (st && st->bmp) {
            HDC memDC = CreateCompatibleDC(hdc);
            HGDIOBJ old = SelectObject(memDC, st->bmp);
            double sx = (double)rc.right / st->w;
            double sy = (double)rc.bottom / st->h;
            double scale = sx < sy ? sx : sy;
            int dw = (int)(st->w * scale);
            int dh = (int)(st->h * scale);
            int dx = (rc.right - dw) / 2;
            int dy = (rc.bottom - dh) / 2;
            SetStretchBltMode(hdc, HALFTONE);
            StretchBlt(hdc, dx, dy, dw, dh,
                       memDC, 0, 0, st->w, st->h, SRCCOPY);
            SelectObject(memDC, old);
            DeleteDC(memDC);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void EnsurePvClass(HINSTANCE hInstance)
{
    static BOOL registered = FALSE;
    WNDCLASSEXW wc;
    if (registered) return;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = Pv_CanvasProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = PV_CLASS;
    RegisterClassExW(&wc);
    registered = TRUE;
}

static WNDPROC g_origPvFrame = NULL;

static LRESULT CALLBACK Pv_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    PvState *st = (PvState *)GetPropW(hwnd, PV_PROP);

    if (msg == WM_COMMAND && LOWORD(wp) == ID_PV_OPEN && st) {
        OPENFILENAMEW ofn;
        wchar_t file[MAX_PATH] = L"";
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner   = hwnd;
        ofn.lpstrFile   = file;
        ofn.nMaxFile    = MAX_PATH;
        ofn.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.tif;*.tiff\0All files\0*.*\0";
        ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
        if (GetOpenFileNameW(&ofn)) {
            HBITMAP bmp = NULL;
            int bw = 0, bh = 0;
            if (Pv_LoadWIC(file, &bmp, &bw, &bh)) {
                if (st->bmp) DeleteObject(st->bmp);
                st->bmp = bmp; st->w = bw; st->h = bh;
                InvalidateRect(st->canvas, NULL, TRUE);
            } else {
                MessageBoxW(hwnd, L"Failed to decode image via WIC.",
                            L"PngView", MB_ICONWARNING);
            }
        }
        return 0;
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->openBtn, 8, 34, 80, 24, TRUE);
        MoveWindow(st->canvas,  8, 64, w - 16, h - 72, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->bmp) DeleteObject(st->bmp);
        if (st->comOk) CoUninitialize();
        free(st);
        RemovePropW(hwnd, PV_PROP);
    }
    return CallWindowProcW(g_origPvFrame, hwnd, msg, wp, lp);
}

static HWND PngView_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    PvState *st;
    HRESULT hr;
    (void)self;

    EnsurePvClass(hInstance);
    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"PngView",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (PvState *)calloc(1, sizeof(PvState));
    if (!st) { DestroyWindow(frame); return NULL; }

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    st->comOk = SUCCEEDED(hr);

    st->openBtn = CreateWindowExW(0, L"BUTTON", L"Open...",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        8, 34, 80, 24, frame, (HMENU)(LONG_PTR)ID_PV_OPEN, hInstance, NULL);

    st->canvas = CreateWindowExW(0, PV_CLASS, L"",
        WS_CHILD | WS_VISIBLE,
        8, 64, w - 16, h - 72, frame, NULL, hInstance, NULL);

    SetPropW(frame, PV_PROP, (HANDLE)st);
    if (!g_origPvFrame)
        g_origPvFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Pv_FrameProc);
    return frame;
}

MsApp g_AppPngView = {
    L"PngView",
    PngView_Create,
    560, 460
};
