/*
 * app_rstrtmgr.c — Restart Manager: which process locks a file?
 *
 * Demonstrates the Restart Manager API (rstrtmgr.dll) — the same one
 * Windows Update uses to discover which processes need to be restarted
 * after a file is updated:
 *   - RmStartSession(&sessionHandle, 0, sessionKey) — sessionKey is a
 *     CCH_RM_SESSION_KEY+1 wchar buffer that we leave zeroed
 *   - RmRegisterResources(session, nFiles, &filePaths, 0, NULL, 0, NULL)
 *     tells the manager which files we're concerned with
 *   - RmGetList(session, &procInfoNeeded, &count, procInfoArray, &reason)
 *     two-call pattern: first NULL/0 returns ERROR_MORE_DATA + the
 *     needed count; allocate and call again
 *   - RmEndSession(session)
 *
 * Each RM_PROCESS_INFO has Process.dwProcessId + strAppName + ApplicationType
 * + bRestartable flag.
 */

#include "shell.h"
#include <restartmanager.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "rstrtmgr.lib")

#define RM_PROP    L"MS_RM_STATE"
#define ID_RM_PATH 91001
#define ID_RM_BR   91002
#define ID_RM_GO   91003
#define ID_RM_OUT  91004

typedef struct { HWND pathEdit, browseBtn, goBtn, output; } RmState;
static WNDPROC g_origRmFrame = NULL;

static void Rm_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
}

static const wchar_t *Rm_AppTypeName(RM_APP_TYPE t)
{
    switch (t) {
    case RmUnknownApp: return L"Unknown";
    case RmMainWindow: return L"MainWindow";
    case RmOtherWindow:return L"OtherWindow";
    case RmService:    return L"Service";
    case RmExplorer:   return L"Explorer";
    case RmConsole:    return L"Console";
    case RmCritical:   return L"Critical";
    }
    return L"?";
}

static void Rm_Probe(RmState *st)
{
    wchar_t path[MAX_PATH];
    DWORD   session = 0;
    WCHAR   sessionKey[CCH_RM_SESSION_KEY + 1] = {0};
    LPCWSTR fileList[1];
    DWORD   needed = 0, actual = 0;
    RM_PROCESS_INFO *procs = NULL;
    DWORD   reason = 0;
    DWORD   r;

    SetWindowTextW(st->output, L"");
    GetWindowTextW(st->pathEdit, path, MAX_PATH);
    if (!path[0]) {
        Rm_Append(st->output, L"Choose or enter a file path first.\r\n");
        return;
    }

    r = RmStartSession(&session, 0, sessionKey);
    if (r != ERROR_SUCCESS) {
        wchar_t buf[80];
        swprintf_s(buf, 80, L"RmStartSession failed: %lu\r\n", r);
        Rm_Append(st->output, buf);
        return;
    }

    fileList[0] = path;
    r = RmRegisterResources(session, 1, fileList, 0, NULL, 0, NULL);
    if (r != ERROR_SUCCESS) {
        wchar_t buf[80];
        swprintf_s(buf, 80, L"RmRegisterResources failed: %lu\r\n", r);
        Rm_Append(st->output, buf);
        RmEndSession(session);
        return;
    }

    /* Two-call sizing pattern */
    actual = 0;
    r = RmGetList(session, &needed, &actual, NULL, &reason);
    if (r == ERROR_SUCCESS && needed == 0) {
        Rm_Append(st->output, L"No process currently locks this file.\r\n");
        RmEndSession(session);
        return;
    }
    if (r != ERROR_MORE_DATA) {
        wchar_t buf[80];
        swprintf_s(buf, 80, L"RmGetList (probe) returned %lu (needed=%lu)\r\n", r, needed);
        Rm_Append(st->output, buf);
        RmEndSession(session);
        return;
    }

    procs = (RM_PROCESS_INFO *)calloc(needed, sizeof(RM_PROCESS_INFO));
    if (!procs) { RmEndSession(session); return; }

    actual = needed;
    r = RmGetList(session, &needed, &actual, procs, &reason);
    if (r != ERROR_SUCCESS) {
        wchar_t buf[80];
        swprintf_s(buf, 80, L"RmGetList failed: %lu\r\n", r);
        Rm_Append(st->output, buf);
        free(procs);
        RmEndSession(session);
        return;
    }

    {
        wchar_t hdr[160];
        const wchar_t *reasonStr =
            (reason == RmRebootReasonPermissionDenied)         ? L"permission denied" :
            (reason == RmRebootReasonSessionMismatch)          ? L"session mismatch" :
            (reason == RmRebootReasonCriticalProcess)          ? L"critical process" :
            (reason == RmRebootReasonCriticalService)          ? L"critical service" :
            (reason == RmRebootReasonDetectedSelf)             ? L"detected self" :
            (reason == RmRebootReasonInvalidSession)           ? L"invalid session" :
            L"none";
        swprintf_s(hdr, 160, L"%lu process(es) hold this file. Reboot reason: %s\r\n\r\n",
                   actual, reasonStr);
        Rm_Append(st->output, hdr);
    }

    {
        DWORD i;
        for (i = 0; i < actual; ++i) {
            wchar_t line[400];
            swprintf_s(line, 400,
                L"  PID %-6lu  %-12s  restartable=%s  name=%s\r\n",
                procs[i].Process.dwProcessId,
                Rm_AppTypeName(procs[i].ApplicationType),
                procs[i].bRestartable ? L"yes" : L"no",
                procs[i].strAppName);
            Rm_Append(st->output, line);
        }
    }

    free(procs);
    RmEndSession(session);
}

static void Rm_Browse(RmState *st)
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

static LRESULT CALLBACK Rm_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    RmState *st = (RmState *)GetPropW(hwnd, RM_PROP);
    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_RM_GO) { Rm_Probe(st);  return 0; }
        if (LOWORD(wp) == ID_RM_BR) { Rm_Browse(st); return 0; }
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->pathEdit,  12, 38, w - 224, 24, TRUE);
        MoveWindow(st->browseBtn, w - 208, 38, 96, 24, TRUE);
        MoveWindow(st->goBtn,     w - 108, 38, 90, 24, TRUE);
        MoveWindow(st->output,    8, 70, w - 16, h - 78, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, RM_PROP); }
    return CallWindowProcW(g_origRmFrame, hwnd, msg, wp, lp);
}

static HWND RstrtMgr_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    RmState *st;
    HFONT mono;
    wchar_t defaultPath[MAX_PATH];
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"RstrtMgr",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (RmState *)calloc(1, sizeof(RmState));
    if (!st) { DestroyWindow(frame); return NULL; }

    ExpandEnvironmentStringsW(L"%windir%\\notepad.exe", defaultPath, MAX_PATH);

    st->pathEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", defaultPath,
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        12, 38, w - 224, 24, frame, (HMENU)(LONG_PTR)ID_RM_PATH, hInstance, NULL);
    st->browseBtn = CreateWindowExW(0, L"BUTTON", L"Browse...",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        w - 208, 38, 96, 24, frame, (HMENU)(LONG_PTR)ID_RM_BR, hInstance, NULL);
    st->goBtn = CreateWindowExW(0, L"BUTTON", L"Who locks?",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        w - 108, 38, 90, 24, frame, (HMENU)(LONG_PTR)ID_RM_GO, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"Choose a file (e.g. an EXE or DLL of a running process) and click Who locks?\r\n",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 70, w - 16, h - 78, frame, (HMENU)(LONG_PTR)ID_RM_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, RM_PROP, (HANDLE)st);
    if (!g_origRmFrame) g_origRmFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Rm_FrameProc);
    return frame;
}

MsApp g_AppRstrtMgr = { L"RstrtMgr", RstrtMgr_Create, 760, 440 };
