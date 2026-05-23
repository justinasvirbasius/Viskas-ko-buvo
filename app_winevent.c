/*
 * app_winevent.c — Global accessibility event observer
 *
 * Demonstrates SetWinEventHook, the modern (since Windows 2000) accessibility
 * event mechanism — distinct from SetWindowsHookEx in that:
 *   - It uses WINEVENT_OUTOFCONTEXT, so the callback is marshalled back to
 *     the installing thread's message pump (no external DLL needed for
 *     cross-process observation)
 *   - It receives high-level UI events (foreground change, focus change,
 *     window create/destroy, menu open, name change) rather than raw input
 *
 * Hooks installed:
 *   - EVENT_SYSTEM_FOREGROUND    — the foreground window changed
 *   - EVENT_OBJECT_NAMECHANGE    — a window/control title changed
 *   - EVENT_OBJECT_CREATE        — a UI object was created
 *   - EVENT_OBJECT_DESTROY       — a UI object was destroyed
 *
 * We skip our own process via WINEVENT_SKIPOWNPROCESS so we don't observe
 * ourselves. For each event we capture the HWND and its window title.
 */

#include "shell.h"
#include <stdlib.h>
#include <stdio.h>

#define WE_PROP    L"MS_WE_STATE"
#define ID_WE_GO   73001
#define ID_WE_STOP 73002
#define ID_WE_OUT  73003
#define ID_WE_CLR  73004

typedef struct {
    HWINEVENTHOOK hooks[4];
    HWND          frame, output;
    int           eventCount;
} WeState;

static WNDPROC g_origWeFrame = NULL;
static WeState *g_weCurrent  = NULL;

static const wchar_t *We_Name(DWORD ev)
{
    switch (ev) {
    case EVENT_SYSTEM_FOREGROUND: return L"foreground";
    case EVENT_OBJECT_NAMECHANGE: return L"name-change";
    case EVENT_OBJECT_CREATE:     return L"create";
    case EVENT_OBJECT_DESTROY:    return L"destroy";
    }
    return L"?";
}

static void CALLBACK We_HookProc(HWINEVENTHOOK hook, DWORD event,
                                  HWND hwnd, LONG idObject, LONG idChild,
                                  DWORD eventThread, DWORD eventTime)
{
    wchar_t *payload;
    wchar_t title[200] = L"";
    wchar_t cls[100] = L"";
    (void)hook; (void)idChild; (void)eventThread; (void)eventTime;

    /* Object-level events for non-window objects produce a lot of noise;
       only report objid 0 (the window itself) for create/destroy. */
    if ((event == EVENT_OBJECT_CREATE || event == EVENT_OBJECT_DESTROY) &&
        idObject != OBJID_WINDOW) return;

    if (hwnd) {
        GetWindowTextW(hwnd, title, 200);
        GetClassNameW(hwnd, cls, 100);
    }

    payload = (wchar_t *)malloc(400 * sizeof(wchar_t));
    if (!payload) return;
    swprintf_s(payload, 400, L"  %-13s  hwnd=0x%p  [%s]  \"%s\"\r\n",
               We_Name(event), (void *)hwnd, cls, title);
    if (g_weCurrent) {
        PostMessageW(g_weCurrent->frame, WM_APP + 1, 0, (LPARAM)payload);
    } else {
        free(payload);
    }
}

static void We_Start(WeState *st)
{
    int i;
    DWORD events[4][2] = {
        { EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND },
        { EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE },
        { EVENT_OBJECT_CREATE,     EVENT_OBJECT_CREATE     },
        { EVENT_OBJECT_DESTROY,    EVENT_OBJECT_DESTROY    },
    };

    for (i = 0; i < 4; ++i) {
        if (st->hooks[i]) continue;
        st->hooks[i] = SetWinEventHook(
            events[i][0], events[i][1],
            NULL, We_HookProc, 0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    }
    g_weCurrent = st;
    {
        int len = GetWindowTextLengthW(st->output);
        const wchar_t *t = L"\r\n[hooks installed]\r\n";
        SendMessageW(st->output, EM_SETSEL, len, len);
        SendMessageW(st->output, EM_REPLACESEL, FALSE, (LPARAM)t);
    }
}

static void We_Stop(WeState *st)
{
    int i;
    for (i = 0; i < 4; ++i) {
        if (st->hooks[i]) {
            UnhookWinEvent(st->hooks[i]);
            st->hooks[i] = NULL;
        }
    }
    if (g_weCurrent == st) g_weCurrent = NULL;
    {
        int len = GetWindowTextLengthW(st->output);
        const wchar_t *t = L"\r\n[hooks removed]\r\n";
        SendMessageW(st->output, EM_SETSEL, len, len);
        SendMessageW(st->output, EM_REPLACESEL, FALSE, (LPARAM)t);
    }
}

static LRESULT CALLBACK We_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    WeState *st = (WeState *)GetPropW(hwnd, WE_PROP);
    if (msg == WM_COMMAND && st) {
        switch (LOWORD(wp)) {
        case ID_WE_GO:   We_Start(st); return 0;
        case ID_WE_STOP: We_Stop(st);  return 0;
        case ID_WE_CLR:  SetWindowTextW(st->output, L""); st->eventCount = 0; return 0;
        }
    }
    if (msg == WM_APP + 1 && st) {
        wchar_t *p = (wchar_t *)lp;
        if (p) {
            int len = GetWindowTextLengthW(st->output);
            SendMessageW(st->output, EM_SETSEL, len, len);
            SendMessageW(st->output, EM_REPLACESEL, FALSE, (LPARAM)p);
            SendMessageW(st->output, EM_SCROLLCARET, 0, 0);
            ++st->eventCount;
            free(p);
            /* Cap to ~5000 chars to keep the edit responsive */
            if (st->eventCount % 50 == 0) {
                int cur = GetWindowTextLengthW(st->output);
                if (cur > 8000) {
                    SendMessageW(st->output, EM_SETSEL, 0, cur / 2);
                    SendMessageW(st->output, EM_REPLACESEL, FALSE, (LPARAM)L"");
                }
            }
        }
        return 0;
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        We_Stop(st);
        free(st);
        RemovePropW(hwnd, WE_PROP);
    }
    return CallWindowProcW(g_origWeFrame, hwnd, msg, wp, lp);
}

static HWND WinEvent_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    WeState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"WinEvent",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (WeState *)calloc(1, sizeof(WeState));
    if (!st) { DestroyWindow(frame); return NULL; }
    st->frame = frame;

    CreateWindowExW(0, L"BUTTON", L"Start hooks",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 130, 26, frame, (HMENU)(LONG_PTR)ID_WE_GO, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Stop",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        150, 38, 90, 26, frame, (HMENU)(LONG_PTR)ID_WE_STOP, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Clear",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        248, 38, 80, 26, frame, (HMENU)(LONG_PTR)ID_WE_CLR, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_WE_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, WE_PROP, (HANDLE)st);
    if (!g_origWeFrame) g_origWeFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)We_FrameProc);
    return frame;
}

MsApp g_AppWinEvent = { L"WinEvent", WinEvent_Create, 700, 460 };
