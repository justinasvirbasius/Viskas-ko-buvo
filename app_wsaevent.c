/*
 * app_wsaevent.c — Event-based async sockets via WSAEventSelect
 *
 * Demonstrates the WSAEventSelect socket model — distinct from overlapped
 * (Async.c) and IOCP (also Async.c) — which signals an event when any of a
 * mask of network events occurs:
 *   - WSACreateEvent / WSACloseEvent for the event handle
 *   - WSAEventSelect(s, hEvent, FD_CONNECT | FD_READ | FD_WRITE | FD_CLOSE)
 *     subscribes that socket to those events; socket becomes non-blocking
 *   - WSAWaitForMultipleEvents lets a worker thread block on multiple sockets
 *     plus a stop event
 *   - WSAEnumNetworkEvents(s, hEvent, &netEvents) tells us which event
 *     fired and any per-event error code, then clears it
 *
 * The app connects to example.com:80 in the background, sends GET, and
 * accumulates the response — all driven by event-select rather than
 * blocking, overlapped, or completion ports.
 */

#include "shell.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "ws2_32.lib")

#define WE_PROP    L"MS_WSA_STATE"
#define ID_WE_GO   81001
#define ID_WE_STOP 81002
#define ID_WE_OUT  81003
#define ID_WE_HOST 81004

#define WM_WSA_LOG (WM_USER + 170)   /* lparam = wchar_t* (heap) */
#define WM_WSA_DONE (WM_USER + 171)

typedef struct {
    HWND     frame, output, hostEdit, goBtn, stopBtn;
    HANDLE   thread;
    HANDLE   stopEvent;
    WSADATA  wsa;
    BOOL     wsaOk;
} WeState;

static WNDPROC g_origWeFrame = NULL;

static void We_Post(HWND frame, const wchar_t *t)
{
    wchar_t *p = _wcsdup(t);
    if (p) PostMessageW(frame, WM_WSA_LOG, 0, (LPARAM)p);
}

static DWORD WINAPI We_Worker(LPVOID arg)
{
    WeState *st = (WeState *)arg;
    char host[200] = "";
    SOCKET s = INVALID_SOCKET;
    HANDLE hEvent = WSA_INVALID_EVENT;
    HANDLE waits[2];
    struct addrinfo hints, *result = NULL;
    struct addrinfo *p;
    BOOL sentRequest = FALSE;
    wchar_t hostW[200];

    GetWindowTextW(st->hostEdit, hostW, 200);
    WideCharToMultiByte(CP_UTF8, 0, hostW, -1, host, 200, NULL, NULL);
    if (!host[0]) strcpy_s(host, 200, "example.com");

    {
        wchar_t buf[260];
        swprintf_s(buf, 260, L"Resolving %hs:80 ...\r\n", host);
        We_Post(st->frame, buf);
    }

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, "80", &hints, &result) != 0) {
        We_Post(st->frame, L"getaddrinfo failed.\r\n");
        goto done;
    }
    p = result;

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { We_Post(st->frame, L"socket failed.\r\n"); goto done; }

    hEvent = WSACreateEvent();
    if (hEvent == WSA_INVALID_EVENT) { We_Post(st->frame, L"WSACreateEvent failed.\r\n"); goto done; }

    if (WSAEventSelect(s, hEvent,
            FD_CONNECT | FD_READ | FD_WRITE | FD_CLOSE) == SOCKET_ERROR) {
        We_Post(st->frame, L"WSAEventSelect failed.\r\n"); goto done;
    }

    /* socket is now non-blocking due to WSAEventSelect */
    connect(s, p->ai_addr, (int)p->ai_addrlen);  /* will return WSAEWOULDBLOCK */
    We_Post(st->frame, L"Connecting...\r\n");

    waits[0] = st->stopEvent;
    waits[1] = hEvent;

    for (;;) {
        DWORD wait = WSAWaitForMultipleEvents(2, waits, FALSE, WSA_INFINITE, FALSE);
        if (wait == WSA_WAIT_EVENT_0) { We_Post(st->frame, L"Stopped.\r\n"); break; }
        if (wait != WSA_WAIT_EVENT_0 + 1) break;
        {
            WSANETWORKEVENTS ev;
            if (WSAEnumNetworkEvents(s, hEvent, &ev) == SOCKET_ERROR) {
                We_Post(st->frame, L"WSAEnumNetworkEvents failed.\r\n");
                break;
            }
            if (ev.lNetworkEvents & FD_CONNECT) {
                if (ev.iErrorCode[FD_CONNECT_BIT]) {
                    wchar_t buf[80];
                    swprintf_s(buf, 80, L"Connect error %d.\r\n",
                               ev.iErrorCode[FD_CONNECT_BIT]);
                    We_Post(st->frame, buf);
                    break;
                }
                We_Post(st->frame, L"Connected. Sending request...\r\n");
            }
            if (ev.lNetworkEvents & FD_WRITE && !sentRequest) {
                char req[400];
                int len;
                len = sprintf_s(req, 400,
                    "GET / HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
                    host);
                if (send(s, req, len, 0) > 0) {
                    sentRequest = TRUE;
                    We_Post(st->frame, L"Request sent.\r\n");
                }
            }
            if (ev.lNetworkEvents & FD_READ) {
                char buf[4096];
                int n;
                while ((n = recv(s, buf, sizeof(buf), 0)) > 0) {
                    /* Just count and log the first 200 chars of first chunk */
                    static int totalIn = 0;
                    static BOOL preview = FALSE;
                    wchar_t line[300];
                    totalIn += n;
                    if (!preview && n > 8) {
                        wchar_t snippet[60];
                        int snipBytes = n > 50 ? 50 : n;
                        MultiByteToWideChar(CP_UTF8, 0, buf, snipBytes,
                                             snippet, 60);
                        snippet[snipBytes] = 0;
                        swprintf_s(line, 300, L"  first bytes: \"%s\"\r\n", snippet);
                        We_Post(st->frame, line);
                        preview = TRUE;
                    }
                    swprintf_s(line, 300, L"  received %d bytes (total %d)\r\n",
                               n, totalIn);
                    We_Post(st->frame, line);
                }
            }
            if (ev.lNetworkEvents & FD_CLOSE) {
                We_Post(st->frame, L"Server closed.\r\n");
                break;
            }
        }
    }

done:
    if (result) freeaddrinfo(result);
    if (hEvent != WSA_INVALID_EVENT) WSACloseEvent(hEvent);
    if (s != INVALID_SOCKET) closesocket(s);
    PostMessageW(st->frame, WM_WSA_DONE, 0, 0);
    return 0;
}

static void We_Start(WeState *st)
{
    DWORD tid;
    if (st->thread) return;
    ResetEvent(st->stopEvent);
    SetWindowTextW(st->output, L"");
    st->thread = CreateThread(NULL, 0, We_Worker, st, 0, &tid);
}

static void We_Stop(WeState *st)
{
    if (!st->thread) return;
    SetEvent(st->stopEvent);
    WaitForSingleObject(st->thread, 2000);
    CloseHandle(st->thread);
    st->thread = NULL;
}

static void We_Append(WeState *st, const wchar_t *t)
{
    int len = GetWindowTextLengthW(st->output);
    SendMessageW(st->output, EM_SETSEL, len, len);
    SendMessageW(st->output, EM_REPLACESEL, FALSE, (LPARAM)t);
    SendMessageW(st->output, EM_SCROLLCARET, 0, 0);
}

static LRESULT CALLBACK We_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    WeState *st = (WeState *)GetPropW(hwnd, WE_PROP);
    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_WE_GO)   { We_Start(st); return 0; }
        if (LOWORD(wp) == ID_WE_STOP) { We_Stop(st);  return 0; }
    }
    if (msg == WM_WSA_LOG && st) {
        wchar_t *p = (wchar_t *)lp;
        if (p) { We_Append(st, p); free(p); }
        return 0;
    }
    if (msg == WM_WSA_DONE && st) {
        if (st->thread) { CloseHandle(st->thread); st->thread = NULL; }
        We_Append(st, L"[worker exited]\r\n");
        return 0;
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->hostEdit, 12, 38, w - 230, 24, TRUE);
        MoveWindow(st->goBtn,    w - 216, 38, 100, 24, TRUE);
        MoveWindow(st->stopBtn,  w - 110, 38, 90, 24, TRUE);
        MoveWindow(st->output,   8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        We_Stop(st);
        if (st->stopEvent) CloseHandle(st->stopEvent);
        if (st->wsaOk) WSACleanup();
        free(st); RemovePropW(hwnd, WE_PROP);
    }
    return CallWindowProcW(g_origWeFrame, hwnd, msg, wp, lp);
}

static HWND WsaEvent_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    WeState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"WsaEvent",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (WeState *)calloc(1, sizeof(WeState));
    if (!st) { DestroyWindow(frame); return NULL; }
    st->frame = frame;
    st->wsaOk = (WSAStartup(MAKEWORD(2, 2), &st->wsa) == 0);
    st->stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    st->hostEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"example.com",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        12, 38, w - 230, 24, frame, (HMENU)(LONG_PTR)ID_WE_HOST, hInstance, NULL);
    st->goBtn = CreateWindowExW(0, L"BUTTON", L"GET",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        w - 216, 38, 100, 24, frame, (HMENU)(LONG_PTR)ID_WE_GO, hInstance, NULL);
    st->stopBtn = CreateWindowExW(0, L"BUTTON", L"Stop",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        w - 110, 38, 90, 24, frame, (HMENU)(LONG_PTR)ID_WE_STOP, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_WE_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, WE_PROP, (HANDLE)st);
    if (!g_origWeFrame) g_origWeFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)We_FrameProc);
    return frame;
}

MsApp g_AppWsaEvent = { L"WsaEvent", WsaEvent_Create, 640, 460 };
