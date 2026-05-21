/*
 * app_metafile.c — GDI Enhanced Metafile recording and playback
 *
 * Demonstrates EMF — the GDI device-independent vector format:
 *   - CreateEnhMetaFileW(NULL, NULL, &rect, L"...") to start an in-memory recording
 *   - Draw onto the metafile DC like any HDC (lines, ellipses, text, fills)
 *   - CloseEnhMetaFile returns an HENHMETAFILE
 *   - GetEnhMetaFileBits to extract the byte stream (could be saved as .emf)
 *   - PlayEnhMetaFile to render the recorded picture into our window's HDC
 *
 * Two buttons: Record (re-creates a metafile with the current "scene" and
 * shows its size in bytes), and Replay (draws the recorded metafile into the
 * canvas at the current window size, demonstrating resolution independence).
 */

#include "shell.h"
#include <stdlib.h>
#include <stdio.h>

#define MF_PROP    L"MS_MF_STATE"
#define ID_MF_REC  46001
#define ID_MF_REP  46002
#define ID_MF_STAT 46003

typedef struct {
    HENHMETAFILE meta;
    HWND status;
    UINT byteSize;
} MfState;

static WNDPROC g_origMfFrame = NULL;

static HENHMETAFILE Mf_Record(MfState *st)
{
    HDC mfdc;
    RECT box = { 0, 0, 2000, 2000 };   /* HIMETRIC units (0.01 mm) */
    HENHMETAFILE meta;
    HPEN pen, oldPen;
    HBRUSH brush, oldBrush;
    HFONT font, oldFont;
    int i;

    mfdc = CreateEnhMetaFileW(NULL, NULL, &box,
        L"MiniShell\0MetaFile demo\0");
    if (!mfdc) return NULL;

    /* gradient-ish ring of rectangles */
    pen = CreatePen(PS_SOLID, 6, RGB(40, 80, 120));
    oldPen = (HPEN)SelectObject(mfdc, pen);
    for (i = 0; i < 8; ++i) {
        BYTE r = (BYTE)(60 + i * 24);
        BYTE g = (BYTE)(180 - i * 18);
        BYTE b = (BYTE)(220);
        brush = CreateSolidBrush(RGB(r, g, b));
        oldBrush = (HBRUSH)SelectObject(mfdc, brush);
        Rectangle(mfdc, 200 + i * 150, 200 + i * 80,
                        200 + i * 150 + 400, 200 + i * 80 + 700);
        SelectObject(mfdc, oldBrush);
        DeleteObject(brush);
    }
    SelectObject(mfdc, oldPen);
    DeleteObject(pen);

    /* Diagonals */
    pen = CreatePen(PS_DASH, 2, RGB(220, 30, 30));
    oldPen = (HPEN)SelectObject(mfdc, pen);
    MoveToEx(mfdc,    0,    0, NULL);
    LineTo(mfdc,   2000, 2000);
    MoveToEx(mfdc,    0, 2000, NULL);
    LineTo(mfdc,   2000,    0);
    SelectObject(mfdc, oldPen);
    DeleteObject(pen);

    /* Caption */
    font = CreateFontW(180, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    oldFont = (HFONT)SelectObject(mfdc, font);
    SetBkMode(mfdc, TRANSPARENT);
    SetTextColor(mfdc, RGB(20, 20, 20));
    TextOutW(mfdc, 200, 1700, L"EnhMetaFile vector recording", 28);
    SelectObject(mfdc, oldFont);
    DeleteObject(font);

    meta = CloseEnhMetaFile(mfdc);

    /* Extract size: just for status */
    st->byteSize = GetEnhMetaFileBits(meta, 0, NULL);

    return meta;
}

static void Mf_Paint(HWND hwnd, MfState *st)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    HBRUSH bg;

    GetClientRect(hwnd, &rc);
    rc.top += 60;
    bg = CreateSolidBrush(RGB(250, 250, 250));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    if (st->meta) {
        PlayEnhMetaFile(hdc, st->meta, &rc);
    } else {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(100, 100, 100));
        DrawTextW(hdc, L"Click 'Record' first.", -1, &rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK Mf_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    MfState *st = (MfState *)GetPropW(hwnd, MF_PROP);

    if (msg == WM_COMMAND && st) {
        if (LOWORD(wp) == ID_MF_REC) {
            if (st->meta) DeleteEnhMetaFile(st->meta);
            st->meta = Mf_Record(st);
            if (st->meta) {
                wchar_t buf[80];
                swprintf_s(buf, 80,
                    L"Recorded: %u bytes (could save as .emf).", st->byteSize);
                SetWindowTextW(st->status, buf);
                InvalidateRect(hwnd, NULL, FALSE);
            } else {
                SetWindowTextW(st->status, L"CreateEnhMetaFile failed.");
            }
            return 0;
        }
        if (LOWORD(wp) == ID_MF_REP) {
            if (st->meta) InvalidateRect(hwnd, NULL, TRUE);
            else SetWindowTextW(st->status, L"Record first.");
            return 0;
        }
    }
    if (msg == WM_PAINT && st) { Mf_Paint(hwnd, st); return 0; }
    if (msg == WM_SIZE) { InvalidateRect(hwnd, NULL, TRUE); }
    if (msg == WM_DESTROY && st) {
        if (st->meta) DeleteEnhMetaFile(st->meta);
        free(st);
        RemovePropW(hwnd, MF_PROP);
    }
    return CallWindowProcW(g_origMfFrame, hwnd, msg, wp, lp);
}

static HWND MetaFile_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    MfState *st;
    (void)self;

    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"MetaFile",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;
    st = (MfState *)calloc(1, sizeof(MfState));
    if (!st) { DestroyWindow(frame); return NULL; }

    CreateWindowExW(0, L"BUTTON", L"Record",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        12, 34, 90, 24, frame, (HMENU)(LONG_PTR)ID_MF_REC, hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Replay",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        110, 34, 90, 24, frame, (HMENU)(LONG_PTR)ID_MF_REP, hInstance, NULL);
    st->status = CreateWindowExW(0, L"STATIC",
        L"Press Record to build an EMF, then Replay (or resize) to scale it.",
        WS_CHILD | WS_VISIBLE,
        208, 38, w - 220, 20, frame, (HMENU)(LONG_PTR)ID_MF_STAT, hInstance, NULL);

    SetPropW(frame, MF_PROP, (HANDLE)st);
    if (!g_origMfFrame) g_origMfFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Mf_FrameProc);
    return frame;
}

MsApp g_AppMetaFile = {
    L"MetaFile",
    MetaFile_Create,
    560, 420
};
