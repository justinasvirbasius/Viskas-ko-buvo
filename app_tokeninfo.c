/*
 * app_tokeninfo.c — Inspect the current process's access token
 *
 * Demonstrates the security token APIs:
 *   - OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &h)
 *   - GetTokenInformation(h, TokenUser, ...) for the user SID
 *   - GetTokenInformation(h, TokenIntegrityLevel, ...) reads the integrity SID;
 *     the last sub-authority is the integrity level (LOW/MEDIUM/HIGH/SYSTEM)
 *   - GetTokenInformation(h, TokenGroups, ...) walks SID_AND_ATTRIBUTES[]
 *   - GetTokenInformation(h, TokenPrivileges, ...) walks LUID_AND_ATTRIBUTES[]
 *     with LookupPrivilegeNameW + LookupPrivilegeDisplayNameW
 *   - ConvertSidToStringSidW for printable SID; LookupAccountSidW for friendly names
 *
 * One-shot inspector; output to a read-only edit. Refresh re-queries the
 * token (useful if the user elevates via a separate process).
 */

#include "shell.h"
#include <sddl.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "advapi32.lib")

#define TI_PROP    L"MS_TI_STATE"
#define ID_TI_OUT  64001
#define ID_TI_REF  64002

typedef struct { HWND output, refBtn; } TiState;

static WNDPROC g_origTiFrame = NULL;

static void Ti_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
}

static const wchar_t *Ti_IntegrityName(DWORD rid)
{
    if (rid <  SECURITY_MANDATORY_LOW_RID)     return L"Untrusted";
    if (rid <  SECURITY_MANDATORY_MEDIUM_RID)  return L"Low";
    if (rid <  SECURITY_MANDATORY_HIGH_RID)    return L"Medium";
    if (rid <  SECURITY_MANDATORY_SYSTEM_RID)  return L"High";
    if (rid <  SECURITY_MANDATORY_PROTECTED_PROCESS_RID) return L"System";
    return L"Protected";
}

static void Ti_PrintSid(TiState *st, PSID sid, const wchar_t *prefix)
{
    wchar_t *sidStr = NULL;
    wchar_t name[256] = L"", dom[256] = L"";
    DWORD nameLen = 256, domLen = 256;
    SID_NAME_USE use;
    wchar_t line[800];

    ConvertSidToStringSidW(sid, &sidStr);
    if (LookupAccountSidW(NULL, sid, name, &nameLen, dom, &domLen, &use)) {
        swprintf_s(line, 800, L"%s%s\\%s   [%s]\r\n",
            prefix, dom[0] ? dom : L"", name, sidStr ? sidStr : L"");
    } else {
        swprintf_s(line, 800, L"%s%s\r\n", prefix, sidStr ? sidStr : L"<sid>");
    }
    Ti_Append(st->output, line);
    if (sidStr) LocalFree(sidStr);
}

static void Ti_Refresh(TiState *st)
{
    HANDLE tok = NULL;
    DWORD cb = 0;

    SetWindowTextW(st->output, L"");

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        Ti_Append(st->output, L"OpenProcessToken failed.\r\n");
        return;
    }

    /* TokenUser — owner SID */
    Ti_Append(st->output, L"== User ==\r\n");
    GetTokenInformation(tok, TokenUser, NULL, 0, &cb);
    if (cb) {
        TOKEN_USER *tu = (TOKEN_USER *)malloc(cb);
        if (GetTokenInformation(tok, TokenUser, tu, cb, &cb)) {
            Ti_PrintSid(st, tu->User.Sid, L"  ");
        }
        free(tu);
    }

    /* TokenIntegrityLevel */
    Ti_Append(st->output, L"\r\n== Integrity ==\r\n");
    cb = 0;
    GetTokenInformation(tok, TokenIntegrityLevel, NULL, 0, &cb);
    if (cb) {
        TOKEN_MANDATORY_LABEL *tml = (TOKEN_MANDATORY_LABEL *)malloc(cb);
        if (GetTokenInformation(tok, TokenIntegrityLevel, tml, cb, &cb)) {
            DWORD subCount = *GetSidSubAuthorityCount(tml->Label.Sid);
            DWORD rid = *GetSidSubAuthority(tml->Label.Sid, subCount - 1);
            wchar_t line[80];
            swprintf_s(line, 80, L"  level: %s  (rid 0x%lx)\r\n",
                       Ti_IntegrityName(rid), rid);
            Ti_Append(st->output, line);
        }
        free(tml);
    }

    /* TokenElevation */
    Ti_Append(st->output, L"\r\n== Elevation ==\r\n");
    {
        TOKEN_ELEVATION elev = { 0 };
        TOKEN_ELEVATION_TYPE elevType = TokenElevationTypeDefault;
        DWORD got = 0;
        if (GetTokenInformation(tok, TokenElevation, &elev, sizeof(elev), &got)) {
            Ti_Append(st->output,
                elev.TokenIsElevated ? L"  elevated: YES\r\n"
                                     : L"  elevated: no\r\n");
        }
        if (GetTokenInformation(tok, TokenElevationType, &elevType,
                                 sizeof(elevType), &got)) {
            wchar_t buf[64];
            const wchar_t *kind =
                elevType == TokenElevationTypeDefault ? L"default" :
                elevType == TokenElevationTypeFull    ? L"full" :
                                                        L"limited";
            swprintf_s(buf, 64, L"  type:     %s\r\n", kind);
            Ti_Append(st->output, buf);
        }
    }

    /* TokenGroups */
    Ti_Append(st->output, L"\r\n== Groups (first 12) ==\r\n");
    cb = 0;
    GetTokenInformation(tok, TokenGroups, NULL, 0, &cb);
    if (cb) {
        TOKEN_GROUPS *tg = (TOKEN_GROUPS *)malloc(cb);
        if (GetTokenInformation(tok, TokenGroups, tg, cb, &cb)) {
            DWORD i, n = tg->GroupCount > 12 ? 12 : tg->GroupCount;
            for (i = 0; i < n; ++i) {
                Ti_PrintSid(st, tg->Groups[i].Sid, L"  ");
            }
            if (tg->GroupCount > 12) {
                wchar_t buf[64];
                swprintf_s(buf, 64, L"  ... and %lu more\r\n",
                           tg->GroupCount - 12);
                Ti_Append(st->output, buf);
            }
        }
        free(tg);
    }

    /* TokenPrivileges */
    Ti_Append(st->output, L"\r\n== Privileges ==\r\n");
    cb = 0;
    GetTokenInformation(tok, TokenPrivileges, NULL, 0, &cb);
    if (cb) {
        TOKEN_PRIVILEGES *tp = (TOKEN_PRIVILEGES *)malloc(cb);
        if (GetTokenInformation(tok, TokenPrivileges, tp, cb, &cb)) {
            DWORD i;
            for (i = 0; i < tp->PrivilegeCount; ++i) {
                wchar_t name[80] = L"", line[200];
                DWORD nameLen = 80;
                BOOL enabled =
                    (tp->Privileges[i].Attributes &
                     (SE_PRIVILEGE_ENABLED | SE_PRIVILEGE_ENABLED_BY_DEFAULT)) != 0;
                LookupPrivilegeNameW(NULL, &tp->Privileges[i].Luid,
                                      name, &nameLen);
                swprintf_s(line, 200, L"  %s %s\r\n",
                           enabled ? L"[ON ]" : L"[off]", name);
                Ti_Append(st->output, line);
            }
        }
        free(tp);
    }

    CloseHandle(tok);
}

static LRESULT CALLBACK Ti_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    TiState *st = (TiState *)GetPropW(hwnd, TI_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_TI_REF) { Ti_Refresh(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->refBtn, 8, 34, 100, 24, TRUE);
        MoveWindow(st->output, 8, 64, w - 16, h - 72, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, TI_PROP); }
    return CallWindowProcW(g_origTiFrame, hwnd, msg, wp, lp);
}

static HWND TokenInfo_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    TiState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"TokenInfo",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (TiState *)calloc(1, sizeof(TiState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->refBtn = CreateWindowExW(0, L"BUTTON", L"Refresh",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        8, 34, 100, 24, frame, (HMENU)(LONG_PTR)ID_TI_REF, hInstance, NULL);
    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 64, w - 16, h - 72, frame, (HMENU)(LONG_PTR)ID_TI_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, TI_PROP, (HANDLE)st);
    if (!g_origTiFrame) g_origTiFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Ti_FrameProc);
    Ti_Refresh(st);
    return frame;
}

MsApp g_AppTokenInfo = { L"TokenInfo", TokenInfo_Create, 640, 500 };
