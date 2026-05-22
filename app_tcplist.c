/*
 * app_tcplist.c — Enumerate active TCP connections with owning process
 *
 * Demonstrates the IP Helper API:
 *   - GetExtendedTcpTable with TCP_TABLE_OWNER_PID_ALL (IPv4) — two-pass sizing
 *   - MIB_TCPROW_OWNER_PID layout: state, local/remote addr+port, owning PID
 *   - inet_ntop for printable IPv4 addresses; ntohs for port byte-swap
 *   - Cross-referencing PIDs with process names via OpenProcess +
 *     QueryFullProcessImageNameW
 *
 * The view refreshes on Refresh and on a 3-second timer. Many local
 * connections will be in LISTENING state (no remote), some in ESTABLISHED.
 */

#include "shell.h"
#include <commctrl.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <psapi.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "comctl32.lib")

#define TL_PROP   L"MS_TL_STATE"
#define ID_TL_LV  54001
#define ID_TL_REF 54002
#define TL_TIMER  1

typedef struct {
    HWND list, refBtn;
} TlState;

static WNDPROC g_origTlFrame = NULL;

static const wchar_t *Tl_State(DWORD s)
{
    switch (s) {
    case MIB_TCP_STATE_CLOSED:     return L"CLOSED";
    case MIB_TCP_STATE_LISTEN:     return L"LISTEN";
    case MIB_TCP_STATE_SYN_SENT:   return L"SYN_SENT";
    case MIB_TCP_STATE_SYN_RCVD:   return L"SYN_RCVD";
    case MIB_TCP_STATE_ESTAB:      return L"ESTABLISHED";
    case MIB_TCP_STATE_FIN_WAIT1:  return L"FIN_WAIT1";
    case MIB_TCP_STATE_FIN_WAIT2:  return L"FIN_WAIT2";
    case MIB_TCP_STATE_CLOSE_WAIT: return L"CLOSE_WAIT";
    case MIB_TCP_STATE_CLOSING:    return L"CLOSING";
    case MIB_TCP_STATE_LAST_ACK:   return L"LAST_ACK";
    case MIB_TCP_STATE_TIME_WAIT:  return L"TIME_WAIT";
    case MIB_TCP_STATE_DELETE_TCB: return L"DELETE_TCB";
    }
    return L"?";
}

static void Tl_Ipv4Port(DWORD addr, DWORD port, wchar_t *out, int cch)
{
    struct in_addr a;
    char buf[INET_ADDRSTRLEN];
    a.S_un.S_addr = addr;
    inet_ntop(AF_INET, &a, buf, sizeof(buf));
    swprintf_s(out, cch, L"%hs:%u", buf, (unsigned)ntohs((u_short)port));
}

static void Tl_ProcessName(DWORD pid, wchar_t *out, int cch)
{
    HANDLE h;
    wchar_t path[MAX_PATH];
    DWORD cb = MAX_PATH;

    if (pid == 0) { wcscpy_s(out, cch, L"<System>"); return; }
    if (pid == 4) { wcscpy_s(out, cch, L"<Kernel>"); return; }
    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) { swprintf_s(out, cch, L"PID %lu", pid); return; }
    if (QueryFullProcessImageNameW(h, 0, path, &cb)) {
        const wchar_t *base = wcsrchr(path, L'\\');
        wcscpy_s(out, cch, base ? base + 1 : path);
    } else {
        swprintf_s(out, cch, L"PID %lu", pid);
    }
    CloseHandle(h);
}

static void Tl_Refresh(TlState *st)
{
    PMIB_TCPTABLE_OWNER_PID table = NULL;
    DWORD cb = 0, rc;
    DWORD i;

    SendMessageW(st->list, LVM_DELETEALLITEMS, 0, 0);

    /* Sizing pass */
    rc = GetExtendedTcpTable(NULL, &cb, TRUE, AF_INET,
                              TCP_TABLE_OWNER_PID_ALL, 0);
    if (rc != ERROR_INSUFFICIENT_BUFFER) return;
    table = (PMIB_TCPTABLE_OWNER_PID)malloc(cb);
    if (!table) return;
    rc = GetExtendedTcpTable(table, &cb, TRUE, AF_INET,
                              TCP_TABLE_OWNER_PID_ALL, 0);
    if (rc != NO_ERROR) { free(table); return; }

    for (i = 0; i < table->dwNumEntries; ++i) {
        MIB_TCPROW_OWNER_PID *r = &table->table[i];
        wchar_t local[64], remote[64], pidStr[16], name[MAX_PATH];
        LVITEMW it;

        Tl_Ipv4Port(r->dwLocalAddr, r->dwLocalPort, local, 64);
        if (r->dwState == MIB_TCP_STATE_LISTEN) {
            wcscpy_s(remote, 64, L"-");
        } else {
            Tl_Ipv4Port(r->dwRemoteAddr, r->dwRemotePort, remote, 64);
        }
        swprintf_s(pidStr, 16, L"%lu", r->dwOwningPid);
        Tl_ProcessName(r->dwOwningPid, name, MAX_PATH);

        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        it.pszText = (LPWSTR)Tl_State(r->dwState);
        SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        it.iSubItem = 1; it.pszText = local;
        SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
        it.iSubItem = 2; it.pszText = remote;
        SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
        it.iSubItem = 3; it.pszText = pidStr;
        SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
        it.iSubItem = 4; it.pszText = name;
        SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
    }
    free(table);
}

static LRESULT CALLBACK Tl_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    TlState *st = (TlState *)GetPropW(hwnd, TL_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_TL_REF) { Tl_Refresh(st); return 0; }
    if (msg == WM_TIMER && st) { Tl_Refresh(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->refBtn, 8, 34, 100, 24, TRUE);
        MoveWindow(st->list,   8, 64, w - 16, h - 72, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        KillTimer(hwnd, TL_TIMER);
        free(st);
        RemovePropW(hwnd, TL_PROP);
    }
    return CallWindowProcW(g_origTlFrame, hwnd, msg, wp, lp);
}

static HWND TcpList_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    TlState *st;
    INITCOMMONCONTROLSEX icc;
    LVCOLUMNW col;
    WSADATA wsa;
    (void)self;

    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);
    WSAStartup(MAKEWORD(2, 2), &wsa);   /* needed for inet_ntop */

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"TcpList",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (TlState *)calloc(1, sizeof(TlState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->refBtn = CreateWindowExW(0, L"BUTTON", L"Refresh",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        8, 34, 100, 24, frame, (HMENU)(LONG_PTR)ID_TL_REF, hInstance, NULL);

    st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
        WS_CHILD | WS_VISIBLE | LVS_REPORT,
        8, 64, w - 16, h - 72, frame, (HMENU)(LONG_PTR)ID_TL_LV, hInstance, NULL);
    SendMessageW(st->list, LVM_SETEXTENDEDLISTVIEWSTYLE,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 110; col.pszText = (LPWSTR)L"State";
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.cx = 160; col.pszText = (LPWSTR)L"Local";
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    col.cx = 160; col.pszText = (LPWSTR)L"Remote";
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);
    col.cx =  60; col.pszText = (LPWSTR)L"PID";
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 3, (LPARAM)&col);
    col.cx = 200; col.pszText = (LPWSTR)L"Process";
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 4, (LPARAM)&col);

    SetPropW(frame, TL_PROP, (HANDLE)st);
    if (!g_origTlFrame) g_origTlFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Tl_FrameProc);

    Tl_Refresh(st);
    SetTimer(frame, TL_TIMER, 3000, NULL);
    return frame;
}

MsApp g_AppTcpList = {
    L"TcpList",
    TcpList_Create,
    760, 440
};
