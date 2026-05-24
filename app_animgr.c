/*
 * app_animgr.c — Windows Animation Manager (uianimation.h)
 *
 * Demonstrates the Windows Animation Manager — the timeline engine
 * behind shell, taskbar, and Aero animations. Distinct from DComp
 * (Batch 15) and from Direct2D animations:
 *
 *   - CoCreateInstance(CLSID_UIAnimationManager, IID_IUIAnimationManager)
 *   - CoCreateInstance(CLSID_UIAnimationTimer, IID_IUIAnimationTimer) —
 *     a high-resolution clock that returns UI_ANIMATION_SECONDS
 *   - CoCreateInstance(CLSID_UIAnimationTransitionLibrary,
 *     IID_IUIAnimationTransitionLibrary) — factory of curve transitions
 *   - IUIAnimationManager::CreateAnimationVariable(initialValue, &var)
 *   - IUIAnimationTransitionLibrary::CreateLinearTransition(duration,
 *     finalValue, &transition)
 *   - IUIAnimationManager::ScheduleTransition(var, transition, timeNow)
 *     creates a one-transition storyboard and starts it
 *   - Per-frame: IUIAnimationTimer::GetTime(&now) →
 *     IUIAnimationManager::Update(now) → IUIAnimationVariable::GetValue
 *
 * We animate a colored bar's width to demonstrate the pipeline.
 */

#define COBJMACROS
#define CINTERFACE
#define INITGUID

#include "shell.h"
#include <uianimation.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "ole32.lib")

#define AN_PROP   L"MS_AN_STATE"
#define ID_AN_GO  121001
#define AN_TIMER  1

typedef struct {
    IUIAnimationManager           *mgr;
    IUIAnimationTimer             *timer;
    IUIAnimationTransitionLibrary *lib;
    IUIAnimationVariable          *xVar;
    UINT_PTR                       timerId;
    double                         currentValue;
    BOOL                           animating;
    BOOL                           direction;       /* TRUE: growing to 1.0, FALSE: shrinking to 0.0 */
} AnState;

static WNDPROC g_origAnFrame = NULL;

static void An_StartTransition(AnState *st)
{
    IUIAnimationTransition *trans = NULL;
    UI_ANIMATION_SECONDS now = 0.0;
    DOUBLE target = st->direction ? 1.0 : 0.0;
    if (!st->lib || !st->mgr || !st->timer || !st->xVar) return;

    if (FAILED(IUIAnimationTransitionLibrary_CreateLinearTransition(
                st->lib, 1.2, target, &trans)) || !trans) return;
    if (SUCCEEDED(IUIAnimationTimer_GetTime(st->timer, &now))) {
        IUIAnimationManager_ScheduleTransition(st->mgr, st->xVar, trans, now);
        st->animating = TRUE;
        st->direction = !st->direction;  /* alternate next time */
    }
    IUIAnimationTransition_Release(trans);
}

static void An_OnTimer(HWND hwnd, AnState *st)
{
    UI_ANIMATION_SECONDS now;
    if (!st->mgr || !st->timer) return;
    if (FAILED(IUIAnimationTimer_GetTime(st->timer, &now))) return;
    IUIAnimationManager_Update(st->mgr, now, NULL);
    if (st->xVar) {
        DOUBLE v = 0;
        IUIAnimationVariable_GetValue(st->xVar, &v);
        st->currentValue = v;
    }
    InvalidateRect(hwnd, NULL, FALSE);

    /* Check if still busy */
    {
        UI_ANIMATION_MANAGER_STATUS status = UI_ANIMATION_MANAGER_IDLE;
        IUIAnimationManager_GetStatus(st->mgr, &status);
        if (status == UI_ANIMATION_MANAGER_IDLE && st->animating) {
            st->animating = FALSE;
        }
    }
}

static void An_Paint(HWND hwnd, AnState *st)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT client, bar;
    HBRUSH bg, fg, track;
    HFONT font, oldFont;
    wchar_t info[120];

    GetClientRect(hwnd, &client);
    bg = CreateSolidBrush(RGB(245, 245, 250));
    FillRect(hdc, &client, bg);
    DeleteObject(bg);

    /* Track */
    track = CreateSolidBrush(RGB(220, 220, 230));
    bar.left = 24; bar.right = client.right - 24;
    bar.top = client.bottom / 2 - 22;
    bar.bottom = bar.top + 44;
    FillRect(hdc, &bar, track);
    DeleteObject(track);

    /* Bar */
    fg = CreateSolidBrush(RGB(58, 140, 230));
    {
        int trackW = bar.right - bar.left;
        RECT fill = bar;
        double v = st->currentValue;
        if (v < 0) v = 0; if (v > 1.0) v = 1.0;
        fill.right = bar.left + (int)(trackW * v);
        FillRect(hdc, &fill, fg);
    }
    DeleteObject(fg);

    font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    oldFont = (HFONT)SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);
    TextOutW(hdc, 24, 40,
        L"Click Animate. IUIAnimationManager interpolates value 0.0 → 1.0 over 1.2 s.",
        76);
    swprintf_s(info, 120, L"current value: %.4f   (animation %s)",
               st->currentValue, st->animating ? L"running" : L"idle");
    TextOutW(hdc, 24, bar.bottom + 14, info, (int)wcslen(info));
    SelectObject(hdc, oldFont);

    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK An_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    AnState *st = (AnState *)GetPropW(hwnd, AN_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_AN_GO) {
        An_StartTransition(st);
        return 0;
    }
    if (msg == WM_TIMER && st && wp == AN_TIMER) { An_OnTimer(hwnd, st); return 0; }
    if (msg == WM_PAINT && st) { An_Paint(hwnd, st); return 0; }
    if (msg == WM_DESTROY && st) {
        if (st->timerId) KillTimer(hwnd, st->timerId);
        if (st->xVar)   IUIAnimationVariable_Release(st->xVar);
        if (st->lib)    IUIAnimationTransitionLibrary_Release(st->lib);
        if (st->timer)  IUIAnimationTimer_Release(st->timer);
        if (st->mgr)    IUIAnimationManager_Release(st->mgr);
        free(st); RemovePropW(hwnd, AN_PROP);
    }
    return CallWindowProcW(g_origAnFrame, hwnd, msg, wp, lp);
}

static HWND AniMgr_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    AnState *st;
    HRESULT hr;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"AniMgr",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (AnState *)calloc(1, sizeof(AnState));
    if (!st) { DestroyWindow(frame); return NULL; }
    st->direction = TRUE;

    hr = CoCreateInstance(&CLSID_UIAnimationManager, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IUIAnimationManager, (void **)&st->mgr);
    if (SUCCEEDED(hr)) {
        CoCreateInstance(&CLSID_UIAnimationTimer, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IUIAnimationTimer, (void **)&st->timer);
        CoCreateInstance(&CLSID_UIAnimationTransitionLibrary, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IUIAnimationTransitionLibrary, (void **)&st->lib);
        if (st->mgr) {
            IUIAnimationManager_CreateAnimationVariable(st->mgr, 0.0, &st->xVar);
        }
    }

    CreateWindowExW(0, L"BUTTON", L"Animate",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 100, 26, frame, (HMENU)(LONG_PTR)ID_AN_GO, hInstance, NULL);

    SetPropW(frame, AN_PROP, (HANDLE)st);
    if (!g_origAnFrame) g_origAnFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)An_FrameProc);

    st->timerId = SetTimer(frame, AN_TIMER, 16, NULL);
    return frame;
}

MsApp g_AppAniMgr = { L"AniMgr", AniMgr_Create, 640, 360 };
