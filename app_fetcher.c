/*
 * app_fetcher.c — HTTP fetcher (Winsock + worker thread)
 *
 * Demonstrates:
 *   - Winsock client (WSAStartup, getaddrinfo, socket, connect, send, recv)
 *   - Worker thread (CreateThread)
 *   - Cross-thread UI updates via PostMessage with allocated payloads
 *     (the UI thread frees them after consuming)
 *
 * The user types a hostname; the worker connects on port 80, sends a minimal
 * HTTP GET, and streams the response into the result EDIT control.
 */

#include "shell.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "ws2_32.lib")

#define FETCH_PROP    L"MS_FETCH_STATE"
#define ID_HOST       6001
#define ID_GO         6002
#define ID_RESULT     6003

/* Custom messages from worker → UI thread */
#define WM_FETCH_LINE   (WM_USER + 50)   /* wparam = (wchar_t*) heap line (UI frees) */
#define WM_FETCH_DONE   (WM_USER + 51)
#define WM_FETCH_ERROR  (WM_USER + 52)   /* wparam = (wchar_t*) heap msg */

typedef struct {
    HWND   frame;
    HWND   hostEdit;
    HWND   goBtn;
    HWND   result;
    HANDLE thread;
    BOOL   wsaInited;
} FetchState;

typedef struct {
    HWND    target;
    wchar_t host[256];
} FetchTask;

static WNDPROC g_origFetchFrame = NULL;

static void Fetch_AppendW(HWND result, const wchar_t *text)
{
    int len = GetWindowTextLengthW(result);
    SendMessageW(result, EM_SETSEL, len, len);
    SendMessageW(result, EM_REPLACESEL, FALSE, (LPARAM)text);
}

/* Helper: send a heap-allocated wchar_t* to the UI thread. */
static void PostLine(HWND target, UINT msg, const wchar_t *text)
{
    size_t len = wcslen(text);
    wchar_t *copy = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
    if (!copy) return;
    wcscpy_s(copy, len + 1, text);
    if (!PostMessageW(target, msg, (WPARAM)copy, 0)) {
        free(copy);
    }
}

static DWORD WINAPI Fetch_Worker(LPVOID arg)
{
    FetchTask *task = (FetchTask *)arg;
    HWND target = task->target;
    struct addrinfoW hints, *res = NULL, *p;
    SOCKET sock = INVALID_SOCKET;
    char request[1024];
    char hostA[256];
    int n;
    wchar_t line[1024];
    char recvBuf[2048];
    int total = 0;

    WideCharToMultiByte(CP_UTF8, 0, task->host, -1, hostA, sizeof(hostA), NULL, NULL);

    swprintf_s(line, 1024, L"Resolving %s...\r\n", task->host);
    PostLine(target, WM_FETCH_LINE, line);

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    if (GetAddrInfoW(task->host, L"80", &hints, &res) != 0 || !res) {
        PostLine(target, WM_FETCH_ERROR, L"Resolution failed.\r\n");
        free(task);
        return 1;
    }

    for (p = res; p; p = p->ai_next) {
        sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock == INVALID_SOCKET) continue;
        if (connect(sock, p->ai_addr, (int)p->ai_addrlen) == 0) break;
        closesocket(sock);
        sock = INVALID_SOCKET;
    }
    FreeAddrInfoW(res);

    if (sock == INVALID_SOCKET) {
        PostLine(target, WM_FETCH_ERROR, L"Connect failed.\r\n");
        free(task);
        return 1;
    }

    PostLine(target, WM_FETCH_LINE, L"Connected. Sending GET /...\r\n\r\n");

    n = sprintf_s(request, 1024,
        "GET / HTTP/1.0\r\nHost: %s\r\nUser-Agent: MiniShell/1.0\r\nConnection: close\r\n\r\n",
        hostA);
    send(sock, request, n, 0);

    while ((n = recv(sock, recvBuf, sizeof(recvBuf) - 1, 0)) > 0) {
        recvBuf[n] = 0;
        total += n;
        if (total < 8192) {  /* clamp displayed payload */
            wchar_t chunk[2100];
            int cw = MultiByteToWideChar(CP_UTF8, 0, recvBuf, n, chunk, 2099);
            chunk[cw] = 0;
            PostLine(target, WM_FETCH_LINE, chunk);
        }
    }
    {
        wchar_t footer[64];
        swprintf_s(footer, 64, L"\r\n\r\n[done — %d bytes]\r\n", total);
        PostLine(target, WM_FETCH_LINE, footer);
    }

    closesocket(sock);
    PostMessageW(target, WM_FETCH_DONE, 0, 0);
    free(task);
    return 0;
}

static void Fetch_Go(FetchState *st)
{
    FetchTask *task;
    DWORD tid;

    if (st->thread) {
        /* Already running — refuse */
        return;
    }
    task = (FetchTask *)calloc(1, sizeof(FetchTask));
    if (!task) return;
    task->target = st->frame;
    GetWindowTextW(st->hostEdit, task->host, 256);
    if (task->host[0] == 0) {
        free(task);
        return;
    }

    SetWindowTextW(st->result, L"");
    EnableWindow(st->goBtn, FALSE);

    st->thread = CreateThread(NULL, 0, Fetch_Worker, task, 0, &tid);
    if (!st->thread) {
        free(task);
        EnableWindow(st->goBtn, TRUE);
    }
}

static LRESULT CALLBACK Fetch_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    FetchState *st = (FetchState *)GetPropW(hwnd, FETCH_PROP);

    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_GO) {
            Fetch_Go(st);
            return 0;
        }
    }
    if (msg == WM_FETCH_LINE && st) {
        wchar_t *text = (wchar_t *)wp;
        if (text) {
            Fetch_AppendW(st->result, text);
            free(text);
        }
        return 0;
    }
    if (msg == WM_FETCH_ERROR && st) {
        wchar_t *text = (wchar_t *)wp;
        if (text) {
            Fetch_AppendW(st->result, text);
            free(text);
        }
        EnableWindow(st->goBtn, TRUE);
        if (st->thread) { CloseHandle(st->thread); st->thread = NULL; }
        return 0;
    }
    if (msg == WM_FETCH_DONE && st) {
        EnableWindow(st->goBtn, TRUE);
        if (st->thread) { CloseHandle(st->thread); st->thread = NULL; }
        return 0;
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->hostEdit, 8,        34, w - 100, 24, TRUE);
        MoveWindow(st->goBtn,    w - 84,   34, 76,      24, TRUE);
        MoveWindow(st->result,   8,        66, w - 16,  h - 74, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->thread) {
            WaitForSingleObject(st->thread, 1000);
            CloseHandle(st->thread);
        }
        if (st->wsaInited) WSACleanup();
        free(st);
        RemovePropW(hwnd, FETCH_PROP);
    }
    return CallWindowProcW(g_origFetchFrame, hwnd, msg, wp, lp);
}

static HWND Fetcher_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    FetchState *st;
    WSADATA wsa;
    HFONT mono;

    (void)self;
    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Fetch",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (FetchState *)calloc(1, sizeof(FetchState));
    if (!st) { DestroyWindow(frame); return NULL; }
    st->frame = frame;
    st->wsaInited = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);

    st->hostEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"example.com",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        8, 34, w - 100, 24, frame, (HMENU)(LONG_PTR)ID_HOST, hInstance, NULL);

    st->goBtn = CreateWindowExW(0, L"BUTTON", L"Fetch",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        w - 84, 34, 76, 24, frame, (HMENU)(LONG_PTR)ID_GO, hInstance, NULL);

    st->result = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        8, 66, w - 16, h - 74, frame, (HMENU)(LONG_PTR)ID_RESULT, hInstance, NULL);

    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->result, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, FETCH_PROP, (HANDLE)st);
    if (!g_origFetchFrame)
        g_origFetchFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Fetch_FrameProc);
    return frame;
}

MsApp g_AppFetcher = {
    L"Fetch",
    Fetcher_Create,
    600, 400
};
