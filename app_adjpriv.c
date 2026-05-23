/*
 * app_adjpriv.c — Enable/disable process token privileges
 *
 * Demonstrates the access-token privilege management API:
 *   - OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES |
 *     TOKEN_QUERY, &token) opens our own token writable
 *   - LookupPrivilegeValueW(NULL, L"SeShutdownPrivilege", &luid) resolves
 *     a privilege name string to its system LUID
 *   - LookupPrivilegeDisplayNameW(NULL, name, buf, &cb, &langId) gets
 *     the localized friendly description
 *   - AdjustTokenPrivileges(token, FALSE, &tp, ..., NULL, NULL) flips
 *     the SE_PRIVILEGE_ENABLED bit; on success GetLastError can still
 *     report ERROR_NOT_ALL_ASSIGNED meaning we don't *hold* that privilege
 *   - GetTokenInformation(TokenPrivileges, ...) walks the token's
 *     TOKEN_PRIVILEGES array to inspect current state
 *
 * We list the privileges held by our process token and let the user
 * toggle the enabled bit on/off for the selected one.
 */

#include "shell.h"
#include <commctrl.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comctl32.lib")

#define AP_PROP   L"MS_AP_STATE"
#define ID_AP_LV  110001
#define ID_AP_EN  110002
#define ID_AP_DIS 110003
#define ID_AP_REF 110004

typedef struct { HWND list, enableBtn, disableBtn, refreshBtn; } ApState;
static WNDPROC g_origApFrame = NULL;

static void Ap_Refresh(ApState *st)
{
    HANDLE token;
    DWORD  needed = 0;
    TOKEN_PRIVILEGES *tp;

    SendMessageW(st->list, LVM_DELETEALLITEMS, 0, 0);

    if (!OpenProcessToken(GetCurrentProcess(),
            TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &token)) return;

    GetTokenInformation(token, TokenPrivileges, NULL, 0, &needed);
    if (!needed) { CloseHandle(token); return; }
    tp = (TOKEN_PRIVILEGES *)calloc(1, needed);
    if (!tp) { CloseHandle(token); return; }
    if (GetTokenInformation(token, TokenPrivileges, tp, needed, &needed)) {
        DWORD i;
        for (i = 0; i < tp->PrivilegeCount; ++i) {
            wchar_t name[80]    = L"";
            wchar_t display[200]= L"";
            wchar_t enabled[16];
            DWORD   nameLen = 80, dispLen = 200, langId = 0;
            LVITEMW it;

            LookupPrivilegeNameW(NULL, &tp->Privileges[i].Luid, name, &nameLen);
            LookupPrivilegeDisplayNameW(NULL, name, display, &dispLen, &langId);
            wcscpy_s(enabled, 16,
                (tp->Privileges[i].Attributes & SE_PRIVILEGE_ENABLED)
                    ? L"ENABLED"
                    : (tp->Privileges[i].Attributes & SE_PRIVILEGE_ENABLED_BY_DEFAULT)
                        ? L"default"
                        : L"-");

            ZeroMemory(&it, sizeof(it));
            it.mask = LVIF_TEXT; it.iItem = (int)i;
            it.pszText = name;     SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
            it.iSubItem = 1; it.pszText = enabled; SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
            it.iSubItem = 2; it.pszText = display; SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
        }
    }
    free(tp);
    CloseHandle(token);
}

static void Ap_Toggle(ApState *st, BOOL enable)
{
    int sel = (int)SendMessageW(st->list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    wchar_t name[80];
    LVITEMW it;
    HANDLE  token;
    LUID    luid;
    TOKEN_PRIVILEGES tp;

    if (sel < 0) return;
    ZeroMemory(&it, sizeof(it));
    it.iSubItem = 0; it.cchTextMax = 80; it.pszText = name;
    SendMessageW(st->list, LVM_GETITEMTEXTW, sel, (LPARAM)&it);
    if (!name[0]) return;

    if (!LookupPrivilegeValueW(NULL, name, &luid)) return;
    if (!OpenProcessToken(GetCurrentProcess(),
            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return;

    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = enable ? SE_PRIVILEGE_ENABLED : 0;

    AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), NULL, NULL);
    /* GetLastError == ERROR_NOT_ALL_ASSIGNED means we don't hold this one */

    CloseHandle(token);
    Ap_Refresh(st);
}

static LRESULT CALLBACK Ap_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    ApState *st = (ApState *)GetPropW(hwnd, AP_PROP);
    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_AP_REF) { Ap_Refresh(st); return 0; }
        if (LOWORD(wp) == ID_AP_EN)  { Ap_Toggle(st, TRUE); return 0; }
        if (LOWORD(wp) == ID_AP_DIS) { Ap_Toggle(st, FALSE); return 0; }
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->enableBtn,  12, 38, 120, 26, TRUE);
        MoveWindow(st->disableBtn, 140, 38, 120, 26, TRUE);
        MoveWindow(st->refreshBtn, 268, 38, 100, 26, TRUE);
        MoveWindow(st->list,       8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, AP_PROP); }
    return CallWindowProcW(g_origApFrame, hwnd, msg, wp, lp);
}

static HWND AdjPriv_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    ApState *st;
    INITCOMMONCONTROLSEX icc;
    LVCOLUMNW col;
    (void)self;

    icc.dwSize = sizeof(icc); icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"AdjPriv",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (ApState *)calloc(1, sizeof(ApState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->enableBtn  = CreateWindowExW(0, L"BUTTON", L"Enable selected",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 120, 26, frame, (HMENU)(LONG_PTR)ID_AP_EN, hInstance, NULL);
    st->disableBtn = CreateWindowExW(0, L"BUTTON", L"Disable",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        140, 38, 120, 26, frame, (HMENU)(LONG_PTR)ID_AP_DIS, hInstance, NULL);
    st->refreshBtn = CreateWindowExW(0, L"BUTTON", L"Refresh",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        268, 38, 100, 26, frame, (HMENU)(LONG_PTR)ID_AP_REF, hInstance, NULL);

    st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_AP_LV, hInstance, NULL);
    SendMessageW(st->list, LVM_SETEXTENDEDLISTVIEWSTYLE,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 240; col.pszText = (LPWSTR)L"Privilege name";     SendMessageW(st->list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.cx = 100; col.pszText = (LPWSTR)L"State";              SendMessageW(st->list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    col.cx = 400; col.pszText = (LPWSTR)L"Display name";        SendMessageW(st->list, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);

    SetPropW(frame, AP_PROP, (HANDLE)st);
    if (!g_origApFrame) g_origApFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Ap_FrameProc);
    Ap_Refresh(st);
    return frame;
}

MsApp g_AppAdjPriv = { L"AdjPriv", AdjPriv_Create, 880, 460 };
