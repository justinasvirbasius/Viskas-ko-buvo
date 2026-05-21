/*
 * app_miclevel.c — Microphone level meter via waveIn (mmsystem)
 *
 * Demonstrates the older but simpler audio input API:
 *   - waveInOpen with a callback function (CALLBACK_FUNCTION)
 *   - waveInPrepareHeader + waveInAddBuffer for double-buffered capture
 *   - MM_WIM_DATA notification when each buffer fills
 *   - Computing peak amplitude in the callback and signaling the UI
 *
 * The UI shows a horizontal level bar that updates in real time. No audio
 * is stored or transmitted — only the peak level per buffer is read off.
 */

#include "shell.h"
#include <commctrl.h>
#include <mmsystem.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winmm.lib")

#define ML_PROP   L"MS_ML_STATE"
#define ID_ML_GO  36001
#define ID_ML_STOP 36002
#define ID_ML_BAR  36003
#define ID_ML_LBL  36004

#define BUF_FRAMES  4410   /* ~100 ms @ 44100 Hz */
#define NUM_BUFFERS 2

#define WM_ML_LEVEL (WM_USER + 130)   /* wparam = peak 0..32767 */

typedef struct {
    HWAVEIN     wavein;
    WAVEHDR     headers[NUM_BUFFERS];
    short      *buffers[NUM_BUFFERS];
    HWND        frame;
    int         running;
} MlState;

static WNDPROC g_origMlFrame = NULL;

static void CALLBACK Ml_WaveCb(HWAVEIN hwi, UINT msg, DWORD_PTR userData,
                               DWORD_PTR p1, DWORD_PTR p2)
{
    MlState *st = (MlState *)userData;
    (void)p2;

    if (msg == WIM_DATA) {
        WAVEHDR *hdr = (WAVEHDR *)p1;
        short  *samples = (short *)hdr->lpData;
        int     n = hdr->dwBytesRecorded / 2;
        int     i, peak = 0;
        for (i = 0; i < n; ++i) {
            int v = samples[i];
            if (v < 0) v = -v;
            if (v > peak) peak = v;
        }
        PostMessageW(st->frame, WM_ML_LEVEL, (WPARAM)peak, 0);
        if (st->running) {
            waveInAddBuffer(hwi, hdr, sizeof(*hdr));
        }
    }
}

static void Ml_Start(MlState *st)
{
    WAVEFORMATEX wfx;
    MMRESULT mr;
    int i;

    if (st->running) return;

    ZeroMemory(&wfx, sizeof(wfx));
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 1;
    wfx.nSamplesPerSec  = 44100;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    mr = waveInOpen(&st->wavein, WAVE_MAPPER, &wfx,
                    (DWORD_PTR)Ml_WaveCb, (DWORD_PTR)st,
                    CALLBACK_FUNCTION);
    if (mr != MMSYSERR_NOERROR) {
        MessageBoxW(st->frame, L"waveInOpen failed (no microphone, or permission denied).",
                    L"MicLevel", MB_ICONWARNING);
        return;
    }

    st->running = 1;
    for (i = 0; i < NUM_BUFFERS; ++i) {
        st->buffers[i] = (short *)calloc(BUF_FRAMES, sizeof(short));
        ZeroMemory(&st->headers[i], sizeof(WAVEHDR));
        st->headers[i].lpData         = (LPSTR)st->buffers[i];
        st->headers[i].dwBufferLength = BUF_FRAMES * sizeof(short);
        waveInPrepareHeader(st->wavein, &st->headers[i], sizeof(WAVEHDR));
        waveInAddBuffer(st->wavein, &st->headers[i], sizeof(WAVEHDR));
    }
    waveInStart(st->wavein);
}

static void Ml_Stop(MlState *st)
{
    int i;
    if (!st->running) return;
    st->running = 0;
    waveInStop(st->wavein);
    waveInReset(st->wavein);
    for (i = 0; i < NUM_BUFFERS; ++i) {
        waveInUnprepareHeader(st->wavein, &st->headers[i], sizeof(WAVEHDR));
        free(st->buffers[i]);
        st->buffers[i] = NULL;
    }
    waveInClose(st->wavein);
    st->wavein = NULL;
}

static LRESULT CALLBACK Ml_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    MlState *st = (MlState *)GetPropW(hwnd, ML_PROP);

    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_ML_GO)   { Ml_Start(st); return 0; }
        if (LOWORD(wp) == ID_ML_STOP) { Ml_Stop(st);  return 0; }
    }
    if (msg == WM_ML_LEVEL && st) {
        int peak = (int)wp;
        int percent = (peak * 100) / 32767;
        HWND bar = GetDlgItem(hwnd, ID_ML_BAR);
        HWND lbl = GetDlgItem(hwnd, ID_ML_LBL);
        wchar_t buf[40];
        SendMessageW(bar, PBM_SETPOS, (WPARAM)percent, 0);
        swprintf_s(buf, 40, L"Peak: %d / 32767 (%d%%)", peak, percent);
        SetWindowTextW(lbl, buf);
        return 0;
    }
    if (msg == WM_DESTROY && st) {
        Ml_Stop(st);
        free(st);
        RemovePropW(hwnd, ML_PROP);
    }
    return CallWindowProcW(g_origMlFrame, hwnd, msg, wp, lp);
}

static HWND MicLevel_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    MlState *st;
    HWND bar;
    INITCOMMONCONTROLSEX icc;
    (void)self;

    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"MicLevel",
        WS_POPUP | WS_BORDER,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (MlState *)calloc(1, sizeof(MlState));
    if (!st) { DestroyWindow(frame); return NULL; }
    st->frame = frame;

    CreateWindowExW(0, L"STATIC",
        L"Live peak-level meter from the default microphone.\n"
        L"(No audio is captured or stored.)",
        WS_CHILD | WS_VISIBLE,
        12, 40, w - 24, 36, frame, NULL, hInstance, NULL);

    bar = CreateWindowExW(0, PROGRESS_CLASSW, NULL,
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        12, 84, w - 24, 24, frame, (HMENU)(LONG_PTR)ID_ML_BAR, hInstance, NULL);
    SendMessageW(bar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

    CreateWindowExW(0, L"STATIC", L"Peak: 0",
        WS_CHILD | WS_VISIBLE,
        12, 114, w - 24, 22, frame, (HMENU)(LONG_PTR)ID_ML_LBL, hInstance, NULL);

    CreateWindowExW(0, L"BUTTON", L"Start",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 142, 100, 28, frame, (HMENU)(LONG_PTR)ID_ML_GO, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Stop",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        124, 142, 100, 28, frame, (HMENU)(LONG_PTR)ID_ML_STOP, hInstance, NULL);

    SetPropW(frame, ML_PROP, (HANDLE)st);
    if (!g_origMlFrame) g_origMlFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Ml_FrameProc);
    return frame;
}

MsApp g_AppMicLevel = {
    L"MicLevel",
    MicLevel_Create,
    340, 220
};
