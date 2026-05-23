/*
 * app_printenum.c — Enumerate installed printers
 *
 * Demonstrates the spooler enumeration API in winspool.drv:
 *   - EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, NULL,
 *                    2 /* level 2 */, NULL, 0, &cb, &returned) sizes
 *   - Second call writes PRINTER_INFO_2W records into the buffer
 *   - Per-printer: pPrinterName, pDriverName, pPortName, pServerName,
 *     pComment, pLocation, Status (bitmask), Attributes (bitmask)
 *   - GetDefaultPrinterW for the default
 *
 * winspool is a separate import library; we link winspool.lib.
 *
 * Output to a ListView with sortable columns.
 */

#include "shell.h"
#include <commctrl.h>
#include <winspool.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winspool.lib")

#define PE_PROP    L"MS_PE_STATE"
#define ID_PE_LV   67001
#define ID_PE_REF  67002
#define ID_PE_DEF  67003

typedef struct { HWND list, refBtn, defLbl; } PeState;
static WNDPROC g_origPeFrame = NULL;

static void Pe_StatusText(DWORD s, wchar_t *out, int cch)
{
    if (s == 0) { wcscpy_s(out, cch, L"ready"); return; }
    out[0] = 0;
    if (s & PRINTER_STATUS_PAUSED)         wcscat_s(out, cch, L"paused ");
    if (s & PRINTER_STATUS_ERROR)          wcscat_s(out, cch, L"error ");
    if (s & PRINTER_STATUS_PENDING_DELETION) wcscat_s(out, cch, L"pending-deletion ");
    if (s & PRINTER_STATUS_PAPER_JAM)      wcscat_s(out, cch, L"jam ");
    if (s & PRINTER_STATUS_PAPER_OUT)      wcscat_s(out, cch, L"paper-out ");
    if (s & PRINTER_STATUS_OFFLINE)        wcscat_s(out, cch, L"offline ");
    if (s & PRINTER_STATUS_BUSY)           wcscat_s(out, cch, L"busy ");
    if (s & PRINTER_STATUS_PRINTING)       wcscat_s(out, cch, L"printing ");
    if (s & PRINTER_STATUS_TONER_LOW)      wcscat_s(out, cch, L"toner-low ");
    if (out[0] == 0) swprintf_s(out, cch, L"0x%lx", s);
}

static void Pe_Refresh(PeState *st)
{
    DWORD cb = 0, returned = 0;
    BYTE *buf = NULL;
    wchar_t defName[256] = L"<none>";
    DWORD defCch = 256;

    SendMessageW(st->list, LVM_DELETEALLITEMS, 0, 0);

    if (GetDefaultPrinterW(defName, &defCch)) {
        wchar_t lbl[300];
        swprintf_s(lbl, 300, L"Default printer: %s", defName);
        SetWindowTextW(st->defLbl, lbl);
    } else {
        SetWindowTextW(st->defLbl, L"Default printer: <none>");
    }

    EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS,
                  NULL, 2, NULL, 0, &cb, &returned);
    if (cb == 0) return;
    buf = (BYTE *)malloc(cb);
    if (!buf) return;
    if (!EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS,
                       NULL, 2, buf, cb, &cb, &returned)) {
        free(buf);
        return;
    }
    {
        PRINTER_INFO_2W *info = (PRINTER_INFO_2W *)buf;
        DWORD i;
        for (i = 0; i < returned; ++i) {
            LVITEMW it;
            wchar_t status[120];
            Pe_StatusText(info[i].Status, status, 120);

            ZeroMemory(&it, sizeof(it));
            it.mask = LVIF_TEXT;
            it.iItem = (int)i;
            it.pszText = info[i].pPrinterName ? info[i].pPrinterName : L"";
            SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);

            it.iSubItem = 1; it.pszText = info[i].pDriverName ? info[i].pDriverName : L"";
            SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
            it.iSubItem = 2; it.pszText = info[i].pPortName ? info[i].pPortName : L"";
            SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
            it.iSubItem = 3; it.pszText = status;
            SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
            it.iSubItem = 4; it.pszText = info[i].pLocation ? info[i].pLocation : L"";
            SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
        }
    }
    free(buf);
}

static LRESULT CALLBACK Pe_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    PeState *st = (PeState *)GetPropW(hwnd, PE_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_PE_REF) { Pe_Refresh(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->refBtn, 8, 34, 100, 24, TRUE);
        MoveWindow(st->defLbl, 116, 38, w - 124, 22, TRUE);
        MoveWindow(st->list,   8, 70, w - 16, h - 78, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, PE_PROP); }
    return CallWindowProcW(g_origPeFrame, hwnd, msg, wp, lp);
}

static HWND PrintEnum_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    PeState *st;
    INITCOMMONCONTROLSEX icc;
    LVCOLUMNW col;
    (void)self;

    icc.dwSize = sizeof(icc); icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"PrintEnum",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (PeState *)calloc(1, sizeof(PeState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->refBtn = CreateWindowExW(0, L"BUTTON", L"Refresh",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        8, 34, 100, 24, frame, (HMENU)(LONG_PTR)ID_PE_REF, hInstance, NULL);
    st->defLbl = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE,
        116, 38, w - 124, 22, frame, (HMENU)(LONG_PTR)ID_PE_DEF, hInstance, NULL);

    st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
        WS_CHILD | WS_VISIBLE | LVS_REPORT,
        8, 70, w - 16, h - 78, frame, (HMENU)(LONG_PTR)ID_PE_LV, hInstance, NULL);
    SendMessageW(st->list, LVM_SETEXTENDEDLISTVIEWSTYLE,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 220; col.pszText = (LPWSTR)L"Name";
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.cx = 180; col.pszText = (LPWSTR)L"Driver";
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    col.cx = 100; col.pszText = (LPWSTR)L"Port";
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);
    col.cx = 140; col.pszText = (LPWSTR)L"Status";
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 3, (LPARAM)&col);
    col.cx = 180; col.pszText = (LPWSTR)L"Location";
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 4, (LPARAM)&col);

    SetPropW(frame, PE_PROP, (HANDLE)st);
    if (!g_origPeFrame) g_origPeFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Pe_FrameProc);
    Pe_Refresh(st);
    return frame;
}

MsApp g_AppPrintEnum = { L"PrintEnum", PrintEnum_Create, 820, 440 };
