/*
 * app_perpixalpha.c — Per-pixel alpha layered window
 *
 * Demonstrates the *other* layered-window mode: per-pixel alpha via
 * UpdateLayeredWindow. This differs from the WS_EX_LAYERED + SetLayeredWindowAttributes
 * (constant alpha) approach in app_layered.c:
 *
 *   - Window created with WS_EX_LAYERED and NO WS_BORDER / caption
 *   - A 32-bit top-down DIB section holds premultiplied BGRA pixels
 *   - UpdateLayeredWindow(hwnd, NULL, &dstPt, &size, srcDC, &srcPt, 0,
 *                          &blend, ULW_ALPHA) drives BOTH position and
 *     content in a single atomic update — there is no WM_PAINT in this mode
 *
 * Pixels are PREMULTIPLIED: a 50%-transparent red pixel is (B=0, G=0, R=128,
 * A=128), not (0, 0, 255, 128). Failing to premultiply produces fringing.
 *
 * The app renders a soft glowing orb with a fade-to-transparent edge so the
 * transparency is obvious against whatever's behind it.
 */

#include "shell.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>

#define PP_PROP    L"MS_PP_STATE"
#define ID_PP_GO   75001
#define ID_PP_HIDE 75002
#define ID_PP_STAT 75003

#define ORB_W 220
#define ORB_H 220

typedef struct {
    HWND     frame, orb, status;
    BOOL     visible;
    UINT_PTR timerId;
    DWORD    startTick;
} PpState;

static WNDPROC g_origPpFrame = NULL;

static void Pp_RenderOrb(HWND orb, float t)
{
    HDC      screen = GetDC(NULL);
    HDC      mem    = CreateCompatibleDC(screen);
    BITMAPINFO bi;
    HBITMAP  bmp;
    DWORD   *pixels;
    HBITMAP  oldBmp;
    POINT    srcPt = { 0, 0 };
    SIZE     size  = { ORB_W, ORB_H };
    BLENDFUNCTION bf;
    int x, y;

    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = ORB_W;
    bi.bmiHeader.biHeight      = -ORB_H;     /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void **)&pixels, NULL, 0);
    if (!bmp) {
        DeleteDC(mem);
        ReleaseDC(NULL, screen);
        return;
    }
    oldBmp = (HBITMAP)SelectObject(mem, bmp);

    /* Soft glowing orb. radius = distance from center, normalized to 0..1
       at the outer edge. Alpha falls off smoothly so the boundary blends
       into whatever's underneath the window. */
    for (y = 0; y < ORB_H; ++y) {
        for (x = 0; x < ORB_W; ++x) {
            float dx = (float)(x - ORB_W / 2);
            float dy = (float)(y - ORB_H / 2);
            float r  = sqrtf(dx * dx + dy * dy) / (ORB_W / 2.0f);
            float a;
            BYTE B, G, R, A;
            int idx = y * ORB_W + x;

            if (r >= 1.0f) {
                pixels[idx] = 0;
                continue;
            }
            /* Pulse with t */
            {
                float core    = 1.0f - r;
                float intensity = 0.5f + 0.5f * sinf(t * 2.0f);
                a = core * core * (0.4f + 0.6f * intensity);
                if (a > 1.0f) a = 1.0f;
            }
            /* Cyan/magenta gradient by angle for visual interest */
            R = (BYTE)(255.0f * (0.5f + 0.5f * sinf(t + 0.0f)));
            G = (BYTE)(255.0f * (0.5f + 0.5f * sinf(t + 2.0f)));
            B = (BYTE)(255.0f * (0.5f + 0.5f * sinf(t + 4.0f)));
            A = (BYTE)(255.0f * a);
            /* Premultiplied alpha! */
            B = (BYTE)((B * A) / 255);
            G = (BYTE)((G * A) / 255);
            R = (BYTE)((R * A) / 255);
            pixels[idx] = ((DWORD)A << 24) | ((DWORD)R << 16) |
                          ((DWORD)G << 8)  |  (DWORD)B;
        }
    }

    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(orb, screen, NULL, &size, mem, &srcPt,
                        0, &bf, ULW_ALPHA);

    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
}

static void Pp_Show(PpState *st)
{
    if (st->orb) {
        ShowWindow(st->orb, SW_SHOW);
        st->visible = TRUE;
        st->startTick = GetTickCount();
        if (!st->timerId) {
            st->timerId = SetTimer(st->frame, 1, 50, NULL);
        }
        SetWindowTextW(st->status, L"Orb visible (animated).");
    }
}

static void Pp_Hide(PpState *st)
{
    if (st->orb) {
        ShowWindow(st->orb, SW_HIDE);
        st->visible = FALSE;
        if (st->timerId) { KillTimer(st->frame, st->timerId); st->timerId = 0; }
        SetWindowTextW(st->status, L"Orb hidden.");
    }
}

static LRESULT CALLBACK Pp_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    PpState *st = (PpState *)GetPropW(hwnd, PP_PROP);
    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_PP_GO)   { Pp_Show(st); return 0; }
        if (LOWORD(wp) == ID_PP_HIDE) { Pp_Hide(st); return 0; }
    }
    if (msg == WM_TIMER && st && st->visible) {
        float t = (GetTickCount() - st->startTick) / 1000.0f;
        Pp_RenderOrb(st->orb, t);
        return 0;
    }
    if (msg == WM_DESTROY && st) {
        Pp_Hide(st);
        if (st->orb) DestroyWindow(st->orb);
        free(st);
        RemovePropW(hwnd, PP_PROP);
    }
    return CallWindowProcW(g_origPpFrame, hwnd, msg, wp, lp);
}

static LRESULT CALLBACK Pp_OrbProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    /* Make the orb draggable */
    if (msg == WM_NCHITTEST) return HTCAPTION;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void Pp_RegisterOrbClass(HINSTANCE hi)
{
    static BOOL registered = FALSE;
    WNDCLASSEXW wc;
    if (registered) return;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Pp_OrbProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_HAND);
    wc.lpszClassName = L"MiniShell_PerPixOrb";
    RegisterClassExW(&wc);
    registered = TRUE;
}

static HWND PerPixAlpha_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    PpState *st;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"PerPixAlpha",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (PpState *)calloc(1, sizeof(PpState));
    if (!st) { DestroyWindow(frame); return NULL; }
    st->frame = frame;

    Pp_RegisterOrbClass(hInstance);
    st->orb = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"MiniShell_PerPixOrb", L"",
        WS_POPUP,
        x + 240, y + 60, ORB_W, ORB_H,
        NULL, NULL, hInstance, NULL);

    CreateWindowExW(0, L"STATIC",
        L"Per-pixel alpha via UpdateLayeredWindow.\n"
        L"The orb is its own borderless layered window with a 32-bit\n"
        L"premultiplied BGRA bitmap. Drag it; it will float over anything.",
        WS_CHILD | WS_VISIBLE,
        12, 30, w - 24, 60, frame, NULL, hInstance, NULL);

    CreateWindowExW(0, L"BUTTON", L"Show orb",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 96, 110, 26, frame, (HMENU)(LONG_PTR)ID_PP_GO, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Hide",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        130, 96, 90, 26, frame, (HMENU)(LONG_PTR)ID_PP_HIDE, hInstance, NULL);

    st->status = CreateWindowExW(0, L"STATIC", L"Click Show to render.",
        WS_CHILD | WS_VISIBLE,
        12, 132, w - 24, 22, frame, (HMENU)(LONG_PTR)ID_PP_STAT, hInstance, NULL);

    SetPropW(frame, PP_PROP, (HANDLE)st);
    if (!g_origPpFrame) g_origPpFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Pp_FrameProc);
    return frame;
}

MsApp g_AppPerPixAlpha = { L"PerPixAlpha", PerPixAlpha_Create, 460, 200 };
