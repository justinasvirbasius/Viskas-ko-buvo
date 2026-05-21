/*
 * app_netinfo.c — Network and user identity info
 *
 * Demonstrates a small bouquet of identity / network APIs:
 *   - GetUserNameExW from secur32.dll (NameSamCompatible, NameDisplay, etc.)
 *   - GetComputerNameExW with multiple ComputerNameFormat values
 *   - NetWkstaGetInfo from netapi32 (workstation info: domain, version, platform)
 *
 * Output displayed in a read-only edit. No outbound network traffic — these
 * are local LSA / SAM lookups.
 */

#include "shell.h"
#define SECURITY_WIN32
#include <security.h>
#include <secext.h>
#include <lm.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "netapi32.lib")

#define NI_PROP   L"MS_NI_STATE"
#define ID_NI_OUT 43001
#define ID_NI_REF 43002

typedef struct {
    HWND output, refBtn;
} NiState;

static WNDPROC g_origNiFrame = NULL;

static void Ni_AppendName(NiState *st, const wchar_t *label,
                          EXTENDED_NAME_FORMAT format)
{
    wchar_t buf[300];
    ULONG cb = 300;
    wchar_t line[400];

    if (GetUserNameExW(format, buf, &cb)) {
        swprintf_s(line, 400, L"%s: %s\r\n", label, buf);
    } else {
        swprintf_s(line, 400, L"%s: <unavailable>\r\n", label);
    }
    {
        int len = GetWindowTextLengthW(st->output);
        SendMessageW(st->output, EM_SETSEL, len, len);
        SendMessageW(st->output, EM_REPLACESEL, FALSE, (LPARAM)line);
    }
}

static void Ni_AppendComputer(NiState *st, const wchar_t *label,
                              COMPUTER_NAME_FORMAT fmt)
{
    wchar_t buf[300];
    DWORD cb = 300;
    wchar_t line[400];

    if (GetComputerNameExW(fmt, buf, &cb)) {
        swprintf_s(line, 400, L"%s: %s\r\n", label, buf);
    } else {
        swprintf_s(line, 400, L"%s: <unavailable>\r\n", label);
    }
    {
        int len = GetWindowTextLengthW(st->output);
        SendMessageW(st->output, EM_SETSEL, len, len);
        SendMessageW(st->output, EM_REPLACESEL, FALSE, (LPARAM)line);
    }
}

static void Ni_Refresh(NiState *st)
{
    WKSTA_INFO_102 *info = NULL;
    NET_API_STATUS status;
    wchar_t line[400];

    SetWindowTextW(st->output, L"");

    Ni_AppendName(st, L"User (Sam)",     NameSamCompatible);
    Ni_AppendName(st, L"User (Display)", NameDisplay);
    Ni_AppendName(st, L"User (UPN)",     NameUserPrincipal);

    Ni_AppendComputer(st, L"Computer (NetBIOS)",   ComputerNameNetBIOS);
    Ni_AppendComputer(st, L"Computer (DNS host)",  ComputerNameDnsHostname);
    Ni_AppendComputer(st, L"Computer (DNS domain)", ComputerNameDnsDomain);
    Ni_AppendComputer(st, L"Computer (DNS FQDN)",  ComputerNameDnsFullyQualified);

    status = NetWkstaGetInfo(NULL, 102, (LPBYTE *)&info);
    if (status == NERR_Success && info) {
        swprintf_s(line, 400,
            L"\r\nNetWkstaGetInfo (level 102):\r\n"
            L"  computer       : %s\r\n"
            L"  domain         : %s\r\n"
            L"  Windows version: %lu.%lu\r\n"
            L"  platform ID    : %lu\r\n"
            L"  logged-on users: %lu\r\n",
            info->wki102_computername ? info->wki102_computername : L"",
            info->wki102_langroup     ? info->wki102_langroup     : L"",
            info->wki102_ver_major, info->wki102_ver_minor,
            info->wki102_platform_id,
            info->wki102_logged_on_users);
        {
            int len = GetWindowTextLengthW(st->output);
            SendMessageW(st->output, EM_SETSEL, len, len);
            SendMessageW(st->output, EM_REPLACESEL, FALSE, (LPARAM)line);
        }
        NetApiBufferFree(info);
    }
}

static LRESULT CALLBACK Ni_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    NiState *st = (NiState *)GetPropW(hwnd, NI_PROP);

    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_NI_REF) {
        Ni_Refresh(st);
        return 0;
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->refBtn, 8, 34, 100, 24, TRUE);
        MoveWindow(st->output, 8, 64, w - 16, h - 72, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, NI_PROP); }
    return CallWindowProcW(g_origNiFrame, hwnd, msg, wp, lp);
}

static HWND NetInfo_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    NiState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"NetInfo",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (NiState *)calloc(1, sizeof(NiState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->refBtn = CreateWindowExW(0, L"BUTTON", L"Refresh",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        8, 34, 100, 24, frame, (HMENU)(LONG_PTR)ID_NI_REF, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 64, w - 16, h - 72, frame, (HMENU)(LONG_PTR)ID_NI_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, NI_PROP, (HANDLE)st);
    if (!g_origNiFrame) g_origNiFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Ni_FrameProc);

    Ni_Refresh(st);
    return frame;
}

MsApp g_AppNetInfo = {
    L"NetInfo",
    NetInfo_Create,
    560, 380
};
