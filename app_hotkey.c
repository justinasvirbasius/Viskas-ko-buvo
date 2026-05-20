/*
 * app_hotkey.c — System-wide hotkey registration
 *
 * Demonstrates:
 *   - RegisterHotKey for global hotkeys that fire even when this window
 *     doesn't have focus
 *   - WM_HOTKEY handling
 *   - UnregisterHotKey on cleanup
 *
 * Registers Ctrl+Alt+M and Ctrl+Alt+L by default. When fired, increments
 * the corresponding counter and flashes the window briefly via FlashWindowEx.
 */

#include "shell.h"
#include <stdlib.h>
#include <stdio.h>

#define HK_PROP    L"MS_HK_STATE"
#define ID_REG_M   17001
#define ID_REG_L   17002

/* Hotkey IDs are app-local */
#define HK_ID_M    1
#define HK_ID_L    2

typedef struct {
    HWND statusM, statusL, info;
    int  countM, countL;
    BOOL regM, regL;
    HWND regMBtn, regLBtn;
} HkState;

static WNDPROC g_origHkFrame = NULL;

static void Hk_UpdateLabels(HkState *st)
{
    wchar_t buf[80];
    swprintf_s(buf, 80, L"Ctrl+Alt+M  fired %d times  (%s)",
               st->countM, st->regM ? L"registered" : L"OFF");
    SetWindowTextW(st->statusM, buf);
    swprintf_s(buf, 80, L"Ctrl+Alt+L  fired %d times  (%s)",
               st->countL, st->regL ? L"registered" : L"OFF");
    SetWindowTextW(st->statusL, buf);

    SetWindowTextW(st->regMBtn, st->regM ? L"Unregister M" : L"Register M");
    SetWindowTextW(st->regLBtn, st->regL ? L"Unregister L" : L"Register L");
}

static void Hk_Toggle(HWND frame, HkState *st, int id, BOOL *flag, UINT key)
{
    if (*flag) {
        UnregisterHotKey(frame, id);
        *flag = FALSE;
    } else {
        if (RegisterHotKey(frame, id, MOD_CONTROL | MOD_ALT, key)) {
            *flag = TRUE;
        } else {
            MessageBoxW(frame, L"RegisterHotKey failed.\nThis combination may "
                        L"already be in use system-wide.",
                        L"HotKey", MB_ICONWARNING);
        }
    }
    Hk_UpdateLabels(st);
}

static LRESULT CALLBACK Hk_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    HkState *st = (HkState *)GetPropW(hwnd, HK_PROP);

    if (msg == WM_HOTKEY && st) {
        FLASHWINFO fi;
        if (wp == HK_ID_M) ++st->countM;
        if (wp == HK_ID_L) ++st->countL;
        Hk_UpdateLabels(st);

        fi.cbSize    = sizeof(fi);
        fi.hwnd      = hwnd;
        fi.dwFlags   = FLASHW_CAPTION;
        fi.uCount    = 2;
        fi.dwTimeout = 80;
        FlashWindowEx(&fi);
        return 0;
    }
    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_REG_M) {
            Hk_Toggle(hwnd, st, HK_ID_M, &st->regM, 'M');
            return 0;
        }
        if (LOWORD(wp) == ID_REG_L) {
            Hk_Toggle(hwnd, st, HK_ID_L, &st->regL, 'L');
            return 0;
        }
    }
    if (msg == WM_DESTROY && st) {
        if (st->regM) UnregisterHotKey(hwnd, HK_ID_M);
        if (st->regL) UnregisterHotKey(hwnd, HK_ID_L);
        free(st);
        RemovePropW(hwnd, HK_PROP);
    }
    return CallWindowProcW(g_origHkFrame, hwnd, msg, wp, lp);
}

static HWND HotKey_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    HkState *st;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"HotKey",
        WS_POPUP | WS_BORDER,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (HkState *)calloc(1, sizeof(HkState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->info = CreateWindowExW(0, L"STATIC",
        L"These hotkeys fire even when this window is not focused.",
        WS_CHILD | WS_VISIBLE,
        12, 40, w - 24, 20, frame, NULL, hInstance, NULL);

    st->statusM = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE,
        12, 70, w - 24, 20, frame, NULL, hInstance, NULL);

    st->regMBtn = CreateWindowExW(0, L"BUTTON", L"Register M",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        12, 94, 130, 28, frame, (HMENU)(LONG_PTR)ID_REG_M, hInstance, NULL);

    st->statusL = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE,
        12, 138, w - 24, 20, frame, NULL, hInstance, NULL);

    st->regLBtn = CreateWindowExW(0, L"BUTTON", L"Register L",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        12, 162, 130, 28, frame, (HMENU)(LONG_PTR)ID_REG_L, hInstance, NULL);

    SetPropW(frame, HK_PROP, (HANDLE)st);
    if (!g_origHkFrame)
        g_origHkFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Hk_FrameProc);

    Hk_UpdateLabels(st);
    return frame;
}

MsApp g_AppHotKey = {
    L"HotKey",
    HotKey_Create,
    320, 220
};
