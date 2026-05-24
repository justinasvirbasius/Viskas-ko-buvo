/*
 * app_gpupref.c — DXGI adapter ranking by GPU preference (Win 10+)
 *
 * Demonstrates IDXGIFactory6 — the Win 10 (1803+) DXGI extension that
 * lets an app ask for adapters in a *preference* order rather than the
 * raw enumeration order. This is how modern games and ML libs find the
 * "high-performance" GPU vs the integrated GPU on hybrid laptops:
 *
 *   - CreateDXGIFactory1(&IID_IDXGIFactory6, &factory) — Factory6 ships
 *     with Win 10 1803; the requested IID upgrades automatically
 *   - IDXGIFactory6::EnumAdapterByGpuPreference(index, preference,
 *     &IID_IDXGIAdapter1, &adapter)
 *     where preference is one of DXGI_GPU_PREFERENCE_UNSPECIFIED,
 *     _MINIMUM_POWER (integrated first), or _HIGH_PERFORMANCE (discrete first)
 *   - IDXGIAdapter1::GetDesc1 yields DXGI_ADAPTER_DESC1 with Description,
 *     vendor/device IDs, dedicated/shared memory, and the Flags field
 *     (DXGI_ADAPTER_FLAG_SOFTWARE bit identifies WARP)
 *
 * We show all three ranking orders side-by-side.
 */

#define COBJMACROS
#define CINTERFACE
#define INITGUID

#include "shell.h"
#include <dxgi1_6.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "dxgi.lib")

#define GP_PROP   L"MS_GP_STATE"
#define ID_GP_OUT 119001
#define ID_GP_GO  119002

typedef struct { HWND output; } GpState;
static WNDPROC g_origGpFrame = NULL;

static void Gp_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
}

static void Gp_DumpRank(GpState *st, IDXGIFactory6 *f6, DXGI_GPU_PREFERENCE pref, const wchar_t *label)
{
    UINT idx;
    wchar_t hdr[80];
    swprintf_s(hdr, 80, L"\r\n== %s ==\r\n", label);
    Gp_Append(st->output, hdr);

    for (idx = 0; ; ++idx) {
        IDXGIAdapter1 *adapter = NULL;
        DXGI_ADAPTER_DESC1 desc;
        HRESULT hr;

        hr = IDXGIFactory6_EnumAdapterByGpuPreference(f6, idx, pref,
                &IID_IDXGIAdapter1, (void **)&adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr) || !adapter) break;

        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &desc))) {
            wchar_t line[400];
            swprintf_s(line, 400,
                L"  [%u] %-44s  Vendor=0x%04x  Device=0x%04x  vmem=%lluMB  %s\r\n",
                idx, desc.Description,
                desc.VendorId, desc.DeviceId,
                (ULONGLONG)desc.DedicatedVideoMemory / (1024 * 1024),
                (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? L"(SOFTWARE/WARP)" : L"");
            Gp_Append(st->output, line);
        }
        IDXGIAdapter1_Release(adapter);
    }
}

static void Gp_RunDemo(GpState *st)
{
    IDXGIFactory6 *f6 = NULL;
    HRESULT hr;

    SetWindowTextW(st->output, L"");

    hr = CreateDXGIFactory1(&IID_IDXGIFactory6, (void **)&f6);
    if (FAILED(hr) || !f6) {
        wchar_t buf[200];
        swprintf_s(buf, 200,
            L"CreateDXGIFactory1(IDXGIFactory6) failed: 0x%08lx\r\n"
            L"(IDXGIFactory6 requires Windows 10 1803 or newer.)\r\n", hr);
        Gp_Append(st->output, buf);
        return;
    }
    Gp_Append(st->output, L"IDXGIFactory6 obtained.\r\n");

    Gp_DumpRank(st, f6, DXGI_GPU_PREFERENCE_UNSPECIFIED,    L"UNSPECIFIED order");
    Gp_DumpRank(st, f6, DXGI_GPU_PREFERENCE_MINIMUM_POWER,  L"MINIMUM_POWER (integrated first)");
    Gp_DumpRank(st, f6, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, L"HIGH_PERFORMANCE (discrete first)");

    IDXGIFactory6_Release(f6);
}

static LRESULT CALLBACK Gp_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    GpState *st = (GpState *)GetPropW(hwnd, GP_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_GP_GO) { Gp_RunDemo(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, GP_PROP); }
    return CallWindowProcW(g_origGpFrame, hwnd, msg, wp, lp);
}

static HWND GpuPref_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    GpState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"GpuPref",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (GpState *)calloc(1, sizeof(GpState));
    if (!st) { DestroyWindow(frame); return NULL; }

    CreateWindowExW(0, L"BUTTON", L"Enumerate by preference",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 200, 26, frame, (HMENU)(LONG_PTR)ID_GP_GO, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_GP_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, GP_PROP, (HANDLE)st);
    if (!g_origGpFrame) g_origGpFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Gp_FrameProc);
    return frame;
}

MsApp g_AppGpuPref = { L"GpuPref", GpuPref_Create, 920, 420 };
