/*
 * app_taskdlg.c — Modern task dialogs (TaskDialogIndirect)
 *
 * Demonstrates the modern replacement for MessageBox:
 *   - TASKDIALOGCONFIG with command-link buttons, a footer, an icon
 *   - TaskDialogIndirect for the indirect form (full configurability)
 *   - TaskDialogCallback for periodic progress updates via TDM_SET_PROGRESS_BAR_POS
 *
 * Three buttons demonstrate increasingly rich dialog patterns:
 *   - Simple: just headline + content
 *   - Command links: explanatory text under each choice
 *   - Progress: marquee that completes after 3 seconds via callback
 */

#include "shell.h"
#include <commctrl.h>
#include <stdlib.h>

#pragma comment(lib, "comctl32.lib")

#define TD_PROP      L"MS_TD_STATE"
#define ID_TD_SIMPLE 37001
#define ID_TD_LINKS  37002
#define ID_TD_PROG   37003
#define ID_TD_OUT    37004

typedef struct {
    HWND output;
} TdState;

static WNDPROC g_origTdFrame = NULL;

static void Td_Append(TdState *st, const wchar_t *line)
{
    int len = GetWindowTextLengthW(st->output);
    SendMessageW(st->output, EM_SETSEL, len, len);
    SendMessageW(st->output, EM_REPLACESEL, FALSE, (LPARAM)line);
}

static HRESULT CALLBACK Td_ProgressCb(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                       LONG_PTR ref)
{
    static int ticks = 0;
    (void)wp; (void)lp; (void)ref;
    if (msg == TDN_CREATED) {
        ticks = 0;
        SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_RANGE, 0, MAKELPARAM(0, 30));
        SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_POS, 0, 0);
    } else if (msg == TDN_TIMER) {
        ++ticks;
        SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_POS, ticks, 0);
        if (ticks >= 30) {
            SendMessageW(hwnd, TDM_CLICK_BUTTON, IDOK, 0);
        }
    }
    return S_OK;
}

static void Td_Simple(HWND hwnd, TdState *st)
{
    TASKDIALOGCONFIG c;
    int btn = 0;
    HRESULT hr;
    ZeroMemory(&c, sizeof(c));
    c.cbSize = sizeof(c);
    c.hwndParent = hwnd;
    c.dwCommonButtons = TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON;
    c.pszWindowTitle = L"MiniShell";
    c.pszMainIcon = TD_INFORMATION_ICON;
    c.pszMainInstruction = L"This is a simple task dialog.";
    c.pszContent = L"It uses TaskDialogIndirect with an icon and OK/Cancel buttons.";

    hr = TaskDialogIndirect(&c, &btn, NULL, NULL);
    if (SUCCEEDED(hr)) {
        wchar_t buf[80];
        swprintf_s(buf, 80, L"Simple dialog → %s\r\n",
                   btn == IDOK ? L"OK" : L"Cancel");
        Td_Append(st, buf);
    }
}

static void Td_Links(HWND hwnd, TdState *st)
{
    TASKDIALOGCONFIG c;
    TASKDIALOG_BUTTON buttons[3];
    int btn = 0;
    HRESULT hr;

    buttons[0].nButtonID = 100;
    buttons[0].pszButtonText = L"Save\nWrite to disk and continue editing.";
    buttons[1].nButtonID = 101;
    buttons[1].pszButtonText = L"Discard\nThrow away unsaved changes.";
    buttons[2].nButtonID = 102;
    buttons[2].pszButtonText = L"Cancel\nReturn without making a choice.";

    ZeroMemory(&c, sizeof(c));
    c.cbSize = sizeof(c);
    c.hwndParent = hwnd;
    c.dwFlags = TDF_USE_COMMAND_LINKS;
    c.pszWindowTitle = L"MiniShell";
    c.pszMainIcon = TD_WARNING_ICON;
    c.pszMainInstruction = L"You have unsaved changes.";
    c.pszContent = L"What would you like to do?";
    c.cButtons = 3;
    c.pButtons = buttons;
    c.pszFooter = L"This is the footer area, useful for hints.";
    c.pszFooterIcon = TD_INFORMATION_ICON;

    hr = TaskDialogIndirect(&c, &btn, NULL, NULL);
    if (SUCCEEDED(hr)) {
        const wchar_t *name = L"?";
        wchar_t buf[80];
        switch (btn) {
        case 100: name = L"Save"; break;
        case 101: name = L"Discard"; break;
        case 102: name = L"Cancel"; break;
        case IDCANCEL: name = L"closed via X"; break;
        }
        swprintf_s(buf, 80, L"Command-link dialog → %s\r\n", name);
        Td_Append(st, buf);
    }
}

static void Td_Progress(HWND hwnd, TdState *st)
{
    TASKDIALOGCONFIG c;
    int btn = 0;
    ZeroMemory(&c, sizeof(c));
    c.cbSize = sizeof(c);
    c.hwndParent = hwnd;
    c.dwFlags = TDF_SHOW_PROGRESS_BAR | TDF_CALLBACK_TIMER;
    c.pszWindowTitle = L"MiniShell";
    c.pszMainIcon = TD_INFORMATION_ICON;
    c.pszMainInstruction = L"Working...";
    c.pszContent = L"This dialog auto-completes after 3 seconds via TDM_SET_PROGRESS_BAR_POS.";
    c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    c.pfCallback = Td_ProgressCb;

    if (SUCCEEDED(TaskDialogIndirect(&c, &btn, NULL, NULL))) {
        Td_Append(st,
            btn == IDOK ? L"Progress dialog → completed\r\n"
                        : L"Progress dialog → cancelled\r\n");
    }
}

static LRESULT CALLBACK Td_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    TdState *st = (TdState *)GetPropW(hwnd, TD_PROP);

    if (msg == WM_COMMAND && st) {
        switch (LOWORD(wp)) {
        case ID_TD_SIMPLE: Td_Simple(hwnd, st);  return 0;
        case ID_TD_LINKS:  Td_Links(hwnd, st);   return 0;
        case ID_TD_PROG:   Td_Progress(hwnd, st); return 0;
        }
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 80, w - 16, h - 88, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, TD_PROP); }
    return CallWindowProcW(g_origTdFrame, hwnd, msg, wp, lp);
}

static HWND TaskDlg_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    TdState *st;
    INITCOMMONCONTROLSEX icc;
    (void)self;

    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"TaskDlg",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (TdState *)calloc(1, sizeof(TdState));
    if (!st) { DestroyWindow(frame); return NULL; }

    CreateWindowExW(0, L"BUTTON", L"Simple",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 36, 100, 28, frame, (HMENU)(LONG_PTR)ID_TD_SIMPLE, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Command links",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        124, 36, 130, 28, frame, (HMENU)(LONG_PTR)ID_TD_LINKS, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Progress",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        266, 36, 100, 28, frame, (HMENU)(LONG_PTR)ID_TD_PROG, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"Click a button to invoke a task dialog.\r\n",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 80, w - 16, h - 88, frame, (HMENU)(LONG_PTR)ID_TD_OUT, hInstance, NULL);

    SetPropW(frame, TD_PROP, (HANDLE)st);
    if (!g_origTdFrame) g_origTdFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Td_FrameProc);
    return frame;
}

MsApp g_AppTaskDlg = {
    L"TaskDlg",
    TaskDlg_Create,
    480, 300
};
