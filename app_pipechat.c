/*
 * app_pipechat.c — Inter-process chat via named pipes
 *
 * Demonstrates Windows named-pipe IPC:
 *   - One instance acts as SERVER: CreateNamedPipeW + ConnectNamedPipe on a
 *     worker thread, then ReadFile in a loop
 *   - Another instance acts as CLIENT: CreateFileW on \\.\pipe\... +
 *     WriteFile from the UI thread when the user types and hits Enter
 *
 * The two instances communicate via \\.\pipe\MiniShell_Chat. The first
 * instance launched becomes the server automatically (the server-thread
 * creates the pipe; the second instance opens it as a client).
 *
 * Messages received are posted to the UI thread via PostMessage with a
 * heap-allocated wide string (UI thread frees).
 */

#include "shell.h"
#include <stdlib.h>
#include <stdio.h>

#define PC_PROP    L"MS_PC_STATE"
#define ID_PC_IN   21001
#define ID_PC_OUT  21002
#define ID_PC_SEND 21003
#define ID_PC_MODE 21004

#define WM_PC_LINE  (WM_USER + 100)   /* wparam = heap wchar_t* */
#define WM_PC_INFO  (WM_USER + 101)

#define PIPE_NAME   L"\\\\.\\pipe\\MiniShell_Chat"

typedef enum { PC_UNKNOWN, PC_SERVER, PC_CLIENT } PcRole;

typedef struct {
    HWND   inputEdit, output, sendBtn, modeLbl;
    HANDLE pipe;           /* server: instance handle; client: file handle */
    HANDLE workerThread;
    HANDLE stopEvent;
    PcRole role;
} PcState;

static WNDPROC g_origPcFrame = NULL;

static void PcPostLine(HWND target, UINT msg, const wchar_t *text)
{
    size_t len = wcslen(text);
    wchar_t *copy = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
    if (!copy) return;
    wcscpy_s(copy, len + 1, text);
    if (!PostMessageW(target, msg, (WPARAM)copy, 0)) free(copy);
}

/* Server worker: create pipe, wait for client, then read forever */
static DWORD WINAPI Pc_ServerWorker(LPVOID arg)
{
    PcState *st = (PcState *)arg;
    HANDLE pipe;
    char buf[1024];
    DWORD nRead;

    pipe = CreateNamedPipeW(PIPE_NAME,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1, 4096, 4096, 0, NULL);
    if (pipe == INVALID_HANDLE_VALUE) {
        PcPostLine(GetParent(st->output), WM_PC_INFO, L"[server: CreateNamedPipe failed]\r\n");
        return 1;
    }
    st->pipe = pipe;
    PcPostLine(GetParent(st->output), WM_PC_INFO, L"[server: waiting for client...]\r\n");

    if (!ConnectNamedPipe(pipe, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) {
        PcPostLine(GetParent(st->output), WM_PC_INFO, L"[server: ConnectNamedPipe failed]\r\n");
        return 1;
    }
    PcPostLine(GetParent(st->output), WM_PC_INFO, L"[server: client connected]\r\n");

    while (WaitForSingleObject(st->stopEvent, 0) != WAIT_OBJECT_0) {
        if (!ReadFile(pipe, buf, sizeof(buf) - 1, &nRead, NULL) || nRead == 0) break;
        buf[nRead] = 0;
        {
            wchar_t wbuf[1100];
            wchar_t line[1200];
            int cw = MultiByteToWideChar(CP_UTF8, 0, buf, (int)nRead, wbuf, 1099);
            wbuf[cw] = 0;
            swprintf_s(line, 1200, L"<peer> %s\r\n", wbuf);
            PcPostLine(GetParent(st->output), WM_PC_LINE, line);
        }
    }
    return 0;
}

/* Client worker: connect to the existing pipe, then read forever */
static DWORD WINAPI Pc_ClientWorker(LPVOID arg)
{
    PcState *st = (PcState *)arg;
    HANDLE pipe;
    char buf[1024];
    DWORD nRead;

    pipe = CreateFileW(PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
                       0, NULL, OPEN_EXISTING, 0, NULL);
    if (pipe == INVALID_HANDLE_VALUE) return 1;

    {
        DWORD mode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(pipe, &mode, NULL, NULL);
    }
    st->pipe = pipe;
    PcPostLine(GetParent(st->output), WM_PC_INFO, L"[client: connected to server]\r\n");

    while (WaitForSingleObject(st->stopEvent, 0) != WAIT_OBJECT_0) {
        if (!ReadFile(pipe, buf, sizeof(buf) - 1, &nRead, NULL) || nRead == 0) break;
        buf[nRead] = 0;
        {
            wchar_t wbuf[1100];
            wchar_t line[1200];
            int cw = MultiByteToWideChar(CP_UTF8, 0, buf, (int)nRead, wbuf, 1099);
            wbuf[cw] = 0;
            swprintf_s(line, 1200, L"<peer> %s\r\n", wbuf);
            PcPostLine(GetParent(st->output), WM_PC_LINE, line);
        }
    }
    return 0;
}

static void Pc_Append(HWND output, const wchar_t *t)
{
    int len = GetWindowTextLengthW(output);
    SendMessageW(output, EM_SETSEL, len, len);
    SendMessageW(output, EM_REPLACESEL, FALSE, (LPARAM)t);
    SendMessageW(output, EM_SCROLLCARET, 0, 0);
}

static void Pc_Send(PcState *st)
{
    wchar_t text[512];
    char    bytes[1100];
    int     n;
    DWORD   nWritten;
    wchar_t echo[600];

    if (!st->pipe) return;
    GetWindowTextW(st->inputEdit, text, 512);
    if (text[0] == 0) return;
    SetWindowTextW(st->inputEdit, L"");

    n = WideCharToMultiByte(CP_UTF8, 0, text, -1, bytes, sizeof(bytes), NULL, NULL);
    if (n <= 0) return;
    WriteFile(st->pipe, bytes, n - 1, &nWritten, NULL);

    swprintf_s(echo, 600, L"<me> %s\r\n", text);
    Pc_Append(st->output, echo);
}

static LRESULT CALLBACK Pc_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    PcState *st = (PcState *)GetPropW(hwnd, PC_PROP);

    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_PC_SEND) {
        Pc_Send(st);
        return 0;
    }
    if ((msg == WM_PC_LINE || msg == WM_PC_INFO) && st) {
        wchar_t *txt = (wchar_t *)wp;
        if (txt) { Pc_Append(st->output, txt); free(txt); }
        return 0;
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->modeLbl,   8, 32, w - 16, 18, TRUE);
        MoveWindow(st->output,    8, 54, w - 16, h - 96, TRUE);
        MoveWindow(st->inputEdit, 8, h - 36, w - 100, 24, TRUE);
        MoveWindow(st->sendBtn,   w - 88, h - 36, 80, 24, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        SetEvent(st->stopEvent);
        if (st->pipe) {
            CancelIoEx(st->pipe, NULL);
            if (st->role == PC_SERVER) DisconnectNamedPipe(st->pipe);
            CloseHandle(st->pipe);
        }
        if (st->workerThread) {
            WaitForSingleObject(st->workerThread, 1500);
            CloseHandle(st->workerThread);
        }
        if (st->stopEvent) CloseHandle(st->stopEvent);
        free(st);
        RemovePropW(hwnd, PC_PROP);
    }
    return CallWindowProcW(g_origPcFrame, hwnd, msg, wp, lp);
}

/* Subclass the input EDIT so Enter = send */
static WNDPROC g_origPcInput = NULL;

static LRESULT CALLBACK Pc_InputProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        HWND frame = GetParent(hwnd);
        PcState *st = (PcState *)GetPropW(frame, PC_PROP);
        if (st) Pc_Send(st);
        return 0;
    }
    if (msg == WM_CHAR && wp == VK_RETURN) return 0;
    return CallWindowProcW(g_origPcInput, hwnd, msg, wp, lp);
}

static HWND PipeChat_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    PcState *st;
    DWORD tid;
    HANDLE probe;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"PipeChat",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (PcState *)calloc(1, sizeof(PcState));
    if (!st) { DestroyWindow(frame); return NULL; }
    st->stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    st->modeLbl = CreateWindowExW(0, L"STATIC", L"(deciding role…)",
        WS_CHILD | WS_VISIBLE,
        8, 32, w - 16, 18, frame, (HMENU)(LONG_PTR)ID_PC_MODE, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        8, 54, w - 16, h - 96, frame, (HMENU)(LONG_PTR)ID_PC_OUT, hInstance, NULL);

    st->inputEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        8, h - 36, w - 100, 24, frame, (HMENU)(LONG_PTR)ID_PC_IN, hInstance, NULL);

    st->sendBtn = CreateWindowExW(0, L"BUTTON", L"Send",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        w - 88, h - 36, 80, 24, frame, (HMENU)(LONG_PTR)ID_PC_SEND, hInstance, NULL);

    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, PC_PROP, (HANDLE)st);
    if (!g_origPcFrame) g_origPcFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Pc_FrameProc);

    g_origPcInput = (WNDPROC)GetWindowLongPtrW(st->inputEdit, GWLP_WNDPROC);
    SetWindowLongPtrW(st->inputEdit, GWLP_WNDPROC, (LONG_PTR)Pc_InputProc);

    /* Decide role: try to open the pipe as a client. If that works,
     * we're the second instance — become CLIENT. Otherwise become SERVER. */
    probe = CreateFileW(PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
                        0, NULL, OPEN_EXISTING, 0, NULL);
    if (probe != INVALID_HANDLE_VALUE) {
        CloseHandle(probe);
        st->role = PC_CLIENT;
        SetWindowTextW(st->modeLbl, L"Role: CLIENT (a server is already running)");
        st->workerThread = CreateThread(NULL, 0, Pc_ClientWorker, st, 0, &tid);
    } else {
        st->role = PC_SERVER;
        SetWindowTextW(st->modeLbl,
            L"Role: SERVER — launch a second PipeChat instance to connect");
        st->workerThread = CreateThread(NULL, 0, Pc_ServerWorker, st, 0, &tid);
    }
    return frame;
}

MsApp g_AppPipeChat = {
    L"PipeChat",
    PipeChat_Create,
    520, 380
};
