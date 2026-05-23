/*
 * app_registry.c — Central registry of all built-in apps
 */

#include "shell.h"
#include <stdio.h>

/* Batch 1 — fundamentals */
extern MsApp g_AppClock;
extern MsApp g_AppEditor;
extern MsApp g_AppCalc;
/* Batch 2 — controls and GDI */
extern MsApp g_AppExplorer;
extern MsApp g_AppPaint;
extern MsApp g_AppTerminal;
extern MsApp g_AppNote;
extern MsApp g_AppSysMon;
extern MsApp g_AppColor;
/* Batch 3 — system services */
extern MsApp g_AppImageView;
extern MsApp g_AppSnake;
extern MsApp g_AppFetcher;
extern MsApp g_AppProcs;
extern MsApp g_AppSettings;
extern MsApp g_AppClipboard;
extern MsApp g_AppBeeper;
extern MsApp g_AppRegTree;
/* Batch 4 — advanced surfaces */
extern MsApp g_AppGlCube;
extern MsApp g_AppHexView;
extern MsApp g_AppCmdRun;
extern MsApp g_AppTray;
extern MsApp g_AppRichDoc;
extern MsApp g_AppPngView;
extern MsApp g_AppHotKey;
extern MsApp g_AppProgress;
/* Batch 5 — modern stacks and IPC */
extern MsApp g_AppD2D;
extern MsApp g_AppHasher;
extern MsApp g_AppHttpsGet;
extern MsApp g_AppPipeChat;
extern MsApp g_AppShared;
extern MsApp g_AppServices;
extern MsApp g_AppMonitors;
extern MsApp g_AppGdiPlus;
extern MsApp g_AppLayered;
extern MsApp g_AppDateBook;
extern MsApp g_AppWavPlay;
/* Batch 6 — deeper-water APIs */
extern MsApp g_AppMailSlot;
extern MsApp g_AppAsync;
extern MsApp g_AppRawInput;
extern MsApp g_AppThreadPool;
extern MsApp g_AppDpiAware;
extern MsApp g_AppEventLog;
extern MsApp g_AppCounters;
extern MsApp g_AppShapedWin;
extern MsApp g_AppAtoms;
/* Batch 7 — multimedia, modern dialogs, orphaned surfaces */
extern MsApp g_AppD3D11;
extern MsApp g_AppWasapiOut;
extern MsApp g_AppMicLevel;
extern MsApp g_AppTaskDlg;
extern MsApp g_AppOpenDlg;
extern MsApp g_AppSendIn;
extern MsApp g_AppConsole;
extern MsApp g_AppWindows;
extern MsApp g_AppPower;
extern MsApp g_AppNetInfo;
/* Batch 8 — system inspection, crypto, richer shell */
extern MsApp g_AppAesCipher;
extern MsApp g_AppCompressor;
extern MsApp g_AppMetaFile;
extern MsApp g_AppSessions;
extern MsApp g_AppFileOps;
extern MsApp g_AppLocales;
extern MsApp g_AppModules;
extern MsApp g_AppBroadcast;
extern MsApp g_AppDibClip;
extern MsApp g_AppSysSpec;
/* Batch 9 — networking, devices, shell integration, identity, modern UI */
extern MsApp g_AppTcpList;
extern MsApp g_AppNetAdapt;
extern MsApp g_AppDevices;
extern MsApp g_AppHookKbd;
extern MsApp g_AppDragSrc;
extern MsApp g_AppDpapi;
extern MsApp g_AppDirWatch;
extern MsApp g_AppShellLnk;
extern MsApp g_AppFonts;
extern MsApp g_AppDwmAttr;
/* Batch 10 — security, taskbar, hardware, drawing primitives, INI/GUID/SEH */
extern MsApp g_AppTokenInfo;
extern MsApp g_AppJumpList;
extern MsApp g_AppTopology;
extern MsApp g_AppPrintEnum;
extern MsApp g_AppIniFile;
extern MsApp g_AppGuidGen;
extern MsApp g_AppCurves;
extern MsApp g_AppNetWatch;
extern MsApp g_AppSehProbe;
extern MsApp g_AppDibDraw;
/* Batch 11 — accessibility events, shell modernity, sessions, encoding */
extern MsApp g_AppWinEvent;
extern MsApp g_AppTaskBar3;
extern MsApp g_AppPerPixAlpha;
extern MsApp g_AppShellProps;
extern MsApp g_AppKeyMap;
extern MsApp g_AppTimeZones;
extern MsApp g_AppDevNotify;
extern MsApp g_AppSessHook;
extern MsApp g_AppWsaEvent;
extern MsApp g_AppEncodings;
/* Batch 12 — MF, WMI, UIA, OLE tgt, MMCSS, certs, RM */
extern MsApp g_AppMediaFndr;
extern MsApp g_AppWmiQuery;
extern MsApp g_AppUIAuto;
extern MsApp g_AppPasteTgt;
extern MsApp g_AppOldWatch;
extern MsApp g_AppMmcssAud;
extern MsApp g_AppSaveDlg;
extern MsApp g_AppCertStore;
extern MsApp g_AppRstrtMgr;
extern MsApp g_AppClipMon;
/* Batch 13 — process introspection, install/security/network metadata, themes, PE, COM ROT */
extern MsApp g_AppPsapi;
extern MsApp g_AppMsiList;
extern MsApp g_AppSymlink;
extern MsApp g_AppGuiInfo;
extern MsApp g_AppShellWins;
extern MsApp g_AppWlanInfo;
extern MsApp g_AppPowerSch;
extern MsApp g_AppUxTheme;
extern MsApp g_AppPeInfo;
extern MsApp g_AppRotList;

#define MAX_APPS 140
static MsApp *g_apps[MAX_APPS];
static int    g_app_count = 0;

void Registry_Init(void)
{
    g_app_count = 0;
    g_apps[g_app_count++] = &g_AppClock;
    g_apps[g_app_count++] = &g_AppEditor;
    g_apps[g_app_count++] = &g_AppCalc;
    g_apps[g_app_count++] = &g_AppExplorer;
    g_apps[g_app_count++] = &g_AppPaint;
    g_apps[g_app_count++] = &g_AppTerminal;
    g_apps[g_app_count++] = &g_AppNote;
    g_apps[g_app_count++] = &g_AppSysMon;
    g_apps[g_app_count++] = &g_AppColor;
    g_apps[g_app_count++] = &g_AppImageView;
    g_apps[g_app_count++] = &g_AppSnake;
    g_apps[g_app_count++] = &g_AppFetcher;
    g_apps[g_app_count++] = &g_AppProcs;
    g_apps[g_app_count++] = &g_AppSettings;
    g_apps[g_app_count++] = &g_AppClipboard;
    g_apps[g_app_count++] = &g_AppBeeper;
    g_apps[g_app_count++] = &g_AppRegTree;
    g_apps[g_app_count++] = &g_AppGlCube;
    g_apps[g_app_count++] = &g_AppHexView;
    g_apps[g_app_count++] = &g_AppCmdRun;
    g_apps[g_app_count++] = &g_AppTray;
    g_apps[g_app_count++] = &g_AppRichDoc;
    g_apps[g_app_count++] = &g_AppPngView;
    g_apps[g_app_count++] = &g_AppHotKey;
    g_apps[g_app_count++] = &g_AppProgress;
    g_apps[g_app_count++] = &g_AppD2D;
    g_apps[g_app_count++] = &g_AppHasher;
    g_apps[g_app_count++] = &g_AppHttpsGet;
    g_apps[g_app_count++] = &g_AppPipeChat;
    g_apps[g_app_count++] = &g_AppShared;
    g_apps[g_app_count++] = &g_AppServices;
    g_apps[g_app_count++] = &g_AppMonitors;
    g_apps[g_app_count++] = &g_AppGdiPlus;
    g_apps[g_app_count++] = &g_AppLayered;
    g_apps[g_app_count++] = &g_AppDateBook;
    g_apps[g_app_count++] = &g_AppWavPlay;
    g_apps[g_app_count++] = &g_AppMailSlot;
    g_apps[g_app_count++] = &g_AppAsync;
    g_apps[g_app_count++] = &g_AppRawInput;
    g_apps[g_app_count++] = &g_AppThreadPool;
    g_apps[g_app_count++] = &g_AppDpiAware;
    g_apps[g_app_count++] = &g_AppEventLog;
    g_apps[g_app_count++] = &g_AppCounters;
    g_apps[g_app_count++] = &g_AppShapedWin;
    g_apps[g_app_count++] = &g_AppAtoms;
    g_apps[g_app_count++] = &g_AppD3D11;
    g_apps[g_app_count++] = &g_AppWasapiOut;
    g_apps[g_app_count++] = &g_AppMicLevel;
    g_apps[g_app_count++] = &g_AppTaskDlg;
    g_apps[g_app_count++] = &g_AppOpenDlg;
    g_apps[g_app_count++] = &g_AppSendIn;
    g_apps[g_app_count++] = &g_AppConsole;
    g_apps[g_app_count++] = &g_AppWindows;
    g_apps[g_app_count++] = &g_AppPower;
    g_apps[g_app_count++] = &g_AppNetInfo;
    g_apps[g_app_count++] = &g_AppAesCipher;
    g_apps[g_app_count++] = &g_AppCompressor;
    g_apps[g_app_count++] = &g_AppMetaFile;
    g_apps[g_app_count++] = &g_AppSessions;
    g_apps[g_app_count++] = &g_AppFileOps;
    g_apps[g_app_count++] = &g_AppLocales;
    g_apps[g_app_count++] = &g_AppModules;
    g_apps[g_app_count++] = &g_AppBroadcast;
    g_apps[g_app_count++] = &g_AppDibClip;
    g_apps[g_app_count++] = &g_AppSysSpec;
    g_apps[g_app_count++] = &g_AppTcpList;
    g_apps[g_app_count++] = &g_AppNetAdapt;
    g_apps[g_app_count++] = &g_AppDevices;
    g_apps[g_app_count++] = &g_AppHookKbd;
    g_apps[g_app_count++] = &g_AppDragSrc;
    g_apps[g_app_count++] = &g_AppDpapi;
    g_apps[g_app_count++] = &g_AppDirWatch;
    g_apps[g_app_count++] = &g_AppShellLnk;
    g_apps[g_app_count++] = &g_AppFonts;
    g_apps[g_app_count++] = &g_AppDwmAttr;
    g_apps[g_app_count++] = &g_AppTokenInfo;
    g_apps[g_app_count++] = &g_AppJumpList;
    g_apps[g_app_count++] = &g_AppTopology;
    g_apps[g_app_count++] = &g_AppPrintEnum;
    g_apps[g_app_count++] = &g_AppIniFile;
    g_apps[g_app_count++] = &g_AppGuidGen;
    g_apps[g_app_count++] = &g_AppCurves;
    g_apps[g_app_count++] = &g_AppNetWatch;
    g_apps[g_app_count++] = &g_AppSehProbe;
    g_apps[g_app_count++] = &g_AppDibDraw;
    g_apps[g_app_count++] = &g_AppWinEvent;
    g_apps[g_app_count++] = &g_AppTaskBar3;
    g_apps[g_app_count++] = &g_AppPerPixAlpha;
    g_apps[g_app_count++] = &g_AppShellProps;
    g_apps[g_app_count++] = &g_AppKeyMap;
    g_apps[g_app_count++] = &g_AppTimeZones;
    g_apps[g_app_count++] = &g_AppDevNotify;
    g_apps[g_app_count++] = &g_AppSessHook;
    g_apps[g_app_count++] = &g_AppWsaEvent;
    g_apps[g_app_count++] = &g_AppEncodings;
    g_apps[g_app_count++] = &g_AppMediaFndr;
    g_apps[g_app_count++] = &g_AppWmiQuery;
    g_apps[g_app_count++] = &g_AppUIAuto;
    g_apps[g_app_count++] = &g_AppPasteTgt;
    g_apps[g_app_count++] = &g_AppOldWatch;
    g_apps[g_app_count++] = &g_AppMmcssAud;
    g_apps[g_app_count++] = &g_AppSaveDlg;
    g_apps[g_app_count++] = &g_AppCertStore;
    g_apps[g_app_count++] = &g_AppRstrtMgr;
    g_apps[g_app_count++] = &g_AppClipMon;
    g_apps[g_app_count++] = &g_AppPsapi;
    g_apps[g_app_count++] = &g_AppMsiList;
    g_apps[g_app_count++] = &g_AppSymlink;
    g_apps[g_app_count++] = &g_AppGuiInfo;
    g_apps[g_app_count++] = &g_AppShellWins;
    g_apps[g_app_count++] = &g_AppWlanInfo;
    g_apps[g_app_count++] = &g_AppPowerSch;
    g_apps[g_app_count++] = &g_AppUxTheme;
    g_apps[g_app_count++] = &g_AppPeInfo;
    g_apps[g_app_count++] = &g_AppRotList;
}

int Registry_Count(void)
{
    return g_app_count;
}

MsApp *Registry_GetAt(int index)
{
    if (index < 0 || index >= g_app_count) return NULL;
    return g_apps[index];
}

void Registry_Launch(int index, HWND desktop)
{
    MsApp *app;
    RECT   rc;
    int    x, y, w, h, offset;
    HWND   hwnd;
    MsAppWindow *aw;

    app = Registry_GetAt(index);
    if (!app) return;

    GetClientRect(desktop, &rc);
    w = app->default_w;
    h = app->default_h;
    offset = WM_Count() * 24;
    x = 80 + offset;
    y = 60 + offset;

    hwnd = app->create(desktop, x, y, w, h, app);
    if (!hwnd) return;

    aw = WM_Register(hwnd, app, app->title);
    if (aw) {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
        SetForegroundWindow(hwnd);
    } else {
        DestroyWindow(hwnd);
    }
}
