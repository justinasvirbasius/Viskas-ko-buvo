/*
 * app_broadcast.c — Broadcast messages with SendMessageTimeout
 *
 * Demonstrates inter-window broadcast notifications:
 *   - SendMessageTimeoutW(HWND_BROADCAST, msg, ..., SMTO_ABORTIFHUNG | SMTO_NORMAL,
 *                          5000, &result)
 *   - WM_SETTINGCHANGE broadcast (the standard "settings changed" signal —
 *     conventionally posted with lParam pointing to an environment variable
 *     name like L"Environment")
 *   - WM_FONTCHANGE broadcast (font list changed)
 *   - WM_THEMECHANGED equivalent via WM_SETTINGCHANGE
 *
 * Also registers a *received* listener: this window receives WM_SETTINGCHANGE
 * itself (since it's a top-level window in HWND_BROADCAST), so you can verify
 * the broadcast from a second instance.
 */

#include "shell.h"
#include <stdlib.h>
#include <stdio.h>

#define BC_PROP    L"MS_BC_STATE"
#define ID_BC_OUT  51001
#define ID_BC_ENV  51002
#define ID_BC_FONT 51003
#define ID_BC_GEN  51004

typedef struct {
    HWND output;
    int  received;
} BcState;

static WNDPROC g_origBcFrame = NULL;

static void Bc_Append(BcState *st, const wchar_t *t)
{
    int len = GetWindowTextLengthW(st->output);
    SendMessageW(st->output, EM_SETSEL, len, len);
    SendMessageW(st->output, EM_REPLACESEL, FALSE, (LPARAM)t);
    SendMessageW(st->output, EM_SCROLLCARET, 0, 0);
}

static void Bc_Broadcast(BcState *st, UINT msg, WPARAM wp, LPARAM lp,
                         const wchar_t *label)
{
    DWORD_PTR result = 0;
    LRESULT lr;
    wchar_t buf[200];

    lr = SendMessageTimeoutW(HWND_BROADCAST, msg, wp, lp,
                              SMTO_ABORTIFHUNG | SMTO_NORMAL, 3000, &result);
    if (lr) {
        swprintf_s(buf, 200,
            L"→ broadcast %s sent (result=0x%llx)\r\n",
            label, (unsigned long long)result);
    } else {
        swprintf_s(buf, 200,
            L"→ broadcast %s timed out or failed (err=%lu)\r\n",
            label, GetLastError());
    }
    Bc_Append(st, buf);
}

static LRESULT CALLBACK Bc_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    BcState *st = (BcState *)GetPropW(hwnd, BC_PROP);

    if (msg == WM_COMMAND && st) {
        switch (LOWORD(wp)) {
        case ID_BC_ENV:
            Bc_Broadcast(st, WM_SETTINGCHANGE, 0,
                         (LPARAM)L"Environment", L"WM_SETTINGCHANGE(Environment)");
            return 0;
        case ID_BC_FONT:
            Bc_Broadcast(st, WM_FONTCHANGE, 0, 0, L"WM_FONTCHANGE");
            return 0;
        case ID_BC_GEN:
            Bc_Broadcast(st, WM_SETTINGCHANGE,
                         SPI_SETNONCLIENTMETRICS, 0,
                         L"WM_SETTINGCHANGE(non-client)");
            return 0;
        }
    }

    /* Inbound broadcasts arrive here because we're a top-level window. */
    if (st) {
        if (msg == WM_SETTINGCHANGE) {
            wchar_t buf[240];
            const wchar_t *area = lp ? (const wchar_t *)lp : L"";
            ++st->received;
            swprintf_s(buf, 240,
                L"  [#%d received] WM_SETTINGCHANGE wp=%llu lp=\"%s\"\r\n",
                st->received, (unsigned long long)wp, area ? area : L"");
            Bc_Append(st, buf);
        } else if (msg == WM_FONTCHANGE) {
            ++st->received;
            {
                wchar_t buf[80];
                swprintf_s(buf, 80, L"  [#%d received] WM_FONTCHANGE\r\n",
                           st->received);
                Bc_Append(st, buf);
            }
        }
    }

    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) { free(st); RemovePropW(hwnd, BC_PROP); }
    return CallWindowProcW(g_origBcFrame, hwnd, msg, wp, lp);
}

static HWND Broadcast_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    BcState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Broadcast",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (BcState *)calloc(1, sizeof(BcState));
    if (!st) { DestroyWindow(frame); return NULL; }

    CreateWindowExW(0, L"STATIC",
        L"Broadcasts to HWND_BROADCAST with SendMessageTimeout.\n"
        L"This window also receives, so open two instances to see both sides.",
        WS_CHILD | WS_VISIBLE,
        12, 36, w - 24, 32, frame, NULL, hInstance, NULL);

    CreateWindowExW(0, L"BUTTON", L"Env changed",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 76, 110, 24, frame, (HMENU)(LONG_PTR)ID_BC_ENV, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Font changed",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        130, 76, 110, 24, frame, (HMENU)(LONG_PTR)ID_BC_FONT, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Non-client",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        248, 76, 110, 24, frame, (HMENU)(LONG_PTR)ID_BC_GEN, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 108, w - 16, h - 116, frame, (HMENU)(LONG_PTR)ID_BC_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, BC_PROP, (HANDLE)st);
    if (!g_origBcFrame) g_origBcFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Bc_FrameProc);
    /* Re-fire WM_SIZE so MoveWindow on output gets the right rect */
    SendMessageW(frame, WM_SIZE, 0, MAKELPARAM(w, h));
    return frame;
}

MsApp g_AppBroadcast = {
    L"Broadcast",
    Broadcast_Create,
    520, 360
};
