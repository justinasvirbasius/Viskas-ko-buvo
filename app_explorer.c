/*
 * app_explorer.c — Minimal file explorer
 *
 * Shows the contents of a directory in a LISTBOX. Double-click a folder to
 * enter it, ".." to go up. Uses FindFirstFileW / FindNextFileW. Current path
 * is shown in a STATIC at the top.
 */

#include "shell.h"
#include <wchar.h>
#include <shlwapi.h>
#include <stdlib.h>

#define EXP_PROP_STATE L"MS_EXP_STATE"
#define EXP_ID_LIST    2001
#define EXP_ID_PATH    2002

typedef struct {
    HWND    list;
    HWND    pathLabel;
    wchar_t cwd[MAX_PATH];
} ExpState;

static WNDPROC g_origExpProc = NULL;

static void Exp_Populate(ExpState *st)
{
    WIN32_FIND_DATAW fd;
    HANDLE hFind;
    wchar_t pattern[MAX_PATH + 4];

    SendMessageW(st->list, LB_RESETCONTENT, 0, 0);
    SetWindowTextW(st->pathLabel, st->cwd);

    swprintf_s(pattern, MAX_PATH + 4, L"%s\\*", st->cwd);
    hFind = FindFirstFileW(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        SendMessageW(st->list, LB_ADDSTRING, 0, (LPARAM)L"<unable to read directory>");
        return;
    }

    /* Add .. unless we're at a drive root */
    if (wcslen(st->cwd) > 3) {
        SendMessageW(st->list, LB_ADDSTRING, 0, (LPARAM)L"[..]");
    }

    do {
        wchar_t entry[MAX_PATH + 4];
        if (wcscmp(fd.cFileName, L".") == 0 ||
            wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            swprintf_s(entry, MAX_PATH + 4, L"[%s]", fd.cFileName);
        } else {
            wcscpy_s(entry, MAX_PATH + 4, fd.cFileName);
        }
        SendMessageW(st->list, LB_ADDSTRING, 0, (LPARAM)entry);
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

static void Exp_Navigate(ExpState *st, const wchar_t *entry)
{
    wchar_t newPath[MAX_PATH];
    size_t len;

    if (wcscmp(entry, L"[..]") == 0) {
        wcscpy_s(newPath, MAX_PATH, st->cwd);
        len = wcslen(newPath);
        /* Strip trailing slash if any */
        if (len > 0 && newPath[len - 1] == L'\\') newPath[len - 1] = 0;
        /* Drop last component */
        {
            wchar_t *slash = wcsrchr(newPath, L'\\');
            if (slash) {
                if (slash == newPath + 2) slash[1] = 0; /* "C:\" */
                else *slash = 0;
            }
        }
        wcscpy_s(st->cwd, MAX_PATH, newPath);
        Exp_Populate(st);
    } else if (entry[0] == L'[') {
        /* Folder: strip brackets and append */
        wchar_t name[MAX_PATH];
        size_t l;
        wcscpy_s(name, MAX_PATH, entry + 1);
        l = wcslen(name);
        if (l > 0 && name[l - 1] == L']') name[l - 1] = 0;

        swprintf_s(newPath, MAX_PATH, L"%s\\%s", st->cwd, name);
        /* Normalize: avoid C:\\ on root */
        if (wcsstr(newPath, L"\\\\") == newPath + 2) {
            /* remove the doubled slash */
            wmemmove(newPath + 2, newPath + 3, wcslen(newPath + 3) + 1);
        }
        wcscpy_s(st->cwd, MAX_PATH, newPath);
        Exp_Populate(st);
    }
    /* Files: no action — could ShellExecuteW here */
}

static LRESULT CALLBACK Exp_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_COMMAND) {
        if (HIWORD(wp) == LBN_DBLCLK && LOWORD(wp) == EXP_ID_LIST) {
            ExpState *st = (ExpState *)GetPropW(hwnd, EXP_PROP_STATE);
            int sel;
            wchar_t entry[MAX_PATH + 4];
            if (!st) return 0;
            sel = (int)SendMessageW(st->list, LB_GETCURSEL, 0, 0);
            if (sel == LB_ERR) return 0;
            SendMessageW(st->list, LB_GETTEXT, sel, (LPARAM)entry);
            Exp_Navigate(st, entry);
            return 0;
        }
    }
    if (msg == WM_SIZE) {
        ExpState *st = (ExpState *)GetPropW(hwnd, EXP_PROP_STATE);
        if (st) {
            int w = LOWORD(lp), h = HIWORD(lp);
            MoveWindow(st->pathLabel, 8, 34, w - 16, 20, TRUE);
            MoveWindow(st->list,      8, 60, w - 16, h - 68, TRUE);
        }
    }
    if (msg == WM_DESTROY) {
        ExpState *st = (ExpState *)GetPropW(hwnd, EXP_PROP_STATE);
        if (st) free(st);
        RemovePropW(hwnd, EXP_PROP_STATE);
    }
    return CallWindowProcW(g_origExpProc, hwnd, msg, wp, lp);
}

static HWND Explorer_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    ExpState *st;
    DWORD dirLen;

    (void)self;
    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Explorer",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (ExpState *)calloc(1, sizeof(ExpState));
    if (!st) { DestroyWindow(frame); return NULL; }

    dirLen = GetCurrentDirectoryW(MAX_PATH, st->cwd);
    if (dirLen == 0) wcscpy_s(st->cwd, MAX_PATH, L"C:\\");

    st->pathLabel = CreateWindowExW(0, L"STATIC", st->cwd,
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        8, 34, w - 16, 20, frame, (HMENU)(LONG_PTR)EXP_ID_PATH, hInstance, NULL);

    st->list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS,
        8, 60, w - 16, h - 68, frame, (HMENU)(LONG_PTR)EXP_ID_LIST, hInstance, NULL);

    SetPropW(frame, EXP_PROP_STATE, (HANDLE)st);
    if (!g_origExpProc)
        g_origExpProc = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Exp_FrameProc);

    Exp_Populate(st);
    return frame;
}

MsApp g_AppExplorer = {
    L"Explorer",
    Explorer_Create,
    480, 380
};
