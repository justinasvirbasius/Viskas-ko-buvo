/*
 * app_dxgivbl.c — DXGI output enumeration and vertical-blank waits
 *
 * Demonstrates the DXGI 1.1 output object — the entry point for monitor
 * timing and refresh-rate-locked redraws:
 *   - CreateDXGIFactory1(&IID_IDXGIFactory1, &factory) entry
 *   - IDXGIFactory1::EnumAdapters1 walks GPU adapters
 *   - IDXGIAdapter1::EnumOutputs walks monitor outputs of each adapter
 *   - IDXGIOutput::GetDesc returns DXGI_OUTPUT_DESC with display name,
 *     desktop coordinates, monitor handle, rotation
 *   - IDXGIOutput::WaitForVBlank() blocks until the next vertical retrace
 *     of that monitor — the precision-timer used by present-frame loops
 *
 * We enumerate the topology, then measure inter-VBlank intervals over
 * 20 samples to derive the actual refresh rate.
 */

#define COBJMACROS
#define CINTERFACE
#define INITGUID

#include "shell.h"
#include <dxgi1_2.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "dxgi.lib")

#define DV_PROP    L"MS_DV_STATE"
#define ID_DV_OUT  104001
#define ID_DV_GO   104002
#define ID_DV_STOP 104003
#define WM_DV_LINE (WM_USER + 200)
#define WM_DV_DONE (WM_USER + 201)

typedef struct {
    HWND   frame, output;
    HANDLE thread, stopEvent;
} DvState;
static WNDPROC g_origDvFrame = NULL;

static void Dv_Append(HWND e, const wchar_t *t)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)t);
    SendMessageW(e, EM_SCROLLCARET, 0, 0);
}

static void Dv_Post(HWND frame, const wchar_t *t)
{
    wchar_t *p = _wcsdup(t);
    if (p) PostMessageW(frame, WM_DV_LINE, 0, (LPARAM)p);
}

static DWORD WINAPI Dv_Worker(LPVOID arg)
{
    DvState *st = (DvState *)arg;
    IDXGIFactory1 *factory = NULL;
    IDXGIAdapter1 *adapter = NULL;
    IDXGIOutput   *output = NULL;
    HRESULT hr;
    UINT ai = 0, oi = 0;

    hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory);
    if (FAILED(hr) || !factory) {
        Dv_Post(st->frame, L"CreateDXGIFactory1 failed.\r\n");
        PostMessageW(st->frame, WM_DV_DONE, 0, 0);
        return 0;
    }

    while (IDXGIFactory1_EnumAdapters1(factory, ai, &adapter) != DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC1 adesc;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &adesc))) {
            wchar_t line[400];
            swprintf_s(line, 400,
                L"\r\n== Adapter %u ==\r\n"
                L"  Description: %s\r\n"
                L"  VendorId   : 0x%04x\r\n"
                L"  DeviceId   : 0x%04x\r\n"
                L"  DedicatedVMem: %llu MB\r\n",
                ai, adesc.Description,
                adesc.VendorId, adesc.DeviceId,
                (ULONGLONG)adesc.DedicatedVideoMemory / (1024 * 1024));
            Dv_Post(st->frame, line);
        }
        oi = 0;
        while (IDXGIAdapter1_EnumOutputs(adapter, oi, &output) != DXGI_ERROR_NOT_FOUND) {
            DXGI_OUTPUT_DESC odesc;
            if (SUCCEEDED(IDXGIOutput_GetDesc(output, &odesc))) {
                wchar_t line[400];
                swprintf_s(line, 400,
                    L"  Output %u: %s\r\n"
                    L"     desktop: (%ld,%ld)-(%ld,%ld)  attached=%s\r\n",
                    oi, odesc.DeviceName,
                    odesc.DesktopCoordinates.left, odesc.DesktopCoordinates.top,
                    odesc.DesktopCoordinates.right, odesc.DesktopCoordinates.bottom,
                    odesc.AttachedToDesktop ? L"yes" : L"no");
                Dv_Post(st->frame, line);
            }
            /* Only measure VBlanks on the first attached output */
            if (oi == 0 && ai == 0) {
                LARGE_INTEGER freq, prev, now;
                int samples = 20;
                int i;
                double totalMs = 0;
                QueryPerformanceFrequency(&freq);
                QueryPerformanceCounter(&prev);
                Dv_Post(st->frame, L"     measuring 20 VBlanks...\r\n");
                for (i = 0; i < samples; ++i) {
                    if (WaitForSingleObject(st->stopEvent, 0) == WAIT_OBJECT_0) break;
                    IDXGIOutput_WaitForVBlank(output);
                    QueryPerformanceCounter(&now);
                    {
                        double ms = (now.QuadPart - prev.QuadPart) * 1000.0 / freq.QuadPart;
                        totalMs += ms;
                    }
                    prev = now;
                }
                if (i > 0) {
                    double avg = totalMs / i;
                    wchar_t line[200];
                    swprintf_s(line, 200,
                        L"     average VBlank period: %.3f ms (~%.1f Hz)\r\n",
                        avg, 1000.0 / avg);
                    Dv_Post(st->frame, line);
                }
            }
            IDXGIOutput_Release(output);
            ++oi;
        }
        IDXGIAdapter1_Release(adapter);
        ++ai;
    }
    IDXGIFactory1_Release(factory);
    PostMessageW(st->frame, WM_DV_DONE, 0, 0);
    return 0;
}

static LRESULT CALLBACK Dv_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    DvState *st = (DvState *)GetPropW(hwnd, DV_PROP);
    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_DV_GO) {
            DWORD tid;
            if (st->thread) return 0;
            SetWindowTextW(st->output, L"");
            ResetEvent(st->stopEvent);
            st->thread = CreateThread(NULL, 0, Dv_Worker, st, 0, &tid);
            return 0;
        }
        if (LOWORD(wp) == ID_DV_STOP) {
            if (st->thread) {
                SetEvent(st->stopEvent);
                WaitForSingleObject(st->thread, 4000);
                CloseHandle(st->thread);
                st->thread = NULL;
            }
            return 0;
        }
    }
    if (msg == WM_DV_LINE && st) {
        wchar_t *p = (wchar_t *)lp;
        if (p) { Dv_Append(st->output, p); free(p); }
        return 0;
    }
    if (msg == WM_DV_DONE && st) {
        if (st->thread) { CloseHandle(st->thread); st->thread = NULL; }
        Dv_Append(st->output, L"\r\n[worker exited]\r\n");
        return 0;
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->output, 8, 76, w - 16, h - 84, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        if (st->thread) {
            SetEvent(st->stopEvent);
            WaitForSingleObject(st->thread, 4000);
            CloseHandle(st->thread);
        }
        if (st->stopEvent) CloseHandle(st->stopEvent);
        free(st); RemovePropW(hwnd, DV_PROP);
    }
    return CallWindowProcW(g_origDvFrame, hwnd, msg, wp, lp);
}

static HWND DXGIVbl_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    DvState *st;
    HFONT mono;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"DXGIVbl",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (DvState *)calloc(1, sizeof(DvState));
    if (!st) { DestroyWindow(frame); return NULL; }
    st->frame = frame;
    st->stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    CreateWindowExW(0, L"BUTTON", L"Enumerate + measure",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 38, 200, 26, frame, (HMENU)(LONG_PTR)ID_DV_GO, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Stop",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        220, 38, 90, 26, frame, (HMENU)(LONG_PTR)ID_DV_STOP, hInstance, NULL);

    st->output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        8, 76, w - 16, h - 84, frame, (HMENU)(LONG_PTR)ID_DV_OUT, hInstance, NULL);
    mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(st->output, WM_SETFONT, (WPARAM)mono, TRUE);

    SetPropW(frame, DV_PROP, (HANDLE)st);
    if (!g_origDvFrame) g_origDvFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Dv_FrameProc);
    return frame;
}

MsApp g_AppDXGIVbl = { L"DXGIVbl", DXGIVbl_Create, 720, 480 };
