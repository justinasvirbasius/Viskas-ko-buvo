/*
 * app_msilist.c — Enumerate installed MSI products
 *
 * Demonstrates Windows Installer (msi.dll) product enumeration — the
 * authoritative source for "what MSI packages are installed":
 *   - MsiEnumProductsW(index, productCode) — walks every installed product;
 *     productCode is a 39-char GUID string (38 chars + NUL)
 *   - MsiGetProductInfoW(productCode, property, buf, &cch) — reads a
 *     property (Name, Publisher, Version, InstallDate, InstallLocation,
 *     InstallSource, etc.)
 *   - Two-call pattern: first call with cch=0 returns ERROR_MORE_DATA
 *     and the needed size; second call retrieves the value.
 *
 * Properties pulled: INSTALLPROPERTY_INSTALLEDPRODUCTNAME, _PUBLISHER,
 * _VERSIONSTRING, _INSTALLDATE.
 */

#include "shell.h"
#include <msi.h>
#include <commctrl.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "msi.lib")
#pragma comment(lib, "comctl32.lib")

#define ML_PROP   L"MS_ML_STATE"
#define ID_ML_REF 94001
#define ID_ML_LV  94002

typedef struct { HWND refresh, list; } MlState;
static WNDPROC g_origMlFrame = NULL;

static void Ml_GetProp(LPCWSTR product, LPCWSTR prop, wchar_t *out, DWORD outCch)
{
    DWORD cch = outCch;
    UINT r = MsiGetProductInfoW(product, prop, out, &cch);
    if (r != ERROR_SUCCESS) wcscpy_s(out, outCch, L"");
}

static void Ml_Refresh(MlState *st)
{
    DWORD i = 0;
    wchar_t product[40];

    SendMessageW(st->list, LVM_DELETEALLITEMS, 0, 0);

    while (MsiEnumProductsW(i, product) == ERROR_SUCCESS) {
        wchar_t name[256], publisher[256], version[64], installDate[32];
        LVITEMW it;

        Ml_GetProp(product, INSTALLPROPERTY_INSTALLEDPRODUCTNAME, name, 256);
        Ml_GetProp(product, INSTALLPROPERTY_PUBLISHER,            publisher, 256);
        Ml_GetProp(product, INSTALLPROPERTY_VERSIONSTRING,        version, 64);
        Ml_GetProp(product, INSTALLPROPERTY_INSTALLDATE,          installDate, 32);

        if (!name[0]) wcscpy_s(name, 256, L"(unnamed)");

        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT; it.iItem = (int)i;
        it.pszText = name;          SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        it.iSubItem = 1; it.pszText = publisher;   SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
        it.iSubItem = 2; it.pszText = version;     SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
        it.iSubItem = 3; it.pszText = installDate; SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
        it.iSubItem = 4; it.pszText = product;     SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);

        ++i;
    }
}

static LRESULT CALLBACK Ml_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    MlState *st = (MlState *)GetPropW(hwnd, ML_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_ML_REF) { Ml_Refresh(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->refresh, 12, 38, 110, 26, TRUE);
        MoveWindow(st->list,    8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, ML_PROP); }
    return CallWindowProcW(g_origMlFrame, hwnd, msg, wp, lp);
}

static HWND MsiList_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    MlState *st;
    INITCOMMONCONTROLSEX icc;
    LVCOLUMNW col;
    (void)self;

    icc.dwSize = sizeof(icc); icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"MsiList",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (MlState *)calloc(1, sizeof(MlState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->refresh = CreateWindowExW(0, L"BUTTON", L"Refresh",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 110, 26, frame, (HMENU)(LONG_PTR)ID_ML_REF, hInstance, NULL);

    st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
        WS_CHILD | WS_VISIBLE | LVS_REPORT,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_ML_LV, hInstance, NULL);
    SendMessageW(st->list, LVM_SETEXTENDEDLISTVIEWSTYLE,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 280; col.pszText = (LPWSTR)L"Product";       SendMessageW(st->list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.cx = 200; col.pszText = (LPWSTR)L"Publisher";     SendMessageW(st->list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    col.cx = 110; col.pszText = (LPWSTR)L"Version";       SendMessageW(st->list, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);
    col.cx =  90; col.pszText = (LPWSTR)L"Installed";     SendMessageW(st->list, LVM_INSERTCOLUMNW, 3, (LPARAM)&col);
    col.cx = 260; col.pszText = (LPWSTR)L"Product code";  SendMessageW(st->list, LVM_INSERTCOLUMNW, 4, (LPARAM)&col);

    SetPropW(frame, ML_PROP, (HANDLE)st);
    if (!g_origMlFrame) g_origMlFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Ml_FrameProc);
    Ml_Refresh(st);
    return frame;
}

MsApp g_AppMsiList = { L"MsiList", MsiList_Create, 940, 460 };
