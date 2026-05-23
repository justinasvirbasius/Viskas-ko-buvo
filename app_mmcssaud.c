/*
 * app_mmcssaud.c — MMCSS (Multimedia Class Scheduler Service) thread boost
 *
 * Demonstrates AvSetMmThreadCharacteristicsW, the API audio/video apps use
 * to ask the OS scheduler to give a thread realtime-grade priority:
 *   - AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex)
 *     returns a HANDLE; the thread is now in the MMCSS "Pro Audio" class
 *   - AvSetMmThreadPriority(handle, AVRT_PRIORITY_HIGH) — finer control
 *   - AvRevertMmThreadCharacteristics(handle) on shutdown
 *   - Standard task profiles: Audio, Capture, DisplayPostProcessing,
 *     Distribution, Games, Playback, Pro Audio, Window Manager
 *
 * The DLL is avrt.dll; load functions dynamically because the import-lib
 * isn't on every SDK.
 *
 * We spin a worker thread that calls QueryPerformanceCounter in a tight
 * loop, measuring scheduling jitter, then compares before/after MMCSS.
 */

#include "shell.h"
#include <stdlib.h>
#include <stdio.h>

#define MM_PROP    L"MS_MM_STATE"
#define ID_MM_GO   88001
#define ID_MM_STOP 88002
#define ID_MM_OUT  88003

#define WM_MM_LINE (WM_USER + 190)
#define WM_MM_DONE (WM_USER + 191)

typedef HANDLE (WINAPI *PFN_AvSetMmThreadCharacteristicsW)(LPCWSTR, LPDWORD);
typedef BOOL   (WINAPI *PFN_AvRevertMmThreadCharacteristics)(HANDLE);
typedef BOOL   (WINAPI *PFN_AvSetMmThreadPriority)(HANDLE, int);

typedef struct {
    HWND     frame, output;
    HANDLE   thread, stopEvent;
    HMODULE  avrt;
    PFN_AvSetMmThreadCharacteristicsW   pSet;
    PFN_AvRevertMmThreadCharacteristics pRevert;
    PFN_AvSetMmThreadPriority           pSetPri;
} MmState;

static WNDPROC g_origMmFrame = NULL;

static void Mm_Post(HWND f, const wchar_t *t)
{
    wchar_t *p = _wcsdup(t);
    if (p) PostMessageW(f, WM_MM_LINE, 0, (LPARAM)p);
}

static double Mm_MeasureJitter(HANDLE stopEvent, int durationMs)
{
    LARGE_INTEGER freq, last, now;
    double sumMs = 0, sumSq = 0;
    int    samples = 0;
    DWORD  startTick;

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    startTick = GetTickCount();

    while (GetTickCount() - startTick < (DWORD)durationMs) {
        if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;
        /* Sleep(1) — measure how long it actually takes */
        Sleep(1);
        QueryPerformanceCounter(&now);
        {
            double ms = (now.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart;
            sumMs += ms;
            sumSq += ms * ms;
            ++samples;
        }
        last = now;
    }
    if (samples < 2) return 0;
    {
        double mean = sumMs / samples;
        double var  = (sumSq / samples) - mean * mean;
        if (var < 0) var = 0;
        return (mean - 1.0);  /* over-sleep amount on average */
    }
}

static DWORD WINAPI Mm_Worker(LPVOID arg)
{
    MmState *st = (MmState *)arg;
    HANDLE  mmHandle = NULL;
    DWORD   taskIndex = 0;
    double  jitterBefore, jitterAfter;
    wchar_t line[200];

    Mm_Post(st->frame, L"Measuring Sleep(1) over-sleep BEFORE MMCSS...\r\n");
    jitterBefore = Mm_MeasureJitter(st->stopEvent, 2000);
    swprintf_s(line, 200, L"  baseline average over-sleep: %.3f ms\r\n", jitterBefore);
    Mm_Post(st->frame, line);

    if (!st->pSet) {
        Mm_Post(st->frame, L"avrt.dll not available.\r\n");
        PostMessageW(st->frame, WM_MM_DONE, 0, 0);
        return 0;
    }

    Mm_Post(st->frame, L"Joining MMCSS \"Pro Audio\" task class...\r\n");
    mmHandle = st->pSet(L"Pro Audio", &taskIndex);
    if (!mmHandle) {
        Mm_Post(st->frame, L"AvSetMmThreadCharacteristics failed.\r\n");
        PostMessageW(st->frame, WM_MM_DONE, 0, 0);
        return 0;
    }
    if (st->pSetPri) st->pSetPri(mmHandle, 2 /* AVRT_PRIORITY_HIGH */);

    swprintf_s(line, 200, L"  joined (taskIndex=%lu)\r\n", taskIndex);
    Mm_Post(st->frame, line);

    Mm_Post(st->frame, L"Measuring Sleep(1) over-sleep WITH MMCSS...\r\n");
    jitterAfter = Mm_MeasureJitter(st->stopEvent, 2000);
    swprintf_s(line, 200, L"  MMCSS  average over-sleep: %.3f ms\r\n", jitterAfter);
    Mm_Post(st->frame, line);

    if (st->pRevert) st->pRevert(mmHandle);

    swprintf_s(line, 200,
        L"\r\nDelta: %.3f ms %s (lower is better).\r\n",
        jitterBefore - jitterAfter,
        (jitterAfter < jitterBefore) ? L"improvement" : L"regression");
    Mm_Post(st->frame, line);

    PostMessageW(st->frame, WM_MM_DONE, 0, 0);
    return 0;
}

static void Mm_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
    SendMessageW(e, EM_SCROLLCARET, 0, 0);
}

static void Mm_Start(MmState *st)
{
    DWORD tid;
    if (st->thread) return;
    SetWindowTextW(st->output, L"");
    ResetEvent(st->stopEvent);
    st->thread = CreateThread(NULL, 0, Mm_Worker, st, 0, &tid);
}

static void Mm_Stop(MmState *st)
{
    if (!st->thread) return;
    SetEvent(st->stopEvent);
    WaitForSingleObject(st->thread, 4000);
    CloseHandle(st->thread);
    st->thread = NULL;
}

static LRESULT CALLBACK Mm_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    MmState *st = (MmState *)GetPropW(hwnd, MM_PROP);
    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_MM_GO)   { Mm_Start(st); return 0; }
        if (LOWORD(wp) == ID_MM_STOP) { Mm_Stop(st);  return 0; }
    }
    if (msg == WM_MM_LINE && st) {
        wchar_t *p = (wchar_t *)lp;
        if (p) { Mm_Append(st->output, p); free(p); }
        return 0;
    }
    if (msg == WM_MM_DONE && st) {
        if (st->thread) { CloseHandle(st->thread); st->thread = NULL; }
        Mm_Append(st->output, L"[worker exited]\r\n");
        return 0;
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        Mm_Stop(st);
        if (st->stopEvent) CloseHandle(st->stopEvent);
        if (st->avrt) FreeLibrary(st->avrt);
        free(st); RemovePropW(hwnd, MM_PROP);
    }
    return CallWindowProcW(g_origMmFrame, hwnd, msg, wp, lp);
}

static HWND MmcssAud_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    MmState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"MmcssAud",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (MmState *)calloc(1, sizeof(MmState));
    if (!st) { DestroyWindow(frame); return NULL; }
    st->frame = frame;
    st->stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    st->avrt = LoadLibraryW(L"avrt.dll");
    if (st->avrt) {
        st->pSet    = (PFN_AvSetMmThreadCharacteristicsW)
                       GetProcAddress(st->avrt, "AvSetMmThreadCharacteristicsW");
        st->pRevert = (PFN_AvRevertMmThreadCharacteristics)
                       GetProcAddress(st->avrt, "AvRevertMmThreadCharacteristics");
        st->pSetPri = (PFN_AvSetMmThreadPriority)
                       GetProcAddress(st->avrt, "AvSetMmThreadPriority");
    }

    CreateWindowExW(0, L"STATIC",
        L"Compares Sleep(1) jitter before and after joining MMCSS \"Pro Audio\".",
        WS_CHILD | WS_VISIBLE,
        12, 30, w - 24, 22, frame, NULL, hInstance, NULL);

    CreateWindowExW(0, L"BUTTON", L"Measure",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 56, 110, 26, frame, (HMENU)(LONG_PTR)ID_MM_GO, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Stop",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        130, 56, 90, 26, frame, (HMENU)(LONG_PTR)ID_MM_STOP, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 92, w - 16, h - 100, frame, (HMENU)(LONG_PTR)ID_MM_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, MM_PROP, (HANDLE)st);
    if (!g_origMmFrame) g_origMmFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Mm_FrameProc);
    return frame;
}

MsApp g_AppMmcssAud = { L"MmcssAud", MmcssAud_Create, 640, 400 };
