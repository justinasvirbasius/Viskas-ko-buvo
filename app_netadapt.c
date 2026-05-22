/*
 * app_netadapt.c — Enumerate network adapters and their addresses
 *
 * Demonstrates GetAdaptersAddresses for richer NIC info than the older
 * GetAdaptersInfo:
 *   - Two-pass sizing with ERROR_BUFFER_OVERFLOW
 *   - PIP_ADAPTER_ADDRESSES linked list, walking ->Next
 *   - Per-adapter: AdapterName, FriendlyName, Description, IfType,
 *     OperStatus, physical address bytes
 *   - Per-adapter address lists: FirstUnicastAddress (IPv4/IPv6),
 *     FirstGatewayAddress, FirstDnsServerAddress
 *   - inet_ntop/getnameinfo for printable addresses
 *
 * Output goes to a read-only multi-line edit so users can scroll/copy.
 */

#include "shell.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

#define NA_PROP   L"MS_NA_STATE"
#define ID_NA_OUT 55001
#define ID_NA_REF 55002

typedef struct {
    HWND output, refBtn;
} NaState;

static WNDPROC g_origNaFrame = NULL;

static const wchar_t *Na_IfType(IFTYPE t)
{
    switch (t) {
    case IF_TYPE_ETHERNET_CSMACD: return L"Ethernet";
    case IF_TYPE_IEEE80211:       return L"Wi-Fi (802.11)";
    case IF_TYPE_SOFTWARE_LOOPBACK: return L"Loopback";
    case IF_TYPE_TUNNEL:          return L"Tunnel";
    case IF_TYPE_PPP:             return L"PPP";
    case IF_TYPE_IEEE1394:        return L"FireWire";
    }
    return L"Other";
}

static const wchar_t *Na_OperStatus(IF_OPER_STATUS s)
{
    switch (s) {
    case IfOperStatusUp:             return L"up";
    case IfOperStatusDown:           return L"down";
    case IfOperStatusTesting:        return L"testing";
    case IfOperStatusUnknown:        return L"unknown";
    case IfOperStatusDormant:        return L"dormant";
    case IfOperStatusNotPresent:     return L"not present";
    case IfOperStatusLowerLayerDown: return L"lower-layer down";
    }
    return L"?";
}

static void Na_Append(HWND edit, const wchar_t *t)
{
    int len = GetWindowTextLengthW(edit);
    SendMessageW(edit, EM_SETSEL, len, len);
    SendMessageW(edit, EM_REPLACESEL, FALSE, (LPARAM)t);
}

static void Na_FormatSockaddr(struct sockaddr *sa, wchar_t *out, int cch)
{
    char buf[INET6_ADDRSTRLEN] = "";
    if (sa->sa_family == AF_INET) {
        struct sockaddr_in *in4 = (struct sockaddr_in *)sa;
        inet_ntop(AF_INET, &in4->sin_addr, buf, sizeof(buf));
    } else if (sa->sa_family == AF_INET6) {
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)sa;
        inet_ntop(AF_INET6, &in6->sin6_addr, buf, sizeof(buf));
    } else {
        swprintf_s(out, cch, L"(family %d)", sa->sa_family);
        return;
    }
    swprintf_s(out, cch, L"%hs", buf);
}

static void Na_Refresh(NaState *st)
{
    IP_ADAPTER_ADDRESSES *buf = NULL;
    ULONG cb = 0;
    DWORD rc;
    IP_ADAPTER_ADDRESSES *a;

    SetWindowTextW(st->output, L"");

    rc = GetAdaptersAddresses(AF_UNSPEC,
        GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_ANYCAST,
        NULL, NULL, &cb);
    if (rc != ERROR_BUFFER_OVERFLOW) {
        Na_Append(st->output, L"GetAdaptersAddresses sizing failed.\r\n");
        return;
    }
    buf = (IP_ADAPTER_ADDRESSES *)malloc(cb);
    if (!buf) return;
    rc = GetAdaptersAddresses(AF_UNSPEC,
        GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_ANYCAST,
        NULL, buf, &cb);
    if (rc != NO_ERROR) {
        free(buf);
        Na_Append(st->output, L"GetAdaptersAddresses failed.\r\n");
        return;
    }

    for (a = buf; a; a = a->Next) {
        wchar_t line[600];
        const PIP_ADAPTER_UNICAST_ADDRESS u;
        const PIP_ADAPTER_GATEWAY_ADDRESS_LH g;
        const PIP_ADAPTER_DNS_SERVER_ADDRESS d;

        swprintf_s(line, 600,
            L"=== %s ===\r\n"
            L"  description : %s\r\n"
            L"  type        : %s\r\n"
            L"  status      : %s\r\n"
            L"  MTU         : %lu bytes\r\n"
            L"  link speed  : %llu Mbps\r\n",
            a->FriendlyName ? a->FriendlyName : L"<noname>",
            a->Description  ? a->Description  : L"",
            Na_IfType(a->IfType),
            Na_OperStatus(a->OperStatus),
            a->Mtu,
            a->TransmitLinkSpeed / 1000000ULL);
        Na_Append(st->output, line);

        /* Physical (MAC) */
        if (a->PhysicalAddressLength > 0) {
            wchar_t mac[64] = L"  MAC         : ";
            wchar_t hex[8];
            ULONG i;
            for (i = 0; i < a->PhysicalAddressLength; ++i) {
                swprintf_s(hex, 8, i ? L":%02X" : L"%02X", a->PhysicalAddress[i]);
                wcscat_s(mac, 64, hex);
            }
            wcscat_s(mac, 64, L"\r\n");
            Na_Append(st->output, mac);
        }

        /* Unicast addresses (IPv4/IPv6) */
        for (u = a->FirstUnicastAddress; u; u = u->Next) {
            wchar_t addr[80];
            Na_FormatSockaddr(u->Address.lpSockaddr, addr, 80);
            swprintf_s(line, 600, L"  addr        : %s\r\n", addr);
            Na_Append(st->output, line);
        }
        for (g = a->FirstGatewayAddress; g; g = g->Next) {
            wchar_t addr[80];
            Na_FormatSockaddr(g->Address.lpSockaddr, addr, 80);
            swprintf_s(line, 600, L"  gateway     : %s\r\n", addr);
            Na_Append(st->output, line);
        }
        for (d = a->FirstDnsServerAddress; d; d = d->Next) {
            wchar_t addr[80];
            Na_FormatSockaddr(d->Address.lpSockaddr, addr, 80);
            swprintf_s(line, 600, L"  DNS server  : %s\r\n", addr);
            Na_Append(st->output, line);
        }
        Na_Append(st->output, L"\r\n");
    }
    free(buf);
}

static LRESULT CALLBACK Na_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    NaState *st = (NaState *)GetPropW(hwnd, NA_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_NA_REF) { Na_Refresh(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->refBtn, 8, 34, 100, 24, TRUE);
        MoveWindow(st->output, 8, 64, w - 16, h - 72, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, NA_PROP); }
    return CallWindowProcW(g_origNaFrame, hwnd, msg, wp, lp);
}

static HWND NetAdapt_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    NaState *st;
    HFONT mono;
    WSADATA wsa;
    (void)self;

    WSAStartup(MAKEWORD(2, 2), &wsa);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"NetAdapt",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (NaState *)calloc(1, sizeof(NaState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->refBtn = CreateWindowExW(0, L"BUTTON", L"Refresh",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        8, 34, 100, 24, frame, (HMENU)(LONG_PTR)ID_NA_REF, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 64, w - 16, h - 72, frame, (HMENU)(LONG_PTR)ID_NA_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, NA_PROP, (HANDLE)st);
    if (!g_origNaFrame) g_origNaFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Na_FrameProc);

    Na_Refresh(st);
    return frame;
}

MsApp g_AppNetAdapt = {
    L"NetAdapt",
    NetAdapt_Create,
    640, 460
};
