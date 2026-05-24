/*
 * app_xaudio2.c — Synthesized sine-wave tone via XAudio2
 *
 * Demonstrates the IXAudio2 audio engine — Microsoft's modern audio
 * pipeline that replaced DirectSound. Distinct from WasapiOut (Batch 7,
 * lower-level WASAPI client) and WavPlay (Batch 5, PlaySound). XAudio2
 * is what most Win32 games use today.
 *
 *   - XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR) instantiates the
 *     engine (xaudio2_9.dll on Win 10+, xaudio2_8.dll on Win 8)
 *   - IXAudio2::CreateMasteringVoice(&master) — the bridge to the
 *     audio endpoint device
 *   - IXAudio2::CreateSourceVoice(&src, &waveFmt, 0, MAX_RATIO, NULL,
 *     NULL, NULL) — one input into the mix graph
 *   - IXAudio2SourceVoice::SubmitSourceBuffer(&buf) enqueues PCM data;
 *     XAUDIO2_BUFFER has the byte pointer/length, loop count
 *   - IXAudio2SourceVoice::Start(0, 0) begins playback
 *
 * Loaded dynamically — the XAudio2 DLL name varies by OS version. We
 * synthesize one second of a 440 Hz sine wave at 48 kHz 16-bit mono.
 */

#define COBJMACROS
#define CINTERFACE
#define INITGUID

#include "shell.h"
#include <mmreg.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

/* Minimal XAudio2 declarations — avoid header version conflicts */
#ifndef XAUDIO2_DEFAULT_PROCESSOR
#define XAUDIO2_DEFAULT_PROCESSOR 0x00000001
#endif
#ifndef XAUDIO2_DEFAULT_CHANNELS
#define XAUDIO2_DEFAULT_CHANNELS 0
#endif
#ifndef XAUDIO2_DEFAULT_SAMPLERATE
#define XAUDIO2_DEFAULT_SAMPLERATE 0
#endif

typedef UINT32 XAUDIO2_PROCESSOR;

typedef struct IXAudio2 IXAudio2;
typedef struct IXAudio2Voice IXAudio2Voice;
typedef struct IXAudio2MasteringVoice IXAudio2MasteringVoice;
typedef struct IXAudio2SourceVoice IXAudio2SourceVoice;
typedef struct IXAudio2EngineCallback IXAudio2EngineCallback;
typedef struct IXAudio2VoiceCallback IXAudio2VoiceCallback;
typedef struct IXAudio2VoiceSends IXAudio2VoiceSends;
typedef struct XAUDIO2_EFFECT_CHAIN XAUDIO2_EFFECT_CHAIN;

typedef struct XAUDIO2_BUFFER {
    UINT32  Flags;
    UINT32  AudioBytes;
    const BYTE *pAudioData;
    UINT32  PlayBegin;
    UINT32  PlayLength;
    UINT32  LoopBegin;
    UINT32  LoopLength;
    UINT32  LoopCount;
    void   *pContext;
} XAUDIO2_BUFFER;

typedef HRESULT (WINAPI *PFN_XAudio2Create)(IXAudio2 **, UINT32, XAUDIO2_PROCESSOR);

/* IXAudio2 vtable (just the methods we use) */
typedef struct IXAudio2Vtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IXAudio2 *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(IXAudio2 *);
    ULONG (STDMETHODCALLTYPE *Release)(IXAudio2 *);
    HRESULT (STDMETHODCALLTYPE *RegisterForCallbacks)(IXAudio2 *, IXAudio2EngineCallback *);
    void (STDMETHODCALLTYPE *UnregisterForCallbacks)(IXAudio2 *, IXAudio2EngineCallback *);
    HRESULT (STDMETHODCALLTYPE *CreateSourceVoice)(IXAudio2 *, IXAudio2SourceVoice **,
        const WAVEFORMATEX *, UINT32, float, IXAudio2VoiceCallback *,
        const IXAudio2VoiceSends *, const XAUDIO2_EFFECT_CHAIN *);
    HRESULT (STDMETHODCALLTYPE *CreateSubmixVoice)(IXAudio2 *, void **,
        UINT32, UINT32, UINT32, UINT32, const IXAudio2VoiceSends *, const XAUDIO2_EFFECT_CHAIN *);
    HRESULT (STDMETHODCALLTYPE *CreateMasteringVoice)(IXAudio2 *, IXAudio2MasteringVoice **,
        UINT32, UINT32, UINT32, LPCWSTR, const XAUDIO2_EFFECT_CHAIN *, int);
    HRESULT (STDMETHODCALLTYPE *StartEngine)(IXAudio2 *);
    void (STDMETHODCALLTYPE *StopEngine)(IXAudio2 *);
} IXAudio2Vtbl;

struct IXAudio2 { const IXAudio2Vtbl *lpVtbl; };

#define IXAudio2_Release(p)              ((p)->lpVtbl->Release(p))
#define IXAudio2_CreateMasteringVoice(p,m) ((p)->lpVtbl->CreateMasteringVoice((p),(m),0,0,0,NULL,NULL,0))
#define IXAudio2_CreateSourceVoice(p,s,f) ((p)->lpVtbl->CreateSourceVoice((p),(s),(f),0,2.0f,NULL,NULL,NULL))

/* IXAudio2SourceVoice vtable subset */
typedef struct IXAudio2SourceVoiceVtbl {
    /* IXAudio2Voice methods */
    void    (STDMETHODCALLTYPE *GetVoiceDetails)(IXAudio2SourceVoice *, void *);
    HRESULT (STDMETHODCALLTYPE *SetOutputVoices)(IXAudio2SourceVoice *, const void *);
    HRESULT (STDMETHODCALLTYPE *SetEffectChain)(IXAudio2SourceVoice *, const void *);
    HRESULT (STDMETHODCALLTYPE *EnableEffect)(IXAudio2SourceVoice *, UINT32, UINT32);
    HRESULT (STDMETHODCALLTYPE *DisableEffect)(IXAudio2SourceVoice *, UINT32, UINT32);
    void    (STDMETHODCALLTYPE *GetEffectState)(IXAudio2SourceVoice *, UINT32, BOOL *);
    HRESULT (STDMETHODCALLTYPE *SetEffectParameters)(IXAudio2SourceVoice *, UINT32, const void *, UINT32, UINT32);
    HRESULT (STDMETHODCALLTYPE *GetEffectParameters)(IXAudio2SourceVoice *, UINT32, void *, UINT32);
    HRESULT (STDMETHODCALLTYPE *SetFilterParameters)(IXAudio2SourceVoice *, const void *, UINT32);
    void    (STDMETHODCALLTYPE *GetFilterParameters)(IXAudio2SourceVoice *, void *);
    HRESULT (STDMETHODCALLTYPE *SetOutputFilterParameters)(IXAudio2SourceVoice *, void *, const void *, UINT32);
    void    (STDMETHODCALLTYPE *GetOutputFilterParameters)(IXAudio2SourceVoice *, void *, void *);
    HRESULT (STDMETHODCALLTYPE *SetVolume)(IXAudio2SourceVoice *, float, UINT32);
    void    (STDMETHODCALLTYPE *GetVolume)(IXAudio2SourceVoice *, float *);
    HRESULT (STDMETHODCALLTYPE *SetChannelVolumes)(IXAudio2SourceVoice *, UINT32, const float *, UINT32);
    void    (STDMETHODCALLTYPE *GetChannelVolumes)(IXAudio2SourceVoice *, UINT32, float *);
    HRESULT (STDMETHODCALLTYPE *SetOutputMatrix)(IXAudio2SourceVoice *, void *, UINT32, UINT32, const float *, UINT32);
    void    (STDMETHODCALLTYPE *GetOutputMatrix)(IXAudio2SourceVoice *, void *, UINT32, UINT32, float *);
    void    (STDMETHODCALLTYPE *DestroyVoice)(IXAudio2SourceVoice *);
    /* SourceVoice-specific */
    HRESULT (STDMETHODCALLTYPE *Start)(IXAudio2SourceVoice *, UINT32, UINT32);
    HRESULT (STDMETHODCALLTYPE *Stop)(IXAudio2SourceVoice *, UINT32, UINT32);
    HRESULT (STDMETHODCALLTYPE *SubmitSourceBuffer)(IXAudio2SourceVoice *, const XAUDIO2_BUFFER *, const void *);
    HRESULT (STDMETHODCALLTYPE *FlushSourceBuffers)(IXAudio2SourceVoice *);
    HRESULT (STDMETHODCALLTYPE *Discontinuity)(IXAudio2SourceVoice *);
    HRESULT (STDMETHODCALLTYPE *ExitLoop)(IXAudio2SourceVoice *, UINT32);
    void    (STDMETHODCALLTYPE *GetState)(IXAudio2SourceVoice *, void *, UINT32);
    HRESULT (STDMETHODCALLTYPE *SetFrequencyRatio)(IXAudio2SourceVoice *, float, UINT32);
    void    (STDMETHODCALLTYPE *GetFrequencyRatio)(IXAudio2SourceVoice *, float *);
    HRESULT (STDMETHODCALLTYPE *SetSourceSampleRate)(IXAudio2SourceVoice *, UINT32);
} IXAudio2SourceVoiceVtbl;
struct IXAudio2SourceVoice { const IXAudio2SourceVoiceVtbl *lpVtbl; };

#define IXAudio2SourceVoice_Start(p)              ((p)->lpVtbl->Start((p),0,0))
#define IXAudio2SourceVoice_SubmitSourceBuffer(p,b) ((p)->lpVtbl->SubmitSourceBuffer((p),(b),NULL))
#define IXAudio2SourceVoice_DestroyVoice(p)       ((p)->lpVtbl->DestroyVoice((p)))

typedef struct IXAudio2MasteringVoiceVtbl {
    void (STDMETHODCALLTYPE *_pad[19])(void);
    void (STDMETHODCALLTYPE *DestroyVoice)(IXAudio2MasteringVoice *);
} IXAudio2MasteringVoiceVtbl;
struct IXAudio2MasteringVoice { const IXAudio2MasteringVoiceVtbl *lpVtbl; };
#define IXAudio2MasteringVoice_DestroyVoice(p) ((p)->lpVtbl->DestroyVoice((p)))

#define XA_PROP   L"MS_XA_STATE"
#define ID_XA_GO  122001
#define ID_XA_OUT 122002

typedef struct {
    HWND   output;
    HMODULE dll;
    PFN_XAudio2Create pCreate;
    IXAudio2 *engine;
    IXAudio2MasteringVoice *master;
    IXAudio2SourceVoice    *src;
    BYTE   *buffer;
} XaState;

static WNDPROC g_origXaFrame = NULL;

static void Xa_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
}

static void Xa_PlayTone(XaState *st)
{
    HRESULT hr;
    WAVEFORMATEX fmt;
    const UINT32 sampleRate = 48000;
    const UINT32 lenSeconds = 1;
    const UINT32 sampleCount = sampleRate * lenSeconds;
    UINT32 i;
    XAUDIO2_BUFFER buf;

    SetWindowTextW(st->output, L"");
    if (!st->pCreate) {
        Xa_Append(st->output,
            L"XAudio2 not available — could not load xaudio2_9.dll or xaudio2_8.dll.\r\n");
        return;
    }

    if (!st->engine) {
        hr = st->pCreate(&st->engine, 0, XAUDIO2_DEFAULT_PROCESSOR);
        if (FAILED(hr) || !st->engine) {
            wchar_t line[80];
            swprintf_s(line, 80, L"XAudio2Create failed: 0x%08lx\r\n", hr);
            Xa_Append(st->output, line);
            return;
        }
        Xa_Append(st->output, L"XAudio2 engine created.\r\n");

        hr = IXAudio2_CreateMasteringVoice(st->engine, &st->master);
        if (FAILED(hr)) {
            wchar_t line[80];
            swprintf_s(line, 80, L"CreateMasteringVoice failed: 0x%08lx\r\n", hr);
            Xa_Append(st->output, line);
            return;
        }
        Xa_Append(st->output, L"Mastering voice created.\r\n");
    }

    /* Build PCM 16-bit mono format */
    ZeroMemory(&fmt, sizeof(fmt));
    fmt.wFormatTag      = WAVE_FORMAT_PCM;
    fmt.nChannels       = 1;
    fmt.nSamplesPerSec  = sampleRate;
    fmt.wBitsPerSample  = 16;
    fmt.nBlockAlign     = fmt.nChannels * fmt.wBitsPerSample / 8;
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    fmt.cbSize          = 0;

    /* Synthesize a 440 Hz sine */
    if (st->buffer) { free(st->buffer); st->buffer = NULL; }
    st->buffer = (BYTE *)malloc(sampleCount * sizeof(INT16));
    if (!st->buffer) { Xa_Append(st->output, L"malloc failed.\r\n"); return; }
    {
        INT16 *s = (INT16 *)st->buffer;
        for (i = 0; i < sampleCount; ++i) {
            double t = (double)i / sampleRate;
            /* Fade-in / fade-out envelope so the tone doesn't click */
            double env = 1.0;
            if (i < 1000) env = i / 1000.0;
            else if (i > sampleCount - 1000) env = (sampleCount - i) / 1000.0;
            s[i] = (INT16)(sin(t * 440.0 * 2.0 * 3.14159265358979) * 16000.0 * env);
        }
    }

    /* Always create a fresh source voice; destroy the previous one if any. */
    if (st->src) {
        IXAudio2SourceVoice_DestroyVoice(st->src);
        st->src = NULL;
    }
    hr = IXAudio2_CreateSourceVoice(st->engine, &st->src, &fmt);
    if (FAILED(hr) || !st->src) {
        wchar_t line[80];
        swprintf_s(line, 80, L"CreateSourceVoice failed: 0x%08lx\r\n", hr);
        Xa_Append(st->output, line);
        return;
    }

    ZeroMemory(&buf, sizeof(buf));
    buf.AudioBytes = sampleCount * sizeof(INT16);
    buf.pAudioData = st->buffer;
    buf.Flags      = 0x0040;  /* XAUDIO2_END_OF_STREAM */
    hr = IXAudio2SourceVoice_SubmitSourceBuffer(st->src, &buf);
    if (FAILED(hr)) {
        wchar_t line[80];
        swprintf_s(line, 80, L"SubmitSourceBuffer failed: 0x%08lx\r\n", hr);
        Xa_Append(st->output, line);
        return;
    }
    IXAudio2SourceVoice_Start(st->src);
    Xa_Append(st->output, L"Playing 1 second of 440 Hz sine.\r\n");
}

static LRESULT CALLBACK Xa_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    XaState *st = (XaState *)GetPropW(hwnd, XA_PROP);
    if (msg == WM_COMMAND && st && LOWORD(wp) == ID_XA_GO) { Xa_PlayTone(st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->src)    IXAudio2SourceVoice_DestroyVoice(st->src);
        if (st->master) IXAudio2MasteringVoice_DestroyVoice(st->master);
        if (st->engine) IXAudio2_Release(st->engine);
        if (st->buffer) free(st->buffer);
        if (st->dll)    FreeLibrary(st->dll);
        free(st); RemovePropW(hwnd, XA_PROP);
    }
    return CallWindowProcW(g_origXaFrame, hwnd, msg, wp, lp);
}

static HWND XAudio2_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    XaState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"XAudio2",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (XaState *)calloc(1, sizeof(XaState));
    if (!st) { DestroyWindow(frame); return NULL; }

    /* Try the most likely DLLs in order */
    st->dll = LoadLibraryW(L"xaudio2_9.dll");
    if (!st->dll) st->dll = LoadLibraryW(L"xaudio2_8.dll");
    if (!st->dll) st->dll = LoadLibraryW(L"xaudio2_7.dll");
    if (st->dll) {
        st->pCreate = (PFN_XAudio2Create)GetProcAddress(st->dll, "XAudio2Create");
    }

    CreateWindowExW(0, L"BUTTON", L"Play 440 Hz tone",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 160, 26, frame, (HMENU)(LONG_PTR)ID_XA_GO, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"Click to synthesize a 1-second 440 Hz sine wave and play it via\r\n"
        L"IXAudio2 source voice → mastering voice → audio endpoint.\r\n",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_XA_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, XA_PROP, (HANDLE)st);
    if (!g_origXaFrame) g_origXaFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Xa_FrameProc);
    return frame;
}

MsApp g_AppXAudio2 = { L"XAudio2", XAudio2_Create, 720, 380 };
