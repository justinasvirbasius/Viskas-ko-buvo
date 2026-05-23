/*
 * app_timezones.c — Time-zone enumeration and conversion
 *
 * Demonstrates the time-zone APIs:
 *   - EnumDynamicTimeZoneInformation(index, &dtzi) walks the Windows time-zone
 *     registry, returning DYNAMIC_TIME_ZONE_INFORMATION records with
 *     standard/daylight names, biases, and friendly display strings
 *   - SystemTimeToTzSpecificLocalTimeEx(&dtzi, &utc, &local) converts a UTC
 *     SYSTEMTIME to a given zone's local time, respecting DST rules
 *   - GetSystemTime to get current UTC
 *
 * Top: combo of every installed zone (sorted by name). Bottom: a 4-zone
 * "world clock" with a 1-second tick refresh.
 */

#include "shell.h"
#include <timezoneapi.h>
#include <stdlib.h>
#include <stdio.h>

#define TZ_PROP    L"MS_TZ_STATE"
#define ID_TZ_CMB  78001
#define ID_TZ_ADD  78002
#define ID_TZ_CLR  78003
#define ID_TZ_OUT  78004
#define ID_TZ_CLOCK 78005
#define TZ_TIMER   1

#define MAX_PICKED 6

typedef struct {
    HWND     combo, addBtn, clrBtn, output, clockLbl;
    UINT_PTR timerId;
    DYNAMIC_TIME_ZONE_INFORMATION picked[MAX_PICKED];
    int      pickedCount;
} TzState;

static WNDPROC g_origTzFrame = NULL;

static void Tz_FillCombo(TzState *st)
{
    DWORD i = 0;
    DYNAMIC_TIME_ZONE_INFORMATION dtzi;

    SendMessageW(st->combo, CB_RESETCONTENT, 0, 0);
    while (EnumDynamicTimeZoneInformation(i, &dtzi) == ERROR_SUCCESS) {
        int idx = (int)SendMessageW(st->combo, CB_ADDSTRING, 0,
                                     (LPARAM)dtzi.StandardName);
        SendMessageW(st->combo, CB_SETITEMDATA, idx, (LPARAM)i);
        ++i;
    }
    if (i > 0) SendMessageW(st->combo, CB_SETCURSEL, 0, 0);
}

static void Tz_Add(TzState *st)
{
    int sel;
    DWORD enumIdx;
    DYNAMIC_TIME_ZONE_INFORMATION dtzi;

    if (st->pickedCount >= MAX_PICKED) return;
    sel = (int)SendMessageW(st->combo, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) return;
    enumIdx = (DWORD)SendMessageW(st->combo, CB_GETITEMDATA, sel, 0);
    if (EnumDynamicTimeZoneInformation(enumIdx, &dtzi) != ERROR_SUCCESS) return;
    st->picked[st->pickedCount++] = dtzi;
}

static void Tz_Clear(TzState *st)
{
    st->pickedCount = 0;
    SetWindowTextW(st->clockLbl, L"");
}

static void Tz_RefreshClocks(TzState *st)
{
    wchar_t buf[2048];
    SYSTEMTIME utc, local;
    int i, len = 0;

    GetSystemTime(&utc);

    /* Always show local first */
    GetLocalTime(&local);
    len += swprintf_s(buf, 2048,
        L"== UTC ==        %04d-%02d-%02d %02d:%02d:%02d\r\n"
        L"== Local ==      %04d-%02d-%02d %02d:%02d:%02d\r\n\r\n",
        utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond,
        local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond);

    for (i = 0; i < st->pickedCount; ++i) {
        SYSTEMTIME zoneTime;
        if (SystemTimeToTzSpecificLocalTimeEx(&st->picked[i], &utc, &zoneTime)) {
            len += swprintf_s(buf + len, 2048 - len,
                L"%-30s %04d-%02d-%02d %02d:%02d:%02d\r\n",
                st->picked[i].StandardName,
                zoneTime.wYear, zoneTime.wMonth, zoneTime.wDay,
                zoneTime.wHour, zoneTime.wMinute, zoneTime.wSecond);
        }
    }
    SetWindowTextW(st->clockLbl, buf);
}

static LRESULT CALLBACK Tz_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    TzState *st = (TzState *)GetPropW(hwnd, TZ_PROP);
    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_TZ_ADD) { Tz_Add(st);   Tz_RefreshClocks(st); return 0; }
        if (LOWORD(wp) == ID_TZ_CLR) { Tz_Clear(st);                       return 0; }
    }
    if (msg == WM_TIMER && st) { Tz_RefreshClocks(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->combo,    12, 38, w - 220, 24, TRUE);
        MoveWindow(st->addBtn,   w - 200, 38, 90, 24, TRUE);
        MoveWindow(st->clrBtn,   w - 104, 38, 90, 24, TRUE);
        MoveWindow(st->clockLbl, 8,  74, w - 16, h - 82, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->timerId) KillTimer(hwnd, st->timerId);
        free(st); RemovePropW(hwnd, TZ_PROP);
    }
    return CallWindowProcW(g_origTzFrame, hwnd, msg, wp, lp);
}

static HWND TimeZones_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    TzState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"TimeZones",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (TzState *)calloc(1, sizeof(TzState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->combo = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        12, 38, w - 220, 200, frame, (HMENU)(LONG_PTR)ID_TZ_CMB, hInstance, NULL);
    st->addBtn = CreateWindowExW(0, L"BUTTON", L"Add",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        w - 200, 38, 90, 24, frame, (HMENU)(LONG_PTR)ID_TZ_ADD, hInstance, NULL);
    st->clrBtn = CreateWindowExW(0, L"BUTTON", L"Clear",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        w - 104, 38, 90, 24, frame, (HMENU)(LONG_PTR)ID_TZ_CLR, hInstance, NULL);

    st->clockLbl = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY,
        8, 74, w - 16, h - 82, frame, (HMENU)(LONG_PTR)ID_TZ_CLOCK, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->clockLbl, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, TZ_PROP, (HANDLE)st);
    if (!g_origTzFrame) g_origTzFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Tz_FrameProc);
    Tz_FillCombo(st);
    Tz_RefreshClocks(st);
    st->timerId = SetTimer(frame, TZ_TIMER, 1000, NULL);
    return frame;
}

MsApp g_AppTimeZones = { L"TimeZones", TimeZones_Create, 640, 440 };
