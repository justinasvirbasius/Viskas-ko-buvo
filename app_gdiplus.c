/*
 * app_gdiplus.c — GDI+ anti-aliased drawing from plain C
 *
 * The official gdiplus.h is C++-only. We forward-declare the small subset of
 * the GDI+ flat API we need and link against gdiplus.lib. This is a
 * documented technique for C consumers.
 *
 * Demonstrates:
 *   - GdiplusStartup / GdiplusShutdown
 *   - SmoothingMode anti-aliasing, TextRenderingHint
 *   - GraphicsPath compound shape, linear gradient brush
 *   - Anti-aliased text via GdipDrawString
 */

#include "shell.h"
#include <stdlib.h>

#pragma comment(lib, "gdiplus.lib")

/* ---- Minimal GDI+ flat API surface in C ---- */

typedef int    GpStatus;
typedef float  REAL;
typedef DWORD  ARGB;

typedef struct GpGraphics       GpGraphics;
typedef struct GpPath           GpPath;
typedef struct GpBrush          GpBrush;
typedef struct GpSolidFill      GpSolidFill;
typedef struct GpLineGradient   GpLineGradient;
typedef struct GpPen            GpPen;
typedef struct GpFontFamily     GpFontFamily;
typedef struct GpFont           GpFont;
typedef struct GpStringFormat   GpStringFormat;

typedef struct { REAL X, Y; } GpPointF;
typedef struct { REAL X, Y, Width, Height; } GpRectF;

#define Ok 0
#define SmoothingModeAntiAlias 4
#define TextRenderingHintAntiAliasGridFit 3
#define FillModeAlternate 0
#define UnitPixel 2
#define FontStyleBold 1
#define WrapModeTile 0
#define StringAlignmentCenter 1

typedef struct {
    UINT32  GdiplusVersion;
    UINT_PTR DebugEventCallback;
    BOOL    SuppressBackgroundThread;
    BOOL    SuppressExternalCodecs;
} GdiplusStartupInput;

GpStatus WINAPI GdiplusStartup(ULONG_PTR *token, const GdiplusStartupInput *in, void *out);
VOID     WINAPI GdiplusShutdown(ULONG_PTR token);

GpStatus WINAPI GdipCreateFromHDC(HDC hdc, GpGraphics **g);
GpStatus WINAPI GdipDeleteGraphics(GpGraphics *g);
GpStatus WINAPI GdipSetSmoothingMode(GpGraphics *g, int mode);
GpStatus WINAPI GdipSetTextRenderingHint(GpGraphics *g, int hint);
GpStatus WINAPI GdipFillRectangleI(GpGraphics *g, GpBrush *brush, INT x, INT y, INT w, INT h);

GpStatus WINAPI GdipCreatePath(int mode, GpPath **path);
GpStatus WINAPI GdipDeletePath(GpPath *path);
GpStatus WINAPI GdipAddPathEllipseI(GpPath *path, INT x, INT y, INT w, INT h);
GpStatus WINAPI GdipFillPath(GpGraphics *g, GpBrush *brush, GpPath *path);
GpStatus WINAPI GdipDrawPath(GpGraphics *g, GpPen *pen, GpPath *path);

GpStatus WINAPI GdipCreateSolidFill(ARGB c, GpSolidFill **brush);
GpStatus WINAPI GdipDeleteBrush(GpBrush *brush);
GpStatus WINAPI GdipCreateLineBrush(const GpPointF *p1, const GpPointF *p2,
    ARGB c1, ARGB c2, int wrap, GpLineGradient **brush);

GpStatus WINAPI GdipCreatePen1(ARGB color, REAL width, int unit, GpPen **pen);
GpStatus WINAPI GdipDeletePen(GpPen *pen);

GpStatus WINAPI GdipCreateFontFamilyFromName(const WCHAR *name, void *coll,
                                             GpFontFamily **family);
GpStatus WINAPI GdipDeleteFontFamily(GpFontFamily *family);
GpStatus WINAPI GdipCreateFont(GpFontFamily *fam, REAL emSize, INT style,
                               int unit, GpFont **font);
GpStatus WINAPI GdipDeleteFont(GpFont *font);

GpStatus WINAPI GdipCreateStringFormat(INT flags, LANGID lang, GpStringFormat **fmt);
GpStatus WINAPI GdipDeleteStringFormat(GpStringFormat *fmt);
GpStatus WINAPI GdipSetStringFormatAlign(GpStringFormat *fmt, INT align);
GpStatus WINAPI GdipSetStringFormatLineAlign(GpStringFormat *fmt, INT align);

GpStatus WINAPI GdipDrawString(GpGraphics *g, const WCHAR *str, INT len,
    const GpFont *font, const GpRectF *layout, const GpStringFormat *fmt,
    const GpBrush *brush);

/* ---- The app itself ---- */

#define GP_CLASS L"MiniShell_GdipCanvas"

typedef struct {
    ULONG_PTR token;
} GpState;

static LRESULT CALLBACK Gp_CanvasProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    GpState *st = (GpState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        GdiplusStartupInput in = { 1, 0, FALSE, FALSE };
        st = (GpState *)calloc(1, sizeof(GpState));
        if (!st) return -1;
        if (GdiplusStartup(&st->token, &in, NULL) != Ok) {
            free(st);
            return -1;
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc;
        RECT rc;
        GpGraphics *g = NULL;
        GpPath *path = NULL;
        GpLineGradient *brush = NULL;
        GpSolidFill *textBrush = NULL, *bg = NULL;
        GpStringFormat *fmt = NULL;
        GpFontFamily *fam = NULL;
        GpFont *font = NULL;
        GpPen *pen = NULL;
        GpPointF gradFrom = { 0, 0 }, gradTo;

        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);
        if (GdipCreateFromHDC(hdc, &g) != Ok) {
            EndPaint(hwnd, &ps);
            return 0;
        }

        GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
        GdipSetTextRenderingHint(g, TextRenderingHintAntiAliasGridFit);

        /* Background */
        GdipCreateSolidFill(0xFF181C24, &bg);
        GdipFillRectangleI(g, (GpBrush *)bg, 0, 0, rc.right, rc.bottom);

        /* Compound path of overlapping ellipses */
        GdipCreatePath(FillModeAlternate, &path);
        GdipAddPathEllipseI(path, 40,  60,  220, 220);
        GdipAddPathEllipseI(path, 160, 60,  220, 220);
        GdipAddPathEllipseI(path, 100, 130, 200, 200);

        gradTo.X = (REAL)rc.right;
        gradTo.Y = (REAL)rc.bottom;
        GdipCreateLineBrush(&gradFrom, &gradTo, 0xFF3DA6FF, 0xFFF09BFF,
                            WrapModeTile, &brush);
        GdipFillPath(g, (GpBrush *)brush, path);

        GdipCreatePen1(0x80FFFFFF, 2.0f, UnitPixel, &pen);
        GdipDrawPath(g, pen, path);

        /* Anti-aliased text */
        if (GdipCreateFontFamilyFromName(L"Segoe UI", NULL, &fam) == Ok) {
            if (GdipCreateFont(fam, 22.0f, FontStyleBold, UnitPixel, &font) == Ok) {
                GpRectF tr;
                GdipCreateSolidFill(0xFFFFFFFF, &textBrush);
                GdipCreateStringFormat(0, 0, &fmt);
                GdipSetStringFormatAlign(fmt, StringAlignmentCenter);
                GdipSetStringFormatLineAlign(fmt, StringAlignmentCenter);
                tr.X = 0; tr.Y = 0;
                tr.Width  = (REAL)rc.right;
                tr.Height = (REAL)rc.bottom;
                GdipDrawString(g, L"GDI+", -1, font, &tr, fmt,
                               (GpBrush *)textBrush);
                GdipDeleteFont(font);
            }
            GdipDeleteFontFamily(fam);
        }

        if (textBrush) GdipDeleteBrush((GpBrush *)textBrush);
        if (fmt)       GdipDeleteStringFormat(fmt);
        if (pen)       GdipDeletePen(pen);
        if (brush)     GdipDeleteBrush((GpBrush *)brush);
        if (bg)        GdipDeleteBrush((GpBrush *)bg);
        if (path)      GdipDeletePath(path);
        GdipDeleteGraphics(g);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        if (st) { GdiplusShutdown(st->token); free(st); }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void EnsureGpClass(HINSTANCE hInstance)
{
    static BOOL registered = FALSE;
    WNDCLASSEXW wc;
    if (registered) return;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = Gp_CanvasProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = GP_CLASS;
    RegisterClassExW(&wc);
    registered = TRUE;
}

static WNDPROC g_origGpFrame = NULL;

static LRESULT CALLBACK Gp_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_SIZE) {
        HWND canvas = FindWindowExW(hwnd, NULL, GP_CLASS, NULL);
        if (canvas) {
            int w = LOWORD(lp), h = HIWORD(lp);
            MoveWindow(canvas, 4, 32, w - 8, h - 36, TRUE);
        }
    }
    return CallWindowProcW(g_origGpFrame, hwnd, msg, wp, lp);
}

static HWND GdiPlus_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    (void)self;

    EnsureGpClass(hInstance);
    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"GdiPlus",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    CreateWindowExW(0, GP_CLASS, L"",
        WS_CHILD | WS_VISIBLE,
        4, 32, w - 8, h - 36, frame, NULL, hInstance, NULL);

    if (!g_origGpFrame) g_origGpFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Gp_FrameProc);
    return frame;
}

MsApp g_AppGdiPlus = {
    L"GdiPlus",
    GdiPlus_Create,
    480, 380
};
