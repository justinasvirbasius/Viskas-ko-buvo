/*
 * app_hid.c — Raw HID device enumeration via hid.dll + setupapi
 *
 * Demonstrates the HID API — beneath the unified pointer messages
 * (PtrInput, Batch 15) and Raw Input (RawInput, Batch 6), Windows
 * presents every HID-class device through hidsdi.h primitives. This is
 * the surface custom-device tools speak to read PSI VIDs/PIDs, usage
 * pages, and feature reports:
 *
 *   - HidD_GetHidGuid(&guid) — the device-interface GUID for HID class
 *   - SetupDiGetClassDevsW(&guid, NULL, NULL, DIGCF_PRESENT|DIGCF_DEVICEINTERFACE)
 *     enumerates present HID devices
 *   - SetupDiEnumDeviceInterfaces / SetupDiGetDeviceInterfaceDetailW
 *     yields the device-path file name (\\?\HID#...)
 *   - CreateFileW(devicePath, ...) opens the device
 *   - HidD_GetAttributes(h, &HIDD_ATTRIBUTES) returns VendorID,
 *     ProductID, VersionNumber
 *   - HidD_GetPreparsedData(h, &PHIDP_PREPARSED_DATA) returns an opaque
 *     parser for the report descriptor
 *   - HidP_GetCaps(parsed, &HIDP_CAPS) returns the device's UsagePage,
 *     Usage, and report sizes
 *   - HidD_GetManufacturerString / HidD_GetProductString optionally
 *     return display strings
 *
 * Loaded dynamically because hid.dll is not always present at link
 * time on certain SDKs.
 */

#include "shell.h"
#include <setupapi.h>
#include <commctrl.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "comctl32.lib")

typedef struct {
    USHORT Size;
    USHORT VendorID;
    USHORT ProductID;
    USHORT VersionNumber;
} MS_HIDD_ATTRIBUTES;

typedef void *MS_PHIDP_PREPARSED_DATA;

typedef struct {
    USHORT Usage;
    USHORT UsagePage;
    USHORT InputReportByteLength;
    USHORT OutputReportByteLength;
    USHORT FeatureReportByteLength;
    USHORT Reserved[17];
    USHORT NumberLinkCollectionNodes;
    USHORT NumberInputButtonCaps;
    USHORT NumberInputValueCaps;
    USHORT NumberInputDataIndices;
    USHORT NumberOutputButtonCaps;
    USHORT NumberOutputValueCaps;
    USHORT NumberOutputDataIndices;
    USHORT NumberFeatureButtonCaps;
    USHORT NumberFeatureValueCaps;
    USHORT NumberFeatureDataIndices;
} MS_HIDP_CAPS;

typedef void    (WINAPI *PFN_HidD_GetHidGuid)(LPGUID);
typedef BOOLEAN (WINAPI *PFN_HidD_GetAttributes)(HANDLE, MS_HIDD_ATTRIBUTES *);
typedef BOOLEAN (WINAPI *PFN_HidD_GetPreparsedData)(HANDLE, MS_PHIDP_PREPARSED_DATA *);
typedef BOOLEAN (WINAPI *PFN_HidD_FreePreparsedData)(MS_PHIDP_PREPARSED_DATA);
typedef LONG    (WINAPI *PFN_HidP_GetCaps)(MS_PHIDP_PREPARSED_DATA, MS_HIDP_CAPS *);
typedef BOOLEAN (WINAPI *PFN_HidD_GetProductString)(HANDLE, PVOID, ULONG);
typedef BOOLEAN (WINAPI *PFN_HidD_GetManufacturerString)(HANDLE, PVOID, ULONG);

#define HD_PROP   L"MS_HD_STATE"
#define ID_HD_REF 124001
#define ID_HD_LV  124002

typedef struct {
    HWND     refresh, list;
    HMODULE  hidDll;
    PFN_HidD_GetHidGuid           pGetHidGuid;
    PFN_HidD_GetAttributes        pGetAttributes;
    PFN_HidD_GetPreparsedData     pGetPreparsedData;
    PFN_HidD_FreePreparsedData    pFreePreparsedData;
    PFN_HidP_GetCaps              pGetCaps;
    PFN_HidD_GetProductString     pGetProductString;
    PFN_HidD_GetManufacturerString pGetMfgString;
} HdState;
static WNDPROC g_origHdFrame = NULL;

static void Hd_Refresh(HdState *st)
{
    GUID hidGuid;
    HDEVINFO h;
    SP_DEVICE_INTERFACE_DATA ifd;
    DWORD i;

    SendMessageW(st->list, LVM_DELETEALLITEMS, 0, 0);
    if (!st->pGetHidGuid) {
        LVITEMW it;
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT;
        it.pszText = (LPWSTR)L"(hid.dll unavailable)";
        SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        return;
    }

    st->pGetHidGuid(&hidGuid);

    h = SetupDiGetClassDevsW(&hidGuid, NULL, NULL,
                              DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (h == INVALID_HANDLE_VALUE) return;

    ZeroMemory(&ifd, sizeof(ifd));
    ifd.cbSize = sizeof(ifd);
    for (i = 0; SetupDiEnumDeviceInterfaces(h, NULL, &hidGuid, i, &ifd); ++i) {
        DWORD needed = 0;
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail = NULL;
        HANDLE dev = INVALID_HANDLE_VALUE;
        MS_HIDD_ATTRIBUTES attr;
        MS_HIDP_CAPS caps;
        MS_PHIDP_PREPARSED_DATA pd = NULL;
        WCHAR product[200] = L"";
        WCHAR mfg[200] = L"";
        WCHAR vidPid[40] = L"";
        WCHAR usage[60] = L"";
        LVITEMW it;

        SetupDiGetDeviceInterfaceDetailW(h, &ifd, NULL, 0, &needed, NULL);
        if (!needed) continue;
        detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)calloc(1, needed);
        if (!detail) continue;
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(h, &ifd, detail, needed, NULL, NULL)) {
            free(detail); continue;
        }

        dev = CreateFileW(detail->DevicePath,
                          GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          NULL, OPEN_EXISTING, 0, NULL);
        if (dev != INVALID_HANDLE_VALUE) {
            attr.Size = sizeof(attr);
            if (st->pGetAttributes && st->pGetAttributes(dev, &attr)) {
                swprintf_s(vidPid, 40, L"%04X:%04X v%04X",
                           attr.VendorID, attr.ProductID, attr.VersionNumber);
            }
            if (st->pGetPreparsedData && st->pGetPreparsedData(dev, &pd) && pd) {
                if (st->pGetCaps && st->pGetCaps(pd, &caps) == 0x110000 /* HIDP_STATUS_SUCCESS */) {
                    swprintf_s(usage, 60, L"page 0x%04X / usage 0x%04X",
                               caps.UsagePage, caps.Usage);
                }
                if (st->pFreePreparsedData) st->pFreePreparsedData(pd);
            }
            if (st->pGetProductString) st->pGetProductString(dev, product, sizeof(product));
            if (st->pGetMfgString)     st->pGetMfgString(dev, mfg, sizeof(mfg));
            CloseHandle(dev);
        }

        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT; it.iItem = (int)i;
        it.pszText = product[0] ? product : (LPWSTR)L"(no name)";
        SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        it.iSubItem = 1; it.pszText = mfg;    SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
        it.iSubItem = 2; it.pszText = vidPid; SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
        it.iSubItem = 3; it.pszText = usage;  SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);

        free(detail);
        if (i >= 200) break;
    }

    SetupDiDestroyDeviceInfoList(h);
}

static LRESULT CALLBACK Hd_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    HdState *st = (HdState *)GetPropW(hwnd, HD_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_HD_REF) { Hd_Refresh(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->refresh, 12, 38, 110, 26, TRUE);
        MoveWindow(st->list,    8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->hidDll) FreeLibrary(st->hidDll);
        free(st); RemovePropW(hwnd, HD_PROP);
    }
    return CallWindowProcW(g_origHdFrame, hwnd, msg, wp, lp);
}

static HWND Hid_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    HdState *st;
    INITCOMMONCONTROLSEX icc;
    LVCOLUMNW col;
    (void)self;

    icc.dwSize = sizeof(icc); icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Hid",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (HdState *)calloc(1, sizeof(HdState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->hidDll = LoadLibraryW(L"hid.dll");
    if (st->hidDll) {
        st->pGetHidGuid        = (PFN_HidD_GetHidGuid)        GetProcAddress(st->hidDll, "HidD_GetHidGuid");
        st->pGetAttributes     = (PFN_HidD_GetAttributes)     GetProcAddress(st->hidDll, "HidD_GetAttributes");
        st->pGetPreparsedData  = (PFN_HidD_GetPreparsedData)  GetProcAddress(st->hidDll, "HidD_GetPreparsedData");
        st->pFreePreparsedData = (PFN_HidD_FreePreparsedData) GetProcAddress(st->hidDll, "HidD_FreePreparsedData");
        st->pGetCaps           = (PFN_HidP_GetCaps)           GetProcAddress(st->hidDll, "HidP_GetCaps");
        st->pGetProductString  = (PFN_HidD_GetProductString)  GetProcAddress(st->hidDll, "HidD_GetProductString");
        st->pGetMfgString      = (PFN_HidD_GetManufacturerString) GetProcAddress(st->hidDll, "HidD_GetManufacturerString");
    }

    st->refresh = CreateWindowExW(0, L"BUTTON", L"Refresh",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 110, 26, frame, (HMENU)(LONG_PTR)ID_HD_REF, hInstance, NULL);

    st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
        WS_CHILD | WS_VISIBLE | LVS_REPORT,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_HD_LV, hInstance, NULL);
    SendMessageW(st->list, LVM_SETEXTENDEDLISTVIEWSTYLE,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 220; col.pszText = (LPWSTR)L"Product";       SendMessageW(st->list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.cx = 200; col.pszText = (LPWSTR)L"Manufacturer";  SendMessageW(st->list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    col.cx = 160; col.pszText = (LPWSTR)L"VID:PID";       SendMessageW(st->list, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);
    col.cx = 240; col.pszText = (LPWSTR)L"HID usage";     SendMessageW(st->list, LVM_INSERTCOLUMNW, 3, (LPARAM)&col);

    SetPropW(frame, HD_PROP, (HANDLE)st);
    if (!g_origHdFrame) g_origHdFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Hd_FrameProc);
    Hd_Refresh(st);
    return frame;
}

MsApp g_AppHid = { L"Hid", Hid_Create, 880, 440 };
