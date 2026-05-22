/*
 * app_devices.c — Enumerate Plug and Play devices via SetupAPI
 *
 * Demonstrates SetupDi*:
 *   - SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT)
 *     to get an HDEVINFO covering every present device
 *   - SetupDiEnumDeviceInfo iterating SP_DEVINFO_DATA records
 *   - SetupDiGetDeviceRegistryPropertyW(SPDRP_DEVICEDESC / SPDRP_CLASS /
 *     SPDRP_MFG / SPDRP_HARDWAREID) for human-readable info
 *   - SetupDiDestroyDeviceInfoList for cleanup
 *
 * The list typically has 200-500 entries on a modern PC. Filter pill lets the
 * user grep by device class (case-insensitive substring).
 */

#include "shell.h"
#include <commctrl.h>
#include <setupapi.h>
#include <devguid.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "comctl32.lib")

#define DV_PROP    L"MS_DV_STATE"
#define ID_DV_LV   56001
#define ID_DV_REF  56002
#define ID_DV_FILT 56003

typedef struct {
    HWND list, refBtn, filterEdit;
} DvState;

static WNDPROC g_origDvFrame = NULL;

static BOOL Dv_GetProp(HDEVINFO h, SP_DEVINFO_DATA *did, DWORD prop,
                        wchar_t *out, DWORD cb)
{
    DWORD type = 0;
    if (SetupDiGetDeviceRegistryPropertyW(h, did, prop, &type,
            (PBYTE)out, cb, NULL)) {
        return TRUE;
    }
    out[0] = 0;
    return FALSE;
}

static void Dv_Refresh(DvState *st)
{
    HDEVINFO h;
    SP_DEVINFO_DATA did;
    DWORD idx;
    wchar_t filter[80];

    SendMessageW(st->list, LVM_DELETEALLITEMS, 0, 0);
    GetWindowTextW(st->filterEdit, filter, 80);
    CharLowerW(filter);

    h = SetupDiGetClassDevsW(NULL, NULL, NULL,
            DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (h == INVALID_HANDLE_VALUE) return;

    did.cbSize = sizeof(did);
    for (idx = 0; SetupDiEnumDeviceInfo(h, idx, &did); ++idx) {
        wchar_t desc[200], cls[100], mfg[200];
        wchar_t lower[200];
        LVITEMW it;

        Dv_GetProp(h, &did, SPDRP_DEVICEDESC, desc, sizeof(desc));
        if (!desc[0]) Dv_GetProp(h, &did, SPDRP_FRIENDLYNAME, desc, sizeof(desc));
        Dv_GetProp(h, &did, SPDRP_CLASS,      cls,  sizeof(cls));
        Dv_GetProp(h, &did, SPDRP_MFG,        mfg,  sizeof(mfg));

        if (filter[0]) {
            int matched = 0;
            wcscpy_s(lower, 200, desc); CharLowerW(lower);
            if (wcsstr(lower, filter)) matched = 1;
            wcscpy_s(lower, 200, cls);  CharLowerW(lower);
            if (wcsstr(lower, filter)) matched = 1;
            wcscpy_s(lower, 200, mfg);  CharLowerW(lower);
            if (wcsstr(lower, filter)) matched = 1;
            if (!matched) continue;
        }

        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT;
        it.iItem = (int)SendMessageW(st->list, LVM_GETITEMCOUNT, 0, 0);
        it.pszText = desc;
        SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        it.iSubItem = 1; it.pszText = cls;
        SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
        it.iSubItem = 2; it.pszText = mfg;
        SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
    }
    SetupDiDestroyDeviceInfoList(h);
}

static LRESULT CALLBACK Dv_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    DvState *st = (DvState *)GetPropW(hwnd, DV_PROP);

    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_DV_REF) { Dv_Refresh(st); return 0; }
        if (LOWORD(wp) == ID_DV_FILT && HIWORD(wp) == EN_CHANGE) {
            Dv_Refresh(st);
            return 0;
        }
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->refBtn,     8, 34, 90, 24, TRUE);
        MoveWindow(st->filterEdit, 106, 34, w - 114, 24, TRUE);
        MoveWindow(st->list,       8, 64, w - 16, h - 72, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, DV_PROP); }
    return CallWindowProcW(g_origDvFrame, hwnd, msg, wp, lp);
}

static HWND Devices_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    DvState *st;
    INITCOMMONCONTROLSEX icc;
    LVCOLUMNW col;
    (void)self;

    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Devices",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (DvState *)calloc(1, sizeof(DvState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->refBtn = CreateWindowExW(0, L"BUTTON", L"Refresh",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        8, 34, 90, 24, frame, (HMENU)(LONG_PTR)ID_DV_REF, hInstance, NULL);
    st->filterEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        106, 34, w - 114, 24, frame, (HMENU)(LONG_PTR)ID_DV_FILT, hInstance, NULL);

    st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
        WS_CHILD | WS_VISIBLE | LVS_REPORT,
        8, 64, w - 16, h - 72, frame, (HMENU)(LONG_PTR)ID_DV_LV, hInstance, NULL);
    SendMessageW(st->list, LVM_SETEXTENDEDLISTVIEWSTYLE,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 280; col.pszText = (LPWSTR)L"Description";
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.cx = 140; col.pszText = (LPWSTR)L"Class";
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    col.cx = 200; col.pszText = (LPWSTR)L"Manufacturer";
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);

    SetPropW(frame, DV_PROP, (HANDLE)st);
    if (!g_origDvFrame) g_origDvFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Dv_FrameProc);
    Dv_Refresh(st);
    return frame;
}

MsApp g_AppDevices = {
    L"Devices",
    Devices_Create,
    760, 460
};
