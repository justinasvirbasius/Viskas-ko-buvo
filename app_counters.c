/*
 * app_counters.c — PDH performance counter polling
 *
 * Demonstrates Performance Data Helper (PDH), the Windows API for reading
 * the same counter data exposed by Performance Monitor:
 *   - PdhOpenQueryW: create a query handle
 *   - PdhAddCounterW: add the formatted "\Processor(_Total)\% Processor Time"
 *     and a few memory/disk counters
 *   - PdhCollectQueryData on a timer
 *   - PdhGetFormattedCounterValue (PDH_FMT_DOUBLE)
 *
 * The CPU counter, like in PerfMon, needs two samples before it returns a
 * meaningful value — the app simply shows whatever PDH gives back each tick.
 */

#include "shell.h"
#include <pdh.h>
#include <pdhmsg.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "pdh.lib")

#define PC_PROP    L"MS_PC_STATE"
#define ID_PC_OUT  32001
#define PC_TIMER   1

typedef struct {
    HQUERY  query;
    HCOUNTER cpu;
    HCOUNTER pagesPerSec;
    HCOUNTER ctxSwitches;
    HCOUNTER diskTime;
    HWND     output;
} PcState;

static WNDPROC g_origPcFrame = NULL;

static void Pc_Tick(PcState *st)
{
    PDH_FMT_COUNTERVALUE v;
    wchar_t buf[600];
    double cpu = 0, paging = 0, csw = 0, disk = 0;

    if (!st->query) return;
    PdhCollectQueryData(st->query);

    if (st->cpu &&
        PdhGetFormattedCounterValue(st->cpu, PDH_FMT_DOUBLE, NULL, &v) == ERROR_SUCCESS)
        cpu = v.doubleValue;
    if (st->pagesPerSec &&
        PdhGetFormattedCounterValue(st->pagesPerSec, PDH_FMT_DOUBLE, NULL, &v) == ERROR_SUCCESS)
        paging = v.doubleValue;
    if (st->ctxSwitches &&
        PdhGetFormattedCounterValue(st->ctxSwitches, PDH_FMT_DOUBLE, NULL, &v) == ERROR_SUCCESS)
        csw = v.doubleValue;
    if (st->diskTime &&
        PdhGetFormattedCounterValue(st->diskTime, PDH_FMT_DOUBLE, NULL, &v) == ERROR_SUCCESS)
        disk = v.doubleValue;

    swprintf_s(buf, 600,
        L"\\Processor(_Total)\\%% Processor Time     %8.2f %%\r\n"
        L"\\Memory\\Pages/sec                      %8.2f\r\n"
        L"\\System\\Context Switches/sec           %8.2f\r\n"
        L"\\PhysicalDisk(_Total)\\%% Disk Time      %8.2f %%\r\n",
        cpu, paging, csw, disk);
    SetWindowTextW(st->output, buf);
}

static LRESULT CALLBACK Pc_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    PcState *st = (PcState *)GetPropW(hwnd, PC_PROP);

    if (msg == WM_TIMER && st) { Pc_Tick(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 36, w - 16, h - 44, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        KillTimer(hwnd, PC_TIMER);
        if (st->query) PdhCloseQuery(st->query);
        free(st);
        RemovePropW(hwnd, PC_PROP);
    }
    return CallWindowProcW(g_origPcFrame, hwnd, msg, wp, lp);
}

static HWND Counters_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    PcState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Counters",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (PcState *)calloc(1, sizeof(PcState));
    if (!st) { DestroyWindow(frame); return NULL; }

    if (PdhOpenQueryW(NULL, 0, &st->query) == ERROR_SUCCESS) {
        PdhAddCounterW(st->query,
            L"\\Processor(_Total)\\% Processor Time", 0, &st->cpu);
        PdhAddCounterW(st->query,
            L"\\Memory\\Pages/sec", 0, &st->pagesPerSec);
        PdhAddCounterW(st->query,
            L"\\System\\Context Switches/sec", 0, &st->ctxSwitches);
        PdhAddCounterW(st->query,
            L"\\PhysicalDisk(_Total)\\% Disk Time", 0, &st->diskTime);
        /* Prime the rate counters */
        PdhCollectQueryData(st->query);
    }

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"Collecting...",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY,
        8, 36, w - 16, h - 44, frame, (HMENU)(LONG_PTR)ID_PC_OUT, hInstance, NULL);

    mono = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, PC_PROP, (HANDLE)st);
    if (!g_origPcFrame)
        g_origPcFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Pc_FrameProc);

    SetTimer(frame, PC_TIMER, 1000, NULL);
    return frame;
}

MsApp g_AppCounters = {
    L"Counters",
    Counters_Create,
    540, 220
};
