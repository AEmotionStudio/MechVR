// Window, OpenGL context and small GL helpers.
// Linux uses X11 and GLX (gfx.c); Windows uses Win32 and WGL (gfx_win32.c). Shader and texture helpers are shared (gfx.c).
#ifndef GFX_H
#define GFX_H
#ifdef _WIN32
// Windows' own OpenGL library stops at version 1.1; everything newer is looked up when the context is made (glfuncs.h).
// Defining these two first keeps the GL headers from pulling all of windows.h into every file.
#ifndef APIENTRY
#define APIENTRY __stdcall
#endif
#ifndef WINGDIAPI
#define WINGDIAPI __declspec(dllimport)
#endif
#include <GL/gl.h>
#include <GL/glext.h>
#include "glfuncs.h"
#else
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#endif

typedef struct {
#ifdef _WIN32
    void *hwnd, *dc, *ctx;      // HWND, HDC and HGLRC
#else
    Display *dpy;
    Window win;
    GLXContext ctx;
    GLXFBConfig fbc;
    unsigned long visualid;
#endif
    int w, h;
    int quit;
    unsigned char key[256];     // held state, indexed by lowercase ASCII (plus K_* codes)
    unsigned char pressed[256]; // went down this frame
    int mx, my, mdx, mdy;       // mouse position and per-frame delta
    unsigned char mbtn[4], mpressed[4];
} Gfx;

enum { K_UP = 128, K_DOWN, K_LEFT, K_RIGHT, K_SHIFT, K_ESC, K_TAB };

int gfx_init(Gfx *g, int w, int h, const char *title);
void gfx_poll(Gfx *g);
void gfx_swap(Gfx *g);

extern const char *gfx_define; // set before gfx_program to put extra lines (such as a #define) at the top of both shaders
GLuint gfx_program(const char *name, const char *vs, int vsLen, const char *fs, int fsLen);
GLuint gfx_tex(int w, int h, GLenum internal, GLenum filter);
double gfx_time(void);
#endif
