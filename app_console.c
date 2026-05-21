/*
 * app_console.c — Attach a console to the GUI app at runtime
 *
 * Demonstrates:
 *   - AllocConsole: spawn a new console window owned by this process
 *   - SetConsoleTitleW
 *   - GetStdHandle(STD_OUTPUT_HANDLE) and WriteConsoleW
 *   - SetConsoleTextAttribute for colored output
 *   - FreeConsole to detach (and the console window goes away)
 *
 * Useful as a debug-output trick during development: a GUI EXE can pop a
 * console any time it wants. Click "Allocate" to spawn the console, "Print"
 * to write to it, and "Free" to close it.
 */

#include "shell.h"
#include <stdlib.h>
#include <stdio.h>

#define CN_PROP    L"MS_CN_STATE"
#define ID_CN_ALLOC 40001
#define ID_CN_PRINT 40002
#define ID_CN_FREE  40003
#define ID_CN_STAT  40004

typedef struct {
    BOOL allocated;
    HWND status;
} CnState;

static WNDPROC g_origCnFrame = NULL;

static void Cn_WriteLine(const wchar_t *text, WORD attr)
{
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written;
    if (out == INVALID_HANDLE_VALUE || out == NULL) return;
    SetConsoleTextAttribute(out, attr);
    WriteConsoleW(out, text, (DWORD)wcslen(text), &written, NULL);
}

static void Cn_DemoPrint(void)
{
    Cn_WriteLine(L"[MiniShell debug console]\r\n",
                 FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    Cn_WriteLine(L"This is regular output.\r\n",
                 FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    Cn_WriteLine(L"This is a warning.\r\n",
                 FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    Cn_WriteLine(L"This is success.\r\n",
                 FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    Cn_WriteLine(L"This is an error.\r\n",
                 FOREGROUND_RED | FOREGROUND_INTENSITY);
    Cn_WriteLine(L"-----\r\n",
                 FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
}

static LRESULT CALLBACK Cn_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    CnState *st = (CnState *)GetPropW(hwnd, CN_PROP);

    if (msg == WM_COMMAND && st) {
        switch (LOWORD(wp)) {
        case ID_CN_ALLOC:
            if (!st->allocated) {
                if (AllocConsole()) {
                    SetConsoleTitleW(L"MiniShell Debug Console");
                    st->allocated = TRUE;
                    SetWindowTextW(st->status,
                        L"Status: console allocated.");
                    Cn_DemoPrint();
                } else {
                    SetWindowTextW(st->status,
                        L"Status: AllocConsole failed (already attached?).");
                }
            } else {
                SetWindowTextW(st->status, L"Status: already allocated.");
            }
            return 0;
        case ID_CN_PRINT:
            if (st->allocated) {
                Cn_DemoPrint();
            } else {
                SetWindowTextW(st->status, L"Status: allocate first.");
            }
            return 0;
        case ID_CN_FREE:
            if (st->allocated) {
                FreeConsole();
                st->allocated = FALSE;
                SetWindowTextW(st->status, L"Status: console detached.");
            } else {
                SetWindowTextW(st->status, L"Status: nothing to free.");
            }
            return 0;
        }
    }
    if (msg == WM_DESTROY && st) {
        if (st->allocated) FreeConsole();
        free(st);
        RemovePropW(hwnd, CN_PROP);
    }
    return CallWindowProcW(g_origCnFrame, hwnd, msg, wp, lp);
}

static HWND Console_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    CnState *st;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Console",
        WS_POPUP | WS_BORDER,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (CnState *)calloc(1, sizeof(CnState));
    if (!st) { DestroyWindow(frame); return NULL; }

    CreateWindowExW(0, L"STATIC",
        L"Attach a fresh console to this GUI process.\n"
        L"It pops as a separate window and accepts colored writes.",
        WS_CHILD | WS_VISIBLE,
        12, 40, w - 24, 36, frame, NULL, hInstance, NULL);

    CreateWindowExW(0, L"BUTTON", L"Allocate",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 84, 90, 28, frame, (HMENU)(LONG_PTR)ID_CN_ALLOC, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Print",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        112, 84, 90, 28, frame, (HMENU)(LONG_PTR)ID_CN_PRINT, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Free",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        212, 84, 90, 28, frame, (HMENU)(LONG_PTR)ID_CN_FREE, hInstance, NULL);

    st->status = CreateWindowExW(0, L"STATIC", L"Status: not allocated.",
        WS_CHILD | WS_VISIBLE,
        12, 124, w - 24, 22, frame, (HMENU)(LONG_PTR)ID_CN_STAT, hInstance, NULL);

    SetPropW(frame, CN_PROP, (HANDLE)st);
    if (!g_origCnFrame) g_origCnFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Cn_FrameProc);
    return frame;
}

MsApp g_AppConsole = {
    L"Console",
    Console_Create,
    340, 200
};
