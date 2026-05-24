/*
 * app_ptrinput.c — Windows Pointer Input messages
 *
 * Demonstrates the unified Pointer Input system (Win 8+). Where WM_TOUCH
 * (legacy), WM_MOUSEMOVE (mouse), and pen messages each speak a separate
 * protocol, WM_POINTER* unifies all three behind one HWND messaging
 * channel:
 *
 *   - WM_POINTERDOWN / WM_POINTERUP / WM_POINTERUPDATE — primary stream
 *   - WM_POINTERENTER / WM_POINTERLEAVE — hover transitions
 *   - WM_POINTERWHEEL — scroll over a pointer
 *   - GET_POINTERID_WPARAM(wp) extracts the per-contact pointer ID
 *   - GetPointerType(id, &type) returns PT_TOUCH / PT_PEN / PT_MOUSE
 *   - GetPointerInfo(id, &info) returns POINTER_INFO with the point in
 *     screen pixels, pointer flags (DOWN/UPDATE/UP/INRANGE/INCONTACT),
 *     and a precise high-resolution timestamp
 *
 * Distinct from TouchInj (Batch 14): that one *injects* synthetic touch,
 * this one *receives* pointer input. We draw small dots at each pointer
 * frame so movement traces are visible.
 */

#include "shell.h"
#include <windowsx.h>
#include <stdlib.h>
#include <stdio.h>

#ifndef WM_POINTERDOWN
#define WM_POINTERDOWN       0x0246
#define WM_POINTERUPDATE     0x0245
#define WM_POINTERUP         0x0247
#define WM_POINTERENTER      0x0249
#define WM_POINTERLEAVE      0x024A
#define WM_POINTERWHEEL      0x024E
#endif
#ifndef GET_POINTERID_WPARAM
#define GET_POINTERID_WPARAM(wp) (LOWORD(wp))
#endif

typedef enum { MS_PT_POINTER = 1, MS_PT_TOUCH = 2, MS_PT_PEN = 3, MS_PT_MOUSE = 4 } MS_POINTER_INPUT_TYPE_E;

typedef BOOL (WINAPI *PFN_GetPointerType)(UINT32, UINT32 *);

#define PI_PROP   L"MS_PI_STATE"
#define PI_MAX_DOTS 200

typedef struct {
    POINT  pt;
    UINT32 type;
    BOOL   down;
} PiDot;

typedef struct {
    PiDot   dots[PI_MAX_DOTS];
    int     count;
    HMODULE user32;
    PFN_GetPointerType pGetType;
} PiState;

static WNDPROC g_origPiFrame = NULL;

static void Pi_AddDot(PiState *st, POINT screenPt, HWND frame, UINT32 type, BOOL down)
{
    POINT lp = screenPt;
    ScreenToClient(frame, &lp);
    if (st->count >= PI_MAX_DOTS) {
        memmove(&st->dots[0], &st->dots[1], (PI_MAX_DOTS - 1) * sizeof(PiDot));
        st->count = PI_MAX_DOTS - 1;
    }
    st->dots[st->count].pt = lp;
    st->dots[st->count].type = type;
    st->dots[st->count].down = down;
    st->count++;
    InvalidateRect(frame, NULL, FALSE);
}

static void Pi_Paint(HWND hwnd, PiState *st)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    int i;
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT old = (HFONT)SelectObject(hdc, font);
    RECT client;
    GetClientRect(hwnd, &client);

    /* Background */
    {
        HBRUSH bg = CreateSolidBrush(RGB(248, 248, 252));
        FillRect(hdc, &client, bg);
        DeleteObject(bg);
    }

    SetBkMode(hdc, TRANSPARENT);
    TextOutW(hdc, 12, 36,
        L"Move/tap/click anywhere in this window. Each pointer frame leaves a dot.",
        70);
    {
        wchar_t hdr[80];
        swprintf_s(hdr, 80, L"Captured frames: %d / %d", st->count, PI_MAX_DOTS);
        TextOutW(hdc, 12, 58, hdr, (int)wcslen(hdr));
    }

    for (i = 0; i < st->count; ++i) {
        COLORREF c;
        HBRUSH br;
        int r;
        PiDot *d = &st->dots[i];
        switch (d->type) {
        case MS_PT_TOUCH: c = RGB(50,180,80); break;
        case MS_PT_PEN:   c = RGB(60,90,220); break;
        case MS_PT_MOUSE: c = RGB(220,60,60); break;
        default:          c = RGB(120,120,120); break;
        }
        br = CreateSolidBrush(c);
        r = d->down ? 6 : 3;
        {
            HBRUSH oldBr = (HBRUSH)SelectObject(hdc, br);
            HPEN   oldPn = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
            Ellipse(hdc, d->pt.x - r, d->pt.y - r, d->pt.x + r, d->pt.y + r);
            SelectObject(hdc, oldBr);
            SelectObject(hdc, oldPn);
        }
        DeleteObject(br);
    }

    /* Legend */
    {
        const wchar_t *labels[] = { L"touch", L"pen", L"mouse" };
        COLORREF cols[]  = { RGB(50,180,80), RGB(60,90,220), RGB(220,60,60) };
        int x = 12, y = client.bottom - 28;
        int k;
        for (k = 0; k < 3; ++k) {
            HBRUSH br = CreateSolidBrush(cols[k]);
            RECT swatch = { x, y, x + 14, y + 14 };
            FillRect(hdc, &swatch, br);
            DeleteObject(br);
            TextOutW(hdc, x + 18, y, labels[k], (int)wcslen(labels[k]));
            x += 70;
        }
    }

    SelectObject(hdc, old);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK Pi_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    PiState *st = (PiState *)GetPropW(hwnd, PI_PROP);

    if (st && (msg == WM_POINTERDOWN || msg == WM_POINTERUPDATE || msg == WM_POINTERUP)) {
        POINT spt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        UINT32 type = MS_PT_POINTER;
        if (st->pGetType) st->pGetType(GET_POINTERID_WPARAM(wp), &type);
        Pi_AddDot(st, spt, hwnd, type, msg != WM_POINTERUP);
        return 0;
    }
    if (msg == WM_PAINT && st) { Pi_Paint(hwnd, st); return 0; }
    if (msg == WM_LBUTTONDOWN && st) {
        /* Also accept legacy mouse input for systems without pointer routing */
        POINT spt; GetCursorPos(&spt);
        Pi_AddDot(st, spt, hwnd, MS_PT_MOUSE, TRUE);
        return 0;
    }
    if (msg == WM_MOUSEMOVE && st && (wp & MK_LBUTTON)) {
        POINT spt; GetCursorPos(&spt);
        Pi_AddDot(st, spt, hwnd, MS_PT_MOUSE, TRUE);
        return 0;
    }
    if (msg == WM_DESTROY && st) {
        if (st->user32) FreeLibrary(st->user32);
        free(st); RemovePropW(hwnd, PI_PROP);
    }
    return CallWindowProcW(g_origPiFrame, hwnd, msg, wp, lp);
}

static HWND PtrInput_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    PiState *st;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"PtrInput",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (PiState *)calloc(1, sizeof(PiState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->user32 = LoadLibraryW(L"user32.dll");
    if (st->user32) {
        st->pGetType = (PFN_GetPointerType)GetProcAddress(st->user32, "GetPointerType");
    }

    SetPropW(frame, PI_PROP, (HANDLE)st);
    if (!g_origPiFrame) g_origPiFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Pi_FrameProc);
    return frame;
}

MsApp g_AppPtrInput = { L"PtrInput", PtrInput_Create, 700, 500 };
