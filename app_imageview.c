/*
 * app_imageview.c — Image viewer
 *
 * Demonstrates the Win32 common file dialog (GetOpenFileNameW) and image
 * loading via LoadImageW. Supports .bmp natively. Click "Open..." button to
 * load; the bitmap is stretched to fit the client area.
 */

#include "shell.h"
#include <commdlg.h>
#include <stdlib.h>

#pragma comment(lib, "comdlg32.lib")

#define IV_PROP    L"MS_IV_STATE"
#define IV_ID_OPEN 5101

typedef struct {
    HBITMAP   bmp;
    int       bmpW, bmpH;
    HWND      openBtn;
    HWND      canvas;
} IvState;

#define IV_CANVAS_CLASS L"MiniShell_IvCanvas"

static LRESULT CALLBACK Iv_CanvasProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc, memDC;
        IvState *st;
        RECT rc;

        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, (HBRUSH)GetStockObject(GRAY_BRUSH));

        st = (IvState *)GetPropW(GetParent(hwnd), IV_PROP);
        if (st && st->bmp) {
            HGDIOBJ oldBmp;
            double sx, sy, scale;
            int dw, dh, dx, dy;

            memDC = CreateCompatibleDC(hdc);
            oldBmp = SelectObject(memDC, st->bmp);

            sx = (double)rc.right / st->bmpW;
            sy = (double)rc.bottom / st->bmpH;
            scale = sx < sy ? sx : sy;
            dw = (int)(st->bmpW * scale);
            dh = (int)(st->bmpH * scale);
            dx = (rc.right - dw) / 2;
            dy = (rc.bottom - dh) / 2;

            SetStretchBltMode(hdc, HALFTONE);
            StretchBlt(hdc, dx, dy, dw, dh,
                       memDC, 0, 0, st->bmpW, st->bmpH, SRCCOPY);

            SelectObject(memDC, oldBmp);
            DeleteDC(memDC);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void EnsureIvClass(HINSTANCE hInstance)
{
    static BOOL registered = FALSE;
    WNDCLASSEXW wc;
    if (registered) return;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = Iv_CanvasProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = IV_CANVAS_CLASS;
    RegisterClassExW(&wc);
    registered = TRUE;
}

static void Iv_OpenDialog(HWND frame, IvState *st)
{
    OPENFILENAMEW ofn;
    wchar_t file[MAX_PATH] = L"";
    HBITMAP newBmp;
    BITMAP   info;

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = frame;
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrFilter = L"Bitmap files (*.bmp)\0*.bmp\0All files\0*.*\0";
    ofn.lpstrTitle  = L"Open image";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;

    if (!GetOpenFileNameW(&ofn)) return;

    newBmp = (HBITMAP)LoadImageW(NULL, file, IMAGE_BITMAP, 0, 0,
                                 LR_LOADFROMFILE | LR_CREATEDIBSECTION);
    if (!newBmp) {
        MessageBoxW(frame, L"Failed to load image.", L"Image Viewer", MB_ICONWARNING);
        return;
    }
    if (st->bmp) DeleteObject(st->bmp);
    st->bmp = newBmp;
    GetObjectW(newBmp, sizeof(info), &info);
    st->bmpW = info.bmWidth;
    st->bmpH = info.bmHeight;
    InvalidateRect(st->canvas, NULL, TRUE);
}

static WNDPROC g_origIvFrame = NULL;

static LRESULT CALLBACK Iv_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    IvState *st = (IvState *)GetPropW(hwnd, IV_PROP);

    if (msg == WM_COMMAND && LOWORD(wp) == IV_ID_OPEN && st) {
        Iv_OpenDialog(hwnd, st);
        return 0;
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->openBtn, 8, 34, 80, 24, TRUE);
        MoveWindow(st->canvas,  8, 64, w - 16, h - 72, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->bmp) DeleteObject(st->bmp);
        free(st);
        RemovePropW(hwnd, IV_PROP);
    }
    return CallWindowProcW(g_origIvFrame, hwnd, msg, wp, lp);
}

static HWND ImageView_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    IvState *st;
    (void)self;

    EnsureIvClass(hInstance);
    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"ImageView",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (IvState *)calloc(1, sizeof(IvState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->openBtn = CreateWindowExW(0, L"BUTTON", L"Open...",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        8, 34, 80, 24, frame, (HMENU)(LONG_PTR)IV_ID_OPEN, hInstance, NULL);

    st->canvas = CreateWindowExW(0, IV_CANVAS_CLASS, L"",
        WS_CHILD | WS_VISIBLE,
        8, 64, w - 16, h - 72, frame, NULL, hInstance, NULL);

    SetPropW(frame, IV_PROP, (HANDLE)st);
    if (!g_origIvFrame)
        g_origIvFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Iv_FrameProc);
    return frame;
}

MsApp g_AppImageView = {
    L"ImageView",
    ImageView_Create,
    520, 420
};
