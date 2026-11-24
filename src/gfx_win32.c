// Windows: Win32 window, WGL context, keyboard and mouse. The Linux version of these functions is in gfx.c;
// the shader and texture helpers there are shared.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "gfx.h"
#include <stdio.h>
#include <string.h>

#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
typedef HGLRC(WINAPI *CreateCtx)(HDC, HGLRC, const int *);
typedef BOOL(WINAPI *SwapInt)(int);

#define GL_FUNC_DEFINE(type, name) type name;
GL_FUNCS(GL_FUNC_DEFINE)
#undef GL_FUNC_DEFINE

static Gfx *cur; // the window procedure writes input here

// Some drivers return small numbers instead of NULL for a function they do not have.
static void *gl_proc(const char *name) {
    void *p = (void *)wglGetProcAddress(name);
    if (p == (void *)1 || p == (void *)2 || p == (void *)3 || p == (void *)-1) p = NULL;
    return p;
}

static int load_gl(void) {
    int ok = 1;
#define GL_FUNC_LOAD(type, name)                                                     \
    if (!(name = (type)gl_proc(#name))) {                                            \
        fprintf(stderr, "gfx: the OpenGL driver has no %s\n", #name);                \
        ok = 0;                                                                      \
    }
    GL_FUNCS(GL_FUNC_LOAD)
#undef GL_FUNC_LOAD
    return ok;
}

double gfx_time(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
}

// Same codes as the X11 version: lowercase letters and digits as ASCII, the rest as K_* codes.
static int keycode(WPARAM vk) {
    if (vk >= 'A' && vk <= 'Z') return (int)(vk - 'A' + 'a');
    if (vk >= '0' && vk <= '9') return (int)vk;
    switch (vk) {
    case VK_SPACE: return ' ';
    case VK_UP: return K_UP;
    case VK_DOWN: return K_DOWN;
    case VK_LEFT: return K_LEFT;
    case VK_RIGHT: return K_RIGHT;
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return K_SHIFT;
    case VK_ESCAPE: return K_ESC;
    case VK_TAB: return K_TAB;
    }
    return 0;
}

static void button(Gfx *g, HWND hwnd, int b, int down) {
    if (down) g->mpressed[b] = 1;
    g->mbtn[b] = (unsigned char)down;
    // keep getting the mouse while a button is held, as X11 does, so a look-around drag can leave the window
    if (down) SetCapture(hwnd);
    else if (!g->mbtn[1] && !g->mbtn[2] && !g->mbtn[3]) ReleaseCapture();
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Gfx *g = cur;
    if (g) switch (msg) {
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: {
        int k = keycode(wp), down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        if (k) {
            if (down && !g->key[k]) g->pressed[k] = 1; // held keys repeat; only the first press counts
            g->key[k] = (unsigned char)down;
        }
        if (msg == WM_KEYDOWN || msg == WM_KEYUP) return 0;
        break; // let Windows handle Alt combinations such as Alt+F4
    }
    case WM_LBUTTONDOWN: button(g, hwnd, 1, 1); return 0;
    case WM_LBUTTONUP: button(g, hwnd, 1, 0); return 0;
    case WM_MBUTTONDOWN: button(g, hwnd, 2, 1); return 0;
    case WM_MBUTTONUP: button(g, hwnd, 2, 0); return 0;
    case WM_RBUTTONDOWN: button(g, hwnd, 3, 1); return 0;
    case WM_RBUTTONUP: button(g, hwnd, 3, 0); return 0;
    case WM_MOUSEMOVE: {
        int x = (int)(short)LOWORD(lp), y = (int)(short)HIWORD(lp);
        g->mdx += x - g->mx;
        g->mdy += y - g->my;
        g->mx = x;
        g->my = y;
        return 0;
    }
    case WM_KILLFOCUS: // the key-up of a key held while switching windows goes elsewhere; do not leave it stuck down
        memset(g->key, 0, sizeof g->key);
        memset(g->mbtn, 0, sizeof g->mbtn);
        return 0;
    case WM_SIZE:
        g->w = LOWORD(lp);
        g->h = HIWORD(lp);
        return 0;
    case WM_SYSCOMMAND: // a tap of Alt or F10 would open the window menu, and the game (and headset) would wait on it
        if ((wp & 0xFFF0) == SC_KEYMENU) return 0;
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        g->quit = 1;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int gfx_init(Gfx *g, int w, int h, const char *title) {
    memset(g, 0, sizeof *g);
    cur = g;
    SetProcessDPIAware(); // a window of w x h real pixels, as on Linux, rather than one Windows scales up and blurs
    HINSTANCE inst = GetModuleHandleA(NULL);
    WNDCLASSA wc = {0};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.lpszClassName = "MechVR";
    RegisterClassA(&wc);
    DWORD style = WS_OVERLAPPEDWINDOW;
    RECT r = {0, 0, w, h};
    AdjustWindowRect(&r, style, FALSE);
    HWND hwnd = CreateWindowA("MechVR", title, style | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                              r.bottom - r.top, NULL, NULL, inst, NULL);
    if (!hwnd) { fprintf(stderr, "gfx: cannot create a window\n"); return 0; }
    HDC dc = GetDC(hwnd);

    PIXELFORMATDESCRIPTOR pfd = {0};
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 24;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24; // the game draws into its own buffers, but some drivers pick a better format when asked for these
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;
    int pf = ChoosePixelFormat(dc, &pfd);
    if (!pf || !SetPixelFormat(dc, pf, &pfd)) { fprintf(stderr, "gfx: no OpenGL pixel format\n"); return 0; }

    // A plain context first, only to look up the function that makes a 4.5 core one.
    HGLRC tmp = wglCreateContext(dc);
    if (!tmp || !wglMakeCurrent(dc, tmp)) { fprintf(stderr, "gfx: cannot create an OpenGL context\n"); return 0; }
    CreateCtx create = (CreateCtx)gl_proc("wglCreateContextAttribsARB");
    int catt[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 4, WGL_CONTEXT_MINOR_VERSION_ARB, 5,
                  WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
    HGLRC ctx = create ? create(dc, NULL, catt) : NULL;
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(tmp);
    if (!ctx) { fprintf(stderr, "gfx: cannot create an OpenGL 4.5 context\n"); return 0; }
    wglMakeCurrent(dc, ctx);
    if (!load_gl()) return 0;
    SwapInt si = (SwapInt)gl_proc("wglSwapIntervalEXT");
    if (si) si(0); // never wait for the desktop monitor; the headset paces us

    g->hwnd = hwnd;
    g->dc = dc;
    g->ctx = ctx;
    g->w = w;
    g->h = h;
    return 1;
}

void gfx_poll(Gfx *g) {
    memset(g->pressed, 0, sizeof g->pressed);
    memset(g->mpressed, 0, sizeof g->mpressed);
    g->mdx = g->mdy = 0;
    cur = g;
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) g->quit = 1;
        DispatchMessageA(&m);
    }
}

void gfx_swap(Gfx *g) { SwapBuffers((HDC)g->dc); }
#endif
