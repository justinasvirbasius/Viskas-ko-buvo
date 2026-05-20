/*
 * app_snake.c — Snake game
 *
 * Demonstrates a game loop pattern: high-resolution timing via
 * QueryPerformanceCounter (to compute frame deltas), WM_KEYDOWN input,
 * SetTimer at 30 Hz, and frame-based state updates with rendering.
 */

#include "shell.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define SNAKE_CLASS   L"MiniShell_SnakeCanvas"
#define SNAKE_TIMER   1
#define GRID_W        20
#define GRID_H        16
#define CELL          18
#define MAX_LEN       (GRID_W * GRID_H)

typedef struct { int x, y; } Cell;

typedef struct {
    Cell    body[MAX_LEN];
    int     length;
    int     dx, dy;
    Cell    food;
    int     score;
    BOOL    alive;
    LARGE_INTEGER freq;
    LARGE_INTEGER lastTick;
    double  accum;
    double  stepSec;
} SnakeState;

static void Snake_PlaceFood(SnakeState *st)
{
    int i, ok;
    do {
        st->food.x = rand() % GRID_W;
        st->food.y = rand() % GRID_H;
        ok = 1;
        for (i = 0; i < st->length; ++i) {
            if (st->body[i].x == st->food.x && st->body[i].y == st->food.y) {
                ok = 0;
                break;
            }
        }
    } while (!ok);
}

static void Snake_Reset(SnakeState *st)
{
    st->length = 4;
    st->body[0].x = GRID_W / 2;     st->body[0].y = GRID_H / 2;
    st->body[1].x = GRID_W / 2 - 1; st->body[1].y = GRID_H / 2;
    st->body[2].x = GRID_W / 2 - 2; st->body[2].y = GRID_H / 2;
    st->body[3].x = GRID_W / 2 - 3; st->body[3].y = GRID_H / 2;
    st->dx = 1; st->dy = 0;
    st->score = 0;
    st->alive = TRUE;
    st->accum = 0;
    st->stepSec = 0.12;
    Snake_PlaceFood(st);
}

static void Snake_Step(SnakeState *st)
{
    int i, nx, ny;
    if (!st->alive) return;
    nx = st->body[0].x + st->dx;
    ny = st->body[0].y + st->dy;
    if (nx < 0 || nx >= GRID_W || ny < 0 || ny >= GRID_H) {
        st->alive = FALSE; return;
    }
    for (i = 0; i < st->length; ++i) {
        if (st->body[i].x == nx && st->body[i].y == ny) {
            st->alive = FALSE; return;
        }
    }
    if (nx == st->food.x && ny == st->food.y) {
        if (st->length < MAX_LEN) st->length++;
        st->score++;
        if (st->stepSec > 0.05) st->stepSec *= 0.97;
        Snake_PlaceFood(st);
    }
    for (i = st->length - 1; i > 0; --i) st->body[i] = st->body[i - 1];
    st->body[0].x = nx;
    st->body[0].y = ny;
}

static LRESULT CALLBACK Snake_CanvasProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    SnakeState *st = (SnakeState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE:
        st = (SnakeState *)calloc(1, sizeof(SnakeState));
        if (!st) return -1;
        QueryPerformanceFrequency(&st->freq);
        QueryPerformanceCounter(&st->lastTick);
        Snake_Reset(st);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        SetTimer(hwnd, SNAKE_TIMER, 33, NULL);
        return 0;

    case WM_TIMER: {
        LARGE_INTEGER now;
        double dt;
        QueryPerformanceCounter(&now);
        dt = (double)(now.QuadPart - st->lastTick.QuadPart) / (double)st->freq.QuadPart;
        st->lastTick = now;
        st->accum += dt;
        while (st->accum >= st->stepSec && st->alive) {
            Snake_Step(st);
            st->accum -= st->stepSec;
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        return 0;

    case WM_KEYDOWN:
        if (!st) return 0;
        if (!st->alive && (wp == VK_RETURN || wp == VK_SPACE)) {
            Snake_Reset(st);
            return 0;
        }
        switch (wp) {
        case VK_LEFT:  if (st->dx !=  1) { st->dx = -1; st->dy = 0; } break;
        case VK_RIGHT: if (st->dx != -1) { st->dx =  1; st->dy = 0; } break;
        case VK_UP:    if (st->dy !=  1) { st->dx = 0;  st->dy = -1; } break;
        case VK_DOWN:  if (st->dy != -1) { st->dx = 0;  st->dy =  1; } break;
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc, memDC;
        HBITMAP memBmp, oldBmp;
        RECT rc;
        HBRUSH bg, snakeBrush, headBrush, foodBrush;
        HFONT font, oldFont;
        wchar_t scoreBuf[64];
        int i;

        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);

        memDC = CreateCompatibleDC(hdc);
        memBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

        bg = CreateSolidBrush(RGB(20, 30, 25));
        FillRect(memDC, &rc, bg);
        DeleteObject(bg);

        snakeBrush = CreateSolidBrush(RGB(80, 200, 100));
        headBrush  = CreateSolidBrush(RGB(180, 240, 140));
        foodBrush  = CreateSolidBrush(RGB(240, 80, 80));

        for (i = 0; i < st->length; ++i) {
            RECT cell;
            cell.left   = 10 + st->body[i].x * CELL;
            cell.top    = 40 + st->body[i].y * CELL;
            cell.right  = cell.left + CELL - 1;
            cell.bottom = cell.top  + CELL - 1;
            FillRect(memDC, &cell, i == 0 ? headBrush : snakeBrush);
        }
        {
            RECT cell;
            cell.left   = 10 + st->food.x * CELL;
            cell.top    = 40 + st->food.y * CELL;
            cell.right  = cell.left + CELL - 1;
            cell.bottom = cell.top  + CELL - 1;
            FillRect(memDC, &cell, foodBrush);
        }
        DeleteObject(snakeBrush);
        DeleteObject(headBrush);
        DeleteObject(foodBrush);

        font = CreateFontW(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        oldFont = (HFONT)SelectObject(memDC, font);
        SetBkMode(memDC, TRANSPARENT);
        SetTextColor(memDC, RGB(220, 240, 220));
        swprintf_s(scoreBuf, 64, L"Score: %d", st->score);
        TextOutW(memDC, 12, 10, scoreBuf, (int)wcslen(scoreBuf));
        if (!st->alive) {
            SetTextColor(memDC, RGB(255, 200, 200));
            TextOutW(memDC, rc.right - 220, 10,
                     L"Game Over - press Enter", 23);
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

    case WM_DESTROY:
        KillTimer(hwnd, SNAKE_TIMER);
        if (st) free(st);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void EnsureSnakeClass(HINSTANCE hInstance)
{
    static BOOL registered = FALSE;
    WNDCLASSEXW wc;
    if (registered) return;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = Snake_CanvasProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = SNAKE_CLASS;
    RegisterClassExW(&wc);
    registered = TRUE;
}

static HWND Snake_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame, canvas;
    (void)self;

    EnsureSnakeClass(hInstance);
    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"Snake",
        WS_POPUP | WS_BORDER,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    canvas = CreateWindowExW(0, SNAKE_CLASS, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        4, 32, w - 8, h - 36, frame, NULL, hInstance, NULL);

    SetFocus(canvas);
    return frame;
}

MsApp g_AppSnake = {
    L"Snake",
    Snake_Create,
    GRID_W * CELL + 28, GRID_H * CELL + 80
};
