/*
 * app_d3d11.c — Direct3D 11 rotating triangle
 *
 * Demonstrates the modern 3D pipeline on Windows from plain C:
 *   - D3D11CreateDeviceAndSwapChain for a swap chain bound to our HWND
 *   - ID3D11RenderTargetView wrapping the back buffer
 *   - Vertex buffer (ID3D11Buffer with D3D11_BIND_VERTEX_BUFFER)
 *   - HLSL shaders compiled at runtime via D3DCompile from d3dcompiler.dll
 *     (dynamic-load so we don't hard-depend on a specific compiler DLL name
 *     existing at link time)
 *   - Input layout, vertex/pixel shader, draw call, present
 *
 * The triangle rotates via a constant buffer updated each frame.
 *
 * If D3D11 or d3dcompiler aren't available, the window paints a fallback
 * message instead of crashing.
 */

#define COBJMACROS
#define CINTERFACE

#include "shell.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdlib.h>

#pragma comment(lib, "d3d11.lib")
/* d3dcompiler is loaded dynamically — see EnsureCompiler */

#define D3D_CLASS L"MiniShell_D3D11Canvas"
#define D3D_TIMER 1

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef HRESULT (WINAPI *D3DCompileFn)(LPCVOID, SIZE_T, LPCSTR,
    const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR,
    UINT, UINT, ID3DBlob **, ID3DBlob **);

typedef struct {
    ID3D11Device           *device;
    ID3D11DeviceContext    *ctx;
    IDXGISwapChain         *swap;
    ID3D11RenderTargetView *rtv;
    ID3D11Buffer           *vbuf;
    ID3D11Buffer           *cbuf;
    ID3D11VertexShader     *vs;
    ID3D11PixelShader      *ps;
    ID3D11InputLayout      *layout;
    HMODULE                 compilerDll;
    float                   angle;
    BOOL                    initOk;
} D3dState;

typedef struct { float x, y, z; float r, g, b; } D3dVertex;
typedef struct { float matrix[16]; } D3dCBuffer;

/* Inline HLSL: pos in object space + rotation around Z */
static const char *kShaderHLSL =
"cbuffer CB : register(b0) { float4x4 mat; };\n"
"struct VIn { float3 pos : POS; float3 col : COL; };\n"
"struct VOut { float4 pos : SV_POSITION; float3 col : COLOR; };\n"
"VOut vs_main(VIn i) {\n"
"  VOut o;\n"
"  o.pos = mul(float4(i.pos,1.0), mat);\n"
"  o.col = i.col;\n"
"  return o;\n"
"}\n"
"float4 ps_main(VOut i) : SV_TARGET { return float4(i.col, 1.0); }\n";

static D3DCompileFn LoadCompiler(D3dState *st)
{
    /* Try a few versioned names; d3dcompiler_47 ships with modern Windows */
    static const wchar_t *names[] = {
        L"d3dcompiler_47.dll", L"d3dcompiler_46.dll", L"d3dcompiler.dll", NULL
    };
    int i;
    for (i = 0; names[i]; ++i) {
        st->compilerDll = LoadLibraryW(names[i]);
        if (st->compilerDll) {
            FARPROC p = GetProcAddress(st->compilerDll, "D3DCompile");
            if (p) return (D3DCompileFn)p;
            FreeLibrary(st->compilerDll);
            st->compilerDll = NULL;
        }
    }
    return NULL;
}

static BOOL D3d_InitDevice(HWND hwnd, D3dState *st)
{
    DXGI_SWAP_CHAIN_DESC sd;
    D3D_FEATURE_LEVEL got;
    HRESULT hr;
    ID3D11Texture2D *back = NULL;
    D3DCompileFn compile;
    ID3DBlob *vsBlob = NULL, *psBlob = NULL, *err = NULL;
    D3D11_BUFFER_DESC vbd, cbd;
    D3D11_SUBRESOURCE_DATA vinit;
    D3D11_INPUT_ELEMENT_DESC elements[2];
    RECT rc;
    static const D3D_FEATURE_LEVEL fls[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
    };

    GetClientRect(hwnd, &rc);
    if (rc.right == 0 || rc.bottom == 0) return FALSE;

    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount        = 1;
    sd.BufferDesc.Width   = rc.right;
    sd.BufferDesc.Height  = rc.bottom;
    sd.BufferDesc.Format  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow       = hwnd;
    sd.SampleDesc.Count   = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed           = TRUE;
    sd.SwapEffect         = DXGI_SWAP_EFFECT_DISCARD;

    hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
            0, fls, ARRAYSIZE(fls), D3D11_SDK_VERSION,
            &sd, &st->swap, &st->device, &got, &st->ctx);
    if (FAILED(hr)) {
        /* Try WARP software fallback */
        hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_WARP, NULL,
                0, fls, ARRAYSIZE(fls), D3D11_SDK_VERSION,
                &sd, &st->swap, &st->device, &got, &st->ctx);
        if (FAILED(hr)) return FALSE;
    }

    hr = IDXGISwapChain_GetBuffer(st->swap, 0, &IID_ID3D11Texture2D, (void **)&back);
    if (FAILED(hr)) return FALSE;
    ID3D11Device_CreateRenderTargetView(st->device, (ID3D11Resource *)back, NULL, &st->rtv);
    ID3D11Texture2D_Release(back);

    compile = LoadCompiler(st);
    if (!compile) return FALSE;

    hr = compile(kShaderHLSL, strlen(kShaderHLSL), NULL, NULL, NULL,
                 "vs_main", "vs_4_0", 0, 0, &vsBlob, &err);
    if (FAILED(hr)) { if (err) ID3D10Blob_Release(err); return FALSE; }
    hr = compile(kShaderHLSL, strlen(kShaderHLSL), NULL, NULL, NULL,
                 "ps_main", "ps_4_0", 0, 0, &psBlob, &err);
    if (FAILED(hr)) {
        if (err) ID3D10Blob_Release(err);
        ID3D10Blob_Release(vsBlob);
        return FALSE;
    }

    ID3D11Device_CreateVertexShader(st->device,
        ID3D10Blob_GetBufferPointer(vsBlob),
        ID3D10Blob_GetBufferSize(vsBlob), NULL, &st->vs);
    ID3D11Device_CreatePixelShader(st->device,
        ID3D10Blob_GetBufferPointer(psBlob),
        ID3D10Blob_GetBufferSize(psBlob), NULL, &st->ps);

    elements[0].SemanticName = "POS"; elements[0].SemanticIndex = 0;
    elements[0].Format = DXGI_FORMAT_R32G32B32_FLOAT;
    elements[0].InputSlot = 0; elements[0].AlignedByteOffset = 0;
    elements[0].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
    elements[0].InstanceDataStepRate = 0;
    elements[1].SemanticName = "COL"; elements[1].SemanticIndex = 0;
    elements[1].Format = DXGI_FORMAT_R32G32B32_FLOAT;
    elements[1].InputSlot = 0; elements[1].AlignedByteOffset = 12;
    elements[1].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
    elements[1].InstanceDataStepRate = 0;
    ID3D11Device_CreateInputLayout(st->device, elements, 2,
        ID3D10Blob_GetBufferPointer(vsBlob),
        ID3D10Blob_GetBufferSize(vsBlob), &st->layout);

    ID3D10Blob_Release(vsBlob);
    ID3D10Blob_Release(psBlob);

    {
        static const D3dVertex verts[3] = {
            {  0.0f,  0.6f, 0.0f,   1.0f, 0.2f, 0.3f },
            {  0.6f, -0.5f, 0.0f,   0.3f, 0.9f, 0.4f },
            { -0.6f, -0.5f, 0.0f,   0.3f, 0.5f, 1.0f },
        };
        ZeroMemory(&vbd, sizeof(vbd));
        vbd.Usage = D3D11_USAGE_DEFAULT;
        vbd.ByteWidth = sizeof(verts);
        vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        ZeroMemory(&vinit, sizeof(vinit));
        vinit.pSysMem = verts;
        ID3D11Device_CreateBuffer(st->device, &vbd, &vinit, &st->vbuf);
    }
    ZeroMemory(&cbd, sizeof(cbd));
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.ByteWidth = sizeof(D3dCBuffer);
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(st->device, &cbd, NULL, &st->cbuf);

    st->initOk = TRUE;
    return TRUE;
}

static void D3d_Render(D3dState *st, int w, int h)
{
    float clear[4] = { 0.07f, 0.09f, 0.16f, 1.0f };
    UINT stride = sizeof(D3dVertex);
    UINT offset = 0;
    D3D11_VIEWPORT vp;
    D3D11_MAPPED_SUBRESOURCE map;

    if (!st->initOk) return;

    /* Update rotation matrix */
    if (SUCCEEDED(ID3D11DeviceContext_Map(st->ctx, (ID3D11Resource *)st->cbuf,
                                          0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        D3dCBuffer *cb = (D3dCBuffer *)map.pData;
        float c = cosf(st->angle), s = sinf(st->angle);
        /* Column-major; default cbuffer packing in HLSL */
        cb->matrix[0]  =  c; cb->matrix[1]  =  s; cb->matrix[2]  = 0; cb->matrix[3]  = 0;
        cb->matrix[4]  = -s; cb->matrix[5]  =  c; cb->matrix[6]  = 0; cb->matrix[7]  = 0;
        cb->matrix[8]  =  0; cb->matrix[9]  =  0; cb->matrix[10] = 1; cb->matrix[11] = 0;
        cb->matrix[12] =  0; cb->matrix[13] =  0; cb->matrix[14] = 0; cb->matrix[15] = 1;
        ID3D11DeviceContext_Unmap(st->ctx, (ID3D11Resource *)st->cbuf, 0);
    }

    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = (FLOAT)w; vp.Height = (FLOAT)h;
    vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(st->ctx, 1, &vp);
    ID3D11DeviceContext_OMSetRenderTargets(st->ctx, 1, &st->rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(st->ctx, st->rtv, clear);

    ID3D11DeviceContext_IASetInputLayout(st->ctx, st->layout);
    ID3D11DeviceContext_IASetVertexBuffers(st->ctx, 0, 1, &st->vbuf, &stride, &offset);
    ID3D11DeviceContext_IASetPrimitiveTopology(st->ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(st->ctx, st->vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(st->ctx, 0, 1, &st->cbuf);
    ID3D11DeviceContext_PSSetShader(st->ctx, st->ps, NULL, 0);
    ID3D11DeviceContext_Draw(st->ctx, 3, 0);
    IDXGISwapChain_Present(st->swap, 1, 0);
}

static LRESULT CALLBACK D3d_CanvasProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    D3dState *st = (D3dState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE:
        st = (D3dState *)calloc(1, sizeof(D3dState));
        if (!st) return -1;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        D3d_InitDevice(hwnd, st);
        SetTimer(hwnd, D3D_TIMER, 16, NULL);
        return 0;

    case WM_TIMER: {
        RECT rc;
        if (!st || !st->initOk) {
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        st->angle += 0.02f;
        if (st->angle > (float)(2 * M_PI)) st->angle -= (float)(2 * M_PI);
        GetClientRect(hwnd, &rc);
        D3d_Render(st, rc.right, rc.bottom);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (!st || !st->initOk) {
            RECT rc;
            HBRUSH bg = CreateSolidBrush(RGB(40, 30, 30));
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 220, 200));
            DrawTextW(hdc,
                L"Direct3D 11 init failed.\n"
                L"d3d11.dll or d3dcompiler_47.dll may be unavailable.",
                -1, &rc, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SIZE:
        if (st && st->initOk && st->swap) {
            UINT w = LOWORD(lp), h = HIWORD(lp);
            if (w > 0 && h > 0) {
                ID3D11Texture2D *back;
                if (st->rtv) { ID3D11RenderTargetView_Release(st->rtv); st->rtv = NULL; }
                ID3D11DeviceContext_OMSetRenderTargets(st->ctx, 0, NULL, NULL);
                IDXGISwapChain_ResizeBuffers(st->swap, 0, w, h,
                                             DXGI_FORMAT_UNKNOWN, 0);
                if (SUCCEEDED(IDXGISwapChain_GetBuffer(st->swap, 0,
                        &IID_ID3D11Texture2D, (void **)&back))) {
                    ID3D11Device_CreateRenderTargetView(st->device,
                        (ID3D11Resource *)back, NULL, &st->rtv);
                    ID3D11Texture2D_Release(back);
                }
            }
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, D3D_TIMER);
        if (st) {
            if (st->layout) ID3D11InputLayout_Release(st->layout);
            if (st->vbuf)   ID3D11Buffer_Release(st->vbuf);
            if (st->cbuf)   ID3D11Buffer_Release(st->cbuf);
            if (st->vs)     ID3D11VertexShader_Release(st->vs);
            if (st->ps)     ID3D11PixelShader_Release(st->ps);
            if (st->rtv)    ID3D11RenderTargetView_Release(st->rtv);
            if (st->swap)   IDXGISwapChain_Release(st->swap);
            if (st->ctx)    ID3D11DeviceContext_Release(st->ctx);
            if (st->device) ID3D11Device_Release(st->device);
            if (st->compilerDll) FreeLibrary(st->compilerDll);
            free(st);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void EnsureD3dClass(HINSTANCE hInstance)
{
    static BOOL registered = FALSE;
    WNDCLASSEXW wc;
    if (registered) return;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = D3d_CanvasProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = D3D_CLASS;
    RegisterClassExW(&wc);
    registered = TRUE;
}

static WNDPROC g_origD3dFrame = NULL;

static LRESULT CALLBACK D3d_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_SIZE) {
        HWND canvas = FindWindowExW(hwnd, NULL, D3D_CLASS, NULL);
        if (canvas) {
            int w = LOWORD(lp), h = HIWORD(lp);
            MoveWindow(canvas, 4, 32, w - 8, h - 36, TRUE);
        }
    }
    return CallWindowProcW(g_origD3dFrame, hwnd, msg, wp, lp);
}

static HWND D3D11_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    (void)self;

    EnsureD3dClass(hInstance);
    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"D3D11",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    CreateWindowExW(0, D3D_CLASS, L"",
        WS_CHILD | WS_VISIBLE,
        4, 32, w - 8, h - 36, frame, NULL, hInstance, NULL);

    if (!g_origD3dFrame) g_origD3dFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)D3d_FrameProc);
    return frame;
}

MsApp g_AppD3D11 = {
    L"D3D11",
    D3D11_Create,
    480, 380
};
