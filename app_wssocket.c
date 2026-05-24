/*
 * app_wssocket.c — WinHTTP WebSocket client
 *
 * Demonstrates the WinHTTP WebSocket extensions (Win 8+) — the framework
 * Windows Store apps and built-in services use to speak the WebSocket
 * protocol without pulling in a third-party library. Unlike raw sockets
 * (WinSock2 in Batch 14), WinHTTP handles the HTTP-101 upgrade
 * handshake, RFC 6455 framing, masking, and ping/pong for us:
 *
 *   - WinHttpOpen / WinHttpConnect / WinHttpOpenRequest as for any HTTPS
 *     request
 *   - BEFORE WinHttpSendRequest: WinHttpSetOption(hReq,
 *     WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0) marks the request
 *     as a WebSocket upgrade
 *   - WinHttpSendRequest + WinHttpReceiveResponse perform the handshake;
 *     server should reply 101 Switching Protocols
 *   - WinHttpWebSocketCompleteUpgrade(hReq, 0) gets back the WebSocket
 *     handle that replaces the request handle for all further I/O
 *   - WinHttpWebSocketSend(hWs, BUFFER_TYPE, pvBuffer, cbBuffer) frames
 *     and sends a message; BUFFER_TYPE is _BINARY_MESSAGE or _UTF8_MESSAGE
 *   - WinHttpWebSocketReceive reads back a server frame
 *   - WinHttpWebSocketClose sends a Close frame
 *
 * Since the demo runs without a guaranteed-available endpoint, it
 * targets the public echo.websocket.events service.
 */

#include "shell.h"
#include <winhttp.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "winhttp.lib")

/* Some headers may not have these constants in older SDKs */
#ifndef WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET
#define WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET 114
#endif

#define WS_PROP   L"MS_WSK_STATE"
#define ID_WS_GO  112001
#define ID_WS_OUT 112002

#define WM_WSK_LINE (WM_USER + 230)
#define WM_WSK_DONE (WM_USER + 231)

typedef struct { HWND frame, output; HANDLE thread; } WsState;
static WNDPROC g_origWsFrame = NULL;

static void Wsk_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
    SendMessageW(e, EM_SCROLLCARET, 0, 0);
}

static void Wsk_Post(HWND f, const wchar_t *t)
{
    wchar_t *p = _wcsdup(t);
    if (p) PostMessageW(f, WM_WSK_LINE, 0, (LPARAM)p);
}

static DWORD WINAPI Wsk_Worker(LPVOID arg)
{
    WsState *st = (WsState *)arg;
    HINTERNET hSession = NULL, hConn = NULL, hReq = NULL, hWs = NULL;
    DWORD err = 0;

    hSession = WinHttpOpen(L"MiniShell/1.0 WebSocket",
                           WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { Wsk_Post(st->frame, L"WinHttpOpen failed.\r\n"); goto done; }

    hConn = WinHttpConnect(hSession, L"echo.websocket.events",
                           INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConn) { Wsk_Post(st->frame, L"WinHttpConnect failed.\r\n"); goto done; }

    hReq = WinHttpOpenRequest(hConn, L"GET", L"/", NULL,
                              WINHTTP_NO_REFERER,
                              WINHTTP_DEFAULT_ACCEPT_TYPES,
                              WINHTTP_FLAG_SECURE);
    if (!hReq) { Wsk_Post(st->frame, L"WinHttpOpenRequest failed.\r\n"); goto done; }

    if (!WinHttpSetOption(hReq, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0)) {
        Wsk_Post(st->frame, L"WinHttpSetOption(UPGRADE_TO_WEB_SOCKET) failed.\r\n");
        goto done;
    }
    if (!WinHttpSendRequest(hReq,
                            WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        wchar_t line[80];
        swprintf_s(line, 80, L"WinHttpSendRequest failed: %lu\r\n", GetLastError());
        Wsk_Post(st->frame, line);
        goto done;
    }
    if (!WinHttpReceiveResponse(hReq, NULL)) {
        wchar_t line[80];
        swprintf_s(line, 80, L"WinHttpReceiveResponse failed: %lu\r\n", GetLastError());
        Wsk_Post(st->frame, line);
        goto done;
    }
    {
        DWORD status = 0, cb = sizeof(status);
        WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            NULL, &status, &cb, WINHTTP_NO_HEADER_INDEX);
        {
            wchar_t line[80];
            swprintf_s(line, 80, L"HTTP status: %lu (expect 101)\r\n", status);
            Wsk_Post(st->frame, line);
        }
        if (status != 101) {
            Wsk_Post(st->frame, L"Server did not switch protocols.\r\n");
            goto done;
        }
    }

    hWs = WinHttpWebSocketCompleteUpgrade(hReq, 0);
    if (!hWs) {
        Wsk_Post(st->frame, L"WinHttpWebSocketCompleteUpgrade failed.\r\n");
        goto done;
    }
    WinHttpCloseHandle(hReq); hReq = NULL;
    Wsk_Post(st->frame, L"WebSocket upgrade complete.\r\n");

    /* Send a small message */
    {
        const char msg[] = "hello from MiniShell";
        err = WinHttpWebSocketSend(hWs,
                WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                (PVOID)msg, (DWORD)strlen(msg));
        if (err != ERROR_SUCCESS) {
            wchar_t line[80];
            swprintf_s(line, 80, L"WebSocketSend failed: %lu\r\n", err);
            Wsk_Post(st->frame, line);
        } else {
            Wsk_Post(st->frame, L"Sent: hello from MiniShell\r\n");
        }
    }

    /* Receive one frame */
    {
        BYTE buf[1024];
        DWORD got = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE bufType;
        err = WinHttpWebSocketReceive(hWs, buf, sizeof(buf) - 1, &got, &bufType);
        if (err == ERROR_SUCCESS) {
            wchar_t line[256];
            char asc[1024];
            memcpy(asc, buf, got);
            asc[got] = 0;
            swprintf_s(line, 256, L"Recv: type=%d size=%lu\r\n", bufType, got);
            Wsk_Post(st->frame, line);
            /* Print as UTF-8 if it is */
            {
                wchar_t wmsg[1024];
                MultiByteToWideChar(CP_UTF8, 0, asc, -1, wmsg, 1024);
                Wsk_Post(st->frame, L"  payload: ");
                Wsk_Post(st->frame, wmsg);
                Wsk_Post(st->frame, L"\r\n");
            }
        } else {
            wchar_t line[80];
            swprintf_s(line, 80, L"WebSocketReceive failed: %lu\r\n", err);
            Wsk_Post(st->frame, line);
        }
    }

    WinHttpWebSocketClose(hWs, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
    Wsk_Post(st->frame, L"Closed.\r\n");

done:
    if (hWs)      WinHttpCloseHandle(hWs);
    if (hReq)     WinHttpCloseHandle(hReq);
    if (hConn)    WinHttpCloseHandle(hConn);
    if (hSession) WinHttpCloseHandle(hSession);
    PostMessageW(st->frame, WM_WSK_DONE, 0, 0);
    return 0;
}

static LRESULT CALLBACK Wsk_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    WsState *st = (WsState *)GetPropW(hwnd, WS_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_WS_GO) {
        DWORD tid;
        if (st->thread) return 0;
        SetWindowTextW(st->output, L"");
        st->thread = CreateThread(NULL, 0, Wsk_Worker, st, 0, &tid);
        return 0;
    }
    if (msg == WM_WSK_LINE && st) {
        wchar_t *p = (wchar_t *)lp;
        if (p) { Wsk_Append(st->output, p); free(p); }
        return 0;
    }
    if (msg == WM_WSK_DONE && st) {
        if (st->thread) { CloseHandle(st->thread); st->thread = NULL; }
        Wsk_Append(st->output, L"\r\n[done]\r\n");
        return 0;
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->thread) { WaitForSingleObject(st->thread, 8000); CloseHandle(st->thread); }
        free(st); RemovePropW(hwnd, WS_PROP);
    }
    return CallWindowProcW(g_origWsFrame, hwnd, msg, wp, lp);
}

static HWND WsSocket_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    WsState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"WsSocket",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (WsState *)calloc(1, sizeof(WsState));
    if (!st) { DestroyWindow(frame); return NULL; }
    st->frame = frame;

    CreateWindowExW(0, L"BUTTON", L"Connect + echo",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 150, 26, frame, (HMENU)(LONG_PTR)ID_WS_GO, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"Connects to wss://echo.websocket.events/ and echoes a message.\r\n",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_WS_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, WS_PROP, (HANDLE)st);
    if (!g_origWsFrame) g_origWsFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Wsk_FrameProc);
    return frame;
}

MsApp g_AppWsSocket = { L"WsSocket", WsSocket_Create, 720, 440 };
