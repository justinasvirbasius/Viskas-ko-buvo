/*
 * window_manager.c — Tracks all active app windows
 *
 * The shell hosts up to MS_MAX_APP_WINDOWS app instances. This module owns the
 * table that maps HWND -> app instance, used by the taskbar and event router.
 */

#include "shell.h"
#include <string.h>

static MsAppWindow g_windows[MS_MAX_APP_WINDOWS];

void WM_Init(void)
{
    ZeroMemory(g_windows, sizeof(g_windows));
}

MsAppWindow *WM_Register(HWND hwnd, MsApp *app, const wchar_t *title)
{
    int i;
    for (i = 0; i < MS_MAX_APP_WINDOWS; ++i) {
        if (!g_windows[i].in_use) {
            g_windows[i].hwnd   = hwnd;
            g_windows[i].app    = app;
            g_windows[i].in_use = TRUE;
            if (title) {
                wcsncpy_s(g_windows[i].title, MS_APP_TITLE_LEN, title, _TRUNCATE);
            } else {
                g_windows[i].title[0] = L'\0';
            }
            return &g_windows[i];
        }
    }
    return NULL; /* table full */
}

void WM_Unregister(HWND hwnd)
{
    int i;
    for (i = 0; i < MS_MAX_APP_WINDOWS; ++i) {
        if (g_windows[i].in_use && g_windows[i].hwnd == hwnd) {
            ZeroMemory(&g_windows[i], sizeof(g_windows[i]));
            return;
        }
    }
}

MsAppWindow *WM_Find(HWND hwnd)
{
    int i;
    for (i = 0; i < MS_MAX_APP_WINDOWS; ++i) {
        if (g_windows[i].in_use && g_windows[i].hwnd == hwnd) {
            return &g_windows[i];
        }
    }
    return NULL;
}

int WM_Count(void)
{
    int i, count = 0;
    for (i = 0; i < MS_MAX_APP_WINDOWS; ++i) {
        if (g_windows[i].in_use) ++count;
    }
    return count;
}

MsAppWindow *WM_GetAt(int index)
{
    int i, seen = 0;
    for (i = 0; i < MS_MAX_APP_WINDOWS; ++i) {
        if (g_windows[i].in_use) {
            if (seen == index) return &g_windows[i];
            ++seen;
        }
    }
    return NULL;
}

void WM_FocusNext(void)
{
    HWND current = GetForegroundWindow();
    int i, start = 0;

    /* Find current in table */
    for (i = 0; i < MS_MAX_APP_WINDOWS; ++i) {
        if (g_windows[i].in_use && g_windows[i].hwnd == current) {
            start = i + 1;
            break;
        }
    }
    /* Cycle to next live window */
    for (i = 0; i < MS_MAX_APP_WINDOWS; ++i) {
        int idx = (start + i) % MS_MAX_APP_WINDOWS;
        if (g_windows[idx].in_use && g_windows[idx].hwnd != current) {
            SetForegroundWindow(g_windows[idx].hwnd);
            return;
        }
    }
}
