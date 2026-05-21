/*
 * app_monitors.c — Multi-monitor enumeration
 *
 * Demonstrates:
 *   - EnumDisplayMonitors with a callback that collects MONITORINFOEX records
 *   - Drawing a scaled diagram showing where each monitor sits in the
 *     virtual screen space, with its device name and resolution
 *
 * Useful as a sanity check for multi-monitor setups and a demo of the
 * monitor-related Win32 surface.
 */

#include "shell.h"
#include <stdlib.h>
#include <stdio.h>

#define MN_CLASS L"MiniShell_MonCanvas"

#define MAX_MONS 8

typedef struct {
    MONITORINFOEXW info[MAX_MONS];
    int count;
} MonState;

static BOOL CALLBACK Mn_Collector(HMONITOR mon, HDC hdc, LPRECT rc, LPARAM lp)
{
    MonState *st = (MonState *)lp;
    (void)hdc; (void)rc;
    if (st->count >= MAX_MONS) return TRUE;
    st->info[st->count].cbSize = sizeof(MONITORINFOEXW);
    if (GetMonitorInfoW(mon, (MONITORINFO *)&st->info[st->count])) {
        st->count++;
    }
    return TRUE;
}

static LRESULT CALLBACK Mn_CanvasProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    MonState *st = (MonState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE:
        st = (MonState *)calloc(1, sizeof(MonState));
        if (!st) return -1;
        EnumDisplayMonitors(NULL, NULL, Mn_Collector, (LPARAM)st);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc;
        RECT rc;
        int i, minX = 0, minY = 0, maxX = 0, maxY = 0;
        double sx, sy, scale;
        int marginX, marginY;
        HFONT font, oldFont;
        HBRUSH bg, monBr, primBr;

        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);
        bg = CreateSolidBrush(RGB(28, 30, 40));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        if (!st || st->count == 0) {
            EndPaint(hwnd, &ps);
            return 0;
        }

        /* Compute virtual bounding rect */
        minX = st->info[0].rcMonitor.left;
        minY = st->info[0].rcMonitor.top;
        maxX = st->info[0].rcMonitor.right;
        maxY = st->info[0].rcMonitor.bottom;
        for (i = 1; i < st->count; ++i) {
            if (st->info[i].rcMonitor.left   < minX) minX = st->info[i].rcMonitor.left;
            if (st->info[i].rcMonitor.top    < minY) minY = st->info[i].rcMonitor.top;
            if (st->info[i].rcMonitor.right  > maxX) maxX = st->info[i].rcMonitor.right;
            if (st->info[i].rcMonitor.bottom > maxY) maxY = st->info[i].rcMonitor.bottom;
        }
        if (maxX <= minX || maxY <= minY) { EndPaint(hwnd, &ps); return 0; }

        sx = (double)(rc.right - 40)  / (double)(maxX - minX);
        sy = (double)(rc.bottom - 60) / (double)(maxY - minY);
        scale = sx < sy ? sx : sy;
        marginX = (rc.right  - (int)((maxX - minX) * scale)) / 2;
        marginY = (rc.bottom - (int)((maxY - minY) * scale)) / 2;

        font = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        oldFont = (HFONT)SelectObject(hdc, font);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(240, 240, 245));

        monBr  = CreateSolidBrush(RGB(60, 80, 130));
        primBr = CreateSolidBrush(RGB(80, 140, 180));

        for (i = 0; i < st->count; ++i) {
            RECT r;
            wchar_t label[200];
            BOOL primary = (st->info[i].dwFlags & MONITORINFOF_PRIMARY) != 0;
            r.left   = marginX + (int)((st->info[i].rcMonitor.left   - minX) * scale);
            r.top    = marginY + (int)((st->info[i].rcMonitor.top    - minY) * scale);
            r.right  = marginX + (int)((st->info[i].rcMonitor.right  - minX) * scale);
            r.bottom = marginY + (int)((st->info[i].rcMonitor.bottom - minY) * scale);
            FillRect(hdc, &r, primary ? primBr : monBr);
            FrameRect(hdc, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));

            swprintf_s(label, 200, L"%s\n%ldx%ld\n%s",
                st->info[i].szDevice,
                st->info[i].rcMonitor.right - st->info[i].rcMonitor.left,
                st->info[i].rcMonitor.bottom - st->info[i].rcMonitor.top,
                primary ? L"(primary)" : L"");
            DrawTextW(hdc, label, -1, &r, DT_CENTER | DT_WORDBREAK);
        }

        DeleteObject(monBr);
        DeleteObject(primBr);
        SelectObject(hdc, oldFont);
        DeleteObject(font);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DISPLAYCHANGE:
        if (st) {
            st->count = 0;
            EnumDisplayMonitors(NULL, NULL, Mn_Collector, (LPARAM)st);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_DESTROY:
        if (st) free(st);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void EnsureMnClass(HINSTANCE hInstance)
{
    static BOOL registered = FALSE;
    WNDCLASSEXW wc;
    if (registered) return;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = Mn_CanvasProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = MN_CLASS;
    RegisterClassExW(&wc);
    registered = TRUE;
}

static WNDPROC g_origMnFrame = NULL;

static LRESULT CALLBACK Mn_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_SIZE) {
        HWND canvas = FindWindowExW(hwnd, NULL, MN_CLASS, NULL);
        if (canvas) {
            int w = LOWORD(lp), h = HIWORD(lp);
            MoveWindow(canvas, 4, 32, w - 8, h - 36, TRUE);
        }
    }
    return CallWindowProcW(g_origMnFrame, hwnd, msg, wp, lp);
}

static HWND Monitors_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    (void)self;

    EnsureMnClass(hInstance);
    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Monitors",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    CreateWindowExW(0, MN_CLASS, L"",
        WS_CHILD | WS_VISIBLE,
        4, 32, w - 8, h - 36, frame, NULL, hInstance, NULL);

    if (!g_origMnFrame) g_origMnFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Mn_FrameProc);
    return frame;
}

MsApp g_AppMonitors = {
    L"Monitors",
    Monitors_Create,
    520, 380
};
