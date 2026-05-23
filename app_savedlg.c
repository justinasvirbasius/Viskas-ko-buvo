/*
 * app_savedlg.c — IFileSaveDialog (modern save-as dialog)
 *
 * Complements Batch 7's OpenDlg which used IFileOpenDialog. The save
 * dialog adds:
 *   - IFileSaveDialog (inherits IFileDialog)
 *   - SetFileTypes with multiple COMDLG_FILTERSPEC entries — the chosen
 *     index after dismissal determines the extension via SetDefaultExtension
 *   - SetDefaultExtension drives the auto-append behavior
 *   - SetClientGuid for per-app remembered location/extension
 *   - GetFileTypeIndex tells us which filter the user picked
 *   - GetResult → IShellItem → GetDisplayName(SIGDN_FILESYSPATH)
 *
 * No file is actually written; we just demonstrate the modern dialog and
 * report the chosen path back.
 */

#define COBJMACROS
#define CINTERFACE
#define INITGUID

#include "shell.h"
#include <objbase.h>
#include <shobjidl.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

#define SD_PROP   L"MS_SD_STATE"
#define ID_SD_GO  89001
#define ID_SD_OUT 89002

/* Per-app GUID — drives "remember location/extension by app" behavior */
static const GUID SAVEDLG_APP_GUID = {
    0x1ee8c5a9, 0x2b3d, 0x4f1f,
    { 0xa4, 0x1c, 0x88, 0xff, 0x21, 0x33, 0x44, 0x55 }
};

typedef struct { HWND output; BOOL comOk; } SdState;
static WNDPROC g_origSdFrame = NULL;

static void Sd_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
}

static void Sd_Show(HWND parent, SdState *st)
{
    IFileSaveDialog *dialog = NULL;
    HRESULT hr;

    COMDLG_FILTERSPEC types[] = {
        { L"Text files",      L"*.txt"  },
        { L"Markdown",        L"*.md"   },
        { L"JSON",            L"*.json" },
        { L"All files",       L"*.*"    },
    };

    hr = CoCreateInstance(&CLSID_FileSaveDialog, NULL, CLSCTX_INPROC_SERVER,
                           &IID_IFileSaveDialog, (void **)&dialog);
    if (FAILED(hr)) {
        wchar_t buf[80];
        swprintf_s(buf, 80, L"CoCreateInstance failed: 0x%08lx\r\n", hr);
        Sd_Append(st->output, buf);
        return;
    }
    IFileSaveDialog_SetTitle(dialog, L"MiniShell — Save (demo)");
    IFileSaveDialog_SetFileName(dialog, L"untitled");
    IFileSaveDialog_SetClientGuid(dialog, &SAVEDLG_APP_GUID);
    IFileSaveDialog_SetFileTypes(dialog,
        sizeof(types) / sizeof(types[0]), types);
    IFileSaveDialog_SetFileTypeIndex(dialog, 1);  /* 1-based */
    IFileSaveDialog_SetDefaultExtension(dialog, L"txt");

    {
        DWORD opts = 0;
        IFileSaveDialog_GetOptions(dialog, &opts);
        IFileSaveDialog_SetOptions(dialog,
            opts | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }

    hr = IFileSaveDialog_Show(dialog, parent);
    if (SUCCEEDED(hr)) {
        IShellItem *item = NULL;
        UINT typeIndex = 0;
        IFileSaveDialog_GetFileTypeIndex(dialog, &typeIndex);
        hr = IFileSaveDialog_GetResult(dialog, &item);
        if (SUCCEEDED(hr) && item) {
            PWSTR path = NULL;
            if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path))) {
                wchar_t line[MAX_PATH + 200];
                swprintf_s(line, MAX_PATH + 200,
                    L"User chose:\r\n  path:        %s\r\n  type index:  %u (%s)\r\n\r\n",
                    path, typeIndex,
                    (typeIndex >= 1 && typeIndex <= 4) ? types[typeIndex - 1].pszName : L"?");
                Sd_Append(st->output, line);
                CoTaskMemFree(path);
            }
            IShellItem_Release(item);
        }
    } else if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        Sd_Append(st->output, L"(cancelled)\r\n");
    } else {
        wchar_t buf[60];
        swprintf_s(buf, 60, L"Show failed: 0x%08lx\r\n", hr);
        Sd_Append(st->output, buf);
    }

    IFileSaveDialog_Release(dialog);
}

static LRESULT CALLBACK Sd_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    SdState *st = (SdState *)GetPropW(hwnd, SD_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_SD_GO) { Sd_Show(hwnd, st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->comOk) CoUninitialize();
        free(st); RemovePropW(hwnd, SD_PROP);
    }
    return CallWindowProcW(g_origSdFrame, hwnd, msg, wp, lp);
}

static HWND SaveDlg_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    SdState *st;
    HFONT mono;
    HRESULT hr;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"SaveDlg",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (SdState *)calloc(1, sizeof(SdState));
    if (!st) { DestroyWindow(frame); return NULL; }

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    st->comOk = SUCCEEDED(hr);

    CreateWindowExW(0, L"STATIC",
        L"IFileSaveDialog with multiple file types and overwrite-prompt.",
        WS_CHILD | WS_VISIBLE,
        12, 30, w - 24, 22, frame, NULL, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Save as...",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 56, 130, 26, frame, (HMENU)(LONG_PTR)ID_SD_GO, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"(no result yet)\r\n",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 92, w - 16, h - 100, frame, (HMENU)(LONG_PTR)ID_SD_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, SD_PROP, (HANDLE)st);
    if (!g_origSdFrame) g_origSdFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Sd_FrameProc);
    return frame;
}

MsApp g_AppSaveDlg = { L"SaveDlg", SaveDlg_Create, 580, 380 };
