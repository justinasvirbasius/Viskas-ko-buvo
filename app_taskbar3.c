/*
 * app_taskbar3.c — ITaskbarList3 taskbar customization
 *
 * Demonstrates the Windows 7+ taskbar customization COM interface that
 * controls how the running app's taskbar button appears:
 *   - CoCreateInstance(CLSID_TaskbarList) → ITaskbarList3 (call HrInit once)
 *   - SetProgressState(hwnd, TBPF_NORMAL/PAUSED/ERROR) — colored bar
 *   - SetProgressValue(hwnd, completed, total) — fill ratio
 *   - SetOverlayIcon(hwnd, hicon, descr) — small badge over the button icon
 *   - SetThumbnailClip(hwnd, &rect) — restrict thumbnail preview region
 *
 * Note: these methods target a HWND. Since MiniShell apps are child windows
 * of the shell desktop and not separate taskbar entries, the calls below
 * target the MAIN MiniShell window (the top-level desktop HWND), so the
 * effects are visible on the MiniShell taskbar button itself.
 */

#define COBJMACROS
#define CINTERFACE
#define INITGUID

#include "shell.h"
#include <objbase.h>
#include <shobjidl.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "ole32.lib")

#define TB_PROP    L"MS_TB_STATE"
#define ID_TB_PROG 74001
#define ID_TB_PAUS 74002
#define ID_TB_ERR  74003
#define ID_TB_RST  74004
#define ID_TB_OVR  74005
#define ID_TB_OVRX 74006
#define ID_TB_OUT  74007

typedef struct {
    ITaskbarList3 *taskbar;
    HWND           output;
    HWND           taskbarTarget;
    BOOL           comOk;
    int            progress;   /* 0-100 */
} TbState;

static WNDPROC g_origTbFrame = NULL;

static void Tb_Append(TbState *st, const wchar_t *t)
{
    int len = GetWindowTextLengthW(st->output);
    SendMessageW(st->output, EM_SETSEL, len, len);
    SendMessageW(st->output, EM_REPLACESEL, FALSE, (LPARAM)t);
}

static HICON Tb_MakeOverlayIcon(COLORREF color, wchar_t letter)
{
    /* Build a 16x16 colored disc with a single-character label */
    HDC      screen = GetDC(NULL);
    HDC      mem    = CreateCompatibleDC(screen);
    HBITMAP  bmp    = CreateCompatibleBitmap(screen, 16, 16);
    HBITMAP  mask   = CreateBitmap(16, 16, 1, 1, NULL);
    HBITMAP  oldBmp = (HBITMAP)SelectObject(mem, bmp);
    HBRUSH   brush  = CreateSolidBrush(color);
    HBRUSH   oldBr  = (HBRUSH)SelectObject(mem, brush);
    HPEN     pen    = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HPEN     oldPen = (HPEN)SelectObject(mem, pen);
    ICONINFO ii;
    HICON    icon;
    wchar_t  s[2] = { letter, 0 };

    Ellipse(mem, 0, 0, 16, 16);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(255, 255, 255));
    {
        RECT rc = { 0, 0, 16, 16 };
        DrawTextW(mem, s, 1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    SelectObject(mem, oldPen);
    SelectObject(mem, oldBr);
    SelectObject(mem, oldBmp);
    DeleteObject(brush);
    DeleteObject(pen);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);

    ii.fIcon    = TRUE;
    ii.xHotspot = 0;
    ii.yHotspot = 0;
    ii.hbmMask  = mask;
    ii.hbmColor = bmp;
    icon = CreateIconIndirect(&ii);

    DeleteObject(bmp);
    DeleteObject(mask);
    return icon;
}

static void Tb_Progress(TbState *st, TBPFLAG state, const wchar_t *label)
{
    if (!st->taskbar) return;
    st->progress = (st->progress + 25) % 125;
    if (st->progress == 0) st->progress = 25;
    ITaskbarList3_SetProgressState(st->taskbar, st->taskbarTarget, state);
    ITaskbarList3_SetProgressValue(st->taskbar, st->taskbarTarget,
                                    (ULONGLONG)st->progress, 100ULL);
    {
        wchar_t buf[120];
        swprintf_s(buf, 120, L"Progress: %s at %d%%\r\n", label, st->progress);
        Tb_Append(st, buf);
    }
}

static void Tb_Reset(TbState *st)
{
    if (!st->taskbar) return;
    ITaskbarList3_SetProgressState(st->taskbar, st->taskbarTarget, TBPF_NOPROGRESS);
    ITaskbarList3_SetOverlayIcon(st->taskbar, st->taskbarTarget, NULL, NULL);
    st->progress = 0;
    Tb_Append(st, L"Reset.\r\n");
}

static void Tb_Overlay(TbState *st, BOOL on)
{
    if (!st->taskbar) return;
    if (on) {
        HICON icon = Tb_MakeOverlayIcon(RGB(220, 60, 70), L'7');
        ITaskbarList3_SetOverlayIcon(st->taskbar, st->taskbarTarget,
                                      icon, L"7 notifications");
        if (icon) DestroyIcon(icon);
        Tb_Append(st, L"Overlay set.\r\n");
    } else {
        ITaskbarList3_SetOverlayIcon(st->taskbar, st->taskbarTarget, NULL, NULL);
        Tb_Append(st, L"Overlay cleared.\r\n");
    }
}

static LRESULT CALLBACK Tb_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    TbState *st = (TbState *)GetPropW(hwnd, TB_PROP);
    if (msg == WM_COMMAND && st) {
        switch (LOWORD(wp)) {
        case ID_TB_PROG: Tb_Progress(st, TBPF_NORMAL,       L"normal");  return 0;
        case ID_TB_PAUS: Tb_Progress(st, TBPF_PAUSED,       L"paused");  return 0;
        case ID_TB_ERR:  Tb_Progress(st, TBPF_ERROR,        L"error");   return 0;
        case ID_TB_RST:  Tb_Reset(st);                                   return 0;
        case ID_TB_OVR:  Tb_Overlay(st, TRUE);                           return 0;
        case ID_TB_OVRX: Tb_Overlay(st, FALSE);                          return 0;
        }
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 116, w - 16, h - 124, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        Tb_Reset(st);
        if (st->taskbar) ITaskbarList3_Release(st->taskbar);
        if (st->comOk)   CoUninitialize();
        free(st);
        RemovePropW(hwnd, TB_PROP);
    }
    return CallWindowProcW(g_origTbFrame, hwnd, msg, wp, lp);
}

static HWND TaskBar3_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    TbState *st;
    HFONT mono;
    HRESULT hr;
    HWND  top;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"TaskBar3",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (TbState *)calloc(1, sizeof(TbState));
    if (!st) { DestroyWindow(frame); return NULL; }

    /* Find a real top-level window owned by our process for the taskbar
       target. Walk up from our parent until GetParent is NULL. */
    top = parent;
    while (GetParent(top)) top = GetParent(top);
    st->taskbarTarget = top;

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    st->comOk = SUCCEEDED(hr);

    hr = CoCreateInstance(&CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER,
                           &IID_ITaskbarList3, (void **)&st->taskbar);
    if (SUCCEEDED(hr)) {
        ITaskbarList3_HrInit(st->taskbar);
    }

    CreateWindowExW(0, L"STATIC",
        L"Drives the MiniShell taskbar button progress and overlay.\n"
        L"Look at the taskbar (not this window) to see the effect.",
        WS_CHILD | WS_VISIBLE,
        12, 30, w - 24, 40, frame, NULL, hInstance, NULL);

    CreateWindowExW(0, L"BUTTON", L"Progress +25%",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 76, 130, 26, frame, (HMENU)(LONG_PTR)ID_TB_PROG, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Paused",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        150, 76, 90, 26, frame, (HMENU)(LONG_PTR)ID_TB_PAUS, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Error",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        248, 76, 90, 26, frame, (HMENU)(LONG_PTR)ID_TB_ERR, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Reset",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        346, 76, 90, 26, frame, (HMENU)(LONG_PTR)ID_TB_RST, hInstance, NULL);

    CreateWindowExW(0, L"BUTTON", L"Set overlay",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        12, 110, 130, 24, frame, (HMENU)(LONG_PTR)ID_TB_OVR, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Clear overlay",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        150, 110, 130, 24, frame, (HMENU)(LONG_PTR)ID_TB_OVRX, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 142, w - 16, h - 150, frame, (HMENU)(LONG_PTR)ID_TB_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, TB_PROP, (HANDLE)st);
    if (!g_origTbFrame) g_origTbFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Tb_FrameProc);

    if (!st->taskbar) Tb_Append(st, L"ITaskbarList3 unavailable.\r\n");
    else              Tb_Append(st, L"Ready.\r\n");
    return frame;
}

MsApp g_AppTaskBar3 = { L"TaskBar3", TaskBar3_Create, 600, 400 };
