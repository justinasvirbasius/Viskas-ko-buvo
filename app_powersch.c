/*
 * app_powersch.c — Power scheme enumeration via powrprof
 *
 * Demonstrates the power-profile API (powrprof.dll):
 *   - PowerEnumerate(NULL, NULL, NULL, ACCESS_SCHEME, index, buf, &cbSize)
 *     walks the GUIDs of installed power schemes, returning a GUID per call;
 *     returns ERROR_NO_MORE_ITEMS at the end
 *   - PowerReadFriendlyName(NULL, &schemeGuid, NULL, NULL, buf, &cbSize)
 *     returns the localized display name for a scheme
 *   - PowerReadDescription likewise for the description
 *   - PowerGetActiveScheme(NULL, &activeGuid) reports which scheme is in use
 *
 * The buffer-size convention is "first call with NULL/0 returns required
 * bytes via ERROR_MORE_DATA" similar to many other Win32 APIs.
 */

#include "shell.h"
#include <powrprof.h>
#include <commctrl.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "comctl32.lib")

#define PW_PROP   L"MS_PW_STATE"
#define ID_PW_REF 99001
#define ID_PW_LV  99002

typedef struct { HWND refresh, list; } PwState;
static WNDPROC g_origPwFrame = NULL;

static void Pw_Refresh(PwState *st)
{
    DWORD index = 0;
    GUID  schemeGuid;
    DWORD bufSize;
    GUID  *activeGuid = NULL;
    GUID  active;
    BOOL  haveActive = FALSE;

    SendMessageW(st->list, LVM_DELETEALLITEMS, 0, 0);

    if (PowerGetActiveScheme(NULL, &activeGuid) == ERROR_SUCCESS && activeGuid) {
        active = *activeGuid;
        haveActive = TRUE;
        LocalFree(activeGuid);
    }

    for (;;) {
        DWORD r;
        wchar_t name[256] = L"";
        wchar_t desc[512] = L"";
        wchar_t guidStr[64];
        wchar_t isActive[8];
        LVITEMW it;

        bufSize = sizeof(schemeGuid);
        r = PowerEnumerate(NULL, NULL, NULL, ACCESS_SCHEME,
                            index, (UCHAR *)&schemeGuid, &bufSize);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) break;

        bufSize = sizeof(name);
        PowerReadFriendlyName(NULL, &schemeGuid, NULL, NULL,
                               (UCHAR *)name, &bufSize);
        bufSize = sizeof(desc);
        PowerReadDescription(NULL, &schemeGuid, NULL, NULL,
                              (UCHAR *)desc, &bufSize);
        StringFromGUID2(&schemeGuid, guidStr, 64);
        wcscpy_s(isActive, 8, (haveActive && IsEqualGUID(&active, &schemeGuid))
                                ? L"*" : L"");

        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT; it.iItem = (int)index;
        it.pszText = isActive; SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        it.iSubItem = 1; it.pszText = name;    SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
        it.iSubItem = 2; it.pszText = desc;    SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
        it.iSubItem = 3; it.pszText = guidStr; SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
        ++index;
    }
}

static LRESULT CALLBACK Pw_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    PwState *st = (PwState *)GetPropW(hwnd, PW_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_PW_REF) { Pw_Refresh(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->refresh, 12, 38, 110, 26, TRUE);
        MoveWindow(st->list,    8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, PW_PROP); }
    return CallWindowProcW(g_origPwFrame, hwnd, msg, wp, lp);
}

static HWND PowerSch_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    PwState *st;
    INITCOMMONCONTROLSEX icc;
    LVCOLUMNW col;
    (void)self;

    icc.dwSize = sizeof(icc); icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"PowerSch",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (PwState *)calloc(1, sizeof(PwState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->refresh = CreateWindowExW(0, L"BUTTON", L"Refresh",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 110, 26, frame, (HMENU)(LONG_PTR)ID_PW_REF, hInstance, NULL);

    st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
        WS_CHILD | WS_VISIBLE | LVS_REPORT,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_PW_LV, hInstance, NULL);
    SendMessageW(st->list, LVM_SETEXTENDEDLISTVIEWSTYLE,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx =  40; col.pszText = (LPWSTR)L"*";            SendMessageW(st->list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.cx = 220; col.pszText = (LPWSTR)L"Scheme";        SendMessageW(st->list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    col.cx = 360; col.pszText = (LPWSTR)L"Description";   SendMessageW(st->list, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);
    col.cx = 280; col.pszText = (LPWSTR)L"GUID";          SendMessageW(st->list, LVM_INSERTCOLUMNW, 3, (LPARAM)&col);

    SetPropW(frame, PW_PROP, (HANDLE)st);
    if (!g_origPwFrame) g_origPwFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Pw_FrameProc);
    Pw_Refresh(st);
    return frame;
}

MsApp g_AppPowerSch = { L"PowerSch", PowerSch_Create, 940, 420 };
