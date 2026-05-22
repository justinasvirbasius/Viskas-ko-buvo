/*
 * app_dpapi.c — User-scope data protection via DPAPI
 *
 * Demonstrates CryptProtectData and CryptUnprotectData — the only built-in
 * key-management-included encryption on Windows. The key derives from the
 * user's logon credentials and is recoverable across logons; data protected
 * by user A can be decrypted only when user A is signed in (no key handoff
 * required between processes).
 *
 * Demonstrated entry points:
 *   - CryptProtectData(&in, L"label", NULL, NULL, NULL, 0, &out)
 *   - CryptUnprotectData(&in, NULL, NULL, NULL, NULL, 0, &out)
 *   - DATA_BLOB in/out and LocalFree on the cipher blob
 *
 * UI: a plaintext box, an optional description label, two buttons producing
 * a hex blob that's only decryptable in this user's logon context.
 */

#include "shell.h"
#include <wincrypt.h>
#include <dpapi.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "crypt32.lib")

#define DP_PROP    L"MS_DP_STATE"
#define ID_DP_PT   59001
#define ID_DP_CT   59002
#define ID_DP_PRO  59003
#define ID_DP_UNP  59004
#define ID_DP_STAT 59005

typedef struct {
    HWND plainEdit, cipherEdit, status;
} DpState;

static WNDPROC g_origDpFrame = NULL;

static void Dp_BytesToHex(const BYTE *in, DWORD cb, wchar_t *out)
{
    static const wchar_t *digits = L"0123456789abcdef";
    DWORD i;
    for (i = 0; i < cb; ++i) {
        out[i * 2]     = digits[(in[i] >> 4) & 0xF];
        out[i * 2 + 1] = digits[in[i] & 0xF];
    }
    out[cb * 2] = 0;
}

static int Dp_HexNibble(wchar_t c)
{
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

static BOOL Dp_HexToBytes(const wchar_t *in, BYTE **out, DWORD *cb)
{
    SIZE_T len = wcslen(in), i;
    BYTE *buf;
    if (len % 2 || len == 0) return FALSE;
    *cb = (DWORD)(len / 2);
    buf = (BYTE *)LocalAlloc(LMEM_FIXED, *cb);
    if (!buf) return FALSE;
    for (i = 0; i < *cb; ++i) {
        int hi = Dp_HexNibble(in[i * 2]);
        int lo = Dp_HexNibble(in[i * 2 + 1]);
        if (hi < 0 || lo < 0) { LocalFree(buf); return FALSE; }
        buf[i] = (BYTE)((hi << 4) | lo);
    }
    *out = buf;
    return TRUE;
}

static void Dp_Protect(DpState *st)
{
    int len;
    wchar_t *plain;
    DATA_BLOB in, out;
    int cb;
    char *utf8;

    len = GetWindowTextLengthW(st->plainEdit);
    if (len <= 0) { SetWindowTextW(st->status, L"Plaintext is empty."); return; }
    plain = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
    GetWindowTextW(st->plainEdit, plain, len + 1);

    cb = WideCharToMultiByte(CP_UTF8, 0, plain, len, NULL, 0, NULL, NULL);
    utf8 = (char *)malloc(cb);
    WideCharToMultiByte(CP_UTF8, 0, plain, len, utf8, cb, NULL, NULL);

    in.pbData = (BYTE *)utf8;
    in.cbData = cb;

    if (CryptProtectData(&in, L"MiniShell DPAPI demo", NULL, NULL, NULL,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        wchar_t *hex = (wchar_t *)malloc((out.cbData * 2 + 1) * sizeof(wchar_t));
        wchar_t buf[80];
        Dp_BytesToHex(out.pbData, out.cbData, hex);
        SetWindowTextW(st->cipherEdit, hex);
        free(hex);
        swprintf_s(buf, 80,
            L"Protected %d byte plaintext → %lu byte blob.", cb, out.cbData);
        SetWindowTextW(st->status, buf);
        LocalFree(out.pbData);
    } else {
        SetWindowTextW(st->status, L"CryptProtectData failed.");
    }
    free(utf8);
    free(plain);
}

static void Dp_Unprotect(DpState *st)
{
    int len;
    wchar_t *hex;
    DATA_BLOB in, out;
    LPWSTR desc = NULL;

    len = GetWindowTextLengthW(st->cipherEdit);
    if (len <= 0) { SetWindowTextW(st->status, L"Cipher field is empty."); return; }
    hex = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
    GetWindowTextW(st->cipherEdit, hex, len + 1);

    if (!Dp_HexToBytes(hex, &in.pbData, &in.cbData)) {
        SetWindowTextW(st->status, L"Hex parse failed.");
        free(hex);
        return;
    }

    if (CryptUnprotectData(&in, &desc, NULL, NULL, NULL,
                            CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        int wch;
        wchar_t *wide;
        wchar_t status[200];

        wch = MultiByteToWideChar(CP_UTF8, 0, (LPCSTR)out.pbData, out.cbData, NULL, 0);
        wide = (wchar_t *)malloc((wch + 1) * sizeof(wchar_t));
        MultiByteToWideChar(CP_UTF8, 0, (LPCSTR)out.pbData, out.cbData, wide, wch);
        wide[wch] = 0;
        SetWindowTextW(st->plainEdit, wide);
        free(wide);

        swprintf_s(status, 200,
            L"Unprotected %lu bytes. Description: \"%s\"",
            out.cbData, desc ? desc : L"");
        SetWindowTextW(st->status, status);

        LocalFree(out.pbData);
        if (desc) LocalFree(desc);
    } else {
        SetWindowTextW(st->status,
            L"CryptUnprotectData failed (wrong user, corrupt blob, or wrong machine).");
    }
    LocalFree(in.pbData);
    free(hex);
}

static LRESULT CALLBACK Dp_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    DpState *st = (DpState *)GetPropW(hwnd, DP_PROP);
    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_DP_PRO) { Dp_Protect(st);   return 0; }
        if (LOWORD(wp) == ID_DP_UNP) { Dp_Unprotect(st); return 0; }
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        int half = (h - 160) / 2;
        if (half < 60) half = 60;
        MoveWindow(st->plainEdit,  12, 60, w - 24, half, TRUE);
        MoveWindow(st->cipherEdit, 12, 96 + half, w - 24, half, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, DP_PROP); }
    return CallWindowProcW(g_origDpFrame, hwnd, msg, wp, lp);
}

static HWND Dpapi_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    DpState *st;
    HFONT mono;
    int half;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Dpapi",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (DpState *)calloc(1, sizeof(DpState));
    if (!st) { DestroyWindow(frame); return NULL; }
    half = (h - 160) / 2; if (half < 60) half = 60;

    CreateWindowExW(0, L"STATIC", L"Plaintext (any UTF-8):",
        WS_CHILD | WS_VISIBLE, 12, 36, 200, 20, frame, NULL, hInstance, NULL);
    st->plainEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL,
        12, 60, w - 24, half, frame, (HMENU)(LONG_PTR)ID_DP_PT, hInstance, NULL);

    CreateWindowExW(0, L"BUTTON", L"Protect",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 70 + half, 110, 26, frame, (HMENU)(LONG_PTR)ID_DP_PRO, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Unprotect",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        130, 70 + half, 110, 26, frame, (HMENU)(LONG_PTR)ID_DP_UNP, hInstance, NULL);

    st->cipherEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL,
        12, 96 + half, w - 24, half, frame, (HMENU)(LONG_PTR)ID_DP_CT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->cipherEdit, WM_SETFONT, (WPARAM)mono, TRUE);

    st->status = CreateWindowExW(0, L"STATIC",
        L"Blob is decryptable only by the logged-on user.",
        WS_CHILD | WS_VISIBLE,
        12, h - 28, w - 24, 22, frame, (HMENU)(LONG_PTR)ID_DP_STAT, hInstance, NULL);

    SetPropW(frame, DP_PROP, (HANDLE)st);
    if (!g_origDpFrame) g_origDpFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Dp_FrameProc);
    return frame;
}

MsApp g_AppDpapi = {
    L"Dpapi",
    Dpapi_Create,
    560, 460
};
