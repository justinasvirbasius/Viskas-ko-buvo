/*
 * app_shellwins.c — Enumerate open Explorer windows via IShellWindows
 *
 * Demonstrates IShellWindows, a COM service that exposes every open
 * Explorer (and Internet Explorer, in legacy systems) window:
 *   - CoCreateInstance(CLSID_ShellWindows) → IShellWindows
 *   - IShellWindows::get_Count returns the number of registered windows
 *   - IShellWindows::Item(VARIANT i, &IDispatch) returns each one as a
 *     dispatchable (IWebBrowser2-compatible) object
 *   - QueryInterface for IWebBrowser2, then get_LocationName, get_LocationURL,
 *     get_HWND — these work because Explorer windows implement IWebBrowser2
 *
 * This is how third-party shells / power-tools discover where the user
 * has Explorer open without polling top-level windows.
 */

#define COBJMACROS
#define CINTERFACE
#define INITGUID

#include "shell.h"
#include <objbase.h>
#include <exdisp.h>
#include <exdispid.h>
#include <shlobj.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")

#define SW_PROP   L"MS_SW_STATE"
#define ID_SW_REF 97001
#define ID_SW_OUT 97002

typedef struct { HWND refresh, output; BOOL comOk; } SwState;
static WNDPROC g_origSwFrame = NULL;

static void Sw_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
}

static void Sw_Refresh(SwState *st)
{
    IShellWindows *sw = NULL;
    HRESULT hr;
    LONG count = 0;
    LONG i;

    SetWindowTextW(st->output, L"");

    hr = CoCreateInstance(&CLSID_ShellWindows, NULL,
            CLSCTX_LOCAL_SERVER, &IID_IShellWindows, (void **)&sw);
    if (FAILED(hr) || !sw) {
        wchar_t buf[80];
        swprintf_s(buf, 80, L"CoCreateInstance(ShellWindows) failed: 0x%08lx\r\n", hr);
        Sw_Append(st->output, buf);
        return;
    }

    IShellWindows_get_Count(sw, &count);
    {
        wchar_t hdr[80];
        swprintf_s(hdr, 80, L"%ld Explorer/IE window(s) registered.\r\n\r\n", count);
        Sw_Append(st->output, hdr);
    }

    for (i = 0; i < count; ++i) {
        VARIANT vi;
        IDispatch *disp = NULL;
        IWebBrowser2 *wb = NULL;

        VariantInit(&vi);
        vi.vt = VT_I4;
        vi.lVal = i;
        if (FAILED(IShellWindows_Item(sw, vi, &disp)) || !disp) {
            VariantClear(&vi);
            continue;
        }
        VariantClear(&vi);

        if (SUCCEEDED(IDispatch_QueryInterface(disp, &IID_IWebBrowser2, (void **)&wb)) && wb) {
            BSTR nm = NULL, url = NULL;
            SHANDLE_PTR hwnd = 0;
            wchar_t line[800];

            IWebBrowser2_get_LocationName(wb, &nm);
            IWebBrowser2_get_LocationURL(wb, &url);
            IWebBrowser2_get_HWND(wb, &hwnd);

            swprintf_s(line, 800,
                L"[%ld]  HWND=0x%p\r\n"
                L"     name: %s\r\n"
                L"     URL : %s\r\n\r\n",
                i, (void *)(LONG_PTR)hwnd,
                nm  ? nm  : L"(none)",
                url ? url : L"(none)");
            Sw_Append(st->output, line);

            if (nm)  SysFreeString(nm);
            if (url) SysFreeString(url);
            IWebBrowser2_Release(wb);
        }
        IDispatch_Release(disp);
    }

    IShellWindows_Release(sw);
}

static LRESULT CALLBACK Sw_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    SwState *st = (SwState *)GetPropW(hwnd, SW_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_SW_REF) { Sw_Refresh(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->refresh, 12, 38, 130, 26, TRUE);
        MoveWindow(st->output,  8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->comOk) CoUninitialize();
        free(st); RemovePropW(hwnd, SW_PROP);
    }
    return CallWindowProcW(g_origSwFrame, hwnd, msg, wp, lp);
}

static HWND ShellWins_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    SwState *st;
    HFONT mono;
    HRESULT hr;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"ShellWins",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (SwState *)calloc(1, sizeof(SwState));
    if (!st) { DestroyWindow(frame); return NULL; }

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    st->comOk = SUCCEEDED(hr);

    st->refresh = CreateWindowExW(0, L"BUTTON", L"Refresh",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 130, 26, frame, (HMENU)(LONG_PTR)ID_SW_REF, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"Click Refresh. Open an Explorer window first to see results.\r\n",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_SW_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, SW_PROP, (HANDLE)st);
    if (!g_origSwFrame) g_origSwFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Sw_FrameProc);
    Sw_Refresh(st);
    return frame;
}

MsApp g_AppShellWins = { L"ShellWins", ShellWins_Create, 720, 460 };
