/*
 * app_clock.c — Analog clock app
 *
 * Inside an app-frame window, a child window draws an analog clock face
 * using GDI. A 1-second timer triggers redraw.
 */

#include "shell.h"
#include <math.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define CLOCK_CLASS L"MiniShell_ClockClass"
#define CLOCK_TIMER 1

static LRESULT CALLBACK Clock_WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, CLOCK_TIMER, 1000, NULL);
        return 0;

    case WM_TIMER:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC  hdc;
        RECT rc;
        int  cx, cy, radius, i;
        HBRUSH faceBrush;
        HPEN   tickPen, hourPen, minPen, secPen;
        HGDIOBJ oldPen, oldBrush;
        time_t now;
        struct tm lt;
        double hAng, mAng, sAng;

        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);
        cx = (rc.right - rc.left) / 2;
        cy = (rc.bottom - rc.top) / 2;
        radius = (cx < cy ? cx : cy) - 12;

        /* Background */
        faceBrush = CreateSolidBrush(RGB(245, 245, 240));
        FillRect(hdc, &rc, faceBrush);
        oldBrush = SelectObject(hdc, faceBrush);

        /* Face circle */
        tickPen = CreatePen(PS_SOLID, 2, RGB(40, 40, 50));
        oldPen = SelectObject(hdc, tickPen);
        Ellipse(hdc, cx - radius, cy - radius, cx + radius, cy + radius);

        /* Hour ticks */
        for (i = 0; i < 12; ++i) {
            double a = (i / 12.0) * 2.0 * M_PI - M_PI / 2.0;
            int x1 = cx + (int)((radius - 10) * cos(a));
            int y1 = cy + (int)((radius - 10) * sin(a));
            int x2 = cx + (int)((radius - 2)  * cos(a));
            int y2 = cy + (int)((radius - 2)  * sin(a));
            MoveToEx(hdc, x1, y1, NULL);
            LineTo(hdc, x2, y2);
        }
        SelectObject(hdc, oldPen);
        DeleteObject(tickPen);

        /* Get current time */
        time(&now);
        localtime_s(&lt, &now);

        sAng = (lt.tm_sec / 60.0) * 2.0 * M_PI - M_PI / 2.0;
        mAng = ((lt.tm_min + lt.tm_sec / 60.0) / 60.0) * 2.0 * M_PI - M_PI / 2.0;
        hAng = (((lt.tm_hour % 12) + lt.tm_min / 60.0) / 12.0) * 2.0 * M_PI - M_PI / 2.0;

        /* Hour hand */
        hourPen = CreatePen(PS_SOLID, 5, RGB(20, 20, 30));
        oldPen = SelectObject(hdc, hourPen);
        MoveToEx(hdc, cx, cy, NULL);
        LineTo(hdc, cx + (int)((radius * 0.5) * cos(hAng)),
                    cy + (int)((radius * 0.5) * sin(hAng)));
        SelectObject(hdc, oldPen);
        DeleteObject(hourPen);

        /* Minute hand */
        minPen = CreatePen(PS_SOLID, 3, RGB(20, 20, 30));
        oldPen = SelectObject(hdc, minPen);
        MoveToEx(hdc, cx, cy, NULL);
        LineTo(hdc, cx + (int)((radius * 0.75) * cos(mAng)),
                    cy + (int)((radius * 0.75) * sin(mAng)));
        SelectObject(hdc, oldPen);
        DeleteObject(minPen);

        /* Second hand */
        secPen = CreatePen(PS_SOLID, 1, RGB(200, 40, 40));
        oldPen = SelectObject(hdc, secPen);
        MoveToEx(hdc, cx, cy, NULL);
        LineTo(hdc, cx + (int)((radius * 0.85) * cos(sAng)),
                    cy + (int)((radius * 0.85) * sin(sAng)));
        SelectObject(hdc, oldPen);
        DeleteObject(secPen);

        SelectObject(hdc, oldBrush);
        DeleteObject(faceBrush);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, CLOCK_TIMER);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void EnsureClass(HINSTANCE hInstance)
{
    static BOOL registered = FALSE;
    WNDCLASSEXW wc;
    if (registered) return;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc   = Clock_WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = CLOCK_CLASS;
    RegisterClassExW(&wc);
    registered = TRUE;
}

static HWND Clock_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame, child;

    (void)self;
    EnsureClass(hInstance);

    /* Create the app frame */
    frame = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        MS_CLASS_APPFRAME,
        L"Clock",
        WS_POPUP | WS_BORDER,
        x, y, w, h,
        parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    /* Child window draws the clock face below the title bar */
    child = CreateWindowExW(
        0,
        CLOCK_CLASS, L"",
        WS_CHILD | WS_VISIBLE,
        0, 28, w, h - 28,
        frame, NULL, hInstance, NULL);
    (void)child;

    return frame;
}

MsApp g_AppClock = {
    L"Clock",
    Clock_Create,
    260, 290
};
