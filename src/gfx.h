// Window, OpenGL context and small GL helpers.
#ifndef GFX_H
#define GFX_H
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <X11/Xlib.h>

typedef struct {
    Display *dpy;
    Window win;
    GLXContext ctx;
    GLXFBConfig fbc;
    unsigned long visualid;
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

GLuint gfx_program(const char *name, const char *vs, int vsLen, const char *fs, int fsLen);
GLuint gfx_tex(int w, int h, GLenum internal, GLenum filter);
double gfx_time(void);
#endif
