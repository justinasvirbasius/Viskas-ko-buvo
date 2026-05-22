/*
 * app_dirwatch.c — Live folder change events via ReadDirectoryChangesW
 *
 * Demonstrates the directory change-notification API used internally by
 * Windows Explorer and most file watchers:
 *   - CreateFileW(dir, FILE_LIST_DIRECTORY, ..., FILE_FLAG_BACKUP_SEMANTICS |
 *                 FILE_FLAG_OVERLAPPED)
 *   - ReadDirectoryChangesW with a notification filter
 *     (FILE_NOTIFY_CHANGE_FILE_NAME | DIR_NAME | LAST_WRITE | SIZE | ATTRIBUTES)
 *   - OVERLAPPED + manual-reset event for blocking-on-event in a worker thread
 *   - FILE_NOTIFY_INFORMATION linked list: Action + FileName (not NUL-terminated)
 *
 * The worker thread posts each event to the UI via PostMessage with a heap
 * wchar_t* payload. The UI thread free()s the payload after appending.
 */

#include "shell.h"
#include <shlobj.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

#define DW_PROP    L"MS_DW_STATE"
#define ID_DW_PICK 60001
#define ID_DW_STOP 60002
#define ID_DW_PATH 60003
#define ID_DW_OUT  60004

#define WM_DW_EVENT (WM_USER + 150)   /* lparam = wchar_t* (heap, freed by UI) */
#define WM_DW_FAIL  (WM_USER + 151)

typedef struct {
    HWND     frame, pathLbl, output;
    HANDLE   dirHandle, stopEvent;
    HANDLE   thread;
    wchar_t  watchPath[MAX_PATH];
} DwState;

static WNDPROC g_origDwFrame = NULL;

static const wchar_t *Dw_Action(DWORD a)
{
    switch (a) {
    case FILE_ACTION_ADDED:            return L"added";
    case FILE_ACTION_REMOVED:          return L"removed";
    case FILE_ACTION_MODIFIED:         return L"modified";
    case FILE_ACTION_RENAMED_OLD_NAME: return L"renamed-from";
    case FILE_ACTION_RENAMED_NEW_NAME: return L"renamed-to";
    }
    return L"?";
}

static DWORD WINAPI Dw_Worker(LPVOID arg)
{
    DwState *st = (DwState *)arg;
    BYTE buffer[8192];
    OVERLAPPED ov;
    HANDLE waits[2];

    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    waits[0] = st->stopEvent;
    waits[1] = ov.hEvent;

    for (;;) {
        DWORD bytes = 0, wait;
        BOOL  rc;

        ResetEvent(ov.hEvent);
        rc = ReadDirectoryChangesW(st->dirHandle, buffer, sizeof(buffer), FALSE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE |
                FILE_NOTIFY_CHANGE_ATTRIBUTES,
                &bytes, &ov, NULL);
        if (!rc) { PostMessageW(st->frame, WM_DW_FAIL, 0, 0); break; }

        wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0) {
            /* stop signaled */
            CancelIo(st->dirHandle);
            break;
        }
        if (wait != WAIT_OBJECT_0 + 1) break;

        if (!GetOverlappedResult(st->dirHandle, &ov, &bytes, FALSE)) {
            PostMessageW(st->frame, WM_DW_FAIL, 0, 0);
            break;
        }
        if (bytes == 0) {
            /* buffer overflowed — too many changes */
            wchar_t *msg = _wcsdup(L"(too many changes — buffer overflowed)");
            PostMessageW(st->frame, WM_DW_EVENT, 0, (LPARAM)msg);
            continue;
        }
        {
            FILE_NOTIFY_INFORMATION *info = (FILE_NOTIFY_INFORMATION *)buffer;
            for (;;) {
                int nameLen = info->FileNameLength / sizeof(wchar_t);
                /* The name is NOT NUL-terminated in the structure */
                wchar_t *payload = (wchar_t *)malloc(
                    (nameLen + 80) * sizeof(wchar_t));
                if (payload) {
                    int written = swprintf_s(payload, nameLen + 80,
                        L"  %s: %.*s\r\n",
                        Dw_Action(info->Action),
                        nameLen, info->FileName);
                    (void)written;
                    PostMessageW(st->frame, WM_DW_EVENT, 0, (LPARAM)payload);
                }
                if (info->NextEntryOffset == 0) break;
                info = (FILE_NOTIFY_INFORMATION *)
                    ((BYTE *)info + info->NextEntryOffset);
            }
        }
    }
    CloseHandle(ov.hEvent);
    return 0;
}

static void Dw_Stop(DwState *st)
{
    if (st->thread) {
        SetEvent(st->stopEvent);
        WaitForSingleObject(st->thread, 3000);
        CloseHandle(st->thread);
        st->thread = NULL;
    }
    if (st->dirHandle && st->dirHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(st->dirHandle);
        st->dirHandle = NULL;
    }
}

static void Dw_Start(DwState *st, const wchar_t *path)
{
    Dw_Stop(st);
    wcscpy_s(st->watchPath, MAX_PATH, path);
    SetWindowTextW(st->pathLbl, path);
    ResetEvent(st->stopEvent);

    st->dirHandle = CreateFileW(path,
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        NULL);
    if (st->dirHandle == INVALID_HANDLE_VALUE) {
        st->dirHandle = NULL;
        SetWindowTextW(st->pathLbl, L"(unable to open folder)");
        return;
    }
    {
        DWORD tid;
        st->thread = CreateThread(NULL, 0, Dw_Worker, st, 0, &tid);
    }
}

static void Dw_Pick(HWND hwnd, DwState *st)
{
    BROWSEINFOW bi;
    LPITEMIDLIST pidl;
    wchar_t path[MAX_PATH];
    ZeroMemory(&bi, sizeof(bi));
    bi.hwndOwner = hwnd;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;
    bi.lpszTitle = L"Pick a folder to watch";
    pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    if (SHGetPathFromIDListW(pidl, path)) {
        Dw_Start(st, path);
    }
    CoTaskMemFree(pidl);
}

static void Dw_Append(DwState *st, const wchar_t *t)
{
    int len = GetWindowTextLengthW(st->output);
    SendMessageW(st->output, EM_SETSEL, len, len);
    SendMessageW(st->output, EM_REPLACESEL, FALSE, (LPARAM)t);
    SendMessageW(st->output, EM_SCROLLCARET, 0, 0);
}

static LRESULT CALLBACK Dw_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    DwState *st = (DwState *)GetPropW(hwnd, DW_PROP);

    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_DW_PICK) { Dw_Pick(hwnd, st); return 0; }
        if (LOWORD(wp) == ID_DW_STOP) { Dw_Stop(st); SetWindowTextW(st->pathLbl, L"(stopped)"); return 0; }
    }
    if (msg == WM_DW_EVENT && st) {
        wchar_t *payload = (wchar_t *)lp;
        if (payload) { Dw_Append(st, payload); free(payload); }
        return 0;
    }
    if (msg == WM_DW_FAIL && st) {
        Dw_Append(st, L"(watch failed)\r\n");
        return 0;
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->pathLbl, 12, 70, w - 24, 22, TRUE);
        MoveWindow(st->output,  8, 100, w - 16, h - 108, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        Dw_Stop(st);
        if (st->stopEvent) CloseHandle(st->stopEvent);
        free(st);
        RemovePropW(hwnd, DW_PROP);
    }
    return CallWindowProcW(g_origDwFrame, hwnd, msg, wp, lp);
}

static HWND DirWatch_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    DwState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"DirWatch",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (DwState *)calloc(1, sizeof(DwState));
    if (!st) { DestroyWindow(frame); return NULL; }
    st->frame = frame;
    st->stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    CreateWindowExW(0, L"BUTTON", L"Pick folder…",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 130, 26, frame, (HMENU)(LONG_PTR)ID_DW_PICK, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Stop",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        148, 38, 90, 26, frame, (HMENU)(LONG_PTR)ID_DW_STOP, hInstance, NULL);

    st->pathLbl = CreateWindowExW(0, L"STATIC", L"(no folder)",
        WS_CHILD | WS_VISIBLE,
        12, 70, w - 24, 22, frame, (HMENU)(LONG_PTR)ID_DW_PATH, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 100, w - 16, h - 108, frame, (HMENU)(LONG_PTR)ID_DW_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, DW_PROP, (HANDLE)st);
    if (!g_origDwFrame) g_origDwFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Dw_FrameProc);
    return frame;
}

MsApp g_AppDirWatch = {
    L"DirWatch",
    DirWatch_Create,
    640, 420
};
