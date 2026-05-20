/*
 * app_hexview.c — Memory-mapped hex viewer
 *
 * Demonstrates:
 *   - CreateFileW + CreateFileMappingW + MapViewOfFile (read-only mmap)
 *   - DragAcceptFiles + WM_DROPFILES (drop a file onto the window to load it)
 *   - Custom-drawn scrollable hex/ASCII grid using WM_PAINT + WM_VSCROLL
 *   - Mouse-wheel scrolling via WM_MOUSEWHEEL
 *
 * Files are clamped to 1 MiB to keep the scroll math simple. Load by either
 * clicking "Open..." or dropping a file from Explorer.
 */

#include "shell.h"
#include <commdlg.h>
#include <shellapi.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

#define HV_CLASS    L"MiniShell_HexCanvas"
#define HV_PROP     L"MS_HV_STATE"
#define ID_HV_OPEN  12001
#define BYTES_PER_ROW 16
#define MAX_BYTES     (1024 * 1024)

typedef struct {
    HANDLE   hFile;
    HANDLE   hMap;
    const BYTE *view;
    SIZE_T   size;
    int      scrollRow;
    HWND     canvas;
    HWND     openBtn;
    HWND     pathLabel;
    wchar_t  path[MAX_PATH];
} HvState;

static void Hv_Unload(HvState *st)
{
    if (st->view) { UnmapViewOfFile(st->view); st->view = NULL; }
    if (st->hMap) { CloseHandle(st->hMap); st->hMap = NULL; }
    if (st->hFile && st->hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(st->hFile); st->hFile = NULL;
    }
    st->size = 0;
    st->scrollRow = 0;
    st->path[0] = 0;
}

static BOOL Hv_Load(HvState *st, const wchar_t *path)
{
    LARGE_INTEGER size;
    Hv_Unload(st);

    st->hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (st->hFile == INVALID_HANDLE_VALUE) {
        st->hFile = NULL;
        return FALSE;
    }
    if (!GetFileSizeEx(st->hFile, &size) || size.QuadPart == 0) {
        Hv_Unload(st);
        return FALSE;
    }
    st->size = size.QuadPart > MAX_BYTES ? MAX_BYTES : (SIZE_T)size.QuadPart;

    st->hMap = CreateFileMappingW(st->hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!st->hMap) { Hv_Unload(st); return FALSE; }

    st->view = (const BYTE *)MapViewOfFile(st->hMap, FILE_MAP_READ, 0, 0, st->size);
    if (!st->view) { Hv_Unload(st); return FALSE; }

    wcsncpy_s(st->path, MAX_PATH, path, _TRUNCATE);
    SetWindowTextW(st->pathLabel, st->path);
    return TRUE;
}

static LRESULT CALLBACK Hv_CanvasProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    HvState *st = (HvState *)GetPropW(GetParent(hwnd), HV_PROP);

    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc, memDC;
        HBITMAP memBmp, oldBmp;
        RECT rc;
        HFONT font, oldFont;
        TEXTMETRICW tm;
        int rowH, visibleRows, i, j;
        SIZE_T total;

        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);

        memDC = CreateCompatibleDC(hdc);
        memBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

        {
            HBRUSH bg = CreateSolidBrush(RGB(18, 22, 30));
            FillRect(memDC, &rc, bg);
            DeleteObject(bg);
        }

        font = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        oldFont = (HFONT)SelectObject(memDC, font);
        SetBkMode(memDC, TRANSPARENT);
        GetTextMetricsW(memDC, &tm);
        rowH = tm.tmHeight + 2;
        visibleRows = rc.bottom / rowH;

        if (!st || !st->view) {
            SetTextColor(memDC, RGB(160, 170, 190));
            TextOutW(memDC, 8, 8,
                L"Drop a file onto this window, or click Open...", 47);
        } else {
            total = st->size;
            for (i = 0; i < visibleRows; ++i) {
                SIZE_T offset = ((SIZE_T)st->scrollRow + i) * BYTES_PER_ROW;
                wchar_t line[200];
                int len;
                if (offset >= total) break;

                len = swprintf_s(line, 200, L"%08X  ", (unsigned)offset);
                for (j = 0; j < BYTES_PER_ROW; ++j) {
                    if (offset + j < total) {
                        len += swprintf_s(line + len, 200 - len, L"%02X ",
                                          st->view[offset + j]);
                    } else {
                        len += swprintf_s(line + len, 200 - len, L"   ");
                    }
                    if (j == 7) {
                        line[len++] = L' '; line[len] = 0;
                    }
                }
                line[len++] = L' '; line[len++] = L'|';
                for (j = 0; j < BYTES_PER_ROW && offset + j < total; ++j) {
                    BYTE b = st->view[offset + j];
                    line[len++] = (b >= 32 && b < 127) ? (wchar_t)b : L'.';
                }
                line[len++] = L'|'; line[len] = 0;

                SetTextColor(memDC, RGB(180, 220, 230));
                TextOutW(memDC, 6, i * rowH + 4, line, len);
            }
        }
        SelectObject(memDC, oldFont);
        DeleteObject(font);

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int rows = delta / 30;
        if (st) {
            int totalRows = (int)((st->size + BYTES_PER_ROW - 1) / BYTES_PER_ROW);
            st->scrollRow -= rows;
            if (st->scrollRow < 0) st->scrollRow = 0;
            if (st->scrollRow > totalRows - 1) st->scrollRow = totalRows - 1;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (st && st->view) {
            int totalRows = (int)((st->size + BYTES_PER_ROW - 1) / BYTES_PER_ROW);
            switch (wp) {
            case VK_DOWN:  st->scrollRow += 1; break;
            case VK_UP:    st->scrollRow -= 1; break;
            case VK_NEXT:  st->scrollRow += 20; break;
            case VK_PRIOR: st->scrollRow -= 20; break;
            case VK_HOME:  st->scrollRow = 0; break;
            case VK_END:   st->scrollRow = totalRows - 1; break;
            }
            if (st->scrollRow < 0) st->scrollRow = 0;
            if (st->scrollRow >= totalRows) st->scrollRow = totalRows - 1;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void EnsureHvClass(HINSTANCE hInstance)
{
    static BOOL registered = FALSE;
    WNDCLASSEXW wc;
    if (registered) return;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = Hv_CanvasProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = HV_CLASS;
    RegisterClassExW(&wc);
    registered = TRUE;
}

static WNDPROC g_origHvFrame = NULL;

static LRESULT CALLBACK Hv_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    HvState *st = (HvState *)GetPropW(hwnd, HV_PROP);

    if (msg == WM_DROPFILES && st) {
        HDROP drop = (HDROP)wp;
        wchar_t path[MAX_PATH];
        if (DragQueryFileW(drop, 0, path, MAX_PATH)) {
            Hv_Load(st, path);
            InvalidateRect(st->canvas, NULL, FALSE);
        }
        DragFinish(drop);
        return 0;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == ID_HV_OPEN && st) {
        OPENFILENAMEW ofn;
        wchar_t file[MAX_PATH] = L"";
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner   = hwnd;
        ofn.lpstrFile   = file;
        ofn.nMaxFile    = MAX_PATH;
        ofn.lpstrFilter = L"All files\0*.*\0";
        ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
        if (GetOpenFileNameW(&ofn)) {
            Hv_Load(st, file);
            InvalidateRect(st->canvas, NULL, FALSE);
        }
        return 0;
    }
    if (msg == WM_SIZE && st) {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(st->openBtn,   8,  34, 80,      24, TRUE);
        MoveWindow(st->pathLabel, 96, 38, w - 104, 18, TRUE);
        MoveWindow(st->canvas,    8,  64, w - 16,  h - 72, TRUE);
    }
    if (msg == WM_DESTROY && st) {
        Hv_Unload(st);
        free(st);
        RemovePropW(hwnd, HV_PROP);
    }
    return CallWindowProcW(g_origHvFrame, hwnd, msg, wp, lp);
}

static HWND HexView_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    HvState *st;
    (void)self;

    EnsureHvClass(hInstance);
    frame = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_ACCEPTFILES,
        MS_CLASS_APPFRAME, L"HexView",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    st = (HvState *)calloc(1, sizeof(HvState));
    if (!st) { DestroyWindow(frame); return NULL; }

    st->openBtn = CreateWindowExW(0, L"BUTTON", L"Open...",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        8, 34, 80, 24, frame, (HMENU)(LONG_PTR)ID_HV_OPEN, hInstance, NULL);

    st->pathLabel = CreateWindowExW(0, L"STATIC",
        L"(drop a file onto this window)",
        WS_CHILD | WS_VISIBLE,
        96, 38, w - 104, 18, frame, NULL, hInstance, NULL);

    st->canvas = CreateWindowExW(0, HV_CLASS, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        8, 64, w - 16, h - 72, frame, NULL, hInstance, NULL);

    SetPropW(frame, HV_PROP, (HANDLE)st);
    if (!g_origHvFrame)
        g_origHvFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Hv_FrameProc);

    DragAcceptFiles(frame, TRUE);
    return frame;
}

MsApp g_AppHexView = {
    L"HexView",
    HexView_Create,
    640, 460
};
