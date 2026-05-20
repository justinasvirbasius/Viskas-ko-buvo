/*
 * app_registry.c — Central registry of built-in apps
 */

#include "shell.h"
#include <stdio.h>

extern MsApp g_AppClock;
extern MsApp g_AppEditor;
extern MsApp g_AppCalc;
extern MsApp g_AppExplorer;
extern MsApp g_AppPaint;
extern MsApp g_AppTerminal;
extern MsApp g_AppNote;
extern MsApp g_AppSysMon;
extern MsApp g_AppColor;
extern MsApp g_AppImageView;
extern MsApp g_AppSnake;
extern MsApp g_AppFetcher;
extern MsApp g_AppProcs;
extern MsApp g_AppSettings;
extern MsApp g_AppClipboard;
extern MsApp g_AppBeeper;
extern MsApp g_AppRegTree;
extern MsApp g_AppGlCube;
extern MsApp g_AppHexView;
extern MsApp g_AppCmdRun;
extern MsApp g_AppTray;
extern MsApp g_AppRichDoc;
extern MsApp g_AppPngView;
extern MsApp g_AppHotKey;
extern MsApp g_AppProgress;

#define MAX_APPS 32
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
