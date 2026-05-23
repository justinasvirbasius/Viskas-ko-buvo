/*
 * app_apprestart.c — Application restart and crash-recovery registration
 *
 * Demonstrates the Application Restart and Recovery API (kernel32) —
 * how applications opt into Windows' "after a crash, Vista+ will
 * restart your app" behavior, the same one Word and Excel use:
 *
 *   - RegisterApplicationRestart(L"/restart-args", flags) — tells the
 *     OS to relaunch this process with the given command line after a
 *     crash/hang/Windows-Update reboot. Flags control which scenarios
 *     trigger restart (RESTART_NO_CRASH/HANG/PATCH/REBOOT to suppress).
 *   - GetApplicationRestartSettings(process, buf, &cch, &flags) reads
 *     back what's currently registered for a given process.
 *   - UnregisterApplicationRestart() clears the registration.
 *   - RegisterApplicationRecoveryCallback(pfnCallback, ctx, pingMs, 0)
 *     registers a function to be called when WER detects the process is
 *     hanging, giving it pingMs to save state and exit gracefully.
 *   - ApplicationRecoveryInProgress(&cancelled) keeps the recovery
 *     watchdog from killing us during save.
 *   - ApplicationRecoveryFinished(TRUE) signals success.
 */

#include "shell.h"
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "kernel32.lib")

#define AR_PROP   L"MS_AR_STATE"
#define ID_AR_REG 111001
#define ID_AR_QRY 111002
#define ID_AR_UNR 111003
#define ID_AR_OUT 111004

typedef struct { HWND output; BOOL recoveryReg; } ArState;
static WNDPROC g_origArFrame = NULL;

static void Ar_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
}

static DWORD WINAPI Ar_RecoveryCallback(PVOID context)
{
    BOOL cancelled = FALSE;
    /* WER calls us when the process is hung. Ping the OS to keep the
       watchdog from killing us while we save. */
    ApplicationRecoveryInProgress(&cancelled);
    /* (Pretend we saved state) */
    ApplicationRecoveryFinished(TRUE);
    (void)context;
    return 0;
}

static void Ar_Register(ArState *st)
{
    HRESULT hr = RegisterApplicationRestart(L"/restarted-by-wer", 0);
    if (SUCCEEDED(hr)) {
        Ar_Append(st->output,
            L"RegisterApplicationRestart succeeded.\r\n"
            L"  command-line tail: \"/restarted-by-wer\"\r\n");
    } else {
        wchar_t buf[80];
        swprintf_s(buf, 80, L"RegisterApplicationRestart failed: 0x%08lx\r\n", hr);
        Ar_Append(st->output, buf);
    }

    if (!st->recoveryReg) {
        hr = RegisterApplicationRecoveryCallback(Ar_RecoveryCallback, NULL, 5000, 0);
        if (SUCCEEDED(hr)) {
            st->recoveryReg = TRUE;
            Ar_Append(st->output,
                L"RegisterApplicationRecoveryCallback succeeded "
                L"(5-second WER ping).\r\n");
        } else {
            wchar_t buf[80];
            swprintf_s(buf, 80, L"  ...recovery callback failed: 0x%08lx\r\n", hr);
            Ar_Append(st->output, buf);
        }
    }
}

static void Ar_Query(ArState *st)
{
    wchar_t buf[1024] = L"";
    DWORD   cch = 1024;
    DWORD   flags = 0;
    HRESULT hr = GetApplicationRestartSettings(GetCurrentProcess(), buf, &cch, &flags);
    if (hr == S_OK) {
        wchar_t line[1100];
        swprintf_s(line, 1100,
            L"GetApplicationRestartSettings:\r\n"
            L"  command-line tail: \"%s\"\r\n"
            L"  flags            : 0x%08lx\r\n",
            buf, flags);
        Ar_Append(st->output, line);
    } else if (hr == S_FALSE) {
        Ar_Append(st->output, L"GetApplicationRestartSettings: not registered.\r\n");
    } else {
        wchar_t line[80];
        swprintf_s(line, 80, L"GetApplicationRestartSettings: 0x%08lx\r\n", hr);
        Ar_Append(st->output, line);
    }
}

static void Ar_Unregister(ArState *st)
{
    HRESULT hr = UnregisterApplicationRestart();
    if (SUCCEEDED(hr)) {
        Ar_Append(st->output, L"UnregisterApplicationRestart succeeded.\r\n");
    } else {
        wchar_t buf[80];
        swprintf_s(buf, 80, L"UnregisterApplicationRestart: 0x%08lx\r\n", hr);
        Ar_Append(st->output, buf);
    }
    if (st->recoveryReg) {
        UnregisterApplicationRecoveryCallback();
        st->recoveryReg = FALSE;
        Ar_Append(st->output, L"UnregisterApplicationRecoveryCallback called.\r\n");
    }
}

static LRESULT CALLBACK Ar_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    ArState *st = (ArState *)GetPropW(hwnd, AR_PROP);
    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_AR_REG) { Ar_Register(st);   return 0; }
        if (LOWORD(wp) == ID_AR_QRY) { Ar_Query(st);      return 0; }
        if (LOWORD(wp) == ID_AR_UNR) { Ar_Unregister(st); return 0; }
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->recoveryReg) UnregisterApplicationRecoveryCallback();
        UnregisterApplicationRestart();
        free(st); RemovePropW(hwnd, AR_PROP);
    }
    return CallWindowProcW(g_origArFrame, hwnd, msg, wp, lp);
}

static HWND AppRestart_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    ArState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"AppRestart",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (ArState *)calloc(1, sizeof(ArState));
    if (!st) { DestroyWindow(frame); return NULL; }

    CreateWindowExW(0, L"BUTTON", L"Register",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 110, 26, frame, (HMENU)(LONG_PTR)ID_AR_REG, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Query",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        130, 38, 100, 26, frame, (HMENU)(LONG_PTR)ID_AR_QRY, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Unregister",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        238, 38, 110, 26, frame, (HMENU)(LONG_PTR)ID_AR_UNR, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"Click Register, then Query to see current setting.\r\n"
        L"After a crash, Windows would relaunch with /restarted-by-wer.\r\n\r\n",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_AR_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, AR_PROP, (HANDLE)st);
    if (!g_origArFrame) g_origArFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Ar_FrameProc);
    return frame;
}

MsApp g_AppAppRestart = { L"AppRestart", AppRestart_Create, 720, 380 };
