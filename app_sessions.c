/*
 * app_sessions.c — Enumerate Terminal Services / logon sessions
 *
 * Demonstrates the WTSAPI:
 *   - WTSEnumerateSessionsW with WTS_CURRENT_SERVER_HANDLE
 *   - PWTS_SESSION_INFOW array of {SessionId, WinStationName, State}
 *   - WTSQuerySessionInformationW(WTSUserName / WTSDomainName / WTSClientName /
 *     WTSConnectState) for each session
 *   - WTSFreeMemory for everything returned by WTS
 *
 * Even on a single-user PC there's usually a "Services" session (0) plus
 * the interactive session (1+), so this exercises the API meaningfully.
 */

#include "shell.h"
#include <commctrl.h>
#include <wtsapi32.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "comctl32.lib")

#define SE_PROP   L"MS_SE_STATE"
#define ID_SE_LV  47001
#define ID_SE_REF 47002

typedef struct {
    HWND list, refBtn;
} SeState;

static WNDPROC g_origSeFrame = NULL;

static const wchar_t *Se_StateName(WTS_CONNECTSTATE_CLASS s)
{
    switch (s) {
    case WTSActive:      return L"Active";
    case WTSConnected:   return L"Connected";
    case WTSConnectQuery:return L"ConnectQuery";
    case WTSShadow:      return L"Shadow";
    case WTSDisconnected:return L"Disconnected";
    case WTSIdle:        return L"Idle";
    case WTSListen:      return L"Listen";
    case WTSReset:       return L"Reset";
    case WTSDown:        return L"Down";
    case WTSInit:        return L"Init";
    }
    return L"?";
}

static void Se_AddRow(HWND list, int row, const wchar_t *id,
                      const wchar_t *win, const wchar_t *state,
                      const wchar_t *user, const wchar_t *domain,
                      const wchar_t *client)
{
    LVITEMW it;
    ZeroMemory(&it, sizeof(it));
    it.mask = LVIF_TEXT;
    it.iItem = row;
    it.pszText = (LPWSTR)id;
    SendMessageW(list, LVM_INSERTITEMW, 0, (LPARAM)&it);
    it.iSubItem = 1; it.pszText = (LPWSTR)win;
    SendMessageW(list, LVM_SETITEMW, 0, (LPARAM)&it);
    it.iSubItem = 2; it.pszText = (LPWSTR)state;
    SendMessageW(list, LVM_SETITEMW, 0, (LPARAM)&it);
    it.iSubItem = 3; it.pszText = (LPWSTR)user;
    SendMessageW(list, LVM_SETITEMW, 0, (LPARAM)&it);
    it.iSubItem = 4; it.pszText = (LPWSTR)domain;
    SendMessageW(list, LVM_SETITEMW, 0, (LPARAM)&it);
    it.iSubItem = 5; it.pszText = (LPWSTR)client;
    SendMessageW(list, LVM_SETITEMW, 0, (LPARAM)&it);
}

static void Se_Refresh(SeState *st)
{
    PWTS_SESSION_INFOW infos = NULL;
    DWORD count = 0, i;
    SendMessageW(st->list, LVM_DELETEALLITEMS, 0, 0);

    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &infos, &count)) {
        Se_AddRow(st->list, 0, L"-", L"WTSEnumerateSessions failed",
                  L"", L"", L"", L"");
        return;
    }
    for (i = 0; i < count; ++i) {
        wchar_t idstr[16];
        LPWSTR user = NULL, domain = NULL, client = NULL;
        DWORD cb;
        swprintf_s(idstr, 16, L"%lu", infos[i].SessionId);

        WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE,
            infos[i].SessionId, WTSUserName, &user, &cb);
        WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE,
            infos[i].SessionId, WTSDomainName, &domain, &cb);
        WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE,
            infos[i].SessionId, WTSClientName, &client, &cb);

        Se_AddRow(st->list, (int)i, idstr,
                  infos[i].pWinStationName ? infos[i].pWinStationName : L"",
                  Se_StateName(infos[i].State),
                  (user   && *user)   ? user   : L"-",
                  (domain && *domain) ? domain : L"-",
                  (client && *client) ? client : L"-");

        if (user)   WTSFreeMemory(user);
        if (domain) WTSFreeMemory(domain);
        if (client) WTSFreeMemory(client);
    }
    WTSFreeMemory(infos);
}

static LRESULT CALLBACK Se_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    SeState *st = (SeState *)GetPropW(hwnd, SE_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_SE_REF) { Se_Refresh(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->refBtn, 8, 34, 100, 24, TRUE);
        MoveWindow(st->list,   8, 64, w - 16, h - 72, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, SE_PROP); }
    return CallWindowProcW(g_origSeFrame, hwnd, msg, wp, lp);
}

static HWND Sessions_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    SeState *st;
    INITCOMMONCONTROLSEX icc;
    LVCOLUMNW col;
    (void)self;

    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Sessions",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (SeState *)calloc(1, sizeof(SeState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->refBtn = CreateWindowExW(0, L"BUTTON", L"Refresh",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        8, 34, 100, 24, frame, (HMENU)(LONG_PTR)ID_SE_REF, hInstance, NULL);

    st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
        WS_CHILD | WS_VISIBLE | LVS_REPORT,
        8, 64, w - 16, h - 72, frame, (HMENU)(LONG_PTR)ID_SE_LV, hInstance, NULL);
    SendMessageW(st->list, LVM_SETEXTENDEDLISTVIEWSTYLE,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 60;  col.pszText = (LPWSTR)L"ID";
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.cx = 130; col.pszText = (LPWSTR)L"Station";
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    col.cx = 110; col.pszText = (LPWSTR)L"State";
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);
    col.cx = 140; col.pszText = (LPWSTR)L"User";
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 3, (LPARAM)&col);
    col.cx = 110; col.pszText = (LPWSTR)L"Domain";
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 4, (LPARAM)&col);
    col.cx = 130; col.pszText = (LPWSTR)L"Client";
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 5, (LPARAM)&col);

    SetPropW(frame, SE_PROP, (HANDLE)st);
    if (!g_origSeFrame) g_origSeFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Se_FrameProc);
    Se_Refresh(st);
    return frame;
}

MsApp g_AppSessions = {
    L"Sessions",
    Sessions_Create,
    760, 360
};
