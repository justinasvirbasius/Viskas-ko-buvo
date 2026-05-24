/*
 * app_magnify.c — Screen magnifier via Magnification API
 *
 * Demonstrates the Windows Magnification API (magnification.dll) — the
 * accessibility framework behind the built-in Magnifier and screen-reader
 * zoom features:
 *
 *   - MagInitialize() initializes the lib; required before any Mag* call
 *   - The magnifier needs a window of class WC_MAGNIFIER hosted INSIDE a
 *     WS_EX_LAYERED top-level "host" window. The magnifier child fills
 *     the host's client area and shows a zoomed view of whatever screen
 *     region the app aims it at.
 *   - MagSetWindowTransform(hMag, &MAGTRANSFORM) installs the 3x3 matrix
 *     specifying zoom level (e.g. 2x identity)
 *   - MagSetWindowSource(hMag, srcRect) tells the magnifier which region
 *     of the desktop to mirror in
 *   - A timer periodically updates the source rect to follow the mouse
 *
 * We embed the magnifier in our existing app frame so the host-window
 * pattern is visible in one self-contained file.
 */

#include "shell.h"
#include <stdlib.h>
#include <stdio.h>

/* Magnification.h definitions; provide minimal forward decls to avoid
   header-availability fragility. */
typedef struct {
    float v[3][3];
} MS_MAGTRANSFORM;

typedef BOOL (WINAPI *PFN_MagInitialize)(void);
typedef BOOL (WINAPI *PFN_MagUninitialize)(void);
typedef BOOL (WINAPI *PFN_MagSetWindowSource)(HWND, RECT);
typedef BOOL (WINAPI *PFN_MagSetWindowTransform)(HWND, MS_MAGTRANSFORM *);
typedef BOOL (WINAPI *PFN_MagSetWindowFilterList)(HWND, DWORD, int, HWND *);

#define MAG_PROP   L"MS_MAG_STATE"
#define WC_MAG_W   L"Magnifier"
#define MAG_TIMER  1

typedef struct {
    HMODULE  magDll;
    PFN_MagInitialize        pInit;
    PFN_MagUninitialize      pUninit;
    PFN_MagSetWindowSource   pSetSrc;
    PFN_MagSetWindowTransform pSetTransform;
    HWND     mag;
    UINT_PTR timer;
    BOOL     initialized;
} MagState;

static WNDPROC g_origMagFrame = NULL;

static void Mag_Update(HWND frame, MagState *st)
{
    POINT pt;
    RECT  src;
    int   srcW = 200, srcH = 150;
    if (!st->pSetSrc || !st->mag) return;
    GetCursorPos(&pt);
    src.left   = pt.x - srcW / 2;
    src.top    = pt.y - srcH / 2;
    src.right  = src.left + srcW;
    src.bottom = src.top + srcH;
    st->pSetSrc(st->mag, src);
}

static LRESULT CALLBACK Mag_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    MagState *st = (MagState *)GetPropW(hwnd, MAG_PROP);
    if (msg == WM_TIMER && st) {
        Mag_Update(hwnd, st);
        return 0;
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        if (st->mag) MoveWindow(st->mag, 8, 40, w - 16, h - 48, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->timer) KillTimer(hwnd, st->timer);
        if (st->initialized && st->pUninit) st->pUninit();
        if (st->magDll) FreeLibrary(st->magDll);
        free(st); RemovePropW(hwnd, MAG_PROP);
    }
    return CallWindowProcW(g_origMagFrame, hwnd, msg, wp, lp);
}

static HWND Magnify_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    MagState *st;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED, MS_CLASS_APPFRAME, L"Magnify",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    SetLayeredWindowAttributes(frame, 0, 255, LWA_ALPHA);

    st = (MagState *)calloc(1, sizeof(MagState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->magDll = LoadLibraryW(L"Magnification.dll");
    if (st->magDll) {
        st->pInit         = (PFN_MagInitialize)        GetProcAddress(st->magDll, "MagInitialize");
        st->pUninit       = (PFN_MagUninitialize)      GetProcAddress(st->magDll, "MagUninitialize");
        st->pSetSrc       = (PFN_MagSetWindowSource)   GetProcAddress(st->magDll, "MagSetWindowSource");
        st->pSetTransform = (PFN_MagSetWindowTransform)GetProcAddress(st->magDll, "MagSetWindowTransform");
    }

    if (st->pInit && st->pInit()) {
        MS_MAGTRANSFORM t;
        st->initialized = TRUE;

        st->mag = CreateWindowExW(0, WC_MAG_W, L"",
                                  WS_CHILD | WS_VISIBLE,
                                  8, 40, w - 16, h - 48,
                                  frame, NULL, hInstance, NULL);
        if (st->mag && st->pSetTransform) {
            ZeroMemory(&t, sizeof(t));
            t.v[0][0] = 2.0f;  /* X zoom */
            t.v[1][1] = 2.0f;  /* Y zoom */
            t.v[2][2] = 1.0f;
            st->pSetTransform(st->mag, &t);
        }
        st->timer = SetTimer(frame, MAG_TIMER, 40, NULL);
    }

    SetPropW(frame, MAG_PROP, (HANDLE)st);
    if (!g_origMagFrame) g_origMagFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Mag_FrameProc);
    return frame;
}

MsApp g_AppMagnify = { L"Magnify", Magnify_Create, 500, 380 };
