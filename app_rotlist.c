/*
 * app_rotlist.c — COM Running Object Table enumeration
 *
 * Demonstrates the Running Object Table (ROT) — the COM-level registry of
 * named, accessible running objects. Used by Office automation, Excel/Word
 * "GetObject" lookups, and IDE-debugger connection points.
 *
 *   - GetRunningObjectTable(0, &rot) → IRunningObjectTable
 *   - IRunningObjectTable::EnumRunning(&enum) → IEnumMoniker
 *   - IEnumMoniker::Next yields IMoniker pointers (each represents an
 *     entry in the ROT)
 *   - IMoniker::GetDisplayName(bindCtx, NULL, &name) returns the friendly
 *     name of that registered object
 *   - CreateBindCtx(0, &ctx) for the binding context that GetDisplayName
 *     wants
 *
 * The display names usually look like file paths (for documents),
 * !-prefixed strings (for IDEs), or CLSID strings.
 */

#define COBJMACROS
#define CINTERFACE

#include "shell.h"
#include <objbase.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "ole32.lib")

#define RO_PROP   L"MS_RO_STATE"
#define ID_RO_REF 101001
#define ID_RO_OUT 101002

typedef struct { HWND refresh, output; BOOL comOk; } RoState;
static WNDPROC g_origRoFrame = NULL;

static void Ro_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
}

static void Ro_Refresh(RoState *st)
{
    IRunningObjectTable *rot = NULL;
    IBindCtx *ctx = NULL;
    IEnumMoniker *en = NULL;
    HRESULT hr;
    int count = 0;

    SetWindowTextW(st->output, L"");
    hr = GetRunningObjectTable(0, &rot);
    if (FAILED(hr) || !rot) {
        Ro_Append(st->output, L"GetRunningObjectTable failed.\r\n");
        return;
    }
    hr = CreateBindCtx(0, &ctx);
    if (FAILED(hr)) {
        Ro_Append(st->output, L"CreateBindCtx failed.\r\n");
        IRunningObjectTable_Release(rot);
        return;
    }

    hr = IRunningObjectTable_EnumRunning(rot, &en);
    if (FAILED(hr) || !en) {
        Ro_Append(st->output, L"EnumRunning failed.\r\n");
        IBindCtx_Release(ctx);
        IRunningObjectTable_Release(rot);
        return;
    }

    {
        IMoniker *mk;
        ULONG fetched;
        while (IEnumMoniker_Next(en, 1, &mk, &fetched) == S_OK && mk) {
            LPOLESTR name = NULL;
            if (SUCCEEDED(IMoniker_GetDisplayName(mk, ctx, NULL, &name)) && name) {
                wchar_t line[800];
                swprintf_s(line, 800, L"  [%3d]  %s\r\n", ++count, name);
                Ro_Append(st->output, line);
                CoTaskMemFree(name);
            } else {
                ++count;
                Ro_Append(st->output, L"  (anonymous moniker)\r\n");
            }
            IMoniker_Release(mk);
            if (count >= 200) break;
        }
    }
    {
        wchar_t hdr[80];
        swprintf_s(hdr, 80, L"\r\n%d ROT entries.\r\n", count);
        Ro_Append(st->output, hdr);
    }

    IEnumMoniker_Release(en);
    IBindCtx_Release(ctx);
    IRunningObjectTable_Release(rot);
}

static LRESULT CALLBACK Ro_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    RoState *st = (RoState *)GetPropW(hwnd, RO_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_RO_REF) { Ro_Refresh(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->refresh, 12, 38, 130, 26, TRUE);
        MoveWindow(st->output,  8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->comOk) CoUninitialize();
        free(st); RemovePropW(hwnd, RO_PROP);
    }
    return CallWindowProcW(g_origRoFrame, hwnd, msg, wp, lp);
}

static HWND RotList_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    RoState *st;
    HFONT mono;
    HRESULT hr;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"RotList",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (RoState *)calloc(1, sizeof(RoState));
    if (!st) { DestroyWindow(frame); return NULL; }

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    st->comOk = SUCCEEDED(hr);

    st->refresh = CreateWindowExW(0, L"BUTTON", L"Refresh",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 130, 26, frame, (HMENU)(LONG_PTR)ID_RO_REF, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_RO_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, RO_PROP, (HANDLE)st);
    if (!g_origRoFrame) g_origRoFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Ro_FrameProc);
    Ro_Refresh(st);
    return frame;
}

MsApp g_AppRotList = { L"RotList", RotList_Create, 720, 460 };
