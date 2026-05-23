/*
 * app_setthrdesc.c — Thread naming via SetThreadDescription (Win 10 1607+)
 *
 * Demonstrates SetThreadDescription / GetThreadDescription — the modern,
 * official replacement for the old MS_VC_EXCEPTION (0x406D1388) "magic
 * exception" trick that named threads for the Visual Studio debugger.
 *
 *   - SetThreadDescription(hThread, L"name") — kernel32 export, Win 10+
 *   - GetThreadDescription(hThread, &PWSTR) — returns LocalAlloc'd wide
 *     string that the caller must LocalFree
 *   - OpenThread(THREAD_QUERY_LIMITED_INFORMATION) opens an arbitrary TID
 *
 * Loaded dynamically because the imports are absent on Win 7/8.
 * We spawn 3 worker threads with distinct names and then read them
 * back via GetThreadDescription.
 */

#include "shell.h"
#include <stdlib.h>
#include <stdio.h>

typedef HRESULT (WINAPI *PFN_SetThreadDescription)(HANDLE, PCWSTR);
typedef HRESULT (WINAPI *PFN_GetThreadDescription)(HANDLE, PWSTR *);

#define ST_PROP   L"MS_ST_STATE"
#define ID_ST_GO  107001
#define ID_ST_OUT 107002

typedef struct {
    HWND     output;
    HMODULE  k32;
    PFN_SetThreadDescription pSet;
    PFN_GetThreadDescription pGet;
} StState;
static WNDPROC g_origStFrame = NULL;

static void St_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
}

typedef struct {
    StState *st;
    const wchar_t *name;
    HANDLE startedEvent;
} StThreadArg;

static DWORD WINAPI St_Worker(LPVOID arg)
{
    StThreadArg *a = (StThreadArg *)arg;
    if (a->st->pSet) {
        a->st->pSet(GetCurrentThread(), a->name);
    }
    SetEvent(a->startedEvent);
    /* Hold the thread alive briefly so the UI can query it */
    Sleep(500);
    return 0;
}

static void St_RunDemo(StState *st)
{
    const wchar_t *names[3] = {
        L"MS-Producer",
        L"MS-Renderer",
        L"MS-IOQueueDispatcher"
    };
    HANDLE threads[3];
    DWORD  tids[3];
    StThreadArg args[3];
    HANDLE started[3];
    int i;

    SetWindowTextW(st->output, L"");
    if (!st->pSet || !st->pGet) {
        St_Append(st->output,
            L"SetThreadDescription / GetThreadDescription unavailable.\r\n"
            L"(Requires Windows 10 version 1607 or newer.)\r\n");
        return;
    }
    St_Append(st->output, L"Spawning 3 named worker threads...\r\n");

    for (i = 0; i < 3; ++i) {
        started[i] = CreateEventW(NULL, TRUE, FALSE, NULL);
        args[i].st = st;
        args[i].name = names[i];
        args[i].startedEvent = started[i];
        threads[i] = CreateThread(NULL, 0, St_Worker, &args[i], 0, &tids[i]);
    }

    /* Wait for all three to have called SetThreadDescription */
    WaitForMultipleObjects(3, started, TRUE, 2000);

    St_Append(st->output, L"\r\nReading back names via GetThreadDescription:\r\n");
    for (i = 0; i < 3; ++i) {
        PWSTR name = NULL;
        HRESULT hr = st->pGet(threads[i], &name);
        if (SUCCEEDED(hr) && name) {
            wchar_t line[200];
            swprintf_s(line, 200, L"  TID %-6lu  '%s'\r\n", tids[i], name);
            St_Append(st->output, line);
            LocalFree(name);
        } else {
            wchar_t line[100];
            swprintf_s(line, 100, L"  TID %-6lu  (failed: 0x%08lx)\r\n", tids[i], hr);
            St_Append(st->output, line);
        }
    }

    /* Wait for the workers to exit */
    WaitForMultipleObjects(3, threads, TRUE, 1500);
    for (i = 0; i < 3; ++i) {
        CloseHandle(threads[i]);
        CloseHandle(started[i]);
    }
    St_Append(st->output, L"\r\nDemo done.\r\n");
}

static LRESULT CALLBACK St_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    StState *st = (StState *)GetPropW(hwnd, ST_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_ST_GO) { St_RunDemo(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->k32) FreeLibrary(st->k32);
        free(st); RemovePropW(hwnd, ST_PROP);
    }
    return CallWindowProcW(g_origStFrame, hwnd, msg, wp, lp);
}

static HWND SetThrDesc_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    StState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"SetThrDesc",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (StState *)calloc(1, sizeof(StState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->k32 = LoadLibraryW(L"kernel32.dll");
    if (st->k32) {
        st->pSet = (PFN_SetThreadDescription)GetProcAddress(st->k32, "SetThreadDescription");
        st->pGet = (PFN_GetThreadDescription)GetProcAddress(st->k32, "GetThreadDescription");
    }

    CreateWindowExW(0, L"BUTTON", L"Run demo",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 110, 26, frame, (HMENU)(LONG_PTR)ID_ST_GO, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"Click Run demo. Creates 3 threads, names them via\r\n"
        L"SetThreadDescription, reads them back via GetThreadDescription.\r\n",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_ST_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, ST_PROP, (HANDLE)st);
    if (!g_origStFrame) g_origStFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)St_FrameProc);
    return frame;
}

MsApp g_AppSetThrDesc = { L"SetThrDesc", SetThrDesc_Create, 660, 360 };
