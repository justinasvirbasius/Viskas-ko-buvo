/*
 * app_power.c — Power state and execution-state control
 *
 * Demonstrates:
 *   - GetSystemPowerStatus for battery and AC info
 *   - SetThreadExecutionState with ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED |
 *     ES_CONTINUOUS to keep the system awake (the "presentation" trick)
 *   - WM_POWERBROADCAST notifications when the power source changes
 *
 * Three buttons toggle keep-awake state. A timer polls power status once
 * a second so battery percentage updates live.
 */

#include "shell.h"
#include <stdlib.h>
#include <stdio.h>

#define PW_PROP    L"MS_PW_STATE"
#define ID_PW_ON   42001
#define ID_PW_OFF  42002
#define ID_PW_DISP 42003
#define ID_PW_LBL  42004
#define ID_PW_KEEP 42005
#define PW_TIMER   1

typedef struct {
    HWND infoLbl, keepLbl;
    BOOL keepAwake;
    BOOL keepDisplay;
} PwState;

static WNDPROC g_origPwFrame = NULL;

static void Pw_UpdateInfo(PwState *st)
{
    SYSTEM_POWER_STATUS sps;
    wchar_t buf[400];

    if (!GetSystemPowerStatus(&sps)) {
        SetWindowTextW(st->infoLbl, L"GetSystemPowerStatus failed.");
        return;
    }
    swprintf_s(buf, 400,
        L"AC line status: %s\r\n"
        L"Battery: %s%s%s\r\n"
        L"Battery life: %s\r\n"
        L"Time remaining: %s",
        sps.ACLineStatus == 0 ? L"offline"
            : sps.ACLineStatus == 1 ? L"online"
            : L"unknown",
        (sps.BatteryFlag == 128) ? L"no system battery" :
        (sps.BatteryFlag & 8) ? L"charging" :
        (sps.BatteryFlag & 4) ? L"critical" :
        (sps.BatteryFlag & 2) ? L"low" :
        (sps.BatteryFlag & 1) ? L"high" : L"present",
        L"", L"",
        sps.BatteryLifePercent == 255 ? L"unknown" : L"see below",
        sps.BatteryLifeTime == 0xFFFFFFFF ? L"unknown" : L"see below");
    {
        wchar_t pct[40], tim[40];
        if (sps.BatteryLifePercent == 255) wcscpy_s(pct, 40, L"unknown");
        else swprintf_s(pct, 40, L"%u %%", sps.BatteryLifePercent);
        if (sps.BatteryLifeTime == 0xFFFFFFFF) wcscpy_s(tim, 40, L"unknown");
        else {
            DWORD s = sps.BatteryLifeTime;
            swprintf_s(tim, 40, L"%lu:%02lu:%02lu",
                       s / 3600, (s / 60) % 60, s % 60);
        }
        swprintf_s(buf, 400,
            L"AC line status: %s\r\n"
            L"Battery flag: 0x%02X\r\n"
            L"Battery life: %s\r\n"
            L"Time remaining: %s",
            sps.ACLineStatus == 0 ? L"offline"
                : sps.ACLineStatus == 1 ? L"online"
                : L"unknown",
            sps.BatteryFlag, pct, tim);
        SetWindowTextW(st->infoLbl, buf);
    }
}

static void Pw_UpdateKeep(PwState *st)
{
    EXECUTION_STATE es = ES_CONTINUOUS;
    wchar_t buf[120];
    if (st->keepAwake)   es |= ES_SYSTEM_REQUIRED;
    if (st->keepDisplay) es |= ES_DISPLAY_REQUIRED;
    SetThreadExecutionState(es);

    swprintf_s(buf, 120, L"Keep awake: %s   |   Keep display on: %s",
               st->keepAwake   ? L"YES" : L"no",
               st->keepDisplay ? L"YES" : L"no");
    SetWindowTextW(st->keepLbl, buf);
}

static LRESULT CALLBACK Pw_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    PwState *st = (PwState *)GetPropW(hwnd, PW_PROP);

    if (msg == WM_COMMAND && st) {
        switch (LOWORD(wp)) {
        case ID_PW_ON:   st->keepAwake   = !st->keepAwake;   Pw_UpdateKeep(st); return 0;
        case ID_PW_DISP: st->keepDisplay = !st->keepDisplay; Pw_UpdateKeep(st); return 0;
        case ID_PW_OFF:
            st->keepAwake = st->keepDisplay = FALSE;
            Pw_UpdateKeep(st);
            return 0;
        }
    }
    if (msg == WM_TIMER && st) { Pw_UpdateInfo(st); return 0; }
    if (msg == WM_POWERBROADCAST && st) { Pw_UpdateInfo(st); }
    if (msg == WM_DESTROY && st) {
        KillTimer(hwnd, PW_TIMER);
        SetThreadExecutionState(ES_CONTINUOUS);
        free(st);
        RemovePropW(hwnd, PW_PROP);
    }
    return CallWindowProcW(g_origPwFrame, hwnd, msg, wp, lp);
}

static HWND Power_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    PwState *st;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Power",
        WS_POPUP | WS_BORDER,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (PwState *)calloc(1, sizeof(PwState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->infoLbl = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE,
        12, 36, w - 24, 80, frame, (HMENU)(LONG_PTR)ID_PW_LBL, hInstance, NULL);

    CreateWindowExW(0, L"BUTTON", L"Toggle keep-awake",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        12, 124, 160, 28, frame, (HMENU)(LONG_PTR)ID_PW_ON, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Toggle display-on",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        180, 124, 160, 28, frame, (HMENU)(LONG_PTR)ID_PW_DISP, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Reset",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        348, 124, 80, 28, frame, (HMENU)(LONG_PTR)ID_PW_OFF, hInstance, NULL);

    st->keepLbl = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE,
        12, 162, w - 24, 22, frame, (HMENU)(LONG_PTR)ID_PW_KEEP, hInstance, NULL);

    SetPropW(frame, PW_PROP, (HANDLE)st);
    if (!g_origPwFrame) g_origPwFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Pw_FrameProc);

    Pw_UpdateInfo(st);
    Pw_UpdateKeep(st);
    SetTimer(frame, PW_TIMER, 1000, NULL);
    return frame;
}

MsApp g_AppPower = {
    L"Power",
    Power_Create,
    480, 230
};
