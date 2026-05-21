/*
 * app_compressor.c — Windows Compression API round-trip
 *
 * Demonstrates the cabinet.dll compression interface in buffer mode (the
 * simpler of the two — automatically splits the input into appropriate
 * blocks and stores their sizes):
 *   - CreateCompressor(COMPRESS_ALGORITHM_LZMS, ...) and CreateDecompressor
 *   - Compress and Decompress with two-pass sizing
 *   - Free with CloseCompressor / CloseDecompressor
 *
 * The UI feeds a multi-line text input through compress and then decompress
 * and shows the ratio and round-tripped output side-by-side. Available
 * Windows 8 and later.
 */

#include "shell.h"
#include <compressapi.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "cabinet.lib")

#define CP_PROP    L"MS_CP_STATE"
#define ID_CP_IN   45001
#define ID_CP_OUT  45002
#define ID_CP_GO   45003
#define ID_CP_STAT 45004
#define ID_CP_FILL 45005

typedef struct {
    HWND inEdit, outEdit, status;
} CpState;

static WNDPROC g_origCpFrame = NULL;

static void Cp_Run(CpState *st)
{
    COMPRESSOR_HANDLE   comp = NULL;
    DECOMPRESSOR_HANDLE dec  = NULL;
    int len;
    char  *inUtf8 = NULL;
    BYTE  *comped = NULL;
    BYTE  *deced  = NULL;
    SIZE_T cIn = 0, cComp = 0, cDec = 0;
    wchar_t *inW = NULL, *outW = NULL;
    wchar_t status[160];

    len = GetWindowTextLengthW(st->inEdit);
    if (len <= 0) {
        SetWindowTextW(st->status, L"Input is empty.");
        return;
    }
    inW = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
    if (!inW) return;
    GetWindowTextW(st->inEdit, inW, len + 1);

    cIn = WideCharToMultiByte(CP_UTF8, 0, inW, len, NULL, 0, NULL, NULL);
    inUtf8 = (char *)malloc(cIn);
    if (!inUtf8) goto done;
    WideCharToMultiByte(CP_UTF8, 0, inW, len, inUtf8, (int)cIn, NULL, NULL);

    if (!CreateCompressor(COMPRESS_ALGORITHM_LZMS, NULL, &comp)) {
        SetWindowTextW(st->status, L"CreateCompressor failed.");
        goto done;
    }

    /* Sizing pass */
    if (!Compress(comp, inUtf8, cIn, NULL, 0, &cComp)) {
        DWORD e = GetLastError();
        if (e != ERROR_INSUFFICIENT_BUFFER) {
            SetWindowTextW(st->status, L"Compress sizing failed.");
            goto done;
        }
    }
    comped = (BYTE *)malloc(cComp);
    if (!comped) goto done;
    if (!Compress(comp, inUtf8, cIn, comped, cComp, &cComp)) {
        SetWindowTextW(st->status, L"Compress failed.");
        goto done;
    }

    if (!CreateDecompressor(COMPRESS_ALGORITHM_LZMS, NULL, &dec)) {
        SetWindowTextW(st->status, L"CreateDecompressor failed.");
        goto done;
    }

    if (!Decompress(dec, comped, cComp, NULL, 0, &cDec)) {
        DWORD e = GetLastError();
        if (e != ERROR_INSUFFICIENT_BUFFER) {
            SetWindowTextW(st->status, L"Decompress sizing failed.");
            goto done;
        }
    }
    deced = (BYTE *)malloc(cDec + 1);
    if (!deced) goto done;
    if (!Decompress(dec, comped, cComp, deced, cDec, &cDec)) {
        SetWindowTextW(st->status, L"Decompress failed.");
        goto done;
    }
    deced[cDec] = 0;

    {
        int wch = MultiByteToWideChar(CP_UTF8, 0, (LPCSTR)deced, (int)cDec, NULL, 0);
        outW = (wchar_t *)malloc((wch + 1) * sizeof(wchar_t));
        MultiByteToWideChar(CP_UTF8, 0, (LPCSTR)deced, (int)cDec, outW, wch);
        outW[wch] = 0;
        SetWindowTextW(st->outEdit, outW);
    }

    swprintf_s(status, 160,
        L"LZMS: %llu B → %llu B (%.1f%%); round-trip %s.",
        (unsigned long long)cIn,
        (unsigned long long)cComp,
        cIn ? (cComp * 100.0 / cIn) : 0.0,
        cDec == cIn && memcmp(deced, inUtf8, cIn) == 0 ? L"OK" : L"MISMATCH");
    SetWindowTextW(st->status, status);

done:
    if (comp) CloseCompressor(comp);
    if (dec)  CloseDecompressor(dec);
    free(inUtf8); free(comped); free(deced); free(inW); free(outW);
}

static void Cp_FillSample(CpState *st)
{
    /* Compressible text: repetitive content shows off LZMS well. */
    static const wchar_t *sample =
        L"The quick brown fox jumps over the lazy dog.\r\n"
        L"The quick brown fox jumps over the lazy dog.\r\n"
        L"The quick brown fox jumps over the lazy dog.\r\n"
        L"The quick brown fox jumps over the lazy dog.\r\n"
        L"The quick brown fox jumps over the lazy dog.\r\n"
        L"The quick brown fox jumps over the lazy dog.\r\n"
        L"The quick brown fox jumps over the lazy dog.\r\n"
        L"The quick brown fox jumps over the lazy dog.\r\n"
        L"--- Some unique text in the middle to defeat trivial dedup ---\r\n"
        L"The quick brown fox jumps over the lazy dog.\r\n"
        L"The quick brown fox jumps over the lazy dog.\r\n"
        L"The quick brown fox jumps over the lazy dog.\r\n"
        L"The quick brown fox jumps over the lazy dog.\r\n";
    SetWindowTextW(st->inEdit, sample);
}

static LRESULT CALLBACK Cp_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    CpState *st = (CpState *)GetPropW(hwnd, CP_PROP);

    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_CP_GO)   { Cp_Run(st); return 0; }
        if (LOWORD(wp) == ID_CP_FILL) { Cp_FillSample(st); return 0; }
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        int half = (h - 130) / 2;
        if (half < 60) half = 60;
        MoveWindow(st->inEdit,  12, 70, w - 24, half, TRUE);
        MoveWindow(st->outEdit, 12, 82 + half, w - 24, half, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, CP_PROP); }
    return CallWindowProcW(g_origCpFrame, hwnd, msg, wp, lp);
}

static HWND Compressor_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    CpState *st;
    int half;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Compressor",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (CpState *)calloc(1, sizeof(CpState));
    if (!st) { DestroyWindow(frame); return NULL; }
    half = (h - 130) / 2; if (half < 60) half = 60;

    CreateWindowExW(0, L"BUTTON", L"Run round-trip",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 34, 140, 26, frame, (HMENU)(LONG_PTR)ID_CP_GO, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Fill sample",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        160, 34, 110, 26, frame, (HMENU)(LONG_PTR)ID_CP_FILL, hInstance, NULL);

    st->inEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"Type or paste text here, then click 'Run round-trip'.\r\n",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL,
        12, 70, w - 24, half, frame, (HMENU)(LONG_PTR)ID_CP_IN, hInstance, NULL);
    st->outEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        12, 82 + half, w - 24, half, frame, (HMENU)(LONG_PTR)ID_CP_OUT, hInstance, NULL);

    st->status = CreateWindowExW(0, L"STATIC", L"Ready (LZMS).",
        WS_CHILD | WS_VISIBLE,
        12, h - 28, w - 24, 22, frame, (HMENU)(LONG_PTR)ID_CP_STAT, hInstance, NULL);

    SetPropW(frame, CP_PROP, (HANDLE)st);
    if (!g_origCpFrame) g_origCpFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Cp_FrameProc);
    return frame;
}

MsApp g_AppCompressor = {
    L"Compress",
    Compressor_Create,
    560, 460
};
