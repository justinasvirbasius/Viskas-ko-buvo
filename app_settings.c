/*
 * app_settings.c — Settings, with registry persistence
 *
 * Demonstrates:
 *   - TabControl (WC_TABCONTROL) with multiple pages
 *   - Show/hide of child controls when the active tab changes
 *   - Registry I/O via RegCreateKeyExW / RegSetValueExW / RegQueryValueExW
 *     under HKCU\Software\MiniShell
 *
 * Stores two settings: a display "Username" string and a numeric "Volume"
 * (0-100, edited via a trackbar). Save writes both to the registry; the
 * values persist across app restarts.
 */

#include "shell.h"
#include <commctrl.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "advapi32.lib")

#define SET_PROP   L"MS_SET_STATE"
#define ID_TABS    8001
#define ID_USER    8010
#define ID_VOLUME  8011
#define ID_SAVE    8012
#define ID_VOLLBL  8013

#define REG_PATH   L"Software\\MiniShell"

typedef struct {
    HWND tabs;
    HWND userEdit;       /* page 0 */
    HWND volume;         /* page 1 (trackbar) */
    HWND volLabel;       /* page 1 */
    HWND saveBtn;
    int  currentTab;
} SetState;

static WNDPROC g_origSetFrame = NULL;

static void Set_Load(SetState *st)
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        wchar_t buf[256];
        DWORD len = sizeof(buf);
        DWORD vol = 50;
        DWORD volLen = sizeof(vol);

        if (RegQueryValueExW(key, L"Username", NULL, NULL,
                             (BYTE *)buf, &len) == ERROR_SUCCESS) {
            SetWindowTextW(st->userEdit, buf);
        }
        if (RegQueryValueExW(key, L"Volume", NULL, NULL,
                             (BYTE *)&vol, &volLen) == ERROR_SUCCESS) {
            SendMessageW(st->volume, TBM_SETPOS, TRUE, (LPARAM)vol);
        }
        RegCloseKey(key);
    }
}

static void Set_Save(SetState *st)
{
    HKEY key;
    DWORD disp;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, NULL, 0,
                        KEY_WRITE, NULL, &key, &disp) != ERROR_SUCCESS) {
        MessageBoxW(GetParent(st->tabs), L"Failed to open registry key.",
                    L"Settings", MB_ICONWARNING);
        return;
    }
    {
        wchar_t buf[256];
        DWORD len;
        DWORD vol = (DWORD)SendMessageW(st->volume, TBM_GETPOS, 0, 0);

        GetWindowTextW(st->userEdit, buf, 256);
        len = (DWORD)((wcslen(buf) + 1) * sizeof(wchar_t));
        RegSetValueExW(key, L"Username", 0, REG_SZ, (BYTE *)buf, len);
        RegSetValueExW(key, L"Volume", 0, REG_DWORD, (BYTE *)&vol, sizeof(vol));
    }
    RegCloseKey(key);
    MessageBoxW(GetParent(st->tabs), L"Settings saved to HKCU\\Software\\MiniShell.",
                L"Settings", MB_ICONINFORMATION);
}

static void Set_ShowTab(SetState *st, int tab)
{
    st->currentTab = tab;
    ShowWindow(st->userEdit, tab == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(st->volume,   tab == 1 ? SW_SHOW : SW_HIDE);
    ShowWindow(st->volLabel, tab == 1 ? SW_SHOW : SW_HIDE);
}

static void Set_UpdateVolLabel(SetState *st)
{
    wchar_t buf[32];
    int v = (int)SendMessageW(st->volume, TBM_GETPOS, 0, 0);
    swprintf_s(buf, 32, L"Volume: %d", v);
    SetWindowTextW(st->volLabel, buf);
}

static LRESULT CALLBACK Set_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    SetState *st = (SetState *)GetPropW(hwnd, SET_PROP);

    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_SAVE) {
            Set_Save(st);
            return 0;
        }
    }
    if (msg == WM_NOTIFY && st) {
        NMHDR *hdr = (NMHDR *)lp;
        if (hdr->idFrom == ID_TABS && hdr->code == TCN_SELCHANGE) {
            int sel = (int)SendMessageW(st->tabs, TCM_GETCURSEL, 0, 0);
            Set_ShowTab(st, sel);
            return 0;
        }
    }
    if (msg == WM_HSCROLL && st) {
        Set_UpdateVolLabel(st);
        return 0;
    }
    if (msg == WM_DESTROY && st) {
        free(st);
        RemovePropW(hwnd, SET_PROP);
    }
    return CallWindowProcW(g_origSetFrame, hwnd, msg, wp, lp);
}

static HWND Settings_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    SetState *st;
    INITCOMMONCONTROLSEX icc;
    TCITEMW ti;
    RECT pageRc;

    (void)self;
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_TAB_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Settings",
        WS_POPUP | WS_BORDER,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (SetState *)calloc(1, sizeof(SetState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->tabs = CreateWindowExW(0, WC_TABCONTROLW, NULL,
        WS_CHILD | WS_VISIBLE,
        8, 34, w - 16, h - 80, frame, (HMENU)(LONG_PTR)ID_TABS, hInstance, NULL);

    ZeroMemory(&ti, sizeof(ti));
    ti.mask = TCIF_TEXT;
    ti.pszText = (LPWSTR)L"Account";
    SendMessageW(st->tabs, TCM_INSERTITEMW, 0, (LPARAM)&ti);
    ti.pszText = (LPWSTR)L"Audio";
    SendMessageW(st->tabs, TCM_INSERTITEMW, 1, (LPARAM)&ti);

    /* Compute the tab control's display area */
    GetClientRect(st->tabs, &pageRc);
    SendMessageW(st->tabs, TCM_ADJUSTRECT, FALSE, (LPARAM)&pageRc);

    /* Account page */
    st->userEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | ES_AUTOHSCROLL,
        16 + pageRc.left, 34 + pageRc.top + 8,
        w - 16 - pageRc.left * 2 - 16, 24,
        frame, (HMENU)(LONG_PTR)ID_USER, hInstance, NULL);

    /* Audio page */
    st->volLabel = CreateWindowExW(0, L"STATIC", L"Volume: 50",
        WS_CHILD,
        16 + pageRc.left, 34 + pageRc.top + 8, 120, 20,
        frame, (HMENU)(LONG_PTR)ID_VOLLBL, hInstance, NULL);

    st->volume = CreateWindowExW(0, TRACKBAR_CLASSW, NULL,
        WS_CHILD | TBS_HORZ | TBS_AUTOTICKS,
        16 + pageRc.left, 34 + pageRc.top + 32,
        w - 16 - pageRc.left * 2 - 16, 30,
        frame, (HMENU)(LONG_PTR)ID_VOLUME, hInstance, NULL);
    SendMessageW(st->volume, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(st->volume, TBM_SETPOS,   TRUE, 50);

    st->saveBtn = CreateWindowExW(0, L"BUTTON", L"Save",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        w - 96, h - 38, 80, 26,
        frame, (HMENU)(LONG_PTR)ID_SAVE, hInstance, NULL);

    SetPropW(frame, SET_PROP, (HANDLE)st);
    if (!g_origSetFrame)
        g_origSetFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Set_FrameProc);

    Set_ShowTab(st, 0);
    Set_Load(st);
    Set_UpdateVolLabel(st);
    return frame;
}

MsApp g_AppSettings = {
    L"Settings",
    Settings_Create,
    400, 320
};
