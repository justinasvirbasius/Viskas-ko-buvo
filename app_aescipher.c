/*
 * app_aescipher.c — AES-256-CBC encrypt/decrypt via BCrypt (CNG)
 *
 * Demonstrates symmetric-cipher use of the Cryptography Next Generation API,
 * complementing app_hasher.c (which only hashes):
 *   - BCryptOpenAlgorithmProvider(BCRYPT_AES_ALGORITHM)
 *   - BCryptSetProperty to choose CBC chaining mode
 *   - BCryptGenerateSymmetricKey from a SHA-256 of the passphrase
 *   - BCryptGenRandom for the IV
 *   - BCryptEncrypt / BCryptDecrypt with BCRYPT_BLOCK_PADDING (PKCS#7)
 *
 * The UI has a plaintext box, a passphrase box, and two buttons. Encrypt
 * produces hex output (IV || ciphertext); Decrypt parses that hex back.
 * Stays single-threaded for simplicity — these are tiny buffers.
 */

#include "shell.h"
#include <bcrypt.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "bcrypt.lib")

#ifndef NT_SUCCESS
#define NT_SUCCESS(x) ((x) >= 0)
#endif

#define AE_PROP    L"MS_AE_STATE"
#define ID_AE_PT   44001
#define ID_AE_PASS 44002
#define ID_AE_CT   44003
#define ID_AE_ENC  44004
#define ID_AE_DEC  44005
#define ID_AE_STAT 44006

typedef struct {
    HWND plainEdit, passEdit, cipherEdit, status;
} AeState;

static WNDPROC g_origAeFrame = NULL;

static void Ae_SetStatus(AeState *st, const wchar_t *t)
{
    SetWindowTextW(st->status, t);
}

/* Derive 32-byte AES-256 key from passphrase via SHA-256(passphrase_utf8) */
static BOOL Ae_DeriveKey(const wchar_t *pass, BYTE key[32])
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    NTSTATUS s;
    int cb;
    char *utf8;
    BOOL ok = FALSE;

    cb = WideCharToMultiByte(CP_UTF8, 0, pass, -1, NULL, 0, NULL, NULL);
    if (cb <= 1) return FALSE;
    utf8 = (char *)malloc(cb);
    if (!utf8) return FALSE;
    WideCharToMultiByte(CP_UTF8, 0, pass, -1, utf8, cb, NULL, NULL);

    s = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    if (!NT_SUCCESS(s)) goto done;
    s = BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0);
    if (!NT_SUCCESS(s)) goto done;
    /* exclude trailing NUL */
    s = BCryptHashData(hash, (PUCHAR)utf8, (ULONG)(cb - 1), 0);
    if (!NT_SUCCESS(s)) goto done;
    s = BCryptFinishHash(hash, key, 32, 0);
    if (!NT_SUCCESS(s)) goto done;
    ok = TRUE;

done:
    if (hash) BCryptDestroyHash(hash);
    if (alg)  BCryptCloseAlgorithmProvider(alg, 0);
    free(utf8);
    return ok;
}

static void Ae_BytesToHex(const BYTE *in, ULONG cb, wchar_t *out)
{
    static const wchar_t *digits = L"0123456789abcdef";
    ULONG i;
    for (i = 0; i < cb; ++i) {
        out[i * 2]     = digits[(in[i] >> 4) & 0xF];
        out[i * 2 + 1] = digits[in[i] & 0xF];
    }
    out[cb * 2] = 0;
}

static int Ae_HexNibble(wchar_t c)
{
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

static BOOL Ae_HexToBytes(const wchar_t *in, BYTE **out, ULONG *cb)
{
    size_t len = wcslen(in), i;
    BYTE *buf;
    if (len % 2) return FALSE;
    *cb = (ULONG)(len / 2);
    if (*cb == 0) return FALSE;
    buf = (BYTE *)malloc(*cb);
    if (!buf) return FALSE;
    for (i = 0; i < *cb; ++i) {
        int hi = Ae_HexNibble(in[i * 2]);
        int lo = Ae_HexNibble(in[i * 2 + 1]);
        if (hi < 0 || lo < 0) { free(buf); return FALSE; }
        buf[i] = (BYTE)((hi << 4) | lo);
    }
    *out = buf;
    return TRUE;
}

static void Ae_Encrypt(AeState *st)
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    NTSTATUS s;
    BYTE keyBytes[32];
    BYTE iv[16];
    BYTE *pt = NULL, *ct = NULL, *combined = NULL;
    int  ptCb;
    ULONG ctCb = 0;
    int   ptLen, passLen;
    wchar_t *pass = NULL, *plain = NULL, *hex = NULL;
    char *ptUtf8 = NULL;

    passLen = GetWindowTextLengthW(st->passEdit);
    ptLen   = GetWindowTextLengthW(st->plainEdit);
    if (passLen <= 0) { Ae_SetStatus(st, L"Enter a passphrase first."); return; }
    if (ptLen   <= 0) { Ae_SetStatus(st, L"Plaintext is empty.");      return; }

    pass  = (wchar_t *)malloc((passLen + 1) * sizeof(wchar_t));
    plain = (wchar_t *)malloc((ptLen + 1) * sizeof(wchar_t));
    if (!pass || !plain) goto done;
    GetWindowTextW(st->passEdit,  pass,  passLen + 1);
    GetWindowTextW(st->plainEdit, plain, ptLen + 1);

    if (!Ae_DeriveKey(pass, keyBytes)) { Ae_SetStatus(st, L"Key derivation failed."); goto done; }

    /* plaintext as UTF-8 bytes */
    ptCb = WideCharToMultiByte(CP_UTF8, 0, plain, ptLen, NULL, 0, NULL, NULL);
    pt = (BYTE *)malloc(ptCb);
    if (!pt) goto done;
    WideCharToMultiByte(CP_UTF8, 0, plain, ptLen, (LPSTR)pt, ptCb, NULL, NULL);

    s = BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, NULL, 0);
    if (!NT_SUCCESS(s)) { Ae_SetStatus(st, L"AES provider open failed."); goto done; }
    s = BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
            (PUCHAR)BCRYPT_CHAIN_MODE_CBC,
            sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
    if (!NT_SUCCESS(s)) goto done;

    s = BCryptGenerateSymmetricKey(alg, &key, NULL, 0, keyBytes, 32, 0);
    if (!NT_SUCCESS(s)) goto done;

    BCryptGenRandom(NULL, iv, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG);

    /* Two-pass: ask required size, then do the encrypt */
    {
        BYTE iv_copy[16];
        memcpy(iv_copy, iv, 16);   /* BCryptEncrypt overwrites the IV */
        s = BCryptEncrypt(key, pt, ptCb, NULL, iv_copy, 16,
                          NULL, 0, &ctCb, BCRYPT_BLOCK_PADDING);
        if (!NT_SUCCESS(s)) goto done;
        ct = (BYTE *)malloc(ctCb);
        if (!ct) goto done;
        memcpy(iv_copy, iv, 16);
        s = BCryptEncrypt(key, pt, ptCb, NULL, iv_copy, 16,
                          ct, ctCb, &ctCb, BCRYPT_BLOCK_PADDING);
        if (!NT_SUCCESS(s)) goto done;
    }

    /* combined = IV || ciphertext, as hex */
    combined = (BYTE *)malloc(16 + ctCb);
    memcpy(combined, iv, 16);
    memcpy(combined + 16, ct, ctCb);
    hex = (wchar_t *)malloc(((16 + ctCb) * 2 + 1) * sizeof(wchar_t));
    Ae_BytesToHex(combined, 16 + ctCb, hex);
    SetWindowTextW(st->cipherEdit, hex);
    {
        wchar_t buf[80];
        swprintf_s(buf, 80, L"Encrypted %d bytes → %lu byte ciphertext.",
                   ptCb, ctCb);
        Ae_SetStatus(st, buf);
    }

done:
    if (key) BCryptDestroyKey(key);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    free(pt); free(ct); free(combined); free(hex);
    free(pass); free(plain); free(ptUtf8);
}

static void Ae_Decrypt(AeState *st)
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    NTSTATUS s;
    BYTE keyBytes[32];
    BYTE *combined = NULL, *pt = NULL;
    ULONG combinedCb = 0, ptCb = 0;
    int   passLen, ctLen;
    wchar_t *pass = NULL, *hex = NULL, *plain = NULL;

    passLen = GetWindowTextLengthW(st->passEdit);
    ctLen   = GetWindowTextLengthW(st->cipherEdit);
    if (passLen <= 0) { Ae_SetStatus(st, L"Enter a passphrase first."); return; }
    if (ctLen   <= 0) { Ae_SetStatus(st, L"Ciphertext field is empty."); return; }

    pass = (wchar_t *)malloc((passLen + 1) * sizeof(wchar_t));
    hex  = (wchar_t *)malloc((ctLen + 1) * sizeof(wchar_t));
    GetWindowTextW(st->passEdit,   pass, passLen + 1);
    GetWindowTextW(st->cipherEdit, hex,  ctLen + 1);

    if (!Ae_HexToBytes(hex, &combined, &combinedCb) || combinedCb < 32) {
        Ae_SetStatus(st, L"Hex parse failed or input too short.");
        goto done;
    }
    if (!Ae_DeriveKey(pass, keyBytes)) goto done;

    s = BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, NULL, 0);
    if (!NT_SUCCESS(s)) goto done;
    BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
        (PUCHAR)BCRYPT_CHAIN_MODE_CBC, sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
    s = BCryptGenerateSymmetricKey(alg, &key, NULL, 0, keyBytes, 32, 0);
    if (!NT_SUCCESS(s)) goto done;

    {
        BYTE iv_copy[16];
        BYTE *cipher = combined + 16;
        ULONG cipherCb = combinedCb - 16;
        memcpy(iv_copy, combined, 16);
        s = BCryptDecrypt(key, cipher, cipherCb, NULL, iv_copy, 16,
                          NULL, 0, &ptCb, BCRYPT_BLOCK_PADDING);
        if (!NT_SUCCESS(s)) { Ae_SetStatus(st, L"BCryptDecrypt sizing failed."); goto done; }
        pt = (BYTE *)malloc(ptCb + 1);
        if (!pt) goto done;
        memcpy(iv_copy, combined, 16);
        s = BCryptDecrypt(key, cipher, cipherCb, NULL, iv_copy, 16,
                          pt, ptCb, &ptCb, BCRYPT_BLOCK_PADDING);
        if (!NT_SUCCESS(s)) { Ae_SetStatus(st, L"Decrypt failed (wrong key or corrupt data?)."); goto done; }
        pt[ptCb] = 0;
    }

    /* UTF-8 → wide */
    {
        int wch = MultiByteToWideChar(CP_UTF8, 0, (LPCSTR)pt, ptCb, NULL, 0);
        plain = (wchar_t *)malloc((wch + 1) * sizeof(wchar_t));
        MultiByteToWideChar(CP_UTF8, 0, (LPCSTR)pt, ptCb, plain, wch);
        plain[wch] = 0;
    }
    SetWindowTextW(st->plainEdit, plain);
    {
        wchar_t buf[80];
        swprintf_s(buf, 80, L"Decrypted to %lu bytes plaintext.", ptCb);
        Ae_SetStatus(st, buf);
    }

done:
    if (key) BCryptDestroyKey(key);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    free(combined); free(pt); free(pass); free(hex); free(plain);
}

static LRESULT CALLBACK Ae_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    AeState *st = (AeState *)GetPropW(hwnd, AE_PROP);

    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_AE_ENC) { Ae_Encrypt(st); return 0; }
        if (LOWORD(wp) == ID_AE_DEC) { Ae_Decrypt(st); return 0; }
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        int half = (h - 200) / 2;
        if (half < 60) half = 60;
        MoveWindow(st->plainEdit,  12, 60,  w - 24, half, TRUE);
        MoveWindow(st->cipherEdit, 12, 96 + half, w - 24, half, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, AE_PROP); }
    return CallWindowProcW(g_origAeFrame, hwnd, msg, wp, lp);
}

static HWND AesCipher_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    AeState *st;
    HFONT mono;
    int half;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"AesCipher",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (AeState *)calloc(1, sizeof(AeState));
    if (!st) { DestroyWindow(frame); return NULL; }

    half = (h - 200) / 2; if (half < 60) half = 60;

    CreateWindowExW(0, L"STATIC", L"Passphrase:",
        WS_CHILD | WS_VISIBLE, 12, 36, 80, 20, frame, NULL, hInstance, NULL);
    st->passEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_PASSWORD,
        96, 34, w - 108, 22, frame, (HMENU)(LONG_PTR)ID_AE_PASS, hInstance, NULL);

    CreateWindowExW(0, L"STATIC", L"Plaintext:",
        WS_CHILD | WS_VISIBLE, 12, 60, 80, 20, frame, NULL, hInstance, NULL);
    st->plainEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL,
        12, 60, w - 24, half, frame, (HMENU)(LONG_PTR)ID_AE_PT, hInstance, NULL);

    CreateWindowExW(0, L"BUTTON", L"Encrypt",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 70 + half, 90, 26, frame, (HMENU)(LONG_PTR)ID_AE_ENC, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Decrypt",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        110, 70 + half, 90, 26, frame, (HMENU)(LONG_PTR)ID_AE_DEC, hInstance, NULL);

    st->cipherEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL,
        12, 96 + half, w - 24, half, frame, (HMENU)(LONG_PTR)ID_AE_CT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->cipherEdit, WM_SETFONT, (WPARAM)mono, TRUE);

    st->status = CreateWindowExW(0, L"STATIC", L"Ready.",
        WS_CHILD | WS_VISIBLE,
        12, h - 28, w - 24, 22, frame, (HMENU)(LONG_PTR)ID_AE_STAT, hInstance, NULL);

    SetPropW(frame, AE_PROP, (HANDLE)st);
    if (!g_origAeFrame) g_origAeFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Ae_FrameProc);
    return frame;
}

MsApp g_AppAesCipher = {
    L"AesCipher",
    AesCipher_Create,
    560, 460
};
