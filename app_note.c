/*
 * app_note.c — Sticky note
 *
 * Each launcher click creates a new note. Demonstrates that the registry
 * can launch arbitrary instances of the same app without any extra wiring.
 * Yellow tinted frame, single multi-line EDIT inside.
 */

#include "shell.h"

#define NOTE_PROP_EDIT L"MS_NOTE_EDIT"

static WNDPROC g_origNoteFrame = NULL;

/* Custom title bar paint so notes look distinct from other apps */
static LRESULT CALLBACK Note_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_SIZE) {
        HWND edit = (HWND)GetPropW(hwnd, NOTE_PROP_EDIT);
        if (edit) {
            int w = LOWORD(lp), h = HIWORD(lp);
            MoveWindow(edit, 6, 32, w - 12, h - 38, TRUE);
        }
    }
    if (msg == WM_DESTROY) {
        RemovePropW(hwnd, NOTE_PROP_EDIT);
    }
    if (msg == WM_CTLCOLOREDIT) {
        HDC hdc = (HDC)wp;
        static HBRUSH yellow = NULL;
        if (!yellow) yellow = CreateSolidBrush(RGB(255, 245, 170));
        SetBkColor(hdc, RGB(255, 245, 170));
        SetTextColor(hdc, RGB(50, 40, 0));
        return (LRESULT)yellow;
    }
    return CallWindowProcW(g_origNoteFrame, hwnd, msg, wp, lp);
}

static HWND Note_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame, edit;
    HFONT font;

    (void)self;
    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Note",
        WS_POPUP | WS_BORDER,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    edit = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
        6, 32, w - 12, h - 38, frame, NULL, hInstance, NULL);

    font = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    SendMessageW(edit, WM_SETFONT, (WPARAM)font, TRUE);

    SetPropW(frame, NOTE_PROP_EDIT, (HANDLE)edit);
    if (!g_origNoteFrame)
        g_origNoteFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Note_FrameProc);

    return frame;
}

MsApp g_AppNote = {
    L"Note",
    Note_Create,
    240, 220
};
