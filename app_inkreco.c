/*
 * app_inkreco.c — Handwriting recognizer enumeration via Tablet PC API
 *
 * Demonstrates the Tablet PC ink-recognition COM surface (msinkaut.h,
 * inkobj.dll). The Tablet PC API exposes one or more installed
 * handwriting recognizers per OS language; each is an IInkRecognizer
 * that can convert digital ink into text.
 *
 *   - CoCreateInstance(CLSID_InkRecognizers, IID_IInkRecognizers, &col)
 *     returns the recognizer collection
 *   - IInkRecognizers::get_Count(&n)
 *   - IInkRecognizers::Item(i+1, &IInkRecognizer)  -- 1-based!
 *   - IInkRecognizer::get_Name(&BSTR) friendly name
 *   - IInkRecognizer::get_Languages(&VARIANT) returns SAFEARRAY of LCIDs
 *   - IInkRecognizer::get_Vendor(&BSTR)
 *
 * Recognizers are typically only present on locales where handwriting
 * recognition was installed (en-US, ja-JP, zh-CN have stock packs).
 */

#define COBJMACROS
#define CINTERFACE
#define INITGUID

#include "shell.h"
#include <msinkaut.h>
#include <commctrl.h>
#include <oleauto.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "comctl32.lib")

#define IR_PROP   L"MS_IR_STATE"
#define ID_IR_REF 127001
#define ID_IR_LV  127002

typedef struct { HWND refresh, list; } IrState;
static WNDPROC g_origIrFrame = NULL;

static void Ir_FormatLanguages(VARIANT *v, wchar_t *out, int cch)
{
    SAFEARRAY *sa;
    LONG lb = 0, ub = -1;
    LONG i;
    out[0] = 0;
    if (V_VT(v) != (VT_ARRAY | VT_I4) && V_VT(v) != (VT_ARRAY | VT_VARIANT)) return;
    sa = V_ARRAY(v);
    if (!sa) return;
    SafeArrayGetLBound(sa, 1, &lb);
    SafeArrayGetUBound(sa, 1, &ub);
    for (i = lb; i <= ub && i - lb < 12; ++i) {
        LCID lcid = 0;
        wchar_t name[80] = L"";
        if (V_VT(v) == (VT_ARRAY | VT_VARIANT)) {
            VARIANT cell;
            VariantInit(&cell);
            if (SUCCEEDED(SafeArrayGetElement(sa, &i, &cell))) {
                if (V_VT(&cell) == VT_I4) lcid = V_I4(&cell);
                VariantClear(&cell);
            }
        } else {
            SafeArrayGetElement(sa, &i, &lcid);
        }
        if (!lcid) continue;
        LCIDToLocaleName(lcid, name, 80, 0);
        if (out[0]) wcscat_s(out, cch, L", ");
        wcscat_s(out, cch, name[0] ? name : L"?");
    }
}

static void Ir_Refresh(IrState *st)
{
    IInkRecognizers *col = NULL;
    HRESULT hr;
    long count = 0;
    long i;

    SendMessageW(st->list, LVM_DELETEALLITEMS, 0, 0);

    hr = CoCreateInstance(&CLSID_InkRecognizers, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IInkRecognizers, (void **)&col);
    if (FAILED(hr) || !col) {
        LVITEMW it;
        wchar_t buf[140];
        swprintf_s(buf, 140,
            (hr == REGDB_E_CLASSNOTREG)
                ? L"(InkRecognizers COM class not registered — install handwriting features)"
                : L"(CoCreateInstance InkRecognizers: 0x%08lx)", hr);
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT; it.pszText = buf;
        SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        return;
    }

    IInkRecognizers_get_Count(col, &count);
    if (count == 0) {
        LVITEMW it;
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT;
        it.pszText = (LPWSTR)L"(0 recognizers installed)";
        SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        IInkRecognizers_Release(col);
        return;
    }

    for (i = 0; i < count; ++i) {
        IInkRecognizer *r = NULL;
        VARIANT idx;
        VariantInit(&idx);
        V_VT(&idx) = VT_I4;
        V_I4(&idx) = i + 1;
        if (FAILED(IInkRecognizers_Item(col, idx, &r)) || !r) continue;
        {
            BSTR name = NULL, vendor = NULL;
            VARIANT langs;
            wchar_t langStr[400] = L"";
            LVITEMW it;
            VariantInit(&langs);

            IInkRecognizer_get_Name(r, &name);
            IInkRecognizer_get_Vendor(r, &vendor);
            if (SUCCEEDED(IInkRecognizer_get_Languages(r, &langs))) {
                Ir_FormatLanguages(&langs, langStr, 400);
                VariantClear(&langs);
            }

            ZeroMemory(&it, sizeof(it));
            it.mask = LVIF_TEXT; it.iItem = (int)i;
            it.pszText = name ? name : (LPWSTR)L"(no name)";
            SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
            it.iSubItem = 1; it.pszText = vendor ? vendor : (LPWSTR)L"";
            SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
            it.iSubItem = 2; it.pszText = langStr;
            SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);

            if (name)   SysFreeString(name);
            if (vendor) SysFreeString(vendor);
        }
        IInkRecognizer_Release(r);
    }

    IInkRecognizers_Release(col);
}

static LRESULT CALLBACK Ir_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    IrState *st = (IrState *)GetPropW(hwnd, IR_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_IR_REF) { Ir_Refresh(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->refresh, 12, 38, 110, 26, TRUE);
        MoveWindow(st->list,    8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, IR_PROP); }
    return CallWindowProcW(g_origIrFrame, hwnd, msg, wp, lp);
}

static HWND InkReco_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    IrState *st;
    INITCOMMONCONTROLSEX icc;
    LVCOLUMNW col;
    (void)self;

    icc.dwSize = sizeof(icc); icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"InkReco",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (IrState *)calloc(1, sizeof(IrState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->refresh = CreateWindowExW(0, L"BUTTON", L"Refresh",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 110, 26, frame, (HMENU)(LONG_PTR)ID_IR_REF, hInstance, NULL);

    st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
        WS_CHILD | WS_VISIBLE | LVS_REPORT,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_IR_LV, hInstance, NULL);
    SendMessageW(st->list, LVM_SETEXTENDEDLISTVIEWSTYLE,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 240; col.pszText = (LPWSTR)L"Recognizer name"; SendMessageW(st->list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.cx = 220; col.pszText = (LPWSTR)L"Vendor";          SendMessageW(st->list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    col.cx = 380; col.pszText = (LPWSTR)L"Languages";       SendMessageW(st->list, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);

    SetPropW(frame, IR_PROP, (HANDLE)st);
    if (!g_origIrFrame) g_origIrFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Ir_FrameProc);
    Ir_Refresh(st);
    return frame;
}

MsApp g_AppInkReco = { L"InkReco", InkReco_Create, 920, 420 };
