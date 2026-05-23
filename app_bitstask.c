/*
 * app_bitstask.c — Background Intelligent Transfer Service download job
 *
 * Demonstrates BITS (Background Intelligent Transfer Service) — the same
 * service Windows Update uses for resumable, throttled downloads:
 *   - CoCreateInstance(CLSID_BackgroundCopyManager) → IBackgroundCopyManager
 *   - IBackgroundCopyManager::CreateJob(name, BG_JOB_TYPE_DOWNLOAD, &id, &job)
 *   - IBackgroundCopyJob::AddFile(remoteUrl, localPath)
 *   - IBackgroundCopyJob::Resume() activates the job in the queue
 *   - IBackgroundCopyJob::GetState reports BG_JOB_STATE_TRANSFERRING /
 *     _TRANSFERRED / _ERROR / etc.
 *   - IBackgroundCopyJob::GetProgress fills BG_JOB_PROGRESS with bytes
 *     transferred and file counts
 *   - IBackgroundCopyJob::Complete() finalizes a transferred download
 *
 * Unlike WinHTTP (Batch 5 HttpsGet), BITS persists across reboots,
 * throttles to available bandwidth, and resumes interrupted transfers.
 */

#define COBJMACROS
#define CINTERFACE
#define INITGUID

#include "shell.h"
#include <objbase.h>
#include <bits.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "ole32.lib")

#define BT_PROP    L"MS_BT_STATE"
#define ID_BT_URL  102001
#define ID_BT_GO   102002
#define ID_BT_STOP 102003
#define ID_BT_OUT  102004
#define BT_TIMER   1

typedef struct {
    HWND  urlEdit, goBtn, stopBtn, output;
    BOOL  comOk;
    IBackgroundCopyManager *mgr;
    IBackgroundCopyJob     *job;
    UINT_PTR timer;
} BtState;
static WNDPROC g_origBtFrame = NULL;

static void Bt_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
    SendMessageW(e, EM_SCROLLCARET, 0, 0);
}

static const wchar_t *Bt_StateName(BG_JOB_STATE s)
{
    switch (s) {
    case BG_JOB_STATE_QUEUED:        return L"queued";
    case BG_JOB_STATE_CONNECTING:    return L"connecting";
    case BG_JOB_STATE_TRANSFERRING:  return L"transferring";
    case BG_JOB_STATE_SUSPENDED:     return L"suspended";
    case BG_JOB_STATE_ERROR:         return L"error";
    case BG_JOB_STATE_TRANSIENT_ERROR: return L"transient error";
    case BG_JOB_STATE_TRANSFERRED:   return L"transferred";
    case BG_JOB_STATE_ACKNOWLEDGED:  return L"acknowledged";
    case BG_JOB_STATE_CANCELLED:     return L"cancelled";
    }
    return L"?";
}

static void Bt_Cleanup(BtState *st)
{
    if (st->timer) { KillTimer(GetParent(st->urlEdit), st->timer); st->timer = 0; }
    if (st->job) {
        IBackgroundCopyJob_Cancel(st->job);
        IBackgroundCopyJob_Release(st->job);
        st->job = NULL;
    }
}

static void Bt_Poll(HWND frame, BtState *st)
{
    BG_JOB_STATE state;
    BG_JOB_PROGRESS prog;
    wchar_t line[300];
    HRESULT hr;

    if (!st->job) return;
    hr = IBackgroundCopyJob_GetState(st->job, &state);
    if (FAILED(hr)) return;
    hr = IBackgroundCopyJob_GetProgress(st->job, &prog);
    if (FAILED(hr)) return;

    swprintf_s(line, 300,
        L"  state=%-14s  bytes=%llu/%llu  files=%lu/%lu\r\n",
        Bt_StateName(state),
        prog.BytesTransferred, prog.BytesTotal,
        prog.FilesTransferred, prog.FilesTotal);
    Bt_Append(st->output, line);

    if (state == BG_JOB_STATE_TRANSFERRED) {
        IBackgroundCopyJob_Complete(st->job);
        Bt_Append(st->output, L"  -> Complete() called. Job finalized.\r\n");
        Bt_Cleanup(st);
    } else if (state == BG_JOB_STATE_ERROR) {
        Bt_Append(st->output, L"  -> ERROR. Cancelling.\r\n");
        Bt_Cleanup(st);
    }
}

static void Bt_Start(HWND frame, BtState *st)
{
    wchar_t url[1024];
    wchar_t localPath[MAX_PATH];
    GUID    jobId;
    HRESULT hr;

    if (st->job) {
        Bt_Append(st->output, L"Job already running — Stop first.\r\n");
        return;
    }
    GetWindowTextW(st->urlEdit, url, 1024);
    if (!url[0]) return;

    /* Build a local destination path under %TEMP% */
    ExpandEnvironmentStringsW(L"%TEMP%\\minishell_bits_download.bin", localPath, MAX_PATH);
    DeleteFileW(localPath);  /* BITS will refuse an existing destination */

    if (!st->mgr) {
        hr = CoCreateInstance(&CLSID_BackgroundCopyManager, NULL,
                CLSCTX_LOCAL_SERVER, &IID_IBackgroundCopyManager,
                (void **)&st->mgr);
        if (FAILED(hr) || !st->mgr) {
            wchar_t buf[80];
            swprintf_s(buf, 80, L"CoCreateInstance(BITS) failed: 0x%08lx\r\n", hr);
            Bt_Append(st->output, buf);
            return;
        }
    }

    hr = IBackgroundCopyManager_CreateJob(st->mgr, L"MiniShell BITS Demo",
            BG_JOB_TYPE_DOWNLOAD, &jobId, &st->job);
    if (FAILED(hr) || !st->job) {
        wchar_t buf[80];
        swprintf_s(buf, 80, L"CreateJob failed: 0x%08lx\r\n", hr);
        Bt_Append(st->output, buf);
        return;
    }

    hr = IBackgroundCopyJob_AddFile(st->job, url, localPath);
    if (FAILED(hr)) {
        wchar_t buf[80];
        swprintf_s(buf, 80, L"AddFile failed: 0x%08lx\r\n", hr);
        Bt_Append(st->output, buf);
        Bt_Cleanup(st);
        return;
    }
    {
        wchar_t line[MAX_PATH + 200];
        swprintf_s(line, MAX_PATH + 200,
            L"Created job. Source:\r\n  %s\r\nDest:\r\n  %s\r\n", url, localPath);
        Bt_Append(st->output, line);
    }

    hr = IBackgroundCopyJob_Resume(st->job);
    if (FAILED(hr)) {
        wchar_t buf[80];
        swprintf_s(buf, 80, L"Resume failed: 0x%08lx\r\n", hr);
        Bt_Append(st->output, buf);
        Bt_Cleanup(st);
        return;
    }

    if (!st->timer) st->timer = SetTimer(frame, BT_TIMER, 500, NULL);
}

static LRESULT CALLBACK Bt_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    BtState *st = (BtState *)GetPropW(hwnd, BT_PROP);
    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_BT_GO)   { Bt_Start(hwnd, st); return 0; }
        if (LOWORD(wp) == ID_BT_STOP) { Bt_Cleanup(st); Bt_Append(st->output, L"[cancelled]\r\n"); return 0; }
    }
    if (msg == WM_TIMER && st) { Bt_Poll(hwnd, st); return 0; }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->urlEdit, 12, 38, w - 224, 24, TRUE);
        MoveWindow(st->goBtn,   w - 208, 38, 96, 24, TRUE);
        MoveWindow(st->stopBtn, w - 108, 38, 90, 24, TRUE);
        MoveWindow(st->output,  8, 70, w - 16, h - 78, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        Bt_Cleanup(st);
        if (st->mgr) IBackgroundCopyManager_Release(st->mgr);
        if (st->comOk) CoUninitialize();
        free(st); RemovePropW(hwnd, BT_PROP);
    }
    return CallWindowProcW(g_origBtFrame, hwnd, msg, wp, lp);
}

static HWND BitsTask_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    BtState *st;
    HFONT mono;
    HRESULT hr;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"BitsTask",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (BtState *)calloc(1, sizeof(BtState));
    if (!st) { DestroyWindow(frame); return NULL; }

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    st->comOk = SUCCEEDED(hr);

    st->urlEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"https://aka.ms/Win32-CMD-NotePad-Demo",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        12, 38, w - 224, 24, frame, (HMENU)(LONG_PTR)ID_BT_URL, hInstance, NULL);
    st->goBtn = CreateWindowExW(0, L"BUTTON", L"Download",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        w - 208, 38, 96, 24, frame, (HMENU)(LONG_PTR)ID_BT_GO, hInstance, NULL);
    st->stopBtn = CreateWindowExW(0, L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        w - 108, 38, 90, 24, frame, (HMENU)(LONG_PTR)ID_BT_STOP, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"BITS download demo. Saves to %TEMP%\\minishell_bits_download.bin.\r\n",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 70, w - 16, h - 78, frame, (HMENU)(LONG_PTR)ID_BT_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, BT_PROP, (HANDLE)st);
    if (!g_origBtFrame) g_origBtFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Bt_FrameProc);
    return frame;
}

MsApp g_AppBitsTask = { L"BitsTask", BitsTask_Create, 760, 420 };
