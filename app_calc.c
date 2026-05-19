/*
 * app_calc.c — Pocket calculator
 *
 * A grid of Win32 BUTTON controls plus a read-only EDIT for the display.
 * Supports +, -, *, /, =, C. Frame uses AppFrame_WndProc for the title bar;
 * we subclass to handle WM_COMMAND from buttons and route results.
 */

#include "shell.h"
#include <wchar.h>
#include <stdlib.h>
#include <stdio.h>

#define CALC_PROP_STATE L"MS_CALC_STATE"

typedef struct {
    HWND    display;
    double  acc;          /* accumulator */
    double  pending;      /* operand being typed */
    wchar_t op;           /* pending operator: 0, +, -, *, / */
    BOOL    fresh;        /* TRUE = next digit clears the display */
} CalcState;

static const wchar_t *kButtons[5][4] = {
    { L"C", L"\u00B1", L"%", L"/" },
    { L"7", L"8",      L"9", L"*" },
    { L"4", L"5",      L"6", L"-" },
    { L"1", L"2",      L"3", L"+" },
    { L"0", L"0",      L".", L"=" }   /* 0 spans two cells via id check */
};

#define CALC_ID_BASE 1000

static void Calc_UpdateDisplay(CalcState *st, double v)
{
    wchar_t buf[64];
    swprintf_s(buf, 64, L"%.10g", v);
    SetWindowTextW(st->display, buf);
}

static void Calc_HandleButton(HWND frame, int id)
{
    CalcState *st = (CalcState *)GetPropW(frame, CALC_PROP_STATE);
    int idx;
    const wchar_t *label;
    wchar_t cur[64];

    if (!st) return;
    idx = id - CALC_ID_BASE;
    if (idx < 0 || idx >= 20) return;
    label = kButtons[idx / 4][idx % 4];

    GetWindowTextW(st->display, cur, 64);

    if (label[0] >= L'0' && label[0] <= L'9') {
        if (st->fresh || wcscmp(cur, L"0") == 0) {
            SetWindowTextW(st->display, label);
        } else {
            wchar_t buf[64];
            swprintf_s(buf, 64, L"%s%s", cur, label);
            SetWindowTextW(st->display, buf);
        }
        st->fresh = FALSE;
        return;
    }
    if (label[0] == L'.') {
        if (!wcschr(cur, L'.')) {
            wchar_t buf[64];
            swprintf_s(buf, 64, L"%s.", cur);
            SetWindowTextW(st->display, buf);
        }
        st->fresh = FALSE;
        return;
    }
    if (label[0] == L'C') {
        st->acc = 0; st->pending = 0; st->op = 0; st->fresh = TRUE;
        SetWindowTextW(st->display, L"0");
        return;
    }

    /* Operator or equals */
    st->pending = _wtof(cur);
    if (st->op == 0) {
        st->acc = st->pending;
    } else {
        switch (st->op) {
        case L'+': st->acc += st->pending; break;
        case L'-': st->acc -= st->pending; break;
        case L'*': st->acc *= st->pending; break;
        case L'/': st->acc = st->pending != 0 ? st->acc / st->pending : 0; break;
        }
    }
    Calc_UpdateDisplay(st, st->acc);
    if (label[0] == L'=') {
        st->op = 0;
    } else {
        st->op = label[0];
    }
    st->fresh = TRUE;
}

/* Subclassed frame proc for the calculator */
static WNDPROC g_origCalcFrameProc = NULL;

static LRESULT CALLBACK Calc_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_COMMAND) {
        Calc_HandleButton(hwnd, LOWORD(wp));
        return 0;
    }
    if (msg == WM_DESTROY) {
        CalcState *st = (CalcState *)GetPropW(hwnd, CALC_PROP_STATE);
        if (st) free(st);
        RemovePropW(hwnd, CALC_PROP_STATE);
    }
    return CallWindowProcW(g_origCalcFrameProc, hwnd, msg, wp, lp);
}

static HWND Calc_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    CalcState *st;
    int r, c, padding = 8, titleBar = 32;
    int displayH = 40;
    int gridTop, gridLeft, cellW, cellH;

    (void)self;

    frame = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        MS_CLASS_APPFRAME,
        L"Calc",
        WS_POPUP | WS_BORDER,
        x, y, w, h,
        parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (CalcState *)calloc(1, sizeof(CalcState));
    if (!st) { DestroyWindow(frame); return NULL; }
    st->fresh = TRUE;

    st->display = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT", L"0",
        WS_CHILD | WS_VISIBLE | ES_RIGHT | ES_READONLY,
        padding, titleBar, w - 2 * padding, displayH,
        frame, NULL, hInstance, NULL);

    gridTop  = titleBar + displayH + padding;
    gridLeft = padding;
    cellW    = (w - 2 * padding) / 4;
    cellH    = (h - gridTop - padding) / 5;

    for (r = 0; r < 5; ++r) {
        for (c = 0; c < 4; ++c) {
            int id = CALC_ID_BASE + r * 4 + c;
            CreateWindowExW(
                0, L"BUTTON", kButtons[r][c],
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                gridLeft + c * cellW + 2,
                gridTop  + r * cellH + 2,
                cellW - 4, cellH - 4,
                frame, (HMENU)(LONG_PTR)id, hInstance, NULL);
        }
    }

    SetPropW(frame, CALC_PROP_STATE, (HANDLE)st);
    if (!g_origCalcFrameProc) {
        g_origCalcFrameProc = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    }
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Calc_FrameProc);

    return frame;
}

MsApp g_AppCalc = {
    L"Calc",
    Calc_Create,
    280, 360
};
