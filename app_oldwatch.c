/*
 * app_oldwatch.c — Legacy folder change notifications
 *
 * Demonstrates the older, simpler change-notification API as a contrast
 * to Batch 9's ReadDirectoryChangesW:
 *   - FindFirstChangeNotificationW(path, watchSubtree, filter)
 *     returns a HANDLE that becomes signaled when *any* matching change
 *     occurs, without telling you what or where
 *   - WaitForSingleObject (or WaitForMultipleObjects) blocks
 *   - FindNextChangeNotification rearms the handle
 *   - FindCloseChangeNotification cleans up
 *
 * Differences from ReadDirectoryChangesW:
 *   - This API does NOT report which file changed or what changed.
 *   - It's a single bit: "something matching the filter happened."
 *   - No overlapped/IOCP needed; the handle is waitable directly.
 *
 * We watch with FILE_NOTIFY_CHANGE_FILE_NAME | _LAST_WRITE in a worker
 * thread and post a "ping" back to the UI on each notification.
 */

#include "shell.h"
#include <stdlib.h>
#include <stdio.h>

#define OW_PROP    L"MS_OW_STATE"
#define ID_OW_PATH 87001
#define ID_OW_BR   87002
#define ID_OW_GO   87003
#define ID_OW_STOP 87004
#define ID_OW_OUT  87005

#define WM_OW_PING (WM_USER + 180)
#define WM_OW_DONE (WM_USER + 181)

typedef struct {
    HWND   frame, pathEdit, browseBtn, goBtn, stopBtn, output;
    HANDLE thread, stopEvent;
} OwState;

static WNDPROC g_origOwFrame = NULL;

static void Ow_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
    SendMessageW(e, EM_SCROLLCARET, 0, 0);
}

static DWORD WINAPI Ow_Worker(LPVOID arg)
{
    OwState *st = (OwState *)arg;
    wchar_t path[MAX_PATH];
    HANDLE  hChange;
    HANDLE  waits[2];
    DWORD   tick = 0;

    GetWindowTextW(st->pathEdit, path, MAX_PATH);

    hChange = FindFirstChangeNotificationW(path, TRUE,
        FILE_NOTIFY_CHANGE_FILE_NAME |
        FILE_NOTIFY_CHANGE_DIR_NAME |
        FILE_NOTIFY_CHANGE_LAST_WRITE |
        FILE_NOTIFY_CHANGE_SIZE);
    if (hChange == INVALID_HANDLE_VALUE) {
        PostMessageW(st->frame, WM_OW_PING, 0,
                      (LPARAM)_wcsdup(L"FindFirstChangeNotification failed.\r\n"));
        PostMessageW(st->frame, WM_OW_DONE, 0, 0);
        return 0;
    }

    waits[0] = st->stopEvent;
    waits[1] = hChange;

    PostMessageW(st->frame, WM_OW_PING, 0,
                  (LPARAM)_wcsdup(L"Watching (subtree, name+write+size)...\r\n"));

    for (;;) {
        DWORD r = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (r == WAIT_OBJECT_0) break;  /* stop */
        if (r == WAIT_OBJECT_0 + 1) {
            wchar_t *line = (wchar_t *)malloc(120 * sizeof(wchar_t));
            if (line) {
                swprintf_s(line, 120, L"  change #%lu signaled.\r\n", ++tick);
                PostMessageW(st->frame, WM_OW_PING, 0, (LPARAM)line);
            }
            if (!FindNextChangeNotification(hChange)) {
                PostMessageW(st->frame, WM_OW_PING, 0,
                              (LPARAM)_wcsdup(L"FindNextChangeNotification failed.\r\n"));
                break;
            }
        } else break;
    }

    FindCloseChangeNotification(hChange);
    PostMessageW(st->frame, WM_OW_DONE, 0, 0);
    return 0;
}

static void Ow_Browse(OwState *st)
{
    /* Use SHBrowseForFolder for a simple folder picker. */
    BROWSEINFOW bi;
    LPITEMIDLIST pidl;
    wchar_t buf[MAX_PATH] = L"";
    ZeroMemory(&bi, sizeof(bi));
    bi.hwndOwner = st->frame;
    bi.lpszTitle = L"Pick a folder to watch";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        if (SHGetPathFromIDListW(pidl, buf)) SetWindowTextW(st->pathEdit, buf);
        CoTaskMemFree(pidl);
    }
}

static void Ow_Start(OwState *st)
{
    DWORD tid;
    if (st->thread) return;
    ResetEvent(st->stopEvent);
    st->thread = CreateThread(NULL, 0, Ow_Worker, st, 0, &tid);
}

static void Ow_Stop(OwState *st)
{
    if (!st->thread) return;
    SetEvent(st->stopEvent);
    WaitForSingleObject(st->thread, 2000);
    CloseHandle(st->thread);
    st->thread = NULL;
}

static LRESULT CALLBACK Ow_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    OwState *st = (OwState *)GetPropW(hwnd, OW_PROP);
    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_OW_GO)   { Ow_Start(st); return 0; }
        if (LOWORD(wp) == ID_OW_STOP) { Ow_Stop(st);  return 0; }
        if (LOWORD(wp) == ID_OW_BR)   { Ow_Browse(st); return 0; }
    }
    if (msg == WM_OW_PING && st) {
        wchar_t *p = (wchar_t *)lp;
        if (p) { Ow_Append(st->output, p); free(p); }
        return 0;
    }
    if (msg == WM_OW_DONE && st) {
        if (st->thread) { CloseHandle(st->thread); st->thread = NULL; }
        Ow_Append(st->output, L"[worker exited]\r\n");
        return 0;
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->pathEdit,  12, 38, w - 240, 24, TRUE);
        MoveWindow(st->browseBtn, w - 224, 38, 80, 24, TRUE);
        MoveWindow(st->goBtn,     w - 140, 38, 60, 24, TRUE);
        MoveWindow(st->stopBtn,   w - 76,  38, 60, 24, TRUE);
        MoveWindow(st->output,    8, 70, w - 16, h - 78, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        Ow_Stop(st);
        if (st->stopEvent) CloseHandle(st->stopEvent);
        free(st); RemovePropW(hwnd, OW_PROP);
    }
    return CallWindowProcW(g_origOwFrame, hwnd, msg, wp, lp);
}

static HWND OldWatch_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    OwState *st;
    HFONT mono;
    wchar_t initialPath[MAX_PATH] = L"";
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"OldWatch",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (OwState *)calloc(1, sizeof(OwState));
    if (!st) { DestroyWindow(frame); return NULL; }
    st->frame = frame;
    st->stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    ExpandEnvironmentStringsW(L"%TEMP%", initialPath, MAX_PATH);

    st->pathEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", initialPath,
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        12, 38, w - 240, 24, frame, (HMENU)(LONG_PTR)ID_OW_PATH, hInstance, NULL);
    st->browseBtn = CreateWindowExW(0, L"BUTTON", L"Browse",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        w - 224, 38, 80, 24, frame, (HMENU)(LONG_PTR)ID_OW_BR, hInstance, NULL);
    st->goBtn = CreateWindowExW(0, L"BUTTON", L"Watch",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        w - 140, 38, 60, 24, frame, (HMENU)(LONG_PTR)ID_OW_GO, hInstance, NULL);
    st->stopBtn = CreateWindowExW(0, L"BUTTON", L"Stop",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        w - 76, 38, 60, 24, frame, (HMENU)(LONG_PTR)ID_OW_STOP, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 70, w - 16, h - 78, frame, (HMENU)(LONG_PTR)ID_OW_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, OW_PROP, (HANDLE)st);
    if (!g_origOwFrame) g_origOwFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Ow_FrameProc);
    return frame;
}

MsApp g_AppOldWatch = { L"OldWatch", OldWatch_Create, 720, 400 };
