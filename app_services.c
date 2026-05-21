/*
 * app_services.c — Windows services browser
 *
 * Demonstrates the Service Control Manager API:
 *   - OpenSCManagerW with SC_MANAGER_ENUMERATE_SERVICE
 *   - EnumServicesStatusExW (the dynamic-sized two-call dance)
 *   - OpenServiceW, StartServiceW, ControlService (SERVICE_CONTROL_STOP),
 *     QueryServiceStatusEx for refresh
 *
 * Listed in a ListView (report mode). Right-click a row for Start/Stop/
 * Refresh. Most users will only have query access; modifying state typically
 * needs an elevated process and you'll get ACCESS_DENIED otherwise — the
 * app reports this honestly rather than pretending.
 */

#include "shell.h"
#include <commctrl.h>
#include <winsvc.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "advapi32.lib")

#define SV_PROP    L"MS_SV_STATE"
#define ID_SV_LIST 23001
#define IDM_SV_START   23100
#define IDM_SV_STOP    23101
#define IDM_SV_REFRESH 23102

typedef struct {
    HWND list;
} SvState;

static WNDPROC g_origSvFrame = NULL;

static const wchar_t *Sv_StateName(DWORD st)
{
    switch (st) {
    case SERVICE_STOPPED:          return L"Stopped";
    case SERVICE_START_PENDING:    return L"Starting";
    case SERVICE_STOP_PENDING:     return L"Stopping";
    case SERVICE_RUNNING:          return L"Running";
    case SERVICE_CONTINUE_PENDING: return L"Continuing";
    case SERVICE_PAUSE_PENDING:    return L"Pausing";
    case SERVICE_PAUSED:           return L"Paused";
    default:                       return L"Unknown";
    }
}

static void Sv_Refresh(SvState *st)
{
    SC_HANDLE scm;
    DWORD bytesNeeded = 0, count = 0, resume = 0;
    ENUM_SERVICE_STATUS_PROCESSW *buf = NULL;
    LVITEMW item;
    int row = 0;

    SendMessageW(st->list, LVM_DELETEALLITEMS, 0, 0);

    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE | SC_MANAGER_CONNECT);
    if (!scm) return;

    /* First call to learn buffer size */
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
                          NULL, 0, &bytesNeeded, &count, &resume, NULL);
    if (bytesNeeded == 0) { CloseServiceHandle(scm); return; }

    buf = (ENUM_SERVICE_STATUS_PROCESSW *)malloc(bytesNeeded);
    if (!buf) { CloseServiceHandle(scm); return; }

    resume = 0;
    if (EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
                              SERVICE_STATE_ALL,
                              (LPBYTE)buf, bytesNeeded, &bytesNeeded,
                              &count, &resume, NULL)) {
        DWORD i;
        for (i = 0; i < count; ++i) {
            ZeroMemory(&item, sizeof(item));
            item.mask = LVIF_TEXT;
            item.iItem = row;
            item.pszText = buf[i].lpServiceName;
            SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&item);

            item.iSubItem = 1;
            item.pszText = buf[i].lpDisplayName;
            SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&item);

            item.iSubItem = 2;
            item.pszText = (LPWSTR)Sv_StateName(buf[i].ServiceStatusProcess.dwCurrentState);
            SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&item);

            ++row;
        }
    }
    free(buf);
    CloseServiceHandle(scm);
}

static BOOL Sv_GetSelectedName(SvState *st, wchar_t *out, int outLen)
{
    int sel = (int)SendMessageW(st->list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    LVITEMW it;
    if (sel < 0) return FALSE;
    ZeroMemory(&it, sizeof(it));
    it.mask = LVIF_TEXT;
    it.iItem = sel;
    it.pszText = out;
    it.cchTextMax = outLen;
    return (BOOL)SendMessageW(st->list, LVM_GETITEMTEXTW, sel, (LPARAM)&it);
}

static void Sv_Operate(HWND frame, SvState *st, BOOL start)
{
    wchar_t name[256];
    SC_HANDLE scm, svc;
    DWORD access = start ? SERVICE_START : SERVICE_STOP;

    if (!Sv_GetSelectedName(st, name, 256)) return;
    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return;
    svc = OpenServiceW(scm, name, access | SERVICE_QUERY_STATUS);
    if (!svc) {
        MessageBoxW(frame, L"OpenService failed (ACCESS_DENIED?).\n"
                           L"Modifying services usually requires elevation.",
                    L"Services", MB_ICONWARNING);
        CloseServiceHandle(scm);
        return;
    }
    if (start) {
        StartServiceW(svc, 0, NULL);
    } else {
        SERVICE_STATUS s;
        ControlService(svc, SERVICE_CONTROL_STOP, &s);
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    Sv_Refresh(st);
}

static void Sv_ShowContextMenu(HWND frame, SvState *st)
{
    POINT pt;
    HMENU menu;
    int cmd;

    GetCursorPos(&pt);
    menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_SV_START,   L"Start");
    AppendMenuW(menu, MF_STRING, IDM_SV_STOP,    L"Stop");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_SV_REFRESH, L"Refresh");

    SetForegroundWindow(frame);
    cmd = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD,
                         pt.x, pt.y, 0, frame, NULL);
    DestroyMenu(menu);

    switch (cmd) {
    case IDM_SV_START:   Sv_Operate(frame, st, TRUE);  break;
    case IDM_SV_STOP:    Sv_Operate(frame, st, FALSE); break;
    case IDM_SV_REFRESH: Sv_Refresh(st); break;
    }
}

static LRESULT CALLBACK Sv_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    SvState *st = (SvState *)GetPropW(hwnd, SV_PROP);

    if (msg == WM_NOTIFY && st) {
        NMHDR *hdr = (NMHDR *)lp;
        if (hdr->idFrom == ID_SV_LIST && hdr->code == NM_RCLICK) {
            Sv_ShowContextMenu(hwnd, st);
            return 0;
        }
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->list, 4, 32, w - 8, h - 36, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        free(st);
        RemovePropW(hwnd, SV_PROP);
    }
    return CallWindowProcW(g_origSvFrame, hwnd, msg, wp, lp);
}

static HWND Services_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    SvState *st;
    INITCOMMONCONTROLSEX icc;
    LVCOLUMNW col;
    (void)self;

    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Services",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (SvState *)calloc(1, sizeof(SvState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
        4, 32, w - 8, h - 36, frame, (HMENU)(LONG_PTR)ID_SV_LIST, hInstance, NULL);
    SendMessageW(st->list, LVM_SETEXTENDEDLISTVIEWSTYLE,
                 LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES,
                 LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = (LPWSTR)L"Name";    col.cx = 180;
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.pszText = (LPWSTR)L"Display"; col.cx = 280;
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    col.pszText = (LPWSTR)L"Status";  col.cx = 100;
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);

    SetPropW(frame, SV_PROP, (HANDLE)st);
    if (!g_origSvFrame)
        g_origSvFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Sv_FrameProc);

    Sv_Refresh(st);
    return frame;
}

MsApp g_AppServices = {
    L"Services",
    Services_Create,
    640, 460
};
