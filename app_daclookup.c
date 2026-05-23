/*
 * app_daclookup.c — File DACL enumeration via GetSecurityInfo
 *
 * Demonstrates Windows access-control list inspection — what NTFS uses
 * to decide who can read/write/execute a file:
 *   - GetNamedSecurityInfoW(path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
 *                            NULL, NULL, &dacl, NULL, &sd) reads the
 *     security descriptor and discretionary ACL of a filesystem object
 *   - The DACL is an ACL structure; ACE entries are walked with
 *     GetAclInformation + GetAce(dacl, index, &ace)
 *   - Each ACE_HEADER has AceType (ACCESS_ALLOWED_ACE_TYPE / DENIED_ACE_TYPE)
 *   - For an ACCESS_ALLOWED_ACE, the SidStart field is the trustee SID
 *   - LookupAccountSidW(NULL, sid, name, &nameLen, domain, &domLen, &use)
 *     resolves the SID to "DOMAIN\username"
 *   - LocalFree the security descriptor on shutdown
 *
 * We dump every ACE with: type, mask in hex, and the resolved principal.
 */

#include "shell.h"
#include <aclapi.h>
#include <sddl.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "advapi32.lib")

#define DL_PROP    L"MS_DL_STATE"
#define ID_DL_PATH 108001
#define ID_DL_BR   108002
#define ID_DL_GO   108003
#define ID_DL_OUT  108004

typedef struct { HWND pathEdit, browseBtn, goBtn, output; } DlState;
static WNDPROC g_origDlFrame = NULL;

static void Dl_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
}

static void Dl_FormatMask(DWORD m, wchar_t *out, int cch)
{
    out[0] = 0;
    if (m & GENERIC_READ)    wcscat_s(out, cch, L"GR ");
    if (m & GENERIC_WRITE)   wcscat_s(out, cch, L"GW ");
    if (m & GENERIC_EXECUTE) wcscat_s(out, cch, L"GX ");
    if (m & GENERIC_ALL)     wcscat_s(out, cch, L"GA ");
    if (m & FILE_READ_DATA)  wcscat_s(out, cch, L"FR ");
    if (m & FILE_WRITE_DATA) wcscat_s(out, cch, L"FW ");
    if (m & FILE_APPEND_DATA)wcscat_s(out, cch, L"FA ");
    if (m & FILE_EXECUTE)    wcscat_s(out, cch, L"FX ");
    if (m & DELETE)          wcscat_s(out, cch, L"D ");
    if (m & READ_CONTROL)    wcscat_s(out, cch, L"RC ");
    if (m & WRITE_DAC)       wcscat_s(out, cch, L"WD ");
    if (m & WRITE_OWNER)     wcscat_s(out, cch, L"WO ");
    if (m & SYNCHRONIZE)     wcscat_s(out, cch, L"S ");
    if (!out[0]) wcscpy_s(out, cch, L"(none)");
}

static const wchar_t *Dl_AceTypeName(BYTE t)
{
    switch (t) {
    case ACCESS_ALLOWED_ACE_TYPE:        return L"ALLOW";
    case ACCESS_DENIED_ACE_TYPE:         return L"DENY";
    case SYSTEM_AUDIT_ACE_TYPE:          return L"AUDIT";
    case ACCESS_ALLOWED_OBJECT_ACE_TYPE: return L"ALLOW-OBJ";
    case ACCESS_DENIED_OBJECT_ACE_TYPE:  return L"DENY-OBJ";
    }
    return L"?";
}

static void Dl_Lookup(DlState *st)
{
    wchar_t path[MAX_PATH];
    PACL    dacl = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    DWORD   r;

    SetWindowTextW(st->output, L"");
    GetWindowTextW(st->pathEdit, path, MAX_PATH);
    if (!path[0]) return;

    r = GetNamedSecurityInfoW(path, SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION,
            NULL, NULL, &dacl, NULL, &sd);
    if (r != ERROR_SUCCESS) {
        wchar_t buf[200];
        swprintf_s(buf, 200, L"GetNamedSecurityInfo failed: %lu\r\n", r);
        Dl_Append(st->output, buf);
        return;
    }

    /* Owner */
    {
        PSID ownerSid = NULL;
        BOOL ownerDef;
        if (GetSecurityDescriptorOwner(sd, &ownerSid, &ownerDef) && ownerSid) {
            wchar_t name[256] = L"", domain[256] = L"", sidStr[200] = L"";
            DWORD nl = 256, dl = 256;
            SID_NAME_USE use;
            LPWSTR sidText = NULL;
            if (ConvertSidToStringSidW(ownerSid, &sidText) && sidText) {
                wcscpy_s(sidStr, 200, sidText);
                LocalFree(sidText);
            }
            LookupAccountSidW(NULL, ownerSid, name, &nl, domain, &dl, &use);
            {
                wchar_t line[600];
                swprintf_s(line, 600, L"Owner: %s\\%s\r\n   SID: %s\r\n\r\n",
                           domain, name, sidStr);
                Dl_Append(st->output, line);
            }
        }
    }

    if (!dacl) {
        Dl_Append(st->output, L"No DACL (everyone has full access).\r\n");
        if (sd) LocalFree(sd);
        return;
    }

    {
        ACL_SIZE_INFORMATION sz;
        if (GetAclInformation(dacl, &sz, sizeof(sz), AclSizeInformation)) {
            wchar_t hdr[80];
            DWORD i;
            swprintf_s(hdr, 80, L"DACL has %lu ACE(s):\r\n", sz.AceCount);
            Dl_Append(st->output, hdr);
            for (i = 0; i < sz.AceCount; ++i) {
                void *acePtr = NULL;
                if (GetAce(dacl, i, &acePtr) && acePtr) {
                    ACCESS_ALLOWED_ACE *ace = (ACCESS_ALLOWED_ACE *)acePtr;
                    BYTE type = ace->Header.AceType;
                    DWORD mask = ace->Mask;
                    PSID sid = (PSID)&ace->SidStart;
                    wchar_t name[256] = L"", domain[256] = L"", maskStr[100];
                    DWORD nl = 256, dl = 256;
                    SID_NAME_USE use;
                    LookupAccountSidW(NULL, sid, name, &nl, domain, &dl, &use);
                    Dl_FormatMask(mask, maskStr, 100);
                    {
                        wchar_t line[600];
                        swprintf_s(line, 600,
                            L"  [%lu] %-10s  mask=0x%08lx (%s)  %s\\%s\r\n",
                            i, Dl_AceTypeName(type),
                            mask, maskStr,
                            domain[0] ? domain : L"", name);
                        Dl_Append(st->output, line);
                    }
                }
            }
        }
    }

    if (sd) LocalFree(sd);
}

static void Dl_Browse(DlState *st)
{
    OPENFILENAMEW ofn;
    wchar_t fn[MAX_PATH] = L"";
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = fn;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"All files\0*.*\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) SetWindowTextW(st->pathEdit, fn);
}

static LRESULT CALLBACK Dl_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    DlState *st = (DlState *)GetPropW(hwnd, DL_PROP);
    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_DL_GO) { Dl_Lookup(st); return 0; }
        if (LOWORD(wp) == ID_DL_BR) { Dl_Browse(st); return 0; }
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->pathEdit,  12, 38, w - 224, 24, TRUE);
        MoveWindow(st->browseBtn, w - 208, 38, 96, 24, TRUE);
        MoveWindow(st->goBtn,     w - 108, 38, 90, 24, TRUE);
        MoveWindow(st->output,    8, 70, w - 16, h - 78, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, DL_PROP); }
    return CallWindowProcW(g_origDlFrame, hwnd, msg, wp, lp);
}

static HWND DacLookup_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    DlState *st;
    HFONT mono;
    wchar_t defaultPath[MAX_PATH];
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"DacLookup",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (DlState *)calloc(1, sizeof(DlState));
    if (!st) { DestroyWindow(frame); return NULL; }

    ExpandEnvironmentStringsW(L"%windir%\\notepad.exe", defaultPath, MAX_PATH);

    st->pathEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", defaultPath,
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        12, 38, w - 224, 24, frame, (HMENU)(LONG_PTR)ID_DL_PATH, hInstance, NULL);
    st->browseBtn = CreateWindowExW(0, L"BUTTON", L"Browse...",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        w - 208, 38, 96, 24, frame, (HMENU)(LONG_PTR)ID_DL_BR, hInstance, NULL);
    st->goBtn = CreateWindowExW(0, L"BUTTON", L"Lookup DACL",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        w - 108, 38, 90, 24, frame, (HMENU)(LONG_PTR)ID_DL_GO, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 70, w - 16, h - 78, frame, (HMENU)(LONG_PTR)ID_DL_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, DL_PROP, (HANDLE)st);
    if (!g_origDlFrame) g_origDlFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Dl_FrameProc);
    Dl_Lookup(st);
    return frame;
}

MsApp g_AppDacLookup = { L"DacLookup", DacLookup_Create, 880, 520 };
