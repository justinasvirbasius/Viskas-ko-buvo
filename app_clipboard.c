/*
 * app_clipboard.c — Clipboard history
 *
 * Demonstrates:
 *   - AddClipboardFormatListener + WM_CLIPBOARDUPDATE notifications
 *   - Reading text from the clipboard (OpenClipboard, GetClipboardData,
 *     CF_UNICODETEXT, GlobalLock/Unlock)
 *   - Writing text back to the clipboard (SetClipboardData with a copy
 *     allocated via GlobalAlloc/GMEM_MOVEABLE)
 *
 * Captures the last 50 distinct text clips. Double-click an entry to put it
 * back on the clipboard.
 */

#include "shell.h"
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "user32.lib")

#define CB_PROP       L"MS_CB_STATE"
#define ID_CB_LIST    9001
#define CB_HISTORY_MAX 50
#define CB_ENTRY_MAX   1024

typedef struct {
    HWND     list;
    wchar_t *items[CB_HISTORY_MAX];
    int      count;
    BOOL     suppressNext;  /* avoid re-capturing what we just set */
} CbState;

static WNDPROC g_origCbFrame = NULL;

static void Cb_AddEntry(CbState *st, const wchar_t *text)
{
    wchar_t *copy;
    wchar_t preview[80];
    size_t len;
    int i;

    if (!text || !text[0]) return;
    /* Skip duplicates of the most recent */
    if (st->count > 0 && wcscmp(st->items[0], text) == 0) return;

    len = wcslen(text);
    if (len > CB_ENTRY_MAX) len = CB_ENTRY_MAX;
    copy = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
    if (!copy) return;
    wmemcpy(copy, text, len);
    copy[len] = 0;

    if (st->count == CB_HISTORY_MAX) {
        free(st->items[CB_HISTORY_MAX - 1]);
        st->count--;
    }
    for (i = st->count; i > 0; --i) st->items[i] = st->items[i - 1];
    st->items[0] = copy;
    st->count++;

    /* Refresh listbox */
    SendMessageW(st->list, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < st->count; ++i) {
        const wchar_t *src = st->items[i];
        int j, n = 0;
        for (j = 0; j < 70 && src[j]; ++j) {
            wchar_t c = src[j];
            preview[n++] = (c == L'\r' || c == L'\n' || c == L'\t') ? L' ' : c;
        }
        if (src[j]) {
            preview[n++] = L'.'; preview[n++] = L'.'; preview[n++] = L'.';
        }
        preview[n] = 0;
        SendMessageW(st->list, LB_ADDSTRING, 0, (LPARAM)preview);
    }
}

static void Cb_Capture(HWND frame, CbState *st)
{
    if (st->suppressNext) {
        st->suppressNext = FALSE;
        return;
    }
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return;
    if (!OpenClipboard(frame)) return;
    {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) {
            wchar_t *p = (wchar_t *)GlobalLock(h);
            if (p) {
                Cb_AddEntry(st, p);
                GlobalUnlock(h);
            }
        }
    }
    CloseClipboard();
}

static void Cb_Restore(HWND frame, CbState *st, int idx)
{
    const wchar_t *text;
    size_t bytes;
    HGLOBAL h;
    wchar_t *dst;

    if (idx < 0 || idx >= st->count) return;
    text = st->items[idx];
    bytes = (wcslen(text) + 1) * sizeof(wchar_t);

    h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) return;
    dst = (wchar_t *)GlobalLock(h);
    memcpy(dst, text, bytes);
    GlobalUnlock(h);

    if (OpenClipboard(frame)) {
        EmptyClipboard();
        if (SetClipboardData(CF_UNICODETEXT, h)) {
            st->suppressNext = TRUE;
        } else {
            GlobalFree(h);
        }
        CloseClipboard();
    } else {
        GlobalFree(h);
    }
}

static LRESULT CALLBACK Cb_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    CbState *st = (CbState *)GetPropW(hwnd, CB_PROP);

    if (msg == WM_CLIPBOARDUPDATE && st) {
        Cb_Capture(hwnd, st);
        return 0;
    }
    if (msg == WM_COMMAND && st) {
        if (HIWORD(wp) == LBN_DBLCLK && LOWORD(wp) == ID_CB_LIST) {
            int sel = (int)SendMessageW(st->list, LB_GETCURSEL, 0, 0);
            Cb_Restore(hwnd, st, sel);
            return 0;
        }
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->list, 8, 34, w - 16, h - 42, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        int i;
        RemoveClipboardFormatListener(hwnd);
        for (i = 0; i < st->count; ++i) free(st->items[i]);
        free(st);
        RemovePropW(hwnd, CB_PROP);
    }
    return CallWindowProcW(g_origCbFrame, hwnd, msg, wp, lp);
}

static HWND Clip_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    CbState *st;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Clipboard",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (CbState *)calloc(1, sizeof(CbState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS,
        8, 34, w - 16, h - 42,
        frame, (HMENU)(LONG_PTR)ID_CB_LIST, hInstance, NULL);

    SetPropW(frame, CB_PROP, (HANDLE)st);
    if (!g_origCbFrame)
        g_origCbFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Cb_FrameProc);

    AddClipboardFormatListener(frame);
    /* Capture whatever is currently on the clipboard */
    Cb_Capture(frame, st);
    return frame;
}

MsApp g_AppClipboard = {
    L"Clipboard",
    Clip_Create,
    420, 340
};
