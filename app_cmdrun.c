/*
 * app_cmdrun.c — Run a shell command with captured output
 *
 * Demonstrates:
 *   - CreatePipe (anonymous pipe with inherited read/write handles)
 *   - SetHandleInformation to make the inheritable handle for the child
 *   - CreateProcessW with STARTF_USESTDHANDLES redirecting stdout/stderr
 *   - Worker thread that reads from the pipe and forwards lines to the UI
 *     via PostMessage with heap-allocated wide strings (consumer frees)
 *
 * Type a command, hit Run. The child runs as `cmd.exe /C <command>` so it
 * supports built-ins like `dir`, pipes, redirection.
 */

#include "shell.h"
#include <stdlib.h>
#include <stdio.h>

#define CR_PROP    L"MS_CR_STATE"
#define ID_CR_CMD  13001
#define ID_CR_RUN  13002
#define ID_CR_OUT  13003

#define WM_CR_LINE  (WM_USER + 60)   /* wparam = wchar_t* (UI frees) */
#define WM_CR_DONE  (WM_USER + 61)

typedef struct {
    HWND   cmdEdit;
    HWND   runBtn;
    HWND   output;
    HANDLE thread;
    HANDLE childOut;     /* read end of the pipe (parent side) */
    HANDLE childProcess;
} CrState;

typedef struct {
    HWND   target;
    HANDLE readPipe;
} CrTask;

static WNDPROC g_origCrFrame = NULL;

static void Cr_Append(HWND output, const wchar_t *text)
{
    int len = GetWindowTextLengthW(output);
    SendMessageW(output, EM_SETSEL, len, len);
    SendMessageW(output, EM_REPLACESEL, FALSE, (LPARAM)text);
    SendMessageW(output, EM_SCROLLCARET, 0, 0);
}

static void PostLine(HWND target, const wchar_t *text)
{
    size_t len = wcslen(text);
    wchar_t *copy = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
    if (!copy) return;
    wcscpy_s(copy, len + 1, text);
    if (!PostMessageW(target, WM_CR_LINE, (WPARAM)copy, 0)) free(copy);
}

static DWORD WINAPI Cr_Worker(LPVOID arg)
{
    CrTask *task = (CrTask *)arg;
    char buf[2048];
    DWORD nRead;
    BOOL ok;
    while ((ok = ReadFile(task->readPipe, buf, sizeof(buf) - 1, &nRead, NULL))
            && nRead > 0) {
        wchar_t wbuf[2100];
        int cw;
        buf[nRead] = 0;
        /* cmd.exe outputs the system code page (usually CP_OEMCP) */
        cw = MultiByteToWideChar(CP_OEMCP, 0, buf, (int)nRead, wbuf, 2099);
        wbuf[cw] = 0;
        PostLine(task->target, wbuf);
    }
    PostMessageW(task->target, WM_CR_DONE, 0, 0);
    free(task);
    return 0;
}

static void Cr_Run(HWND frame, CrState *st)
{
    HANDLE readPipe = NULL, writePipe = NULL;
    SECURITY_ATTRIBUTES sa;
    PROCESS_INFORMATION pi;
    STARTUPINFOW si;
    wchar_t command[512];
    wchar_t cmdLine[600];
    DWORD tid;
    CrTask *task;

    if (st->thread) return;   /* already running */
    GetWindowTextW(st->cmdEdit, command, 512);
    if (command[0] == 0) return;

    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        Cr_Append(st->output, L"[pipe creation failed]\r\n");
        return;
    }
    /* Ensure the read end isn't inherited by the child */
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    swprintf_s(cmdLine, 600, L"cmd.exe /C %s", command);

    ZeroMemory(&si, sizeof(si));
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = writePipe;
    si.hStdError  = writePipe;
    si.hStdInput  = NULL;
    si.wShowWindow = SW_HIDE;

    SetWindowTextW(st->output, L"");
    Cr_Append(st->output, L"> ");
    Cr_Append(st->output, command);
    Cr_Append(st->output, L"\r\n");

    if (!CreateProcessW(NULL, cmdLine, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        Cr_Append(st->output, L"[CreateProcess failed]\r\n");
        CloseHandle(readPipe); CloseHandle(writePipe);
        return;
    }

    /* Parent must close its copy of the write end so ReadFile returns EOF
     * when the child exits. */
    CloseHandle(writePipe);
    CloseHandle(pi.hThread);
    st->childProcess = pi.hProcess;
    st->childOut     = readPipe;

    task = (CrTask *)calloc(1, sizeof(CrTask));
    if (!task) {
        CloseHandle(readPipe);
        return;
    }
    task->target   = frame;
    task->readPipe = readPipe;
    st->thread = CreateThread(NULL, 0, Cr_Worker, task, 0, &tid);
    EnableWindow(st->runBtn, FALSE);
    if (!st->thread) {
        free(task);
        EnableWindow(st->runBtn, TRUE);
    }
}

static LRESULT CALLBACK Cr_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    CrState *st = (CrState *)GetPropW(hwnd, CR_PROP);

    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_CR_RUN) {
        Cr_Run(hwnd, st);
        return 0;
    }
    if (msg == WM_CR_LINE && st) {
        wchar_t *txt = (wchar_t *)wp;
        if (txt) { Cr_Append(st->output, txt); free(txt); }
        return 0;
    }
    if (msg == WM_CR_DONE && st) {
        Cr_Append(st->output, L"\r\n[done]\r\n");
        EnableWindow(st->runBtn, TRUE);
        if (st->thread) {
            WaitForSingleObject(st->thread, 500);
            CloseHandle(st->thread);
            st->thread = NULL;
        }
        if (st->childProcess) {
            WaitForSingleObject(st->childProcess, 500);
            CloseHandle(st->childProcess);
            st->childProcess = NULL;
        }
        if (st->childOut) {
            CloseHandle(st->childOut);
            st->childOut = NULL;
        }
        return 0;
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->cmdEdit, 8,        34, w - 100, 24, TRUE);
        MoveWindow(st->runBtn,  w - 84,   34, 76,      24, TRUE);
        MoveWindow(st->output,  8,        66, w - 16,  h - 74, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->childProcess) {
            TerminateProcess(st->childProcess, 1);
            CloseHandle(st->childProcess);
        }
        if (st->thread) {
            WaitForSingleObject(st->thread, 1000);
            CloseHandle(st->thread);
        }
        if (st->childOut) CloseHandle(st->childOut);
        free(st);
        RemovePropW(hwnd, CR_PROP);
    }
    return CallWindowProcW(g_origCrFrame, hwnd, msg, wp, lp);
}

static HWND CmdRun_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    CrState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"CmdRun",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (CrState *)calloc(1, sizeof(CrState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->cmdEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"dir",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        8, 34, w - 100, 24, frame, (HMENU)(LONG_PTR)ID_CR_CMD, hInstance, NULL);

    st->runBtn = CreateWindowExW(0, L"BUTTON", L"Run",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        w - 84, 34, 76, 24, frame, (HMENU)(LONG_PTR)ID_CR_RUN, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        8, 66, w - 16, h - 74, frame, (HMENU)(LONG_PTR)ID_CR_OUT, hInstance, NULL);

    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, CR_PROP, (HANDLE)st);
    if (!g_origCrFrame)
        g_origCrFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Cr_FrameProc);
    return frame;
}

MsApp g_AppCmdRun = {
    L"CmdRun",
    CmdRun_Create,
    600, 420
};
