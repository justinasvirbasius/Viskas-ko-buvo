/*
 * app_sysmon.c — Memory + CPU history
 *
 * Calls GlobalMemoryStatusEx and GetSystemTimes once a second. The CPU
 * percentage is computed from the delta of kernel + user time vs total
 * elapsed (which includes the idle slice). A ring buffer holds the last
 * N samples and is drawn as a line graph.
 */

#include "shell.h"
#include <stdio.h>
#include <stdlib.h>

#define SYSMON_CLASS  L"MiniShell_SysMonCanvas"
#define SYSMON_TIMER  1
#define SAMPLES       80

typedef struct {
    double samples[SAMPLES];
    int    head;
    int    count;

    ULARGE_INTEGER lastIdle, lastKernel, lastUser;
    BOOL   hasLast;

    DWORDLONG memTotal;
    DWORDLONG memUsed;
} SysMonState;

static ULARGE_INTEGER ft2u(FILETIME ft)
{
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u;
}

static void SysMon_Sample(SysMonState *st)
{
    FILETIME idle, kernel, user;
    MEMORYSTATUSEX ms;

    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        st->memTotal = ms.ullTotalPhys;
        st->memUsed  = ms.ullTotalPhys - ms.ullAvailPhys;
    }

    if (GetSystemTimes(&idle, &kernel, &user)) {
        ULARGE_INTEGER i = ft2u(idle);
        ULARGE_INTEGER k = ft2u(kernel);
        ULARGE_INTEGER u = ft2u(user);
        if (st->hasLast) {
            ULONGLONG dIdle   = i.QuadPart - st->lastIdle.QuadPart;
            ULONGLONG dKernel = k.QuadPart - st->lastKernel.QuadPart;
            ULONGLONG dUser   = u.QuadPart - st->lastUser.QuadPart;
            ULONGLONG total   = dKernel + dUser; /* kernel already includes idle */
            double pct = 0.0;
            if (total > 0) pct = 100.0 * (double)(total - dIdle) / (double)total;
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;

            st->samples[st->head] = pct;
            st->head = (st->head + 1) % SAMPLES;
            if (st->count < SAMPLES) st->count++;
        }
        st->lastIdle = i;
        st->lastKernel = k;
        st->lastUser = u;
        st->hasLast = TRUE;
    }
}

static LRESULT CALLBACK SysMon_CanvasProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    SysMonState *st = (SysMonState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE:
        st = (SysMonState *)calloc(1, sizeof(SysMonState));
        if (!st) return -1;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        SysMon_Sample(st);
        SetTimer(hwnd, SYSMON_TIMER, 1000, NULL);
        return 0;

    case WM_TIMER:
        SysMon_Sample(st);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc, memDC;
        HBITMAP memBmp, oldBmp;
        RECT rc;
        HBRUSH bg;
        HPEN axisPen, linePen;
        HGDIOBJ oldPen;
        HFONT font, oldFont;
        int i, w, h, graphTop, graphBottom, graphLeft, graphRight;
        wchar_t buf[128];

        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);
        w = rc.right; h = rc.bottom;

        /* Double-buffer */
        memDC = CreateCompatibleDC(hdc);
        memBmp = CreateCompatibleBitmap(hdc, w, h);
        oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

        bg = CreateSolidBrush(RGB(20, 25, 35));
        FillRect(memDC, &rc, bg);
        DeleteObject(bg);

        font = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        oldFont = (HFONT)SelectObject(memDC, font);
        SetBkMode(memDC, TRANSPARENT);
        SetTextColor(memDC, RGB(200, 220, 240));

        /* Header text */
        if (st->memTotal > 0) {
            double pct = 100.0 * (double)st->memUsed / (double)st->memTotal;
            swprintf_s(buf, 128, L"Memory: %.1f / %.1f GiB  (%.1f%%)",
                (double)st->memUsed / (1024.0 * 1024 * 1024),
                (double)st->memTotal / (1024.0 * 1024 * 1024),
                pct);
            TextOutW(memDC, 10, 8, buf, (int)wcslen(buf));
        }
        if (st->count > 0) {
            int latest = (st->head - 1 + SAMPLES) % SAMPLES;
            swprintf_s(buf, 128, L"CPU: %.1f%%", st->samples[latest]);
            TextOutW(memDC, 10, 28, buf, (int)wcslen(buf));
        }

        /* Graph area */
        graphTop    = 56;
        graphBottom = h - 12;
        graphLeft   = 10;
        graphRight  = w - 10;

        axisPen = CreatePen(PS_SOLID, 1, RGB(80, 90, 110));
        oldPen = SelectObject(memDC, axisPen);
        /* Frame */
        MoveToEx(memDC, graphLeft, graphTop, NULL);
        LineTo(memDC, graphLeft, graphBottom);
        LineTo(memDC, graphRight, graphBottom);
        /* 50% gridline */
        {
            int mid = (graphTop + graphBottom) / 2;
            MoveToEx(memDC, graphLeft, mid, NULL);
            LineTo(memDC, graphRight, mid);
        }
        SelectObject(memDC, oldPen);
        DeleteObject(axisPen);

        /* Plot the ring buffer */
        if (st->count >= 2) {
            int gw = graphRight - graphLeft;
            int gh = graphBottom - graphTop;
            int n  = st->count;
            int start = (st->head - n + SAMPLES) % SAMPLES;

            linePen = CreatePen(PS_SOLID, 2, RGB(120, 200, 255));
            oldPen = SelectObject(memDC, linePen);
            for (i = 0; i < n; ++i) {
                int idx = (start + i) % SAMPLES;
                int x = graphLeft + (i * gw) / (SAMPLES - 1);
                int y = graphBottom - (int)((st->samples[idx] / 100.0) * gh);
                if (i == 0) MoveToEx(memDC, x, y, NULL);
                else        LineTo(memDC, x, y);
            }
            SelectObject(memDC, oldPen);
            DeleteObject(linePen);
        }

        SelectObject(memDC, oldFont);
        DeleteObject(font);

        BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, SYSMON_TIMER);
        if (st) free(st);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void EnsureSysMonClass(HINSTANCE hInstance)
{
    static BOOL registered = FALSE;
    WNDCLASSEXW wc;
    if (registered) return;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = SysMon_CanvasProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = SYSMON_CLASS;
    RegisterClassExW(&wc);
    registered = TRUE;
}

static WNDPROC g_origSysMonFrame = NULL;

static LRESULT CALLBACK SysMon_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_SIZE) {
        HWND canvas = FindWindowExW(hwnd, NULL, SYSMON_CLASS, NULL);
        if (canvas) {
            int w = LOWORD(lp), h = HIWORD(lp);
            MoveWindow(canvas, 4, 32, w - 8, h - 36, TRUE);
        }
    }
    return CallWindowProcW(g_origSysMonFrame, hwnd, msg, wp, lp);
}

static HWND SysMon_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    (void)self;

    EnsureSysMonClass(hInstance);
    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"SysMon",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    CreateWindowExW(0, SYSMON_CLASS, L"",
        WS_CHILD | WS_VISIBLE,
        4, 32, w - 8, h - 36, frame, NULL, hInstance, NULL);

    if (!g_origSysMonFrame)
        g_origSysMonFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)SysMon_FrameProc);
    return frame;
}

MsApp g_AppSysMon = {
    L"SysMon",
    SysMon_Create,
    420, 280
};
