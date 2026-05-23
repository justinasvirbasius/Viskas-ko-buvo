/*
 * app_encodings.c — Codepage conversion survey
 *
 * Demonstrates MultiByteToWideChar / WideCharToMultiByte across multiple
 * codepages:
 *   - CP_UTF8     : UTF-8 (modern default)
 *   - CP_ACP      : the active 8-bit codepage (Windows-1252 on most US/EU
 *                   systems, e.g. 437 on legacy)
 *   - CP_OEMCP    : OEM codepage (cmd.exe default — 437 / 850 / 866 etc.)
 *   - 1252        : Windows-1252 (Western European)
 *   - 1251        : Windows-1251 (Cyrillic)
 *   - 932         : Shift-JIS (Japanese)
 *   - 936         : GB2312 (Simplified Chinese)
 *
 * Round-trips a known UTF-16 string through each codepage. Bytes that don't
 * fit the target codepage map to '?' (the default). The result is shown as
 * hex bytes to make the codepage choice visible. Round-trip back to UTF-16
 * shows any loss.
 */

#include "shell.h"
#include <stdlib.h>
#include <stdio.h>

#define EN_PROP    L"MS_EN_STATE"
#define ID_EN_IN   82001
#define ID_EN_GO   82002
#define ID_EN_OUT  82003

typedef struct { HWND input, goBtn, output; } EnState;
static WNDPROC g_origEnFrame = NULL;

static void En_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
}

static void En_HexBytes(const BYTE *b, int n, wchar_t *out, int cch)
{
    int i, w = 0;
    out[0] = 0;
    for (i = 0; i < n && w < cch - 5; ++i) {
        w += swprintf_s(out + w, cch - w, i ? L" %02x" : L"%02x", b[i]);
    }
}

static void En_RunOne(EnState *st, UINT cp, const wchar_t *label, const wchar_t *src)
{
    char  bytes[1024];
    int   bytesLen;
    wchar_t roundTrip[512] = L"";
    int   rtLen;
    wchar_t hex[1024];
    wchar_t line[2048];
    BOOL  usedDefault = FALSE;

    bytesLen = WideCharToMultiByte(cp, 0, src, -1, bytes, 1024,
        cp == CP_UTF8 || cp == CP_UTF7 ? NULL : "?",
        cp == CP_UTF8 || cp == CP_UTF7 ? NULL : &usedDefault);
    if (bytesLen <= 0) {
        swprintf_s(line, 2048, L"  %-24s (conversion failed)\r\n", label);
        En_Append(st->output, line);
        return;
    }

    En_HexBytes((const BYTE *)bytes, bytesLen - 1, hex, 1024);

    rtLen = MultiByteToWideChar(cp, 0, bytes, bytesLen, roundTrip, 512);
    (void)rtLen;

    swprintf_s(line, 2048,
        L"  %-24s %d bytes %s  hex: %s\r\n"
        L"      round-trip: \"%s\"\r\n",
        label, bytesLen - 1,
        usedDefault ? L"(LOSSY)" : L"(clean)", hex,
        roundTrip);
    En_Append(st->output, line);
}

static void En_RunAll(EnState *st)
{
    int len;
    wchar_t *src;

    SetWindowTextW(st->output, L"");
    len = GetWindowTextLengthW(st->input);
    src = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
    if (!src) return;
    GetWindowTextW(st->input, src, len + 1);

    {
        wchar_t header[200];
        swprintf_s(header, 200, L"Input UTF-16 (%d codepoints): \"%s\"\r\n\r\n",
                   len, src);
        En_Append(st->output, header);
    }

    En_RunOne(st, CP_UTF8,  L"CP_UTF8",         src);
    En_RunOne(st, CP_ACP,   L"CP_ACP (current)",src);
    En_RunOne(st, CP_OEMCP, L"CP_OEMCP",        src);
    En_RunOne(st, 1252,     L"Windows-1252",    src);
    En_RunOne(st, 1251,     L"Windows-1251",    src);
    En_RunOne(st, 932,      L"Shift-JIS (932)", src);
    En_RunOne(st, 936,      L"GB2312 (936)",    src);

    free(src);
}

static LRESULT CALLBACK En_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    EnState *st = (EnState *)GetPropW(hwnd, EN_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_EN_GO) { En_RunAll(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->input,  12, 38, w - 124, 24, TRUE);
        MoveWindow(st->goBtn,  w - 108, 38, 90, 24, TRUE);
        MoveWindow(st->output, 8, 74, w - 16, h - 82, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, EN_PROP); }
    return CallWindowProcW(g_origEnFrame, hwnd, msg, wp, lp);
}

static HWND Encodings_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    EnState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Encodings",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (EnState *)calloc(1, sizeof(EnState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->input = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"Café — 日本語 — Привет — \xD83D\xDE00",  /* mixes Latin1, JP, Cyrillic, emoji */
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        12, 38, w - 124, 24, frame, (HMENU)(LONG_PTR)ID_EN_IN, hInstance, NULL);
    st->goBtn = CreateWindowExW(0, L"BUTTON", L"Survey",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        w - 108, 38, 90, 24, frame, (HMENU)(LONG_PTR)ID_EN_GO, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 74, w - 16, h - 82, frame, (HMENU)(LONG_PTR)ID_EN_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, EN_PROP, (HANDLE)st);
    if (!g_origEnFrame) g_origEnFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)En_FrameProc);
    En_RunAll(st);
    return frame;
}

MsApp g_AppEncodings = { L"Encodings", Encodings_Create, 760, 480 };
