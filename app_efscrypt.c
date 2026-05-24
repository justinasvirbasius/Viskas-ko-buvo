/*
 * app_efscrypt.c — NTFS Encrypting File System (EFS) round-trip
 *
 * Demonstrates the per-file NTFS encryption API (advapi32). EFS is the
 * filesystem-level companion to DPAPI (Batch 9): instead of encrypting a
 * blob in memory and storing the ciphertext, EFS marks a file or
 * directory so that NTFS encrypts/decrypts it transparently with the
 * user's key, accessible only by that account.
 *
 *   - EncryptFileW(path) marks the file encrypted; ciphertext is written
 *     and FILE_ATTRIBUTE_ENCRYPTED is set
 *   - DecryptFileW(path, reserved=0) clears the encrypted attribute
 *   - FileEncryptionStatusW(path, &status) reports whether a file is
 *     encryptable; status values include FILE_ENCRYPTABLE,
 *     FILE_IS_ENCRYPTED, FILE_SYSTEM_ATTR, FILE_ROOT_DIR, FILE_SYSTEM_DIR,
 *     FILE_UNKNOWN, FILE_SYSTEM_NOT_SUPPORT (e.g. FAT32)
 *
 * EFS is unavailable on Home SKUs and on non-NTFS volumes; calls then
 * fail with ERROR_ACCESS_DENIED or ERROR_INVALID_FUNCTION respectively.
 */

#include "shell.h"
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "advapi32.lib")

#define EF_PROP    L"MS_EF_STATE"
#define ID_EF_OUT  115001
#define ID_EF_GO   115002

typedef struct { HWND output; } EfState;
static WNDPROC g_origEfFrame = NULL;

static void Ef_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
}

static const wchar_t *Ef_StatusName(DWORD s)
{
    switch (s) {
    case FILE_ENCRYPTABLE:           return L"ENCRYPTABLE";
    case FILE_IS_ENCRYPTED:          return L"IS_ENCRYPTED";
    case FILE_SYSTEM_ATTR:           return L"SYSTEM_ATTR (can't encrypt)";
    case FILE_ROOT_DIR:              return L"ROOT_DIR (can't encrypt)";
    case FILE_SYSTEM_DIR:            return L"SYSTEM_DIR (can't encrypt)";
    case FILE_UNKNOWN:               return L"UNKNOWN";
    case FILE_SYSTEM_NOT_SUPPORT:    return L"SYSTEM_NOT_SUPPORT (e.g. FAT32)";
    case FILE_USER_DISALLOWED:       return L"USER_DISALLOWED";
    case FILE_READ_ONLY:             return L"READ_ONLY (can't encrypt)";
    case FILE_DIR_DISALLOWED:        return L"DIR_DISALLOWED";
    }
    return L"?";
}

static void Ef_RunDemo(EfState *st)
{
    wchar_t path[MAX_PATH];
    HANDLE  h;
    DWORD   written;
    const char content[] = "MiniShell EFS demo plaintext.\r\n";
    DWORD   status;

    SetWindowTextW(st->output, L"");
    ExpandEnvironmentStringsW(L"%TEMP%\\minishell_efs_demo.txt", path, MAX_PATH);

    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        Ef_Append(st->output, L"CreateFile failed.\r\n");
        return;
    }
    WriteFile(h, content, sizeof(content) - 1, &written, NULL);
    CloseHandle(h);
    {
        wchar_t line[MAX_PATH + 100];
        swprintf_s(line, MAX_PATH + 100, L"Created plaintext file:\r\n  %s\r\n\r\n", path);
        Ef_Append(st->output, line);
    }

    /* Status before */
    if (FileEncryptionStatusW(path, &status)) {
        wchar_t line[200];
        swprintf_s(line, 200, L"Status before: %s (%lu)\r\n", Ef_StatusName(status), status);
        Ef_Append(st->output, line);
    }

    /* Encrypt */
    if (EncryptFileW(path)) {
        Ef_Append(st->output, L"\r\nEncryptFileW succeeded.\r\n");
    } else {
        wchar_t line[200];
        swprintf_s(line, 200,
            L"\r\nEncryptFileW failed (err %lu).\r\n"
            L"  Common causes: Home SKU (no EFS), or volume is not NTFS.\r\n",
            GetLastError());
        Ef_Append(st->output, line);
    }

    if (FileEncryptionStatusW(path, &status)) {
        wchar_t line[200];
        swprintf_s(line, 200, L"Status after:  %s (%lu)\r\n", Ef_StatusName(status), status);
        Ef_Append(st->output, line);
    }

    {
        DWORD attr = GetFileAttributesW(path);
        wchar_t line[120];
        swprintf_s(line, 120,
            L"\r\nGetFileAttributes -> 0x%08lx  ENCRYPTED bit: %s\r\n",
            attr,
            (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_ENCRYPTED))
                ? L"set" : L"clear");
        Ef_Append(st->output, line);
    }

    /* Decrypt */
    if (DecryptFileW(path, 0)) {
        Ef_Append(st->output, L"\r\nDecryptFileW succeeded.\r\n");
    } else {
        wchar_t line[100];
        swprintf_s(line, 100, L"\r\nDecryptFileW failed (err %lu).\r\n", GetLastError());
        Ef_Append(st->output, line);
    }

    if (FileEncryptionStatusW(path, &status)) {
        wchar_t line[200];
        swprintf_s(line, 200, L"Final status:  %s\r\n", Ef_StatusName(status));
        Ef_Append(st->output, line);
    }
}

static LRESULT CALLBACK Ef_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    EfState *st = (EfState *)GetPropW(hwnd, EF_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_EF_GO) { Ef_RunDemo(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, EF_PROP); }
    return CallWindowProcW(g_origEfFrame, hwnd, msg, wp, lp);
}

static HWND EfsCrypt_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    EfState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"EfsCrypt",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (EfState *)calloc(1, sizeof(EfState));
    if (!st) { DestroyWindow(frame); return NULL; }

    CreateWindowExW(0, L"BUTTON", L"Run EFS demo",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 150, 26, frame, (HMENU)(LONG_PTR)ID_EF_GO, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"Click to create a plaintext file in %TEMP%, then EncryptFile,\r\n"
        L"then DecryptFile, dumping status at each step.\r\n",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_EF_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, EF_PROP, (HANDLE)st);
    if (!g_origEfFrame) g_origEfFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Ef_FrameProc);
    return frame;
}

MsApp g_AppEfsCrypt = { L"EfsCrypt", EfsCrypt_Create, 760, 440 };
