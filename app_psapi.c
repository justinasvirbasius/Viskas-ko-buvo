/*
 * app_psapi.c — Process introspection via psapi.dll
 *
 * Demonstrates the modern process-info APIs (psapi.dll), which are lighter
 * than the Toolhelp32 snapshot approach used by Batch 3's Procs:
 *
 *   - EnumProcesses(pids[], cb, &returned) fills a flat DWORD array of
 *     active process IDs
 *   - OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, ...)
 *     opens a process; QUERY_LIMITED works on more processes (incl. some
 *     protected ones) than the older QUERY_INFORMATION
 *   - QueryFullProcessImageNameW gives the full image path (modern,
 *     replaces GetModuleFileNameExW)
 *   - GetProcessTimes returns creation/exit/kernel/user FILETIMEs;
 *     subtract user+kernel to get CPU usage
 *   - GetProcessMemoryInfo gives a PROCESS_MEMORY_COUNTERS_EX with
 *     WorkingSetSize, PrivateUsage, PageFaultCount
 *
 * ListView columns: PID, Name, Working set (MB), Private (MB), CPU s, Path.
 */

#include "shell.h"
#include <psapi.h>
#include <commctrl.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "comctl32.lib")

#define PS_PROP   L"MS_PS_STATE"
#define ID_PS_REF 93001
#define ID_PS_LV  93002

typedef struct { HWND refresh, list; } PsState;
static WNDPROC g_origPsFrame = NULL;

static double Ps_FileTimeToSec(FILETIME *ft)
{
    ULARGE_INTEGER u;
    u.LowPart = ft->dwLowDateTime;
    u.HighPart = ft->dwHighDateTime;
    return u.QuadPart / 10000000.0;
}

static void Ps_Refresh(PsState *st)
{
    DWORD pids[2048];
    DWORD bytesReturned;
    DWORD i, count;

    SendMessageW(st->list, LVM_DELETEALLITEMS, 0, 0);

    if (!EnumProcesses(pids, sizeof(pids), &bytesReturned)) return;
    count = bytesReturned / sizeof(DWORD);

    for (i = 0; i < count; ++i) {
        HANDLE h;
        wchar_t name[MAX_PATH] = L"<?>";
        wchar_t fullPath[MAX_PATH] = L"";
        wchar_t pidStr[16];
        wchar_t wsStr[24] = L"-";
        wchar_t privStr[24] = L"-";
        wchar_t cpuStr[24] = L"-";
        LVITEMW it;
        FILETIME ftCreate, ftExit, ftKernel, ftUser;
        PROCESS_MEMORY_COUNTERS_EX pmc;
        DWORD pathLen = MAX_PATH;

        if (pids[i] == 0) continue;

        h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                         FALSE, pids[i]);
        if (h) {
            if (QueryFullProcessImageNameW(h, 0, fullPath, &pathLen)) {
                /* strip path, leave basename */
                wchar_t *slash = wcsrchr(fullPath, L'\\');
                wcscpy_s(name, MAX_PATH, slash ? slash + 1 : fullPath);
            }
            if (GetProcessTimes(h, &ftCreate, &ftExit, &ftKernel, &ftUser)) {
                double cpu = Ps_FileTimeToSec(&ftKernel) + Ps_FileTimeToSec(&ftUser);
                swprintf_s(cpuStr, 24, L"%.2f", cpu);
            }
            if (GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof(pmc))) {
                swprintf_s(wsStr,   24, L"%.1f", pmc.WorkingSetSize / 1048576.0);
                swprintf_s(privStr, 24, L"%.1f", pmc.PrivateUsage  / 1048576.0);
            }
            CloseHandle(h);
        } else {
            /* Permission denied — fill only PID */
            wcscpy_s(name, MAX_PATH, L"(no access)");
        }

        swprintf_s(pidStr, 16, L"%lu", pids[i]);
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT; it.iItem = (int)i;
        it.pszText = pidStr;  SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        it.iSubItem = 1; it.pszText = name;     SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
        it.iSubItem = 2; it.pszText = wsStr;    SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
        it.iSubItem = 3; it.pszText = privStr;  SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
        it.iSubItem = 4; it.pszText = cpuStr;   SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
        it.iSubItem = 5; it.pszText = fullPath; SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
    }
}

static LRESULT CALLBACK Ps_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    PsState *st = (PsState *)GetPropW(hwnd, PS_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_PS_REF) { Ps_Refresh(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->refresh, 12, 38, 110, 26, TRUE);
        MoveWindow(st->list,    8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, PS_PROP); }
    return CallWindowProcW(g_origPsFrame, hwnd, msg, wp, lp);
}

static HWND Psapi_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    PsState *st;
    INITCOMMONCONTROLSEX icc;
    LVCOLUMNW col;
    (void)self;

    icc.dwSize = sizeof(icc); icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Psapi",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (PsState *)calloc(1, sizeof(PsState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->refresh = CreateWindowExW(0, L"BUTTON", L"Refresh",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 110, 26, frame, (HMENU)(LONG_PTR)ID_PS_REF, hInstance, NULL);

    st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
        WS_CHILD | WS_VISIBLE | LVS_REPORT,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_PS_LV, hInstance, NULL);
    SendMessageW(st->list, LVM_SETEXTENDEDLISTVIEWSTYLE,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx =  60; col.pszText = (LPWSTR)L"PID";          SendMessageW(st->list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.cx = 200; col.pszText = (LPWSTR)L"Name";         SendMessageW(st->list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    col.cx =  80; col.pszText = (LPWSTR)L"Working MB";   SendMessageW(st->list, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);
    col.cx =  80; col.pszText = (LPWSTR)L"Private MB";   SendMessageW(st->list, LVM_INSERTCOLUMNW, 3, (LPARAM)&col);
    col.cx =  70; col.pszText = (LPWSTR)L"CPU s";        SendMessageW(st->list, LVM_INSERTCOLUMNW, 4, (LPARAM)&col);
    col.cx = 360; col.pszText = (LPWSTR)L"Image path";   SendMessageW(st->list, LVM_INSERTCOLUMNW, 5, (LPARAM)&col);

    SetPropW(frame, PS_PROP, (HANDLE)st);
    if (!g_origPsFrame) g_origPsFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Ps_FrameProc);
    Ps_Refresh(st);
    return frame;
}

MsApp g_AppPsapi = { L"Psapi", Psapi_Create, 880, 480 };
