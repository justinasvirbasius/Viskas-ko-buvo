/*
 * app_locales.c — Enumerate installed locales and inspect one
 *
 * Demonstrates the modern locale APIs (Vista+ "Ex" variants that use locale
 * name strings instead of LCIDs):
 *   - EnumSystemLocalesEx with a callback that visits each installed locale name
 *   - LOCALE_ALL flag to receive every installed locale
 *   - GetLocaleInfoEx with selectors:
 *       LOCALE_SLOCALIZEDDISPLAYNAME — "English (United States)"
 *       LOCALE_SISO639LANGNAME       — "en"
 *       LOCALE_SISO3166CTRYNAME      — "US"
 *       LOCALE_SDECIMAL              — "."
 *       LOCALE_SCURRENCY             — "$"
 *       LOCALE_SSHORTDATE            — "M/d/yyyy"
 *
 * Single-click a row to see details for that locale in the bottom pane.
 */

#include "shell.h"
#include <commctrl.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "comctl32.lib")

#define LO_PROP   L"MS_LO_STATE"
#define ID_LO_LV  49001
#define ID_LO_OUT 49002

typedef struct {
    HWND list, output;
    int  rowCount;
} LoState;

static LoState *g_loCurrent = NULL;

static WNDPROC g_origLoFrame = NULL;

static BOOL CALLBACK Lo_EnumProc(LPWSTR localeName, DWORD flags, LPARAM lp)
{
    LoState *st = (LoState *)lp;
    LVITEMW it;
    wchar_t disp[160];
    (void)flags;

    if (!localeName || !*localeName) return TRUE;

    GetLocaleInfoEx(localeName, LOCALE_SLOCALIZEDDISPLAYNAME, disp, 160);

    ZeroMemory(&it, sizeof(it));
    it.mask = LVIF_TEXT;
    it.iItem = st->rowCount;
    it.pszText = localeName;
    SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
    it.iSubItem = 1; it.pszText = disp;
    SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);

    ++st->rowCount;
    return TRUE;
}

static void Lo_ShowDetail(LoState *st, const wchar_t *locale)
{
    wchar_t disp[160], iso639[16], iso3166[16], dec[16], cur[16], sd[80];
    wchar_t buf[800];

    GetLocaleInfoEx(locale, LOCALE_SLOCALIZEDDISPLAYNAME, disp,    160);
    GetLocaleInfoEx(locale, LOCALE_SISO639LANGNAME,       iso639,  16);
    GetLocaleInfoEx(locale, LOCALE_SISO3166CTRYNAME,      iso3166, 16);
    GetLocaleInfoEx(locale, LOCALE_SDECIMAL,              dec,     16);
    GetLocaleInfoEx(locale, LOCALE_SCURRENCY,             cur,     16);
    GetLocaleInfoEx(locale, LOCALE_SSHORTDATE,            sd,      80);

    swprintf_s(buf, 800,
        L"Locale name      : %s\r\n"
        L"Display name     : %s\r\n"
        L"ISO 639 language : %s\r\n"
        L"ISO 3166 country : %s\r\n"
        L"Decimal separator: %s\r\n"
        L"Currency symbol  : %s\r\n"
        L"Short date format: %s\r\n",
        locale, disp, iso639, iso3166, dec, cur, sd);
    SetWindowTextW(st->output, buf);
}

static LRESULT CALLBACK Lo_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    LoState *st = (LoState *)GetPropW(hwnd, LO_PROP);

    if (msg == WM_NOTIFY && st) {
        NMHDR *hdr = (NMHDR *)lp;
        if (hdr->idFrom == ID_LO_LV &&
            (hdr->code == LVN_ITEMCHANGED || hdr->code == NM_CLICK)) {
            int sel = (int)SendMessageW(st->list, LVM_GETNEXTITEM,
                                        (WPARAM)-1, LVNI_SELECTED);
            if (sel >= 0) {
                wchar_t locale[80];
                LVITEMW it;
                ZeroMemory(&it, sizeof(it));
                it.mask = LVIF_TEXT;
                it.iItem = sel;
                it.pszText = locale;
                it.cchTextMax = 80;
                SendMessageW(st->list, LVM_GETITEMTEXTW, sel, (LPARAM)&it);
                Lo_ShowDetail(st, locale);
            }
            return 0;
        }
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        int half = (h - 90) / 2;
        if (half < 80) half = 80;
        MoveWindow(st->list,   8, 36, w - 16, half, TRUE);
        MoveWindow(st->output, 8, 44 + half, w - 16, h - 52 - half, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, LO_PROP); g_loCurrent = NULL; }
    return CallWindowProcW(g_origLoFrame, hwnd, msg, wp, lp);
}

static HWND Locales_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    LoState *st;
    INITCOMMONCONTROLSEX icc;
    LVCOLUMNW col;
    HFONT mono;
    int half;
    (void)self;

    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Locales",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (LoState *)calloc(1, sizeof(LoState));
    if (!st) { DestroyWindow(frame); return NULL; }
    half = (h - 90) / 2; if (half < 80) half = 80;

    st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
        8, 36, w - 16, half, frame, (HMENU)(LONG_PTR)ID_LO_LV, hInstance, NULL);
    SendMessageW(st->list, LVM_SETEXTENDEDLISTVIEWSTYLE,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 130; col.pszText = (LPWSTR)L"Locale";
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.cx = w - 170; col.pszText = (LPWSTR)L"Display name";
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"Select a locale to see its details.",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 44 + half, w - 16, h - 52 - half, frame,
        (HMENU)(LONG_PTR)ID_LO_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, LO_PROP, (HANDLE)st);
    if (!g_origLoFrame) g_origLoFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Lo_FrameProc);

    /* Enumerate */
    st->rowCount = 0;
    EnumSystemLocalesEx(Lo_EnumProc, LOCALE_ALL, (LPARAM)st, NULL);

    return frame;
}

MsApp g_AppLocales = {
    L"Locales",
    Locales_Create,
    580, 460
};
