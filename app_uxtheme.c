/*
 * app_uxtheme.c — Visual styles theming via uxtheme
 *
 * Demonstrates the UxTheme API (uxtheme.dll), the engine behind Windows'
 * native button/scrollbar/window look since XP:
 *   - OpenThemeData(hwnd, classList) → HTHEME for that visual class
 *     (e.g. L"BUTTON", L"WINDOW", L"PROGRESS", L"TAB", L"REBAR")
 *   - DrawThemeBackground(htheme, hdc, partId, stateId, &rect, NULL)
 *     paints a themed part (button face, scroll thumb, window caption)
 *   - GetThemeColor(htheme, partId, stateId, propId, &COLORREF) reads
 *     theme-defined colors
 *   - GetThemeFont reads font specifications
 *   - CloseThemeData on cleanup
 *
 * We render a grid of themed button parts in all 6 states so the theme
 * engine output is visible — the same parts Windows uses for its own
 * BUTTON control via the BP_PUSHBUTTON part ID.
 */

#include "shell.h"
#include <uxtheme.h>
#include <vssym32.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "uxtheme.lib")

#define UT_PROP   L"MS_UT_STATE"

/* From vsstyle.h: BUTTON class part IDs */
#ifndef BP_PUSHBUTTON
#define BP_PUSHBUTTON 1
#define BP_RADIOBUTTON 2
#define BP_CHECKBOX 3
#define BP_GROUPBOX 4
#endif
#ifndef PBS_NORMAL
#define PBS_NORMAL  1
#define PBS_HOT     2
#define PBS_PRESSED 3
#define PBS_DISABLED 4
#define PBS_DEFAULTED 5
#define PBS_DEFAULTED_ANIMATING 6
#endif

typedef struct {
    HTHEME btnTheme;
    HTHEME winTheme;
} UtState;

static WNDPROC g_origUtFrame = NULL;

static void Ut_Paint(HWND hwnd, UtState *st)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT client;
    int y0 = 20, x;
    const wchar_t *labels[] = {
        L"NORMAL", L"HOT", L"PRESSED", L"DISABLED", L"DEFAULT", L"DEF-ANIM"
    };
    int states[] = { PBS_NORMAL, PBS_HOT, PBS_PRESSED, PBS_DISABLED, PBS_DEFAULTED, PBS_DEFAULTED_ANIMATING };
    int i;
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT old = (HFONT)SelectObject(hdc, font);

    GetClientRect(hwnd, &client);
    SetBkMode(hdc, TRANSPARENT);

    /* Header */
    TextOutW(hdc, 12, y0, L"BUTTON / BP_PUSHBUTTON in all 6 states:", 41);
    y0 += 24;

    /* Six themed buttons */
    x = 12;
    for (i = 0; i < 6; ++i) {
        RECT r;
        r.left = x; r.top = y0; r.right = x + 120; r.bottom = y0 + 36;
        if (st->btnTheme) {
            DrawThemeBackground(st->btnTheme, hdc, BP_PUSHBUTTON, states[i], &r, NULL);
            DrawThemeText(st->btnTheme, hdc, BP_PUSHBUTTON, states[i],
                          labels[i], -1, DT_CENTER | DT_VCENTER | DT_SINGLELINE,
                          0, &r);
        } else {
            Rectangle(hdc, r.left, r.top, r.right, r.bottom);
            DrawTextW(hdc, labels[i], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        x += 130;
    }

    y0 += 56;

    /* Color readback */
    if (st->btnTheme) {
        COLORREF c = 0;
        wchar_t line[80];
        if (SUCCEEDED(GetThemeColor(st->btnTheme, BP_PUSHBUTTON, PBS_NORMAL, TMT_TEXTCOLOR, &c))) {
            swprintf_s(line, 80, L"BUTTON text color (normal) = #%02X%02X%02X",
                       GetRValue(c), GetGValue(c), GetBValue(c));
            TextOutW(hdc, 12, y0, line, (int)wcslen(line));
        }
        y0 += 22;
    }
    if (st->winTheme) {
        COLORREF c = 0;
        wchar_t line[80];
        if (SUCCEEDED(GetThemeColor(st->winTheme, 0, 0, TMT_FILLCOLOR, &c))) {
            swprintf_s(line, 80, L"WINDOW fill color           = #%02X%02X%02X",
                       GetRValue(c), GetGValue(c), GetBValue(c));
            TextOutW(hdc, 12, y0, line, (int)wcslen(line));
        }
    }

    if (!st->btnTheme && !st->winTheme) {
        TextOutW(hdc, 12, y0, L"(no theme active — classic visual style?)", 41);
    }

    SelectObject(hdc, old);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK Ut_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    UtState *st = (UtState *)GetPropW(hwnd, UT_PROP);
    if (msg == WM_PAINT && st) {
        Ut_Paint(hwnd, st);
        return 0;
    }
    if (msg == WM_THEMECHANGED && st) {
        if (st->btnTheme) { CloseThemeData(st->btnTheme); st->btnTheme = OpenThemeData(hwnd, L"BUTTON"); }
        if (st->winTheme) { CloseThemeData(st->winTheme); st->winTheme = OpenThemeData(hwnd, L"WINDOW"); }
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    if (msg == WM_DESTROY && st) {
        if (st->btnTheme) CloseThemeData(st->btnTheme);
        if (st->winTheme) CloseThemeData(st->winTheme);
        free(st); RemovePropW(hwnd, UT_PROP);
    }
    return CallWindowProcW(g_origUtFrame, hwnd, msg, wp, lp);
}

static HWND UxTheme_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    UtState *st;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"UxTheme",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (UtState *)calloc(1, sizeof(UtState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->btnTheme = OpenThemeData(frame, L"BUTTON");
    st->winTheme = OpenThemeData(frame, L"WINDOW");

    SetPropW(frame, UT_PROP, (HANDLE)st);
    if (!g_origUtFrame) g_origUtFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Ut_FrameProc);
    return frame;
}

MsApp g_AppUxTheme = { L"UxTheme", UxTheme_Create, 820, 220 };
