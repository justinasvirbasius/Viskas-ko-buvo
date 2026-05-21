/*
 * app_eventlog.c — Read entries from the Windows Event Log
 *
 * Demonstrates the classic Event Logging API (still present alongside the
 * newer EvtQuery API):
 *   - OpenEventLogW("System")
 *   - ReadEventLogW EVENTLOG_SEQUENTIAL_READ | EVENTLOG_BACKWARDS_READ
 *   - Iterating EVENTLOGRECORD entries packed in a single buffer
 *   - Formatting timestamps with FileTimeToSystemTime
 *
 * Most users can read the System and Application logs without elevation;
 * the Security log requires admin and is not attempted here.
 */

#include "shell.h"
#include <commctrl.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "advapi32.lib")

#define EL_PROP    L"MS_EL_STATE"
#define ID_EL_LIST 31001
#define ID_EL_LOG  31002
#define ID_EL_LOAD 31003

typedef struct {
    HWND list, logEdit, loadBtn;
} ElState;

static WNDPROC g_origElFrame = NULL;

static const wchar_t *El_TypeName(WORD t)
{
    switch (t) {
    case EVENTLOG_ERROR_TYPE:       return L"Error";
    case EVENTLOG_WARNING_TYPE:     return L"Warning";
    case EVENTLOG_INFORMATION_TYPE: return L"Info";
    case EVENTLOG_AUDIT_SUCCESS:    return L"AuditOK";
    case EVENTLOG_AUDIT_FAILURE:    return L"AuditFail";
    default:                        return L"?";
    }
}

static void El_Load(ElState *st)
{
    wchar_t logName[64];
    HANDLE  log;
    BYTE   *buf;
    DWORD   bufSize = 64 * 1024;
    DWORD   bytesRead, bytesNeeded;
    int     row = 0, max = 200;

    GetWindowTextW(st->logEdit, logName, 64);
    if (logName[0] == 0) wcscpy_s(logName, 64, L"System");

    SendMessageW(st->list, LVM_DELETEALLITEMS, 0, 0);

    log = OpenEventLogW(NULL, logName);
    if (!log) {
        LVITEMW it;
        wchar_t msg[120];
        swprintf_s(msg, 120, L"OpenEventLog(\"%s\") failed (error %lu)",
                   logName, GetLastError());
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT;
        it.pszText = msg;
        SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        return;
    }

    buf = (BYTE *)malloc(bufSize);
    if (!buf) { CloseEventLog(log); return; }

    while (row < max) {
        if (!ReadEventLogW(log,
                EVENTLOG_SEQUENTIAL_READ | EVENTLOG_BACKWARDS_READ,
                0, buf, bufSize, &bytesRead, &bytesNeeded)) {
            DWORD err = GetLastError();
            if (err == ERROR_INSUFFICIENT_BUFFER && bytesNeeded > bufSize) {
                free(buf);
                bufSize = bytesNeeded;
                buf = (BYTE *)malloc(bufSize);
                if (!buf) break;
                continue;
            }
            break;
        }

        {
            BYTE *p = buf;
            BYTE *end = buf + bytesRead;
            while (p < end && row < max) {
                EVENTLOGRECORD *rec = (EVENTLOGRECORD *)p;
                wchar_t timeStr[32], idStr[24], srcCopy[120];
                SYSTEMTIME st_;
                FILETIME ft;
                ULONGLONG ull;
                LVITEMW it;
                const wchar_t *source;

                ull = ((ULONGLONG)rec->TimeGenerated) * 10000000ULL +
                      116444736000000000ULL;
                ft.dwLowDateTime  = (DWORD)(ull & 0xFFFFFFFF);
                ft.dwHighDateTime = (DWORD)(ull >> 32);
                FileTimeToSystemTime(&ft, &st_);
                swprintf_s(timeStr, 32, L"%04d-%02d-%02d %02d:%02d:%02d",
                    st_.wYear, st_.wMonth, st_.wDay,
                    st_.wHour, st_.wMinute, st_.wSecond);

                source = (const wchar_t *)((BYTE *)rec + sizeof(EVENTLOGRECORD));
                wcsncpy_s(srcCopy, 120, source, _TRUNCATE);
                swprintf_s(idStr, 24, L"%lu", rec->EventID & 0xFFFF);

                ZeroMemory(&it, sizeof(it));
                it.mask = LVIF_TEXT;
                it.iItem = row;
                it.pszText = timeStr;
                SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);

                it.iSubItem = 1; it.pszText = (LPWSTR)El_TypeName(rec->EventType);
                SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
                it.iSubItem = 2; it.pszText = idStr;
                SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
                it.iSubItem = 3; it.pszText = srcCopy;
                SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);

                p += rec->Length;
                ++row;
            }
        }
    }
    free(buf);
    CloseEventLog(log);
}

static LRESULT CALLBACK El_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    ElState *st = (ElState *)GetPropW(hwnd, EL_PROP);

    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_EL_LOAD) { El_Load(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->logEdit, 8, 34, 200, 24, TRUE);
        MoveWindow(st->loadBtn, 216, 34, 100, 24, TRUE);
        MoveWindow(st->list,    8, 64, w - 16, h - 72, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, EL_PROP); }
    return CallWindowProcW(g_origElFrame, hwnd, msg, wp, lp);
}

static HWND EventLog_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    ElState *st;
    INITCOMMONCONTROLSEX icc;
    LVCOLUMNW col;
    (void)self;

    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"EventLog",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (ElState *)calloc(1, sizeof(ElState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->logEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"System",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        8, 34, 200, 24, frame, (HMENU)(LONG_PTR)ID_EL_LOG, hInstance, NULL);
    st->loadBtn = CreateWindowExW(0, L"BUTTON", L"Load",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        216, 34, 100, 24, frame, (HMENU)(LONG_PTR)ID_EL_LOAD, hInstance, NULL);

    st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
        WS_CHILD | WS_VISIBLE | LVS_REPORT,
        8, 64, w - 16, h - 72, frame, (HMENU)(LONG_PTR)ID_EL_LIST, hInstance, NULL);
    SendMessageW(st->list, LVM_SETEXTENDEDLISTVIEWSTYLE,
                 LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES,
                 LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = (LPWSTR)L"Time";   col.cx = 160;
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.pszText = (LPWSTR)L"Type";   col.cx = 80;
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    col.pszText = (LPWSTR)L"Event";  col.cx = 80;
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);
    col.pszText = (LPWSTR)L"Source"; col.cx = 240;
    SendMessageW(st->list, LVM_INSERTCOLUMNW, 3, (LPARAM)&col);

    SetPropW(frame, EL_PROP, (HANDLE)st);
    if (!g_origElFrame)
        g_origElFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)El_FrameProc);

    El_Load(st);
    return frame;
}

MsApp g_AppEventLog = {
    L"EventLog",
    EventLog_Create,
    640, 460
};
