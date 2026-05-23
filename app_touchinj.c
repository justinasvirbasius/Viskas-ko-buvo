/*
 * app_touchinj.c — Synthetic touch via InitializeTouchInjection
 *
 * Demonstrates the touch injection API (user32) — Win 8+ extension for
 * automated UI testing and accessibility tools that need to drive a
 * touch-capable app from another process:
 *   - InitializeTouchInjection(maxContacts, dwMode) — call once per
 *     process; mode is TOUCH_FEEDBACK_DEFAULT/INDIRECT/NONE
 *   - InjectTouchInput(count, POINTER_TOUCH_INFO*) — submits a batch of
 *     simultaneous touch points; each POINTER_TOUCH_INFO has
 *     pointerInfo.pointerFlags = POINTER_FLAG_DOWN/UPDATE/UP combined
 *     with POINTER_FLAG_INRANGE/INCONTACT
 *   - Coordinates are in pixels in screen space
 *
 * Loaded dynamically because pre-Win8 systems don't export it. We trace
 * a small horizontal line on the desktop with a single synthetic finger
 * (10 frames: DOWN → 8 × UPDATE → UP).
 */

#include "shell.h"
#include <stdlib.h>
#include <stdio.h>

/* Some MinGW headers omit these; declare manually. */
typedef DWORD POINTER_INPUT_TYPE;
#ifndef PT_TOUCH
#define PT_TOUCH 0x00000002
#endif
#ifndef POINTER_FLAG_NONE
#define POINTER_FLAG_NONE         0x00000000
#define POINTER_FLAG_NEW          0x00000001
#define POINTER_FLAG_INRANGE      0x00000002
#define POINTER_FLAG_INCONTACT    0x00000004
#define POINTER_FLAG_FIRSTBUTTON  0x00000010
#define POINTER_FLAG_DOWN         0x00010000
#define POINTER_FLAG_UPDATE       0x00020000
#define POINTER_FLAG_UP           0x00040000
#endif
#ifndef TOUCH_FEEDBACK_DEFAULT
#define TOUCH_FEEDBACK_DEFAULT  0x1
#define TOUCH_FEEDBACK_INDIRECT 0x2
#define TOUCH_FEEDBACK_NONE     0x3
#endif

typedef struct {
    UINT32             pointerType;
    UINT32             pointerId;
    UINT32             frameId;
    UINT32             pointerFlags;
    HANDLE             sourceDevice;
    HWND               hwndTarget;
    POINT              ptPixelLocation;
    POINT              ptHimetricLocation;
    POINT              ptPixelLocationRaw;
    POINT              ptHimetricLocationRaw;
    DWORD              dwTime;
    UINT32             historyCount;
    INT32              InputData;
    DWORD              dwKeyStates;
    UINT64             PerformanceCount;
    INT32              ButtonChangeType;
} MS_POINTER_INFO;

typedef enum {
    MS_TOUCH_MASK_NONE        = 0x00000000,
    MS_TOUCH_MASK_CONTACTAREA = 0x00000001,
    MS_TOUCH_MASK_ORIENTATION = 0x00000002,
    MS_TOUCH_MASK_PRESSURE    = 0x00000004
} MS_TOUCH_MASK;

typedef struct {
    MS_POINTER_INFO  pointerInfo;
    MS_TOUCH_MASK    touchMask;
    RECT             rcContact;
    RECT             rcContactRaw;
    UINT32           orientation;
    UINT32           pressure;
} MS_POINTER_TOUCH_INFO;

typedef BOOL (WINAPI *PFN_InitializeTouchInjection)(UINT32, DWORD);
typedef BOOL (WINAPI *PFN_InjectTouchInput)(UINT32, const MS_POINTER_TOUCH_INFO *);

#define TI_PROP   L"MS_TI_STATE"
#define ID_TI_GO  109001
#define ID_TI_OUT 109002

typedef struct {
    HWND     output;
    HMODULE  user32;
    PFN_InitializeTouchInjection pInit;
    PFN_InjectTouchInput         pInject;
    BOOL     initialized;
} TiState;
static WNDPROC g_origTiFrame = NULL;

static void Ti_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
}

static void Ti_Demo(TiState *st)
{
    MS_POINTER_TOUCH_INFO ti;
    int i;
    int x0 = 400, y0 = 300;

    SetWindowTextW(st->output, L"");
    if (!st->pInit || !st->pInject) {
        Ti_Append(st->output, L"Touch injection unavailable (pre-Win 8?).\r\n");
        return;
    }

    if (!st->initialized) {
        if (!st->pInit(1, TOUCH_FEEDBACK_DEFAULT)) {
            wchar_t buf[200];
            swprintf_s(buf, 200,
                L"InitializeTouchInjection failed (err %lu).\r\n"
                L"This API typically requires either an injection-capable\r\n"
                L"runtime or running as the interactive user.\r\n",
                GetLastError());
            Ti_Append(st->output, buf);
            return;
        }
        st->initialized = TRUE;
        Ti_Append(st->output, L"Touch injection initialized.\r\n");
    }

    Ti_Append(st->output, L"Sending 10-frame horizontal touch stroke...\r\n");

    for (i = 0; i < 10; ++i) {
        ZeroMemory(&ti, sizeof(ti));
        ti.pointerInfo.pointerType = PT_TOUCH;
        ti.pointerInfo.pointerId = 0;
        ti.pointerInfo.ptPixelLocation.x = x0 + i * 10;
        ti.pointerInfo.ptPixelLocation.y = y0;
        if (i == 0) {
            ti.pointerInfo.pointerFlags = POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
        } else if (i == 9) {
            ti.pointerInfo.pointerFlags = POINTER_FLAG_UP;
        } else {
            ti.pointerInfo.pointerFlags = POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
        }
        ti.touchMask = MS_TOUCH_MASK_CONTACTAREA;
        ti.rcContact.left   = ti.pointerInfo.ptPixelLocation.x - 2;
        ti.rcContact.top    = ti.pointerInfo.ptPixelLocation.y - 2;
        ti.rcContact.right  = ti.pointerInfo.ptPixelLocation.x + 2;
        ti.rcContact.bottom = ti.pointerInfo.ptPixelLocation.y + 2;

        if (!st->pInject(1, &ti)) {
            wchar_t buf[100];
            swprintf_s(buf, 100, L"  frame %d InjectTouchInput failed: %lu\r\n",
                       i, GetLastError());
            Ti_Append(st->output, buf);
            break;
        }
        Sleep(16);
    }
    Ti_Append(st->output, L"Stroke complete.\r\n");
}

static LRESULT CALLBACK Ti_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    TiState *st = (TiState *)GetPropW(hwnd, TI_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_TI_GO) { Ti_Demo(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->user32) FreeLibrary(st->user32);
        free(st); RemovePropW(hwnd, TI_PROP);
    }
    return CallWindowProcW(g_origTiFrame, hwnd, msg, wp, lp);
}

static HWND TouchInj_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    TiState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"TouchInj",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (TiState *)calloc(1, sizeof(TiState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->user32 = LoadLibraryW(L"user32.dll");
    if (st->user32) {
        st->pInit   = (PFN_InitializeTouchInjection)GetProcAddress(st->user32, "InitializeTouchInjection");
        st->pInject = (PFN_InjectTouchInput)        GetProcAddress(st->user32, "InjectTouchInput");
    }

    CreateWindowExW(0, L"BUTTON", L"Synthetic touch",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 150, 26, frame, (HMENU)(LONG_PTR)ID_TI_GO, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"Injects a 10-frame horizontal touch stroke at screen (400,300).\r\n"
        L"On Win 10+ a touch indicator briefly appears at the stroke location.\r\n",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_TI_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, TI_PROP, (HANDLE)st);
    if (!g_origTiFrame) g_origTiFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Ti_FrameProc);
    return frame;
}

MsApp g_AppTouchInj = { L"TouchInj", TouchInj_Create, 700, 380 };
