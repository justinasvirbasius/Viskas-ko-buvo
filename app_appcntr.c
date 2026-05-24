/*
 * app_appcntr.c — AppContainer profile creation
 *
 * Demonstrates the AppContainer sandboxing API (userenv.dll) — the same
 * isolation primitive that backs UWP/Store apps, Edge content processes,
 * and many security sandboxes:
 *
 *   - CreateAppContainerProfile(name, displayName, description,
 *     capabilities[], capabilityCount, &packageSid)
 *     creates a per-user profile under
 *     %LOCALAPPDATA%\Packages\<name>\ and returns the package SID
 *     (form: S-1-15-2-...)
 *   - DeriveAppContainerSidFromAppContainerName(name, &sid) re-derives
 *     the SID without creating the profile
 *   - DeleteAppContainerProfile(name) removes it
 *
 * Each capability is a SID_AND_ATTRIBUTES of a "well-known capability"
 * (e.g. SECURITY_CAPABILITY_INTERNET_CLIENT, SECURITY_CAPABILITY_PRIVATE_NETWORK_CLIENT_SERVER).
 * Capabilities granted to a token created from the package SID give it
 * exactly that resource access and nothing more.
 *
 * Loaded dynamically because the exports require Win 8+ and userenv.h
 * variations.
 */

#include "shell.h"
#include <sddl.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "advapi32.lib")

typedef HRESULT (WINAPI *PFN_CreateAppContainerProfile)(
    PCWSTR pszAppContainerName, PCWSTR pszDisplayName, PCWSTR pszDescription,
    PSID_AND_ATTRIBUTES pCapabilities, DWORD dwCapabilityCount, PSID *ppSidAppContainerSid);

typedef HRESULT (WINAPI *PFN_DeleteAppContainerProfile)(PCWSTR pszAppContainerName);

typedef HRESULT (WINAPI *PFN_DeriveAppContainerSidFromAppContainerName)(
    PCWSTR pszAppContainerName, PSID *ppSidAppContainerSid);

#define AC_PROP   L"MS_AC_STATE"
#define ID_AC_GO  120001
#define ID_AC_DEL 120002
#define ID_AC_OUT 120003

typedef struct {
    HWND     output;
    HMODULE  userenv;
    PFN_CreateAppContainerProfile pCreate;
    PFN_DeleteAppContainerProfile pDelete;
    PFN_DeriveAppContainerSidFromAppContainerName pDerive;
} AcState;

static const wchar_t *AC_NAME = L"MiniShell.Demo.AppContainer";

static WNDPROC g_origAcFrame = NULL;

static void Ac_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
}

static void Ac_PrintSid(AcState *st, PSID sid, const wchar_t *label)
{
    LPWSTR str = NULL;
    if (ConvertSidToStringSidW(sid, &str) && str) {
        wchar_t line[400];
        swprintf_s(line, 400, L"  %s: %s\r\n", label, str);
        Ac_Append(st->output, line);
        LocalFree(str);
    }
}

static void Ac_Create(AcState *st)
{
    PSID sid = NULL;
    HRESULT hr;

    SetWindowTextW(st->output, L"");
    if (!st->pCreate || !st->pDerive) {
        Ac_Append(st->output, L"userenv.dll AppContainer exports unavailable.\r\n");
        return;
    }

    /* No capabilities for the demo — minimal sandbox. Production code
       would build SID_AND_ATTRIBUTES from well-known caps. */
    hr = st->pCreate(AC_NAME,
                     L"MiniShell Demo Container",
                     L"Created by MiniShell to exercise the AppContainer API",
                     NULL, 0, &sid);
    if (SUCCEEDED(hr) && sid) {
        Ac_Append(st->output, L"CreateAppContainerProfile succeeded.\r\n");
        Ac_PrintSid(st, sid, L"package SID");
        FreeSid(sid);
    } else if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
        Ac_Append(st->output, L"Profile already exists (re-deriving SID).\r\n");
        if (SUCCEEDED(st->pDerive(AC_NAME, &sid)) && sid) {
            Ac_PrintSid(st, sid, L"derived SID");
            FreeSid(sid);
        }
    } else {
        wchar_t buf[80];
        swprintf_s(buf, 80, L"CreateAppContainerProfile failed: 0x%08lx\r\n", hr);
        Ac_Append(st->output, buf);
    }

    /* Show where the OS would have provisioned its files */
    {
        wchar_t path[MAX_PATH];
        wchar_t line[MAX_PATH + 80];
        ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\Packages\\MiniShell.Demo.AppContainer",
                                   path, MAX_PATH);
        swprintf_s(line, MAX_PATH + 80, L"\r\nProfile path:\r\n  %s\r\n", path);
        Ac_Append(st->output, line);
    }
}

static void Ac_DeleteProfile(AcState *st)
{
    HRESULT hr;
    if (!st->pDelete) {
        Ac_Append(st->output, L"DeleteAppContainerProfile unavailable.\r\n");
        return;
    }
    hr = st->pDelete(AC_NAME);
    if (SUCCEEDED(hr)) {
        Ac_Append(st->output, L"\r\nDeleteAppContainerProfile succeeded.\r\n");
    } else {
        wchar_t buf[80];
        swprintf_s(buf, 80, L"\r\nDeleteAppContainerProfile: 0x%08lx\r\n", hr);
        Ac_Append(st->output, buf);
    }
}

static LRESULT CALLBACK Ac_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    AcState *st = (AcState *)GetPropW(hwnd, AC_PROP);
    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_AC_GO)  { Ac_Create(st); return 0; }
        if (LOWORD(wp) == ID_AC_DEL) { Ac_DeleteProfile(st); return 0; }
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->userenv) FreeLibrary(st->userenv);
        free(st); RemovePropW(hwnd, AC_PROP);
    }
    return CallWindowProcW(g_origAcFrame, hwnd, msg, wp, lp);
}

static HWND AppCntr_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    AcState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"AppCntr",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (AcState *)calloc(1, sizeof(AcState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->userenv = LoadLibraryW(L"userenv.dll");
    if (st->userenv) {
        st->pCreate = (PFN_CreateAppContainerProfile)
                      GetProcAddress(st->userenv, "CreateAppContainerProfile");
        st->pDelete = (PFN_DeleteAppContainerProfile)
                      GetProcAddress(st->userenv, "DeleteAppContainerProfile");
        st->pDerive = (PFN_DeriveAppContainerSidFromAppContainerName)
                      GetProcAddress(st->userenv, "DeriveAppContainerSidFromAppContainerName");
    }

    CreateWindowExW(0, L"BUTTON", L"Create profile",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 140, 26, frame, (HMENU)(LONG_PTR)ID_AC_GO, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Delete",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        160, 38, 100, 26, frame, (HMENU)(LONG_PTR)ID_AC_DEL, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"Creates an AppContainer profile named MiniShell.Demo.AppContainer.\r\n"
        L"Shows the derived package SID (S-1-15-2-...).\r\n",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_AC_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, AC_PROP, (HANDLE)st);
    if (!g_origAcFrame) g_origAcFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Ac_FrameProc);
    return frame;
}

MsApp g_AppAppCntr = { L"AppCntr", AppCntr_Create, 760, 440 };
