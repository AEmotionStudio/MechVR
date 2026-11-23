// Scene description handed to the renderer each frame, and the renderer itself.
#ifndef RENDER_H
#define RENDER_H
#include "gfx.h"
#include "mathx.h"

#define MAX_PRIMS 16
typedef struct { float f[16]; } Prim; // center+type, half size+rounding, rotation, op+blend+material
typedef struct {
    Prim prim[MAX_PRIMS];
    int count;
    v3 boxC, boxH; // bounding box in part space
    float radius;  // bounding sphere radius around the part origin
    v3 at;         // where the shape's origin sits on the shape it is attached to (its joint)
    int on;        // that shape, or -1
    float scr[4];  // its screen piece, if it has one: center xy and 1/size
    int cone;      // it has cone pieces, which need the EXTRA build of the part shader
    int bio;       // it has flesh, bone or sac pieces: a living part
} Shape;

typedef struct {
    const Shape *shape;
    xf world;
    v3 tintA, tintB;
    int stage, ndent;
    float dent[8][4];
    float heat[8];
    float seed, flash, texel;
    int interior; // lit by cabin light instead of the sun, no distance haze
    float aux[4], stat[12], scr[4];
    float scale; // draw the shape this many times larger; 0 means 1
    int flip;    // draw it mirrored left to right (an arm made for the other side of the body)
} DrawPart;

typedef struct { float x, y, z, size, age, type, seed, spin; } DrawParticle;

// A distant mech drawn as flat cards: one for the legs, one for the torso, one per arm. layer = which sheet, row = which pose.
#define IMP_YAWS 32 // views around the mech on the sheet
#define IMP_LODS 4  // each sheet is kept at this many sizes, halving each time
#define IMP_LAYERS 16
typedef struct { v3 pos; float yaw; int layer, row, flip; v3 tintA, tintB; float flash; } DrawImp; // flip: shown mirrored

#define MAX_DRAW_PARTS 2048
#define MAX_DRAW_IMP 600
#define MAX_DRAW_PTC 4096
#define MAX_SHADOW 48
typedef struct {
    DrawPart part[MAX_DRAW_PARTS];
    int npart;
    DrawImp imp[MAX_DRAW_IMP];
    int nimp;
    DrawParticle ptc[MAX_DRAW_PTC];
    int nptc;
    float shadow[MAX_SHADOW][4];
    int nshadow;
    v3 head;
    float time;
    int look;
    float bulge[4]; // a mound in the sand: x, z, height, radius
    float vignette, fade;
    v3 fadeCol;
} Scene;

typedef struct { xf cam; float l, r, d, u; } View;

void render_init(const float *height, int hn);
// Sheets of distant-mech cards. layout sets a layer's size (rows = poses; width, bottom and height of one card in
// metres; texel = metres per sheet pixel). bake draws one pose, seen from IMP_YAWS directions, from parts placed at x.
void render_imp_layout(int layer, int rows, float width, float y0, float height, float texel);
void render_imp_bake(int layer, int row, const Shape *const *shape, const xf *x, int n);
// Draws the scene into dstTex (w x h). linearize: 1 if the target expects linear values.
void render_view(const Scene *s, const View *v, GLuint dstTex, int w, int h, int linearize);
#endif
