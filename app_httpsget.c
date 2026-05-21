/*
 * app_httpsget.c — HTTPS client via WinHTTP
 *
 * Demonstrates the WinHTTP stack — the proper way to do HTTPS on Windows
 * without rolling your own TLS:
 *   - WinHttpOpen → session handle
 *   - WinHttpConnect → connection handle (host, port 443)
 *   - WinHttpOpenRequest with WINHTTP_FLAG_SECURE for TLS
 *   - WinHttpSendRequest + WinHttpReceiveResponse
 *   - WinHttpQueryHeaders for the status line
 *   - WinHttpReadData in a loop, all on a worker thread
 *   - Progress reported to UI via PostMessage
 */

#include "shell.h"
#include <winhttp.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "winhttp.lib")

#define HG_PROP    L"MS_HG_STATE"
#define ID_HG_HOST 20001
#define ID_HG_PATH 20002
#define ID_HG_GO   20003
#define ID_HG_OUT  20004

#define WM_HG_LINE  (WM_USER + 90)
#define WM_HG_DONE  (WM_USER + 91)
#define WM_HG_ERR   (WM_USER + 92)

typedef struct {
    HWND host, path, output, goBtn;
    HANDLE thread;
} HgState;

typedef struct {
    HWND    target;
    wchar_t host[256];
    wchar_t path[512];
} HgTask;

static WNDPROC g_origHgFrame = NULL;

static void HgPostLine(HWND target, UINT msg, const wchar_t *text)
{
    size_t len = wcslen(text);
    wchar_t *copy = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
    if (!copy) return;
    wcscpy_s(copy, len + 1, text);
    if (!PostMessageW(target, msg, (WPARAM)copy, 0)) free(copy);
}

static DWORD WINAPI Hg_Worker(LPVOID arg)
{
    HgTask *task = (HgTask *)arg;
    HINTERNET session = NULL, conn = NULL, req = NULL;
    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    wchar_t line[1024];
    char buf[4096];
    DWORD nRead;
    DWORD total = 0;
    BOOL ok;

    swprintf_s(line, 1024, L"GET https://%s%s\r\n\r\n", task->host, task->path);
    HgPostLine(task->target, WM_HG_LINE, line);

    session = WinHttpOpen(L"MiniShell/1.0",
                          WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { HgPostLine(task->target, WM_HG_ERR, L"WinHttpOpen failed\r\n"); goto cleanup; }

    conn = WinHttpConnect(session, task->host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn) { HgPostLine(task->target, WM_HG_ERR, L"WinHttpConnect failed\r\n"); goto cleanup; }

    req = WinHttpOpenRequest(conn, L"GET", task->path,
                             NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                             WINHTTP_FLAG_SECURE);
    if (!req) { HgPostLine(task->target, WM_HG_ERR, L"WinHttpOpenRequest failed\r\n"); goto cleanup; }

    ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!ok) { HgPostLine(task->target, WM_HG_ERR, L"WinHttpSendRequest failed\r\n"); goto cleanup; }

    ok = WinHttpReceiveResponse(req, NULL);
    if (!ok) { HgPostLine(task->target, WM_HG_ERR, L"WinHttpReceiveResponse failed\r\n"); goto cleanup; }

    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        NULL, &statusCode, &statusSize, NULL);
    swprintf_s(line, 1024, L"HTTP %lu\r\n\r\n", statusCode);
    HgPostLine(task->target, WM_HG_LINE, line);

    while (WinHttpReadData(req, buf, sizeof(buf) - 1, &nRead) && nRead > 0) {
        wchar_t wbuf[4100];
        int cw;
        buf[nRead] = 0;
        total += nRead;
        if (total < 12000) {
            cw = MultiByteToWideChar(CP_UTF8, 0, buf, (int)nRead, wbuf, 4099);
            wbuf[cw] = 0;
            HgPostLine(task->target, WM_HG_LINE, wbuf);
        }
    }
    swprintf_s(line, 1024, L"\r\n\r\n[done — %lu bytes]\r\n", total);
    HgPostLine(task->target, WM_HG_LINE, line);

cleanup:
    if (req)     WinHttpCloseHandle(req);
    if (conn)    WinHttpCloseHandle(conn);
    if (session) WinHttpCloseHandle(session);
    PostMessageW(task->target, WM_HG_DONE, 0, 0);
    free(task);
    return 0;
}

static void Hg_Append(HWND output, const wchar_t *t)
{
    int len = GetWindowTextLengthW(output);
    SendMessageW(output, EM_SETSEL, len, len);
    SendMessageW(output, EM_REPLACESEL, FALSE, (LPARAM)t);
    SendMessageW(output, EM_SCROLLCARET, 0, 0);
}

static void Hg_Go(HWND frame, HgState *st)
{
    HgTask *task;
    DWORD tid;
    if (st->thread) return;

    task = (HgTask *)calloc(1, sizeof(HgTask));
    if (!task) return;
    task->target = frame;
    GetWindowTextW(st->host, task->host, 256);
    GetWindowTextW(st->path, task->path, 512);
    if (task->host[0] == 0 || task->path[0] == 0) { free(task); return; }

    SetWindowTextW(st->output, L"");
    EnableWindow(st->goBtn, FALSE);
    st->thread = CreateThread(NULL, 0, Hg_Worker, task, 0, &tid);
    if (!st->thread) { free(task); EnableWindow(st->goBtn, TRUE); }
}

static LRESULT CALLBACK Hg_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    HgState *st = (HgState *)GetPropW(hwnd, HG_PROP);

    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_HG_GO) { Hg_Go(hwnd, st); return 0; }
    if ((msg == WM_HG_LINE || msg == WM_HG_ERR) && st) {
        wchar_t *txt = (wchar_t *)wp;
        if (txt) { Hg_Append(st->output, txt); free(txt); }
        return 0;
    }
    if (msg == WM_HG_DONE && st) {
        EnableWindow(st->goBtn, TRUE);
        if (st->thread) { WaitForSingleObject(st->thread, 500);
                          CloseHandle(st->thread); st->thread = NULL; }
        return 0;
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->host,   8,        34, w / 2 - 12, 24, TRUE);
        MoveWindow(st->path,   w / 2 + 4, 34, w / 2 - 100, 24, TRUE);
        MoveWindow(st->goBtn,  w - 92,   34, 84,         24, TRUE);
        MoveWindow(st->output, 8,        66, w - 16,     h - 74, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->thread) { WaitForSingleObject(st->thread, 1000); CloseHandle(st->thread); }
        free(st);
        RemovePropW(hwnd, HG_PROP);
    }
    return CallWindowProcW(g_origHgFrame, hwnd, msg, wp, lp);
}

static HWND HttpsGet_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    HgState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"HttpsGet",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (HgState *)calloc(1, sizeof(HgState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->host = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"example.com",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        8, 34, 200, 24, frame, (HMENU)(LONG_PTR)ID_HG_HOST, hInstance, NULL);
    st->path = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"/",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        216, 34, w - 320, 24, frame, (HMENU)(LONG_PTR)ID_HG_PATH, hInstance, NULL);
    st->goBtn = CreateWindowExW(0, L"BUTTON", L"GET",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        w - 92, 34, 84, 24, frame, (HMENU)(LONG_PTR)ID_HG_GO, hInstance, NULL);
    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        8, 66, w - 16, h - 74, frame, (HMENU)(LONG_PTR)ID_HG_OUT, hInstance, NULL);

    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, HG_PROP, (HANDLE)st);
    if (!g_origHgFrame) g_origHgFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Hg_FrameProc);
    return frame;
}

MsApp g_AppHttpsGet = {
    L"HttpsGet",
    HttpsGet_Create,
    640, 400
};
