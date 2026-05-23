/*
 * app_mediafndr.c — Media Foundation source-reader introspection
 *
 * Demonstrates Media Foundation, the modern (Win7+) media pipeline that
 * replaced DirectShow for most use cases. We use IMFSourceReader, the
 * synchronous high-level facade, to introspect a media file without
 * actually playing it:
 *
 *   - MFStartup(MF_VERSION) — initialize the platform (matched by MFShutdown)
 *   - MFCreateSourceReaderFromURL(url, NULL, &reader) — open a file and
 *     decode-graph it
 *   - IMFSourceReader::GetStreamSelection(streamIndex, &selected)
 *     IMFSourceReader::GetNativeMediaType(streamIndex, 0, &mediaType)
 *     IMFSourceReader::GetCurrentMediaType(streamIndex, &mediaType)
 *   - IMFMediaType::GetMajorType (audio/video/text) and GetGUID(MF_MT_SUBTYPE)
 *   - IMFMediaType::GetUINT32 for MF_MT_AUDIO_NUM_CHANNELS, etc.
 *   - IMFSourceReader::GetPresentationAttribute with MF_PD_DURATION
 *
 * This intentionally does NOT play audio — Batch 5's WasapiOut already
 * covers that path. What's interesting here is the Media Foundation graph
 * and metadata access, distinct from any earlier batch.
 */

#define COBJMACROS
#define CINTERFACE
#define INITGUID

#include "shell.h"
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propvarutil.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "ole32.lib")

#define MF_PROP    L"MS_MF_STATE"
#define ID_MF_PATH 83001
#define ID_MF_BR   83002
#define ID_MF_GO   83003
#define ID_MF_OUT  83004

typedef struct { HWND pathEdit, browseBtn, goBtn, output; BOOL mfOk, comOk; } MfState;
static WNDPROC g_origMfFrame = NULL;

static void Mf_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
}

static const wchar_t *Mf_NameForSubtype(const GUID *g)
{
    static const struct { GUID guid; const wchar_t *name; } known[] = {
        { { 0x00000001, 0,0,{0,0,0,0,0,0x10,0,0x80,0,0,0xAA,0,0x38,0x9B,0x71} }, L"PCM" },
        { { 0x00000003, 0,0,{0,0,0,0,0,0x10,0,0x80,0,0,0xAA,0,0x38,0x9B,0x71} }, L"IEEE-Float" },
        { { 0x00000055, 0,0,{0,0,0,0,0,0x10,0,0x80,0,0,0xAA,0,0x38,0x9B,0x71} }, L"MP3" },
        { { 0x00001610, 0,0,{0,0,0,0,0,0x10,0,0x80,0,0,0xAA,0,0x38,0x9B,0x71} }, L"AAC" },
        { { 0x00000160, 0,0,{0,0,0,0,0,0x10,0,0x80,0,0,0xAA,0,0x38,0x9B,0x71} }, L"WMA" },
    };
    int i;
    for (i = 0; i < (int)(sizeof(known)/sizeof(known[0])); ++i)
        if (IsEqualGUID(g, &known[i].guid)) return known[i].name;
    return NULL;
}

static void Mf_DumpStream(MfState *st, IMFSourceReader *reader, DWORD streamIndex)
{
    IMFMediaType *mt = NULL;
    GUID major, subtype;
    BOOL selected = FALSE;
    HRESULT hr;
    wchar_t line[600];
    UINT32 channels = 0, sampleRate = 0, bitsPerSample = 0, bitRate = 0;
    UINT32 width = 0, height = 0;
    const wchar_t *subName;
    wchar_t guidBuf[80];

    IMFSourceReader_GetStreamSelection(reader, streamIndex, &selected);

    hr = IMFSourceReader_GetNativeMediaType(reader, streamIndex, 0, &mt);
    if (FAILED(hr) || !mt) {
        swprintf_s(line, 600, L"\r\nStream %lu: <no media type>\r\n", streamIndex);
        Mf_Append(st->output, line);
        return;
    }
    IMFMediaType_GetMajorType(mt, &major);
    IMFMediaType_GetGUID(mt, &MF_MT_SUBTYPE, &subtype);

    StringFromGUID2(&subtype, guidBuf, 80);
    subName = Mf_NameForSubtype(&subtype);
    swprintf_s(line, 600,
        L"\r\nStream %lu  %s  selected=%s\r\n"
        L"   subtype: %s  %s\r\n",
        streamIndex,
        IsEqualGUID(&major, &MFMediaType_Audio) ? L"AUDIO" :
        IsEqualGUID(&major, &MFMediaType_Video) ? L"VIDEO" : L"OTHER",
        selected ? L"yes" : L"no",
        guidBuf, subName ? subName : L"(unknown)");
    Mf_Append(st->output, line);

    if (IsEqualGUID(&major, &MFMediaType_Audio)) {
        IMFMediaType_GetUINT32(mt, &MF_MT_AUDIO_NUM_CHANNELS,     &channels);
        IMFMediaType_GetUINT32(mt, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate);
        IMFMediaType_GetUINT32(mt, &MF_MT_AUDIO_BITS_PER_SAMPLE,  &bitsPerSample);
        IMFMediaType_GetUINT32(mt, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bitRate);
        swprintf_s(line, 600,
            L"   channels=%u  rate=%u Hz  bits=%u  avgBytes/s=%u (%u kbps)\r\n",
            channels, sampleRate, bitsPerSample, bitRate, (bitRate * 8) / 1000);
        Mf_Append(st->output, line);
    } else if (IsEqualGUID(&major, &MFMediaType_Video)) {
        UINT64 frameSize = 0;
        IMFMediaType_GetUINT64(mt, &MF_MT_FRAME_SIZE, &frameSize);
        width  = (UINT32)(frameSize >> 32);
        height = (UINT32)(frameSize & 0xFFFFFFFF);
        swprintf_s(line, 600, L"   size: %u x %u\r\n", width, height);
        Mf_Append(st->output, line);
    }

    IMFMediaType_Release(mt);
}

static void Mf_OpenAndDump(MfState *st, const wchar_t *url)
{
    IMFSourceReader *reader = NULL;
    HRESULT hr;
    DWORD i;
    PROPVARIANT pv;
    wchar_t line[400];

    SetWindowTextW(st->output, L"");
    swprintf_s(line, 400, L"Opening: %s\r\n", url);
    Mf_Append(st->output, line);

    hr = MFCreateSourceReaderFromURL(url, NULL, &reader);
    if (FAILED(hr)) {
        swprintf_s(line, 400, L"MFCreateSourceReaderFromURL failed (hr=0x%08lx).\r\n", hr);
        Mf_Append(st->output, line);
        return;
    }

    PropVariantInit(&pv);
    hr = IMFSourceReader_GetPresentationAttribute(reader,
            MF_SOURCE_READER_MEDIASOURCE, &MF_PD_DURATION, &pv);
    if (SUCCEEDED(hr) && pv.vt == VT_UI8) {
        ULONGLONG hundredNs = pv.uhVal.QuadPart;
        double seconds = hundredNs / 10000000.0;
        swprintf_s(line, 400, L"Duration: %.2f seconds\r\n", seconds);
        Mf_Append(st->output, line);
    }
    PropVariantClear(&pv);

    /* Walk streams until GetNativeMediaType fails */
    for (i = 0; ; ++i) {
        IMFMediaType *probe = NULL;
        hr = IMFSourceReader_GetNativeMediaType(reader, i, 0, &probe);
        if (FAILED(hr)) break;
        if (probe) IMFMediaType_Release(probe);
        Mf_DumpStream(st, reader, i);
    }

    IMFSourceReader_Release(reader);
    Mf_Append(st->output, L"\r\nDone.\r\n");
}

static void Mf_BrowseAndOpen(MfState *st)
{
    OPENFILENAMEW ofn;
    wchar_t fn[MAX_PATH] = L"";
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetParent(st->pathEdit);
    ofn.lpstrFile = fn;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter =
        L"Media files\0*.wav;*.mp3;*.wma;*.aac;*.m4a;*.mp4;*.wmv;*.mkv\0All files\0*.*\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) {
        SetWindowTextW(st->pathEdit, fn);
        Mf_OpenAndDump(st, fn);
    }
}

static LRESULT CALLBACK Mf_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    MfState *st = (MfState *)GetPropW(hwnd, MF_PROP);
    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_MF_GO) {
            wchar_t path[MAX_PATH];
            GetWindowTextW(st->pathEdit, path, MAX_PATH);
            if (path[0]) Mf_OpenAndDump(st, path);
            return 0;
        }
        if (LOWORD(wp) == ID_MF_BR) { Mf_BrowseAndOpen(st); return 0; }
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->pathEdit,   12, 38, w - 224, 24, TRUE);
        MoveWindow(st->browseBtn,  w - 208, 38, 96, 24, TRUE);
        MoveWindow(st->goBtn,      w - 108, 38, 90, 24, TRUE);
        MoveWindow(st->output,     8, 70, w - 16, h - 78, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->mfOk)  MFShutdown();
        if (st->comOk) CoUninitialize();
        free(st); RemovePropW(hwnd, MF_PROP);
    }
    return CallWindowProcW(g_origMfFrame, hwnd, msg, wp, lp);
}

static HWND MediaFndr_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    MfState *st;
    HFONT mono;
    HRESULT hr;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"MediaFndr",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (MfState *)calloc(1, sizeof(MfState));
    if (!st) { DestroyWindow(frame); return NULL; }

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    st->comOk = SUCCEEDED(hr);
    hr = MFStartup(MF_VERSION);
    st->mfOk = SUCCEEDED(hr);

    st->pathEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        12, 38, w - 224, 24, frame, (HMENU)(LONG_PTR)ID_MF_PATH, hInstance, NULL);
    st->browseBtn = CreateWindowExW(0, L"BUTTON", L"Browse...",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        w - 208, 38, 96, 24, frame, (HMENU)(LONG_PTR)ID_MF_BR, hInstance, NULL);
    st->goBtn = CreateWindowExW(0, L"BUTTON", L"Open",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        w - 108, 38, 90, 24, frame, (HMENU)(LONG_PTR)ID_MF_GO, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 70, w - 16, h - 78, frame, (HMENU)(LONG_PTR)ID_MF_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    if (!st->mfOk) Mf_Append(st->output, L"MFStartup failed.\r\n");
    else           Mf_Append(st->output, L"Ready. Choose a media file.\r\n");

    SetPropW(frame, MF_PROP, (HANDLE)st);
    if (!g_origMfFrame) g_origMfFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Mf_FrameProc);
    return frame;
}

MsApp g_AppMediaFndr = { L"MediaFndr", MediaFndr_Create, 720, 460 };
