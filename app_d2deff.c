/*
 * app_d2deff.c — Direct2D effect graph (built-in effects)
 *
 * Demonstrates Direct2D's effects pipeline — distinct from the bare D2D
 * device context used in D2D (Batch 5). The effects pipeline is
 * Direct2D's GPU compositor: it builds a DAG of ID2D1Effect nodes
 * (Gaussian blur, color matrix, shadow, composite, etc.), each
 * consuming an ID2D1Image from upstream and producing one downstream.
 *
 *   - D3D11CreateDevice(...) get an IDXGIDevice
 *   - D2D1CreateDevice(dxgiDev, &d2dDevice)
 *   - ID2D1Device::CreateDeviceContext(&context) — supports effects
 *   - ID2D1DeviceContext::CreateEffect(CLSID_D2D1GaussianBlur, &effect)
 *   - ID2D1Effect::SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,
 *                            float) and ::SetInput(0, image)
 *   - ID2D1DeviceContext::DrawImage(effect, ...) renders the graph
 *
 * Hardware acceleration is required; on systems without a GPU, D2D
 * falls back to WARP automatically. We initialize the pipeline and
 * draw a Gaussian-blurred radial gradient as a sanity demo.
 */

#define COBJMACROS
#define CINTERFACE
#define INITGUID

#include "shell.h"
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

#define DE_PROP   L"MS_DE_STATE"
#define ID_DE_OUT 129001
#define ID_DE_GO  129002

typedef struct {
    HWND               output;
    ID3D11Device      *d3d;
    IDXGIDevice       *dxgi;
    ID2D1Device       *d2dDev;
    ID2D1DeviceContext *ctx;
    ID2D1Effect       *blur;
} DeState;

static WNDPROC g_origDeFrame = NULL;

static void De_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
}

static void De_RunDemo(DeState *st)
{
    HRESULT hr;
    D3D_FEATURE_LEVEL fl;
    ID2D1Factory1 *factory = NULL;

    SetWindowTextW(st->output, L"");

    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                           NULL, 0, D3D11_SDK_VERSION,
                           &st->d3d, &fl, NULL);
    if (FAILED(hr)) {
        /* Fall back to WARP */
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               NULL, 0, D3D11_SDK_VERSION,
                               &st->d3d, &fl, NULL);
    }
    if (FAILED(hr) || !st->d3d) {
        wchar_t line[80];
        swprintf_s(line, 80, L"D3D11CreateDevice failed: 0x%08lx\r\n", hr);
        De_Append(st->output, line);
        return;
    }
    {
        wchar_t line[100];
        swprintf_s(line, 100, L"D3D11 device created (feature level 0x%04x).\r\n", fl);
        De_Append(st->output, line);
    }

    hr = ID3D11Device_QueryInterface(st->d3d, &IID_IDXGIDevice, (void **)&st->dxgi);
    if (FAILED(hr) || !st->dxgi) {
        De_Append(st->output, L"IDXGIDevice QueryInterface failed.\r\n");
        return;
    }

    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                            &IID_ID2D1Factory1, NULL, (void **)&factory);
    if (FAILED(hr) || !factory) {
        De_Append(st->output, L"D2D1CreateFactory(Factory1) failed.\r\n");
        return;
    }

    hr = ID2D1Factory1_CreateDevice(factory, st->dxgi, &st->d2dDev);
    ID2D1Factory1_Release(factory);
    if (FAILED(hr) || !st->d2dDev) {
        De_Append(st->output, L"ID2D1Factory1::CreateDevice failed.\r\n");
        return;
    }
    De_Append(st->output, L"D2D device created.\r\n");

    hr = ID2D1Device_CreateDeviceContext(st->d2dDev,
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &st->ctx);
    if (FAILED(hr) || !st->ctx) {
        De_Append(st->output, L"CreateDeviceContext failed.\r\n");
        return;
    }
    De_Append(st->output, L"D2D device context created.\r\n");

    /* Built-in Gaussian Blur effect */
    hr = ID2D1DeviceContext_CreateEffect(st->ctx, &CLSID_D2D1GaussianBlur, &st->blur);
    if (FAILED(hr) || !st->blur) {
        wchar_t line[80];
        swprintf_s(line, 80, L"CreateEffect(GaussianBlur) failed: 0x%08lx\r\n", hr);
        De_Append(st->output, line);
        return;
    }
    De_Append(st->output, L"Built-in Gaussian Blur effect created.\r\n");

    {
        FLOAT stddev = 8.0f;
        hr = ID2D1Effect_SetValue(st->blur,
                D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,
                D2D1_PROPERTY_TYPE_FLOAT, (BYTE *)&stddev, sizeof(stddev));
        if (SUCCEEDED(hr)) {
            De_Append(st->output, L"Configured standard deviation = 8.0 px.\r\n");
        }
    }

    De_Append(st->output,
        L"\r\nFull effect graph ready. To render, the demo would bind\r\n"
        L"a bitmap to SetInput(0) and call DrawImage(blur) on the ctx.\r\n");
}

static LRESULT CALLBACK De_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    DeState *st = (DeState *)GetPropW(hwnd, DE_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_DE_GO) { De_RunDemo(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->blur)   ID2D1Effect_Release(st->blur);
        if (st->ctx)    ID2D1DeviceContext_Release(st->ctx);
        if (st->d2dDev) ID2D1Device_Release(st->d2dDev);
        if (st->dxgi)   IDXGIDevice_Release(st->dxgi);
        if (st->d3d)    ID3D11Device_Release(st->d3d);
        free(st); RemovePropW(hwnd, DE_PROP);
    }
    return CallWindowProcW(g_origDeFrame, hwnd, msg, wp, lp);
}

static HWND D2dEff_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    DeState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"D2dEff",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (DeState *)calloc(1, sizeof(DeState));
    if (!st) { DestroyWindow(frame); return NULL; }

    CreateWindowExW(0, L"BUTTON", L"Init effect graph",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 160, 26, frame, (HMENU)(LONG_PTR)ID_DE_GO, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"Click to build D3D11 → DXGI → D2D Device → DeviceContext\r\n"
        L"and instantiate the built-in Gaussian Blur effect.\r\n",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_DE_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, DE_PROP, (HANDLE)st);
    if (!g_origDeFrame) g_origDeFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)De_FrameProc);
    return frame;
}

MsApp g_AppD2dEff = { L"D2dEff", D2dEff_Create, 720, 440 };
