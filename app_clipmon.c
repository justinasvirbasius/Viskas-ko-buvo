/*
 * app_clipmon.c — Push-based clipboard monitor
 *
 * Demonstrates AddClipboardFormatListener — the modern (Vista+) replacement
 * for the SetClipboardViewer chain. Differences from the Batch 3 Clipboard
 * app (which only reads on demand):
 *   - AddClipboardFormatListener(hwnd) subscribes
 *   - WM_CLIPBOARDUPDATE is posted to all listeners on every clipboard
 *     change, with no chained-window plumbing
 *   - GetClipboardSequenceNumber() exposes a monotonic counter — useful
 *     to dedupe duplicate notifications
 *   - EnumClipboardFormats walks every available format
 *   - GetClipboardFormatNameW resolves the integer to a human-readable name
 *     for registered formats; standard formats like CF_TEXT are handled manually
 *   - RemoveClipboardFormatListener on shutdown
 */

#include "shell.h"
#include <stdlib.h>
#include <stdio.h>

#define CM_PROP    L"MS_CM_STATE"
#define ID_CM_OUT  92001

typedef struct { HWND output; BOOL listening; DWORD lastSeq; } CmState;
static WNDPROC g_origCmFrame = NULL;

static void Cm_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
    SendMessageW(e, EM_SCROLLCARET, 0, 0);
}

static const wchar_t *Cm_StdFormat(UINT f)
{
    switch (f) {
    case CF_TEXT:        return L"CF_TEXT";
    case CF_BITMAP:      return L"CF_BITMAP";
    case CF_METAFILEPICT:return L"CF_METAFILEPICT";
    case CF_SYLK:        return L"CF_SYLK";
    case CF_DIF:         return L"CF_DIF";
    case CF_TIFF:        return L"CF_TIFF";
    case CF_OEMTEXT:     return L"CF_OEMTEXT";
    case CF_DIB:         return L"CF_DIB";
    case CF_PALETTE:     return L"CF_PALETTE";
    case CF_PENDATA:     return L"CF_PENDATA";
    case CF_RIFF:        return L"CF_RIFF";
    case CF_WAVE:        return L"CF_WAVE";
    case CF_UNICODETEXT: return L"CF_UNICODETEXT";
    case CF_ENHMETAFILE: return L"CF_ENHMETAFILE";
    case CF_HDROP:       return L"CF_HDROP";
    case CF_LOCALE:      return L"CF_LOCALE";
    case CF_DIBV5:       return L"CF_DIBV5";
    }
    return NULL;
}

static void Cm_DumpClipboard(HWND owner, CmState *st)
{
    SYSTEMTIME t;
    wchar_t header[200];
    DWORD seq = GetClipboardSequenceNumber();
    UINT fmt;

    GetLocalTime(&t);
    swprintf_s(header, 200,
        L"\r\n[%02u:%02u:%02u]  WM_CLIPBOARDUPDATE  seq=%lu (delta=%ld)\r\n",
        t.wHour, t.wMinute, t.wSecond, seq,
        (long)(seq - st->lastSeq));
    Cm_Append(st->output, header);
    st->lastSeq = seq;

    if (!OpenClipboard(owner)) {
        Cm_Append(st->output, L"  (could not open clipboard)\r\n");
        return;
    }

    fmt = 0;
    while ((fmt = EnumClipboardFormats(fmt)) != 0) {
        const wchar_t *std = Cm_StdFormat(fmt);
        wchar_t name[80] = L"";
        wchar_t line[200];

        if (!std) {
            GetClipboardFormatNameW(fmt, name, 80);
            std = name[0] ? name : L"(unregistered)";
        }
        swprintf_s(line, 200, L"  format 0x%04X (%u)  %s\r\n", fmt, fmt, std);
        Cm_Append(st->output, line);
    }

    /* If there's CF_UNICODETEXT, sniff a preview */
    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) {
            wchar_t *text = (wchar_t *)GlobalLock(h);
            if (text) {
                wchar_t preview[120];
                int i, w = 0;
                for (i = 0; text[i] && w < 100; ++i) {
                    if (text[i] == L'\r' || text[i] == L'\n') {
                        preview[w++] = L'\\'; preview[w++] = (text[i] == L'\r') ? L'r' : L'n';
                    } else preview[w++] = text[i];
                }
                preview[w] = 0;
                {
                    wchar_t line[200];
                    swprintf_s(line, 200, L"  text preview: \"%s\"\r\n", preview);
                    Cm_Append(st->output, line);
                }
                GlobalUnlock(h);
            }
        }
    }

    CloseClipboard();
}

static LRESULT CALLBACK Cm_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    CmState *st = (CmState *)GetPropW(hwnd, CM_PROP);
    if (msg == WM_CLIPBOARDUPDATE && st) {
        Cm_DumpClipboard(hwnd, st);
        return 0;
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 50, w - 16, h - 58, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->listening) RemoveClipboardFormatListener(hwnd);
        free(st); RemovePropW(hwnd, CM_PROP);
    }
    return CallWindowProcW(g_origCmFrame, hwnd, msg, wp, lp);
}

static HWND ClipMon_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    CmState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"ClipMon",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (CmState *)calloc(1, sizeof(CmState));
    if (!st) { DestroyWindow(frame); return NULL; }

    CreateWindowExW(0, L"STATIC",
        L"Watching clipboard via AddClipboardFormatListener. Copy something.",
        WS_CHILD | WS_VISIBLE,
        12, 30, w - 24, 18, frame, NULL, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 50, w - 16, h - 58, frame, (HMENU)(LONG_PTR)ID_CM_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, CM_PROP, (HANDLE)st);
    if (!g_origCmFrame) g_origCmFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Cm_FrameProc);

    if (AddClipboardFormatListener(frame)) {
        st->listening = TRUE;
        st->lastSeq = GetClipboardSequenceNumber();
        Cm_Append(st->output, L"Subscribed.\r\n");
    } else {
        Cm_Append(st->output, L"AddClipboardFormatListener failed.\r\n");
    }
    return frame;
}

MsApp g_AppClipMon = { L"ClipMon", ClipMon_Create, 700, 440 };
