/*
 * app_atoms.c — Global atom table
 *
 * Demonstrates one of the older, less-used Windows IPC primitives:
 *   - GlobalAddAtomW: register (or get the ATOM for) a string in the
 *     system-wide table
 *   - GlobalGetAtomNameW: look up a string by ATOM
 *   - GlobalFindAtomW: test for existence without registering
 *   - GlobalDeleteAtom: drop a reference (atoms are ref-counted)
 *
 * Atoms used to be the standard way to pass strings between processes for
 * window messages (especially RegisterWindowMessage uses the system table).
 * The reference counting means independent apps can race-free share a
 * string identifier without coordinating.
 *
 * Multi-instance: open the app twice — the second instance will see atoms
 * the first one registered.
 */

#include "shell.h"
#include <commctrl.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "comctl32.lib")

#define AT_PROP    L"MS_AT_STATE"
#define ID_AT_IN   34001
#define ID_AT_ADD  34002
#define ID_AT_FIND 34003
#define ID_AT_DEL  34004
#define ID_AT_LIST 34005

typedef struct {
    HWND in, addBtn, findBtn, delBtn, list;
} AtState;

static WNDPROC g_origAtFrame = NULL;

static void At_AppendLine(AtState *st, const wchar_t *line)
{
    int len = (int)SendMessageW(st->list, LB_GETCOUNT, 0, 0);
    SendMessageW(st->list, LB_INSERTSTRING, len, (LPARAM)line);
    SendMessageW(st->list, LB_SETTOPINDEX, len, 0);
}

static void At_Add(AtState *st)
{
    wchar_t name[256], line[400], readback[256];
    ATOM atom;
    GetWindowTextW(st->in, name, 256);
    if (name[0] == 0) return;

    atom = GlobalAddAtomW(name);
    if (atom == 0) {
        swprintf_s(line, 400, L"ADD  \"%s\" → failed", name);
    } else {
        GlobalGetAtomNameW(atom, readback, 256);
        swprintf_s(line, 400, L"ADD  \"%s\" → atom 0x%04X (readback \"%s\")",
                   name, atom, readback);
    }
    At_AppendLine(st, line);
}

static void At_Find(AtState *st)
{
    wchar_t name[256], line[400];
    ATOM atom;
    GetWindowTextW(st->in, name, 256);
    if (name[0] == 0) return;

    atom = GlobalFindAtomW(name);
    if (atom == 0)
        swprintf_s(line, 400, L"FIND \"%s\" → not present (or error %lu)",
                   name, GetLastError());
    else
        swprintf_s(line, 400, L"FIND \"%s\" → atom 0x%04X", name, atom);
    At_AppendLine(st, line);
}

static void At_Del(AtState *st)
{
    wchar_t name[256], line[400];
    ATOM atom;
    GetWindowTextW(st->in, name, 256);
    if (name[0] == 0) return;

    atom = GlobalFindAtomW(name);
    if (atom == 0) {
        swprintf_s(line, 400, L"DEL  \"%s\" → not present", name);
    } else {
        ATOM result = GlobalDeleteAtom(atom);
        if (result == 0)
            swprintf_s(line, 400,
                L"DEL  \"%s\" → atom 0x%04X dropped one reference",
                name, atom);
        else
            swprintf_s(line, 400,
                L"DEL  \"%s\" → GlobalDeleteAtom failed", name);
    }
    At_AppendLine(st, line);
}

static LRESULT CALLBACK At_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    AtState *st = (AtState *)GetPropW(hwnd, AT_PROP);

    if (msg == WM_COMMAND && st) {
        switch (LOWORD(wp)) {
        case ID_AT_ADD:  At_Add(st);  return 0;
        case ID_AT_FIND: At_Find(st); return 0;
        case ID_AT_DEL:  At_Del(st);  return 0;
        }
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->in,      8,  34, w - 280, 24, TRUE);
        MoveWindow(st->addBtn,  w - 268, 34, 80, 24, TRUE);
        MoveWindow(st->findBtn, w - 184, 34, 80, 24, TRUE);
        MoveWindow(st->delBtn,  w - 100, 34, 80, 24, TRUE);
        MoveWindow(st->list,    8,  64, w - 16, h - 72, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        free(st);
        RemovePropW(hwnd, AT_PROP);
    }
    return CallWindowProcW(g_origAtFrame, hwnd, msg, wp, lp);
}

static HWND Atoms_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    AtState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Atoms",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (AtState *)calloc(1, sizeof(AtState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->in = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"MiniShellExample",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        8, 34, w - 280, 24, frame, (HMENU)(LONG_PTR)ID_AT_IN, hInstance, NULL);
    st->addBtn = CreateWindowExW(0, L"BUTTON", L"Add",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        w - 268, 34, 80, 24, frame, (HMENU)(LONG_PTR)ID_AT_ADD, hInstance, NULL);
    st->findBtn = CreateWindowExW(0, L"BUTTON", L"Find",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        w - 184, 34, 80, 24, frame, (HMENU)(LONG_PTR)ID_AT_FIND, hInstance, NULL);
    st->delBtn = CreateWindowExW(0, L"BUTTON", L"Delete",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        w - 100, 34, 80, 24, frame, (HMENU)(LONG_PTR)ID_AT_DEL, hInstance, NULL);

    st->list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        8, 64, w - 16, h - 72, frame, (HMENU)(LONG_PTR)ID_AT_LIST, hInstance, NULL);

    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->list, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, AT_PROP, (HANDLE)st);
    if (!g_origAtFrame)
        g_origAtFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)At_FrameProc);
    return frame;
}

MsApp g_AppAtoms = {
    L"Atoms",
    Atoms_Create,
    600, 360
};
