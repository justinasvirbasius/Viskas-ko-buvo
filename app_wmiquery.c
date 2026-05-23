/*
 * app_wmiquery.c — WMI queries via IWbemLocator
 *
 * Demonstrates Windows Management Instrumentation from C:
 *   - CoInitializeSecurity(...) — required at least once per process for WMI
 *     (we call it best-effort; RPC_E_TOO_LATE is fine)
 *   - CoCreateInstance(CLSID_WbemLocator) → IWbemLocator
 *   - IWbemLocator::ConnectServer(L"ROOT\\CIMV2", ...) → IWbemServices
 *   - CoSetProxyBlanket on the services pointer to authenticate calls
 *   - IWbemServices::ExecQuery(L"WQL", L"SELECT * FROM Win32_OperatingSystem")
 *     → IEnumWbemClassObject
 *   - IEnumWbemClassObject::Next → IWbemClassObject (one row at a time)
 *   - IWbemClassObject::Get(L"PropertyName", &VARIANT)
 *
 * WMI uses VARIANT throughout (not PROPVARIANT). VariantInit / VariantClear.
 */

#define COBJMACROS
#define CINTERFACE

#include "shell.h"
#include <objbase.h>
#include <wbemidl.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "wbemuuid.lib")

#define WMI_PROP    L"MS_WMI_STATE"
#define ID_WMI_QRY  84001
#define ID_WMI_GO   84002
#define ID_WMI_OUT  84003

typedef struct {
    HWND  queryEdit, goBtn, output;
    IWbemLocator  *loc;
    IWbemServices *svc;
    BOOL  comOk;
} WmiState;

static WNDPROC g_origWmiFrame = NULL;

static void Wmi_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
}

static void Wmi_VariantToString(VARIANT *v, wchar_t *out, int cch)
{
    if (v->vt == VT_NULL || v->vt == VT_EMPTY) {
        wcscpy_s(out, cch, L"(null)");
    } else if (v->vt == VT_BSTR && v->bstrVal) {
        swprintf_s(out, cch, L"%s", v->bstrVal);
    } else if (v->vt == VT_I4) {
        swprintf_s(out, cch, L"%ld", v->lVal);
    } else if (v->vt == VT_UI4) {
        swprintf_s(out, cch, L"%lu", v->ulVal);
    } else if (v->vt == VT_I8) {
        swprintf_s(out, cch, L"%lld", v->llVal);
    } else if (v->vt == VT_UI8) {
        swprintf_s(out, cch, L"%llu", v->ullVal);
    } else if (v->vt == VT_BOOL) {
        wcscpy_s(out, cch, v->boolVal ? L"true" : L"false");
    } else if (v->vt == VT_R4) {
        swprintf_s(out, cch, L"%.4f", v->fltVal);
    } else if (v->vt == VT_R8) {
        swprintf_s(out, cch, L"%.6f", v->dblVal);
    } else if (v->vt & VT_ARRAY) {
        swprintf_s(out, cch, L"(array vt=0x%x)", v->vt);
    } else {
        swprintf_s(out, cch, L"(vt=0x%x)", v->vt);
    }
}

static BOOL Wmi_EnsureConnected(WmiState *st)
{
    HRESULT hr;
    BSTR ns;
    if (st->svc) return TRUE;

    /* CoInitializeSecurity is process-wide. Best-effort — RPC_E_TOO_LATE
       just means someone (perhaps another app) already set it. */
    CoInitializeSecurity(
        NULL, -1, NULL, NULL,
        RPC_C_AUTHN_LEVEL_DEFAULT,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        NULL, EOAC_NONE, NULL);

    hr = CoCreateInstance(&CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER,
                           &IID_IWbemLocator, (void **)&st->loc);
    if (FAILED(hr)) {
        wchar_t buf[80];
        swprintf_s(buf, 80, L"CoCreateInstance(WbemLocator) failed: 0x%08lx\r\n", hr);
        Wmi_Append(st->output, buf);
        return FALSE;
    }
    ns = SysAllocString(L"ROOT\\CIMV2");
    hr = IWbemLocator_ConnectServer(st->loc, ns,
            NULL, NULL, NULL, 0, NULL, NULL, &st->svc);
    SysFreeString(ns);
    if (FAILED(hr)) {
        wchar_t buf[80];
        swprintf_s(buf, 80, L"ConnectServer failed: 0x%08lx\r\n", hr);
        Wmi_Append(st->output, buf);
        return FALSE;
    }
    /* Required on WMI service pointers to allow method invocations */
    CoSetProxyBlanket((IUnknown *)st->svc,
        RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
        RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
        NULL, EOAC_NONE);
    return TRUE;
}

static void Wmi_RunQuery(WmiState *st)
{
    wchar_t query[1024];
    BSTR lang, q;
    IEnumWbemClassObject *enumerator = NULL;
    HRESULT hr;
    ULONG row = 0;

    GetWindowTextW(st->queryEdit, query, 1024);
    SetWindowTextW(st->output, L"");
    if (!Wmi_EnsureConnected(st)) return;

    lang = SysAllocString(L"WQL");
    q    = SysAllocString(query);
    hr = IWbemServices_ExecQuery(st->svc, lang, q,
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
            NULL, &enumerator);
    SysFreeString(lang);
    SysFreeString(q);
    if (FAILED(hr) || !enumerator) {
        wchar_t buf[80];
        swprintf_s(buf, 80, L"ExecQuery failed: 0x%08lx\r\n", hr);
        Wmi_Append(st->output, buf);
        return;
    }

    while (1) {
        IWbemClassObject *obj = NULL;
        ULONG returned = 0;
        SAFEARRAY *names = NULL;
        LONG lower, upper, i;
        wchar_t header[80];

        hr = IEnumWbemClassObject_Next(enumerator, WBEM_INFINITE, 1,
                                        &obj, &returned);
        if (FAILED(hr) || returned == 0 || !obj) break;

        swprintf_s(header, 80, L"\r\n--- Row %lu ---\r\n", ++row);
        Wmi_Append(st->output, header);

        if (SUCCEEDED(IWbemClassObject_GetNames(obj, NULL,
                WBEM_FLAG_ALWAYS | WBEM_FLAG_NONSYSTEM_ONLY, NULL, &names))
            && names)
        {
            SafeArrayGetLBound(names, 1, &lower);
            SafeArrayGetUBound(names, 1, &upper);
            for (i = lower; i <= upper; ++i) {
                BSTR name = NULL;
                VARIANT v;
                wchar_t valBuf[400];
                wchar_t line[600];

                SafeArrayGetElement(names, &i, &name);
                if (!name) continue;
                VariantInit(&v);
                if (SUCCEEDED(IWbemClassObject_Get(obj, name, 0, &v, NULL, NULL))) {
                    Wmi_VariantToString(&v, valBuf, 400);
                    swprintf_s(line, 600, L"  %-30s : %s\r\n", name, valBuf);
                    Wmi_Append(st->output, line);
                    VariantClear(&v);
                }
                SysFreeString(name);
                if (row > 5 && (i - lower) > 8) {
                    Wmi_Append(st->output, L"  ...\r\n");
                    break;
                }
            }
            SafeArrayDestroy(names);
        }

        IWbemClassObject_Release(obj);
        if (row >= 20) {
            Wmi_Append(st->output, L"\r\n(stopped at 20 rows)\r\n");
            break;
        }
    }
    IEnumWbemClassObject_Release(enumerator);
    {
        wchar_t footer[60];
        swprintf_s(footer, 60, L"\r\n%lu row(s).\r\n", row);
        Wmi_Append(st->output, footer);
    }
}

static LRESULT CALLBACK Wmi_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    WmiState *st = (WmiState *)GetPropW(hwnd, WMI_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_WMI_GO) { Wmi_RunQuery(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->queryEdit, 12, 38, w - 124, 24, TRUE);
        MoveWindow(st->goBtn,     w - 108, 38, 90, 24, TRUE);
        MoveWindow(st->output,    8, 70, w - 16, h - 78, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->svc) IWbemServices_Release(st->svc);
        if (st->loc) IWbemLocator_Release(st->loc);
        if (st->comOk) CoUninitialize();
        free(st); RemovePropW(hwnd, WMI_PROP);
    }
    return CallWindowProcW(g_origWmiFrame, hwnd, msg, wp, lp);
}

static HWND WmiQuery_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    WmiState *st;
    HFONT mono;
    HRESULT hr;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"WmiQuery",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (WmiState *)calloc(1, sizeof(WmiState));
    if (!st) { DestroyWindow(frame); return NULL; }

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    st->comOk = SUCCEEDED(hr);

    st->queryEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"SELECT Caption, Version, BuildNumber, FreePhysicalMemory FROM Win32_OperatingSystem",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        12, 38, w - 124, 24, frame, (HMENU)(LONG_PTR)ID_WMI_QRY, hInstance, NULL);
    st->goBtn = CreateWindowExW(0, L"BUTTON", L"Run",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        w - 108, 38, 90, 24, frame, (HMENU)(LONG_PTR)ID_WMI_GO, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 70, w - 16, h - 78, frame, (HMENU)(LONG_PTR)ID_WMI_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);
    Wmi_Append(st->output,
        L"WQL examples:\r\n"
        L"   SELECT * FROM Win32_Processor\r\n"
        L"   SELECT Caption FROM Win32_LogicalDisk WHERE DriveType = 3\r\n"
        L"   SELECT Name, ProcessId FROM Win32_Process WHERE Name LIKE '%notepad%'\r\n\r\n");

    SetPropW(frame, WMI_PROP, (HANDLE)st);
    if (!g_origWmiFrame) g_origWmiFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Wmi_FrameProc);
    return frame;
}

MsApp g_AppWmiQuery = { L"WmiQuery", WmiQuery_Create, 780, 460 };
