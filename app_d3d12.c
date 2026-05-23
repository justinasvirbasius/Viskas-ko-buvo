/*
 * app_d3d12.c — Direct3D 12 device introspection
 *
 * Demonstrates Direct3D 12 (d3d12.dll) — the modern explicit GPU API.
 * Unlike D3D11 (Batch 7), D3D12 requires the app to manage command
 * queues, fences, descriptor heaps, and resource state transitions
 * manually. We just initialize the bare minimum and report device
 * capabilities:
 *
 *   - D3D12CreateDevice(adapter, featureLevel, IID_PPV_ARGS-style, &dev)
 *     creates the ID3D12Device for a given adapter and minimum feature
 *     level (D3D_FEATURE_LEVEL_11_0+ required)
 *   - ID3D12Device::CheckFeatureSupport with D3D12_FEATURE_D3D12_OPTIONS
 *     reports tier levels (resource binding, tiled resources, etc.)
 *   - ID3D12Device::CreateCommandQueue(D3D12_COMMAND_LIST_TYPE_DIRECT)
 *     allocates a GPU command queue
 *   - ID3D12Device::GetDescriptorHandleIncrementSize tells us how big
 *     each descriptor is for a heap type — varies by GPU
 *
 * The DLL is loaded dynamically so the demo runs on systems without D3D12.
 */

#define COBJMACROS
#define CINTERFACE
#define INITGUID

#include "shell.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "dxgi.lib")

typedef HRESULT (WINAPI *PFN_D3D12CreateDevice)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);

#define D12_PROP   L"MS_D12_STATE"
#define ID_D12_OUT 103001

typedef struct {
    HWND     output;
    HMODULE  d3d12;
    ID3D12Device *device;
    ID3D12CommandQueue *queue;
} D12State;

static WNDPROC g_origD12Frame = NULL;

static void D12_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
}

static const wchar_t *D12_FeatureLevel(D3D_FEATURE_LEVEL fl)
{
    switch (fl) {
    case D3D_FEATURE_LEVEL_11_0: return L"11_0";
    case D3D_FEATURE_LEVEL_11_1: return L"11_1";
    case D3D_FEATURE_LEVEL_12_0: return L"12_0";
    case D3D_FEATURE_LEVEL_12_1: return L"12_1";
    }
    return L"?";
}

static void D12_Init(D12State *st)
{
    PFN_D3D12CreateDevice pCreate;
    HRESULT hr;
    D3D_FEATURE_LEVEL flLevels[] = {
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL gotFL = D3D_FEATURE_LEVEL_11_0;
    int i;

    st->d3d12 = LoadLibraryW(L"d3d12.dll");
    if (!st->d3d12) {
        D12_Append(st->output, L"d3d12.dll not available on this system.\r\n");
        return;
    }
    pCreate = (PFN_D3D12CreateDevice)GetProcAddress(st->d3d12, "D3D12CreateDevice");
    if (!pCreate) {
        D12_Append(st->output, L"D3D12CreateDevice not found.\r\n");
        return;
    }

    for (i = 0; i < (int)(sizeof(flLevels)/sizeof(flLevels[0])); ++i) {
        hr = pCreate(NULL, flLevels[i], &IID_ID3D12Device, (void **)&st->device);
        if (SUCCEEDED(hr)) { gotFL = flLevels[i]; break; }
    }
    if (!st->device) {
        wchar_t buf[80];
        swprintf_s(buf, 80, L"D3D12CreateDevice failed: 0x%08lx\r\n", hr);
        D12_Append(st->output, buf);
        return;
    }

    {
        wchar_t line[200];
        swprintf_s(line, 200, L"D3D12 device created at feature level %s.\r\n",
                   D12_FeatureLevel(gotFL));
        D12_Append(st->output, line);
    }

    /* Descriptor handle sizes */
    {
        UINT s_cbv  = ID3D12Device_GetDescriptorHandleIncrementSize(st->device,
                        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        UINT s_smp  = ID3D12Device_GetDescriptorHandleIncrementSize(st->device,
                        D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        UINT s_rtv  = ID3D12Device_GetDescriptorHandleIncrementSize(st->device,
                        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        UINT s_dsv  = ID3D12Device_GetDescriptorHandleIncrementSize(st->device,
                        D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        wchar_t line[400];
        swprintf_s(line, 400,
            L"\r\n== Descriptor handle sizes ==\r\n"
            L"  CBV/SRV/UAV  : %u bytes\r\n"
            L"  SAMPLER      : %u bytes\r\n"
            L"  RTV          : %u bytes\r\n"
            L"  DSV          : %u bytes\r\n",
            s_cbv, s_smp, s_rtv, s_dsv);
        D12_Append(st->output, line);
    }

    /* D3D12_OPTIONS */
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS opts;
        ZeroMemory(&opts, sizeof(opts));
        if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(st->device,
                D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts)))) {
            wchar_t line[600];
            swprintf_s(line, 600,
                L"\r\n== D3D12_OPTIONS ==\r\n"
                L"  ResourceBindingTier   : %d\r\n"
                L"  TiledResourcesTier    : %d\r\n"
                L"  ConservativeRasterTier: %d\r\n"
                L"  CrossNodeSharingTier  : %d\r\n"
                L"  DoublePrecisionFloat  : %s\r\n"
                L"  OutputMergerLogicOp   : %s\r\n"
                L"  MinPrecisionSupport   : %d\r\n"
                L"  StandardSwizzle64KB   : %s\r\n",
                (int)opts.ResourceBindingTier,
                (int)opts.TiledResourcesTier,
                (int)opts.ConservativeRasterizationTier,
                (int)opts.CrossNodeSharingTier,
                opts.DoublePrecisionFloatShaderOps ? L"yes" : L"no",
                opts.OutputMergerLogicOp ? L"yes" : L"no",
                (int)opts.MinPrecisionSupport,
                opts.StandardSwizzle64KBSupported ? L"yes" : L"no");
            D12_Append(st->output, line);
        }
    }

    /* Command queue */
    {
        D3D12_COMMAND_QUEUE_DESC qd;
        ZeroMemory(&qd, sizeof(qd));
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        hr = ID3D12Device_CreateCommandQueue(st->device, &qd,
                &IID_ID3D12CommandQueue, (void **)&st->queue);
        if (SUCCEEDED(hr) && st->queue) {
            D12_Append(st->output, L"\r\nDirect command queue created.\r\n");
        } else {
            wchar_t buf[80];
            swprintf_s(buf, 80, L"\r\nCreateCommandQueue failed: 0x%08lx\r\n", hr);
            D12_Append(st->output, buf);
        }
    }
}

static LRESULT CALLBACK D12_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    D12State *st = (D12State *)GetPropW(hwnd, D12_PROP);
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 36, w - 16, h - 44, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->queue)  ID3D12CommandQueue_Release(st->queue);
        if (st->device) ID3D12Device_Release(st->device);
        if (st->d3d12)  FreeLibrary(st->d3d12);
        free(st); RemovePropW(hwnd, D12_PROP);
    }
    return CallWindowProcW(g_origD12Frame, hwnd, msg, wp, lp);
}

static HWND D3D12_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    D12State *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"D3D12",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (D12State *)calloc(1, sizeof(D12State));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 36, w - 16, h - 44, frame, (HMENU)(LONG_PTR)ID_D12_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, D12_PROP, (HANDLE)st);
    if (!g_origD12Frame) g_origD12Frame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)D12_FrameProc);
    D12_Init(st);
    return frame;
}

MsApp g_AppD3D12 = { L"D3D12", D3D12_Create, 720, 520 };
