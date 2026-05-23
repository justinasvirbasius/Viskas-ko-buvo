/*
 * app_wlaninfo.c — Wi-Fi interface and network enumeration via wlanapi
 *
 * Demonstrates the Native Wi-Fi user-mode API (wlanapi.dll):
 *   - WlanOpenHandle(dwClientVersion=2, NULL, &negotiated, &handle)
 *   - WlanEnumInterfaces(handle, NULL, &interfaceList)
 *     → WLAN_INTERFACE_INFO_LIST with one WLAN_INTERFACE_INFO per Wi-Fi NIC
 *   - WlanGetAvailableNetworkList(handle, &ifaceGuid, flags, NULL, &netList)
 *     → WLAN_AVAILABLE_NETWORK_LIST with SSIDs, signal quality, security
 *   - WlanFreeMemory on every returned buffer
 *   - WlanCloseHandle on shutdown
 *
 * dwClientVersion=2 covers Vista+. Buffers returned by Wlan* are
 * Wlan-allocated; client must release with WlanFreeMemory.
 */

#include "shell.h"
#include <wlanapi.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "wlanapi.lib")

#define WL_PROP   L"MS_WL_STATE"
#define ID_WL_REF 98001
#define ID_WL_OUT 98002

typedef struct { HWND refresh, output; HANDLE wlan; BOOL opened; } WlState;
static WNDPROC g_origWlFrame = NULL;

static void Wl_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
}

static const wchar_t *Wl_StateName(WLAN_INTERFACE_STATE s)
{
    switch (s) {
    case wlan_interface_state_not_ready:             return L"not ready";
    case wlan_interface_state_connected:             return L"connected";
    case wlan_interface_state_ad_hoc_network_formed: return L"ad-hoc formed";
    case wlan_interface_state_disconnecting:         return L"disconnecting";
    case wlan_interface_state_disconnected:          return L"disconnected";
    case wlan_interface_state_associating:           return L"associating";
    case wlan_interface_state_discovering:           return L"discovering";
    case wlan_interface_state_authenticating:        return L"authenticating";
    }
    return L"?";
}

static const wchar_t *Wl_AuthName(DOT11_AUTH_ALGORITHM a)
{
    switch (a) {
    case DOT11_AUTH_ALGO_80211_OPEN:       return L"OPEN";
    case DOT11_AUTH_ALGO_80211_SHARED_KEY: return L"SHARED";
    case DOT11_AUTH_ALGO_WPA:              return L"WPA";
    case DOT11_AUTH_ALGO_WPA_PSK:          return L"WPA-PSK";
    case DOT11_AUTH_ALGO_RSNA:             return L"WPA2";
    case DOT11_AUTH_ALGO_RSNA_PSK:         return L"WPA2-PSK";
    }
    return L"other";
}

static void Wl_Refresh(WlState *st)
{
    PWLAN_INTERFACE_INFO_LIST ifaces = NULL;
    DWORD r;
    DWORD i;

    SetWindowTextW(st->output, L"");
    if (!st->opened) {
        Wl_Append(st->output, L"WlanOpenHandle failed (Wi-Fi service not running?)\r\n");
        return;
    }

    r = WlanEnumInterfaces(st->wlan, NULL, &ifaces);
    if (r != ERROR_SUCCESS || !ifaces) {
        wchar_t buf[80];
        swprintf_s(buf, 80, L"WlanEnumInterfaces failed: %lu\r\n", r);
        Wl_Append(st->output, buf);
        return;
    }

    {
        wchar_t hdr[80];
        swprintf_s(hdr, 80, L"%lu Wi-Fi interface(s).\r\n\r\n", ifaces->dwNumberOfItems);
        Wl_Append(st->output, hdr);
    }

    for (i = 0; i < ifaces->dwNumberOfItems; ++i) {
        WLAN_INTERFACE_INFO *info = &ifaces->InterfaceInfo[i];
        wchar_t hdr[400];
        PWLAN_AVAILABLE_NETWORK_LIST nets = NULL;

        swprintf_s(hdr, 400,
            L"Interface [%lu]: %s\r\n"
            L"   state: %s\r\n\r\n",
            i, info->strInterfaceDescription, Wl_StateName(info->isState));
        Wl_Append(st->output, hdr);

        r = WlanGetAvailableNetworkList(st->wlan, &info->InterfaceGuid,
                WLAN_AVAILABLE_NETWORK_INCLUDE_ALL_MANUAL_HIDDEN_PROFILES,
                NULL, &nets);
        if (r == ERROR_SUCCESS && nets) {
            DWORD j;
            wchar_t cnt[80];
            swprintf_s(cnt, 80, L"   %lu network(s) in range:\r\n", nets->dwNumberOfItems);
            Wl_Append(st->output, cnt);
            for (j = 0; j < nets->dwNumberOfItems; ++j) {
                WLAN_AVAILABLE_NETWORK *n = &nets->Network[j];
                wchar_t ssid[64] = L"<hidden>";
                wchar_t line[400];
                if (n->dot11Ssid.uSSIDLength > 0) {
                    int slen = (int)n->dot11Ssid.uSSIDLength;
                    if (slen > 32) slen = 32;
                    MultiByteToWideChar(CP_UTF8, 0,
                        (LPCSTR)n->dot11Ssid.ucSSID, slen, ssid, 64);
                    ssid[slen] = 0;
                }
                swprintf_s(line, 400,
                    L"     %3lu%%  %-10s  %-32s  %s\r\n",
                    n->wlanSignalQuality,
                    Wl_AuthName(n->dot11DefaultAuthAlgorithm),
                    ssid,
                    n->bNetworkConnectable ? L"" : L"(not connectable)");
                Wl_Append(st->output, line);
            }
            WlanFreeMemory(nets);
        } else {
            wchar_t bad[80];
            swprintf_s(bad, 80, L"   WlanGetAvailableNetworkList: %lu\r\n", r);
            Wl_Append(st->output, bad);
        }
        Wl_Append(st->output, L"\r\n");
    }

    WlanFreeMemory(ifaces);
}

static LRESULT CALLBACK Wl_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    WlState *st = (WlState *)GetPropW(hwnd, WL_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_WL_REF) { Wl_Refresh(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->refresh, 12, 38, 130, 26, TRUE);
        MoveWindow(st->output,  8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->opened) WlanCloseHandle(st->wlan, NULL);
        free(st); RemovePropW(hwnd, WL_PROP);
    }
    return CallWindowProcW(g_origWlFrame, hwnd, msg, wp, lp);
}

static HWND WlanInfo_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    WlState *st;
    HFONT mono;
    DWORD negotiated, r;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"WlanInfo",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (WlState *)calloc(1, sizeof(WlState));
    if (!st) { DestroyWindow(frame); return NULL; }

    r = WlanOpenHandle(2, NULL, &negotiated, &st->wlan);
    st->opened = (r == ERROR_SUCCESS);

    st->refresh = CreateWindowExW(0, L"BUTTON", L"Refresh",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 130, 26, frame, (HMENU)(LONG_PTR)ID_WL_REF, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_WL_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, WL_PROP, (HANDLE)st);
    if (!g_origWlFrame) g_origWlFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Wl_FrameProc);
    Wl_Refresh(st);
    return frame;
}

MsApp g_AppWlanInfo = { L"WlanInfo", WlanInfo_Create, 720, 480 };
