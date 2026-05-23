/*
 * app_symlink.c — Create symlinks and inspect reparse points
 *
 * Demonstrates the symlink + reparse-point APIs:
 *   - CreateSymbolicLinkW(linkPath, targetPath, flags) where flags is
 *     SYMBOLIC_LINK_FLAG_DIRECTORY (or 0 for file) bitwise OR'd with
 *     SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE (Win10 Developer Mode)
 *   - GetFileAttributesW returns FILE_ATTRIBUTE_REPARSE_POINT for symlinks
 *     and junctions
 *   - To read the target: CreateFileW with FILE_FLAG_OPEN_REPARSE_POINT
 *     and FILE_FLAG_BACKUP_SEMANTICS, then DeviceIoControl with
 *     FSCTL_GET_REPARSE_POINT into a REPARSE_DATA_BUFFER
 *   - IO_REPARSE_TAG_SYMLINK vs IO_REPARSE_TAG_MOUNT_POINT (junction)
 *     determine which union member holds the target name
 *
 * Note: creating a symlink without ALLOW_UNPRIVILEGED_CREATE needs the
 * SE_CREATE_SYMBOLIC_LINK_NAME privilege (admin) on most systems. We try
 * both flags; if both fail, we still demonstrate reading existing reparse
 * points (e.g. C:\Users\All Users → ProgramData).
 */

#include "shell.h"
#include <winioctl.h>
#include <stdlib.h>
#include <stdio.h>

#ifndef IO_REPARSE_TAG_SYMLINK
#define IO_REPARSE_TAG_SYMLINK 0xA000000CL
#endif
#ifndef IO_REPARSE_TAG_MOUNT_POINT
#define IO_REPARSE_TAG_MOUNT_POINT 0xA0000003L
#endif
#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif
#ifndef SYMBOLIC_LINK_FLAG_DIRECTORY
#define SYMBOLIC_LINK_FLAG_DIRECTORY 0x1
#endif

typedef struct {
    DWORD ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    union {
        struct {
            USHORT SubstituteNameOffset;
            USHORT SubstituteNameLength;
            USHORT PrintNameOffset;
            USHORT PrintNameLength;
            ULONG  Flags;
            WCHAR  PathBuffer[1];
        } SymbolicLinkReparseBuffer;
        struct {
            USHORT SubstituteNameOffset;
            USHORT SubstituteNameLength;
            USHORT PrintNameOffset;
            USHORT PrintNameLength;
            WCHAR  PathBuffer[1];
        } MountPointReparseBuffer;
        struct {
            UCHAR DataBuffer[1];
        } GenericReparseBuffer;
    } u;
} MS_REPARSE_DATA_BUFFER;

#define SL_PROP    L"MS_SL_STATE"
#define ID_SL_PATH 95001
#define ID_SL_INSP 95002
#define ID_SL_MAKE 95003
#define ID_SL_OUT  95004

typedef struct { HWND pathEdit, inspectBtn, makeBtn, output; } SlState;
static WNDPROC g_origSlFrame = NULL;

static void Sl_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
}

static void Sl_Inspect(SlState *st)
{
    wchar_t path[MAX_PATH];
    DWORD   attr;
    HANDLE  h;
    BYTE    buf[16384];
    DWORD   returned;
    MS_REPARSE_DATA_BUFFER *rdb;

    SetWindowTextW(st->output, L"");
    GetWindowTextW(st->pathEdit, path, MAX_PATH);

    attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        Sl_Append(st->output, L"Path does not exist or access denied.\r\n");
        return;
    }
    {
        wchar_t line[400];
        swprintf_s(line, 400,
            L"Attributes: 0x%08lx\r\n"
            L"  directory:    %s\r\n"
            L"  reparse pt:   %s\r\n",
            attr,
            (attr & FILE_ATTRIBUTE_DIRECTORY)    ? L"YES" : L"no",
            (attr & FILE_ATTRIBUTE_REPARSE_POINT)? L"YES" : L"no");
        Sl_Append(st->output, line);
    }
    if (!(attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
        Sl_Append(st->output, L"\r\nNot a reparse point.\r\n");
        return;
    }

    h = CreateFileW(path, 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        Sl_Append(st->output, L"\r\nCreateFile failed.\r\n");
        return;
    }
    if (!DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, NULL, 0,
                          buf, sizeof(buf), &returned, NULL)) {
        Sl_Append(st->output, L"\r\nFSCTL_GET_REPARSE_POINT failed.\r\n");
        CloseHandle(h);
        return;
    }
    CloseHandle(h);

    rdb = (MS_REPARSE_DATA_BUFFER *)buf;
    {
        wchar_t line[400];
        swprintf_s(line, 400, L"\r\nReparseTag: 0x%08lx\r\n", rdb->ReparseTag);
        Sl_Append(st->output, line);
    }
    if (rdb->ReparseTag == IO_REPARSE_TAG_SYMLINK) {
        WCHAR *base = rdb->u.SymbolicLinkReparseBuffer.PathBuffer;
        USHORT off  = rdb->u.SymbolicLinkReparseBuffer.PrintNameOffset / sizeof(WCHAR);
        USHORT len  = rdb->u.SymbolicLinkReparseBuffer.PrintNameLength / sizeof(WCHAR);
        wchar_t name[MAX_PATH];
        wcsncpy_s(name, MAX_PATH, base + off, len);
        name[len] = 0;
        {
            wchar_t line[MAX_PATH + 60];
            swprintf_s(line, MAX_PATH + 60, L"  Symlink target: %s\r\n", name);
            Sl_Append(st->output, line);
        }
        Sl_Append(st->output,
            (rdb->u.SymbolicLinkReparseBuffer.Flags & 1)
                ? L"  Relative: yes\r\n" : L"  Relative: no\r\n");
    } else if (rdb->ReparseTag == IO_REPARSE_TAG_MOUNT_POINT) {
        WCHAR *base = rdb->u.MountPointReparseBuffer.PathBuffer;
        USHORT off  = rdb->u.MountPointReparseBuffer.PrintNameOffset / sizeof(WCHAR);
        USHORT len  = rdb->u.MountPointReparseBuffer.PrintNameLength / sizeof(WCHAR);
        wchar_t name[MAX_PATH];
        wcsncpy_s(name, MAX_PATH, base + off, len);
        name[len] = 0;
        {
            wchar_t line[MAX_PATH + 60];
            swprintf_s(line, MAX_PATH + 60, L"  Junction target: %s\r\n", name);
            Sl_Append(st->output, line);
        }
    } else {
        Sl_Append(st->output, L"  (other reparse tag)\r\n");
    }
}

static void Sl_TryMakeLink(SlState *st)
{
    wchar_t link[MAX_PATH];
    wchar_t target[MAX_PATH] = L"";
    BOOL ok;

    ExpandEnvironmentStringsW(L"%TEMP%\\minishell_symlink_demo.txt", link, MAX_PATH);
    ExpandEnvironmentStringsW(L"%windir%\\notepad.exe", target, MAX_PATH);

    DeleteFileW(link);

    ok = CreateSymbolicLinkW(link, target,
        SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE);
    if (!ok) ok = CreateSymbolicLinkW(link, target, 0);

    {
        wchar_t line[MAX_PATH + 200];
        if (ok) {
            swprintf_s(line, MAX_PATH + 200,
                L"\r\nCreated symlink:\r\n  link:   %s\r\n  target: %s\r\n", link, target);
        } else {
            swprintf_s(line, MAX_PATH + 200,
                L"\r\nCreateSymbolicLink failed (err %lu).\r\n"
                L"  Need Developer Mode (Win10+) or SeCreateSymbolicLinkPrivilege (admin).\r\n",
                GetLastError());
        }
        Sl_Append(st->output, line);
    }

    if (ok) {
        SetWindowTextW(st->pathEdit, link);
        Sl_Inspect(st);
    }
}

static LRESULT CALLBACK Sl_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    SlState *st = (SlState *)GetPropW(hwnd, SL_PROP);
    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_SL_INSP) { Sl_Inspect(st); return 0; }
        if (LOWORD(wp) == ID_SL_MAKE) { Sl_TryMakeLink(st); return 0; }
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->pathEdit,   12, 38, w - 232, 24, TRUE);
        MoveWindow(st->inspectBtn, w - 216, 38, 100, 24, TRUE);
        MoveWindow(st->makeBtn,    w - 112, 38, 94, 24, TRUE);
        MoveWindow(st->output,     8, 70, w - 16, h - 78, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, SL_PROP); }
    return CallWindowProcW(g_origSlFrame, hwnd, msg, wp, lp);
}

static HWND Symlink_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    SlState *st;
    HFONT mono;
    wchar_t defaultPath[MAX_PATH];
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Symlink",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (SlState *)calloc(1, sizeof(SlState));
    if (!st) { DestroyWindow(frame); return NULL; }

    /* C:\Users\All Users is a junction on most Windows installs */
    ExpandEnvironmentStringsW(L"%SystemDrive%\\Users\\All Users", defaultPath, MAX_PATH);

    st->pathEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", defaultPath,
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        12, 38, w - 232, 24, frame, (HMENU)(LONG_PTR)ID_SL_PATH, hInstance, NULL);
    st->inspectBtn = CreateWindowExW(0, L"BUTTON", L"Inspect",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        w - 216, 38, 100, 24, frame, (HMENU)(LONG_PTR)ID_SL_INSP, hInstance, NULL);
    st->makeBtn = CreateWindowExW(0, L"BUTTON", L"Make demo",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        w - 112, 38, 94, 24, frame, (HMENU)(LONG_PTR)ID_SL_MAKE, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 70, w - 16, h - 78, frame, (HMENU)(LONG_PTR)ID_SL_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, SL_PROP, (HANDLE)st);
    if (!g_origSlFrame) g_origSlFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Sl_FrameProc);
    Sl_Inspect(st);
    return frame;
}

MsApp g_AppSymlink = { L"Symlink", Symlink_Create, 780, 460 };
