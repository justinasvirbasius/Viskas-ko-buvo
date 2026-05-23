/*
 * app_guidgen.c — GUID generation and cryptographic random bytes
 *
 * Demonstrates two related, often-confused APIs:
 *   - CoCreateGuid produces a UUID following the Windows guid algorithm
 *     (cryptographically strong on Windows; not the v4 algorithm but
 *     equally suitable for identifiers); the resulting GUID is rendered
 *     with StringFromGUID2 (registry format with braces) or manually as the
 *     dashed lowercase form
 *   - BCryptGenRandom(NULL, buf, cb, BCRYPT_USE_SYSTEM_PREFERRED_RNG) gives
 *     arbitrary-length cryptographic randomness suitable for keys, nonces,
 *     and similar — this is the modern recommended Win32 RNG
 *
 * UI: Generate button produces 5 GUIDs in both formats and 32 bytes of CSPRNG
 * randomness as hex.
 */

#define COBJMACROS
#define CINTERFACE
#define INITGUID

#include "shell.h"
#include <objbase.h>
#include <bcrypt.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "bcrypt.lib")

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif

#define GG_PROP    L"MS_GG_STATE"
#define ID_GG_OUT  69001
#define ID_GG_GEN  69002

typedef struct { HWND output, genBtn; BOOL comOk; } GgState;
static WNDPROC g_origGgFrame = NULL;

static void Gg_Append(GgState *st, const wchar_t *t)
{
    int len = GetWindowTextLengthW(st->output);
    SendMessageW(st->output, EM_SETSEL, len, len);
    SendMessageW(st->output, EM_REPLACESEL, FALSE, (LPARAM)t);
}

static void Gg_BytesToHex(const BYTE *in, DWORD cb, wchar_t *out)
{
    static const wchar_t *d = L"0123456789abcdef";
    DWORD i;
    for (i = 0; i < cb; ++i) {
        out[i * 2]     = d[(in[i] >> 4) & 0xF];
        out[i * 2 + 1] = d[in[i] & 0xF];
    }
    out[cb * 2] = 0;
}

static void Gg_Generate(GgState *st)
{
    int i;
    BYTE rnd[32];
    wchar_t hex[65];
    NTSTATUS ns;

    SetWindowTextW(st->output, L"");
    Gg_Append(st, L"== 5 GUIDs (CoCreateGuid) ==\r\n");
    for (i = 0; i < 5; ++i) {
        GUID g;
        OLECHAR registry[64];
        wchar_t line[200];
        if (FAILED(CoCreateGuid(&g))) {
            Gg_Append(st, L"  (CoCreateGuid failed)\r\n");
            continue;
        }
        StringFromGUID2(&g, registry, 64);
        swprintf_s(line, 200, L"  registry form : %s\r\n", registry);
        Gg_Append(st, line);
        swprintf_s(line, 200,
            L"  bare form     : %08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x\r\n\r\n",
            g.Data1, g.Data2, g.Data3,
            g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
            g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
        Gg_Append(st, line);
    }

    Gg_Append(st, L"== 32 bytes from BCryptGenRandom ==\r\n");
    ns = BCryptGenRandom(NULL, rnd, sizeof(rnd), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (ns == STATUS_SUCCESS) {
        Gg_BytesToHex(rnd, sizeof(rnd), hex);
        Gg_Append(st, L"  ");
        Gg_Append(st, hex);
        Gg_Append(st, L"\r\n");
    } else {
        Gg_Append(st, L"  BCryptGenRandom failed.\r\n");
    }
}

static LRESULT CALLBACK Gg_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    GgState *st = (GgState *)GetPropW(hwnd, GG_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_GG_GEN) { Gg_Generate(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->genBtn, 8, 34, 120, 24, TRUE);
        MoveWindow(st->output, 8, 64, w - 16, h - 72, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->comOk) CoUninitialize();
        free(st); RemovePropW(hwnd, GG_PROP);
    }
    return CallWindowProcW(g_origGgFrame, hwnd, msg, wp, lp);
}

static HWND GuidGen_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    GgState *st;
    HFONT mono;
    HRESULT hr;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"GuidGen",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (GgState *)calloc(1, sizeof(GgState));
    if (!st) { DestroyWindow(frame); return NULL; }
    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    st->comOk = SUCCEEDED(hr);

    st->genBtn = CreateWindowExW(0, L"BUTTON", L"Generate",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        8, 34, 120, 24, frame, (HMENU)(LONG_PTR)ID_GG_GEN, hInstance, NULL);
    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 64, w - 16, h - 72, frame, (HMENU)(LONG_PTR)ID_GG_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, GG_PROP, (HANDLE)st);
    if (!g_origGgFrame) g_origGgFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Gg_FrameProc);
    Gg_Generate(st);
    return frame;
}

MsApp g_AppGuidGen = { L"GuidGen", GuidGen_Create, 620, 380 };
