/*
 * app_jumplist.c — Build a custom taskbar jump list with ICustomDestinationList
 *
 * Demonstrates the Windows 7+ Jump List API (shobjidl.h):
 *   - CoCreateInstance(CLSID_DestinationList) → ICustomDestinationList
 *   - SetAppID(L"MiniShell.JumpListDemo") gives our app a stable AppUserModelID
 *   - BeginList(&minSlots, IID_IObjectArray, &removed) starts the transaction
 *   - CoCreateInstance(CLSID_EnumerableObjectCollection) → IObjectCollection
 *   - Each task = a CLSID_ShellLink IShellLinkW with SetPath + SetArguments
 *     + SetDescription + SetIconLocation (AddUserTasks rejects argument-less links)
 *   - IObjectCollection::QueryInterface(IID_IObjectArray) → handed to AddUserTasks
 *   - CommitList finalizes; in case of error AbortList rolls back
 *
 * After running, right-click MiniShell.exe in the taskbar (Win 7+) to see the
 * custom tasks under "Tasks". Delete clears the jump list via DeleteList.
 */

#define COBJMACROS
#define CINTERFACE
#define INITGUID

#include "shell.h"
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

#define JL_APPID  L"MiniShell.JumpListDemo"

#define JL_PROP    L"MS_JL_STATE"
#define ID_JL_BLD  65001
#define ID_JL_DEL  65002
#define ID_JL_OUT  65003

typedef struct { HWND output; BOOL comOk; } JlState;

static WNDPROC g_origJlFrame = NULL;

static void Jl_Append(JlState *st, const wchar_t *t)
{
    int len = GetWindowTextLengthW(st->output);
    SendMessageW(st->output, EM_SETSEL, len, len);
    SendMessageW(st->output, EM_REPLACESEL, FALSE, (LPARAM)t);
}

static IShellLinkW *Jl_MakeTask(const wchar_t *target, const wchar_t *args,
                                  const wchar_t *desc, const wchar_t *iconPath,
                                  int iconIdx)
{
    IShellLinkW *link = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                   &IID_IShellLinkW, (void **)&link);
    if (FAILED(hr)) return NULL;
    IShellLinkW_SetPath(link, target);
    IShellLinkW_SetArguments(link, args);
    IShellLinkW_SetDescription(link, desc);
    if (iconPath) IShellLinkW_SetIconLocation(link, iconPath, iconIdx);
    IShellLinkW_SetShowCmd(link, SW_SHOWNORMAL);
    return link;
}

static void Jl_Build(JlState *st)
{
    ICustomDestinationList *list = NULL;
    IObjectCollection      *coll = NULL;
    IObjectArray           *arr  = NULL;
    IObjectArray           *removed = NULL;
    UINT minSlots = 0;
    HRESULT hr;
    IShellLinkW *t1, *t2, *t3;

    SetWindowTextW(st->output, L"");

    hr = CoCreateInstance(&CLSID_DestinationList, NULL, CLSCTX_INPROC_SERVER,
                           &IID_ICustomDestinationList, (void **)&list);
    if (FAILED(hr)) { Jl_Append(st, L"CoCreateInstance(DestinationList) failed.\r\n"); return; }

    ICustomDestinationList_SetAppID(list, JL_APPID);
    hr = ICustomDestinationList_BeginList(list, &minSlots,
            &IID_IObjectArray, (void **)&removed);
    if (FAILED(hr)) {
        Jl_Append(st, L"BeginList failed.\r\n");
        ICustomDestinationList_Release(list);
        return;
    }

    /* Build a collection of three sample tasks */
    hr = CoCreateInstance(&CLSID_EnumerableObjectCollection, NULL,
            CLSCTX_INPROC, &IID_IObjectCollection, (void **)&coll);
    if (FAILED(hr)) {
        Jl_Append(st, L"CoCreateInstance(EnumerableObjectCollection) failed.\r\n");
        goto cleanup;
    }

    t1 = Jl_MakeTask(L"%windir%\\system32\\notepad.exe", L"",
                     L"Launch Notepad",
                     L"%windir%\\system32\\notepad.exe", 0);
    t2 = Jl_MakeTask(L"%windir%\\system32\\calc.exe", L"",
                     L"Launch Calculator",
                     L"%windir%\\system32\\calc.exe", 0);
    t3 = Jl_MakeTask(L"%windir%\\explorer.exe", L"%userprofile%",
                     L"Open user folder",
                     L"%windir%\\explorer.exe", 0);

    if (t1) {
        IObjectCollection_AddObject(coll, (IUnknown *)t1);
        IShellLinkW_Release(t1);
    }
    if (t2) {
        IObjectCollection_AddObject(coll, (IUnknown *)t2);
        IShellLinkW_Release(t2);
    }
    if (t3) {
        IObjectCollection_AddObject(coll, (IUnknown *)t3);
        IShellLinkW_Release(t3);
    }

    hr = IObjectCollection_QueryInterface(coll, &IID_IObjectArray, (void **)&arr);
    if (FAILED(hr)) { Jl_Append(st, L"QI(IObjectArray) failed.\r\n"); goto cleanup; }

    hr = ICustomDestinationList_AddUserTasks(list, arr);
    if (FAILED(hr)) {
        Jl_Append(st, L"AddUserTasks failed.\r\n");
        ICustomDestinationList_AbortList(list);
        goto cleanup;
    }

    hr = ICustomDestinationList_CommitList(list);
    if (SUCCEEDED(hr)) {
        wchar_t buf[200];
        swprintf_s(buf, 200,
            L"Jump list committed under AppID %s.\r\n"
            L"Right-click MiniShell.exe on the taskbar to see Tasks.\r\n"
            L"  minimum slots: %u\r\n",
            JL_APPID, minSlots);
        Jl_Append(st, buf);
    } else {
        Jl_Append(st, L"CommitList failed.\r\n");
    }

cleanup:
    if (arr) IObjectArray_Release(arr);
    if (coll) IObjectCollection_Release(coll);
    if (removed) IObjectArray_Release(removed);
    if (list) ICustomDestinationList_Release(list);
}

static void Jl_Delete(JlState *st)
{
    ICustomDestinationList *list = NULL;
    HRESULT hr;

    hr = CoCreateInstance(&CLSID_DestinationList, NULL, CLSCTX_INPROC_SERVER,
                           &IID_ICustomDestinationList, (void **)&list);
    if (FAILED(hr)) { Jl_Append(st, L"CoCreateInstance failed.\r\n"); return; }
    ICustomDestinationList_DeleteList(list, JL_APPID);
    ICustomDestinationList_Release(list);
    Jl_Append(st, L"Jump list deleted.\r\n");
}

static LRESULT CALLBACK Jl_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    JlState *st = (JlState *)GetPropW(hwnd, JL_PROP);
    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_JL_BLD) { Jl_Build(st);  return 0; }
        if (LOWORD(wp) == ID_JL_DEL) { Jl_Delete(st); return 0; }
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 80, w - 16, h - 88, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->comOk) CoUninitialize();
        free(st);
        RemovePropW(hwnd, JL_PROP);
    }
    return CallWindowProcW(g_origJlFrame, hwnd, msg, wp, lp);
}

static HWND JumpList_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    JlState *st;
    HFONT mono;
    HRESULT hr;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"JumpList",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (JlState *)calloc(1, sizeof(JlState));
    if (!st) { DestroyWindow(frame); return NULL; }

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    st->comOk = SUCCEEDED(hr);
    SetCurrentProcessExplicitAppUserModelID(JL_APPID);

    CreateWindowExW(0, L"STATIC",
        L"Build a custom taskbar jump list under AppID MiniShell.JumpListDemo.\n"
        L"Three tasks: Notepad, Calculator, Explorer at the user folder.",
        WS_CHILD | WS_VISIBLE,
        12, 30, w - 24, 36, frame, NULL, hInstance, NULL);

    CreateWindowExW(0, L"BUTTON", L"Build jump list",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 76, 140, 26, frame, (HMENU)(LONG_PTR)ID_JL_BLD, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Delete",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        160, 76, 90, 26, frame, (HMENU)(LONG_PTR)ID_JL_DEL, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 110, w - 16, h - 118, frame, (HMENU)(LONG_PTR)ID_JL_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, JL_PROP, (HANDLE)st);
    if (!g_origJlFrame) g_origJlFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Jl_FrameProc);
    return frame;
}

MsApp g_AppJumpList = { L"JumpList", JumpList_Create, 560, 380 };
