/*
 * app_procs.c — Process list
 *
 * Demonstrates:
 *   - CreateToolhelp32Snapshot + Process32FirstW/Process32NextW
 *   - ListView in report mode (LVS_REPORT) with columns
 *   - Right-click context menu via CreatePopupMenu + TrackPopupMenu
 *   - OpenProcess + TerminateProcess (only succeeds with sufficient rights)
 *   - Periodic refresh via SetTimer
 */

#include "shell.h"
#include <commctrl.h>
#include <tlhelp32.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "comctl32.lib")

#define PROC_PROP    L"MS_PROC_STATE"
#define ID_LIST      7001
#define PROC_TIMER   1

#define IDM_TERMINATE 7100
#define IDM_REFRESH   7101

typedef struct {
    HWND list;
} ProcState;

static WNDPROC g_origProcFrame = NULL;

static void Proc_Refresh(ProcState *st)
{
    HANDLE snap;
    PROCESSENTRY32W pe;
    LVITEMW item;
    int row = 0;

    SendMessageW(st->list, LVM_DELETEALLITEMS, 0, 0);
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            wchar_t pidStr[16], thrStr[16];
            ZeroMemory(&item, sizeof(item));
            item.mask = LVIF_TEXT | LVIF_PARAM;
            item.iItem = row;
            item.pszText = pe.szExeFile;
            item.lParam  = (LPARAM)pe.th32ProcessID;
            SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&item);

            swprintf_s(pidStr, 16, L"%u", pe.th32ProcessID);
            item.mask = LVIF_TEXT;
            item.iSubItem = 1;
            item.pszText = pidStr;
            SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&item);

            swprintf_s(thrStr, 16, L"%u", pe.cntThreads);
            item.iSubItem = 2;
            item.pszText = thrStr;
            SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&item);

            ++row;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

static DWORD Proc_SelectedPID(HWND list)
{
    int sel = (int)SendMessageW(list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    LVITEMW it;
    if (sel < 0) return 0;
    ZeroMemory(&it, sizeof(it));
    it.mask = LVIF_PARAM;
    it.iItem = sel;
    SendMessageW(list, LVM_GETITEMW, 0, (LPARAM)&it);
    return (DWORD)it.lParam;
}

static void Proc_ShowContextMenu(HWND frame, ProcState *st, int x, int y)
{
    HMENU menu;
    DWORD pid;
    int cmd;

    pid = Proc_SelectedPID(st->list);
    menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (pid ? 0 : MF_GRAYED),
                IDM_TERMINATE, L"End Process");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_REFRESH, L"Refresh");

    cmd = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD,
                         x, y, 0, frame, NULL);
    DestroyMenu(menu);

    if (cmd == IDM_TERMINATE && pid) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (h) {
            if (!TerminateProcess(h, 1)) {
                MessageBoxW(frame, L"TerminateProcess failed.",
                            L"Process", MB_ICONWARNING);
            }
            CloseHandle(h);
            Proc_Refresh(st);
        } else {
            MessageBoxW(frame, L"OpenProcess failed (insufficient rights?).",
                        L"Process", MB_ICONWARNING);
        }
    } else if (cmd == IDM_REFRESH) {
        Proc_Refresh(st);
    }
}

static LRESULT CALLBACK Proc_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    ProcState *st = (ProcState *)GetPropW(hwnd, PROC_PROP);

    if (msg == WM_TIMER && st) {
        Proc_Refresh(st);
        return 0;
    }
    if (msg == WM_NOTIFY && st) {
        NMHDR *hdr = (NMHDR *)lp;
        if (hdr->idFrom == ID_LIST && hdr->code == NM_RCLICK) {
            POINT pt;
            GetCursorPos(&pt);
            Proc_ShowContextMenu(hwnd, st, pt.x, pt.y);
            return 0;
        }
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->list, 4, 32, w - 8, h - 36, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        KillTimer(hwnd, PROC_TIMER);
        free(st);
        RemovePropW(hwnd, PROC_PROP);
    }
    return CallWindowProcW(g_origProcFrame, hwnd, msg, wp, lp);
}

static HWND Procs_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    ProcState *st;
    INITCOMMONCONTROLSEX icc;
    LVCOLUMNW col;

    (void)self;
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Procs",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (ProcState *)calloc(1, sizeof(ProcState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
        4, 32, w - 8, h - 36, frame, (HMENU)(LONG_PTR)ID_LIST, hInstance, NULL);

    SendMessageW(st->list, LVM_SETEXTENDEDLISTVIEWSTYLE,
                 LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES,
                 LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = (LPWSTR)L"Image Name"; col.cx = 260;
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.pszText = (LPWSTR)L"PID";        col.cx = 80;
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    col.pszText = (LPWSTR)L"Threads";    col.cx = 80;
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);

    SetPropW(frame, PROC_PROP, (HANDLE)st);
    if (!g_origProcFrame)
        g_origProcFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Proc_FrameProc);

    Proc_Refresh(st);
    SetTimer(frame, PROC_TIMER, 2000, NULL);
    return frame;
}

MsApp g_AppProcs = {
    L"Procs",
    Procs_Create,
    520, 420
};
