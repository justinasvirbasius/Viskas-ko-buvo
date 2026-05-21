/*
 * app_threadpool.c — Win32 thread pool (CreateThreadpoolWork)
 *
 * Demonstrates the modern Win32 thread pool API (Vista+), which is what you
 * should use instead of spawning your own threads for short tasks:
 *   - CreateThreadpoolWork registers a work callback
 *   - SubmitThreadpoolWork queues an instance to run
 *   - WaitForThreadpoolWorkCallbacks waits for all instances of a work item
 *   - The OS schedules them across a pool of threads sized to your CPU
 *
 * The app submits N "compute" jobs (each computes a sum of sines to use a
 * little CPU) and updates the UI as they finish. The pool size is shown in
 * the status — you'll see jobs run in parallel.
 */

#include "shell.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>

#define TP_PROP    L"MS_TP_STATE"
#define ID_TP_GO   29001
#define ID_TP_OUT  29002
#define ID_TP_NUM  29003
#define ID_TP_LBL  29004

#define WM_TP_DONE_ONE (WM_USER + 120)   /* wparam = job idx, lparam = ms elapsed */
#define WM_TP_ALL_DONE (WM_USER + 121)

typedef struct {
    HWND output, goBtn, numEdit, statusLbl;
    PTP_WORK work;
    LONG     remaining;
} TpState;

typedef struct {
    HWND target;
    int  jobIdx;
} TpJob;

typedef struct {
    TpState *st;
    TpJob   *jobs;
    int      count;
} TpBatch;

static WNDPROC g_origTpFrame = NULL;
static TpBatch g_batch;

static void Tp_Append(HWND output, const wchar_t *t)
{
    int len = GetWindowTextLengthW(output);
    SendMessageW(output, EM_SETSEL, len, len);
    SendMessageW(output, EM_REPLACESEL, FALSE, (LPARAM)t);
    SendMessageW(output, EM_SCROLLCARET, 0, 0);
}

static VOID CALLBACK Tp_Worker(PTP_CALLBACK_INSTANCE inst, PVOID ctx, PTP_WORK work)
{
    LARGE_INTEGER t0, t1, freq;
    double sum = 0.0;
    int i;
    (void)inst; (void)work;

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    /* Burn some CPU so we can see parallelism */
    for (i = 0; i < 4000000; ++i) sum += sin((double)i * 0.0001);
    /* Use sum so the compiler can't optimize it away */
    if (sum > 1e300) ((TpJob *)ctx)->jobIdx = -1;

    QueryPerformanceCounter(&t1);
    {
        TpJob *job = (TpJob *)ctx;
        LONG ms = (LONG)((t1.QuadPart - t0.QuadPart) * 1000 / freq.QuadPart);
        PostMessageW(job->target, WM_TP_DONE_ONE, (WPARAM)job->jobIdx, (LPARAM)ms);
    }
}

static void Tp_Start(HWND frame, TpState *st)
{
    wchar_t numStr[16];
    int n, i;
    wchar_t line[80];

    if (st->work) return;
    GetWindowTextW(st->numEdit, numStr, 16);
    n = _wtoi(numStr);
    if (n <= 0 || n > 64) n = 8;

    /* Clean up any prior batch */
    if (g_batch.jobs) { free(g_batch.jobs); g_batch.jobs = NULL; }

    g_batch.st = st;
    g_batch.count = n;
    g_batch.jobs = (TpJob *)calloc(n, sizeof(TpJob));
    if (!g_batch.jobs) return;

    SetWindowTextW(st->output, L"");
    swprintf_s(line, 80, L"Submitting %d jobs to the thread pool...\r\n", n);
    Tp_Append(st->output, line);

    InterlockedExchange(&st->remaining, n);
    EnableWindow(st->goBtn, FALSE);

    /* One PTP_WORK can be submitted multiple times; each submission becomes
     * a separately-scheduled instance with its own callback parameter. */
    st->work = CreateThreadpoolWork(Tp_Worker, NULL /* set per-submit */, NULL);
    if (!st->work) {
        Tp_Append(st->output, L"CreateThreadpoolWork failed\r\n");
        EnableWindow(st->goBtn, TRUE);
        return;
    }

    /* The threadpool callback gets the Work object's context, not per-submit
     * context. We work around this by using one PTP_WORK per job. */
    {
        free(g_batch.jobs);
        g_batch.jobs = (TpJob *)calloc(n, sizeof(TpJob));
        CloseThreadpoolWork(st->work);
        st->work = NULL;
    }
    for (i = 0; i < n; ++i) {
        g_batch.jobs[i].target = frame;
        g_batch.jobs[i].jobIdx = i;
        {
            PTP_WORK w = CreateThreadpoolWork(Tp_Worker, &g_batch.jobs[i], NULL);
            if (w) {
                SubmitThreadpoolWork(w);
                /* We pass ownership; close after submission — the system
                 * keeps the work alive until callbacks finish. */
                CloseThreadpoolWork(w);
            }
        }
    }
}

static LRESULT CALLBACK Tp_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    TpState *st = (TpState *)GetPropW(hwnd, TP_PROP);

    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_TP_GO) { Tp_Start(hwnd, st); return 0; }
    if (msg == WM_TP_DONE_ONE && st) {
        wchar_t line[80];
        LONG remaining;
        swprintf_s(line, 80, L"job %d finished in %ld ms\r\n", (int)wp, (long)lp);
        Tp_Append(st->output, line);
        remaining = InterlockedDecrement(&st->remaining);
        if (remaining == 0) PostMessageW(hwnd, WM_TP_ALL_DONE, 0, 0);
        return 0;
    }
    if (msg == WM_TP_ALL_DONE && st) {
        Tp_Append(st->output, L"\r\nAll jobs done.\r\n");
        EnableWindow(st->goBtn, TRUE);
        return 0;
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->statusLbl, 8, 36, w - 16, 18, TRUE);
        MoveWindow(st->numEdit,   8, 58, 60, 24, TRUE);
        MoveWindow(st->goBtn,     74, 58, 100, 24, TRUE);
        MoveWindow(st->output,    8, 90, w - 16, h - 98, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (g_batch.jobs) { free(g_batch.jobs); g_batch.jobs = NULL; }
        free(st);
        RemovePropW(hwnd, TP_PROP);
    }
    return CallWindowProcW(g_origTpFrame, hwnd, msg, wp, lp);
}

static HWND ThreadPool_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    TpState *st;
    SYSTEM_INFO si;
    wchar_t status[80];
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"ThreadPool",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (TpState *)calloc(1, sizeof(TpState));
    if (!st) { DestroyWindow(frame); return NULL; }

    GetSystemInfo(&si);
    swprintf_s(status, 80,
        L"System has %lu logical processors. Default pool sizes accordingly.",
        si.dwNumberOfProcessors);
    st->statusLbl = CreateWindowExW(0, L"STATIC", status,
        WS_CHILD | WS_VISIBLE,
        8, 36, w - 16, 18, frame, (HMENU)(LONG_PTR)ID_TP_LBL, hInstance, NULL);

    st->numEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"16",
        WS_CHILD | WS_VISIBLE | ES_NUMBER,
        8, 58, 60, 24, frame, (HMENU)(LONG_PTR)ID_TP_NUM, hInstance, NULL);
    st->goBtn = CreateWindowExW(0, L"BUTTON", L"Submit jobs",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        74, 58, 100, 24, frame, (HMENU)(LONG_PTR)ID_TP_GO, hInstance, NULL);
    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        8, 90, w - 16, h - 98, frame, (HMENU)(LONG_PTR)ID_TP_OUT, hInstance, NULL);

    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, TP_PROP, (HANDLE)st);
    if (!g_origTpFrame) g_origTpFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Tp_FrameProc);
    return frame;
}

MsApp g_AppThreadPool = {
    L"ThreadPool",
    ThreadPool_Create,
    520, 380
};
