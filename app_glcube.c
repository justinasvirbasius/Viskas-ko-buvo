/*
 * app_glcube.c — Rotating OpenGL cube
 *
 * Demonstrates:
 *   - Setting up an OpenGL rendering context on a child window:
 *     PIXELFORMATDESCRIPTOR + ChoosePixelFormat + SetPixelFormat +
 *     wglCreateContext + wglMakeCurrent
 *   - Animation via SetTimer, redrawing at ~60 Hz
 *   - Double-buffered SwapBuffers
 *
 * The cube is drawn with legacy fixed-function GL (glBegin/glEnd) for brevity;
 * fine for opengl32.lib without any extension loader.
 */

#include "shell.h"
#include <GL/gl.h>
#include <stdlib.h>

#pragma comment(lib, "opengl32.lib")

#define GL_CLASS L"MiniShell_GLCanvas"
#define GL_TIMER 1

typedef struct {
    HDC   hdc;
    HGLRC hglrc;
    float angle;
} GlState;

static void Gl_Render(HWND hwnd, GlState *st)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (rc.right == 0 || rc.bottom == 0) return;

    glViewport(0, 0, rc.right, rc.bottom);
    glClearColor(0.08f, 0.10f, 0.16f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    {
        /* Manual perspective: a 60° field of view at the current aspect */
        double aspect = (double)rc.right / (double)rc.bottom;
        double f = 1.0 / 0.5774;   /* tan(30°) = 1/sqrt(3) approx */
        double zn = 0.1, zf = 50.0;
        double m[16] = {0};
        m[0]  = f / aspect;
        m[5]  = f;
        m[10] = (zf + zn) / (zn - zf);
        m[11] = -1.0;
        m[14] = (2 * zf * zn) / (zn - zf);
        glMultMatrixd(m);
    }

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -4.0f);
    glRotatef(st->angle,        1.0f, 0.0f, 0.0f);
    glRotatef(st->angle * 0.7f, 0.0f, 1.0f, 0.0f);

    glEnable(GL_DEPTH_TEST);

    glBegin(GL_QUADS);
    /* Front (red) */
    glColor3f(0.9f, 0.2f, 0.3f);
    glVertex3f(-1, -1,  1); glVertex3f( 1, -1,  1);
    glVertex3f( 1,  1,  1); glVertex3f(-1,  1,  1);
    /* Back (green) */
    glColor3f(0.2f, 0.8f, 0.4f);
    glVertex3f(-1, -1, -1); glVertex3f(-1,  1, -1);
    glVertex3f( 1,  1, -1); glVertex3f( 1, -1, -1);
    /* Top (blue) */
    glColor3f(0.3f, 0.5f, 0.9f);
    glVertex3f(-1,  1, -1); glVertex3f(-1,  1,  1);
    glVertex3f( 1,  1,  1); glVertex3f( 1,  1, -1);
    /* Bottom (yellow) */
    glColor3f(0.9f, 0.8f, 0.2f);
    glVertex3f(-1, -1, -1); glVertex3f( 1, -1, -1);
    glVertex3f( 1, -1,  1); glVertex3f(-1, -1,  1);
    /* Right (magenta) */
    glColor3f(0.8f, 0.3f, 0.8f);
    glVertex3f( 1, -1, -1); glVertex3f( 1,  1, -1);
    glVertex3f( 1,  1,  1); glVertex3f( 1, -1,  1);
    /* Left (cyan) */
    glColor3f(0.3f, 0.8f, 0.9f);
    glVertex3f(-1, -1, -1); glVertex3f(-1, -1,  1);
    glVertex3f(-1,  1,  1); glVertex3f(-1,  1, -1);
    glEnd();

    SwapBuffers(st->hdc);
}

static BOOL Gl_Setup(HWND hwnd, GlState *st)
{
    PIXELFORMATDESCRIPTOR pfd;
    int format;

    st->hdc = GetDC(hwnd);
    if (!st->hdc) return FALSE;

    ZeroMemory(&pfd, sizeof(pfd));
    pfd.nSize        = sizeof(pfd);
    pfd.nVersion     = 1;
    pfd.dwFlags      = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType   = PFD_TYPE_RGBA;
    pfd.cColorBits   = 32;
    pfd.cDepthBits   = 24;
    pfd.iLayerType   = PFD_MAIN_PLANE;

    format = ChoosePixelFormat(st->hdc, &pfd);
    if (!format || !SetPixelFormat(st->hdc, format, &pfd)) return FALSE;

    st->hglrc = wglCreateContext(st->hdc);
    if (!st->hglrc) return FALSE;
    return wglMakeCurrent(st->hdc, st->hglrc);
}

static LRESULT CALLBACK Gl_CanvasProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    GlState *st = (GlState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE:
        st = (GlState *)calloc(1, sizeof(GlState));
        if (!st) return -1;
        if (!Gl_Setup(hwnd, st)) {
            free(st);
            return -1;
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        SetTimer(hwnd, GL_TIMER, 16, NULL);
        return 0;

    case WM_TIMER:
        st->angle += 1.2f;
        if (st->angle > 360.0f) st->angle -= 360.0f;
        Gl_Render(hwnd, st);
        return 0;

    case WM_SIZE:
        Gl_Render(hwnd, st);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        Gl_Render(hwnd, st);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, GL_TIMER);
        if (st) {
            wglMakeCurrent(NULL, NULL);
            if (st->hglrc) wglDeleteContext(st->hglrc);
            if (st->hdc)   ReleaseDC(hwnd, st->hdc);
            free(st);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void EnsureGlClass(HINSTANCE hInstance)
{
    static BOOL registered = FALSE;
    WNDCLASSEXW wc;
    if (registered) return;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    /* CS_OWNDC is required by OpenGL — the DC must persist */
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = Gl_CanvasProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = GL_CLASS;
    RegisterClassExW(&wc);
    registered = TRUE;
}

static WNDPROC g_origGlFrame = NULL;

static LRESULT CALLBACK Gl_FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_SIZE) {
        HWND canvas = FindWindowExW(hwnd, NULL, GL_CLASS, NULL);
        if (canvas) {
            int w = LOWORD(lp), h = HIWORD(lp);
            MoveWindow(canvas, 4, 32, w - 8, h - 36, TRUE);
        }
    }
    return CallWindowProcW(g_origGlFrame, hwnd, msg, wp, lp);
}

static HWND GlCube_Create(HWND parent, int x, int y, int w, int h, MsApp *self)
{
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    HWND frame;
    (void)self;

    EnsureGlClass(hInstance);
    frame = CreateWindowExW(WS_EX_TOOLWINDOW, MS_CLASS_APPFRAME, L"GLCube",
        WS_POPUP | WS_BORDER | WS_THICKFRAME,
        x, y, w, h, parent, NULL, hInstance, NULL);
    if (!frame) return NULL;

    CreateWindowExW(0, GL_CLASS, L"",
        WS_CHILD | WS_VISIBLE,
        4, 32, w - 8, h - 36, frame, NULL, hInstance, NULL);

    if (!g_origGlFrame)
        g_origGlFrame = (WNDPROC)GetWindowLongPtrW(frame, GWLP_WNDPROC);
    SetWindowLongPtrW(frame, GWLP_WNDPROC, (LONG_PTR)Gl_FrameProc);
    return frame;
}

MsApp g_AppGlCube = {
    L"GLCube",
    GlCube_Create,
    420, 360
};
