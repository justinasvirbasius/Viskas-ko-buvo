/*
 * app_sehprobe.c — Structured Exception Handling demonstration
 *
 * Demonstrates SEH and unhandled-exception filters:
 *   - __try / __except (EXCEPTION_EXECUTE_HANDLER) to catch and continue
 *   - GetExceptionCode and EXCEPTION_POINTERS to inspect what was thrown
 *   - SetUnhandledExceptionFilter installing a last-chance LPTOP_LEVEL_FILTER
 *     for exceptions that escape all __except frames
 *   - Friendly mapping of NTSTATUS exception codes to names (access violation,
 *     int divide by zero, illegal instruction, stack overflow, etc.)
 *
 * The probe buttons trigger different exceptions safely inside __try blocks
 * so the app stays alive. The output edit shows each catch.
 *
 * SEH is MSVC-specific. With MinGW, __try/__except compiles via SEH only
 * since gcc 4.8+ targeting x64; this builds clean on both toolchains we use.
 */

#include "shell.h"
#include <stdlib.h>
#include <stdio.h>
#include <excpt.h>

#define SH_PROP    L"MS_SH_STATE"
#define ID_SH_OUT  72001
#define ID_SH_AV   72002
#define ID_SH_DZ   72003
#define ID_SH_PR   72004
#define ID_SH_BP   72005
#define ID_SH_DBG  72006

typedef struct { HWND output; } ShState;
static WNDPROC g_origShFrame = NULL;

static const wchar_t *Sh_ExceptionName(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:       return L"ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:  return L"ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT:             return L"BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT:  return L"DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DENORMAL_OPERAND:   return L"FLT_DENORMAL_OPERAND";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:     return L"FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INEXACT_RESULT:     return L"FLT_INEXACT_RESULT";
    case EXCEPTION_FLT_INVALID_OPERATION:  return L"FLT_INVALID_OPERATION";
    case EXCEPTION_FLT_OVERFLOW:           return L"FLT_OVERFLOW";
    case EXCEPTION_FLT_STACK_CHECK:        return L"FLT_STACK_CHECK";
    case EXCEPTION_FLT_UNDERFLOW:          return L"FLT_UNDERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:    return L"ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:          return L"IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:     return L"INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW:           return L"INT_OVERFLOW";
    case EXCEPTION_INVALID_DISPOSITION:    return L"INVALID_DISPOSITION";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:return L"NONCONTINUABLE_EXCEPTION";
    case EXCEPTION_PRIV_INSTRUCTION:       return L"PRIV_INSTRUCTION";
    case EXCEPTION_SINGLE_STEP:            return L"SINGLE_STEP";
    case EXCEPTION_STACK_OVERFLOW:         return L"STACK_OVERFLOW";
    }
    return L"<unknown>";
}

static void Sh_Append(ShState *st, const wchar_t *t)
{
    int len = GetWindowTextLengthW(st->output);
    SendMessageW(st->output, EM_SETSEL, len, len);
    SendMessageW(st->output, EM_REPLACESEL, FALSE, (LPARAM)t);
    SendMessageW(st->output, EM_SCROLLCARET, 0, 0);
}

static void Sh_TriggerAV(ShState *st)
{
    DWORD code = 0;
    void *addr = NULL;
    __try {
        volatile int *p = (volatile int *)0x42;
        *p = 0;
    } __except (code = GetExceptionCode(),
                addr = GetExceptionInformation()->ExceptionRecord->ExceptionAddress,
                EXCEPTION_EXECUTE_HANDLER) {
        wchar_t line[160];
        swprintf_s(line, 160,
            L"Caught %s (0x%08lx) at pc=0x%p\r\n",
            Sh_ExceptionName(code), code, addr);
        Sh_Append(st, line);
    }
}

static void Sh_TriggerDiv0(ShState *st)
{
    DWORD code = 0;
    __try {
        volatile int z = 0;
        volatile int x = 1 / z;
        (void)x;
    } __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        wchar_t line[160];
        swprintf_s(line, 160,
            L"Caught %s (0x%08lx)\r\n",
            Sh_ExceptionName(code), code);
        Sh_Append(st, line);
    }
}

static void Sh_TriggerPriv(ShState *st)
{
    DWORD code = 0;
    __try {
        RaiseException(0xC0FFEE01, 0, 0, NULL);
    } __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        wchar_t line[160];
        swprintf_s(line, 160,
            L"Caught synthetic exception (code=0x%08lx)\r\n", code);
        Sh_Append(st, line);
    }
}

static void Sh_TriggerBreak(ShState *st)
{
    DWORD code = 0;
    __try {
        DebugBreak();
    } __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        wchar_t line[160];
        swprintf_s(line, 160,
            L"Caught %s (0x%08lx)\r\n",
            Sh_ExceptionName(code), code);
        Sh_Append(st, line);
    }
}

static LONG WINAPI Sh_TopLevelFilter(EXCEPTION_POINTERS *ep)
{
    /* This wouldn't normally have access to the UI; for the demo we just
       report that we *would* run if an unhandled exception escaped. */
    wchar_t buf[200];
    swprintf_s(buf, 200,
        L"<unhandled-filter would run for code 0x%08lx>",
        ep->ExceptionRecord->ExceptionCode);
    MessageBoxW(NULL, buf, L"SehProbe top-level filter", MB_OK | MB_ICONINFORMATION);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void Sh_InstallFilter(ShState *st)
{
    LPTOP_LEVEL_EXCEPTION_FILTER prev =
        SetUnhandledExceptionFilter(Sh_TopLevelFilter);
    wchar_t line[200];
    swprintf_s(line, 200,
        L"SetUnhandledExceptionFilter installed (prev=%p).\r\n"
        L"  This handler runs only for exceptions that escape ALL __except frames.\r\n",
        (void *)prev);
    Sh_Append(st, line);
}

static LRESULT CALLBACK Sh_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    ShState *st = (ShState *)GetPropW(hwnd, SH_PROP);
    if (msg == WM_COMMAND && st) {
        switch (LOWORD(wp)) {
        case ID_SH_AV:  Sh_TriggerAV(st);    return 0;
        case ID_SH_DZ:  Sh_TriggerDiv0(st);  return 0;
        case ID_SH_PR:  Sh_TriggerPriv(st);  return 0;
        case ID_SH_BP:  Sh_TriggerBreak(st); return 0;
        case ID_SH_DBG: Sh_InstallFilter(st);return 0;
        }
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 110, w - 16, h - 118, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, SH_PROP); }
    return CallWindowProcW(g_origShFrame, hwnd, msg, wp, lp);
}

static HWND SehProbe_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    ShState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"SehProbe",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (ShState *)calloc(1, sizeof(ShState));
    if (!st) { DestroyWindow(frame); return NULL; }

    CreateWindowExW(0, L"STATIC",
        L"Each button triggers an exception inside __try / __except so the app\n"
        L"continues running. The top-level filter is what handles exceptions\n"
        L"that escape every guarded frame.",
        WS_CHILD | WS_VISIBLE,
        12, 30, w - 24, 50, frame, NULL, hInstance, NULL);

    CreateWindowExW(0, L"BUTTON", L"AV",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 80, 80, 24, frame, (HMENU)(LONG_PTR)ID_SH_AV, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Int Div/0",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        100, 80, 90, 24, frame, (HMENU)(LONG_PTR)ID_SH_DZ, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Synthetic",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        198, 80, 90, 24, frame, (HMENU)(LONG_PTR)ID_SH_PR, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"DebugBreak",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        296, 80, 100, 24, frame, (HMENU)(LONG_PTR)ID_SH_BP, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Install filter",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        404, 80, 120, 24, frame, (HMENU)(LONG_PTR)ID_SH_DBG, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 110, w - 16, h - 118, frame, (HMENU)(LONG_PTR)ID_SH_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, SH_PROP, (HANDLE)st);
    if (!g_origShFrame) g_origShFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Sh_FrameProc);
    return frame;
}

MsApp g_AppSehProbe = { L"SehProbe", SehProbe_Create, 620, 420 };
