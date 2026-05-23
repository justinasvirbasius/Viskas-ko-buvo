/*
 * app_uiauto.c — UI Automation focused-element inspector
 *
 * Demonstrates Microsoft UI Automation, the accessibility framework that
 * supersedes MSAA:
 *   - CoCreateInstance(CLSID_CUIAutomation) → IUIAutomation
 *   - IUIAutomation::GetFocusedElement → IUIAutomationElement
 *   - IUIAutomationElement::get_CurrentName / ControlType / ClassName /
 *     BoundingRectangle / IsEnabled / ProcessId
 *   - IUIAutomation::ElementFromHandle to anchor at a specific HWND
 *
 * Unlike SetWinEventHook (Batch 11 WinEvent), UI Automation gives a *tree*
 * — we can ask for the focused control regardless of process, get its
 * accessible name, and inspect its semantic role.
 *
 * A 1-second timer re-queries the focused element so the display follows
 * the user's focus across the desktop.
 */

#define COBJMACROS
#define CINTERFACE
#define INITGUID

#include "shell.h"
#include <objbase.h>
#include <uiautomation.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "ole32.lib")

#define UA_PROP    L"MS_UA_STATE"
#define ID_UA_OUT  85001
#define ID_UA_GO   85002
#define ID_UA_STOP 85003
#define UA_TIMER   1

typedef struct {
    HWND      output;
    IUIAutomation *uia;
    BOOL      comOk;
    UINT_PTR  timer;
} UaState;

static WNDPROC g_origUaFrame = NULL;

static const wchar_t *Ua_ControlTypeName(int ct)
{
    /* UIA_*ControlTypeId constants (uiautomationclient.h) */
    switch (ct) {
    case 50000: return L"Button";
    case 50001: return L"Calendar";
    case 50002: return L"CheckBox";
    case 50003: return L"ComboBox";
    case 50004: return L"Edit";
    case 50005: return L"Hyperlink";
    case 50006: return L"Image";
    case 50007: return L"ListItem";
    case 50008: return L"List";
    case 50009: return L"Menu";
    case 50010: return L"MenuBar";
    case 50011: return L"MenuItem";
    case 50012: return L"ProgressBar";
    case 50013: return L"RadioButton";
    case 50014: return L"ScrollBar";
    case 50015: return L"Slider";
    case 50016: return L"Spinner";
    case 50017: return L"StatusBar";
    case 50018: return L"Tab";
    case 50019: return L"TabItem";
    case 50020: return L"Text";
    case 50021: return L"ToolBar";
    case 50022: return L"ToolTip";
    case 50023: return L"Tree";
    case 50024: return L"TreeItem";
    case 50025: return L"Custom";
    case 50026: return L"Group";
    case 50027: return L"Thumb";
    case 50028: return L"DataGrid";
    case 50029: return L"DataItem";
    case 50030: return L"Document";
    case 50031: return L"SplitButton";
    case 50032: return L"Window";
    case 50033: return L"Pane";
    case 50034: return L"Header";
    case 50035: return L"HeaderItem";
    case 50036: return L"Table";
    case 50037: return L"TitleBar";
    case 50038: return L"Separator";
    }
    return L"?";
}

static void Ua_QueryFocused(UaState *st)
{
    IUIAutomationElement *elem = NULL;
    HRESULT hr;
    wchar_t out[2048];
    int len = 0;

    if (!st->uia) return;
    hr = IUIAutomation_GetFocusedElement(st->uia, &elem);
    if (FAILED(hr) || !elem) {
        SetWindowTextW(st->output, L"(no focused element)\r\n");
        return;
    }

    {
        BSTR name = NULL, className = NULL, frameworkId = NULL;
        CONTROLTYPEID ct = 0;
        RECT br = {0};
        int pid = 0;
        BOOL enabled = FALSE, isOffscreen = FALSE;

        IUIAutomationElement_get_CurrentName(elem, &name);
        IUIAutomationElement_get_CurrentClassName(elem, &className);
        IUIAutomationElement_get_CurrentFrameworkId(elem, &frameworkId);
        IUIAutomationElement_get_CurrentControlType(elem, &ct);
        IUIAutomationElement_get_CurrentBoundingRectangle(elem, &br);
        IUIAutomationElement_get_CurrentProcessId(elem, &pid);
        IUIAutomationElement_get_CurrentIsEnabled(elem, &enabled);
        IUIAutomationElement_get_CurrentIsOffscreen(elem, &isOffscreen);

        len = swprintf_s(out, 2048,
            L"=== Focused UIA element ===\r\n"
            L"  Name        : %s\r\n"
            L"  ClassName   : %s\r\n"
            L"  FrameworkId : %s\r\n"
            L"  ControlType : %d (%s)\r\n"
            L"  Bounds      : (%ld,%ld) - (%ld,%ld)  %ldx%ld\r\n"
            L"  ProcessId   : %d\r\n"
            L"  Enabled     : %s\r\n"
            L"  Offscreen   : %s\r\n",
            name        ? name        : L"(none)",
            className   ? className   : L"(none)",
            frameworkId ? frameworkId : L"(none)",
            ct, Ua_ControlTypeName(ct),
            br.left, br.top, br.right, br.bottom,
            br.right - br.left, br.bottom - br.top,
            pid,
            enabled ? L"true" : L"false",
            isOffscreen ? L"true" : L"false");

        if (name)        SysFreeString(name);
        if (className)   SysFreeString(className);
        if (frameworkId) SysFreeString(frameworkId);
    }

    SetWindowTextW(st->output, out);
    IUIAutomationElement_Release(elem);
}

static LRESULT CALLBACK Ua_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    UaState *st = (UaState *)GetPropW(hwnd, UA_PROP);
    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_UA_GO) {
            if (!st->timer) st->timer = SetTimer(hwnd, UA_TIMER, 1000, NULL);
            Ua_QueryFocused(st);
            return 0;
        }
        if (LOWORD(wp) == ID_UA_STOP) {
            if (st->timer) { KillTimer(hwnd, st->timer); st->timer = 0; }
            return 0;
        }
    }
    if (msg == WM_TIMER && st) { Ua_QueryFocused(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->timer) KillTimer(hwnd, st->timer);
        if (st->uia)   IUIAutomation_Release(st->uia);
        if (st->comOk) CoUninitialize();
        free(st); RemovePropW(hwnd, UA_PROP);
    }
    return CallWindowProcW(g_origUaFrame, hwnd, msg, wp, lp);
}

static HWND UIAuto_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    UaState *st;
    HFONT mono;
    HRESULT hr;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"UIAuto",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (UaState *)calloc(1, sizeof(UaState));
    if (!st) { DestroyWindow(frame); return NULL; }

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    st->comOk = SUCCEEDED(hr);

    hr = CoCreateInstance(&CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER,
                           &IID_IUIAutomation, (void **)&st->uia);
    (void)hr;

    CreateWindowExW(0, L"BUTTON", L"Start polling",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 130, 26, frame, (HMENU)(LONG_PTR)ID_UA_GO, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Stop",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        150, 38, 90, 26, frame, (HMENU)(LONG_PTR)ID_UA_STOP, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"Click Start; then click into other windows to see UIA report focus.",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_UA_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    if (!st->uia) SetWindowTextW(st->output, L"IUIAutomation unavailable.\r\n");

    SetPropW(frame, UA_PROP, (HANDLE)st);
    if (!g_origUaFrame) g_origUaFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Ua_FrameProc);
    return frame;
}

MsApp g_AppUIAuto = { L"UIAuto", UIAuto_Create, 660, 420 };
