/*
 * app_fwpolicy.c — Windows Firewall management via INetFwPolicy2
 *
 * Demonstrates the Windows Firewall with Advanced Security COM API
 * (CLSID_NetFwPolicy2, hnetcfg.dll) — the same interface netsh
 * advfirewall and PowerShell New-NetFirewallRule wrap. Lets us
 * enumerate profiles (Domain / Private / Public), check enabled state,
 * and walk the configured rule set:
 *
 *   - CoCreateInstance(CLSID_NetFwPolicy2, IID_INetFwPolicy2, &policy)
 *   - INetFwPolicy2::get_FirewallEnabled(profileType, &VARIANT_BOOL)
 *     for each NET_FW_PROFILE2_DOMAIN/PRIVATE/PUBLIC
 *   - INetFwPolicy2::get_Rules(&INetFwRules) and ::get_Count(&n);
 *     IEnumVARIANT walks the collection
 *   - Per-rule INetFwRule::get_Name/get_Description/get_Enabled/
 *     get_Direction/get_Action returns each property
 *
 * No admin rights are required to READ firewall config; modifying it
 * (NewRule + ::Add) requires elevation.
 */

#define COBJMACROS
#define CINTERFACE
#define INITGUID

#include "shell.h"
#include <netfw.h>
#include <commctrl.h>
#include <oleauto.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "comctl32.lib")

#define FW_PROP   L"MS_FW_STATE"
#define ID_FW_REF 125001
#define ID_FW_LV  125002

typedef struct { HWND refresh, list; } FwState;
static WNDPROC g_origFwFrame = NULL;

static const wchar_t *Fw_DirName(NET_FW_RULE_DIRECTION d)
{
    switch (d) {
    case NET_FW_RULE_DIR_IN:  return L"IN";
    case NET_FW_RULE_DIR_OUT: return L"OUT";
    }
    return L"?";
}

static const wchar_t *Fw_ActionName(NET_FW_ACTION a)
{
    switch (a) {
    case NET_FW_ACTION_ALLOW: return L"ALLOW";
    case NET_FW_ACTION_BLOCK: return L"BLOCK";
    }
    return L"?";
}

static void Fw_Refresh(FwState *st)
{
    INetFwPolicy2 *policy = NULL;
    HRESULT hr;
    INetFwRules *rules = NULL;
    IUnknown *unk = NULL;
    IEnumVARIANT *en = NULL;
    long count = 0;
    VARIANT_BOOL enabledD = VARIANT_FALSE, enabledP = VARIANT_FALSE, enabledU = VARIANT_FALSE;
    VARIANT var;
    ULONG fetched = 0;
    int idx = 0;

    SendMessageW(st->list, LVM_DELETEALLITEMS, 0, 0);

    hr = CoCreateInstance(&CLSID_NetFwPolicy2, NULL, CLSCTX_INPROC_SERVER,
                          &IID_INetFwPolicy2, (void **)&policy);
    if (FAILED(hr) || !policy) {
        LVITEMW it;
        wchar_t buf[100];
        swprintf_s(buf, 100, L"(CoCreateInstance NetFwPolicy2 failed: 0x%08lx)", hr);
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT; it.pszText = buf;
        SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        return;
    }

    INetFwPolicy2_get_FirewallEnabled(policy, NET_FW_PROFILE2_DOMAIN,  &enabledD);
    INetFwPolicy2_get_FirewallEnabled(policy, NET_FW_PROFILE2_PRIVATE, &enabledP);
    INetFwPolicy2_get_FirewallEnabled(policy, NET_FW_PROFILE2_PUBLIC,  &enabledU);

    /* Header row */
    {
        LVITEMW it;
        wchar_t buf[200];
        swprintf_s(buf, 200, L"[Profiles] Domain=%s Private=%s Public=%s",
            enabledD ? L"ON" : L"off",
            enabledP ? L"ON" : L"off",
            enabledU ? L"ON" : L"off");
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT; it.iItem = idx;
        it.pszText = buf;
        SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        ++idx;
    }

    hr = INetFwPolicy2_get_Rules(policy, &rules);
    if (FAILED(hr) || !rules) {
        INetFwPolicy2_Release(policy);
        return;
    }
    INetFwRules_get_Count(rules, &count);

    hr = INetFwRules_get__NewEnum(rules, &unk);
    if (FAILED(hr) || !unk) {
        INetFwRules_Release(rules);
        INetFwPolicy2_Release(policy);
        return;
    }
    if (FAILED(IUnknown_QueryInterface(unk, &IID_IEnumVARIANT, (void **)&en)) || !en) {
        IUnknown_Release(unk);
        INetFwRules_Release(rules);
        INetFwPolicy2_Release(policy);
        return;
    }
    IUnknown_Release(unk);

    VariantInit(&var);
    while (IEnumVARIANT_Next(en, 1, &var, &fetched) == S_OK && fetched > 0 && idx < 250) {
        INetFwRule *rule = NULL;
        if (V_VT(&var) == VT_DISPATCH &&
            SUCCEEDED(IDispatch_QueryInterface(V_DISPATCH(&var), &IID_INetFwRule, (void **)&rule)) &&
            rule) {
            BSTR name = NULL;
            VARIANT_BOOL en2 = VARIANT_FALSE;
            NET_FW_RULE_DIRECTION dir = NET_FW_RULE_DIR_IN;
            NET_FW_ACTION action = NET_FW_ACTION_ALLOW;
            LVITEMW it;

            INetFwRule_get_Name(rule, &name);
            INetFwRule_get_Enabled(rule, &en2);
            INetFwRule_get_Direction(rule, &dir);
            INetFwRule_get_Action(rule, &action);

            ZeroMemory(&it, sizeof(it));
            it.mask = LVIF_TEXT; it.iItem = idx;
            it.pszText = name ? name : (LPWSTR)L"(no name)";
            SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
            it.iSubItem = 1; it.pszText = (LPWSTR)(en2 ? L"on" : L"off");
            SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
            it.iSubItem = 2; it.pszText = (LPWSTR)Fw_DirName(dir);
            SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
            it.iSubItem = 3; it.pszText = (LPWSTR)Fw_ActionName(action);
            SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);

            if (name) SysFreeString(name);
            INetFwRule_Release(rule);
            ++idx;
        }
        VariantClear(&var);
    }

    IEnumVARIANT_Release(en);
    INetFwRules_Release(rules);
    INetFwPolicy2_Release(policy);
}

static LRESULT CALLBACK Fw_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    FwState *st = (FwState *)GetPropW(hwnd, FW_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_FW_REF) { Fw_Refresh(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->refresh, 12, 38, 110, 26, TRUE);
        MoveWindow(st->list,    8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, FW_PROP); }
    return CallWindowProcW(g_origFwFrame, hwnd, msg, wp, lp);
}

static HWND FwPolicy_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    FwState *st;
    INITCOMMONCONTROLSEX icc;
    LVCOLUMNW col;
    (void)self;

    icc.dwSize = sizeof(icc); icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"FwPolicy",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (FwState *)calloc(1, sizeof(FwState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->refresh = CreateWindowExW(0, L"BUTTON", L"Refresh",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 110, 26, frame, (HMENU)(LONG_PTR)ID_FW_REF, hInstance, NULL);

    st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
        WS_CHILD | WS_VISIBLE | LVS_REPORT,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_FW_LV, hInstance, NULL);
    SendMessageW(st->list, LVM_SETEXTENDEDLISTVIEWSTYLE,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 360; col.pszText = (LPWSTR)L"Rule";       SendMessageW(st->list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.cx = 90;  col.pszText = (LPWSTR)L"Enabled";    SendMessageW(st->list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    col.cx = 90;  col.pszText = (LPWSTR)L"Dir";        SendMessageW(st->list, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);
    col.cx = 90;  col.pszText = (LPWSTR)L"Action";     SendMessageW(st->list, LVM_INSERTCOLUMNW, 3, (LPARAM)&col);

    SetPropW(frame, FW_PROP, (HANDLE)st);
    if (!g_origFwFrame) g_origFwFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Fw_FrameProc);
    Fw_Refresh(st);
    return frame;
}

MsApp g_AppFwPolicy = { L"FwPolicy", FwPolicy_Create, 760, 480 };
