/*
 * app_pastetgt.c — OLE drop *target* (the complement to DragSrc in Batch 9)
 *
 * Demonstrates registering a window as a drop target:
 *   - OleInitialize  (drop-target requires full OLE init, not just CoInit)
 *   - RegisterDragDrop(hwnd, &dropTarget)
 *   - Custom IDropTarget vtable with DragEnter / DragOver / DragLeave / Drop
 *   - In Drop, query IDataObject for CF_HDROP (file paths) and CF_UNICODETEXT
 *
 * This is the *consumer* side of OLE drag-and-drop. Together with DragSrc,
 * MiniShell can produce and consume drags. Hand-written vtables in C —
 * no ATL, no smart pointers, just function-pointer tables.
 */

#define COBJMACROS
#define CINTERFACE

#include "shell.h"
#include <ole2.h>
#include <shlobj.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

#define PT_PROP    L"MS_PT_STATE"
#define ID_PT_OUT  86001

typedef struct {
    IDropTargetVtbl *vtbl;   /* must be first */
    LONG  ref;
    HWND  output;
} PtDropTarget;

typedef struct {
    HWND          output;
    PtDropTarget *target;
    BOOL          oleOk, registered;
} PtState;

static WNDPROC g_origPtFrame = NULL;

static void Pt_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
    SendMessageW(e, EM_SCROLLCARET, 0, 0);
}

/* --- IDropTarget vtable in C ---------------------------------------- */

static HRESULT STDMETHODCALLTYPE Pt_QI(IDropTarget *p, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDropTarget)) {
        *out = p;
        p->lpVtbl->AddRef(p);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE Pt_AddRef(IDropTarget *p)
{
    PtDropTarget *self = (PtDropTarget *)p;
    return (ULONG)InterlockedIncrement(&self->ref);
}

static ULONG STDMETHODCALLTYPE Pt_Release(IDropTarget *p)
{
    PtDropTarget *self = (PtDropTarget *)p;
    LONG n = InterlockedDecrement(&self->ref);
    if (n == 0) { /* freed by owner */ }
    return (ULONG)n;
}

static HRESULT STDMETHODCALLTYPE Pt_DragEnter(IDropTarget *p, IDataObject *dataObj,
                                               DWORD keyState, POINTL pt, DWORD *effect)
{
    PtDropTarget *self = (PtDropTarget *)p;
    (void)dataObj; (void)keyState; (void)pt;
    *effect = DROPEFFECT_COPY;
    Pt_Append(self->output, L"DragEnter\r\n");
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Pt_DragOver(IDropTarget *p, DWORD keyState,
                                              POINTL pt, DWORD *effect)
{
    (void)p; (void)keyState; (void)pt;
    *effect = DROPEFFECT_COPY;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Pt_DragLeave(IDropTarget *p)
{
    PtDropTarget *self = (PtDropTarget *)p;
    Pt_Append(self->output, L"DragLeave\r\n");
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Pt_Drop(IDropTarget *p, IDataObject *dataObj,
                                          DWORD keyState, POINTL pt, DWORD *effect)
{
    PtDropTarget *self = (PtDropTarget *)p;
    FORMATETC fmt;
    STGMEDIUM medium;
    (void)keyState; (void)pt;

    Pt_Append(self->output, L"Drop! Inspecting IDataObject:\r\n");

    /* Try CF_HDROP (file paths) */
    fmt.cfFormat = CF_HDROP;
    fmt.ptd = NULL;
    fmt.dwAspect = DVASPECT_CONTENT;
    fmt.lindex = -1;
    fmt.tymed = TYMED_HGLOBAL;
    if (SUCCEEDED(IDataObject_GetData(dataObj, &fmt, &medium))) {
        HDROP hdrop = (HDROP)medium.hGlobal;
        UINT count = DragQueryFileW(hdrop, 0xFFFFFFFF, NULL, 0);
        UINT i;
        wchar_t header[80];
        swprintf_s(header, 80, L"  CF_HDROP: %u file(s)\r\n", count);
        Pt_Append(self->output, header);
        for (i = 0; i < count && i < 20; ++i) {
            wchar_t path[MAX_PATH];
            DragQueryFileW(hdrop, i, path, MAX_PATH);
            {
                wchar_t line[MAX_PATH + 20];
                swprintf_s(line, MAX_PATH + 20, L"    %s\r\n", path);
                Pt_Append(self->output, line);
            }
        }
        ReleaseStgMedium(&medium);
    }

    /* Try CF_UNICODETEXT */
    fmt.cfFormat = CF_UNICODETEXT;
    if (SUCCEEDED(IDataObject_GetData(dataObj, &fmt, &medium))) {
        wchar_t *text = (wchar_t *)GlobalLock(medium.hGlobal);
        if (text) {
            wchar_t snip[200];
            int n = (int)wcsnlen_s(text, 100);
            wcsncpy_s(snip, 200, text, n);
            snip[n] = 0;
            {
                wchar_t line[300];
                swprintf_s(line, 300, L"  CF_UNICODETEXT: \"%s\"...\r\n", snip);
                Pt_Append(self->output, line);
            }
            GlobalUnlock(medium.hGlobal);
        }
        ReleaseStgMedium(&medium);
    }

    *effect = DROPEFFECT_COPY;
    return S_OK;
}

static IDropTargetVtbl g_PtVtbl = {
    Pt_QI, Pt_AddRef, Pt_Release,
    Pt_DragEnter, Pt_DragOver, Pt_DragLeave, Pt_Drop
};

/* --- frame ----------------------------------------------------------- */

static LRESULT CALLBACK Pt_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    PtState *st = (PtState *)GetPropW(hwnd, PT_PROP);
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 60, w - 16, h - 68, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->registered) RevokeDragDrop(hwnd);
        if (st->target) free(st->target);
        if (st->oleOk) OleUninitialize();
        free(st); RemovePropW(hwnd, PT_PROP);
    }
    return CallWindowProcW(g_origPtFrame, hwnd, msg, wp, lp);
}

static HWND PasteTgt_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    PtState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_ACCEPTFILES,
        MS_CLASS_APPFRAME, L"PasteTgt",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (PtState *)calloc(1, sizeof(PtState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->oleOk = SUCCEEDED(OleInitialize(NULL));

    st->target = (PtDropTarget *)calloc(1, sizeof(PtDropTarget));
    st->target->vtbl = &g_PtVtbl;
    st->target->ref = 1;

    CreateWindowExW(0, L"STATIC",
        L"Drag files or text from Explorer/another app into the window below.\n"
        L"This is an OLE drop target — distinct from WM_DROPFILES.",
        WS_CHILD | WS_VISIBLE,
        12, 30, w - 24, 26, frame, NULL, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"(Drop something here)\r\n",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 60, w - 16, h - 68, frame, (HMENU)(LONG_PTR)ID_PT_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);
    st->target->output = st->output;

    if (st->oleOk) {
        if (SUCCEEDED(RegisterDragDrop(frame, (IDropTarget *)st->target))) {
            st->registered = TRUE;
        } else {
            Pt_Append(st->output, L"RegisterDragDrop failed.\r\n");
        }
    }

    SetPropW(frame, PT_PROP, (HANDLE)st);
    if (!g_origPtFrame) g_origPtFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Pt_FrameProc);
    return frame;
}

MsApp g_AppPasteTgt = { L"PasteTgt", PasteTgt_Create, 580, 380 };
