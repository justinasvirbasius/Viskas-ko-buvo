/*
 * app_shellprops.c — Read shell properties (file metadata) via IPropertyStore
 *
 * Demonstrates the modern shell property system:
 *   - SHCreateItemFromParsingName(L"C:\\path\\file.ext", NULL, IID_IShellItem2)
 *     creates a shell item for an absolute path
 *   - IShellItem2::GetPropertyStore(GPS_DEFAULT, IID_IPropertyStore) returns
 *     an IPropertyStore over the item's properties
 *   - IPropertyStore::GetCount + GetAt + GetValue(prop, &PROPVARIANT) walks
 *     every property by PROPERTYKEY
 *   - PSGetNameFromPropertyKey returns the canonical name (e.g.
 *     "System.Size") for a key
 *   - PropVariantToStringAlloc gives a human-readable representation
 *
 * UI: a path entry (defaulted to %windir%\notepad.exe) and a list view of
 * every property the shell knows about that file.
 */

#define COBJMACROS
#define CINTERFACE
#define INITGUID

#include "shell.h"
#include <commctrl.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propsys.h>
#include <propvarutil.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "comctl32.lib")

#define SP_PROP    L"MS_SP_STATE"
#define ID_SP_PATH 76001
#define ID_SP_GO   76002
#define ID_SP_LV   76003

typedef struct {
    HWND pathEdit, goBtn, list;
    BOOL comOk;
} SpState;

static WNDPROC g_origSpFrame = NULL;

static void Sp_LoadFor(SpState *st, const wchar_t *path)
{
    IShellItem2  *item  = NULL;
    IPropertyStore *ps  = NULL;
    HRESULT hr;
    DWORD count, i;

    SendMessageW(st->list, LVM_DELETEALLITEMS, 0, 0);

    hr = SHCreateItemFromParsingName(path, NULL,
            &IID_IShellItem2, (void **)&item);
    if (FAILED(hr)) {
        LVITEMW it;
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT;
        it.iItem = 0;
        it.pszText = (LPWSTR)L"(SHCreateItemFromParsingName failed)";
        SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        return;
    }
    hr = IShellItem2_GetPropertyStore(item, GPS_DEFAULT,
            &IID_IPropertyStore, (void **)&ps);
    if (FAILED(hr)) { IShellItem2_Release(item); return; }

    IPropertyStore_GetCount(ps, &count);
    for (i = 0; i < count; ++i) {
        PROPERTYKEY pk;
        PROPVARIANT pv;
        LVITEMW it;
        PWSTR name = NULL, value = NULL;
        wchar_t typeStr[40];

        if (FAILED(IPropertyStore_GetAt(ps, i, &pk))) continue;
        PropVariantInit(&pv);
        if (FAILED(IPropertyStore_GetValue(ps, &pk, &pv))) continue;

        PSGetNameFromPropertyKey(&pk, &name);
        PropVariantToStringAlloc(&pv, &value);
        swprintf_s(typeStr, 40, L"VT 0x%04x", pv.vt);

        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        it.pszText = name ? name : (LPWSTR)L"<unknown>";
        SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        it.iSubItem = 1; it.pszText = typeStr;
        SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
        it.iSubItem = 2; it.pszText = value ? value : (LPWSTR)L"";
        SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);

        if (name)  CoTaskMemFree(name);
        if (value) CoTaskMemFree(value);
        PropVariantClear(&pv);
    }
    IPropertyStore_Release(ps);
    IShellItem2_Release(item);
}

static LRESULT CALLBACK Sp_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    SpState *st = (SpState *)GetPropW(hwnd, SP_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_SP_GO) {
        wchar_t path[MAX_PATH] = L"";
        wchar_t expanded[MAX_PATH] = L"";
        GetWindowTextW(st->pathEdit, path, MAX_PATH);
        ExpandEnvironmentStringsW(path, expanded, MAX_PATH);
        Sp_LoadFor(st, expanded[0] ? expanded : path);
        return 0;
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->pathEdit, 12, 38, w - 124, 24, TRUE);
        MoveWindow(st->goBtn,    w - 108, 38, 90, 24, TRUE);
        MoveWindow(st->list,     8, 70, w - 16, h - 78, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->comOk) CoUninitialize();
        free(st); RemovePropW(hwnd, SP_PROP);
    }
    return CallWindowProcW(g_origSpFrame, hwnd, msg, wp, lp);
}

static HWND ShellProps_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    SpState *st;
    INITCOMMONCONTROLSEX icc;
    LVCOLUMNW col;
    HRESULT hr;
    (void)self;

    icc.dwSize = sizeof(icc); icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"ShellProps",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (SpState *)calloc(1, sizeof(SpState));
    if (!st) { DestroyWindow(frame); return NULL; }

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    st->comOk = SUCCEEDED(hr);

    st->pathEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"%windir%\\notepad.exe",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        12, 38, w - 124, 24, frame, (HMENU)(LONG_PTR)ID_SP_PATH, hInstance, NULL);
    st->goBtn = CreateWindowExW(0, L"BUTTON", L"Load",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        w - 108, 38, 90, 24, frame, (HMENU)(LONG_PTR)ID_SP_GO, hInstance, NULL);

    st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
        WS_CHILD | WS_VISIBLE | LVS_REPORT,
        8, 70, w - 16, h - 78, frame, (HMENU)(LONG_PTR)ID_SP_LV, hInstance, NULL);
    SendMessageW(st->list, LVM_SETEXTENDEDLISTVIEWSTYLE,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 280; col.pszText = (LPWSTR)L"Property";
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.cx =  90; col.pszText = (LPWSTR)L"Type";
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    col.cx = 380; col.pszText = (LPWSTR)L"Value";
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);

    SetPropW(frame, SP_PROP, (HANDLE)st);
    if (!g_origSpFrame) g_origSpFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Sp_FrameProc);

    {
        wchar_t expanded[MAX_PATH];
        ExpandEnvironmentStringsW(L"%windir%\\notepad.exe", expanded, MAX_PATH);
        Sp_LoadFor(st, expanded);
    }
    return frame;
}

MsApp g_AppShellProps = { L"ShellProps", ShellProps_Create, 820, 480 };
