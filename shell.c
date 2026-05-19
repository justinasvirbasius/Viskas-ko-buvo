/*
 * shell.c — Desktop shell window and message loop
 *
 * Responsibilities:
 *   - Register window classes (desktop + app frame)
 *   - Create the full-screen desktop window
 *   - Draw the taskbar with one launcher button per registered app
 *   - Pump the Win32 message loop until the user quits
 */

#include "shell.h"
#include <stdio.h>

static HINSTANCE g_hInstance = NULL;

/* ---- Internal: register both window classes ---- */
static BOOL Shell_RegisterClasses(HINSTANCE hInstance)
{
    WNDCLASSEXW wc;

    /* Desktop class */
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = Shell_WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(30, 60, 100));
    wc.lpszClassName = MS_CLASS_SHELL;
    if (!RegisterClassExW(&wc)) return FALSE;

    /* App frame class — each app window uses this as its outer frame */
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = AppFrame_WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = MS_CLASS_APPFRAME;
    if (!RegisterClassExW(&wc)) return FALSE;

    return TRUE;
}

/* ---- Public entry: build everything and run the loop ---- */
int Shell_Run(HINSTANCE hInstance, int nCmdShow)
{
    MSG msg;
    HWND hShell;
    int screenW, screenH;

    g_hInstance = hInstance;

    if (!Shell_RegisterClasses(hInstance)) {
        MessageBoxW(NULL, L"Failed to register window classes.", L"MiniShell", MB_ICONERROR);
        return 1;
    }

    WM_Init();
    Registry_Init();

    screenW = GetSystemMetrics(SM_CXSCREEN);
    screenH = GetSystemMetrics(SM_CYSCREEN);

    hShell = CreateWindowExW(
        0,
        MS_CLASS_SHELL,
        L"MiniShell Desktop",
        WS_POPUP,                  /* borderless desktop */
        0, 0, screenW, screenH,
        NULL, NULL, hInstance, NULL);

    if (!hShell) {
        MessageBoxW(NULL, L"Failed to create shell window.", L"MiniShell", MB_ICONERROR);
        return 1;
    }

    ShowWindow(hShell, nCmdShow);
    UpdateWindow(hShell);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return (int)msg.wParam;
}
