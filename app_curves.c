/*
 * app_curves.c — GDI curve and arc primitives
 *
 * Showcases primitives that previous apps haven't drawn:
 *   - Pie:        a filled "pie slice" from two radial angles
 *   - Chord:      like Pie but closed with a straight chord (no radii)
 *   - Arc:        the curve along the ellipse, no fill
 *   - ArcTo:      arc that connects to the current point (good for path UIs)
 *   - AngleArc:   arc by center+radius+start angle+sweep (in degrees)
 *   - PolyBezier: cubic Bezier curve with N anchor + control points
 *   - BeginPath / EndPath / StrokeAndFillPath: build a compound path
 *
 * The canvas is divided into a 3x2 grid; each cell labels its primitive
 * with TextOutW above it and draws an example below.
 */

#include "shell.h"
#include <math.h>
#include <stdlib.h>

#define CV_PROP    L"MS_CV_STATE"
#define ID_CV_BTN  70001

typedef struct { int dummy; } CvState;
static WNDPROC g_origCvFrame = NULL;

static void Cv_DrawCell(HDC hdc, RECT *cell, const wchar_t *label, int which)
{
    HBRUSH bg = CreateSolidBrush(RGB(248, 248, 250));
    HBRUSH fillRed   = CreateSolidBrush(RGB(220,  90, 100));
    HBRUSH fillBlue  = CreateSolidBrush(RGB( 90, 130, 220));
    HBRUSH fillGreen = CreateSolidBrush(RGB( 90, 180, 110));
    HPEN   thick     = CreatePen(PS_SOLID, 3, RGB(40, 50, 80));
    HPEN   thin      = CreatePen(PS_SOLID, 1, RGB(150, 150, 160));
    HBRUSH oldBrush;
    HPEN   oldPen;
    int    cx = (cell->left + cell->right) / 2;
    int    cy = (cell->top + cell->bottom) / 2 + 12;
    int    half = (cell->right - cell->left - 24) / 2;
    int    halfV = (cell->bottom - cell->top - 36) / 2;

    /* Frame */
    FillRect(hdc, cell, bg);
    FrameRect(hdc, cell, (HBRUSH)GetStockObject(GRAY_BRUSH));

    /* Label */
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(40, 40, 70));
    TextOutW(hdc, cell->left + 8, cell->top + 4, label, (int)wcslen(label));

    oldBrush = (HBRUSH)SelectObject(hdc, fillRed);
    oldPen   = (HPEN)SelectObject(hdc, thick);

    switch (which) {
    case 0: /* Pie */
        SelectObject(hdc, fillRed);
        Pie(hdc, cx - half, cy - halfV, cx + half, cy + halfV,
            cx + half, cy, cx, cy - halfV);
        break;
    case 1: /* Chord */
        SelectObject(hdc, fillBlue);
        Chord(hdc, cx - half, cy - halfV, cx + half, cy + halfV,
              cx + half, cy, cx - half / 2, cy + halfV);
        break;
    case 2: /* Arc + ArcTo composite */
        SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Arc(hdc, cx - half, cy - halfV, cx + half, cy + halfV,
            cx + half, cy, cx - half, cy);
        SelectObject(hdc, thin);
        MoveToEx(hdc, cx + half, cy, NULL);
        LineTo(hdc, cx + half + 10, cy);
        break;
    case 3: /* AngleArc */
        SelectObject(hdc, GetStockObject(NULL_BRUSH));
        MoveToEx(hdc, cx - halfV, cy, NULL);
        AngleArc(hdc, cx, cy, halfV, 0.0f, 270.0f);
        break;
    case 4: { /* PolyBezier (cubic) */
        POINT pts[4];
        pts[0].x = cell->left + 16;       pts[0].y = cy + halfV - 6;
        pts[1].x = cell->left + 16;       pts[1].y = cy - halfV;
        pts[2].x = cell->right - 16;      pts[2].y = cy + halfV;
        pts[3].x = cell->right - 16;      pts[3].y = cy - halfV + 6;
        SelectObject(hdc, GetStockObject(NULL_BRUSH));
        PolyBezier(hdc, pts, 4);
        /* anchors */
        SelectObject(hdc, thin);
        SelectObject(hdc, GetStockObject(BLACK_BRUSH));
        Ellipse(hdc, pts[0].x - 3, pts[0].y - 3, pts[0].x + 3, pts[0].y + 3);
        Ellipse(hdc, pts[3].x - 3, pts[3].y - 3, pts[3].x + 3, pts[3].y + 3);
        break;
    }
    case 5: { /* BeginPath / StrokeAndFillPath */
        BeginPath(hdc);
        MoveToEx(hdc, cx - half, cy + halfV, NULL);
        LineTo(hdc, cx,          cy - halfV);
        LineTo(hdc, cx + half,   cy + halfV);
        CloseFigure(hdc);
        Ellipse(hdc, cx - half / 3, cy - halfV / 3,
                     cx + half / 3, cy + halfV / 3);
        EndPath(hdc);
        SelectObject(hdc, fillGreen);
        StrokeAndFillPath(hdc);
        break;
    }
    }

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(bg);
    DeleteObject(fillRed);
    DeleteObject(fillBlue);
    DeleteObject(fillGreen);
    DeleteObject(thick);
    DeleteObject(thin);
}

static void Cv_Paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    int colW, rowH;
    int r, c;
    static const wchar_t *labels[6] = {
        L"Pie", L"Chord", L"Arc + ArcTo", L"AngleArc",
        L"PolyBezier (cubic)", L"Path: triangle + ellipse"
    };
    HDC mem;
    HBITMAP memBmp, oldBmp;

    GetClientRect(hwnd, &rc);
    /* Double-buffer */
    mem = CreateCompatibleDC(hdc);
    memBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    oldBmp = (HBITMAP)SelectObject(mem, memBmp);
    {
        HBRUSH bg = (HBRUSH)(COLOR_BTNFACE + 1);
        FillRect(mem, &rc, bg);
    }

    rc.top += 30; /* leave room for app header band */
    colW = (rc.right - rc.left) / 3;
    rowH = (rc.bottom - rc.top) / 2;
    for (r = 0; r < 2; ++r) {
        for (c = 0; c < 3; ++c) {
            RECT cell;
            cell.left   = rc.left + c * colW + 4;
            cell.top    = rc.top  + r * rowH + 4;
            cell.right  = cell.left + colW - 8;
            cell.bottom = cell.top  + rowH - 8;
            Cv_DrawCell(mem, &cell, labels[r * 3 + c], r * 3 + c);
        }
    }

    BitBlt(hdc, 0, 0, ps.rcPaint.right, ps.rcPaint.bottom, mem, 0, 0, SRCCOPY);

    SelectObject(mem, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK Cv_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    CvState *st = (CvState *)GetPropW(hwnd, CV_PROP);
    if (msg == WM_PAINT) { Cv_Paint(hwnd); return 0; }
    if (msg == WM_SIZE) { InvalidateRect(hwnd, NULL, FALSE); }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, CV_PROP); }
    return CallWindowProcW(g_origCvFrame, hwnd, msg, wp, lp);
}

static HWND Curves_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    CvState *st;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Curves",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (CvState *)calloc(1, sizeof(CvState));
    if (!st) { DestroyWindow(frame); return NULL; }

    SetPropW(frame, CV_PROP, (HANDLE)st);
    if (!g_origCvFrame) g_origCvFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Cv_FrameProc);
    return frame;
}

MsApp g_AppCurves = { L"Curves", Curves_Create, 720, 480 };
