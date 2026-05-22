/*
 * app_dragsrc.c — OLE drag-and-drop source (outbound)
 *
 * Complements existing drop targets (Hasher, HexView) which only accept
 * drags. This demonstrates BEING a drag source: when you press and drag in
 * the source area, the app initiates an OLE drag operation carrying text.
 * Drop onto Notepad, an Explorer rename, a browser address bar, etc.
 *
 * Required objects (all manually implemented as static vtables since IDropSource
 * and IDataObject have no off-the-shelf factory):
 *   - IDropSource: QueryContinueDrag / GiveFeedback
 *   - IDataObject: GetData(CF_UNICODETEXT) returning an HGLOBAL wide string
 *   - DoDragDrop(pDataObj, pDropSrc, DROPEFFECT_COPY, &effect)
 *
 * Plain C with CINTERFACE: each object is a pair (vtable + instance) and
 * QueryInterface is the minimal IUnknown set.
 */

#define COBJMACROS
#define CINTERFACE
#define INITGUID

#include "shell.h"
#include <ole2.h>
#include <objbase.h>
#include <oleidl.h>
#include <shellapi.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ole32.lib")

#define DS_PROP    L"MS_DS_STATE"
#define ID_DS_TEXT 58001
#define ID_DS_AREA 58002
#define ID_DS_STAT 58003

typedef struct {
    HWND     editText, dragArea, status;
} DsState;

static WNDPROC g_origDsFrame  = NULL;
static WNDPROC g_origDsArea   = NULL;

/* ---------- IDropSource implementation ---------- */
typedef struct {
    IDropSourceVtbl *vtbl;
    LONG refCount;
} MyDropSrc;

static HRESULT STDMETHODCALLTYPE Ds_DS_QI(IDropSource *this_, REFIID iid, void **out)
{
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IDropSource)) {
        *out = this_;
        IDropSource_AddRef(this_);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE Ds_DS_AddRef(IDropSource *this_)
{
    return InterlockedIncrement(&((MyDropSrc *)this_)->refCount);
}
static ULONG STDMETHODCALLTYPE Ds_DS_Release(IDropSource *this_)
{
    LONG c = InterlockedDecrement(&((MyDropSrc *)this_)->refCount);
    if (c == 0) { free(this_); }
    return c;
}
static HRESULT STDMETHODCALLTYPE Ds_DS_QueryContinueDrag(IDropSource *this_,
        BOOL escapePressed, DWORD keyState)
{
    (void)this_;
    if (escapePressed) return DRAGDROP_S_CANCEL;
    if ((keyState & MK_LBUTTON) == 0) return DRAGDROP_S_DROP;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE Ds_DS_GiveFeedback(IDropSource *this_, DWORD effect)
{
    (void)this_; (void)effect;
    return DRAGDROP_S_USEDEFAULTCURSORS;
}
static IDropSourceVtbl g_dsVtbl = {
    Ds_DS_QI, Ds_DS_AddRef, Ds_DS_Release,
    Ds_DS_QueryContinueDrag, Ds_DS_GiveFeedback
};

/* ---------- IDataObject implementation ---------- */
typedef struct {
    IDataObjectVtbl *vtbl;
    LONG     refCount;
    wchar_t *payload;
} MyDataObj;

static HRESULT STDMETHODCALLTYPE Ds_DO_QI(IDataObject *this_, REFIID iid, void **out)
{
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IDataObject)) {
        *out = this_;
        IDataObject_AddRef(this_);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE Ds_DO_AddRef(IDataObject *this_)
{
    return InterlockedIncrement(&((MyDataObj *)this_)->refCount);
}
static ULONG STDMETHODCALLTYPE Ds_DO_Release(IDataObject *this_)
{
    LONG c = InterlockedDecrement(&((MyDataObj *)this_)->refCount);
    if (c == 0) {
        free(((MyDataObj *)this_)->payload);
        free(this_);
    }
    return c;
}
static HRESULT STDMETHODCALLTYPE Ds_DO_GetData(IDataObject *this_,
        FORMATETC *fmt, STGMEDIUM *med)
{
    MyDataObj *me = (MyDataObj *)this_;
    SIZE_T cb;
    HGLOBAL h;
    void *p;

    if (fmt->cfFormat != CF_UNICODETEXT || !(fmt->tymed & TYMED_HGLOBAL))
        return DV_E_FORMATETC;

    cb = (wcslen(me->payload) + 1) * sizeof(wchar_t);
    h  = GlobalAlloc(GHND, cb);
    if (!h) return STG_E_MEDIUMFULL;
    p = GlobalLock(h);
    memcpy(p, me->payload, cb);
    GlobalUnlock(h);

    med->tymed = TYMED_HGLOBAL;
    med->hGlobal = h;
    med->pUnkForRelease = NULL;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE Ds_DO_GetDataHere(IDataObject *this_,
        FORMATETC *f, STGMEDIUM *m) { (void)this_; (void)f; (void)m; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE Ds_DO_QueryGetData(IDataObject *this_, FORMATETC *fmt)
{
    (void)this_;
    if (fmt->cfFormat == CF_UNICODETEXT && (fmt->tymed & TYMED_HGLOBAL))
        return S_OK;
    return S_FALSE;
}
static HRESULT STDMETHODCALLTYPE Ds_DO_GetCanonicalFormatEtc(IDataObject *this_,
        FORMATETC *in, FORMATETC *out) { (void)this_; (void)in; out->ptd = NULL; return DATA_S_SAMEFORMATETC; }
static HRESULT STDMETHODCALLTYPE Ds_DO_SetData(IDataObject *this_, FORMATETC *f,
        STGMEDIUM *m, BOOL release) { (void)this_; (void)f; (void)m; (void)release; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE Ds_DO_EnumFormatEtc(IDataObject *this_, DWORD dir,
        IEnumFORMATETC **out)
{
    /* OLE provides a helper to create an enumerator from a FORMATETC array */
    FORMATETC f = { CF_UNICODETEXT, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    (void)this_;
    if (dir == DATADIR_GET) {
        return SHCreateStdEnumFmtEtc(1, &f, out);
    }
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE Ds_DO_DAdvise(IDataObject *this_, FORMATETC *f,
        DWORD adv, IAdviseSink *s, DWORD *conn)
{ (void)this_; (void)f; (void)adv; (void)s; (void)conn; return OLE_E_ADVISENOTSUPPORTED; }
static HRESULT STDMETHODCALLTYPE Ds_DO_DUnadvise(IDataObject *this_, DWORD conn)
{ (void)this_; (void)conn; return OLE_E_ADVISENOTSUPPORTED; }
static HRESULT STDMETHODCALLTYPE Ds_DO_EnumDAdvise(IDataObject *this_, IEnumSTATDATA **e)
{ (void)this_; (void)e; return OLE_E_ADVISENOTSUPPORTED; }

static IDataObjectVtbl g_doVtbl = {
    Ds_DO_QI, Ds_DO_AddRef, Ds_DO_Release,
    Ds_DO_GetData, Ds_DO_GetDataHere, Ds_DO_QueryGetData,
    Ds_DO_GetCanonicalFormatEtc, Ds_DO_SetData, Ds_DO_EnumFormatEtc,
    Ds_DO_DAdvise, Ds_DO_DUnadvise, Ds_DO_EnumDAdvise
};

/* ---------- Drag area subclass ---------- */

static LRESULT CALLBACK Ds_AreaProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_LBUTTONDOWN) {
        DsState *st = (DsState *)GetPropW(GetParent(hwnd), DS_PROP);
        if (st) {
            int len = GetWindowTextLengthW(st->editText);
            MyDataObj *dat = (MyDataObj *)malloc(sizeof(MyDataObj));
            MyDropSrc *src = (MyDropSrc *)malloc(sizeof(MyDropSrc));
            DWORD effect = 0;
            HRESULT hr;

            if (dat && src) {
                dat->vtbl = &g_doVtbl;
                dat->refCount = 1;
                dat->payload = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
                GetWindowTextW(st->editText, dat->payload, len + 1);

                src->vtbl = &g_dsVtbl;
                src->refCount = 1;

                hr = DoDragDrop((IDataObject *)dat, (IDropSource *)src,
                                DROPEFFECT_COPY, &effect);

                IDataObject_Release((IDataObject *)dat);
                IDropSource_Release((IDropSource *)src);

                if (hr == DRAGDROP_S_DROP)
                    SetWindowTextW(st->status,
                        effect == DROPEFFECT_COPY ? L"Dropped (copy)." : L"Dropped.");
                else if (hr == DRAGDROP_S_CANCEL)
                    SetWindowTextW(st->status, L"Cancelled.");
                else
                    SetWindowTextW(st->status, L"DoDragDrop returned an error.");
            }
        }
        return 0;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(200, 220, 245));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(30, 50, 90));
        DrawTextW(hdc, L"Click and drag to start a copy",
                  -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return CallWindowProcW(g_origDsArea, hwnd, msg, wp, lp);
}

static LRESULT CALLBACK Ds_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    DsState *st = (DsState *)GetPropW(hwnd, DS_PROP);
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->editText, 12, 60, w - 24, 60, TRUE);
        MoveWindow(st->dragArea, 12, 132, w - 24, h - 168, TRUE);
        MoveWindow(st->status, 12, h - 28, w - 24, 22, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        OleUninitialize();
        free(st);
        RemovePropW(hwnd, DS_PROP);
    }
    return CallWindowProcW(g_origDsFrame, hwnd, msg, wp, lp);
}

static HWND DragSrc_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    DsState *st;
    (void)self;

    OleInitialize(NULL);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"DragSrc",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (DsState *)calloc(1, sizeof(DsState));
    if (!st) { DestroyWindow(frame); return NULL; }

    CreateWindowExW(0, L"STATIC", L"Edit the text below, then drag the blue area to another app:",
        WS_CHILD | WS_VISIBLE,
        12, 36, w - 24, 22, frame, NULL, hInstance, NULL);

    st->editText = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"Greetings from MiniShell's DragSrc demo!",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL,
        12, 60, w - 24, 60, frame, (HMENU)(LONG_PTR)ID_DS_TEXT, hInstance, NULL);

    st->dragArea = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_NOTIFY,
        12, 132, w - 24, h - 168, frame, (HMENU)(LONG_PTR)ID_DS_AREA, hInstance, NULL);
    g_origDsArea = (WNDPROC)GetWindowLongPtrW(st->dragArea, GWLP_WNDPROC);
    SetWindowLongPtrW(st->dragArea, GWLP_WNDPROC, (LONG_PTR)Ds_AreaProc);

    st->status = CreateWindowExW(0, L"STATIC", L"Ready.",
        WS_CHILD | WS_VISIBLE,
        12, h - 28, w - 24, 22, frame, (HMENU)(LONG_PTR)ID_DS_STAT, hInstance, NULL);

    SetPropW(frame, DS_PROP, (HANDLE)st);
    if (!g_origDsFrame) g_origDsFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Ds_FrameProc);
    return frame;
}

MsApp g_AppDragSrc = {
    L"DragSrc",
    DragSrc_Create,
    480, 320
};
