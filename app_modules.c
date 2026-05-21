/*
 * app_modules.c — Enumerate modules (DLLs) loaded in a process
 *
 * Demonstrates the Toolhelp32 module API, complementing app_procs.c which
 * only enumerates processes:
 *   - CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid)
 *   - Module32FirstW / Module32NextW iterating MODULEENTRY32W records
 *   - Per-module info: szModule, szExePath, modBaseAddr, modBaseSize
 *
 * The top combo lists running processes (also via Toolhelp); selecting one
 * snapshots its module list into a ListView.
 *
 * Note: enumerating modules of another process requires sufficient access;
 * many system processes will refuse without elevation. Our own process
 * always works as a demo.
 */

#include "shell.h"
#include <commctrl.h>
#include <tlhelp32.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "comctl32.lib")

#define MO_PROP    L"MS_MO_STATE"
#define ID_MO_CMB  50001
#define ID_MO_LV   50002
#define ID_MO_REF  50003

typedef struct {
    HWND combo, list, refBtn;
} MoState;

static WNDPROC g_origMoFrame = NULL;

static void Mo_FillProcesses(MoState *st)
{
    HANDLE snap;
    PROCESSENTRY32W pe;
    int idx;
    DWORD selPid = 0;
    int selIdx = -1;
    DWORD myPid = GetCurrentProcessId();

    SendMessageW(st->combo, CB_RESETCONTENT, 0, 0);
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        idx = 0;
        do {
            wchar_t label[300];
            int n;
            swprintf_s(label, 300, L"[%lu] %s", pe.th32ProcessID, pe.szExeFile);
            n = (int)SendMessageW(st->combo, CB_ADDSTRING, 0, (LPARAM)label);
            SendMessageW(st->combo, CB_SETITEMDATA, n, (LPARAM)pe.th32ProcessID);
            if (pe.th32ProcessID == myPid) selIdx = idx;
            ++idx;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (selIdx >= 0) SendMessageW(st->combo, CB_SETCURSEL, selIdx, 0);
    (void)selPid;
}

static void Mo_FillModulesFor(MoState *st, DWORD pid)
{
    HANDLE snap;
    MODULEENTRY32W me;
    int row = 0;

    SendMessageW(st->list, LVM_DELETEALLITEMS, 0, 0);
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) {
        LVITEMW it;
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT;
        it.iItem = 0;
        it.pszText = (LPWSTR)L"(snapshot failed — access denied?)";
        SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        return;
    }
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            LVITEMW it;
            wchar_t base[24], size[24];
            swprintf_s(base, 24, L"0x%p", (void *)me.modBaseAddr);
            swprintf_s(size, 24, L"%lu KB", me.modBaseSize / 1024);

            ZeroMemory(&it, sizeof(it));
            it.mask = LVIF_TEXT;
            it.iItem = row;
            it.pszText = me.szModule;
            SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
            it.iSubItem = 1; it.pszText = base;
            SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
            it.iSubItem = 2; it.pszText = size;
            SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
            it.iSubItem = 3; it.pszText = me.szExePath;
            SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
            ++row;
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
}

static void Mo_RefreshForCombo(MoState *st)
{
    int sel = (int)SendMessageW(st->combo, CB_GETCURSEL, 0, 0);
    DWORD pid;
    if (sel == CB_ERR) return;
    pid = (DWORD)SendMessageW(st->combo, CB_GETITEMDATA, sel, 0);
    Mo_FillModulesFor(st, pid);
}

static LRESULT CALLBACK Mo_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    MoState *st = (MoState *)GetPropW(hwnd, MO_PROP);

    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_MO_REF) {
            Mo_FillProcesses(st);
            Mo_RefreshForCombo(st);
            return 0;
        }
        if (LOWORD(wp) == ID_MO_CMB && HIWORD(wp) == CBN_SELCHANGE) {
            Mo_RefreshForCombo(st);
            return 0;
        }
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->combo,  8, 34, w - 130, 24, TRUE);
        MoveWindow(st->refBtn, w - 116, 34, 100, 24, TRUE);
        MoveWindow(st->list,   8, 66, w - 16, h - 74, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, MO_PROP); }
    return CallWindowProcW(g_origMoFrame, hwnd, msg, wp, lp);
}

static HWND Modules_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    MoState *st;
    INITCOMMONCONTROLSEX icc;
    LVCOLUMNW col;
    (void)self;

    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Modules",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (MoState *)calloc(1, sizeof(MoState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->combo = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        8, 34, w - 130, 200, frame, (HMENU)(LONG_PTR)ID_MO_CMB, hInstance, NULL);
    st->refBtn = CreateWindowExW(0, L"BUTTON", L"Refresh",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        w - 116, 34, 100, 24, frame, (HMENU)(LONG_PTR)ID_MO_REF, hInstance, NULL);

    st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
        8, 66, w - 16, h - 74, frame, (HMENU)(LONG_PTR)ID_MO_LV, hInstance, NULL);
    SendMessageW(st->list, LVM_SETEXTENDEDLISTVIEWSTYLE,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 170; col.pszText = (LPWSTR)L"Module";
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.cx = 130; col.pszText = (LPWSTR)L"Base";
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    col.cx =  80; col.pszText = (LPWSTR)L"Size";
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);
    col.cx = 320; col.pszText = (LPWSTR)L"Path";
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 3, (LPARAM)&col);

    SetPropW(frame, MO_PROP, (HANDLE)st);
    if (!g_origMoFrame) g_origMoFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Mo_FrameProc);

    Mo_FillProcesses(st);
    Mo_RefreshForCombo(st);
    return frame;
}

MsApp g_AppModules = {
    L"Modules",
    Modules_Create,
    760, 440
};
