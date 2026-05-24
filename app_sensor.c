/*
 * app_sensor.c — Windows Sensor API enumeration
 *
 * Demonstrates the Sensor & Location API — the OS-level abstraction
 * over accelerometers, gyroscopes, ambient-light sensors, etc.
 * Internally backed by SensorService and the sensor class extension.
 *
 *   - CoCreateInstance(CLSID_SensorManager, IID_ISensorManager, &mgr)
 *   - ISensorManager::GetSensorsByCategory(SENSOR_CATEGORY_ALL, &coll)
 *     returns ISensorCollection
 *   - ISensorCollection::GetCount(&n) and GetAt(i, &ISensor) walk
 *   - ISensor::GetFriendlyName(&BSTR), GetType(&GUID), GetCategory(&GUID),
 *     GetProperty(SENSOR_PROPERTY_PERSISTENT_UNIQUE_ID, &propVar) for ID
 *   - GetState(&SensorState) returns SENSOR_STATE_READY / _NOT_AVAILABLE /
 *     _NO_DATA / _INITIALIZING / _ACCESS_DENIED / _ERROR
 *
 * Notably, the Sensor API requires admin permission for most non-trivial
 * sensors; the ALL category is safe.
 */

#define COBJMACROS
#define CINTERFACE
#define INITGUID

#include "shell.h"
#include <sensorsapi.h>
#include <sensors.h>
#include <commctrl.h>
#include <oleauto.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "sensorsapi.lib")
#pragma comment(lib, "comctl32.lib")

#define SN_PROP   L"MS_SN_STATE"
#define ID_SN_REF 123001
#define ID_SN_LV  123002

typedef struct { HWND refresh, list; } SnState;
static WNDPROC g_origSnFrame = NULL;

static const wchar_t *Sn_StateName(SensorState s)
{
    switch (s) {
    case SENSOR_STATE_READY:         return L"READY";
    case SENSOR_STATE_NOT_AVAILABLE: return L"NOT_AVAILABLE";
    case SENSOR_STATE_NO_DATA:       return L"NO_DATA";
    case SENSOR_STATE_INITIALIZING:  return L"INITIALIZING";
    case SENSOR_STATE_ACCESS_DENIED: return L"ACCESS_DENIED";
    case SENSOR_STATE_ERROR:         return L"ERROR";
    }
    return L"?";
}

static void Sn_Refresh(SnState *st)
{
    ISensorManager *mgr = NULL;
    HRESULT hr;
    ISensorCollection *coll = NULL;
    ULONG count = 0, i;

    SendMessageW(st->list, LVM_DELETEALLITEMS, 0, 0);

    hr = CoCreateInstance(&CLSID_SensorManager, NULL, CLSCTX_INPROC_SERVER,
                          &IID_ISensorManager, (void **)&mgr);
    if (FAILED(hr) || !mgr) {
        LVITEMW it;
        wchar_t buf[120];
        swprintf_s(buf, 120,
            (hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DISABLED_BY_POLICY))
                ? L"(sensor service disabled by policy)"
                : L"(CoCreateInstance failed: 0x%08lx)", hr);
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT; it.pszText = buf;
        SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        return;
    }

    hr = ISensorManager_GetSensorsByCategory(mgr, &SENSOR_CATEGORY_ALL, &coll);
    if (FAILED(hr) || !coll) {
        LVITEMW it;
        wchar_t buf[120];
        swprintf_s(buf, 120,
            (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
                ? L"(no sensors present)"
                : L"(GetSensorsByCategory: 0x%08lx)", hr);
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT; it.pszText = buf;
        SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        ISensorManager_Release(mgr);
        return;
    }

    if (FAILED(ISensorCollection_GetCount(coll, &count)) || count == 0) {
        LVITEMW it;
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT; it.pszText = (LPWSTR)L"(0 sensors found)";
        SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        ISensorCollection_Release(coll);
        ISensorManager_Release(mgr);
        return;
    }

    for (i = 0; i < count; ++i) {
        ISensor *s = NULL;
        if (FAILED(ISensorCollection_GetAt(coll, i, &s)) || !s) continue;
        {
            BSTR name = NULL;
            SensorState state = SENSOR_STATE_NOT_AVAILABLE;
            GUID typeId;
            wchar_t typeStr[64] = L"";
            LVITEMW it;

            ISensor_GetFriendlyName(s, &name);
            ISensor_GetState(s, &state);
            if (SUCCEEDED(ISensor_GetType(s, &typeId))) {
                StringFromGUID2(&typeId, typeStr, 64);
            }

            ZeroMemory(&it, sizeof(it));
            it.mask = LVIF_TEXT; it.iItem = (int)i;
            it.pszText = name ? name : (LPWSTR)L"(no name)";
            SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
            it.iSubItem = 1; it.pszText = (LPWSTR)Sn_StateName(state);
            SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
            it.iSubItem = 2; it.pszText = typeStr;
            SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);

            if (name) SysFreeString(name);
        }
        ISensor_Release(s);
    }

    ISensorCollection_Release(coll);
    ISensorManager_Release(mgr);
}

static LRESULT CALLBACK Sn_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    SnState *st = (SnState *)GetPropW(hwnd, SN_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_SN_REF) { Sn_Refresh(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->refresh, 12, 38, 110, 26, TRUE);
        MoveWindow(st->list,    8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, SN_PROP); }
    return CallWindowProcW(g_origSnFrame, hwnd, msg, wp, lp);
}

static HWND Sensor_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    SnState *st;
    INITCOMMONCONTROLSEX icc;
    LVCOLUMNW col;
    (void)self;

    icc.dwSize = sizeof(icc); icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Sensor",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (SnState *)calloc(1, sizeof(SnState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->refresh = CreateWindowExW(0, L"BUTTON", L"Refresh",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 110, 26, frame, (HMENU)(LONG_PTR)ID_SN_REF, hInstance, NULL);

    st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
        WS_CHILD | WS_VISIBLE | LVS_REPORT,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_SN_LV, hInstance, NULL);
    SendMessageW(st->list, LVM_SETEXTENDEDLISTVIEWSTYLE,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 340; col.pszText = (LPWSTR)L"Friendly name"; SendMessageW(st->list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.cx = 140; col.pszText = (LPWSTR)L"State";          SendMessageW(st->list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    col.cx = 360; col.pszText = (LPWSTR)L"Type GUID";      SendMessageW(st->list, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);

    SetPropW(frame, SN_PROP, (HANDLE)st);
    if (!g_origSnFrame) g_origSnFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Sn_FrameProc);
    Sn_Refresh(st);
    return frame;
}

MsApp g_AppSensor = { L"Sensor", Sensor_Create, 920, 440 };
