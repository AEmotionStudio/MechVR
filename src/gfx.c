#include "gfx.h"
#include <X11/XKBlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

extern const char shader_common[];
extern const int shader_common_len;

static Atom wmDelete;

double gfx_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int gfx_init(Gfx *g, int w, int h, const char *title) {
    memset(g, 0, sizeof *g);
    g->dpy = XOpenDisplay(NULL);
    if (!g->dpy) { fprintf(stderr, "gfx: cannot open X display\n"); return 0; }
    int scr = DefaultScreen(g->dpy);
    int att[] = {GLX_X_RENDERABLE, True, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT, GLX_RENDER_TYPE, GLX_RGBA_BIT,
                 GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_DOUBLEBUFFER, True, None};
    int n = 0;
    GLXFBConfig *cfgs = glXChooseFBConfig(g->dpy, scr, att, &n);
    if (!cfgs || !n) { fprintf(stderr, "gfx: no GLX framebuffer config\n"); return 0; }
    g->fbc = cfgs[0];
    XFree(cfgs);
    XVisualInfo *vi = glXGetVisualFromFBConfig(g->dpy, g->fbc);
    g->visualid = vi->visualid;
    Window root = RootWindow(g->dpy, scr);
    XSetWindowAttributes swa = {0};
    swa.colormap = XCreateColormap(g->dpy, root, vi->visual, AllocNone);
    swa.event_mask = KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                     StructureNotifyMask;
    g->win = XCreateWindow(g->dpy, root, 0, 0, w, h, 0, vi->depth, InputOutput, vi->visual,
                           CWColormap | CWEventMask, &swa);
    XFree(vi);
    XStoreName(g->dpy, g->win, title);
    wmDelete = XInternAtom(g->dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(g->dpy, g->win, &wmDelete, 1);
    XkbSetDetectableAutoRepeat(g->dpy, True, NULL);
    XMapWindow(g->dpy, g->win);

    typedef GLXContext (*CreateCtx)(Display *, GLXFBConfig, GLXContext, Bool, const int *);
    CreateCtx create = (CreateCtx)glXGetProcAddressARB((const GLubyte *)"glXCreateContextAttribsARB");
    int catt[] = {GLX_CONTEXT_MAJOR_VERSION_ARB, 4, GLX_CONTEXT_MINOR_VERSION_ARB, 5,
                  GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB, None};
    g->ctx = create ? create(g->dpy, g->fbc, NULL, True, catt) : NULL;
    if (!g->ctx) { fprintf(stderr, "gfx: cannot create an OpenGL 4.5 context\n"); return 0; }
    glXMakeCurrent(g->dpy, g->win, g->ctx);
    typedef void (*SwapInt)(Display *, GLXDrawable, int);
    SwapInt si = (SwapInt)glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalEXT");
    if (si) si(g->dpy, g->win, 0); // never wait for the desktop monitor; the headset paces us
    g->w = w;
    g->h = h;
    return 1;
}

static int keycode(KeySym s) {
    if (s >= XK_a && s <= XK_z) return (int)s;
    if (s >= XK_A && s <= XK_Z) return (int)(s - XK_A + XK_a);
    if (s >= XK_0 && s <= XK_9) return (int)s;
    switch (s) {
    case XK_space: return ' ';
    case XK_Up: return K_UP;
    case XK_Down: return K_DOWN;
    case XK_Left: return K_LEFT;
    case XK_Right: return K_RIGHT;
    case XK_Shift_L: case XK_Shift_R: return K_SHIFT;
    case XK_Escape: return K_ESC;
    case XK_Tab: return K_TAB;
    }
    return 0;
}

void gfx_poll(Gfx *g) {
    memset(g->pressed, 0, sizeof g->pressed);
    memset(g->mpressed, 0, sizeof g->mpressed);
    g->mdx = g->mdy = 0;
    while (XPending(g->dpy)) {
        XEvent e;
        XNextEvent(g->dpy, &e);
        switch (e.type) {
        case KeyPress: case KeyRelease: {
            int k = keycode(XLookupKeysym(&e.xkey, 0));
            if (k) {
                if (e.type == KeyPress && !g->key[k]) g->pressed[k] = 1;
                g->key[k] = e.type == KeyPress;
            }
        } break;
        case ButtonPress: case ButtonRelease:
            if (e.xbutton.button >= 1 && e.xbutton.button <= 3) {
                if (e.type == ButtonPress) g->mpressed[e.xbutton.button] = 1;
                g->mbtn[e.xbutton.button] = e.type == ButtonPress;
            }
            break;
        case MotionNotify:
            g->mdx += e.xmotion.x - g->mx;
            g->mdy += e.xmotion.y - g->my;
            g->mx = e.xmotion.x;
            g->my = e.xmotion.y;
            break;
        case ConfigureNotify:
            g->w = e.xconfigure.width;
            g->h = e.xconfigure.height;
            break;
        case ClientMessage:
            if ((Atom)e.xclient.data.l[0] == wmDelete) g->quit = 1;
            break;
        }
    }
}

void gfx_swap(Gfx *g) { glXSwapBuffers(g->dpy, g->win); }

static GLuint compile(const char *name, GLenum type, const char *src, int len) {
    static const char head[] = "#version 450 core\n";
    const char *parts[3] = {head, shader_common, src};
    GLint lens[3] = {(GLint)sizeof head - 1, shader_common_len, len};
    GLuint s = glCreateShader(type);
    glShaderSource(s, 3, parts, lens);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "shader %s (%s) failed:\n%s\n", name, type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
    }
    return s;
}

GLuint gfx_program(const char *name, const char *vs, int vsLen, const char *fs, int fsLen) {
    GLuint p = glCreateProgram();
    GLuint a = compile(name, GL_VERTEX_SHADER, vs, vsLen), b = compile(name, GL_FRAGMENT_SHADER, fs, fsLen);
    glAttachShader(p, a);
    glAttachShader(p, b);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "program %s link failed:\n%s\n", name, log);
    }
    glDeleteShader(a);
    glDeleteShader(b);
    return p;
}

GLuint gfx_tex(int w, int h, GLenum internal, GLenum filter) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexStorage2D(GL_TEXTURE_2D, 1, internal, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}
