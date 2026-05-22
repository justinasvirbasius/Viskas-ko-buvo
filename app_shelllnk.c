/*
 * app_shelllnk.c — Create and resolve Windows shortcut (.lnk) files
 *
 * Demonstrates the shell link COM interfaces:
 *   - CoCreateInstance(CLSID_ShellLink) returns IShellLinkW
 *   - IShellLinkW::SetPath / SetDescription / SetArguments / SetWorkingDirectory
 *     / SetIconLocation to configure the link
 *   - QueryInterface for IPersistFile, then ::Save(L".lnk path", TRUE) to write
 *   - For resolve: IPersistFile::Load + IShellLinkW::GetPath / GetDescription
 *
 * Demo: builds a shortcut on the desktop that launches notepad.exe with
 * arguments, then loads it back and shows the parsed target.
 */

#define COBJMACROS
#define CINTERFACE
#define INITGUID

#include "shell.h"
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

#define SL_PROP    L"MS_SL_STATE"
#define ID_SL_MAKE 61001
#define ID_SL_OPEN 61002
#define ID_SL_OUT  61003

typedef struct {
    HWND output;
    BOOL comOk;
    wchar_t lastPath[MAX_PATH];
} SlState;

static WNDPROC g_origSlFrame = NULL;

static void Sl_Append(SlState *st, const wchar_t *t)
{
    int len = GetWindowTextLengthW(st->output);
    SendMessageW(st->output, EM_SETSEL, len, len);
    SendMessageW(st->output, EM_REPLACESEL, FALSE, (LPARAM)t);
    SendMessageW(st->output, EM_SCROLLCARET, 0, 0);
}

static void Sl_BuildDemoPath(wchar_t *out, DWORD cch)
{
    PWSTR desktop = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Desktop, 0, NULL, &desktop))) {
        swprintf_s(out, cch, L"%s\\MiniShell Demo Shortcut.lnk", desktop);
        CoTaskMemFree(desktop);
    } else {
        wcscpy_s(out, cch, L"C:\\MiniShell Demo Shortcut.lnk");
    }
}

static void Sl_Create(SlState *st)
{
    IShellLinkW   *link = NULL;
    IPersistFile  *pf   = NULL;
    wchar_t        path[MAX_PATH];
    HRESULT        hr;

    Sl_BuildDemoPath(path, MAX_PATH);
    wcscpy_s(st->lastPath, MAX_PATH, path);

    hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                           &IID_IShellLinkW, (void **)&link);
    if (FAILED(hr)) { Sl_Append(st, L"CoCreateInstance(ShellLink) failed\r\n"); return; }

    IShellLinkW_SetPath(link, L"%windir%\\system32\\notepad.exe");
    IShellLinkW_SetArguments(link, L"%temp%\\hello.txt");
    IShellLinkW_SetDescription(link, L"Built by MiniShell ShellLnk demo");
    IShellLinkW_SetIconLocation(link, L"%windir%\\system32\\notepad.exe", 0);
    IShellLinkW_SetShowCmd(link, SW_SHOWNORMAL);

    hr = IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&pf);
    if (SUCCEEDED(hr)) {
        hr = IPersistFile_Save(pf, path, TRUE);
        if (SUCCEEDED(hr)) {
            wchar_t buf[MAX_PATH + 80];
            swprintf_s(buf, MAX_PATH + 80, L"Created: %s\r\n", path);
            Sl_Append(st, buf);
        } else {
            Sl_Append(st, L"IPersistFile::Save failed (permission?).\r\n");
        }
        IPersistFile_Release(pf);
    }
    IShellLinkW_Release(link);
}

static void Sl_Open(SlState *st)
{
    IShellLinkW   *link = NULL;
    IPersistFile  *pf   = NULL;
    HRESULT        hr;
    wchar_t        path[MAX_PATH] = L"";
    wchar_t        target[MAX_PATH] = L"", args[MAX_PATH] = L"", desc[260] = L"";
    int            iconIdx = 0;
    wchar_t        iconPath[MAX_PATH] = L"";
    WIN32_FIND_DATAW wfd;

    if (st->lastPath[0])
        wcscpy_s(path, MAX_PATH, st->lastPath);
    else
        Sl_BuildDemoPath(path, MAX_PATH);

    hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                           &IID_IShellLinkW, (void **)&link);
    if (FAILED(hr)) { Sl_Append(st, L"CoCreateInstance failed\r\n"); return; }

    hr = IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&pf);
    if (FAILED(hr)) { IShellLinkW_Release(link); return; }

    hr = IPersistFile_Load(pf, path, STGM_READ);
    if (FAILED(hr)) {
        wchar_t buf[MAX_PATH + 60];
        swprintf_s(buf, MAX_PATH + 60, L"Load failed for %s\r\n", path);
        Sl_Append(st, buf);
        IPersistFile_Release(pf);
        IShellLinkW_Release(link);
        return;
    }
    IShellLinkW_Resolve(link, NULL, SLR_NO_UI | SLR_NOSEARCH);
    IShellLinkW_GetPath(link, target, MAX_PATH, &wfd, 0);
    IShellLinkW_GetArguments(link, args, MAX_PATH);
    IShellLinkW_GetDescription(link, desc, 260);
    IShellLinkW_GetIconLocation(link, iconPath, MAX_PATH, &iconIdx);

    {
        wchar_t buf[1600];
        swprintf_s(buf, 1600,
            L"Parsed shortcut:\r\n"
            L"  path        : %s\r\n"
            L"  target      : %s\r\n"
            L"  arguments   : %s\r\n"
            L"  description : %s\r\n"
            L"  icon        : %s,%d\r\n",
            path, target, args, desc, iconPath, iconIdx);
        Sl_Append(st, buf);
    }

    IPersistFile_Release(pf);
    IShellLinkW_Release(link);
}

static LRESULT CALLBACK Sl_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    SlState *st = (SlState *)GetPropW(hwnd, SL_PROP);
    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_SL_MAKE) { Sl_Create(st); return 0; }
        if (LOWORD(wp) == ID_SL_OPEN) { Sl_Open(st);   return 0; }
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->comOk) CoUninitialize();
        free(st);
        RemovePropW(hwnd, SL_PROP);
    }
    return CallWindowProcW(g_origSlFrame, hwnd, msg, wp, lp);
}

static HWND ShellLnk_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    SlState *st;
    HFONT mono;
    HRESULT hr;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"ShellLnk",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (SlState *)calloc(1, sizeof(SlState));
    if (!st) { DestroyWindow(frame); return NULL; }

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    st->comOk = SUCCEEDED(hr);

    CreateWindowExW(0, L"BUTTON", L"Create on Desktop",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 160, 26, frame, (HMENU)(LONG_PTR)ID_SL_MAKE, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Resolve last",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        180, 38, 130, 26, frame, (HMENU)(LONG_PTR)ID_SL_OPEN, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"Create makes a .lnk on your Desktop pointing at notepad.exe.\r\n"
        L"Resolve loads it back and prints the parsed fields.\r\n\r\n",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_SL_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, SL_PROP, (HANDLE)st);
    if (!g_origSlFrame) g_origSlFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Sl_FrameProc);
    return frame;
}

MsApp g_AppShellLnk = {
    L"ShellLnk",
    ShellLnk_Create,
    640, 400
};
