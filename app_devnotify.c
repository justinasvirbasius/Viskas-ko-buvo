/*
 * app_devnotify.c — Device arrival/removal notifications
 *
 * Demonstrates RegisterDeviceNotificationW + WM_DEVICECHANGE:
 *   - DEV_BROADCAST_DEVICEINTERFACE_W with a class GUID requests events
 *     for one device interface class; we use GUID_DEVINTERFACE_USB_DEVICE
 *     and the volume-arrival GUID
 *   - DEVICE_NOTIFY_WINDOW_HANDLE routes events to our HWND via
 *     WM_DEVICECHANGE; alternatives are SERVICE_HANDLE for services and
 *     ALL_INTERFACE_CLASSES to skip the GUID filter
 *   - WM_DEVICECHANGE wParam is DBT_DEVICEARRIVAL / DBT_DEVICEREMOVECOMPLETE
 *     (and related) with lParam pointing at a DEV_BROADCAST_HDR
 *   - UnregisterDeviceNotification on shutdown
 *
 * USB device interface GUID: A5DCBF10-6530-11D2-901F-00C04FB951ED
 * Volume interface GUID:      53F5630D-B6BF-11D0-94F2-00A0C91EFB8B
 */

#include "shell.h"
#include <dbt.h>
#include <stdlib.h>
#include <stdio.h>

/* Standard device interface class GUIDs. We define them inline to avoid the
   inconvenience of including <usbiodef.h> / <ntddstor.h> (which can pull in
   driver-mode headers on older SDKs). */
static const GUID DN_USB_INTERFACE_GUID = {
    0xA5DCBF10, 0x6530, 0x11D2,
    { 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED }
};
static const GUID DN_VOLUME_INTERFACE_GUID = {
    0x53F5630D, 0xB6BF, 0x11D0,
    { 0x94, 0xF2, 0x00, 0xA0, 0xC9, 0x1E, 0xFB, 0x8B }
};

#define DN_PROP    L"MS_DN_STATE"
#define ID_DN_OUT  79001
#define ID_DN_GO   79002
#define ID_DN_STOP 79003

typedef struct {
    HWND  output;
    HDEVNOTIFY hUsb, hVol;
    BOOL  watching;
} DnState;

static WNDPROC g_origDnFrame = NULL;

static void Dn_Append(DnState *st, const wchar_t *t)
{
    int len = GetWindowTextLengthW(st->output);
    SendMessageW(st->output, EM_SETSEL, len, len);
    SendMessageW(st->output, EM_REPLACESEL, FALSE, (LPARAM)t);
    SendMessageW(st->output, EM_SCROLLCARET, 0, 0);
}

static const wchar_t *Dn_EventName(WPARAM wp)
{
    switch (wp) {
    case DBT_DEVICEARRIVAL:         return L"arrival";
    case DBT_DEVICEQUERYREMOVE:     return L"query-remove";
    case DBT_DEVICEQUERYREMOVEFAILED:return L"query-remove-failed";
    case DBT_DEVICEREMOVEPENDING:   return L"remove-pending";
    case DBT_DEVICEREMOVECOMPLETE:  return L"remove-complete";
    case DBT_DEVICETYPESPECIFIC:    return L"device-type-specific";
    case DBT_CUSTOMEVENT:           return L"custom";
    }
    return L"?";
}

static void Dn_Start(HWND frame, DnState *st)
{
    DEV_BROADCAST_DEVICEINTERFACE_W filter;
    if (st->watching) return;

    ZeroMemory(&filter, sizeof(filter));
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;

    filter.dbcc_classguid = DN_USB_INTERFACE_GUID;
    st->hUsb = RegisterDeviceNotificationW(frame, &filter,
                                            DEVICE_NOTIFY_WINDOW_HANDLE);

    filter.dbcc_classguid = DN_VOLUME_INTERFACE_GUID;
    st->hVol = RegisterDeviceNotificationW(frame, &filter,
                                            DEVICE_NOTIFY_WINDOW_HANDLE);

    st->watching = TRUE;
    Dn_Append(st,
        L"Watching for USB and volume device events.\r\n"
        L"Plug or unplug a USB device, or mount/unmount a drive to see events.\r\n\r\n");
}

static void Dn_Stop(DnState *st)
{
    if (st->hUsb) { UnregisterDeviceNotification(st->hUsb); st->hUsb = NULL; }
    if (st->hVol) { UnregisterDeviceNotification(st->hVol); st->hVol = NULL; }
    st->watching = FALSE;
    Dn_Append(st, L"Stopped watching.\r\n");
}

static LRESULT CALLBACK Dn_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    DnState *st = (DnState *)GetPropW(hwnd, DN_PROP);

    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_DN_GO)   { Dn_Start(hwnd, st); return 0; }
        if (LOWORD(wp) == ID_DN_STOP) { Dn_Stop(st);        return 0; }
    }
    if (msg == WM_DEVICECHANGE && st && lp) {
        DEV_BROADCAST_HDR *hdr = (DEV_BROADCAST_HDR *)lp;
        wchar_t line[600];
        const wchar_t *name = Dn_EventName(wp);

        if (hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
            DEV_BROADCAST_DEVICEINTERFACE_W *di =
                (DEV_BROADCAST_DEVICEINTERFACE_W *)hdr;
            swprintf_s(line, 600, L"  %s  device: %s\r\n", name,
                       di->dbcc_name[0] ? di->dbcc_name : L"<no name>");
        } else if (hdr->dbch_devicetype == DBT_DEVTYP_VOLUME) {
            DEV_BROADCAST_VOLUME *v = (DEV_BROADCAST_VOLUME *)hdr;
            wchar_t letters[64] = L"";
            int i;
            for (i = 0; i < 26; ++i) {
                if (v->dbcv_unitmask & (1u << i)) {
                    wchar_t one[4];
                    swprintf_s(one, 4, L"%c ", L'A' + i);
                    wcscat_s(letters, 64, one);
                }
            }
            swprintf_s(line, 600,
                L"  %s  volume mask: %s (flags 0x%lx)\r\n",
                name, letters, v->dbcv_flags);
        } else {
            swprintf_s(line, 600, L"  %s  devtype=%lu\r\n",
                       name, hdr->dbch_devicetype);
        }
        Dn_Append(st, line);
        return TRUE;
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        Dn_Stop(st);
        free(st); RemovePropW(hwnd, DN_PROP);
    }
    return CallWindowProcW(g_origDnFrame, hwnd, msg, wp, lp);
}

static HWND DevNotify_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    DnState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"DevNotify",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (DnState *)calloc(1, sizeof(DnState));
    if (!st) { DestroyWindow(frame); return NULL; }

    CreateWindowExW(0, L"BUTTON", L"Start watching",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 140, 26, frame, (HMENU)(LONG_PTR)ID_DN_GO, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Stop",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        160, 38, 90, 26, frame, (HMENU)(LONG_PTR)ID_DN_STOP, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_DN_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, DN_PROP, (HANDLE)st);
    if (!g_origDnFrame) g_origDnFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Dn_FrameProc);
    return frame;
}

MsApp g_AppDevNotify = { L"DevNotify", DevNotify_Create, 600, 400 };
