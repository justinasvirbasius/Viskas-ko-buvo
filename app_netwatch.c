/*
 * app_netwatch.c — Asynchronous network-address-change notifications
 *
 * Demonstrates the iphlpapi async notification API:
 *   - NotifyAddrChange(&handle, &overlapped) registers for the *next*
 *     address-table change and returns ERROR_IO_PENDING immediately
 *   - When the OS detects an IP address add/remove/change (Wi-Fi connect,
 *     VPN up/down, ethernet plug, etc.) the overlapped event is signaled
 *   - A worker thread waits on the event, then re-arms NotifyAddrChange
 *     after each change so the watch is continuous
 *
 * On detection, the worker re-runs a quick GetAdaptersAddresses summary
 * and posts the textual diff/snapshot to the UI. NotifyRouteChange follows
 * the same shape; we use NotifyAddrChange as the canonical example.
 *
 * To trigger: toggle Wi-Fi, plug/unplug Ethernet, or run
 *   ipconfig /release && ipconfig /renew  from a command prompt.
 */

#include "shell.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

#define NW_PROP    L"MS_NW_STATE"
#define ID_NW_OUT  71001
#define ID_NW_GO   71002
#define ID_NW_STOP 71003

#define WM_NW_EVENT (WM_USER + 160)   /* lparam = wchar_t* (heap, freed by UI) */

typedef struct {
    HWND   frame, output, goBtn, stopBtn;
    HANDLE thread, stopEvent;
    BOOL   running;
} NwState;

static WNDPROC g_origNwFrame = NULL;

static void Nw_Append(NwState *st, const wchar_t *t)
{
    int len = GetWindowTextLengthW(st->output);
    SendMessageW(st->output, EM_SETSEL, len, len);
    SendMessageW(st->output, EM_REPLACESEL, FALSE, (LPARAM)t);
    SendMessageW(st->output, EM_SCROLLCARET, 0, 0);
}

static wchar_t *Nw_Snapshot(void)
{
    IP_ADAPTER_ADDRESSES *buf = NULL;
    ULONG cb = 0;
    DWORD rc;
    wchar_t *out;
    SIZE_T outCap = 4096;
    SIZE_T outLen = 0;
    IP_ADAPTER_ADDRESSES *a;

    out = (wchar_t *)malloc(outCap * sizeof(wchar_t));
    if (!out) return NULL;
    out[0] = 0;

    rc = GetAdaptersAddresses(AF_UNSPEC,
            GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_ANYCAST,
            NULL, NULL, &cb);
    if (rc != ERROR_BUFFER_OVERFLOW) return out;
    buf = (IP_ADAPTER_ADDRESSES *)malloc(cb);
    if (!buf) return out;
    if (GetAdaptersAddresses(AF_UNSPEC,
            GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_ANYCAST,
            NULL, buf, &cb) != NO_ERROR) {
        free(buf); return out;
    }

    for (a = buf; a; a = a->Next) {
        const PIP_ADAPTER_UNICAST_ADDRESS u;
        if (a->OperStatus != IfOperStatusUp) continue;
        for (u = a->FirstUnicastAddress; u; u = u->Next) {
            char addr[INET6_ADDRSTRLEN] = "";
            wchar_t line[200];
            int n;

            if (u->Address.lpSockaddr->sa_family == AF_INET) {
                struct sockaddr_in *in4 =
                    (struct sockaddr_in *)u->Address.lpSockaddr;
                inet_ntop(AF_INET, &in4->sin_addr, addr, sizeof(addr));
            } else if (u->Address.lpSockaddr->sa_family == AF_INET6) {
                struct sockaddr_in6 *in6 =
                    (struct sockaddr_in6 *)u->Address.lpSockaddr;
                inet_ntop(AF_INET6, &in6->sin6_addr, addr, sizeof(addr));
            } else continue;

            n = swprintf_s(line, 200, L"    %-30s  %hs\r\n",
                           a->FriendlyName ? a->FriendlyName : L"", addr);
            if (outLen + n + 1 >= outCap) {
                outCap *= 2;
                out = (wchar_t *)realloc(out, outCap * sizeof(wchar_t));
                if (!out) { free(buf); return NULL; }
            }
            wcscat_s(out + outLen, outCap - outLen, line);
            outLen += n;
        }
    }
    free(buf);
    return out;
}

static DWORD WINAPI Nw_Worker(LPVOID arg)
{
    NwState *st = (NwState *)arg;
    HANDLE waits[2];
    OVERLAPPED ov;

    waits[0] = st->stopEvent;
    waits[1] = CreateEventW(NULL, TRUE, FALSE, NULL);

    for (;;) {
        HANDLE notify = NULL;
        DWORD rc;
        DWORD wait;

        ZeroMemory(&ov, sizeof(ov));
        ov.hEvent = waits[1];
        ResetEvent(waits[1]);

        rc = NotifyAddrChange(&notify, &ov);
        if (rc != NO_ERROR && rc != ERROR_IO_PENDING) {
            PostMessageW(st->frame, WM_NW_EVENT,
                         0, (LPARAM)_wcsdup(L"NotifyAddrChange failed\r\n"));
            break;
        }
        wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0) break;
        if (wait != WAIT_OBJECT_0 + 1) break;

        {
            wchar_t header[80];
            SYSTEMTIME t; GetLocalTime(&t);
            swprintf_s(header, 80,
                L"\r\n>> change @ %02u:%02u:%02u.%03u\r\n",
                t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
            PostMessageW(st->frame, WM_NW_EVENT, 0, (LPARAM)_wcsdup(header));
        }
        {
            wchar_t *snap = Nw_Snapshot();
            if (snap) PostMessageW(st->frame, WM_NW_EVENT, 0, (LPARAM)snap);
        }
    }
    CloseHandle(waits[1]);
    return 0;
}

static void Nw_Start(NwState *st)
{
    DWORD tid;
    if (st->running) return;
    ResetEvent(st->stopEvent);
    st->thread = CreateThread(NULL, 0, Nw_Worker, st, 0, &tid);
    st->running = (st->thread != NULL);
    if (st->running) {
        wchar_t *snap;
        Nw_Append(st, L"Watching for address-table changes...\r\n\r\n");
        Nw_Append(st, L"Initial snapshot:\r\n");
        snap = Nw_Snapshot();
        if (snap) { Nw_Append(st, snap); free(snap); }
    } else {
        Nw_Append(st, L"Failed to start worker thread.\r\n");
    }
}

static void Nw_Stop(NwState *st)
{
    if (!st->running) return;
    SetEvent(st->stopEvent);
    /* NotifyAddrChange doesn't have a clean cancel from another thread;
       we wait a short time then let the OS clean up. */
    WaitForSingleObject(st->thread, 1000);
    CloseHandle(st->thread);
    st->thread = NULL;
    st->running = FALSE;
    Nw_Append(st, L"\r\nStopped.\r\n");
}

static LRESULT CALLBACK Nw_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    NwState *st = (NwState *)GetPropW(hwnd, NW_PROP);
    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_NW_GO)   { Nw_Start(st); return 0; }
        if (LOWORD(wp) == ID_NW_STOP) { Nw_Stop(st);  return 0; }
    }
    if (msg == WM_NW_EVENT && st) {
        wchar_t *p = (wchar_t *)lp;
        if (p) { Nw_Append(st, p); free(p); }
        return 0;
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        Nw_Stop(st);
        if (st->stopEvent) CloseHandle(st->stopEvent);
        free(st); RemovePropW(hwnd, NW_PROP);
    }
    return CallWindowProcW(g_origNwFrame, hwnd, msg, wp, lp);
}

static HWND NetWatch_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    NwState *st;
    HFONT mono;
    WSADATA wsa;
    (void)self;

    WSAStartup(MAKEWORD(2, 2), &wsa);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"NetWatch",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (NwState *)calloc(1, sizeof(NwState));
    if (!st) { DestroyWindow(frame); return NULL; }
    st->frame = frame;
    st->stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    st->goBtn = CreateWindowExW(0, L"BUTTON", L"Start watch",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 120, 26, frame, (HMENU)(LONG_PTR)ID_NW_GO, hInstance, NULL);
    st->stopBtn = CreateWindowExW(0, L"BUTTON", L"Stop",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        140, 38, 90, 26, frame, (HMENU)(LONG_PTR)ID_NW_STOP, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_NW_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, NW_PROP, (HANDLE)st);
    if (!g_origNwFrame) g_origNwFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Nw_FrameProc);
    return frame;
}

MsApp g_AppNetWatch = { L"NetWatch", NetWatch_Create, 620, 420 };
