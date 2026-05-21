/*
 * app_async.c — Overlapped (asynchronous) I/O and I/O Completion Ports
 *
 * Demonstrates the two main async I/O patterns on Windows:
 *
 *   1. OVERLAPPED + manual-reset event:
 *      CreateFile with FILE_FLAG_OVERLAPPED, ReadFile returns
 *      ERROR_IO_PENDING immediately, the OS signals our event when done.
 *      A worker thread can then call GetOverlappedResult to harvest.
 *
 *   2. I/O Completion Port:
 *      CreateIoCompletionPort to associate the file handle with a port,
 *      queue overlapped reads, and have N worker threads dequeue
 *      completions via GetQueuedCompletionStatus.
 *
 * The app reads a file you select in chunks via both methods and shows the
 * timing. It also lets you tweak the chunk size to see the effect.
 */

#include "shell.h"
#include <commdlg.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "comdlg32.lib")

#define AS_PROP        L"MS_AS_STATE"
#define ID_AS_PICK     28001
#define ID_AS_OVL      28002
#define ID_AS_IOCP     28003
#define ID_AS_OUT      28004
#define ID_AS_PATH     28005

#define WM_AS_LINE     (WM_USER + 110)
#define WM_AS_DONE     (WM_USER + 111)

#define CHUNK_SIZE     (256 * 1024)
#define IOCP_WORKERS   2

typedef struct {
    HWND   pathLabel, output;
    HWND   pickBtn, ovlBtn, iocpBtn;
    HANDLE thread;
    wchar_t path[MAX_PATH];
} AsState;

typedef struct {
    HWND    target;
    wchar_t path[MAX_PATH];
    int     mode;            /* 0 = overlapped+event, 1 = IOCP */
} AsTask;

static WNDPROC g_origAsFrame = NULL;

static void AsPostLine(HWND target, const wchar_t *text)
{
    size_t len = wcslen(text);
    wchar_t *copy = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
    if (!copy) return;
    wcscpy_s(copy, len + 1, text);
    if (!PostMessageW(target, WM_AS_LINE, (WPARAM)copy, 0)) free(copy);
}

static void As_Append(HWND output, const wchar_t *t)
{
    int len = GetWindowTextLengthW(output);
    SendMessageW(output, EM_SETSEL, len, len);
    SendMessageW(output, EM_REPLACESEL, FALSE, (LPARAM)t);
    SendMessageW(output, EM_SCROLLCARET, 0, 0);
}

/* --- Mode 0: OVERLAPPED + event --- */
static void As_RunOverlapped(AsTask *task)
{
    HANDLE file, evt;
    BYTE *buf;
    OVERLAPPED ov;
    LARGE_INTEGER size, t0, t1, freq;
    DWORD bytesRead, totalRead = 0;
    wchar_t line[200];
    BOOL ok;

    file = CreateFileW(task->path, GENERIC_READ, FILE_SHARE_READ, NULL,
                       OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        AsPostLine(task->target, L"[overlapped: open failed]\r\n");
        return;
    }

    evt = CreateEventW(NULL, TRUE, FALSE, NULL);
    GetFileSizeEx(file, &size);
    buf = (BYTE *)VirtualAlloc(NULL, CHUNK_SIZE, MEM_COMMIT, PAGE_READWRITE);

    swprintf_s(line, 200, L"[overlapped] file size %lld bytes\r\n",
               (long long)size.QuadPart);
    AsPostLine(task->target, line);

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    while (totalRead < size.QuadPart) {
        ZeroMemory(&ov, sizeof(ov));
        ov.Offset     = (DWORD)(totalRead & 0xFFFFFFFFULL);
        ov.OffsetHigh = (DWORD)(totalRead >> 32);
        ov.hEvent     = evt;
        ResetEvent(evt);

        ok = ReadFile(file, buf, CHUNK_SIZE, NULL, &ov);
        if (!ok && GetLastError() != ERROR_IO_PENDING) break;

        /* Block on the event — this is the wait point */
        WaitForSingleObject(evt, INFINITE);
        if (!GetOverlappedResult(file, &ov, &bytesRead, FALSE)) break;
        if (bytesRead == 0) break;
        totalRead += bytesRead;
    }
    QueryPerformanceCounter(&t1);

    swprintf_s(line, 200, L"[overlapped] read %lu bytes in %.2f ms\r\n",
               totalRead,
               (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart);
    AsPostLine(task->target, line);

    VirtualFree(buf, 0, MEM_RELEASE);
    CloseHandle(evt);
    CloseHandle(file);
}

/* --- Mode 1: IOCP --- */
typedef struct {
    OVERLAPPED ov;
    BYTE      *buf;
} AsRequest;

typedef struct {
    HANDLE iocp;
    HWND   target;
    LONG  *outstanding;
    LONG  *totalBytes;
} AsWorkerArg;

static DWORD WINAPI As_IocpWorker(LPVOID arg)
{
    AsWorkerArg *wa = (AsWorkerArg *)arg;
    DWORD nBytes;
    ULONG_PTR key;
    LPOVERLAPPED ov;

    while (GetQueuedCompletionStatus(wa->iocp, &nBytes, &key, &ov, INFINITE)) {
        if (ov == NULL) break;   /* shutdown sentinel */
        InterlockedExchangeAdd(wa->totalBytes, (LONG)nBytes);
        InterlockedDecrement(wa->outstanding);
        {
            AsRequest *req = (AsRequest *)ov;
            VirtualFree(req->buf, 0, MEM_RELEASE);
            free(req);
        }
    }
    return 0;
}

static void As_RunIOCP(AsTask *task)
{
    HANDLE file, iocp;
    HANDLE workers[IOCP_WORKERS];
    AsWorkerArg wa;
    LONG outstanding = 0, totalBytes = 0;
    LARGE_INTEGER size, t0, t1, freq;
    DWORD tid, i;
    ULONGLONG offset = 0;
    wchar_t line[200];
    const int kInFlight = 8;

    file = CreateFileW(task->path, GENERIC_READ, FILE_SHARE_READ, NULL,
                       OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        AsPostLine(task->target, L"[iocp: open failed]\r\n");
        return;
    }
    GetFileSizeEx(file, &size);

    iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, IOCP_WORKERS);
    CreateIoCompletionPort(file, iocp, 1, 0);

    wa.iocp        = iocp;
    wa.target      = task->target;
    wa.outstanding = &outstanding;
    wa.totalBytes  = &totalBytes;
    for (i = 0; i < IOCP_WORKERS; ++i) {
        workers[i] = CreateThread(NULL, 0, As_IocpWorker, &wa, 0, &tid);
    }

    swprintf_s(line, 200, L"[iocp] %d worker threads, %d in flight, %d KiB chunks\r\n",
               IOCP_WORKERS, kInFlight, CHUNK_SIZE / 1024);
    AsPostLine(task->target, line);

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    /* Queue work in flight */
    while (offset < (ULONGLONG)size.QuadPart) {
        AsRequest *req;
        BOOL ok;

        /* Throttle the in-flight count */
        while (outstanding >= kInFlight) Sleep(0);

        req = (AsRequest *)calloc(1, sizeof(AsRequest));
        req->buf = (BYTE *)VirtualAlloc(NULL, CHUNK_SIZE, MEM_COMMIT, PAGE_READWRITE);
        req->ov.Offset     = (DWORD)(offset & 0xFFFFFFFFULL);
        req->ov.OffsetHigh = (DWORD)(offset >> 32);

        InterlockedIncrement(&outstanding);
        ok = ReadFile(file, req->buf, CHUNK_SIZE, NULL, &req->ov);
        if (!ok && GetLastError() != ERROR_IO_PENDING) {
            InterlockedDecrement(&outstanding);
            VirtualFree(req->buf, 0, MEM_RELEASE);
            free(req);
            break;
        }
        offset += CHUNK_SIZE;
    }

    /* Wait for completions to drain */
    while (outstanding > 0) Sleep(1);

    QueryPerformanceCounter(&t1);
    swprintf_s(line, 200, L"[iocp] read %ld bytes in %.2f ms\r\n",
               totalBytes,
               (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart);
    AsPostLine(task->target, line);

    /* Shut down workers with NULL OVERLAPPED sentinels */
    for (i = 0; i < IOCP_WORKERS; ++i) {
        PostQueuedCompletionStatus(iocp, 0, 0, NULL);
    }
    WaitForMultipleObjects(IOCP_WORKERS, workers, TRUE, 2000);
    for (i = 0; i < IOCP_WORKERS; ++i) CloseHandle(workers[i]);
    CloseHandle(iocp);
    CloseHandle(file);
}

static DWORD WINAPI As_Worker(LPVOID arg)
{
    AsTask *task = (AsTask *)arg;
    if (task->mode == 0) As_RunOverlapped(task);
    else                 As_RunIOCP(task);
    PostMessageW(task->target, WM_AS_DONE, 0, 0);
    free(task);
    return 0;
}

static void As_Start(HWND frame, AsState *st, int mode)
{
    AsTask *task;
    DWORD tid;
    if (st->thread || st->path[0] == 0) return;

    task = (AsTask *)calloc(1, sizeof(AsTask));
    if (!task) return;
    task->target = frame;
    task->mode = mode;
    wcscpy_s(task->path, MAX_PATH, st->path);

    EnableWindow(st->ovlBtn, FALSE);
    EnableWindow(st->iocpBtn, FALSE);
    st->thread = CreateThread(NULL, 0, As_Worker, task, 0, &tid);
    if (!st->thread) {
        free(task);
        EnableWindow(st->ovlBtn, TRUE);
        EnableWindow(st->iocpBtn, TRUE);
    }
}

static LRESULT CALLBACK As_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    AsState *st = (AsState *)GetPropW(hwnd, AS_PROP);

    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_AS_PICK) {
            OPENFILENAMEW ofn;
            wchar_t file[MAX_PATH] = L"";
            ZeroMemory(&ofn, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner   = hwnd;
            ofn.lpstrFile   = file;
            ofn.nMaxFile    = MAX_PATH;
            ofn.lpstrFilter = L"All files\0*.*\0";
            ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
            if (GetOpenFileNameW(&ofn)) {
                wcscpy_s(st->path, MAX_PATH, file);
                SetWindowTextW(st->pathLabel, file);
            }
            return 0;
        }
        if (LOWORD(wp) == ID_AS_OVL)  { As_Start(hwnd, st, 0); return 0; }
        if (LOWORD(wp) == ID_AS_IOCP) { As_Start(hwnd, st, 1); return 0; }
    }
    if (msg == WM_AS_LINE && st) {
        wchar_t *txt = (wchar_t *)wp;
        if (txt) { As_Append(st->output, txt); free(txt); }
        return 0;
    }
    if (msg == WM_AS_DONE && st) {
        if (st->thread) {
            WaitForSingleObject(st->thread, 500);
            CloseHandle(st->thread);
            st->thread = NULL;
        }
        EnableWindow(st->ovlBtn, TRUE);
        EnableWindow(st->iocpBtn, TRUE);
        return 0;
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->pickBtn,   8,  34, 80,      24, TRUE);
        MoveWindow(st->pathLabel, 96, 38, w - 104, 18, TRUE);
        MoveWindow(st->ovlBtn,    8,  64, 130, 24, TRUE);
        MoveWindow(st->iocpBtn,   146, 64, 130, 24, TRUE);
        MoveWindow(st->output,    8,  96, w - 16, h - 104, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->thread) { WaitForSingleObject(st->thread, 2000); CloseHandle(st->thread); }
        free(st);
        RemovePropW(hwnd, AS_PROP);
    }
    return CallWindowProcW(g_origAsFrame, hwnd, msg, wp, lp);
}

static HWND Async_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    AsState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Async",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (AsState *)calloc(1, sizeof(AsState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->pickBtn = CreateWindowExW(0, L"BUTTON", L"Pick file...",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        8, 34, 80, 24, frame, (HMENU)(LONG_PTR)ID_AS_PICK, hInstance, NULL);
    st->pathLabel = CreateWindowExW(0, L"STATIC", L"(no file selected)",
        WS_CHILD | WS_VISIBLE,
        96, 38, w - 104, 18, frame, (HMENU)(LONG_PTR)ID_AS_PATH, hInstance, NULL);

    st->ovlBtn = CreateWindowExW(0, L"BUTTON", L"Overlapped read",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        8, 64, 130, 24, frame, (HMENU)(LONG_PTR)ID_AS_OVL, hInstance, NULL);
    st->iocpBtn = CreateWindowExW(0, L"BUTTON", L"IOCP read",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        146, 64, 130, 24, frame, (HMENU)(LONG_PTR)ID_AS_IOCP, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        8, 96, w - 16, h - 104, frame, (HMENU)(LONG_PTR)ID_AS_OUT, hInstance, NULL);

    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, AS_PROP, (HANDLE)st);
    if (!g_origAsFrame) g_origAsFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)As_FrameProc);
    return frame;
}

MsApp g_AppAsync = {
    L"Async",
    Async_Create,
    600, 400
};
