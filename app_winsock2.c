/*
 * app_winsock2.c — Pure WSA Winsock2: address resolution + UDP echo
 *
 * Demonstrates the lower-level Winsock 2 API surface that the rest of
 * the project has so far accessed indirectly:
 *   - WSAStartup(MAKEWORD(2,2), &data) initializes the lib version
 *   - getaddrinfo(host, port, &hints, &result) DNS lookup with
 *     dual-stack hints (AF_UNSPEC); returns linked-list of addrinfo
 *   - WSAAddressToStringW(sockaddr, sa_len, NULL, buf, &len) for
 *     human-readable IPv4/IPv6 strings
 *   - socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP) for UDP datagrams
 *   - sendto / recvfrom for connectionless I/O
 *   - WSAEventSelect-style readiness is covered separately in Batch 11;
 *     here we use a worker thread and a 2-second recv timeout
 *   - WSACleanup on shutdown
 *
 * We resolve a host, then send a datagram to UDP port 7 (echo) and
 * await a reply within 2 seconds. (Most hosts don't reply, but the
 * code exercises the full path.)
 */

#include "shell.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "ws2_32.lib")

#define WS_PROP    L"MS_WS_STATE"
#define ID_WS_HOST 106001
#define ID_WS_GO   106002
#define ID_WS_OUT  106003

#define WM_WS_LINE (WM_USER + 220)
#define WM_WS_DONE (WM_USER + 221)

typedef struct {
    HWND   frame, hostEdit, goBtn, output;
    HANDLE thread;
} WsState;
static WNDPROC g_origWsFrame = NULL;

static void Ws_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
    SendMessageW(e, EM_SCROLLCARET, 0, 0);
}

static void Ws_Post(HWND f, const wchar_t *t)
{
    wchar_t *p = _wcsdup(t);
    if (p) PostMessageW(f, WM_WS_LINE, 0, (LPARAM)p);
}

static DWORD WINAPI Ws_Worker(LPVOID arg)
{
    WsState *st = (WsState *)arg;
    wchar_t hostW[256];
    char host[256];
    WSADATA wsd;
    ADDRINFOA hints, *result = NULL, *p;
    SOCKET sock = INVALID_SOCKET;
    int r;

    GetWindowTextW(st->hostEdit, hostW, 256);
    if (!hostW[0]) { Ws_Post(st->frame, L"Enter a host.\r\n");
        PostMessageW(st->frame, WM_WS_DONE, 0, 0); return 0; }
    WideCharToMultiByte(CP_UTF8, 0, hostW, -1, host, 256, NULL, NULL);

    if (WSAStartup(MAKEWORD(2, 2), &wsd) != 0) {
        Ws_Post(st->frame, L"WSAStartup failed.\r\n");
        PostMessageW(st->frame, WM_WS_DONE, 0, 0);
        return 0;
    }
    {
        wchar_t line[80];
        swprintf_s(line, 80, L"WSAStartup OK — Winsock %d.%d\r\n",
                   LOBYTE(wsd.wVersion), HIBYTE(wsd.wVersion));
        Ws_Post(st->frame, line);
    }

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    r = getaddrinfo(host, "7", &hints, &result);
    if (r != 0) {
        wchar_t line[200];
        swprintf_s(line, 200, L"getaddrinfo('%s') failed: %d\r\n", hostW, r);
        Ws_Post(st->frame, line);
        WSACleanup();
        PostMessageW(st->frame, WM_WS_DONE, 0, 0);
        return 0;
    }

    {
        int idx = 0;
        Ws_Post(st->frame, L"\r\n== Resolved addresses ==\r\n");
        for (p = result; p; p = p->ai_next) {
            wchar_t addrStr[100];
            DWORD len = 100;
            if (WSAAddressToStringW(p->ai_addr, (DWORD)p->ai_addrlen, NULL,
                                     addrStr, &len) == 0) {
                wchar_t line[200];
                swprintf_s(line, 200, L"  [%d] family=%d  %s\r\n",
                           idx++, p->ai_family, addrStr);
                Ws_Post(st->frame, line);
            }
        }
    }

    /* Send a tiny UDP datagram to the first resolved address */
    sock = socket(result->ai_family, SOCK_DGRAM, IPPROTO_UDP);
    if (sock != INVALID_SOCKET) {
        const char msg[] = "MiniShell ping";
        DWORD timeout = 2000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                   (const char *)&timeout, sizeof(timeout));
        if (sendto(sock, msg, (int)strlen(msg), 0,
                   result->ai_addr, (int)result->ai_addrlen) != SOCKET_ERROR) {
            char reply[512];
            struct sockaddr_storage from;
            int fromLen = sizeof(from);
            int got;
            Ws_Post(st->frame, L"\r\nSent UDP datagram to port 7 (echo).\r\n"
                                L"Waiting up to 2 s for reply...\r\n");
            got = recvfrom(sock, reply, sizeof(reply) - 1, 0,
                            (struct sockaddr *)&from, &fromLen);
            if (got > 0) {
                wchar_t line[200];
                reply[got] = 0;
                swprintf_s(line, 200,
                    L"Reply: %d bytes received.\r\n", got);
                Ws_Post(st->frame, line);
            } else {
                wchar_t line[120];
                int err = WSAGetLastError();
                swprintf_s(line, 120,
                    L"No reply (recvfrom err %d — typical, port 7 disabled).\r\n", err);
                Ws_Post(st->frame, line);
            }
        } else {
            Ws_Post(st->frame, L"sendto failed.\r\n");
        }
        closesocket(sock);
    } else {
        Ws_Post(st->frame, L"socket() failed.\r\n");
    }

    freeaddrinfo(result);
    WSACleanup();
    PostMessageW(st->frame, WM_WS_DONE, 0, 0);
    return 0;
}

static LRESULT CALLBACK Ws_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    WsState *st = (WsState *)GetPropW(hwnd, WS_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_WS_GO) {
        DWORD tid;
        if (st->thread) return 0;
        SetWindowTextW(st->output, L"");
        st->thread = CreateThread(NULL, 0, Ws_Worker, st, 0, &tid);
        return 0;
    }
    if (msg == WM_WS_LINE && st) {
        wchar_t *p = (wchar_t *)lp;
        if (p) { Ws_Append(st->output, p); free(p); }
        return 0;
    }
    if (msg == WM_WS_DONE && st) {
        if (st->thread) { CloseHandle(st->thread); st->thread = NULL; }
        Ws_Append(st->output, L"\r\n[done]\r\n");
        return 0;
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->hostEdit, 12, 38, w - 130, 24, TRUE);
        MoveWindow(st->goBtn,    w - 112, 38, 96, 24, TRUE);
        MoveWindow(st->output,   8, 70, w - 16, h - 78, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->thread) { WaitForSingleObject(st->thread, 3000); CloseHandle(st->thread); }
        free(st); RemovePropW(hwnd, WS_PROP);
    }
    return CallWindowProcW(g_origWsFrame, hwnd, msg, wp, lp);
}

static HWND WinSock2_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    WsState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"WinSock2",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (WsState *)calloc(1, sizeof(WsState));
    if (!st) { DestroyWindow(frame); return NULL; }
    st->frame = frame;

    st->hostEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"localhost",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        12, 38, w - 130, 24, frame, (HMENU)(LONG_PTR)ID_WS_HOST, hInstance, NULL);
    st->goBtn = CreateWindowExW(0, L"BUTTON", L"Resolve + ping",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        w - 112, 38, 96, 24, frame, (HMENU)(LONG_PTR)ID_WS_GO, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 70, w - 16, h - 78, frame, (HMENU)(LONG_PTR)ID_WS_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, WS_PROP, (HANDLE)st);
    if (!g_origWsFrame) g_origWsFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Ws_FrameProc);
    return frame;
}

MsApp g_AppWinSock2 = { L"WinSock2", WinSock2_Create, 720, 420 };
