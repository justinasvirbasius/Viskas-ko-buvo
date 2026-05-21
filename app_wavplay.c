/*
 * app_wavplay.c — Play a WAV via PlaySoundW (with synthesized samples)
 *
 * Demonstrates:
 *   - PlaySoundW with SND_MEMORY | SND_ASYNC | SND_LOOP
 *   - A minimal RIFF/WAVE in-memory buffer constructed at runtime so the app
 *     has nothing on disk to depend on. Generates a 0.5-second 440 Hz sine
 *     tone at 22050 Hz, 16-bit PCM, mono.
 *
 * Two buttons: Play and Stop. Loop toggle restarts the sound continuously.
 */

#include "shell.h"
#include <math.h>
#include <stdlib.h>

#pragma comment(lib, "winmm.lib")

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define WAV_PROP    L"MS_WAV_STATE"
#define ID_WAV_PLAY 26001
#define ID_WAV_STOP 26002
#define ID_WAV_LOOP 26003

#pragma pack(push, 1)
typedef struct {
    char     riff[4];
    DWORD    fileSize;
    char     wave[4];
    char     fmt_[4];
    DWORD    fmtSize;
    WORD     formatTag;
    WORD     channels;
    DWORD    sampleRate;
    DWORD    byteRate;
    WORD     blockAlign;
    WORD     bitsPerSample;
    char     data[4];
    DWORD    dataSize;
} WavHeader;
#pragma pack(pop)

typedef struct {
    BYTE *buf;
    DWORD size;
    BOOL  loop;
    HWND  loopBtn;
} WavState;

static WNDPROC g_origWavFrame = NULL;

static BYTE *Wav_BuildSine(double seconds, double freqHz, DWORD *outSize)
{
    const DWORD sampleRate = 22050;
    DWORD samples = (DWORD)(sampleRate * seconds);
    DWORD pcmBytes = samples * 2;
    DWORD total = sizeof(WavHeader) + pcmBytes;
    BYTE *buf;
    WavHeader *h;
    short *pcm;
    DWORD i;

    buf = (BYTE *)malloc(total);
    if (!buf) return NULL;
    h = (WavHeader *)buf;

    memcpy(h->riff, "RIFF", 4);
    h->fileSize = total - 8;
    memcpy(h->wave, "WAVE", 4);
    memcpy(h->fmt_, "fmt ", 4);
    h->fmtSize       = 16;
    h->formatTag     = 1;   /* PCM */
    h->channels      = 1;
    h->sampleRate    = sampleRate;
    h->bitsPerSample = 16;
    h->blockAlign    = (WORD)(h->channels * h->bitsPerSample / 8);
    h->byteRate      = h->sampleRate * h->blockAlign;
    memcpy(h->data, "data", 4);
    h->dataSize = pcmBytes;

    pcm = (short *)(buf + sizeof(WavHeader));
    for (i = 0; i < samples; ++i) {
        double t = (double)i / (double)sampleRate;
        double s = sin(2.0 * M_PI * freqHz * t) * 0.5;
        /* Apply a quick fade in/out so we don't click */
        double env = 1.0;
        DWORD fade = sampleRate / 50; /* 20 ms */
        if (i < fade) env = (double)i / (double)fade;
        else if (i > samples - fade) env = (double)(samples - i) / (double)fade;
        pcm[i] = (short)(s * env * 32767.0);
    }
    *outSize = total;
    return buf;
}

static LRESULT CALLBACK Wav_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    WavState *st = (WavState *)GetPropW(hwnd, WAV_PROP);

    if (msg == WM_COMMAND && st) {
        switch (LOWORD(wp)) {
        case ID_WAV_PLAY: {
            DWORD flags = SND_MEMORY | SND_ASYNC;
            if (st->loop) flags |= SND_LOOP;
            PlaySoundW((LPCWSTR)st->buf, NULL, flags);
            return 0;
        }
        case ID_WAV_STOP:
            PlaySoundW(NULL, NULL, 0);
            return 0;
        case ID_WAV_LOOP:
            st->loop = !st->loop;
            SetWindowTextW(st->loopBtn,
                           st->loop ? L"Loop: ON" : L"Loop: OFF");
            return 0;
        }
    }
    if (msg == WM_DESTROY && st) {
        PlaySoundW(NULL, NULL, 0);
        if (st->buf) free(st->buf);
        free(st);
        RemovePropW(hwnd, WAV_PROP);
    }
    return CallWindowProcW(g_origWavFrame, hwnd, msg, wp, lp);
}

static HWND WavPlay_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    WavState *st;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"WavPlay",
        WS_POPUP | WS_BORDER,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (WavState *)calloc(1, sizeof(WavState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->buf = Wav_BuildSine(0.5, 440.0, &st->size);
    if (!st->buf) { DestroyWindow(frame); free(st); return NULL; }

    CreateWindowExW(0, L"STATIC",
        L"In-memory WAV (440 Hz, 0.5 s, 22 050 Hz mono).",
        WS_CHILD | WS_VISIBLE,
        12, 40, w - 24, 18, frame, NULL, hInstance, NULL);

    CreateWindowExW(0, L"BUTTON", L"Play",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 70, 90, 28, frame, (HMENU)(LONG_PTR)ID_WAV_PLAY, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Stop",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        112, 70, 90, 28, frame, (HMENU)(LONG_PTR)ID_WAV_STOP, hInstance, NULL);
    st->loopBtn = CreateWindowExW(0, L"BUTTON", L"Loop: OFF",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        212, 70, 90, 28, frame, (HMENU)(LONG_PTR)ID_WAV_LOOP, hInstance, NULL);

    SetPropW(frame, WAV_PROP, (HANDLE)st);
    if (!g_origWavFrame) g_origWavFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Wav_FrameProc);
    return frame;
}

MsApp g_AppWavPlay = {
    L"WavPlay",
    WavPlay_Create,
    340, 140
};
