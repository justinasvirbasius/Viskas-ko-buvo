/*
 * main.c — Entry point for MiniShell
 *
 * MiniShell is a tiny Windows desktop shell: a parent window that acts as the
 * "desktop", a taskbar at the bottom with launchers for built-in apps, and
 * child app windows that float over the desktop and can be focused/closed.
 */

#include "shell.h"

int WINAPI wWinMain(HINSTANCE hInstance,
                    HINSTANCE hPrevInstance,
                    LPWSTR    lpCmdLine,
                    int       nCmdShow)
{
    (void)hPrevInstance;
    (void)lpCmdLine;
    return Shell_Run(hInstance, nCmdShow);
}
