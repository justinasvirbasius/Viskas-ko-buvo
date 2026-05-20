/*
 * app_progress.c — Long-running task with progress bar
 *
 * Demonstrates:
 *   - PROGRESS_CLASS (PBM_SETRANGE, PBM_SETPOS, PBM_SETMARQUEE)
 *   - A worker thread that simulates a long-running job
 *   - Progress reporting from worker to UI via PostMessage
 *   - Cancellation via an event handle, checked cooperatively in the worker
 *
 * The simulated work is "compute first N primes via a sieve" with artificial
 * sleeps so the bar moves visibly.
 */

#include "shell.h"
#include <commctrl.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "comctl32.lib")

#define PG_PROP    L"MS_PG_STATE"
#define ID_PG_BAR  18001
#define ID_PG_GO   18002
#define ID_PG_CXL  18003
#define ID_PG_STAT 18004
#define ID_PG_MARQ 18005

#define WM_PG_PROGRESS (WM_USER + 80)   /* wparam = 0..100 */
#define WM_PG_DONE     (WM_USER + 81)
#define WM_PG_FOUND    (WM_USER + 82)   /* wparam = prime count, lparam = highest */

typedef struct {
    HWND bar;
    HWND status;
    HWND goBtn, cancelBtn, marqueeBtn;
    HANDLE thread;
    HANDLE cancelEvent;
    BOOL   marqueeOn;
} PgState;

typedef struct {
    HWND   target;
    HANDLE cancel;
} PgTask;

static WNDPROC g_origPgFrame = NULL;

static DWORD WINAPI Pg_Worker(LPVOID arg)
{
    PgTask *task = (PgTask *)arg;
    const int N = 200000;
    char *sieve;
    int i, j, primes = 0, highest = 0;
    int reportEvery = N / 100;
    int lastReport = 0;

    sieve = (char *)calloc(N, 1);
    if (!sieve) {
        PostMessageW(task->target, WM_PG_DONE, 0, 0);
        free(task);
        return 0;
    }

    for (i = 2; i < N; ++i) {
        /* Cooperative cancellation */
        if (WaitForSingleObject(task->cancel, 0) == WAIT_OBJECT_0) break;

        if (!sieve[i]) {
            ++primes;
            highest = i;
            for (j = i * 2; j < N; j += i) sieve[j] = 1;
        }
        if (i - lastReport >= reportEvery) {
            lastReport = i;
            PostMessageW(task->target, WM_PG_PROGRESS,
                         (WPARAM)((i * 100) / N), 0);
            /* small sleep so the bar is actually visible */
            if (WaitForSingleObject(task->cancel, 8) == WAIT_OBJECT_0) break;
        }
    }
    PostMessageW(task->target, WM_PG_FOUND, primes, highest);
    PostMessageW(task->target, WM_PG_DONE, 0, 0);
    free(sieve);
    free(task);
    return 0;
}

static void Pg_Start(HWND frame, PgState *st)
{
    PgTask *task;
    DWORD tid;
    if (st->thread) return;

    /* Switch off marquee for determinate progress */
    if (st->marqueeOn) {
        SendMessageW(st->bar, PBM_SETMARQUEE, FALSE, 0);
        SetWindowLongPtrW(st->bar, GWL_STYLE,
            GetWindowLongPtrW(st->bar, GWL_STYLE) & ~PBS_MARQUEE);
        st->marqueeOn = FALSE;
        SetWindowTextW(st->marqueeBtn, L"Marquee");
    }

    SendMessageW(st->bar, PBM_SETPOS, 0, 0);
    SetWindowTextW(st->status, L"Working...");
    EnableWindow(st->goBtn, FALSE);
    EnableWindow(st->cancelBtn, TRUE);

    ResetEvent(st->cancelEvent);
    task = (PgTask *)calloc(1, sizeof(PgTask));
    if (!task) return;
    task->target = frame;
    task->cancel = st->cancelEvent;
    st->thread = CreateThread(NULL, 0, Pg_Worker, task, 0, &tid);
    if (!st->thread) {
        free(task);
        EnableWindow(st->goBtn, TRUE);
        EnableWindow(st->cancelBtn, FALSE);
        SetWindowTextW(st->status, L"Failed to start worker");
    }
}

static void Pg_Cancel(PgState *st)
{
    if (st->thread) SetEvent(st->cancelEvent);
}

static void Pg_ToggleMarquee(PgState *st)
{
    if (st->thread) return;   /* not while a real job is running */

    if (st->marqueeOn) {
        SendMessageW(st->bar, PBM_SETMARQUEE, FALSE, 0);
        SetWindowLongPtrW(st->bar, GWL_STYLE,
            GetWindowLongPtrW(st->bar, GWL_STYLE) & ~PBS_MARQUEE);
        st->marqueeOn = FALSE;
        SetWindowTextW(st->marqueeBtn, L"Marquee");
        SendMessageW(st->bar, PBM_SETPOS, 0, 0);
    } else {
        SetWindowLongPtrW(st->bar, GWL_STYLE,
            GetWindowLongPtrW(st->bar, GWL_STYLE) | PBS_MARQUEE);
        SendMessageW(st->bar, PBM_SETMARQUEE, TRUE, 30);
        st->marqueeOn = TRUE;
        SetWindowTextW(st->marqueeBtn, L"Stop marquee");
    }
}

static LRESULT CALLBACK Pg_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    PgState *st = (PgState *)GetPropW(hwnd, PG_PROP);

    if (msg == WM_COMMAND && st) {
        switch (LOWORD(wp)) {
        case ID_PG_GO:   Pg_Start(hwnd, st);  return 0;
        case ID_PG_CXL:  Pg_Cancel(st);       return 0;
        case ID_PG_MARQ: Pg_ToggleMarquee(st); return 0;
        }
    }
    if (msg == WM_PG_PROGRESS && st) {
        SendMessageW(st->bar, PBM_SETPOS, wp, 0);
        return 0;
    }
    if (msg == WM_PG_FOUND && st) {
        wchar_t buf[120];
        swprintf_s(buf, 120,
                   L"Found %d primes, highest = %d", (int)wp, (int)lp);
        SetWindowTextW(st->status, buf);
        return 0;
    }
    if (msg == WM_PG_DONE && st) {
        if (st->thread) {
            WaitForSingleObject(st->thread, 500);
            CloseHandle(st->thread);
            st->thread = NULL;
        }
        EnableWindow(st->goBtn, TRUE);
        EnableWindow(st->cancelBtn, FALSE);
        SendMessageW(st->bar, PBM_SETPOS, 100, 0);
        return 0;
    }
    if (msg == WM_DESTROY && st) {
        if (st->thread) {
            SetEvent(st->cancelEvent);
            WaitForSingleObject(st->thread, 2000);
            CloseHandle(st->thread);
        }
        if (st->cancelEvent) CloseHandle(st->cancelEvent);
        free(st);
        RemovePropW(hwnd, PG_PROP);
    }
    return CallWindowProcW(g_origPgFrame, hwnd, msg, wp, lp);
}

static HWND Progress_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    PgState *st;
    INITCOMMONCONTROLSEX icc;
    (void)self;

    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Progress",
        WS_POPUP | WS_BORDER,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (PgState *)calloc(1, sizeof(PgState));
    if (!st) { DestroyWindow(frame); return NULL; }
    st->cancelEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    st->bar = CreateWindowExW(0, PROGRESS_CLASSW, NULL,
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        16, 44, w - 32, 22, frame, (HMENU)(LONG_PTR)ID_PG_BAR, hInstance, NULL);
    SendMessageW(st->bar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

    st->status = CreateWindowExW(0, L"STATIC", L"Ready",
        WS_CHILD | WS_VISIBLE,
        16, 74, w - 32, 20, frame, (HMENU)(LONG_PTR)ID_PG_STAT, hInstance, NULL);

    st->goBtn = CreateWindowExW(0, L"BUTTON", L"Start",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        16, 104, 90, 28, frame, (HMENU)(LONG_PTR)ID_PG_GO, hInstance, NULL);
    st->cancelBtn = CreateWindowExW(0, L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
        116, 104, 90, 28, frame, (HMENU)(LONG_PTR)ID_PG_CXL, hInstance, NULL);
    st->marqueeBtn = CreateWindowExW(0, L"BUTTON", L"Marquee",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        216, 104, 110, 28, frame, (HMENU)(LONG_PTR)ID_PG_MARQ, hInstance, NULL);

    SetPropW(frame, PG_PROP, (HANDLE)st);
    if (!g_origPgFrame)
        g_origPgFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Pg_FrameProc);
    return frame;
}

MsApp g_AppProgress = {
    L"Progress",
    Progress_Create,
    360, 180
};
