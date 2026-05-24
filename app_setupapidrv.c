/*
 * app_setupapidrv.c — Device driver introspection via SetupDi*
 *
 * Demonstrates the deeper SetupDi APIs — beyond the simple enumeration
 * shown by Devices (Batch 9), this app reads per-device *driver*
 * properties and per-device *registry* properties:
 *
 *   - SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT)
 *     gets a handle to a SP_DEVINFO_DATA collection of all installed
 *     present devices
 *   - SetupDiEnumDeviceInfo walks the collection
 *   - SetupDiGetDeviceRegistryPropertyW reads any of the SPDRP_* properties
 *     (FRIENDLYNAME, DEVICEDESC, HARDWAREID, COMPATIBLEIDS, CLASS, MFG, etc.)
 *   - SetupDiGetDeviceInstanceIdW returns the canonical instance path
 *     (e.g. "PCI\VEN_8086&DEV_..." or "USB\VID_...")
 *
 * Two-call sizing: pass cbBuf=0 → returns required size in &requiredSize,
 * then allocate and call again.
 */

#include "shell.h"
#include <setupapi.h>
#include <commctrl.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "comctl32.lib")

#define SD_PROP   L"MS_SD_STATE"
#define ID_SD_REF 117001
#define ID_SD_LV  117002

typedef struct { HWND refresh, list; } SdState;
static WNDPROC g_origSdFrame = NULL;

static BOOL Sd_GetProp(HDEVINFO h, SP_DEVINFO_DATA *did, DWORD prop, wchar_t *out, DWORD outBytes)
{
    DWORD required = 0;
    DWORD regType  = 0;
    BOOL ok;
    out[0] = 0;
    ok = SetupDiGetDeviceRegistryPropertyW(h, did, prop, &regType,
                                            (PBYTE)out, outBytes, &required);
    if (!ok && GetLastError() == ERROR_INSUFFICIENT_BUFFER && required <= outBytes) {
        ok = SetupDiGetDeviceRegistryPropertyW(h, did, prop, &regType,
                                                (PBYTE)out, outBytes, &required);
    }
    return ok;
}

static void Sd_Refresh(SdState *st)
{
    HDEVINFO h;
    SP_DEVINFO_DATA did;
    DWORD i;

    SendMessageW(st->list, LVM_DELETEALLITEMS, 0, 0);

    h = SetupDiGetClassDevsW(NULL, NULL, NULL,
                              DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (h == INVALID_HANDLE_VALUE) return;

    ZeroMemory(&did, sizeof(did));
    did.cbSize = sizeof(did);
    for (i = 0; SetupDiEnumDeviceInfo(h, i, &did); ++i) {
        wchar_t desc[256] = L"";
        wchar_t friendly[256] = L"";
        wchar_t mfg[128] = L"";
        wchar_t cls[64] = L"";
        wchar_t instId[256] = L"";
        DWORD   instLen = 256;
        wchar_t shown[256];
        LVITEMW it;

        Sd_GetProp(h, &did, SPDRP_DEVICEDESC,   desc,     sizeof(desc));
        Sd_GetProp(h, &did, SPDRP_FRIENDLYNAME, friendly, sizeof(friendly));
        Sd_GetProp(h, &did, SPDRP_MFG,          mfg,      sizeof(mfg));
        Sd_GetProp(h, &did, SPDRP_CLASS,        cls,      sizeof(cls));
        SetupDiGetDeviceInstanceIdW(h, &did, instId, 256, &instLen);

        wcscpy_s(shown, 256, friendly[0] ? friendly : (desc[0] ? desc : L"(no name)"));

        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT; it.iItem = (int)i;
        it.pszText = shown;  SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        it.iSubItem = 1; it.pszText = cls;    SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
        it.iSubItem = 2; it.pszText = mfg;    SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
        it.iSubItem = 3; it.pszText = instId; SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);

        if (i >= 600) break;  /* practical limit */
    }

    SetupDiDestroyDeviceInfoList(h);
}

static LRESULT CALLBACK Sd_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    SdState *st = (SdState *)GetPropW(hwnd, SD_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_SD_REF) { Sd_Refresh(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->refresh, 12, 38, 110, 26, TRUE);
        MoveWindow(st->list,    8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, SD_PROP); }
    return CallWindowProcW(g_origSdFrame, hwnd, msg, wp, lp);
}

static HWND SetupApiDrv_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    SdState *st;
    INITCOMMONCONTROLSEX icc;
    LVCOLUMNW col;
    (void)self;

    icc.dwSize = sizeof(icc); icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"SetupApiDrv",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (SdState *)calloc(1, sizeof(SdState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->refresh = CreateWindowExW(0, L"BUTTON", L"Refresh",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 110, 26, frame, (HMENU)(LONG_PTR)ID_SD_REF, hInstance, NULL);

    st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
        WS_CHILD | WS_VISIBLE | LVS_REPORT,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_SD_LV, hInstance, NULL);
    SendMessageW(st->list, LVM_SETEXTENDEDLISTVIEWSTYLE,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 280; col.pszText = (LPWSTR)L"Device";         SendMessageW(st->list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.cx = 140; col.pszText = (LPWSTR)L"Class";          SendMessageW(st->list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    col.cx = 200; col.pszText = (LPWSTR)L"Manufacturer";   SendMessageW(st->list, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);
    col.cx = 420; col.pszText = (LPWSTR)L"Instance ID";    SendMessageW(st->list, LVM_INSERTCOLUMNW, 3, (LPARAM)&col);

    SetPropW(frame, SD_PROP, (HANDLE)st);
    if (!g_origSdFrame) g_origSdFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Sd_FrameProc);
    Sd_Refresh(st);
    return frame;
}

MsApp g_AppSetupApiDrv = { L"SetupApiDrv", SetupApiDrv_Create, 1060, 480 };
