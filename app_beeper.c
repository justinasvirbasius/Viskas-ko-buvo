/*
 * app_beeper.c — Tiny tone composer
 *
 * Demonstrates:
 *   - Beep(freq, durationMs) — the simplest possible Windows audio
 *   - A worker thread that walks a small sequence, sleeping/Beeping each step
 *   - Cancellation via an event handle (SetEvent + WaitForSingleObject)
 *
 * The UI is a row of toggle buttons, each representing a note in a short
 * melody slot. "Play" kicks off the worker; "Stop" signals cancellation.
 */

#include "shell.h"
#include <stdlib.h>

#define BP_PROP      L"MS_BP_STATE"
#define ID_PLAY      10001
#define ID_STOP      10002
#define ID_NOTE_BASE 10100

#define NUM_STEPS 12
#define NUM_NOTES 8

/* C major scale, octave 4-5 */
static const DWORD kFreqs[NUM_NOTES] = {
    262, 294, 330, 349, 392, 440, 494, 523
};
static const wchar_t *kNoteNames[NUM_NOTES] = {
    L"C", L"D", L"E", L"F", L"G", L"A", L"B", L"C+"
};

typedef struct {
    int    pattern[NUM_STEPS];   /* -1 = rest, else note index 0..7 */
    HWND   buttons[NUM_STEPS];
    HWND   playBtn, stopBtn;
    HANDLE thread;
    HANDLE stopEvent;
} BpState;

static WNDPROC g_origBpFrame = NULL;

static DWORD WINAPI Bp_Worker(LPVOID arg)
{
    BpState *st = (BpState *)arg;
    int i;
    for (i = 0; i < NUM_STEPS; ++i) {
        if (WaitForSingleObject(st->stopEvent, 0) == WAIT_OBJECT_0) break;
        if (st->pattern[i] >= 0) {
            Beep(kFreqs[st->pattern[i]], 180);  /* Beep blocks for duration */
        } else {
            if (WaitForSingleObject(st->stopEvent, 180) == WAIT_OBJECT_0) break;
        }
        if (WaitForSingleObject(st->stopEvent, 40) == WAIT_OBJECT_0) break;
    }
    return 0;
}

static void Bp_UpdateButton(BpState *st, int step)
{
    int n = st->pattern[step];
    SetWindowTextW(st->buttons[step],
                   n < 0 ? L"--" : kNoteNames[n]);
}

static void Bp_CycleNote(BpState *st, int step)
{
    st->pattern[step] = (st->pattern[step] + 1) % (NUM_NOTES + 1);
    if (st->pattern[step] == NUM_NOTES) st->pattern[step] = -1;
    Bp_UpdateButton(st, step);
}

static LRESULT CALLBACK Bp_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    BpState *st = (BpState *)GetPropW(hwnd, BP_PROP);

    if (msg == WM_COMMAND && st) {
        int id = LOWORD(wp);
        if (id == ID_PLAY) {
            DWORD tid;
            if (st->thread) {
                WaitForSingleObject(st->thread, 5000);
                CloseHandle(st->thread);
                st->thread = NULL;
            }
            ResetEvent(st->stopEvent);
            st->thread = CreateThread(NULL, 0, Bp_Worker, st, 0, &tid);
            return 0;
        }
        if (id == ID_STOP) {
            SetEvent(st->stopEvent);
            return 0;
        }
        if (id >= ID_NOTE_BASE && id < ID_NOTE_BASE + NUM_STEPS) {
            Bp_CycleNote(st, id - ID_NOTE_BASE);
            return 0;
        }
    }
    if (msg == WM_DESTROY && st) {
        SetEvent(st->stopEvent);
        if (st->thread) {
            WaitForSingleObject(st->thread, 2000);
            CloseHandle(st->thread);
        }
        if (st->stopEvent) CloseHandle(st->stopEvent);
        free(st);
        RemovePropW(hwnd, BP_PROP);
    }
    return CallWindowProcW(g_origBpFrame, hwnd, msg, wp, lp);
}

static HWND Beeper_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    BpState *st;
    int i, btnW, btnH = 32, gap = 4;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Beeper",
        WS_POPUP | WS_BORDER,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (BpState *)calloc(1, sizeof(BpState));
    if (!st) { DestroyWindow(frame); return NULL; }
    st->stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    for (i = 0; i < NUM_STEPS; ++i) st->pattern[i] = -1;

    btnW = (w - 16 - gap * (NUM_STEPS - 1)) / NUM_STEPS;
    for (i = 0; i < NUM_STEPS; ++i) {
        st->buttons[i] = CreateWindowExW(0, L"BUTTON", L"--",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            8 + i * (btnW + gap), 40, btnW, btnH,
            frame, (HMENU)(LONG_PTR)(ID_NOTE_BASE + i), hInstance, NULL);
    }
    /* Set a default melody: C E G C+ E G C+ -- C E G C+ */
    {
        int defaults[NUM_STEPS] = { 0, 2, 4, 7, 2, 4, 7, -1, 0, 2, 4, 7 };
        for (i = 0; i < NUM_STEPS; ++i) {
            st->pattern[i] = defaults[i];
            Bp_UpdateButton(st, i);
        }
    }

    st->playBtn = CreateWindowExW(0, L"BUTTON", L"Play",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        8, 84, 90, 28, frame, (HMENU)(LONG_PTR)ID_PLAY, hInstance, NULL);
    st->stopBtn = CreateWindowExW(0, L"BUTTON", L"Stop",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        108, 84, 90, 28, frame, (HMENU)(LONG_PTR)ID_STOP, hInstance, NULL);

    /* Hint label */
    CreateWindowExW(0, L"STATIC", L"Click a step to cycle: C D E F G A B C+ --",
        WS_CHILD | WS_VISIBLE,
        8, 122, w - 16, 18, frame, NULL, hInstance, NULL);

    SetPropW(frame, BP_PROP, (HANDLE)st);
    if (!g_origBpFrame)
        g_origBpFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Bp_FrameProc);
    return frame;
}

MsApp g_AppBeeper = {
    L"Beeper",
    Beeper_Create,
    560, 160
};
