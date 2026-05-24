/*
 * app_dcomp.c — DirectComposition device, visual tree, and commit
 *
 * Demonstrates Direct Composition (dcomp.dll) — the engine behind Win 8+
 * window animations, transforms, and effect layers. DComp lets us build a
 * GPU-accelerated visual tree of layered content offset/rotated/scaled in
 * real time, without having to repaint our window:
 *
 *   - DCompositionCreateDevice(D3D11/DXGI device or NULL, IID_IDCompositionDevice,
 *     &dcomp) — NULL works for surface-less inspection
 *   - IDCompositionDevice::CreateTargetForHwnd(hwnd, topMost, &target) binds
 *     a composition to a window
 *   - IDCompositionDevice::CreateVisual(&visual) — a node in the tree
 *   - IDCompositionVisual::SetContent / SetTransform / SetOffsetX/Y
 *   - IDCompositionTarget::SetRoot(visual) attaches a sub-tree
 *   - IDCompositionDevice::Commit() pushes the tree to DWM
 *
 * Loaded dynamically because the export is absent pre-Win 8. We create
 * the device, build an empty visual tree, and commit it — proving the
 * pipeline. (Surface creation requires D3D11 + DXGI plumbing already
 * covered in earlier batches.)
 */

#define COBJMACROS
#define CINTERFACE
#define INITGUID

#include "shell.h"
#include <dcomp.h>
#include <stdlib.h>
#include <stdio.h>

typedef HRESULT (WINAPI *PFN_DCompositionCreateDevice)(IUnknown *, REFIID, void **);

#define DC_PROP   L"MS_DC_STATE"
#define ID_DC_OUT 113001
#define ID_DC_GO  113002

typedef struct {
    HWND     output;
    HMODULE  dcompDll;
    PFN_DCompositionCreateDevice pCreate;
    IDCompositionDevice  *device;
    IDCompositionTarget  *target;
    IDCompositionVisual  *visual;
} DcState;

static WNDPROC g_origDcFrame = NULL;

static void Dc_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
}

static void Dc_Init(HWND frame, DcState *st)
{
    HRESULT hr;

    SetWindowTextW(st->output, L"");
    if (!st->pCreate) {
        Dc_Append(st->output, L"dcomp.dll not available (pre-Win 8?).\r\n");
        return;
    }

    hr = st->pCreate(NULL, &IID_IDCompositionDevice, (void **)&st->device);
    if (FAILED(hr) || !st->device) {
        wchar_t buf[80];
        swprintf_s(buf, 80, L"DCompositionCreateDevice failed: 0x%08lx\r\n", hr);
        Dc_Append(st->output, buf);
        return;
    }
    Dc_Append(st->output, L"DirectComposition device created.\r\n");

    hr = IDCompositionDevice_CreateTargetForHwnd(st->device, frame, TRUE, &st->target);
    if (FAILED(hr) || !st->target) {
        wchar_t buf[80];
        swprintf_s(buf, 80, L"CreateTargetForHwnd failed: 0x%08lx\r\n", hr);
        Dc_Append(st->output, buf);
        return;
    }
    Dc_Append(st->output, L"Composition target bound to this window.\r\n");

    hr = IDCompositionDevice_CreateVisual(st->device, &st->visual);
    if (FAILED(hr) || !st->visual) {
        wchar_t buf[80];
        swprintf_s(buf, 80, L"CreateVisual failed: 0x%08lx\r\n", hr);
        Dc_Append(st->output, buf);
        return;
    }
    Dc_Append(st->output, L"Root visual created.\r\n");

    /* Position offset */
    IDCompositionVisual_SetOffsetX(st->visual, 50.0f);
    IDCompositionVisual_SetOffsetY(st->visual, 50.0f);

    /* Attach */
    hr = IDCompositionTarget_SetRoot(st->target, st->visual);
    if (SUCCEEDED(hr)) Dc_Append(st->output, L"SetRoot succeeded.\r\n");

    /* Commit */
    hr = IDCompositionDevice_Commit(st->device);
    if (SUCCEEDED(hr)) {
        Dc_Append(st->output, L"Commit succeeded — DWM has the tree.\r\n\r\n");
        Dc_Append(st->output,
            L"(Visual has no content surface attached; full content would\r\n"
            L"require a D3D11 device + DXGI surface — see Batch 7 D3D11\r\n"
            L"and Batch 14 DXGIVbl for that plumbing.)\r\n");
    } else {
        wchar_t buf[80];
        swprintf_s(buf, 80, L"Commit failed: 0x%08lx\r\n", hr);
        Dc_Append(st->output, buf);
    }
}

static LRESULT CALLBACK Dc_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    DcState *st = (DcState *)GetPropW(hwnd, DC_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_DC_GO) { Dc_Init(hwnd, st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->visual)   IDCompositionVisual_Release(st->visual);
        if (st->target)   IDCompositionTarget_Release(st->target);
        if (st->device)   IDCompositionDevice_Release(st->device);
        if (st->dcompDll) FreeLibrary(st->dcompDll);
        free(st); RemovePropW(hwnd, DC_PROP);
    }
    return CallWindowProcW(g_origDcFrame, hwnd, msg, wp, lp);
}

static HWND DComp_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    DcState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"DComp",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (DcState *)calloc(1, sizeof(DcState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->dcompDll = LoadLibraryW(L"dcomp.dll");
    if (st->dcompDll) {
        st->pCreate = (PFN_DCompositionCreateDevice)
            GetProcAddress(st->dcompDll, "DCompositionCreateDevice");
    }

    CreateWindowExW(0, L"BUTTON", L"Build visual tree",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 160, 26, frame, (HMENU)(LONG_PTR)ID_DC_GO, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"Click to create a DirectComposition device, target, visual,\r\n"
        L"set offset, and commit. Demonstrates the dcomp pipeline.\r\n",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_DC_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, DC_PROP, (HANDLE)st);
    if (!g_origDcFrame) g_origDcFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Dc_FrameProc);
    return frame;
}

MsApp g_AppDComp = { L"DComp", DComp_Create, 700, 380 };
