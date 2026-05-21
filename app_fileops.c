/*
 * app_fileops.c — Shell file operations via SHFileOperationW
 *
 * Demonstrates the Win32 shell's bulk-file operation API which renders the
 * familiar progress dialog with confirmations:
 *   - SHFILEOPSTRUCTW with double-NUL-terminated source and destination paths
 *   - FO_COPY operation
 *   - FOF_NOCONFIRMATION + FOF_ALLOWUNDO + FOF_NOERRORUI flag combinations
 *   - SHFileOperationW returning 0 on success, fAnyOperationsAborted to detect cancel
 *
 * The demo creates a small temp file in %TEMP% and then asks the shell to
 * copy it to a user-picked folder. The shell renders its own progress UI;
 * we just report the outcome.
 */

#include "shell.h"
#include <shellapi.h>
#include <shlobj.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

#define FO_PROP    L"MS_FO_STATE"
#define ID_FO_GO   48001
#define ID_FO_OUT  48002

typedef struct {
    HWND status;
} FoState;

static WNDPROC g_origFoFrame = NULL;

static BOOL Fo_PickFolder(HWND owner, wchar_t *outBuf, DWORD cch)
{
    BROWSEINFOW bi;
    LPITEMIDLIST pidl;
    BOOL ok = FALSE;
    ZeroMemory(&bi, sizeof(bi));
    bi.hwndOwner = owner;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;
    bi.lpszTitle = L"Pick a destination folder";

    pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return FALSE;
    if (SHGetPathFromIDListW(pidl, outBuf)) {
        ok = TRUE;
    }
    CoTaskMemFree(pidl);
    /* Make sure result fits */
    if (ok && wcslen(outBuf) + 1 > cch) ok = FALSE;
    return ok;
}

static BOOL Fo_CreateTempSource(wchar_t *out, DWORD cch)
{
    wchar_t dir[MAX_PATH];
    DWORD n;
    HANDLE f;
    DWORD wr;
    static const char *body =
        "Hello from MiniShell FileOps demo.\r\n"
        "This file was copied via SHFileOperation.\r\n";

    n = GetTempPathW(MAX_PATH, dir);
    if (n == 0 || n >= MAX_PATH) return FALSE;

    if (swprintf_s(out, cch, L"%sminishell_fileops.txt", dir) < 0) return FALSE;

    f = CreateFileW(out, GENERIC_WRITE, 0, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    WriteFile(f, body, (DWORD)strlen(body), &wr, NULL);
    CloseHandle(f);
    return TRUE;
}

static void Fo_Append(FoState *st, const wchar_t *t)
{
    int len = GetWindowTextLengthW(st->status);
    SendMessageW(st->status, EM_SETSEL, len, len);
    SendMessageW(st->status, EM_REPLACESEL, FALSE, (LPARAM)t);
}

static void Fo_Run(HWND owner, FoState *st)
{
    wchar_t src[MAX_PATH], dst[MAX_PATH];
    /* Double-NUL-terminated buffers required by SHFILEOPSTRUCT */
    wchar_t srcBuf[MAX_PATH + 2];
    wchar_t dstBuf[MAX_PATH + 2];
    SHFILEOPSTRUCTW op;
    int rc;
    wchar_t line[MAX_PATH + 80];

    if (!Fo_CreateTempSource(src, MAX_PATH)) {
        Fo_Append(st, L"Failed to create temp source.\r\n");
        return;
    }
    swprintf_s(line, MAX_PATH + 80, L"Source: %s\r\n", src);
    Fo_Append(st, line);

    if (!Fo_PickFolder(owner, dst, MAX_PATH)) {
        Fo_Append(st, L"(cancelled folder pick)\r\n");
        return;
    }
    swprintf_s(line, MAX_PATH + 80, L"Destination folder: %s\r\n", dst);
    Fo_Append(st, line);

    ZeroMemory(srcBuf, sizeof(srcBuf));
    ZeroMemory(dstBuf, sizeof(dstBuf));
    wcscpy_s(srcBuf, MAX_PATH + 1, src);
    wcscpy_s(dstBuf, MAX_PATH + 1, dst);

    ZeroMemory(&op, sizeof(op));
    op.hwnd   = owner;
    op.wFunc  = FO_COPY;
    op.pFrom  = srcBuf;
    op.pTo    = dstBuf;
    op.fFlags = FOF_ALLOWUNDO | FOF_NOERRORUI;

    rc = SHFileOperationW(&op);
    if (rc == 0 && !op.fAnyOperationsAborted) {
        Fo_Append(st, L"→ Copy succeeded.\r\n\r\n");
    } else if (op.fAnyOperationsAborted) {
        Fo_Append(st, L"→ Operation aborted by user.\r\n\r\n");
    } else {
        swprintf_s(line, MAX_PATH + 80,
                   L"→ SHFileOperation returned 0x%08X\r\n\r\n", rc);
        Fo_Append(st, line);
    }
}

static LRESULT CALLBACK Fo_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    FoState *st = (FoState *)GetPropW(hwnd, FO_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_FO_GO) { Fo_Run(hwnd, st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->status, 8, 70, w - 16, h - 78, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, FO_PROP); }
    return CallWindowProcW(g_origFoFrame, hwnd, msg, wp, lp);
}

static HWND FileOps_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    FoState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"FileOps",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (FoState *)calloc(1, sizeof(FoState));
    if (!st) { DestroyWindow(frame); return NULL; }

    CreateWindowExW(0, L"STATIC",
        L"Creates a small temp file and uses SHFileOperation to copy it to a folder you pick.",
        WS_CHILD | WS_VISIBLE,
        12, 36, w - 24, 20, frame, NULL, hInstance, NULL);

    CreateWindowExW(0, L"BUTTON", L"Run copy…",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 60, 120, 24, frame, (HMENU)(LONG_PTR)ID_FO_GO, hInstance, NULL);

    st->status = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 90, w - 16, h - 98, frame, (HMENU)(LONG_PTR)ID_FO_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->status, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, FO_PROP, (HANDLE)st);
    if (!g_origFoFrame) g_origFoFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Fo_FrameProc);
    return frame;
}

MsApp g_AppFileOps = {
    L"FileOps",
    FileOps_Create,
    580, 380
};
