/*
 * app_procmit.c — Process mitigation policies (DEP, ASLR, CFG, etc.)
 *
 * Demonstrates the SetProcessMitigationPolicy / GetProcessMitigationPolicy
 * API surface (Win 8+, kernel32). These are per-process security
 * hardening flags that the loader and kernel enforce: bottom-up ASLR,
 * heap termination on corruption, dynamic-code execution prevention,
 * font loading restriction, etc.
 *
 *   - GetProcessMitigationPolicy(GetCurrentProcess(), policyType,
 *     &policy, sizeof(policy)) reads a particular mitigation struct
 *   - SetProcessMitigationPolicy(policyType, &policy, sizeof(policy))
 *     applies it (most are one-way during process lifetime)
 *
 * Loaded dynamically because kernel32 doesn't export these on Win 7.
 * We dump the current state of every documented mitigation so the user
 * can see the hardening posture inherited from the EXE manifest.
 */

#include "shell.h"
#include <stdlib.h>
#include <stdio.h>

/* Enum is partly Win 8 / 10 / 11 — replicate just the ones we display */
typedef enum {
    MS_ProcessDEPPolicy                          = 0,
    MS_ProcessASLRPolicy                         = 1,
    MS_ProcessDynamicCodePolicy                  = 2,
    MS_ProcessStrictHandleCheckPolicy            = 3,
    MS_ProcessSystemCallDisablePolicy            = 4,
    MS_ProcessExtensionPointDisablePolicy        = 6,
    MS_ProcessControlFlowGuardPolicy             = 7,
    MS_ProcessSignaturePolicy                    = 8,
    MS_ProcessFontDisablePolicy                  = 9,
    MS_ProcessImageLoadPolicy                    = 10
} MS_PROCESS_MITIGATION_POLICY;

typedef BOOL (WINAPI *PFN_GetProcessMitigationPolicy)(HANDLE, MS_PROCESS_MITIGATION_POLICY, PVOID, SIZE_T);

#define PM_PROP   L"MS_PM_STATE"
#define ID_PM_REF 128001
#define ID_PM_OUT 128002

typedef struct {
    HWND     output, refresh;
    HMODULE  k32;
    PFN_GetProcessMitigationPolicy pGet;
} PmState;

static WNDPROC g_origPmFrame = NULL;

static void Pm_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
}

static void Pm_DumpDword(PmState *st, MS_PROCESS_MITIGATION_POLICY pol, const wchar_t *label)
{
    DWORD flags = 0;
    wchar_t line[200];
    if (!st->pGet) return;
    if (st->pGet(GetCurrentProcess(), pol, &flags, sizeof(flags))) {
        swprintf_s(line, 200, L"  %-44s 0x%08lx\r\n", label, flags);
    } else {
        swprintf_s(line, 200, L"  %-44s (failed, err %lu)\r\n", label, GetLastError());
    }
    Pm_Append(st->output, line);
}

static void Pm_Refresh(PmState *st)
{
    SetWindowTextW(st->output, L"");
    if (!st->pGet) {
        Pm_Append(st->output, L"GetProcessMitigationPolicy unavailable (pre-Win 8).\r\n");
        return;
    }
    Pm_Append(st->output, L"Mitigation policies for THIS process:\r\n\r\n");
    Pm_DumpDword(st, MS_ProcessDEPPolicy,                   L"DEP (Data Execution Prevention)");
    Pm_DumpDword(st, MS_ProcessASLRPolicy,                  L"ASLR (Address Space Layout Random.)");
    Pm_DumpDword(st, MS_ProcessDynamicCodePolicy,           L"Dynamic Code");
    Pm_DumpDword(st, MS_ProcessStrictHandleCheckPolicy,     L"Strict Handle Check");
    Pm_DumpDword(st, MS_ProcessSystemCallDisablePolicy,     L"System Call Disable");
    Pm_DumpDword(st, MS_ProcessExtensionPointDisablePolicy, L"Extension Point Disable");
    Pm_DumpDword(st, MS_ProcessControlFlowGuardPolicy,      L"Control Flow Guard");
    Pm_DumpDword(st, MS_ProcessSignaturePolicy,             L"Signature");
    Pm_DumpDword(st, MS_ProcessFontDisablePolicy,           L"Font Disable");
    Pm_DumpDword(st, MS_ProcessImageLoadPolicy,             L"Image Load");
    Pm_Append(st->output,
        L"\r\n(non-zero bits indicate the corresponding mitigation is enabled.)\r\n");
}

static LRESULT CALLBACK Pm_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    PmState *st = (PmState *)GetPropW(hwnd, PM_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_PM_REF) { Pm_Refresh(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->k32) FreeLibrary(st->k32);
        free(st); RemovePropW(hwnd, PM_PROP);
    }
    return CallWindowProcW(g_origPmFrame, hwnd, msg, wp, lp);
}

static HWND ProcMit_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    PmState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"ProcMit",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (PmState *)calloc(1, sizeof(PmState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->k32 = LoadLibraryW(L"kernel32.dll");
    if (st->k32) {
        st->pGet = (PFN_GetProcessMitigationPolicy)
                   GetProcAddress(st->k32, "GetProcessMitigationPolicy");
    }

    st->refresh = CreateWindowExW(0, L"BUTTON", L"Refresh",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 110, 26, frame, (HMENU)(LONG_PTR)ID_PM_REF, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_PM_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, PM_PROP, (HANDLE)st);
    if (!g_origPmFrame) g_origPmFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Pm_FrameProc);
    Pm_Refresh(st);
    return frame;
}

MsApp g_AppProcMit = { L"ProcMit", ProcMit_Create, 760, 460 };
