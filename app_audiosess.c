/*
 * app_audiosess.c — Audio session enumeration via IAudioSessionManager2
 *
 * Demonstrates a different slice of WASAPI than WasapiOut (Batch 7):
 * here we don't play audio, we enumerate the audio *sessions* — one
 * per app that has opened the endpoint. Same surface that Volume
 * Mixer (sndvol.exe) uses to display per-app volume sliders.
 *
 *   - CoCreateInstance(CLSID_MMDeviceEnumerator, ...) → IMMDeviceEnumerator
 *   - GetDefaultAudioEndpoint(eRender, eConsole, &device)
 *   - IMMDevice::Activate(IID_IAudioSessionManager2, &sessMgr2)
 *   - IAudioSessionManager2::GetSessionEnumerator(&IAudioSessionEnumerator)
 *   - IAudioSessionEnumerator::GetCount + GetSession(i, &session)
 *   - IAudioSessionControl2::GetProcessId(&pid)
 *   - IAudioSessionControl::GetDisplayName(&LPWSTR) — caller CoTaskMemFree
 *   - IAudioSessionControl::GetState — Inactive / Active / Expired
 *
 * One row per session: PID, display name, state, volume level.
 */

#define COBJMACROS
#define CINTERFACE
#define INITGUID

#include "shell.h"
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <commctrl.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comctl32.lib")

#define AS_PROP   L"MS_AS_STATE"
#define ID_AS_REF 130001
#define ID_AS_LV  130002

typedef struct { HWND refresh, list; } AsState;
static WNDPROC g_origAsFrame = NULL;

static const wchar_t *As_StateName(AudioSessionState s)
{
    switch (s) {
    case AudioSessionStateInactive: return L"INACTIVE";
    case AudioSessionStateActive:   return L"ACTIVE";
    case AudioSessionStateExpired:  return L"EXPIRED";
    }
    return L"?";
}

static void As_Refresh(AsState *st)
{
    IMMDeviceEnumerator    *en = NULL;
    IMMDevice              *dev = NULL;
    IAudioSessionManager2  *mgr = NULL;
    IAudioSessionEnumerator *sessEn = NULL;
    HRESULT hr;
    int count = 0, i;

    SendMessageW(st->list, LVM_DELETEALLITEMS, 0, 0);

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IMMDeviceEnumerator, (void **)&en);
    if (FAILED(hr) || !en) {
        LVITEMW it; wchar_t buf[80];
        swprintf_s(buf, 80, L"(CoCreateInstance MMDeviceEnumerator: 0x%08lx)", hr);
        ZeroMemory(&it, sizeof(it)); it.mask = LVIF_TEXT; it.pszText = buf;
        SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        return;
    }

    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    IMMDeviceEnumerator_Release(en);
    if (FAILED(hr) || !dev) {
        LVITEMW it;
        ZeroMemory(&it, sizeof(it)); it.mask = LVIF_TEXT;
        it.pszText = (LPWSTR)L"(no default render endpoint)";
        SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        return;
    }

    hr = IMMDevice_Activate(dev, &IID_IAudioSessionManager2, CLSCTX_INPROC_SERVER,
                            NULL, (void **)&mgr);
    IMMDevice_Release(dev);
    if (FAILED(hr) || !mgr) {
        LVITEMW it; wchar_t buf[80];
        swprintf_s(buf, 80, L"(Activate IAudioSessionManager2: 0x%08lx)", hr);
        ZeroMemory(&it, sizeof(it)); it.mask = LVIF_TEXT; it.pszText = buf;
        SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        return;
    }

    hr = IAudioSessionManager2_GetSessionEnumerator(mgr, &sessEn);
    IAudioSessionManager2_Release(mgr);
    if (FAILED(hr) || !sessEn) {
        LVITEMW it; wchar_t buf[80];
        swprintf_s(buf, 80, L"(GetSessionEnumerator: 0x%08lx)", hr);
        ZeroMemory(&it, sizeof(it)); it.mask = LVIF_TEXT; it.pszText = buf;
        SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        return;
    }

    IAudioSessionEnumerator_GetCount(sessEn, &count);

    for (i = 0; i < count; ++i) {
        IAudioSessionControl  *sess = NULL;
        IAudioSessionControl2 *sess2 = NULL;
        ISimpleAudioVolume    *vol = NULL;
        if (FAILED(IAudioSessionEnumerator_GetSession(sessEn, i, &sess)) || !sess) continue;

        if (SUCCEEDED(IAudioSessionControl_QueryInterface(sess, &IID_IAudioSessionControl2, (void **)&sess2)) &&
            SUCCEEDED(IAudioSessionControl_QueryInterface(sess, &IID_ISimpleAudioVolume, (void **)&vol))) {
            DWORD pid = 0;
            LPWSTR name = NULL;
            AudioSessionState state = AudioSessionStateInactive;
            float level = 0;
            wchar_t pidStr[16], levelStr[16];
            LVITEMW it;

            IAudioSessionControl2_GetProcessId(sess2, &pid);
            IAudioSessionControl_GetDisplayName(sess, &name);
            IAudioSessionControl_GetState(sess, &state);
            ISimpleAudioVolume_GetMasterVolume(vol, &level);
            swprintf_s(pidStr,   16, L"%lu", pid);
            swprintf_s(levelStr, 16, L"%.2f", level);

            ZeroMemory(&it, sizeof(it));
            it.mask = LVIF_TEXT; it.iItem = i;
            it.pszText = pidStr; SendMessageW(st->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
            it.iSubItem = 1; it.pszText = name && name[0] ? name : (LPWSTR)L"(unnamed)";
            SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
            it.iSubItem = 2; it.pszText = (LPWSTR)As_StateName(state);
            SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);
            it.iSubItem = 3; it.pszText = levelStr;
            SendMessageW(st->list, LVM_SETITEMW, 0, (LPARAM)&it);

            if (name) CoTaskMemFree(name);
        }
        if (vol)   ISimpleAudioVolume_Release(vol);
        if (sess2) IAudioSessionControl2_Release(sess2);
        IAudioSessionControl_Release(sess);
    }

    IAudioSessionEnumerator_Release(sessEn);
}

static LRESULT CALLBACK As_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    AsState *st = (AsState *)GetPropW(hwnd, AS_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_AS_REF) { As_Refresh(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->refresh, 12, 38, 110, 26, TRUE);
        MoveWindow(st->list,    8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, AS_PROP); }
    return CallWindowProcW(g_origAsFrame, hwnd, msg, wp, lp);
}

static HWND AudioSess_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    AsState *st;
    INITCOMMONCONTROLSEX icc;
    LVCOLUMNW col;
    (void)self;

    icc.dwSize = sizeof(icc); icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"AudioSess",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (AsState *)calloc(1, sizeof(AsState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->refresh = CreateWindowExW(0, L"BUTTON", L"Refresh",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 110, 26, frame, (HMENU)(LONG_PTR)ID_AS_REF, hInstance, NULL);

    st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
        WS_CHILD | WS_VISIBLE | LVS_REPORT,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_AS_LV, hInstance, NULL);
    SendMessageW(st->list, LVM_SETEXTENDEDLISTVIEWSTYLE,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 80;  col.pszText = (LPWSTR)L"PID";         SendMessageW(st->list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.cx = 340; col.pszText = (LPWSTR)L"Display name"; SendMessageW(st->list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    col.cx = 100; col.pszText = (LPWSTR)L"State";       SendMessageW(st->list, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);
    col.cx = 90;  col.pszText = (LPWSTR)L"Volume";      SendMessageW(st->list, LVM_INSERTCOLUMNW, 3, (LPARAM)&col);

    SetPropW(frame, AS_PROP, (HANDLE)st);
    if (!g_origAsFrame) g_origAsFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)As_FrameProc);
    As_Refresh(st);
    return frame;
}

MsApp g_AppAudioSess = { L"AudioSess", AudioSess_Create, 760, 440 };
