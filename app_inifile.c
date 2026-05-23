/*
 * app_inifile.c — Legacy INI-file API round-trip
 *
 * Demonstrates the WIN.INI-era profile API, still useful for portable
 * key=value persistence:
 *   - WritePrivateProfileStringW(section, key, value, path) writes a key
 *   - GetPrivateProfileStringW(section, key, default, buf, cb, path) reads it
 *   - GetPrivateProfileSectionNamesW lists all sections in the file
 *   - GetPrivateProfileSectionW returns all key=value lines of a section
 *
 * UI: a section/key/value triple with Save and Load. The dump area shows the
 * full INI content via GetPrivateProfileSectionNamesW + GetPrivateProfileSectionW.
 *
 * Storage: %TEMP%\minishell.ini.
 */

#include "shell.h"
#include <shlobj.h>
#include <stdlib.h>
#include <stdio.h>

#define IF_PROP    L"MS_IF_STATE"
#define ID_IF_SEC  68001
#define ID_IF_KEY  68002
#define ID_IF_VAL  68003
#define ID_IF_SAVE 68004
#define ID_IF_LOAD 68005
#define ID_IF_DUMP 68006
#define ID_IF_OUT  68007

typedef struct {
    HWND sec, key, val, save, load, dump, output;
    wchar_t path[MAX_PATH];
} IfState;

static WNDPROC g_origIfFrame = NULL;

static void If_Status(IfState *st, const wchar_t *t)
{
    SetWindowTextW(st->output, t);
}

static void If_Save(IfState *st)
{
    wchar_t s[80], k[80], v[260];
    GetWindowTextW(st->sec, s, 80);
    GetWindowTextW(st->key, k, 80);
    GetWindowTextW(st->val, v, 260);
    if (!s[0] || !k[0]) { If_Status(st, L"Section and key required."); return; }

    if (WritePrivateProfileStringW(s, k, v, st->path)) {
        wchar_t buf[400];
        swprintf_s(buf, 400, L"Wrote [%s]\\%s=%s to\r\n  %s",
                   s, k, v, st->path);
        If_Status(st, buf);
    } else {
        If_Status(st, L"Write failed.");
    }
}

static void If_Load(IfState *st)
{
    wchar_t s[80], k[80], v[400];
    GetWindowTextW(st->sec, s, 80);
    GetWindowTextW(st->key, k, 80);
    if (!s[0] || !k[0]) { If_Status(st, L"Section and key required."); return; }
    GetPrivateProfileStringW(s, k, L"<not set>", v, 400, st->path);
    SetWindowTextW(st->val, v);
    {
        wchar_t buf[480];
        swprintf_s(buf, 480, L"Read [%s]\\%s = %s", s, k, v);
        If_Status(st, buf);
    }
}

static void If_Dump(IfState *st)
{
    wchar_t names[4096];
    DWORD got;
    wchar_t *cur;
    wchar_t section[4096];

    SetWindowTextW(st->output, L"");
    got = GetPrivateProfileSectionNamesW(names, 4096, st->path);
    if (got == 0) { If_Status(st, L"INI file empty or missing."); return; }

    /* names is a series of NUL-terminated strings followed by an extra NUL */
    {
        wchar_t buf[200];
        swprintf_s(buf, 200, L"INI file: %s\r\n\r\n", st->path);
        If_Status(st, buf);
    }
    for (cur = names; *cur; cur += wcslen(cur) + 1) {
        wchar_t header[120];
        wchar_t *kv;
        swprintf_s(header, 120, L"[%s]\r\n", cur);
        {
            int len = GetWindowTextLengthW(st->output);
            SendMessageW(st->output, EM_SETSEL, len, len);
            SendMessageW(st->output, EM_REPLACESEL, FALSE, (LPARAM)header);
        }

        GetPrivateProfileSectionW(cur, section, 4096, st->path);
        for (kv = section; *kv; kv += wcslen(kv) + 1) {
            wchar_t line[600];
            swprintf_s(line, 600, L"  %s\r\n", kv);
            {
                int len = GetWindowTextLengthW(st->output);
                SendMessageW(st->output, EM_SETSEL, len, len);
                SendMessageW(st->output, EM_REPLACESEL, FALSE, (LPARAM)line);
            }
        }
        {
            int len = GetWindowTextLengthW(st->output);
            SendMessageW(st->output, EM_SETSEL, len, len);
            SendMessageW(st->output, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
        }
    }
}

static LRESULT CALLBACK If_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    IfState *st = (IfState *)GetPropW(hwnd, IF_PROP);
    if (msg == WM_COMMAND && st) {
        switch (LOWORD(wp)) {
        case ID_IF_SAVE: If_Save(st); return 0;
        case ID_IF_LOAD: If_Load(st); return 0;
        case ID_IF_DUMP: If_Dump(st); return 0;
        }
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 160, w - 16, h - 168, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, IF_PROP); }
    return CallWindowProcW(g_origIfFrame, hwnd, msg, wp, lp);
}

static HWND IniFile_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    IfState *st;
    HFONT mono;
    wchar_t temp[MAX_PATH];
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"IniFile",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (IfState *)calloc(1, sizeof(IfState));
    if (!st) { DestroyWindow(frame); return NULL; }

    GetTempPathW(MAX_PATH, temp);
    swprintf_s(st->path, MAX_PATH, L"%sminishell.ini", temp);

    CreateWindowExW(0, L"STATIC", L"Section:",
        WS_CHILD | WS_VISIBLE, 12, 38, 60, 22, frame, NULL, hInstance, NULL);
    st->sec = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"General",
        WS_CHILD | WS_VISIBLE,
        78, 36, 180, 24, frame, (HMENU)(LONG_PTR)ID_IF_SEC, hInstance, NULL);

    CreateWindowExW(0, L"STATIC", L"Key:",
        WS_CHILD | WS_VISIBLE, 270, 38, 30, 22, frame, NULL, hInstance, NULL);
    st->key = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"Greeting",
        WS_CHILD | WS_VISIBLE,
        306, 36, 180, 24, frame, (HMENU)(LONG_PTR)ID_IF_KEY, hInstance, NULL);

    CreateWindowExW(0, L"STATIC", L"Value:",
        WS_CHILD | WS_VISIBLE, 12, 72, 60, 22, frame, NULL, hInstance, NULL);
    st->val = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"Hello, world",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        78, 70, w - 90, 24, frame, (HMENU)(LONG_PTR)ID_IF_VAL, hInstance, NULL);

    st->save = CreateWindowExW(0, L"BUTTON", L"Save",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 110, 90, 26, frame, (HMENU)(LONG_PTR)ID_IF_SAVE, hInstance, NULL);
    st->load = CreateWindowExW(0, L"BUTTON", L"Load",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        110, 110, 90, 26, frame, (HMENU)(LONG_PTR)ID_IF_LOAD, hInstance, NULL);
    st->dump = CreateWindowExW(0, L"BUTTON", L"Dump file",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        208, 110, 120, 26, frame, (HMENU)(LONG_PTR)ID_IF_DUMP, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 160, w - 16, h - 168, frame, (HMENU)(LONG_PTR)ID_IF_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, IF_PROP, (HANDLE)st);
    if (!g_origIfFrame) g_origIfFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)If_FrameProc);
    {
        wchar_t buf[200];
        swprintf_s(buf, 200, L"INI file path:\r\n  %s\r\n", st->path);
        SetWindowTextW(st->output, buf);
    }
    return frame;
}

MsApp g_AppIniFile = { L"IniFile", IniFile_Create, 580, 400 };
