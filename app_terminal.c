/*
 * app_terminal.c — Tiny terminal with a small built-in command set
 *
 * Layout: a read-only EDIT for scrollback above an editable EDIT for the
 * prompt. Pressing Enter in the prompt runs the line through a tiny dispatch
 * table (echo, time, ls, pwd, cd, help, clear) and appends the result to the
 * scrollback.
 *
 * We deliberately avoid CreateProcess + pipes here — that's a whole network of
 * its own. The point is to show the input/output loop pattern.
 */

#include "shell.h"
#include <wchar.h>
#include <stdlib.h>
#include <time.h>

#define TERM_PROP_STATE L"MS_TERM_STATE"
#define TERM_ID_OUT     3001
#define TERM_ID_IN      3002

typedef struct {
    HWND    out;
    HWND    in;
    wchar_t cwd[MAX_PATH];
    WNDPROC origInputProc;
} TermState;

static WNDPROC g_origTermFrame = NULL;

static void Term_Append(TermState *st, const wchar_t *line)
{
    int len = GetWindowTextLengthW(st->out);
    SendMessageW(st->out, EM_SETSEL, len, len);
    SendMessageW(st->out, EM_REPLACESEL, FALSE, (LPARAM)line);
    SendMessageW(st->out, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    SendMessageW(st->out, EM_SCROLLCARET, 0, 0);
}

static void Term_RunCommand(TermState *st, const wchar_t *cmd)
{
    wchar_t prompt[MAX_PATH + 16];
    swprintf_s(prompt, MAX_PATH + 16, L"%s> %s", st->cwd, cmd);
    Term_Append(st, prompt);

    if (wcsncmp(cmd, L"echo ", 5) == 0) {
        Term_Append(st, cmd + 5);
    } else if (wcscmp(cmd, L"time") == 0) {
        time_t now;
        struct tm lt;
        wchar_t buf[64];
        time(&now);
        localtime_s(&lt, &now);
        swprintf_s(buf, 64, L"%04d-%02d-%02d %02d:%02d:%02d",
                   lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                   lt.tm_hour, lt.tm_min, lt.tm_sec);
        Term_Append(st, buf);
    } else if (wcscmp(cmd, L"pwd") == 0) {
        Term_Append(st, st->cwd);
    } else if (wcscmp(cmd, L"ls") == 0 || wcscmp(cmd, L"dir") == 0) {
        WIN32_FIND_DATAW fd;
        HANDLE hFind;
        wchar_t pattern[MAX_PATH + 4];
        swprintf_s(pattern, MAX_PATH + 4, L"%s\\*", st->cwd);
        hFind = FindFirstFileW(pattern, &fd);
        if (hFind == INVALID_HANDLE_VALUE) {
            Term_Append(st, L"  (empty or unreadable)");
        } else {
            do {
                wchar_t row[MAX_PATH + 8];
                if (wcscmp(fd.cFileName, L".") == 0) continue;
                swprintf_s(row, MAX_PATH + 8, L"  %s%s",
                    fd.cFileName,
                    (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? L"\\" : L"");
                Term_Append(st, row);
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }
    } else if (wcsncmp(cmd, L"cd ", 3) == 0) {
        wchar_t newPath[MAX_PATH];
        if (cmd[3] == L'\\' || (cmd[3] && cmd[4] == L':')) {
            wcscpy_s(newPath, MAX_PATH, cmd + 3);
        } else {
            swprintf_s(newPath, MAX_PATH, L"%s\\%s", st->cwd, cmd + 3);
        }
        if (SetCurrentDirectoryW(newPath) &&
            GetCurrentDirectoryW(MAX_PATH, st->cwd)) {
            /* Reset back: we don't actually want to mutate the shell's cwd,
             * but for this toy terminal we let it stick. */
        } else {
            Term_Append(st, L"  cd: failed");
        }
    } else if (wcscmp(cmd, L"clear") == 0 || wcscmp(cmd, L"cls") == 0) {
        SetWindowTextW(st->out, L"");
    } else if (wcscmp(cmd, L"help") == 0) {
        Term_Append(st, L"  commands: echo <txt>, time, pwd, ls, cd <dir>, clear, help");
    } else if (cmd[0] == 0) {
        /* empty line */
    } else {
        Term_Append(st, L"  unknown command. try: help");
    }
}

/* Subclass the input EDIT to catch Enter */
static LRESULT CALLBACK Term_InputProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    TermState *st = (TermState *)GetPropW(GetParent(hwnd), TERM_PROP_STATE);

    if (msg == WM_KEYDOWN && wp == VK_RETURN && st) {
        wchar_t line[512];
        GetWindowTextW(hwnd, line, 512);
        SetWindowTextW(hwnd, L"");
        Term_RunCommand(st, line);
        return 0;
    }
    if (msg == WM_CHAR && wp == VK_RETURN) {
        return 0; /* swallow the beep */
    }
    return CallWindowProcW(st ? st->origInputProc : DefWindowProcW,
                           hwnd, msg, wp, lp);
}

static LRESULT CALLBACK Term_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_SIZE) {
        TermState *st = (TermState *)GetPropW(hwnd, TERM_PROP_STATE);
        if (st) {
            int w = LOWORD(lp), h = HIWORD(lp);
            int inH = 24;
            MoveWindow(st->out, 4, 32, w - 8, h - 40 - inH, TRUE);
            MoveWindow(st->in,  4, h - 6 - inH, w - 8, inH, TRUE);
        }
    }
    if (msg == WM_DESTROY) {
        TermState *st = (TermState *)GetPropW(hwnd, TERM_PROP_STATE);
        if (st) free(st);
        RemovePropW(hwnd, TERM_PROP_STATE);
    }
    return CallWindowProcW(g_origTermFrame, hwnd, msg, wp, lp);
}

static HWND Terminal_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    TermState *st;
    HFONT mono;

    (void)self;
    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Terminal",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (TermState *)calloc(1, sizeof(TermState));
    if (!st) { DestroyWindow(frame); return NULL; }
    GetCurrentDirectoryW(MAX_PATH, st->cwd);

    st->out = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"MiniShell terminal — type 'help'\r\n",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        4, 32, w - 8, h - 64, frame, (HMENU)(LONG_PTR)TERM_ID_OUT, hInstance, NULL);

    st->in = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        4, h - 30, w - 8, 24, frame, (HMENU)(LONG_PTR)TERM_ID_IN, hInstance, NULL);

    mono = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->out, WM_SETFONT, (WPARAM)mono, TRUE);
    SendMessageW(st->in,  WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, TERM_PROP_STATE, (HANDLE)st);
    st->origInputProc = (WNDPROC)GetWindowLongPtrW(st->in, GWLP_WNDPROC);
    SetWindowLongPtrW(st->in, GWLP_WNDPROC, (LONG_PTR)Term_InputProc);

    if (!g_origTermFrame)
        g_origTermFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Term_FrameProc);

    SetFocus(st->in);
    return frame;
}

MsApp g_AppTerminal = {
    L"Terminal",
    Terminal_Create,
    560, 360
};
