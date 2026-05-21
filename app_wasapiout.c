/*
 * app_wasapiout.c — WASAPI shared-mode audio playback
 *
 * Demonstrates the modern Windows audio stack from plain C:
 *   - CoCreateInstance(CLSID_MMDeviceEnumerator)
 *   - IMMDeviceEnumerator::GetDefaultAudioEndpoint(eRender, eConsole)
 *   - IMMDevice::Activate(IID_IAudioClient)
 *   - IAudioClient::GetMixFormat (the device's preferred format)
 *   - Initialize(AUDCLNT_SHAREMODE_SHARED) and Start()
 *   - IAudioRenderClient: GetBuffer / ReleaseBuffer in a worker thread
 *
 * Requires INITGUID before including the headers so the IID/CLSID symbols
 * are emitted into this translation unit (no static lib for them).
 *
 * The worker thread writes a 440 Hz sine continuously until stopped.
 */

#define COBJMACROS
#define CINTERFACE
#define INITGUID

#include "shell.h"
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <math.h>
#include <stdlib.h>

#pragma comment(lib, "ole32.lib")

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define WO_PROP    L"MS_WO_STATE"
#define ID_WO_PLAY 35001
#define ID_WO_STOP 35002
#define ID_WO_STAT 35003

typedef struct {
    HANDLE  workerThread;
    HANDLE  stopEvent;
    HWND    status, playBtn, stopBtn;
} WoState;

static WNDPROC g_origWoFrame = NULL;

static void Wo_SetStatus(WoState *st, const wchar_t *text)
{
    SetWindowTextW(st->status, text);
}

static DWORD WINAPI Wo_Worker(LPVOID arg)
{
    WoState *st = (WoState *)arg;
    HRESULT hr;
    IMMDeviceEnumerator *en = NULL;
    IMMDevice           *dev = NULL;
    IAudioClient        *ac  = NULL;
    IAudioRenderClient  *rc  = NULL;
    WAVEFORMATEX        *mix = NULL;
    UINT32 bufferFrames = 0;
    double phase = 0.0;
    REFERENCE_TIME requested = 100 * 10000; /* 100 ms */
    HANDLE frameEvent = NULL;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    /* hr can be S_FALSE if already initialized — both OK */

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&en);
    if (FAILED(hr)) goto done;

    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    if (FAILED(hr)) goto done;

    hr = IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac);
    if (FAILED(hr)) goto done;

    hr = IAudioClient_GetMixFormat(ac, &mix);
    if (FAILED(hr) || !mix) goto done;

    /* Event-driven shared mode: AUDCLNT_STREAMFLAGS_EVENTCALLBACK */
    frameEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            requested, 0, mix, NULL);
    if (FAILED(hr)) goto done;

    IAudioClient_SetEventHandle(ac, frameEvent);
    IAudioClient_GetBufferSize(ac, &bufferFrames);

    hr = IAudioClient_GetService(ac, &IID_IAudioRenderClient, (void **)&rc);
    if (FAILED(hr)) goto done;

    /* Pre-fill the buffer with silence to avoid startup glitches */
    {
        BYTE *data;
        if (SUCCEEDED(IAudioRenderClient_GetBuffer(rc, bufferFrames, &data))) {
            IAudioRenderClient_ReleaseBuffer(rc, bufferFrames,
                AUDCLNT_BUFFERFLAGS_SILENT);
        }
    }
    IAudioClient_Start(ac);

    while (WaitForSingleObject(st->stopEvent, 0) != WAIT_OBJECT_0) {
        UINT32 padding = 0, available;
        BYTE  *data;
        DWORD wait;

        /* Wait until the engine signals our event (each buffer flip) */
        wait = WaitForSingleObject(frameEvent, 200);
        if (wait == WAIT_TIMEOUT) continue;

        IAudioClient_GetCurrentPadding(ac, &padding);
        available = bufferFrames - padding;
        if (available == 0) continue;

        if (FAILED(IAudioRenderClient_GetBuffer(rc, available, &data))) continue;

        /* Synthesize a 440 Hz sine into the mix format */
        {
            UINT32 i;
            double inc = 2.0 * M_PI * 440.0 / (double)mix->nSamplesPerSec;
            if (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                 ((WAVEFORMATEXTENSIBLE *)mix)->SubFormat.Data1 == 0x00000003)) {
                float *out = (float *)data;
                for (i = 0; i < available; ++i) {
                    float s = (float)sin(phase) * 0.18f;
                    UINT16 ch;
                    for (ch = 0; ch < mix->nChannels; ++ch)
                        out[i * mix->nChannels + ch] = s;
                    phase += inc;
                    if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
                }
            } else {
                /* Assume 16-bit PCM */
                short *out = (short *)data;
                for (i = 0; i < available; ++i) {
                    short s = (short)(sin(phase) * 6000.0);
                    UINT16 ch;
                    for (ch = 0; ch < mix->nChannels; ++ch)
                        out[i * mix->nChannels + ch] = s;
                    phase += inc;
                    if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
                }
            }
        }
        IAudioRenderClient_ReleaseBuffer(rc, available, 0);
    }

    IAudioClient_Stop(ac);

done:
    if (rc)  IAudioRenderClient_Release(rc);
    if (mix) CoTaskMemFree(mix);
    if (ac)  IAudioClient_Release(ac);
    if (dev) IMMDevice_Release(dev);
    if (en)  IMMDeviceEnumerator_Release(en);
    if (frameEvent) CloseHandle(frameEvent);
    CoUninitialize();
    return 0;
}

static void Wo_Start(WoState *st)
{
    DWORD tid;
    if (st->workerThread) return;
    ResetEvent(st->stopEvent);
    st->workerThread = CreateThread(NULL, 0, Wo_Worker, st, 0, &tid);
    if (st->workerThread) {
        Wo_SetStatus(st, L"Playing through default render device...");
        EnableWindow(st->playBtn, FALSE);
        EnableWindow(st->stopBtn, TRUE);
    } else {
        Wo_SetStatus(st, L"Failed to start worker.");
    }
}

static void Wo_Stop(WoState *st)
{
    if (!st->workerThread) return;
    SetEvent(st->stopEvent);
    WaitForSingleObject(st->workerThread, 2000);
    CloseHandle(st->workerThread);
    st->workerThread = NULL;
    Wo_SetStatus(st, L"Stopped.");
    EnableWindow(st->playBtn, TRUE);
    EnableWindow(st->stopBtn, FALSE);
}

static LRESULT CALLBACK Wo_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    WoState *st = (WoState *)GetPropW(hwnd, WO_PROP);

    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_WO_PLAY) { Wo_Start(st); return 0; }
        if (LOWORD(wp) == ID_WO_STOP) { Wo_Stop(st);  return 0; }
    }
    if (msg == WM_DESTROY && st) {
        Wo_Stop(st);
        if (st->stopEvent) CloseHandle(st->stopEvent);
        free(st);
        RemovePropW(hwnd, WO_PROP);
    }
    return CallWindowProcW(g_origWoFrame, hwnd, msg, wp, lp);
}

static HWND WasapiOut_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    WoState *st;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"WasapiOut",
        WS_POPUP | WS_BORDER,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (WoState *)calloc(1, sizeof(WoState));
    if (!st) { DestroyWindow(frame); return NULL; }
    st->stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    CreateWindowExW(0, L"STATIC",
        L"Continuous 440 Hz sine via WASAPI shared mode.\n"
        L"Uses the device's native mix format (float or PCM).",
        WS_CHILD | WS_VISIBLE,
        12, 40, w - 24, 40, frame, NULL, hInstance, NULL);

    st->playBtn = CreateWindowExW(0, L"BUTTON", L"Play",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 90, 100, 30, frame, (HMENU)(LONG_PTR)ID_WO_PLAY, hInstance, NULL);
    st->stopBtn = CreateWindowExW(0, L"BUTTON", L"Stop",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
        124, 90, 100, 30, frame, (HMENU)(LONG_PTR)ID_WO_STOP, hInstance, NULL);
    st->status = CreateWindowExW(0, L"STATIC", L"Idle.",
        WS_CHILD | WS_VISIBLE,
        12, 132, w - 24, 22, frame, (HMENU)(LONG_PTR)ID_WO_STAT, hInstance, NULL);

    SetPropW(frame, WO_PROP, (HANDLE)st);
    if (!g_origWoFrame) g_origWoFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Wo_FrameProc);
    return frame;
}

MsApp g_AppWasapiOut = {
    L"WasapiOut",
    WasapiOut_Create,
    360, 200
};
