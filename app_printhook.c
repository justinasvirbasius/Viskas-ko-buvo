/*
 * app_printhook.c — Print spooler change notifications
 *
 * Demonstrates the print-spooler event API (winspool.drv) — the same
 * mechanism Windows uses to update the print queue UI in real time:
 *   - OpenPrinterW(printerName, &handle, NULL)
 *   - FindFirstPrinterChangeNotification(handle, fdwFilter, fdwOptions, NULL)
 *     returns a waitable HANDLE that becomes signaled when any matching
 *     spooler event (job added, job written, printer attribute change)
 *     happens
 *   - FindNextPrinterChangeNotification(hChange, &cause, NULL, &info)
 *     returns the change cause and (if PRINTER_NOTIFY_INFO requested) an
 *     allocated structure describing exactly which fields changed
 *   - FreePrinterNotifyInfo on the returned info
 *   - FindClosePrinterChangeNotification on shutdown
 *
 * We watch the default printer for PRINTER_CHANGE_JOB | PRINTER_CHANGE_PRINTER.
 */

#include "shell.h"
#include <winspool.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "winspool.lib")

#define PH_PROP    L"MS_PH_STATE"
#define ID_PH_PR   105001
#define ID_PH_GO   105002
#define ID_PH_STOP 105003
#define ID_PH_OUT  105004

#define WM_PH_LINE (WM_USER + 210)
#define WM_PH_DONE (WM_USER + 211)

typedef struct {
    HWND   frame, printerEdit, goBtn, stopBtn, output;
    HANDLE thread, stopEvent;
} PhState;
static WNDPROC g_origPhFrame = NULL;

static void Ph_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
    SendMessageW(e, EM_SCROLLCARET, 0, 0);
}

static void Ph_Post(HWND f, const wchar_t *t)
{
    wchar_t *p = _wcsdup(t);
    if (p) PostMessageW(f, WM_PH_LINE, 0, (LPARAM)p);
}

static void Ph_DescribeCause(DWORD cause, wchar_t *out, int cch)
{
    out[0] = 0;
    if (cause & PRINTER_CHANGE_ADD_PRINTER)    wcscat_s(out, cch, L"ADD_PRINTER ");
    if (cause & PRINTER_CHANGE_SET_PRINTER)    wcscat_s(out, cch, L"SET_PRINTER ");
    if (cause & PRINTER_CHANGE_DELETE_PRINTER) wcscat_s(out, cch, L"DEL_PRINTER ");
    if (cause & PRINTER_CHANGE_ADD_JOB)        wcscat_s(out, cch, L"ADD_JOB ");
    if (cause & PRINTER_CHANGE_SET_JOB)        wcscat_s(out, cch, L"SET_JOB ");
    if (cause & PRINTER_CHANGE_DELETE_JOB)     wcscat_s(out, cch, L"DEL_JOB ");
    if (cause & PRINTER_CHANGE_WRITE_JOB)      wcscat_s(out, cch, L"WRITE_JOB ");
    if (cause & PRINTER_CHANGE_FAILED_CONNECTION_PRINTER) wcscat_s(out, cch, L"FAIL_CONN ");
    if (!out[0]) wcscpy_s(out, cch, L"(none)");
}

static DWORD WINAPI Ph_Worker(LPVOID arg)
{
    PhState *st = (PhState *)arg;
    wchar_t name[200];
    HANDLE  hPrinter = NULL;
    HANDLE  hChange = INVALID_HANDLE_VALUE;
    HANDLE  waits[2];
    DWORD   tick = 0;

    GetWindowTextW(st->printerEdit, name, 200);
    if (!name[0]) {
        DWORD len = 200;
        if (!GetDefaultPrinterW(name, &len) || !name[0]) {
            Ph_Post(st->frame, L"No printer name and no default printer.\r\n");
            PostMessageW(st->frame, WM_PH_DONE, 0, 0);
            return 0;
        }
    }

    if (!OpenPrinterW(name, &hPrinter, NULL)) {
        wchar_t buf[200];
        swprintf_s(buf, 200, L"OpenPrinter('%s') failed: %lu\r\n", name, GetLastError());
        Ph_Post(st->frame, buf);
        PostMessageW(st->frame, WM_PH_DONE, 0, 0);
        return 0;
    }

    hChange = FindFirstPrinterChangeNotification(hPrinter,
                PRINTER_CHANGE_JOB | PRINTER_CHANGE_PRINTER, 0, NULL);
    if (hChange == INVALID_HANDLE_VALUE) {
        Ph_Post(st->frame, L"FindFirstPrinterChangeNotification failed.\r\n");
        ClosePrinter(hPrinter);
        PostMessageW(st->frame, WM_PH_DONE, 0, 0);
        return 0;
    }

    {
        wchar_t line[200];
        swprintf_s(line, 200, L"Watching printer '%s'. Print or change a job.\r\n", name);
        Ph_Post(st->frame, line);
    }

    waits[0] = st->stopEvent;
    waits[1] = hChange;
    for (;;) {
        DWORD r = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (r == WAIT_OBJECT_0) break;
        if (r == WAIT_OBJECT_0 + 1) {
            DWORD cause = 0;
            PPRINTER_NOTIFY_INFO info = NULL;
            if (FindNextPrinterChangeNotification(hChange, &cause, NULL, NULL)) {
                wchar_t flags[200];
                wchar_t line[300];
                Ph_DescribeCause(cause, flags, 200);
                swprintf_s(line, 300, L"  event #%lu  cause=0x%08lx (%s)\r\n",
                           ++tick, cause, flags);
                Ph_Post(st->frame, line);
            } else {
                Ph_Post(st->frame, L"FindNextPrinterChangeNotification failed.\r\n");
                break;
            }
            (void)info;
        } else break;
    }

    FindClosePrinterChangeNotification(hChange);
    ClosePrinter(hPrinter);
    PostMessageW(st->frame, WM_PH_DONE, 0, 0);
    return 0;
}

static LRESULT CALLBACK Ph_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    PhState *st = (PhState *)GetPropW(hwnd, PH_PROP);
    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_PH_GO) {
            DWORD tid;
            if (st->thread) return 0;
            ResetEvent(st->stopEvent);
            st->thread = CreateThread(NULL, 0, Ph_Worker, st, 0, &tid);
            return 0;
        }
        if (LOWORD(wp) == ID_PH_STOP) {
            if (st->thread) {
                SetEvent(st->stopEvent);
                WaitForSingleObject(st->thread, 4000);
                CloseHandle(st->thread);
                st->thread = NULL;
            }
            return 0;
        }
    }
    if (msg == WM_PH_LINE && st) {
        wchar_t *p = (wchar_t *)lp;
        if (p) { Ph_Append(st->output, p); free(p); }
        return 0;
    }
    if (msg == WM_PH_DONE && st) {
        if (st->thread) { CloseHandle(st->thread); st->thread = NULL; }
        Ph_Append(st->output, L"[worker exited]\r\n");
        return 0;
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->printerEdit, 12, 38, w - 220, 24, TRUE);
        MoveWindow(st->goBtn,       w - 204, 38, 90, 24, TRUE);
        MoveWindow(st->stopBtn,     w - 108, 38, 90, 24, TRUE);
        MoveWindow(st->output,      8, 70, w - 16, h - 78, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->thread) {
            SetEvent(st->stopEvent);
            WaitForSingleObject(st->thread, 4000);
            CloseHandle(st->thread);
        }
        if (st->stopEvent) CloseHandle(st->stopEvent);
        free(st); RemovePropW(hwnd, PH_PROP);
    }
    return CallWindowProcW(g_origPhFrame, hwnd, msg, wp, lp);
}

static HWND PrintHook_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    PhState *st;
    HFONT mono;
    wchar_t def[200] = L"";
    DWORD len = 200;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"PrintHook",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (PhState *)calloc(1, sizeof(PhState));
    if (!st) { DestroyWindow(frame); return NULL; }
    st->frame = frame;
    st->stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    GetDefaultPrinterW(def, &len);

    st->printerEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", def,
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        12, 38, w - 220, 24, frame, (HMENU)(LONG_PTR)ID_PH_PR, hInstance, NULL);
    st->goBtn = CreateWindowExW(0, L"BUTTON", L"Watch",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        w - 204, 38, 90, 24, frame, (HMENU)(LONG_PTR)ID_PH_GO, hInstance, NULL);
    st->stopBtn = CreateWindowExW(0, L"BUTTON", L"Stop",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        w - 108, 38, 90, 24, frame, (HMENU)(LONG_PTR)ID_PH_STOP, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"Pick a printer name (default shown). Click Watch.\r\n"
        L"Send a print job to see events arrive.\r\n",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 70, w - 16, h - 78, frame, (HMENU)(LONG_PTR)ID_PH_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, PH_PROP, (HANDLE)st);
    if (!g_origPhFrame) g_origPhFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Ph_FrameProc);
    return frame;
}

MsApp g_AppPrintHook = { L"PrintHook", PrintHook_Create, 740, 420 };
