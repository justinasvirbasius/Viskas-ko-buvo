/*
 * app_tray.c — System tray icon
 *
 * Demonstrates:
 *   - Shell_NotifyIconW with NIM_ADD/NIM_MODIFY/NIM_DELETE
 *   - NIF_MESSAGE callback message routing — the icon sends our custom
 *     WM_TRAYICON to a hidden message-only window, which we forward
 *     into the frame
 *   - Balloon notifications via NIF_INFO
 *   - Right-click popup menu via TrackPopupMenu
 *
 * The frame contains a couple of buttons:
 *   - "Add to tray"     — installs the icon
 *   - "Show balloon"    — pops a notification
 *   - "Remove from tray"— removes the icon
 */

#include "shell.h"
#include <shellapi.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "shell32.lib")

#define TRAY_PROP      L"MS_TRAY_STATE"
#define ID_TRAY_ADD    14001
#define ID_TRAY_BALLOON 14002
#define ID_TRAY_REMOVE 14003
#define ID_TRAY_STATUS 14004

#define WM_TRAYICON    (WM_USER + 70)

#define IDM_TRAY_SHOW  14100
#define IDM_TRAY_HIDE  14101
#define IDM_TRAY_EXIT  14102

typedef struct {
    NOTIFYICONDATAW nid;
    BOOL installed;
    HWND status;
    HWND parent;
} TrayState;

static WNDPROC g_origTrayFrame = NULL;

static void Tray_Add(HWND frame, TrayState *st)
{
    if (st->installed) return;
    ZeroMemory(&st->nid, sizeof(st->nid));
    st->nid.cbSize           = sizeof(st->nid);
    st->nid.hWnd             = frame;
    st->nid.uID              = 1;
    st->nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    st->nid.uCallbackMessage = WM_TRAYICON;
    st->nid.hIcon            = LoadIcon(NULL, IDI_APPLICATION);
    wcscpy_s(st->nid.szTip, ARRAYSIZE(st->nid.szTip), L"MiniShell Tray");
    if (Shell_NotifyIconW(NIM_ADD, &st->nid)) {
        st->installed = TRUE;
        SetWindowTextW(st->status, L"Status: installed in tray");
    } else {
        SetWindowTextW(st->status, L"Status: NIM_ADD failed");
    }
}

static void Tray_Remove(TrayState *st)
{
    if (!st->installed) return;
    Shell_NotifyIconW(NIM_DELETE, &st->nid);
    st->installed = FALSE;
    SetWindowTextW(st->status, L"Status: removed");
}

static void Tray_Balloon(TrayState *st)
{
    if (!st->installed) {
        SetWindowTextW(st->status, L"Status: install icon first");
        return;
    }
    st->nid.uFlags = NIF_INFO;
    wcscpy_s(st->nid.szInfoTitle, ARRAYSIZE(st->nid.szInfoTitle),
             L"MiniShell");
    wcscpy_s(st->nid.szInfo, ARRAYSIZE(st->nid.szInfo),
             L"Hello from a tray balloon notification.");
    st->nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &st->nid);
    /* Restore the regular tooltip flags for future calls */
    st->nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}

static void Tray_ShowMenu(HWND frame, TrayState *st)
{
    POINT pt;
    HMENU menu;
    int cmd;
    (void)st;

    GetCursorPos(&pt);
    menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_TRAY_SHOW, L"Show window");
    AppendMenuW(menu, MF_STRING, IDM_TRAY_HIDE, L"Hide window");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"Remove tray icon");

    /* SetForegroundWindow before TrackPopupMenu is the documented
     * workaround for the menu to dismiss properly */
    SetForegroundWindow(frame);
    cmd = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD,
                         pt.x, pt.y, 0, frame, NULL);
    DestroyMenu(menu);
    PostMessageW(frame, WM_NULL, 0, 0);   /* per docs */

    switch (cmd) {
    case IDM_TRAY_SHOW: ShowWindow(frame, SW_SHOW); break;
    case IDM_TRAY_HIDE: ShowWindow(frame, SW_HIDE); break;
    case IDM_TRAY_EXIT: Tray_Remove((TrayState *)GetPropW(frame, TRAY_PROP)); break;
    }
}

static LRESULT CALLBACK Tray_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    TrayState *st = (TrayState *)GetPropW(hwnd, TRAY_PROP);

    if (msg == WM_TRAYICON && st) {
        switch (LOWORD(lp)) {
        case WM_LBUTTONUP:
            ShowWindow(hwnd, IsWindowVisible(hwnd) ? SW_HIDE : SW_SHOW);
            return 0;
        case WM_RBUTTONUP:
            Tray_ShowMenu(hwnd, st);
            return 0;
        }
    }
    if (msg == WM_COMMAND && st) {
        switch (LOWORD(wp)) {
        case ID_TRAY_ADD:     Tray_Add(hwnd, st); return 0;
        case ID_TRAY_BALLOON: Tray_Balloon(st);   return 0;
        case ID_TRAY_REMOVE:  Tray_Remove(st);    return 0;
        }
    }
    if (msg == WM_DESTROY && st) {
        Tray_Remove(st);
        free(st);
        RemovePropW(hwnd, TRAY_PROP);
    }
    return CallWindowProcW(g_origTrayFrame, hwnd, msg, wp, lp);
}

static HWND Tray_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    TrayState *st;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Tray",
        WS_POPUP | WS_BORDER,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (TrayState *)calloc(1, sizeof(TrayState));
    if (!st) { DestroyWindow(frame); return NULL; }
    st->parent = parent;

    CreateWindowExW(0, L"BUTTON", L"Add to tray",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        12, 40, 130, 28, frame, (HMENU)(LONG_PTR)ID_TRAY_ADD, hInstance, NULL);

    CreateWindowExW(0, L"BUTTON", L"Show balloon",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        12, 76, 130, 28, frame, (HMENU)(LONG_PTR)ID_TRAY_BALLOON, hInstance, NULL);

    CreateWindowExW(0, L"BUTTON", L"Remove from tray",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        12, 112, 130, 28, frame, (HMENU)(LONG_PTR)ID_TRAY_REMOVE, hInstance, NULL);

    st->status = CreateWindowExW(0, L"STATIC", L"Status: not installed",
        WS_CHILD | WS_VISIBLE,
        12, 156, w - 24, 20, frame, (HMENU)(LONG_PTR)ID_TRAY_STATUS, hInstance, NULL);

    CreateWindowExW(0, L"STATIC",
        L"Left-click the tray icon to toggle window;\nright-click for menu.",
        WS_CHILD | WS_VISIBLE,
        12, 180, w - 24, 40, frame, NULL, hInstance, NULL);

    SetPropW(frame, TRAY_PROP, (HANDLE)st);
    if (!g_origTrayFrame)
        g_origTrayFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Tray_FrameProc);
    return frame;
}

MsApp g_AppTray = {
    L"Tray",
    Tray_Create,
    320, 260
};
