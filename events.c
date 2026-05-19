/*
 * events.c — Window procedures
 *
 * Shell_WndProc handles the desktop window: paints the wallpaper, draws the
 * taskbar, and routes taskbar clicks to the app registry.
 *
 * AppFrame_WndProc handles each app window's outer frame: paints a title bar,
 * forwards close, and unregisters from the window manager on destruction.
 */

#include "shell.h"
#include <stdio.h>

#define TITLEBAR_HEIGHT 28
#define CLOSE_BTN_SIZE  20

/* ---- Helpers ---- */

static void DrawTaskbar(HWND hwnd, HDC hdc)
{
    RECT rc;
    int  count, i;
    HBRUSH bgBrush, btnBrush;
    HPEN   pen;
    HFONT  font, oldFont;
    HGDIOBJ oldPen, oldBrush;

    GetClientRect(hwnd, &rc);

    /* Taskbar background */
    rc.top = rc.bottom - MS_TASKBAR_HEIGHT;
    bgBrush = CreateSolidBrush(RGB(20, 20, 30));
    FillRect(hdc, &rc, bgBrush);
    DeleteObject(bgBrush);

    /* Top edge highlight */
    pen = CreatePen(PS_SOLID, 1, RGB(80, 80, 100));
    oldPen = SelectObject(hdc, pen);
    MoveToEx(hdc, rc.left, rc.top, NULL);
    LineTo(hdc, rc.right, rc.top);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);

    /* App launcher buttons */
    font = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    oldFont = (HFONT)SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(230, 230, 240));

    btnBrush = CreateSolidBrush(RGB(45, 45, 70));
    oldBrush = SelectObject(hdc, btnBrush);

    count = Registry_Count();
    for (i = 0; i < count; ++i) {
        MsApp *app = Registry_GetAt(i);
        RECT btn;
        btn.left   = 10 + i * (MS_APP_BUTTON_WIDTH + 6);
        btn.top    = rc.top + 6;
        btn.right  = btn.left + MS_APP_BUTTON_WIDTH;
        btn.bottom = rc.bottom - 6;
        FillRect(hdc, &btn, btnBrush);
        DrawTextW(hdc, app->title, -1, &btn,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    SelectObject(hdc, oldBrush);
    DeleteObject(btnBrush);
    SelectObject(hdc, oldFont);
    DeleteObject(font);
}

static int TaskbarHitTest(HWND hwnd, int x, int y)
{
    RECT rc;
    int  count, i;
    GetClientRect(hwnd, &rc);
    if (y < rc.bottom - MS_TASKBAR_HEIGHT) return -1;

    count = Registry_Count();
    for (i = 0; i < count; ++i) {
        int left  = 10 + i * (MS_APP_BUTTON_WIDTH + 6);
        int right = left + MS_APP_BUTTON_WIDTH;
        if (x >= left && x <= right) return i;
    }
    return -1;
}

/* ---- Shell desktop window procedure ---- */

LRESULT CALLBACK Shell_WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        DrawTaskbar(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int x = LOWORD(lp);
        int y = HIWORD(lp);
        int idx = TaskbarHitTest(hwnd, x, y);
        if (idx >= 0) {
            Registry_Launch(idx, hwnd);
        }
        return 0;
    }

    case WM_KEYDOWN:
        /* Alt+Tab-like: Ctrl+Tab cycles app focus */
        if (wp == VK_TAB && (GetKeyState(VK_CONTROL) & 0x8000)) {
            WM_FocusNext();
        }
        /* Esc on desktop closes the shell */
        if (wp == VK_ESCAPE) {
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_MS_APP_CLOSED:
        /* An app frame is notifying us it was closed */
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- App frame window procedure ----
 * Each app is hosted inside one of these. The frame draws a title bar with a
 * close button; the actual app content is a child window beneath it.
 */

LRESULT CALLBACK AppFrame_WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCHITTEST: {
        /* Custom hit test so the title bar acts as drag handle */
        POINT pt;
        RECT  rc;
        pt.x = LOWORD(lp);
        pt.y = HIWORD(lp);
        ScreenToClient(hwnd, &pt);
        GetClientRect(hwnd, &rc);
        if (pt.y < TITLEBAR_HEIGHT) {
            if (pt.x > rc.right - CLOSE_BTN_SIZE - 6) return HTCLIENT;
            return HTCAPTION; /* drag */
        }
        return HTCLIENT;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC  hdc;
        RECT rc, title, closeBtn;
        HBRUSH titleBrush, closeBrush;
        HFONT  font, oldFont;
        MsAppWindow *aw;

        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);

        /* Title bar background */
        title = rc;
        title.bottom = TITLEBAR_HEIGHT;
        titleBrush = CreateSolidBrush(RGB(60, 80, 130));
        FillRect(hdc, &title, titleBrush);
        DeleteObject(titleBrush);

        /* Close button */
        closeBtn.right  = rc.right - 6;
        closeBtn.left   = closeBtn.right - CLOSE_BTN_SIZE;
        closeBtn.top    = 4;
        closeBtn.bottom = closeBtn.top + CLOSE_BTN_SIZE;
        closeBrush = CreateSolidBrush(RGB(180, 60, 60));
        FillRect(hdc, &closeBtn, closeBrush);
        DeleteObject(closeBrush);

        /* Title text */
        font = CreateFontW(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        oldFont = (HFONT)SelectObject(hdc, font);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(240, 240, 250));

        aw = WM_Find(hwnd);
        if (aw) {
            RECT tr = title;
            tr.left += 10;
            DrawTextW(hdc, aw->title, -1, &tr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        SetTextColor(hdc, RGB(255, 255, 255));
        DrawTextW(hdc, L"X", -1, &closeBtn,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SelectObject(hdc, oldFont);
        DeleteObject(font);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int x = LOWORD(lp);
        int y = HIWORD(lp);
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (y < TITLEBAR_HEIGHT && x > rc.right - CLOSE_BTN_SIZE - 6) {
            DestroyWindow(hwnd);
            return 0;
        }
        SetFocus(hwnd);
        return 0;
    }

    case WM_DESTROY: {
        HWND parent = GetParent(hwnd);
        WM_Unregister(hwnd);
        if (parent) PostMessageW(parent, WM_MS_APP_CLOSED, 0, 0);
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}
