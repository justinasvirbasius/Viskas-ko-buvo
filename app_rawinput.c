/*
 * app_rawinput.c — Raw mouse input from the HID stack
 *
 * Demonstrates:
 *   - RegisterRawInputDevices to opt into WM_INPUT messages from a specific
 *     usage page/usage (here: generic desktop / mouse)
 *   - GetRawInputData to extract a RAWINPUT struct describing the device
 *     activity at the HID level (independent of cursor acceleration, pointer
 *     coalescing, etc)
 *   - Painting a velocity dot to visualize the mouse delta stream
 *
 * The hook is scoped to this window only (no RIDEV_INPUTSINK) — we get raw
 * input when the window is in the foreground. This is the standard pattern
 * for games and creative apps that want real input rather than the
 * accelerated cursor delta.
 */

#include "shell.h"
#include <stdlib.h>
#include <stdio.h>

#ifndef HID_USAGE_PAGE_GENERIC
#define HID_USAGE_PAGE_GENERIC 0x01
#endif
#ifndef HID_USAGE_GENERIC_MOUSE
#define HID_USAGE_GENERIC_MOUSE 0x02
#endif

#define RI_CLASS L"MiniShell_RawCanvas"

typedef struct {
    LONG totalDx, totalDy;
    LONG lastDx, lastDy;
    DWORD buttons;
    DWORD events;
} RiState;

static LRESULT CALLBACK Ri_CanvasProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    RiState *st = (RiState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        RAWINPUTDEVICE rid;
        st = (RiState *)calloc(1, sizeof(RiState));
        if (!st) return -1;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);

        rid.usUsagePage = HID_USAGE_PAGE_GENERIC;
        rid.usUsage     = HID_USAGE_GENERIC_MOUSE;
        rid.dwFlags     = 0;     /* WM_INPUT only when in foreground */
        rid.hwndTarget  = hwnd;
        if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
            /* keep window alive, just won't receive WM_INPUT */
        }
        SetTimer(hwnd, 1, 50, NULL);
        return 0;
    }

    case WM_INPUT: {
        BYTE buf[64];
        UINT size = sizeof(buf);
        RAWINPUT *ri = (RAWINPUT *)buf;

        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, buf, &size,
                            sizeof(RAWINPUTHEADER)) > 0) {
            if (ri->header.dwType == RIM_TYPEMOUSE) {
                st->lastDx = ri->data.mouse.lLastX;
                st->lastDy = ri->data.mouse.lLastY;
                st->totalDx += st->lastDx;
                st->totalDy += st->lastDy;
                st->buttons = ri->data.mouse.usButtonFlags;
                st->events++;
            }
        }
        return 0;
    }

    case WM_TIMER:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc, memDC;
        HBITMAP memBmp, oldBmp;
        RECT rc;
        HBRUSH bg;
        HFONT font, oldFont;
        wchar_t lines[6][96];
        int i;

        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);
        memDC = CreateCompatibleDC(hdc);
        memBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        oldBmp = (HBITMAP)SelectObject(memDC, memBmp);
        bg = CreateSolidBrush(RGB(22, 26, 36));
        FillRect(memDC, &rc, bg);
        DeleteObject(bg);

        font = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Consolas");
        oldFont = (HFONT)SelectObject(memDC, font);
        SetBkMode(memDC, TRANSPARENT);
        SetTextColor(memDC, RGB(220, 230, 240));

        if (st) {
            swprintf_s(lines[0], 96, L"WM_INPUT events: %lu", st->events);
            swprintf_s(lines[1], 96, L"Last delta:  %+5ld, %+5ld",
                       st->lastDx, st->lastDy);
            swprintf_s(lines[2], 96, L"Accumulated: %+8ld, %+8ld",
                       st->totalDx, st->totalDy);
            swprintf_s(lines[3], 96, L"Buttons: 0x%04lx", st->buttons);
            swprintf_s(lines[4], 96, L"(register flag = foreground only)");
            swprintf_s(lines[5], 96, L"Move the mouse over this window");
            for (i = 0; i < 6; ++i)
                TextOutW(memDC, 12, 12 + i * 18, lines[i], (int)wcslen(lines[i]));

            /* Draw a velocity dot */
            {
                int cx = rc.right / 2;
                int cy = rc.bottom * 3 / 4;
                int dx = cx + st->lastDx;
                int dy = cy + st->lastDy;
                HBRUSH dotBr = CreateSolidBrush(RGB(255, 180, 80));
                RECT dot;
                dot.left = dx - 5; dot.top = dy - 5;
                dot.right = dx + 5; dot.bottom = dy + 5;
                FillRect(memDC, &dot, dotBr);
                DeleteObject(dotBr);
            }
        }

        SelectObject(memDC, oldFont);
        DeleteObject(font);
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY: {
        RAWINPUTDEVICE rid;
        KillTimer(hwnd, 1);
        rid.usUsagePage = HID_USAGE_PAGE_GENERIC;
        rid.usUsage     = HID_USAGE_GENERIC_MOUSE;
        rid.dwFlags     = RIDEV_REMOVE;
        rid.hwndTarget  = NULL;
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
        if (st) free(st);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void EnsureRiClass(HINSTANCE hInstance)
{
    static BOOL registered = FALSE;
    WNDCLASSEXW wc;
    if (registered) return;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = Ri_CanvasProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = RI_CLASS;
    RegisterClassExW(&wc);
    registered = TRUE;
}

static WNDPROC g_origRiFrame = NULL;

static LRESULT CALLBACK Ri_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_SIZE) {
        HWND canvas = FindWindowExW(hwnd, NULL, RI_CLASS, NULL);
        if (canvas) {
            int w = LOWORD(lp), h = HIWORD(lp);
            MoveWindow(canvas, 4, 32, w - 8, h - 36, TRUE);
        }
    }
    return CallWindowProcW(g_origRiFrame, hwnd, msg, wp, lp);
}

static HWND RawInput_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    (void)self;

    EnsureRiClass(hInstance);
    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"RawInput",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    CreateWindowExW(0, RI_CLASS, L"",
        WS_CHILD | WS_VISIBLE,
        4, 32, w - 8, h - 36, frame, NULL, hInstance, NULL);

    if (!g_origRiFrame) g_origRiFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Ri_FrameProc);
    return frame;
}

MsApp g_AppRawInput = {
    L"RawInput",
    RawInput_Create,
    420, 280
};
