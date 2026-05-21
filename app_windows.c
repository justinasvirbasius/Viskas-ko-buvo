/*
 * app_windows.c — Enumerate top-level windows on the system
 *
 * Demonstrates:
 *   - EnumWindows with a callback that visits each top-level HWND
 *   - IsWindowVisible, GetWindowTextW, GetClassNameW, GetWindowThreadProcessId
 *   - Populating a ListView (report mode) with the results
 *   - Refresh button to re-enumerate; double-click to bring a window to front
 *     via SetForegroundWindow
 *
 * Only visible windows with non-empty titles are listed — the system has
 * hundreds of hidden/tool windows otherwise.
 */

#include "shell.h"
#include <commctrl.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "comctl32.lib")

#define WN_PROP    L"MS_WN_STATE"
#define ID_WN_LIST 41001
#define ID_WN_REF  41002

typedef struct {
    HWND list, refBtn;
} WnState;

typedef struct {
    HWND list;
    int  row;
} WnEnumCtx;

static WNDPROC g_origWnFrame = NULL;

static BOOL CALLBACK Wn_Enum(HWND hwnd, LPARAM lp)
{
    WnEnumCtx *ctx = (WnEnumCtx *)lp;
    wchar_t title[256], cls[128], pidStr[16], hwndStr[20];
    DWORD pid = 0;
    LVITEMW it;

    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindowTextLengthW(hwnd) == 0) return TRUE;

    GetWindowTextW(hwnd, title, 256);
    GetClassNameW(hwnd, cls, 128);
    GetWindowThreadProcessId(hwnd, &pid);
    swprintf_s(pidStr, 16, L"%lu", pid);
    swprintf_s(hwndStr, 20, L"0x%p", (void *)hwnd);

    ZeroMemory(&it, sizeof(it));
    it.mask = LVIF_TEXT | LVIF_PARAM;
    it.iItem = ctx->row;
    it.pszText = title;
    it.lParam  = (LPARAM)hwnd;
    SendMessageW(ctx->list, LVM_INSERTITEMW, 0, (LPARAM)&it);

    it.iSubItem = 1; it.pszText = cls;
    SendMessageW(ctx->list, LVM_SETITEMW, 0, (LPARAM)&it);
    it.iSubItem = 2; it.pszText = pidStr;
    SendMessageW(ctx->list, LVM_SETITEMW, 0, (LPARAM)&it);
    it.iSubItem = 3; it.pszText = hwndStr;
    SendMessageW(ctx->list, LVM_SETITEMW, 0, (LPARAM)&it);

    ++ctx->row;
    return TRUE;
}

static void Wn_Refresh(WnState *st)
{
    WnEnumCtx ctx;
    ctx.list = st->list;
    ctx.row = 0;
    SendMessageW(st->list, LVM_DELETEALLITEMS, 0, 0);
    EnumWindows(Wn_Enum, (LPARAM)&ctx);
}

static LRESULT CALLBACK Wn_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    WnState *st = (WnState *)GetPropW(hwnd, WN_PROP);

    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_WN_REF) {
        Wn_Refresh(st);
        return 0;
    }
    if (msg == WM_NOTIFY && st) {
        NMHDR *hdr = (NMHDR *)lp;
        if (hdr->idFrom == ID_WN_LIST && hdr->code == NM_DBLCLK) {
            int sel = (int)SendMessageW(st->list, LVM_GETNEXTITEM,
                                        (WPARAM)-1, LVNI_SELECTED);
            if (sel >= 0) {
                LVITEMW it;
                ZeroMemory(&it, sizeof(it));
                it.mask = LVIF_PARAM;
                it.iItem = sel;
                if (SendMessageW(st->list, LVM_GETITEMW, 0, (LPARAM)&it)) {
                    HWND target = (HWND)it.lParam;
                    if (IsWindow(target)) SetForegroundWindow(target);
                }
            }
            return 0;
        }
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->refBtn, 8, 34, 100, 24, TRUE);
        MoveWindow(st->list,   8, 64, w - 16, h - 72, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, WN_PROP); }
    return CallWindowProcW(g_origWnFrame, hwnd, msg, wp, lp);
}

static HWND Windows_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    WnState *st;
    INITCOMMONCONTROLSEX icc;
    LVCOLUMNW col;
    (void)self;

    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Windows",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (WnState *)calloc(1, sizeof(WnState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->refBtn = CreateWindowExW(0, L"BUTTON", L"Refresh",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        8, 34, 100, 24, frame, (HMENU)(LONG_PTR)ID_WN_REF, hInstance, NULL);

    st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
        8, 64, w - 16, h - 72, frame, (HMENU)(LONG_PTR)ID_WN_LIST, hInstance, NULL);
    SendMessageW(st->list, LVM_SETEXTENDEDLISTVIEWSTYLE,
                 LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES,
                 LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = (LPWSTR)L"Title"; col.cx = 280;
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.pszText = (LPWSTR)L"Class"; col.cx = 180;
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    col.pszText = (LPWSTR)L"PID";   col.cx = 70;
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);
    col.pszText = (LPWSTR)L"HWND";  col.cx = 120;
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 3, (LPARAM)&col);

    SetPropW(frame, WN_PROP, (HANDLE)st);
    if (!g_origWnFrame) g_origWnFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Wn_FrameProc);

    Wn_Refresh(st);
    return frame;
}

MsApp g_AppWindows = {
    L"Windows",
    Windows_Create,
    700, 460
};
