/*
 * app_txreg.c — Transactional registry edits via TxR / KTM
 *
 * Demonstrates the Kernel Transaction Manager (KTM) registry layer —
 * registry changes that participate in a kernel transaction so they can
 * be atomically committed or rolled back as a unit. Used by Windows
 * Update during package install and by MSI for staged registry
 * writes.
 *
 *   - CreateTransaction(NULL, 0, 0, 0, 0, 0, L"name") returns a
 *     HANDLE to a new TX object (ktmw32.dll)
 *   - RegCreateKeyTransactedW(HKCU, L"Software\\Mini...", 0, NULL, 0,
 *     KEY_ALL_ACCESS, NULL, &key, &disp, txHandle, NULL) opens/creates
 *     a key bound to the transaction
 *   - RegSetValueExW within the txed key
 *   - RegOpenKeyExW from OUTSIDE the tx sees nothing yet
 *   - CommitTransaction(txHandle) makes everything visible at once;
 *     RollbackTransaction(txHandle) discards them all
 *
 * We write three values inside a TX, prove they're invisible outside,
 * then either commit or roll back per user choice.
 */

#include "shell.h"
#include <ktmw32.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ktmw32.lib")

#define TX_PROP   L"MS_TX_STATE"
#define ID_TX_CMT 126001
#define ID_TX_RBK 126002
#define ID_TX_OUT 126003

typedef struct { HWND output; } TxState;
static WNDPROC g_origTxFrame = NULL;

static const wchar_t *TX_SUBKEY = L"Software\\MiniShell\\TxRegDemo";

static void Tx_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
}

static BOOL Tx_KeyExists(HKEY root, const wchar_t *path)
{
    HKEY k;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        RegCloseKey(k);
        return TRUE;
    }
    return FALSE;
}

static void Tx_RunDemo(TxState *st, BOOL commit)
{
    HANDLE tx;
    HKEY   key;
    DWORD  disposition;
    LSTATUS s;

    SetWindowTextW(st->output, L"");
    {
        wchar_t line[120];
        swprintf_s(line, 120, L"Target: HKCU\\%s\r\n\r\n", TX_SUBKEY);
        Tx_Append(st->output, line);
    }

    tx = CreateTransaction(NULL, 0, 0, 0, 0, 0, L"MiniShell-TxReg");
    if (tx == INVALID_HANDLE_VALUE) {
        Tx_Append(st->output, L"CreateTransaction failed.\r\n");
        return;
    }
    Tx_Append(st->output, L"Transaction created.\r\n");

    s = RegCreateKeyTransactedW(HKEY_CURRENT_USER, TX_SUBKEY, 0, NULL, 0,
                                KEY_ALL_ACCESS, NULL, &key, &disposition,
                                tx, NULL);
    if (s != ERROR_SUCCESS) {
        wchar_t line[100];
        swprintf_s(line, 100, L"RegCreateKeyTransacted failed: %lu\r\n", s);
        Tx_Append(st->output, line);
        CloseHandle(tx);
        return;
    }
    Tx_Append(st->output,
        (disposition == REG_CREATED_NEW_KEY)
            ? L"Tx key CREATED.\r\n"
            : L"Tx key OPENED (existed).\r\n");

    /* Write three values inside the tx */
    {
        const wchar_t *v1 = L"hello-from-tx";
        DWORD v2 = 42;
        const wchar_t *v3 = L"about to be committed or rolled back";
        RegSetValueExW(key, L"Greeting",     0, REG_SZ,
                       (const BYTE *)v1, (DWORD)((wcslen(v1) + 1) * sizeof(WCHAR)));
        RegSetValueExW(key, L"Answer",       0, REG_DWORD,
                       (const BYTE *)&v2, sizeof(v2));
        RegSetValueExW(key, L"Comment",      0, REG_SZ,
                       (const BYTE *)v3, (DWORD)((wcslen(v3) + 1) * sizeof(WCHAR)));
    }
    RegCloseKey(key);
    Tx_Append(st->output, L"Wrote 3 values inside the transaction.\r\n");

    /* Outside-transaction visibility check: should be EMPTY */
    if (Tx_KeyExists(HKEY_CURRENT_USER, TX_SUBKEY)) {
        Tx_Append(st->output, L"\r\n[outside tx] key visible (already existed before).\r\n");
    } else {
        Tx_Append(st->output,
            L"\r\n[outside tx] key NOT visible — transaction isolated as expected.\r\n");
    }

    if (commit) {
        if (CommitTransaction(tx)) {
            Tx_Append(st->output, L"\r\nCommitTransaction OK.\r\n");
        } else {
            Tx_Append(st->output, L"\r\nCommitTransaction FAILED.\r\n");
        }
    } else {
        if (RollbackTransaction(tx)) {
            Tx_Append(st->output, L"\r\nRollbackTransaction OK.\r\n");
        } else {
            Tx_Append(st->output, L"\r\nRollbackTransaction FAILED.\r\n");
        }
    }

    /* Re-check outside */
    if (Tx_KeyExists(HKEY_CURRENT_USER, TX_SUBKEY)) {
        Tx_Append(st->output, L"[after] key visible.\r\n");
    } else {
        Tx_Append(st->output, L"[after] key NOT visible.\r\n");
    }

    CloseHandle(tx);
}

static LRESULT CALLBACK Tx_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    TxState *st = (TxState *)GetPropW(hwnd, TX_PROP);
    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_TX_CMT) { Tx_RunDemo(st, TRUE);  return 0; }
        if (LOWORD(wp) == ID_TX_RBK) { Tx_RunDemo(st, FALSE); return 0; }
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, TX_PROP); }
    return CallWindowProcW(g_origTxFrame, hwnd, msg, wp, lp);
}

static HWND TxReg_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    TxState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"TxReg",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (TxState *)calloc(1, sizeof(TxState));
    if (!st) { DestroyWindow(frame); return NULL; }

    CreateWindowExW(0, L"BUTTON", L"Run + COMMIT",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 140, 26, frame, (HMENU)(LONG_PTR)ID_TX_CMT, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Run + ROLLBACK",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        160, 38, 160, 26, frame, (HMENU)(LONG_PTR)ID_TX_RBK, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"Click Commit or Rollback. The demo creates a kernel TX,\r\n"
        L"opens HKCU\\Software\\MiniShell\\TxRegDemo bound to it, writes 3\r\n"
        L"values, then commits or rolls back atomically.\r\n",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_TX_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, TX_PROP, (HANDLE)st);
    if (!g_origTxFrame) g_origTxFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Tx_FrameProc);
    return frame;
}

MsApp g_AppTxReg = { L"TxReg", TxReg_Create, 720, 460 };
