/*
 * app_opendlg.c — Modern Common Item Dialog (IFileOpenDialog)
 *
 * Demonstrates the Vista+ replacement for GetOpenFileName:
 *   - CoCreateInstance(CLSID_FileOpenDialog) returns IFileOpenDialog
 *   - SetFileTypes for filter, SetTitle, SetOptions (FOS_PICKFOLDERS for dirs)
 *   - Show, GetResults (IShellItemArray for multi-select)
 *   - IShellItem::GetDisplayName(SIGDN_FILESYSPATH)
 *
 * Two buttons: pick file(s), pick folder. Results listed in a multi-line edit.
 */

#define COBJMACROS
#define CINTERFACE
#define INITGUID

#include "shell.h"
#include <objbase.h>
#include <shobjidl.h>
#include <stdlib.h>

#pragma comment(lib, "ole32.lib")

#define OD_PROP    L"MS_OD_STATE"
#define ID_OD_FILE 38001
#define ID_OD_DIR  38002
#define ID_OD_OUT  38003

typedef struct {
    HWND output;
    BOOL comOk;
} OdState;

static WNDPROC g_origOdFrame = NULL;

static void Od_Append(OdState *st, const wchar_t *t)
{
    int len = GetWindowTextLengthW(st->output);
    SendMessageW(st->output, EM_SETSEL, len, len);
    SendMessageW(st->output, EM_REPLACESEL, FALSE, (LPARAM)t);
    SendMessageW(st->output, EM_SCROLLCARET, 0, 0);
}

static void Od_PickFile(HWND owner, OdState *st)
{
    IFileOpenDialog *dlg = NULL;
    HRESULT hr;
    DWORD opts = 0;
    IShellItemArray *items = NULL;
    DWORD count = 0, i;

    hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IFileOpenDialog, (void **)&dlg);
    if (FAILED(hr)) {
        Od_Append(st, L"CoCreateInstance(FileOpenDialog) failed\r\n");
        return;
    }

    IFileOpenDialog_GetOptions(dlg, &opts);
    IFileOpenDialog_SetOptions(dlg,
        opts | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM);

    {
        COMDLG_FILTERSPEC filters[2];
        filters[0].pszName = L"Text files";
        filters[0].pszSpec = L"*.txt;*.md;*.log";
        filters[1].pszName = L"All files";
        filters[1].pszSpec = L"*.*";
        IFileOpenDialog_SetFileTypes(dlg, 2, filters);
    }
    IFileOpenDialog_SetTitle(dlg, L"Pick one or more files");

    hr = IFileOpenDialog_Show(dlg, owner);
    if (FAILED(hr)) {
        if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
            Od_Append(st, L"(cancelled)\r\n");
        IFileOpenDialog_Release(dlg);
        return;
    }

    if (SUCCEEDED(IFileOpenDialog_GetResults(dlg, &items))) {
        IShellItemArray_GetCount(items, &count);
        for (i = 0; i < count; ++i) {
            IShellItem *item = NULL;
            if (SUCCEEDED(IShellItemArray_GetItemAt(items, i, &item))) {
                LPWSTR path = NULL;
                if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path)) && path) {
                    Od_Append(st, path);
                    Od_Append(st, L"\r\n");
                    CoTaskMemFree(path);
                }
                IShellItem_Release(item);
            }
        }
        IShellItemArray_Release(items);
    }
    IFileOpenDialog_Release(dlg);
}

static void Od_PickFolder(HWND owner, OdState *st)
{
    IFileOpenDialog *dlg = NULL;
    HRESULT hr;
    IShellItem *item = NULL;
    DWORD opts = 0;

    hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IFileOpenDialog, (void **)&dlg);
    if (FAILED(hr)) {
        Od_Append(st, L"CoCreateInstance(FileOpenDialog) failed\r\n");
        return;
    }
    IFileOpenDialog_GetOptions(dlg, &opts);
    IFileOpenDialog_SetOptions(dlg, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    IFileOpenDialog_SetTitle(dlg, L"Pick a folder");

    hr = IFileOpenDialog_Show(dlg, owner);
    if (SUCCEEDED(hr) && SUCCEEDED(IFileOpenDialog_GetResult(dlg, &item))) {
        LPWSTR path = NULL;
        if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path)) && path) {
            Od_Append(st, L"folder: ");
            Od_Append(st, path);
            Od_Append(st, L"\r\n");
            CoTaskMemFree(path);
        }
        IShellItem_Release(item);
    } else if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        Od_Append(st, L"(cancelled)\r\n");
    }
    IFileOpenDialog_Release(dlg);
}

static LRESULT CALLBACK Od_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    OdState *st = (OdState *)GetPropW(hwnd, OD_PROP);

    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_OD_FILE) { Od_PickFile(hwnd, st); return 0; }
        if (LOWORD(wp) == ID_OD_DIR)  { Od_PickFolder(hwnd, st); return 0; }
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 72, w - 16, h - 80, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->comOk) CoUninitialize();
        free(st);
        RemovePropW(hwnd, OD_PROP);
    }
    return CallWindowProcW(g_origOdFrame, hwnd, msg, wp, lp);
}

static HWND OpenDlg_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    OdState *st;
    HRESULT hr;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"OpenDlg",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (OdState *)calloc(1, sizeof(OdState));
    if (!st) { DestroyWindow(frame); return NULL; }

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    st->comOk = SUCCEEDED(hr);

    CreateWindowExW(0, L"BUTTON", L"Pick file(s)…",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 36, 130, 28, frame, (HMENU)(LONG_PTR)ID_OD_FILE, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Pick folder…",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        152, 36, 130, 28, frame, (HMENU)(LONG_PTR)ID_OD_DIR, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 72, w - 16, h - 80, frame, (HMENU)(LONG_PTR)ID_OD_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, OD_PROP, (HANDLE)st);
    if (!g_origOdFrame) g_origOdFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Od_FrameProc);
    return frame;
}

MsApp g_AppOpenDlg = {
    L"OpenDlg",
    OpenDlg_Create,
    560, 320
};
