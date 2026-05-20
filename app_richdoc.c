/*
 * app_richdoc.c — Rich text editor with toolbar + status bar
 *
 * Demonstrates:
 *   - RichEdit 2.0 control (LoadLibrary "Msftedit.dll" + MSFTEDIT_CLASS)
 *   - Toolbar (TOOLBARCLASSNAME) with TBN_GETBUTTONINFO-free static buttons
 *   - StatusBar (STATUSCLASSNAME) with multiple parts, updated on selection
 *     change via EN_SELCHANGE
 *   - CHARFORMAT2W to toggle bold/italic/underline on the selection
 */

#include "shell.h"
#include <commctrl.h>
#include <richedit.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "comctl32.lib")

#define RD_PROP    L"MS_RD_STATE"
#define ID_RD_TOOL 15001
#define ID_RD_EDIT 15002
#define ID_RD_STAT 15003

#define CMD_BOLD      15100
#define CMD_ITALIC    15101
#define CMD_UNDER     15102
#define CMD_LEFT      15103
#define CMD_CENTER    15104
#define CMD_RIGHT     15105

typedef struct {
    HWND toolbar;
    HWND edit;
    HWND status;
    HMODULE richEditDll;
} RdState;

static WNDPROC g_origRdFrame = NULL;

static void Rd_ToggleFormat(HWND edit, DWORD effect)
{
    CHARFORMAT2W cf;
    ZeroMemory(&cf, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = effect;

    /* Read current state of the selection */
    SendMessageW(edit, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    /* Toggle */
    if (cf.dwEffects & effect) cf.dwEffects &= ~effect;
    else                       cf.dwEffects |=  effect;
    cf.dwMask = effect;
    SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    SetFocus(edit);
}

static void Rd_Align(HWND edit, WORD alignment)
{
    PARAFORMAT2 pf;
    ZeroMemory(&pf, sizeof(pf));
    pf.cbSize = sizeof(pf);
    pf.dwMask = PFM_ALIGNMENT;
    pf.wAlignment = alignment;
    SendMessageW(edit, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
    SetFocus(edit);
}

static void Rd_UpdateStatus(RdState *st)
{
    CHARRANGE cr;
    int total;
    wchar_t buf[80];

    SendMessageW(st->edit, EM_EXGETSEL, 0, (LPARAM)&cr);
    total = GetWindowTextLengthW(st->edit);

    swprintf_s(buf, 80, L" Selection: %ld–%ld (%ld chars)",
               cr.cpMin, cr.cpMax, cr.cpMax - cr.cpMin);
    SendMessageW(st->status, SB_SETTEXTW, 0, (LPARAM)buf);

    swprintf_s(buf, 80, L" Total: %d chars", total);
    SendMessageW(st->status, SB_SETTEXTW, 1, (LPARAM)buf);
}

static LRESULT CALLBACK Rd_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    RdState *st = (RdState *)GetPropW(hwnd, RD_PROP);

    if (msg == WM_COMMAND && st) {
        switch (LOWORD(wp)) {
        case CMD_BOLD:   Rd_ToggleFormat(st->edit, CFE_BOLD);      return 0;
        case CMD_ITALIC: Rd_ToggleFormat(st->edit, CFE_ITALIC);    return 0;
        case CMD_UNDER:  Rd_ToggleFormat(st->edit, CFE_UNDERLINE); return 0;
        case CMD_LEFT:   Rd_Align(st->edit, PFA_LEFT);             return 0;
        case CMD_CENTER: Rd_Align(st->edit, PFA_CENTER);           return 0;
        case CMD_RIGHT:  Rd_Align(st->edit, PFA_RIGHT);            return 0;
        }
    }
    if (msg == WM_NOTIFY && st) {
        NMHDR *hdr = (NMHDR *)lp;
        if (hdr->code == EN_SELCHANGE) {
            Rd_UpdateStatus(st);
            return 0;
        }
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        int tbH = 32, sbH = 22;
        int parts[2];
        SendMessageW(st->toolbar, TB_AUTOSIZE, 0, 0);
        MoveWindow(st->toolbar, 4, 32, w - 8, tbH, TRUE);
        MoveWindow(st->edit,    4, 32 + tbH + 4, w - 8,
                                h - 32 - tbH - sbH - 8, TRUE);
        SendMessageW(st->status, WM_SIZE, 0, 0);
        parts[0] = w - 180;
        parts[1] = w - 4;
        SendMessageW(st->status, SB_SETPARTS, 2, (LPARAM)parts);
    }
    if (msg == WM_DESTROY && st) {
        if (st->richEditDll) FreeLibrary(st->richEditDll);
        free(st);
        RemovePropW(hwnd, RD_PROP);
    }
    return CallWindowProcW(g_origRdFrame, hwnd, msg, wp, lp);
}

static HWND RichDoc_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    RdState *st;
    INITCOMMONCONTROLSEX icc;
    TBBUTTON buttons[6];
    int i;
    HFONT font;
    int parts[2] = { 200, -1 };
    (void)self;

    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_BAR_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"RichDoc",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (RdState *)calloc(1, sizeof(RdState));
    if (!st) { DestroyWindow(frame); return NULL; }

    /* RichEdit 4.x lives in Msftedit.dll; class is MSFTEDIT_CLASS = L"RICHEDIT50W" */
    st->richEditDll = LoadLibraryW(L"Msftedit.dll");

    /* Toolbar */
    st->toolbar = CreateWindowExW(0, TOOLBARCLASSNAMEW, NULL,
        WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | CCS_NODIVIDER,
        4, 32, w - 8, 32, frame, (HMENU)(LONG_PTR)ID_RD_TOOL, hInstance, NULL);
    SendMessageW(st->toolbar, TB_BUTTONSTRUCTSIZE, (WPARAM)sizeof(TBBUTTON), 0);
    SendMessageW(st->toolbar, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_MIXEDBUTTONS);

    /* Use text buttons (no image list) for simplicity */
    {
        const wchar_t *labels[6] = {
            L" B ", L" I ", L" U ", L" L ", L" C ", L" R "
        };
        int cmds[6] = {
            CMD_BOLD, CMD_ITALIC, CMD_UNDER, CMD_LEFT, CMD_CENTER, CMD_RIGHT
        };
        ZeroMemory(buttons, sizeof(buttons));
        for (i = 0; i < 6; ++i) {
            INT_PTR sidx = SendMessageW(st->toolbar, TB_ADDSTRINGW,
                (WPARAM)0, (LPARAM)labels[i]);
            buttons[i].iBitmap   = I_IMAGENONE;
            buttons[i].idCommand = cmds[i];
            buttons[i].fsState   = TBSTATE_ENABLED;
            buttons[i].fsStyle   = BTNS_BUTTON | BTNS_SHOWTEXT | BTNS_AUTOSIZE;
            buttons[i].iString   = sidx;
        }
        SendMessageW(st->toolbar, TB_ADDBUTTONS, 6, (LPARAM)buttons);
        SendMessageW(st->toolbar, TB_AUTOSIZE, 0, 0);
    }

    /* RichEdit */
    st->edit = CreateWindowExW(WS_EX_CLIENTEDGE,
        L"RICHEDIT50W", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL,
        4, 70, w - 8, h - 100, frame, (HMENU)(LONG_PTR)ID_RD_EDIT, hInstance, NULL);

    if (!st->edit) {
        /* fall back to RichEdit 2.0 class */
        st->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"RichEdit20W", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL,
            4, 70, w - 8, h - 100, frame,
            (HMENU)(LONG_PTR)ID_RD_EDIT, hInstance, NULL);
    }

    SendMessageW(st->edit, EM_SETEVENTMASK, 0, ENM_SELCHANGE);
    font = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    SendMessageW(st->edit, WM_SETFONT, (WPARAM)font, TRUE);
    SetWindowTextW(st->edit,
        L"Type some text and select it, then click B/I/U on the toolbar.\r\n"
        L"L/C/R align the current paragraph.\r\n");

    /* Status bar */
    st->status = CreateWindowExW(0, STATUSCLASSNAMEW, NULL,
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, frame, (HMENU)(LONG_PTR)ID_RD_STAT, hInstance, NULL);
    SendMessageW(st->status, SB_SETPARTS, 2, (LPARAM)parts);

    SetPropW(frame, RD_PROP, (HANDLE)st);
    if (!g_origRdFrame)
        g_origRdFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Rd_FrameProc);

    Rd_UpdateStatus(st);
    return frame;
}

MsApp g_AppRichDoc = {
    L"RichDoc",
    RichDoc_Create,
    640, 460
};
