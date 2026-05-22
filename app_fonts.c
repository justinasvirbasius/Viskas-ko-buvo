/*
 * app_fonts.c — Enumerate installed font families with sample preview
 *
 * Demonstrates the GDI font enumeration API:
 *   - EnumFontFamiliesExW(hdc, &logfont, callback, lParam, 0) where logfont
 *     has CharSet = DEFAULT_CHARSET and pitchAndFamily = 0 to ask for all
 *     family names
 *   - The callback receives ENUMLOGFONTEXW (which includes the family name)
 *     and NEWTEXTMETRICEXW
 *   - We deduplicate by family name (the enumeration may return one row per
 *     style; we only want the family list)
 *   - On selection, render a "The quick brown fox..." sample in that face
 *
 * Selection list on the left; live preview on the right via WM_PAINT
 * creating a font with that face name and DrawTextW'ing onto the canvas.
 */

#include "shell.h"
#include <commctrl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#pragma comment(lib, "comctl32.lib")

#define FN_PROP    L"MS_FN_STATE"
#define ID_FN_LIST 62001
#define ID_FN_PANE 62002

typedef struct {
    HWND     list, pane;
    wchar_t  currentFace[LF_FACESIZE];
} FnState;

static WNDPROC g_origFnFrame = NULL;
static WNDPROC g_origFnPane  = NULL;

static int CALLBACK Fn_EnumProc(const LOGFONTW *lf, const TEXTMETRICW *tm,
                                 DWORD fontType, LPARAM lp)
{
    FnState *st = (FnState *)lp;
    int existing;
    LRESULT found;
    (void)tm; (void)fontType;

    /* Skip @-prefixed vertical-writing variants */
    if (lf->lfFaceName[0] == L'@') return 1;

    existing = (int)SendMessageW(st->list, LB_GETCOUNT, 0, 0);
    found = SendMessageW(st->list, LB_FINDSTRINGEXACT, (WPARAM)-1,
                         (LPARAM)lf->lfFaceName);
    if (found == LB_ERR) {
        SendMessageW(st->list, LB_ADDSTRING, 0, (LPARAM)lf->lfFaceName);
    }
    (void)existing;
    return 1;
}

static LRESULT CALLBACK Fn_PaneProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    FnState *st = (FnState *)GetPropW(GetParent(hwnd), FN_PROP);

    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        HBRUSH bg = CreateSolidBrush(RGB(252, 252, 250));
        HFONT  font, old;
        wchar_t line[200];

        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);
        SetBkMode(hdc, TRANSPARENT);

        if (!st || !st->currentFace[0]) {
            SetTextColor(hdc, RGB(150, 150, 150));
            DrawTextW(hdc, L"Pick a font on the left.", -1, &rc,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            int sizes[] = { 12, 16, 24, 36 };
            int i;
            int y = 16;
            SetTextColor(hdc, RGB(30, 30, 30));

            swprintf_s(line, 200, L"%s", st->currentFace);
            font = CreateFontW(20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            old = (HFONT)SelectObject(hdc, font);
            TextOutW(hdc, 16, y, line, (int)wcslen(line));
            y += 30;
            SelectObject(hdc, old);
            DeleteObject(font);

            for (i = 0; i < (int)ARRAYSIZE(sizes); ++i) {
                font = CreateFontW(sizes[i], 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH, st->currentFace);
                if (!font) continue;
                old = (HFONT)SelectObject(hdc, font);
                swprintf_s(line, 200,
                    L"%d pt  The quick brown fox jumps over the lazy dog 0123456789",
                    sizes[i]);
                TextOutW(hdc, 16, y, line, (int)wcslen(line));
                y += sizes[i] + 12;
                SelectObject(hdc, old);
                DeleteObject(font);
            }
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    return CallWindowProcW(g_origFnPane, hwnd, msg, wp, lp);
}

static void Fn_SelChange(FnState *st)
{
    int sel = (int)SendMessageW(st->list, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) { st->currentFace[0] = 0; }
    else {
        SendMessageW(st->list, LB_GETTEXT, sel, (LPARAM)st->currentFace);
    }
    InvalidateRect(st->pane, NULL, TRUE);
}

static LRESULT CALLBACK Fn_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    FnState *st = (FnState *)GetPropW(hwnd, FN_PROP);

    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_FN_LIST && HIWORD(wp) == LBN_SELCHANGE) {
            Fn_SelChange(st);
            return 0;
        }
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        int listW = 220;
        MoveWindow(st->list, 8, 34, listW, h - 42, TRUE);
        MoveWindow(st->pane, listW + 16, 34, w - listW - 24, h - 42, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, FN_PROP); }
    return CallWindowProcW(g_origFnFrame, hwnd, msg, wp, lp);
}

static HWND Fonts_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    FnState *st;
    LOGFONTW lf;
    HDC hdc;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Fonts",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (FnState *)calloc(1, sizeof(FnState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_SORT,
        8, 34, 220, h - 42, frame, (HMENU)(LONG_PTR)ID_FN_LIST, hInstance, NULL);

    st->pane = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
        236, 34, w - 244, h - 42, frame, (HMENU)(LONG_PTR)ID_FN_PANE, hInstance, NULL);
    g_origFnPane = (WNDPROC)GetWindowLongPtrW(st->pane, GWLP_WNDPROC);
    SetWindowLongPtrW(st->pane, GWLP_WNDPROC, (LONG_PTR)Fn_PaneProc);

    /* Enumerate font families using the screen DC */
    ZeroMemory(&lf, sizeof(lf));
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfPitchAndFamily = 0;
    hdc = GetDC(NULL);
    EnumFontFamiliesExW(hdc, &lf, Fn_EnumProc, (LPARAM)st, 0);
    ReleaseDC(NULL, hdc);

    SetPropW(frame, FN_PROP, (HANDLE)st);
    if (!g_origFnFrame) g_origFnFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Fn_FrameProc);
    return frame;
}

MsApp g_AppFonts = {
    L"Fonts",
    Fonts_Create,
    760, 480
};
