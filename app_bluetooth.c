/*
 * app_bluetooth.c — Bluetooth device discovery and enumeration
 *
 * Demonstrates the Bluetooth user-mode API (bthprops.cpl exports via
 * BluetoothApis.h on modern systems, loaded dynamically here for
 * portability):
 *
 *   - BluetoothFindFirstDevice(&searchParams, &deviceInfo) starts a
 *     search using BLUETOOTH_DEVICE_SEARCH_PARAMS — fields specify
 *     whether to return authenticated, remembered, connected, unknown,
 *     and whether to issue a fresh inquiry
 *   - Each BLUETOOTH_DEVICE_INFO has the BTH_ADDR (6-byte MAC),
 *     szName (the friendly name), ulClassofDevice (device class),
 *     fConnected/fAuthenticated/fRemembered/fInquired flags, last seen
 *     and last used timestamps
 *   - BluetoothFindNextDevice walks the enumeration
 *   - BluetoothFindDeviceClose terminates the search handle
 *
 * Loaded dynamically: bthprops.cpl exports BluetoothFindFirstDevice etc.;
 * older systems / non-BT machines simply report unavailable.
 */

#include "shell.h"
#include <commctrl.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "comctl32.lib")

#ifndef BLUETOOTH_MAX_NAME_SIZE
#define BLUETOOTH_MAX_NAME_SIZE 248
#endif

typedef ULONGLONG BTH_ADDR;

typedef struct {
    DWORD     dwSize;
    BTH_ADDR  Address;
    ULONG     ulClassofDevice;
    BOOL      fConnected;
    BOOL      fRemembered;
    BOOL      fAuthenticated;
    SYSTEMTIME stLastSeen;
    SYSTEMTIME stLastUsed;
    WCHAR     szName[BLUETOOTH_MAX_NAME_SIZE];
} MS_BLUETOOTH_DEVICE_INFO;

typedef struct {
    DWORD  dwSize;
    BOOL   fReturnAuthenticated;
    BOOL   fReturnRemembered;
    BOOL   fReturnUnknown;
    BOOL   fReturnConnected;
    BOOL   fIssueInquiry;
    UCHAR  cTimeoutMultiplier;
    HANDLE hRadio;
} MS_BLUETOOTH_DEVICE_SEARCH_PARAMS;

typedef HANDLE (WINAPI *PFN_BluetoothFindFirstDevice)(MS_BLUETOOTH_DEVICE_SEARCH_PARAMS *, MS_BLUETOOTH_DEVICE_INFO *);
typedef BOOL (WINAPI *PFN_BluetoothFindNextDevice)(HANDLE, MS_BLUETOOTH_DEVICE_INFO *);
typedef BOOL (WINAPI *PFN_BluetoothFindDeviceClose)(HANDLE);

#define BT_PROP    L"MS_BT_STATE"
#define ID_BT_REF  114001
#define ID_BT_LV   114002

typedef struct {
    HWND     refresh, list;
    HMODULE  dll;
    PFN_BluetoothFindFirstDevice pFirst;
    PFN_BluetoothFindNextDevice  pNext;
    PFN_BluetoothFindDeviceClose pClose;
} BtState;

static WNDPROC g_origBtFrame = NULL;

static void Bt_FormatAddr(BTH_ADDR addr, wchar_t *out)
{
    BYTE b[8];
    int i;
    memcpy(b, &addr, sizeof(addr));
    /* BTH_ADDR is little-endian; print high-byte first as is conventional */
    swprintf_s(out, 32, L"%02X:%02X:%02X:%02X:%02X:%02X",
               b[5], b[4], b[3], b[2], b[1], b[0]);
    (void)i;
}

static void Bt_Refresh(BtState *st)
{
    MS_BLUETOOTH_DEVICE_SEARCH_PARAMS p;
    MS_BLUETOOTH_DEVICE_INFO info;
    HANDLE find;
    int idx = 0;

    SendMessageW(st->list, LVM_DELETEALLITEMS, 0, 0);

    if (!st->pFirst) {
        LVITEMW it;
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT;
        it.pszText = (LPWSTR)L"(BluetoothApis unavailable on this system)";
        SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        return;
    }

    ZeroMemory(&p, sizeof(p));
    p.dwSize = sizeof(p);
    p.fReturnAuthenticated = TRUE;
    p.fReturnRemembered    = TRUE;
    p.fReturnUnknown       = TRUE;
    p.fReturnConnected     = TRUE;
    p.fIssueInquiry        = FALSE;     /* Skip live scan to keep demo fast */
    p.cTimeoutMultiplier   = 1;
    p.hRadio               = NULL;

    ZeroMemory(&info, sizeof(info));
    info.dwSize = sizeof(info);

    find = st->pFirst(&p, &info);
    if (!find) {
        LVITEMW it;
        wchar_t buf[100];
        swprintf_s(buf, 100, L"(no remembered devices — GetLastError=%lu)", GetLastError());
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT;
        it.pszText = buf;
        SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        return;
    }

    do {
        wchar_t addr[32];
        wchar_t cls[16];
        wchar_t flags[60] = L"";
        LVITEMW it;
        Bt_FormatAddr(info.Address, addr);
        swprintf_s(cls, 16, L"0x%06lx", info.ulClassofDevice);
        if (info.fConnected)     wcscat_s(flags, 60, L"CONN ");
        if (info.fAuthenticated) wcscat_s(flags, 60, L"AUTH ");
        if (info.fRemembered)    wcscat_s(flags, 60, L"REM ");
        if (!flags[0]) wcscpy_s(flags, 60, L"-");

        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT; it.iItem = idx;
        it.pszText = info.szName[0] ? info.szName : (LPWSTR)L"(unnamed)";
        SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        it.iSubItem = 1; it.pszText = addr;  SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
        it.iSubItem = 2; it.pszText = cls;   SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
        it.iSubItem = 3; it.pszText = flags; SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);

        ++idx;
        ZeroMemory(&info, sizeof(info));
        info.dwSize = sizeof(info);
    } while (st->pNext && st->pNext(find, &info));

    if (st->pClose) st->pClose(find);
}

static LRESULT CALLBACK Bt_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    BtState *st = (BtState *)GetPropW(hwnd, BT_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_BT_REF) { Bt_Refresh(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->refresh, 12, 38, 130, 26, TRUE);
        MoveWindow(st->list,    8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->dll) FreeLibrary(st->dll);
        free(st); RemovePropW(hwnd, BT_PROP);
    }
    return CallWindowProcW(g_origBtFrame, hwnd, msg, wp, lp);
}

static HWND Bluetooth_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    BtState *st;
    INITCOMMONCONTROLSEX icc;
    LVCOLUMNW col;
    (void)self;

    icc.dwSize = sizeof(icc); icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Bluetooth",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (BtState *)calloc(1, sizeof(BtState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->dll = LoadLibraryW(L"bthprops.cpl");
    if (!st->dll) st->dll = LoadLibraryW(L"Irprops.cpl");
    if (st->dll) {
        st->pFirst = (PFN_BluetoothFindFirstDevice)GetProcAddress(st->dll, "BluetoothFindFirstDevice");
        st->pNext  = (PFN_BluetoothFindNextDevice) GetProcAddress(st->dll, "BluetoothFindNextDevice");
        st->pClose = (PFN_BluetoothFindDeviceClose)GetProcAddress(st->dll, "BluetoothFindDeviceClose");
    }

    st->refresh = CreateWindowExW(0, L"BUTTON", L"Refresh",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 130, 26, frame, (HMENU)(LONG_PTR)ID_BT_REF, hInstance, NULL);

    st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
        WS_CHILD | WS_VISIBLE | LVS_REPORT,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_BT_LV, hInstance, NULL);
    SendMessageW(st->list, LVM_SETEXTENDEDLISTVIEWSTYLE,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 280; col.pszText = (LPWSTR)L"Friendly name"; SendMessageW(st->list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.cx = 160; col.pszText = (LPWSTR)L"MAC";           SendMessageW(st->list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    col.cx = 110; col.pszText = (LPWSTR)L"Class";         SendMessageW(st->list, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);
    col.cx = 200; col.pszText = (LPWSTR)L"Flags";         SendMessageW(st->list, LVM_INSERTCOLUMNW, 3, (LPARAM)&col);

    SetPropW(frame, BT_PROP, (HANDLE)st);
    if (!g_origBtFrame) g_origBtFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Bt_FrameProc);
    Bt_Refresh(st);
    return frame;
}

MsApp g_AppBluetooth = { L"Bluetooth", Bluetooth_Create, 820, 420 };
