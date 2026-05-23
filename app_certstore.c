/*
 * app_certstore.c — Enumerate certificates from a Windows certificate store
 *
 * Demonstrates CryptoAPI cert-store enumeration (wincrypt.h, crypt32.lib):
 *   - CertOpenSystemStoreW(NULL, L"ROOT" | L"MY" | L"CA" | ...) opens a
 *     well-known logical store
 *   - CertEnumCertificatesInStore(store, prev) walks the certificates;
 *     returns a PCCERT_CONTEXT to a CERT_INFO + raw encoded blob
 *   - CertGetNameStringW(pCert, CERT_NAME_FRIENDLY_DISPLAY_TYPE, ...) for
 *     a human-readable subject/issuer
 *   - CertGetCertificateContextProperty(CERT_HASH_PROP_ID) → SHA-1 thumbprint
 *   - The notBefore / notAfter FILETIMEs come straight off pCertInfo
 *
 * Stores chosen from a combo box: ROOT, MY, CA, TRUST, AuthRoot.
 */

#include "shell.h"
#include <wincrypt.h>
#include <commctrl.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "comctl32.lib")

#define CS_PROP   L"MS_CS_STATE"
#define ID_CS_CMB 90001
#define ID_CS_GO  90002
#define ID_CS_LV  90003

typedef struct { HWND combo, goBtn, list; } CsState;
static WNDPROC g_origCsFrame = NULL;

static const wchar_t *g_stores[] = {
    L"ROOT", L"MY", L"CA", L"TRUST", L"AuthRoot"
};

static void Cs_FormatFileTime(FILETIME *ft, wchar_t *out, int cch)
{
    SYSTEMTIME st;
    FileTimeToSystemTime(ft, &st);
    swprintf_s(out, cch, L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
}

static void Cs_LoadStore(CsState *st, const wchar_t *storeName)
{
    HCERTSTORE store;
    PCCERT_CONTEXT cert = NULL;
    int row = 0;

    SendMessageW(st->list, LVM_DELETEALLITEMS, 0, 0);

    store = CertOpenSystemStoreW(0, storeName);
    if (!store) {
        LVITEMW it;
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT; it.iItem = 0;
        it.pszText = (LPWSTR)L"(failed to open store)";
        SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        return;
    }

    while ((cert = CertEnumCertificatesInStore(store, cert)) != NULL) {
        wchar_t subject[400] = L"";
        wchar_t issuer[400]  = L"";
        wchar_t thumb[64]    = L"";
        wchar_t expires[40]  = L"";
        wchar_t algo[80]     = L"";
        LVITEMW it;
        DWORD   hashLen = 0;
        BYTE    hashBytes[64] = {0};

        CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL,
                            subject, 400);
        CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE,
                            CERT_NAME_ISSUER_FLAG, NULL,
                            issuer, 400);
        Cs_FormatFileTime(&cert->pCertInfo->NotAfter, expires, 40);

        hashLen = 64;
        if (CertGetCertificateContextProperty(cert, CERT_HASH_PROP_ID,
                                               hashBytes, &hashLen)) {
            DWORD i, w = 0;
            for (i = 0; i < hashLen && w < 60; ++i) {
                w += swprintf_s(thumb + w, 64 - w, L"%02X", hashBytes[i]);
                if (i == 4 || i == 9 || i == 14) {
                    thumb[w++] = L' ';
                }
            }
        }

        if (cert->pCertInfo->SignatureAlgorithm.pszObjId) {
            char *o = cert->pCertInfo->SignatureAlgorithm.pszObjId;
            MultiByteToWideChar(CP_UTF8, 0, o, -1, algo, 80);
        }

        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT; it.iItem = row;
        it.pszText = subject;
        SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        it.iSubItem = 1; it.pszText = issuer;  SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
        it.iSubItem = 2; it.pszText = expires; SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
        it.iSubItem = 3; it.pszText = thumb;   SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
        it.iSubItem = 4; it.pszText = algo;    SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
        ++row;
    }

    CertCloseStore(store, 0);
}

static LRESULT CALLBACK Cs_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    CsState *st = (CsState *)GetPropW(hwnd, CS_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_CS_GO) {
        int sel = (int)SendMessageW(st->combo, CB_GETCURSEL, 0, 0);
        if (sel >= 0 && sel < (int)(sizeof(g_stores)/sizeof(g_stores[0]))) {
            Cs_LoadStore(st, g_stores[sel]);
        }
        return 0;
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->combo, 12, 38, w - 124, 24, TRUE);
        MoveWindow(st->goBtn, w - 108, 38, 90, 24, TRUE);
        MoveWindow(st->list,  8, 70, w - 16, h - 78, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, CS_PROP); }
    return CallWindowProcW(g_origCsFrame, hwnd, msg, wp, lp);
}

static HWND CertStore_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    CsState *st;
    INITCOMMONCONTROLSEX icc;
    LVCOLUMNW col;
    int i;
    (void)self;

    icc.dwSize = sizeof(icc); icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"CertStore",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (CsState *)calloc(1, sizeof(CsState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->combo = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
        12, 38, w - 124, 200, frame, (HMENU)(LONG_PTR)ID_CS_CMB, hInstance, NULL);
    for (i = 0; i < (int)(sizeof(g_stores)/sizeof(g_stores[0])); ++i) {
        SendMessageW(st->combo, CB_ADDSTRING, 0, (LPARAM)g_stores[i]);
    }
    SendMessageW(st->combo, CB_SETCURSEL, 0, 0);

    st->goBtn = CreateWindowExW(0, L"BUTTON", L"Load",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        w - 108, 38, 90, 24, frame, (HMENU)(LONG_PTR)ID_CS_GO, hInstance, NULL);

    st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
        WS_CHILD | WS_VISIBLE | LVS_REPORT,
        8, 70, w - 16, h - 78, frame, (HMENU)(LONG_PTR)ID_CS_LV, hInstance, NULL);
    SendMessageW(st->list, LVM_SETEXTENDEDLISTVIEWSTYLE,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 240; col.pszText = (LPWSTR)L"Subject"; SendMessageW(st->list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.cx = 240; col.pszText = (LPWSTR)L"Issuer";  SendMessageW(st->list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    col.cx =  90; col.pszText = (LPWSTR)L"Expires"; SendMessageW(st->list, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);
    col.cx = 230; col.pszText = (LPWSTR)L"SHA-1 thumbprint"; SendMessageW(st->list, LVM_INSERTCOLUMNW, 3, (LPARAM)&col);
    col.cx = 150; col.pszText = (LPWSTR)L"Sig algo OID"; SendMessageW(st->list, LVM_INSERTCOLUMNW, 4, (LPARAM)&col);

    SetPropW(frame, CS_PROP, (HANDLE)st);
    if (!g_origCsFrame) g_origCsFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Cs_FrameProc);

    Cs_LoadStore(st, L"ROOT");
    return frame;
}

MsApp g_AppCertStore = { L"CertStore", CertStore_Create, 980, 480 };
