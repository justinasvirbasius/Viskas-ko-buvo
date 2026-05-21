/*
 * app_hasher.c — File hashing via Cryptography API: Next Generation (CNG)
 *
 * Demonstrates the modern Windows crypto stack:
 *   - BCryptOpenAlgorithmProvider with BCRYPT_SHA256_ALGORITHM
 *   - BCryptCreateHash, BCryptHashData, BCryptFinishHash
 *   - Streaming a large file in chunks instead of loading it all at once
 *   - Drag-and-drop file input via DragAcceptFiles + WM_DROPFILES
 */

#include "shell.h"
#include <bcrypt.h>
#include <shellapi.h>
#include <commdlg.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif

#define HASH_PROP    L"MS_HASH_STATE"
#define ID_HASH_OPEN 19001
#define ID_HASH_OUT  19002
#define ID_HASH_LBL  19003

typedef struct {
    HWND openBtn;
    HWND fileLabel;
    HWND output;
} HashState;

static WNDPROC g_origHashFrame = NULL;

static BOOL Hash_File(const wchar_t *path, wchar_t *outHex, size_t outHexLen)
{
    BCRYPT_ALG_HANDLE  alg  = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    NTSTATUS status;
    DWORD hashLen = 0, hashObjLen = 0, count = 0;
    BYTE *hashObj = NULL;
    BYTE digest[64];
    HANDLE file = INVALID_HANDLE_VALUE;
    BYTE buf[64 * 1024];
    DWORD nRead;
    BOOL  ok = FALSE;
    UINT  i;

    status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    if (status != STATUS_SUCCESS) goto cleanup;

    BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&hashObjLen,
                      sizeof(hashObjLen), &count, 0);
    BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, (PUCHAR)&hashLen,
                      sizeof(hashLen), &count, 0);
    if (hashObjLen == 0 || hashLen == 0 || hashLen > sizeof(digest)) goto cleanup;

    hashObj = (BYTE *)malloc(hashObjLen);
    if (!hashObj) goto cleanup;

    status = BCryptCreateHash(alg, &hash, hashObj, hashObjLen, NULL, 0, 0);
    if (status != STATUS_SUCCESS) goto cleanup;

    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) goto cleanup;

    while (ReadFile(file, buf, sizeof(buf), &nRead, NULL) && nRead > 0) {
        if (BCryptHashData(hash, buf, nRead, 0) != STATUS_SUCCESS) goto cleanup;
    }

    if (BCryptFinishHash(hash, digest, hashLen, 0) != STATUS_SUCCESS) goto cleanup;

    /* Hex-encode */
    for (i = 0; i < hashLen && (i * 2 + 1) < outHexLen; ++i) {
        swprintf_s(outHex + i * 2, outHexLen - i * 2, L"%02x", digest[i]);
    }
    outHex[hashLen * 2] = 0;
    ok = TRUE;

cleanup:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (hash) BCryptDestroyHash(hash);
    if (hashObj) free(hashObj);
    if (alg)  BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

static void Hash_RunOn(HashState *st, const wchar_t *path)
{
    wchar_t hex[129];
    wchar_t lineBuf[400];

    SetWindowTextW(st->fileLabel, path);
    SetWindowTextW(st->output, L"Hashing...");
    /* Force redraw before the (potentially slow) hash call */
    UpdateWindow(st->output);

    if (Hash_File(path, hex, 129)) {
        swprintf_s(lineBuf, 400, L"SHA-256:\r\n%s", hex);
    } else {
        swprintf_s(lineBuf, 400, L"Hash failed.");
    }
    SetWindowTextW(st->output, lineBuf);
}

static LRESULT CALLBACK Hash_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    HashState *st = (HashState *)GetPropW(hwnd, HASH_PROP);

    if (msg == WM_DROPFILES && st) {
        HDROP drop = (HDROP)wp;
        wchar_t path[MAX_PATH];
        if (DragQueryFileW(drop, 0, path, MAX_PATH)) {
            Hash_RunOn(st, path);
        }
        DragFinish(drop);
        return 0;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == ID_HASH_OPEN && st) {
        OPENFILENAMEW ofn;
        wchar_t file[MAX_PATH] = L"";
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner   = hwnd;
        ofn.lpstrFile   = file;
        ofn.nMaxFile    = MAX_PATH;
        ofn.lpstrFilter = L"All files\0*.*\0";
        ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
        if (GetOpenFileNameW(&ofn)) Hash_RunOn(st, file);
        return 0;
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->openBtn,   8,  34, 80,      24, TRUE);
        MoveWindow(st->fileLabel, 96, 38, w - 104, 18, TRUE);
        MoveWindow(st->output,    8,  64, w - 16,  h - 72, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        free(st);
        RemovePropW(hwnd, HASH_PROP);
    }
    return CallWindowProcW(g_origHashFrame, hwnd, msg, wp, lp);
}

static HWND Hasher_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    HashState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_ACCEPTFILES,
        MS_CLASS_APPFRAME, L"Hasher",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (HashState *)calloc(1, sizeof(HashState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->openBtn = CreateWindowExW(0, L"BUTTON", L"Open...",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        8, 34, 80, 24, frame, (HMENU)(LONG_PTR)ID_HASH_OPEN, hInstance, NULL);

    st->fileLabel = CreateWindowExW(0, L"STATIC",
        L"(or drop a file here)",
        WS_CHILD | WS_VISIBLE,
        96, 38, w - 104, 18, frame, (HMENU)(LONG_PTR)ID_HASH_LBL, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"Drop a file to compute its SHA-256.",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
        8, 64, w - 16, h - 72, frame, (HMENU)(LONG_PTR)ID_HASH_OUT, hInstance, NULL);

    mono = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, HASH_PROP, (HANDLE)st);
    if (!g_origHashFrame)
        g_origHashFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Hash_FrameProc);

    DragAcceptFiles(frame, TRUE);
    return frame;
}

MsApp g_AppHasher = {
    L"Hasher",
    Hasher_Create,
    560, 200
};
