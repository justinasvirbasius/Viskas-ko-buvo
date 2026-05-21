/*
 * app_datebook.c — Date/time picker + month calendar
 *
 * Demonstrates two common controls that are often skipped:
 *   - DateTime_GetSystemtime / DateTimePicker control (DATETIMEPICK_CLASS)
 *   - MonthCalendar control (MONTHCAL_CLASS) with selection notifications
 *
 * Selecting a date in either updates the other, plus a status label that
 * formats the result with GetDateFormatExW.
 */

#include "shell.h"
#include <commctrl.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "comctl32.lib")

#define DB_PROP  L"MS_DB_STATE"
#define ID_DB_PICK 25001
#define ID_DB_CAL  25002
#define ID_DB_LBL  25003

typedef struct {
    HWND picker, cal, label;
    BOOL syncing;
} DbState;

static WNDPROC g_origDbFrame = NULL;

static void Db_UpdateLabel(DbState *st, const SYSTEMTIME *t)
{
    wchar_t buf[128];
    wchar_t formatted[64];
    GetDateFormatExW(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, t, NULL,
                     formatted, 64, NULL);
    swprintf_s(buf, 128, L"Selected: %s", formatted);
    SetWindowTextW(st->label, buf);
}

static LRESULT CALLBACK Db_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    DbState *st = (DbState *)GetPropW(hwnd, DB_PROP);

    if (msg == WM_NOTIFY && st && !st->syncing) {
        NMHDR *hdr = (NMHDR *)lp;
        if (hdr->idFrom == ID_DB_PICK && hdr->code == DTN_DATETIMECHANGE) {
            SYSTEMTIME t;
            DateTime_GetSystemtime(st->picker, &t);
            st->syncing = TRUE;
            MonthCal_SetCurSel(st->cal, &t);
            st->syncing = FALSE;
            Db_UpdateLabel(st, &t);
            return 0;
        }
        if (hdr->idFrom == ID_DB_CAL && hdr->code == MCN_SELCHANGE) {
            SYSTEMTIME t;
            MonthCal_GetCurSel(st->cal, &t);
            st->syncing = TRUE;
            DateTime_SetSystemtime(st->picker, GDT_VALID, &t);
            st->syncing = FALSE;
            Db_UpdateLabel(st, &t);
            return 0;
        }
    }
    if (msg == WM_DESTROY && st) {
        free(st);
        RemovePropW(hwnd, DB_PROP);
    }
    return CallWindowProcW(g_origDbFrame, hwnd, msg, wp, lp);
}

static HWND DateBook_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    DbState *st;
    INITCOMMONCONTROLSEX icc;
    SYSTEMTIME now;
    (void)self;

    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_DATE_CLASSES;
    InitCommonControlsEx(&icc);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"DateBook",
        WS_POPUP | WS_BORDER,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (DbState *)calloc(1, sizeof(DbState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->picker = CreateWindowExW(0, DATETIMEPICK_CLASSW, NULL,
        WS_CHILD | WS_VISIBLE | DTS_LONGDATEFORMAT,
        16, 40, w - 32, 24, frame, (HMENU)(LONG_PTR)ID_DB_PICK, hInstance, NULL);

    st->cal = CreateWindowExW(0, MONTHCAL_CLASSW, NULL,
        WS_CHILD | WS_VISIBLE | MCS_DAYSTATE,
        16, 72, w - 32, h - 120, frame, (HMENU)(LONG_PTR)ID_DB_CAL, hInstance, NULL);

    st->label = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE,
        16, h - 40, w - 32, 22, frame, (HMENU)(LONG_PTR)ID_DB_LBL, hInstance, NULL);

    SetPropW(frame, DB_PROP, (HANDLE)st);
    if (!g_origDbFrame) g_origDbFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Db_FrameProc);

    GetLocalTime(&now);
    Db_UpdateLabel(st, &now);
    return frame;
}

MsApp g_AppDateBook = {
    L"DateBook",
    DateBook_Create,
    320, 360
};
