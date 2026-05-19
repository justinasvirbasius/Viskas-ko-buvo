/*
 * app_registry.c — Central registry of built-in apps
 *
 * Each app exposes a `MsApp` descriptor with a creation function. The registry
 * is a static table; the shell's taskbar reads from it to draw launcher
 * buttons and calls Registry_Launch when one is clicked.
 */

#include "shell.h"
#include <stdio.h>

/* Each app's module exposes its descriptor via these externs */
extern MsApp g_AppClock;
extern MsApp g_AppEditor;
extern MsApp g_AppCalc;

#define MAX_APPS 8
static MsApp *g_apps[MAX_APPS];
static int    g_app_count = 0;

void Registry_Init(void)
{
    g_app_count = 0;
    g_apps[g_app_count++] = &g_AppClock;
    g_apps[g_app_count++] = &g_AppEditor;
    g_apps[g_app_count++] = &g_AppCalc;
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
    offset = WM_Count() * 24;          /* cascade new windows */
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
        DestroyWindow(hwnd); /* no slot available */
    }
}
