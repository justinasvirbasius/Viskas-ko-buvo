/*
 * app_editor.c — Minimal text editor
 *
 * Hosts a multiline EDIT control inside an app frame. The frame's title bar is
 * drawn by AppFrame_WndProc; the editor sizes the EDIT control to fill the
 * remaining client area on WM_SIZE (forwarded by a subclass).
 */

#include "shell.h"

/* We attach the EDIT HWND to the frame using window properties */
#define EDITOR_PROP L"MS_EDIT_HWND"

/* We need to know when the frame resizes so the EDIT can follow. The simplest
 * approach: post-create, subclass the frame to intercept WM_SIZE. */

static WNDPROC g_origFrameProc = NULL;

static LRESULT CALLBACK Editor_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_SIZE) {
        HWND edit = (HWND)GetPropW(hwnd, EDITOR_PROP);
        if (edit) {
            int w = LOWORD(lp);
            int h = HIWORD(lp);
            /* Leave room for the title bar (28 px) */
            MoveWindow(edit, 4, 32, w - 8, h - 36, TRUE);
        }
    }
    if (msg == WM_DESTROY) {
        RemovePropW(hwnd, EDITOR_PROP);
    }
    return CallWindowProcW(g_origFrameProc, hwnd, msg, wp, lp);
}

static HWND Editor_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame, edit;
    HFONT font;

    (void)self;

    frame = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        MS_CLASS_APPFRAME,
        L"Editor",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h,
        parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    edit = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
        4, 32, w - 8, h - 36,
        frame, NULL, hInstance, NULL);

    if (!edit) {
        DestroyWindow(frame);
        return NULL;
    }

    /* Use Consolas if available, otherwise fall back to a fixed font */
    font = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(edit, WM_SETFONT, (WPARAM)font, TRUE);
    SetWindowTextW(edit, L"// Welcome to MiniShell Editor\r\n// Type freely.\r\n");

    /* Attach the EDIT to the frame and subclass */
    SetPropW(frame, EDITOR_PROP, (HANDLE)edit);
    if (!g_origFrameProc) {
        g_origFrameProc = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    }
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Editor_FrameProc);

    return frame;
}

MsApp g_AppEditor = {
    L"Editor",
    Editor_Create,
    520, 380
};
