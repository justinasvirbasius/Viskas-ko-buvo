#ifndef MINISHELL_SHELL_H
#define MINISHELL_SHELL_H

#include <windows.h>

/* ---- Configuration constants ---- */
#define MS_MAX_APP_WINDOWS   16
#define MS_TASKBAR_HEIGHT    40
#define MS_APP_BUTTON_WIDTH  100
#define MS_APP_TITLE_LEN     64

#define MS_CLASS_SHELL       L"MiniShell_DesktopClass"
#define MS_CLASS_APPFRAME    L"MiniShell_AppFrameClass"

/* Custom window messages (above WM_USER) */
#define WM_MS_APP_CLOSED     (WM_USER + 1)
#define WM_MS_APP_FOCUS      (WM_USER + 2)
#define WM_MS_LAUNCH_APP     (WM_USER + 3)

/* ---- App descriptor: each demo app registers one of these ---- */
typedef struct MsApp MsApp;

typedef HWND (*MsAppCreateFn)(HWND parent, int x, int y, int w, int h, MsApp *self);

struct MsApp {
    wchar_t       title[MS_APP_TITLE_LEN];
    MsAppCreateFn create;     /* called to spawn an instance */
    int           default_w;
    int           default_h;
};

/* ---- Window manager: tracks live app windows ---- */
typedef struct {
    HWND    hwnd;
    MsApp  *app;
    wchar_t title[MS_APP_TITLE_LEN];
    BOOL    in_use;
} MsAppWindow;

/* ---- Public API ---- */

/* shell.c */
int  Shell_Run(HINSTANCE hInstance, int nCmdShow);

/* window_manager.c */
void          WM_Init(void);
MsAppWindow * WM_Register(HWND hwnd, MsApp *app, const wchar_t *title);
void          WM_Unregister(HWND hwnd);
MsAppWindow * WM_Find(HWND hwnd);
int           WM_Count(void);
MsAppWindow * WM_GetAt(int index);
void          WM_FocusNext(void);

/* events.c */
LRESULT CALLBACK Shell_WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK AppFrame_WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

/* app_registry.c */
void    Registry_Init(void);
int     Registry_Count(void);
MsApp * Registry_GetAt(int index);
void    Registry_Launch(int index, HWND desktop);

#endif /* MINISHELL_SHELL_H */
