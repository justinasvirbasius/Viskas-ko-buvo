/*
 * app_sysspec.c — System specifications inspector
 *
 * Demonstrates a cluster of one-shot system-info APIs:
 *   - GlobalMemoryStatusEx for installed/available physical and pagefile
 *   - GetDiskFreeSpaceExW for the free/total bytes of each fixed drive
 *   - GetLogicalDrives + GetDriveTypeW for drive enumeration
 *   - GetSystemMetrics for SM_CXSCREEN/SM_CYSCREEN/SM_CMONITORS/SM_CMOUSEBUTTONS/etc
 *   - SystemParametersInfoW(SPI_GETWORKAREA), GetSystemInfo for CPU count/arch
 *   - GetVersionExW deprecated; we use VerifyVersionInfo only obliquely via the
 *     simpler "what RTL reports" path — we just call GetVersionExW guarded by
 *     the standard #pragma to silence the deprecation warning.
 */

#include "shell.h"
#include <stdlib.h>
#include <stdio.h>

#ifdef _MSC_VER
#pragma warning(disable: 4996)   /* GetVersionExW deprecated */
#endif

#define SS_PROP    L"MS_SS_STATE"
#define ID_SS_OUT  53001
#define ID_SS_REF  53002

typedef struct {
    HWND output, refBtn;
} SsState;

static WNDPROC g_origSsFrame = NULL;

static void Ss_Append(HWND edit, const wchar_t *t)
{
    int len = GetWindowTextLengthW(edit);
    SendMessageW(edit, EM_SETSEL, len, len);
    SendMessageW(edit, EM_REPLACESEL, FALSE, (LPARAM)t);
}

static const wchar_t *Ss_DriveType(UINT t)
{
    switch (t) {
    case DRIVE_REMOVABLE: return L"removable";
    case DRIVE_FIXED:     return L"fixed";
    case DRIVE_REMOTE:    return L"remote";
    case DRIVE_CDROM:     return L"cdrom";
    case DRIVE_RAMDISK:   return L"ramdisk";
    case DRIVE_NO_ROOT_DIR: return L"no root";
    }
    return L"unknown";
}

static void Ss_Refresh(SsState *st)
{
    wchar_t buf[2048];
    MEMORYSTATUSEX m;
    SYSTEM_INFO si;
    RECT work = { 0 };
    DWORD drives;
    int i;

    SetWindowTextW(st->output, L"");

    /* CPU & architecture */
    GetNativeSystemInfo(&si);
    {
        const wchar_t *arch = L"?";
        switch (si.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64:  arch = L"x64"; break;
        case PROCESSOR_ARCHITECTURE_INTEL:  arch = L"x86"; break;
        case PROCESSOR_ARCHITECTURE_ARM:    arch = L"ARM"; break;
        case PROCESSOR_ARCHITECTURE_ARM64:  arch = L"ARM64"; break;
        case PROCESSOR_ARCHITECTURE_IA64:   arch = L"IA64"; break;
        }
        swprintf_s(buf, 2048,
            L"== CPU ==\r\n"
            L"  architecture       : %s\r\n"
            L"  logical processors : %lu\r\n"
            L"  page size          : %lu\r\n"
            L"  allocation gran.   : %lu\r\n\r\n",
            arch,
            si.dwNumberOfProcessors,
            si.dwPageSize,
            si.dwAllocationGranularity);
        Ss_Append(st->output, buf);
    }

    /* Memory */
    m.dwLength = sizeof(m);
    if (GlobalMemoryStatusEx(&m)) {
        swprintf_s(buf, 2048,
            L"== Memory ==\r\n"
            L"  load            : %lu %%\r\n"
            L"  physical total  : %llu MB\r\n"
            L"  physical avail  : %llu MB\r\n"
            L"  pagefile total  : %llu MB\r\n"
            L"  pagefile avail  : %llu MB\r\n"
            L"  virtual total   : %llu MB\r\n"
            L"  virtual avail   : %llu MB\r\n\r\n",
            m.dwMemoryLoad,
            m.ullTotalPhys     / (1024ULL * 1024),
            m.ullAvailPhys     / (1024ULL * 1024),
            m.ullTotalPageFile / (1024ULL * 1024),
            m.ullAvailPageFile / (1024ULL * 1024),
            m.ullTotalVirtual  / (1024ULL * 1024),
            m.ullAvailVirtual  / (1024ULL * 1024));
        Ss_Append(st->output, buf);
    }

    /* Drives */
    Ss_Append(st->output, L"== Drives ==\r\n");
    drives = GetLogicalDrives();
    for (i = 0; i < 26; ++i) {
        if (drives & (1u << i)) {
            wchar_t root[8];
            UINT type;
            ULARGE_INTEGER avail, total, totalFree;
            swprintf_s(root, 8, L"%c:\\", L'A' + i);
            type = GetDriveTypeW(root);
            if (type == DRIVE_FIXED &&
                GetDiskFreeSpaceExW(root, &avail, &total, &totalFree)) {
                swprintf_s(buf, 2048,
                    L"  %s  %-9s  free %5llu GB / %5llu GB total\r\n",
                    root, Ss_DriveType(type),
                    totalFree.QuadPart / (1024ULL * 1024 * 1024),
                    total.QuadPart     / (1024ULL * 1024 * 1024));
            } else {
                swprintf_s(buf, 2048, L"  %s  %s\r\n", root, Ss_DriveType(type));
            }
            Ss_Append(st->output, buf);
        }
    }
    Ss_Append(st->output, L"\r\n");

    /* Display metrics */
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    swprintf_s(buf, 2048,
        L"== Display ==\r\n"
        L"  primary resolution : %d x %d\r\n"
        L"  virtual desktop    : %d x %d\r\n"
        L"  work area          : %ld,%ld to %ld,%ld\r\n"
        L"  monitor count      : %d\r\n"
        L"  mouse buttons      : %d\r\n"
        L"  double-click time  : %u ms\r\n",
        GetSystemMetrics(SM_CXSCREEN),
        GetSystemMetrics(SM_CYSCREEN),
        GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_CYVIRTUALSCREEN),
        work.left, work.top, work.right, work.bottom,
        GetSystemMetrics(SM_CMONITORS),
        GetSystemMetrics(SM_CMOUSEBUTTONS),
        GetDoubleClickTime());
    Ss_Append(st->output, buf);

    /* Other tidbits */
    {
        TCHAR cn[MAX_COMPUTERNAME_LENGTH + 1];
        DWORD cb = MAX_COMPUTERNAME_LENGTH + 1;
        DWORD tickCount = GetTickCount();
        cn[0] = 0;
        GetComputerNameW(cn, &cb);
        swprintf_s(buf, 2048,
            L"\r\n== Misc ==\r\n"
            L"  computer name : %s\r\n"
            L"  uptime        : %lu sec\r\n"
            L"  tick count    : %lu ms\r\n",
            cn[0] ? cn : L"<unknown>",
            tickCount / 1000,
            tickCount);
        Ss_Append(st->output, buf);
    }
}

static LRESULT CALLBACK Ss_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    SsState *st = (SsState *)GetPropW(hwnd, SS_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_SS_REF) { Ss_Refresh(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->refBtn, 8, 34, 100, 24, TRUE);
        MoveWindow(st->output, 8, 64, w - 16, h - 72, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, SS_PROP); }
    return CallWindowProcW(g_origSsFrame, hwnd, msg, wp, lp);
}

static HWND SysSpec_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    SsState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"SysSpec",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (SsState *)calloc(1, sizeof(SsState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->refBtn = CreateWindowExW(0, L"BUTTON", L"Refresh",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        8, 34, 100, 24, frame, (HMENU)(LONG_PTR)ID_SS_REF, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 64, w - 16, h - 72, frame, (HMENU)(LONG_PTR)ID_SS_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, SS_PROP, (HANDLE)st);
    if (!g_origSsFrame) g_origSsFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Ss_FrameProc);

    Ss_Refresh(st);
    return frame;
}

MsApp g_AppSysSpec = {
    L"SysSpec",
    SysSpec_Create,
    560, 480
};
