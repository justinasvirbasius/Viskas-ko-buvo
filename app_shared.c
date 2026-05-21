/*
 * app_shared.c — Shared memory between processes
 *
 * Demonstrates page-file-backed shared memory:
 *   - CreateFileMappingW with INVALID_HANDLE_VALUE (no real file)
 *   - MapViewOfFile to get a pointer all instances see
 *   - A named mutex (CreateMutexW) to serialize updates
 *
 * Every MiniShell process and every instance of this app within it sees the
 * same counter. Click Increment to bump it; the timer polls so updates from
 * other windows appear here even if we're idle.
 */

#include "shell.h"
#include <stdlib.h>
#include <stdio.h>

#define SHM_PROP    L"MS_SHM_STATE"
#define ID_SHM_INC  22001
#define ID_SHM_RST  22002
#define SHM_TIMER   1
#define SHM_NAME    L"MiniShell_SharedCounter"
#define SHM_MUTEX   L"MiniShell_SharedCounterMutex"

typedef struct {
    HANDLE  mapping;
    HANDLE  mutex;
    LONG   *counter;
    HWND    label;
    HWND    info;
} ShmState;

static WNDPROC g_origShmFrame = NULL;

static void Shm_UpdateLabel(ShmState *st)
{
    wchar_t buf[64];
    LONG v = 0;
    if (st->counter) {
        WaitForSingleObject(st->mutex, INFINITE);
        v = *st->counter;
        ReleaseMutex(st->mutex);
    }
    swprintf_s(buf, 64, L"Counter: %ld", v);
    SetWindowTextW(st->label, buf);
}

static LRESULT CALLBACK Shm_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    ShmState *st = (ShmState *)GetPropW(hwnd, SHM_PROP);

    if (msg == WM_COMMAND && st && st->counter) {
        if (LOWORD(wp) == ID_SHM_INC) {
            WaitForSingleObject(st->mutex, INFINITE);
            ++(*st->counter);
            ReleaseMutex(st->mutex);
            Shm_UpdateLabel(st);
            return 0;
        }
        if (LOWORD(wp) == ID_SHM_RST) {
            WaitForSingleObject(st->mutex, INFINITE);
            *st->counter = 0;
            ReleaseMutex(st->mutex);
            Shm_UpdateLabel(st);
            return 0;
        }
    }
    if (msg == WM_TIMER && st) { Shm_UpdateLabel(st); return 0; }

    if (msg == WM_DESTROY && st) {
        KillTimer(hwnd, SHM_TIMER);
        if (st->counter) UnmapViewOfFile(st->counter);
        if (st->mapping) CloseHandle(st->mapping);
        if (st->mutex)   CloseHandle(st->mutex);
        free(st);
        RemovePropW(hwnd, SHM_PROP);
    }
    return CallWindowProcW(g_origShmFrame, hwnd, msg, wp, lp);
}

static HWND Shared_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    ShmState *st;
    DWORD existed;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Shared",
        WS_POPUP | WS_BORDER,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (ShmState *)calloc(1, sizeof(ShmState));
    if (!st) { DestroyWindow(frame); return NULL; }

    /* Page-file-backed: pass INVALID_HANDLE_VALUE for the file */
    st->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                     0, sizeof(LONG), SHM_NAME);
    existed = GetLastError();   /* ERROR_ALREADY_EXISTS if another instance got there first */
    st->mutex = CreateMutexW(NULL, FALSE, SHM_MUTEX);

    if (st->mapping) {
        st->counter = (LONG *)MapViewOfFile(st->mapping, FILE_MAP_ALL_ACCESS,
                                            0, 0, sizeof(LONG));
        /* The OS zeroes new pages, so we don't need to init the counter
         * when we're the creator. */
    }

    st->info = CreateWindowExW(0, L"STATIC",
        existed == ERROR_ALREADY_EXISTS
            ? L"Attached to an existing shared counter."
            : L"Created a new shared counter.",
        WS_CHILD | WS_VISIBLE,
        12, 40, w - 24, 18, frame, NULL, hInstance, NULL);

    st->label = CreateWindowExW(0, L"STATIC", L"Counter: 0",
        WS_CHILD | WS_VISIBLE,
        12, 70, w - 24, 26, frame, NULL, hInstance, NULL);

    CreateWindowExW(0, L"BUTTON", L"Increment",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 104, 110, 28, frame, (HMENU)(LONG_PTR)ID_SHM_INC, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Reset",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        132, 104, 90, 28, frame, (HMENU)(LONG_PTR)ID_SHM_RST, hInstance, NULL);

    CreateWindowExW(0, L"STATIC",
        L"Open more Shared windows — they all share this counter.",
        WS_CHILD | WS_VISIBLE,
        12, 148, w - 24, 36, frame, NULL, hInstance, NULL);

    SetPropW(frame, SHM_PROP, (HANDLE)st);
    if (!g_origShmFrame)
        g_origShmFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Shm_FrameProc);

    Shm_UpdateLabel(st);
    SetTimer(frame, SHM_TIMER, 500, NULL);
    return frame;
}

MsApp g_AppShared = {
    L"Shared",
    Shared_Create,
    340, 220
};
