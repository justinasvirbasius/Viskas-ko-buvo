/*
 * app_mailslot.c — One-way IPC via mailslots
 *
 * Demonstrates the mailslot API, an old but still-present Windows IPC
 * primitive that's distinct from named pipes:
 *   - CreateMailslotW to become the receiver (server)
 *   - CreateFileW with \\.\mailslot\... to become a writer (client)
 *   - GetMailslotInfo to poll for available messages
 *
 * Unlike named pipes, mailslots are connection-less and one-way (writer →
 * reader). Multiple writers can target the same slot; the reader sees each
 * message as a discrete record. They also support broadcast targets but we
 * stick to a single named slot here.
 *
 * As with PipeChat, the first instance becomes SERVER (creates the slot),
 * subsequent instances become CLIENT (open it for writing).
 */

#include "shell.h"
#include <stdlib.h>
#include <stdio.h>

#define MS_PROP    L"MS_SLOT_STATE"
#define ID_MS_IN   27001
#define ID_MS_SEND 27002
#define ID_MS_OUT  27003
#define ID_MS_MODE 27004
#define MS_TIMER   1
#define MS_SLOT    L"\\\\.\\mailslot\\MiniShell_Slot"

typedef enum { MS_UNKNOWN, MS_SERVER, MS_CLIENT } MsRole;

typedef struct {
    HWND     input, output, sendBtn, modeLbl;
    HANDLE   slot;     /* server: receive handle; client: write handle */
    MsRole   role;
} MsSlotState;

static WNDPROC g_origMsFrame = NULL;

static void Ms_Append(HWND output, const wchar_t *text)
{
    int len = GetWindowTextLengthW(output);
    SendMessageW(output, EM_SETSEL, len, len);
    SendMessageW(output, EM_REPLACESEL, FALSE, (LPARAM)text);
    SendMessageW(output, EM_SCROLLCARET, 0, 0);
}

static void Ms_PollServer(MsSlotState *st)
{
    DWORD next = 0, count = 0;
    if (!GetMailslotInfo(st->slot, NULL, &next, &count, NULL)) return;
    if (next == MAILSLOT_NO_MESSAGE) return;

    while (count > 0) {
        char  bytes[2048];
        DWORD nRead = 0;
        if (!ReadFile(st->slot, bytes, sizeof(bytes) - 1, &nRead, NULL)) break;
        bytes[nRead] = 0;
        {
            wchar_t wbuf[2100], line[2200];
            int cw = MultiByteToWideChar(CP_UTF8, 0, bytes, (int)nRead, wbuf, 2099);
            wbuf[cw] = 0;
            swprintf_s(line, 2200, L"<msg> %s\r\n", wbuf);
            Ms_Append(st->output, line);
        }
        if (!GetMailslotInfo(st->slot, NULL, &next, &count, NULL)) break;
    }
}

static void Ms_Send(MsSlotState *st)
{
    wchar_t text[512];
    char    bytes[1100];
    int     n;
    DWORD   nWritten;

    if (!st->slot || st->role != MS_CLIENT) return;
    GetWindowTextW(st->input, text, 512);
    if (text[0] == 0) return;
    SetWindowTextW(st->input, L"");

    n = WideCharToMultiByte(CP_UTF8, 0, text, -1, bytes, sizeof(bytes), NULL, NULL);
    if (n <= 0) return;
    WriteFile(st->slot, bytes, n - 1, &nWritten, NULL);

    {
        wchar_t echo[600];
        swprintf_s(echo, 600, L"<sent> %s\r\n", text);
        Ms_Append(st->output, echo);
    }
}

static LRESULT CALLBACK Ms_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    MsSlotState *st = (MsSlotState *)GetPropW(hwnd, MS_PROP);

    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_MS_SEND) { Ms_Send(st); return 0; }
    if (msg == WM_TIMER && st && st->role == MS_SERVER) { Ms_PollServer(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->modeLbl, 8, 32, w - 16, 18, TRUE);
        MoveWindow(st->output,  8, 54, w - 16, h - 96, TRUE);
        MoveWindow(st->input,   8, h - 36, w - 100, 24, TRUE);
        MoveWindow(st->sendBtn, w - 88, h - 36, 80, 24, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        KillTimer(hwnd, MS_TIMER);
        if (st->slot && st->slot != INVALID_HANDLE_VALUE) CloseHandle(st->slot);
        free(st);
        RemovePropW(hwnd, MS_PROP);
    }
    return CallWindowProcW(g_origMsFrame, hwnd, msg, wp, lp);
}

static WNDPROC g_origMsInput = NULL;
static LRESULT CALLBACK Ms_InputProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        MsSlotState *st = (MsSlotState *)GetPropW(GetParent(hwnd), MS_PROP);
        if (st) Ms_Send(st);
        return 0;
    }
    if (msg == WM_CHAR && wp == VK_RETURN) return 0;
    return CallWindowProcW(g_origMsInput, hwnd, msg, wp, lp);
}

static HWND MailSlot_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    MsSlotState *st;
    HANDLE serverProbe;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"MailSlot",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (MsSlotState *)calloc(1, sizeof(MsSlotState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->modeLbl = CreateWindowExW(0, L"STATIC", L"(deciding role…)",
        WS_CHILD | WS_VISIBLE,
        8, 32, w - 16, 18, frame, (HMENU)(LONG_PTR)ID_MS_MODE, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        8, 54, w - 16, h - 96, frame, (HMENU)(LONG_PTR)ID_MS_OUT, hInstance, NULL);

    st->input = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        8, h - 36, w - 100, 24, frame, (HMENU)(LONG_PTR)ID_MS_IN, hInstance, NULL);

    st->sendBtn = CreateWindowExW(0, L"BUTTON", L"Send",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        w - 88, h - 36, 80, 24, frame, (HMENU)(LONG_PTR)ID_MS_SEND, hInstance, NULL);

    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, MS_PROP, (HANDLE)st);
    if (!g_origMsFrame) g_origMsFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Ms_FrameProc);

    g_origMsInput = (WNDPROC)GetWindowLongPtrW(st->input, GWLP_WNDPROC);
    SetWindowLongPtrW(st->input, GWLP_WNDPROC, (LONG_PTR)Ms_InputProc);

    /* Decide role. Try to open as a client first. If that succeeds, another
     * instance is already running as server. Otherwise, become the server. */
    serverProbe = CreateFileW(MS_SLOT, GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);
    if (serverProbe != INVALID_HANDLE_VALUE) {
        st->slot = serverProbe;
        st->role = MS_CLIENT;
        SetWindowTextW(st->modeLbl,
            L"Role: CLIENT (writing into existing slot)");
        EnableWindow(st->input, TRUE);
    } else {
        st->slot = CreateMailslotW(MS_SLOT, 0, MAILSLOT_WAIT_FOREVER, NULL);
        if (st->slot == INVALID_HANDLE_VALUE) {
            SetWindowTextW(st->modeLbl, L"CreateMailslot failed");
            EnableWindow(st->input, FALSE);
            EnableWindow(st->sendBtn, FALSE);
        } else {
            st->role = MS_SERVER;
            SetWindowTextW(st->modeLbl,
                L"Role: SERVER — open another MailSlot to send messages here");
            EnableWindow(st->input, FALSE);
            EnableWindow(st->sendBtn, FALSE);
            SetTimer(frame, MS_TIMER, 300, NULL);
        }
    }
    return frame;
}

MsApp g_AppMailSlot = {
    L"MailSlot",
    MailSlot_Create,
    520, 360
};
