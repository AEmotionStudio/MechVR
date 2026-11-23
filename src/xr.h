// OpenXR session, swapchains, frame loop and controller input.
#ifndef XR_H
#define XR_H
#include "gfx.h"
#include "mathx.h"

typedef struct {
    xf pose;                 // eye pose in play space
    float l, r, d, u;        // tangents of the fov half-angles
    GLuint tex;              // swapchain image to render into this frame
    int w, h;
} XrEye;

typedef struct {
    xf grip, aim;
    int tracked;
    float trigger, squeeze;
    float sx, sy;            // thumbstick
    int a, b;                // lower / upper face button (X/Y on the left hand, A/B on the right)
    int aDown, bDown;        // went down this frame
} XrHand;

typedef struct {
    int render;              // 0: the runtime does not want a picture this frame
    XrEye eye[2];
    xf head;
    XrHand hand[2];          // 0 left, 1 right
    int menu;
    double dt;               // predicted display period
} XrFrame;

int xr_init(Gfx *g);
int xr_poll(void);                 // returns 0 when the session has ended and the app should quit
int xr_running(void);
int xr_srgb(void);                 // 1 if the swapchain stores sRGB (write display-ready values directly)
void xr_begin(XrFrame *f);         // waits for the headset's frame timing
void xr_eye_acquire(XrFrame *f, int eye);
void xr_eye_release(int eye);
void xr_end(XrFrame *f);
void xr_haptic(int hand, float amplitude, float seconds);
void xr_shutdown(void);
#endif
