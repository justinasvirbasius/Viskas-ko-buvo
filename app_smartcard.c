/*
 * app_smartcard.c — Smart-card subsystem enumeration
 *
 * Demonstrates the resource-manager half of the smart-card API
 * (winscard.dll) — the same surface Windows authentication, PIV
 * credentials, and the Yubikey driver speak:
 *
 *   - SCardEstablishContext(SCARD_SCOPE_SYSTEM, NULL, NULL, &ctx) opens
 *     a connection to the resource manager (which lives in the
 *     SCardSvr service)
 *   - SCardListReadersW(ctx, NULL, NULL, &len) returns the size of a
 *     double-NUL-terminated list of reader names; second call with a
 *     buffer fills it
 *   - SCardGetStatusChangeW with a SCARD_READERSTATE array reports
 *     present/empty per reader
 *   - SCardReleaseContext on shutdown
 *
 * Standard "two-call + double-NUL multi-string" Win32 enumeration
 * pattern.
 */

#include "shell.h"
#include <winscard.h>
#include <commctrl.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "winscard.lib")
#pragma comment(lib, "comctl32.lib")

#define SC_PROP   L"MS_SC_STATE"
#define ID_SC_REF 118001
#define ID_SC_LV  118002

typedef struct { HWND refresh, list; } ScState;
static WNDPROC g_origScFrame = NULL;

static void Sc_Refresh(ScState *st)
{
    SCARDCONTEXT ctx;
    LONG  r;
    DWORD len = 0;
    LPWSTR readers = NULL;

    SendMessageW(st->list, LVM_DELETEALLITEMS, 0, 0);

    r = SCardEstablishContext(SCARD_SCOPE_SYSTEM, NULL, NULL, &ctx);
    if (r != SCARD_S_SUCCESS) {
        LVITEMW it;
        wchar_t buf[100];
        swprintf_s(buf, 100, L"(SCardEstablishContext failed: 0x%08lx)", r);
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT;
        it.pszText = buf;
        SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        return;
    }

    r = SCardListReadersW(ctx, NULL, NULL, &len);
    if (r != SCARD_S_SUCCESS || len == 0) {
        LVITEMW it;
        wchar_t buf[100];
        swprintf_s(buf, 100,
            (r == SCARD_E_NO_READERS_AVAILABLE)
                ? L"(no smart-card readers attached)"
                : L"(SCardListReaders: 0x%08lx)", r);
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT;
        it.pszText = buf;
        SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        SCardReleaseContext(ctx);
        return;
    }

    readers = (LPWSTR)calloc(len, sizeof(WCHAR));
    if (!readers) { SCardReleaseContext(ctx); return; }

    r = SCardListReadersW(ctx, NULL, readers, &len);
    if (r == SCARD_S_SUCCESS) {
        LPWSTR p = readers;
        int idx = 0;
        while (*p) {
            SCARD_READERSTATEW state;
            wchar_t presence[80];
            wchar_t atrHex[80] = L"";
            LVITEMW it;

            ZeroMemory(&state, sizeof(state));
            state.szReader = p;
            state.dwCurrentState = SCARD_STATE_UNAWARE;

            if (SCardGetStatusChangeW(ctx, 0, &state, 1) == SCARD_S_SUCCESS ||
                (state.dwEventState & SCARD_STATE_CHANGED)) {
                if (state.dwEventState & SCARD_STATE_PRESENT) wcscpy_s(presence, 80, L"CARD-IN");
                else if (state.dwEventState & SCARD_STATE_EMPTY) wcscpy_s(presence, 80, L"EMPTY");
                else if (state.dwEventState & SCARD_STATE_UNAVAILABLE) wcscpy_s(presence, 80, L"UNAVAIL");
                else wcscpy_s(presence, 80, L"?");
                if (state.cbAtr > 0) {
                    DWORD i;
                    wchar_t *out = atrHex;
                    for (i = 0; i < state.cbAtr && i < 30; ++i) {
                        out += swprintf_s(out, 80 - (out - atrHex), L"%02X ", state.rgbAtr[i]);
                    }
                }
            } else {
                wcscpy_s(presence, 80, L"(unknown)");
            }

            ZeroMemory(&it, sizeof(it));
            it.mask = LVIF_TEXT; it.iItem = idx++;
            it.pszText = p;          SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
            it.iSubItem = 1; it.pszText = presence; SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
            it.iSubItem = 2; it.pszText = atrHex;   SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);

            p += wcslen(p) + 1;
        }
    }

    free(readers);
    SCardReleaseContext(ctx);
}

static LRESULT CALLBACK Sc_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    ScState *st = (ScState *)GetPropW(hwnd, SC_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_SC_REF) { Sc_Refresh(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->refresh, 12, 38, 110, 26, TRUE);
        MoveWindow(st->list,    8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, SC_PROP); }
    return CallWindowProcW(g_origScFrame, hwnd, msg, wp, lp);
}

static HWND SmartCard_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    ScState *st;
    INITCOMMONCONTROLSEX icc;
    LVCOLUMNW col;
    (void)self;

    icc.dwSize = sizeof(icc); icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"SmartCard",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (ScState *)calloc(1, sizeof(ScState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->refresh = CreateWindowExW(0, L"BUTTON", L"Refresh",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 110, 26, frame, (HMENU)(LONG_PTR)ID_SC_REF, hInstance, NULL);

    st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
        WS_CHILD | WS_VISIBLE | LVS_REPORT,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_SC_LV, hInstance, NULL);
    SendMessageW(st->list, LVM_SETEXTENDEDLISTVIEWSTYLE,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 340; col.pszText = (LPWSTR)L"Reader";  SendMessageW(st->list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.cx = 110; col.pszText = (LPWSTR)L"State";   SendMessageW(st->list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    col.cx = 400; col.pszText = (LPWSTR)L"ATR";     SendMessageW(st->list, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);

    SetPropW(frame, SC_PROP, (HANDLE)st);
    if (!g_origScFrame) g_origScFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Sc_FrameProc);
    Sc_Refresh(st);
    return frame;
}

MsApp g_AppSmartCard = { L"SmartCard", SmartCard_Create, 920, 420 };
