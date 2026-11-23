// Small vector / quaternion / matrix helpers. Column-major 4x4, right-handed, -Z forward (OpenXR/GL).
#ifndef MATHX_H
#define MATHX_H
#include <math.h>

typedef struct { float x, y, z; } v3;
typedef struct { float x, y, z, w; } quat;
typedef struct { float m[16]; } m4;
typedef struct { v3 p; quat q; } xf; // rigid transform

#define PI 3.14159265358979f
static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float mixf(float a, float b, float t) { return a + (b - a) * t; }
static inline float minf(float a, float b) { return a < b ? a : b; }
static inline float maxf(float a, float b) { return a > b ? a : b; }
static inline float smooth01(float t) { t = clampf(t, 0, 1); return t * t * (3 - 2 * t); }

static inline v3 V3(float x, float y, float z) { v3 r = {x, y, z}; return r; }
static inline v3 vadd(v3 a, v3 b) { return V3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline v3 vsub(v3 a, v3 b) { return V3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline v3 vmul(v3 a, float s) { return V3(a.x * s, a.y * s, a.z * s); }
static inline v3 vmulv(v3 a, v3 b) { return V3(a.x * b.x, a.y * b.y, a.z * b.z); }
static inline float vdot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline v3 vcross(v3 a, v3 b) { return V3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline float vlen(v3 a) { return sqrtf(vdot(a, a)); }
static inline v3 vnorm(v3 a) { float l = vlen(a); return l > 1e-8f ? vmul(a, 1.0f / l) : V3(0, 0, 0); }
static inline v3 vlerp(v3 a, v3 b, float t) { return vadd(a, vmul(vsub(b, a), t)); }
static inline v3 vabs(v3 a) { return V3(fabsf(a.x), fabsf(a.y), fabsf(a.z)); }
static inline v3 vmaxf(v3 a, float f) { return V3(maxf(a.x, f), maxf(a.y, f), maxf(a.z, f)); }

static inline quat Q(float x, float y, float z, float w) { quat r = {x, y, z, w}; return r; }
static inline quat qid(void) { return Q(0, 0, 0, 1); }
static inline quat qaxis(v3 axis, float ang) {
    float s = sinf(ang * 0.5f);
    return Q(axis.x * s, axis.y * s, axis.z * s, cosf(ang * 0.5f));
}
static inline quat qmul(quat a, quat b) {
    return Q(a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
             a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);
}
static inline quat qconj(quat q) { return Q(-q.x, -q.y, -q.z, q.w); }
static inline quat qnorm(quat q) {
    float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return l > 1e-8f ? Q(q.x / l, q.y / l, q.z / l, q.w / l) : qid();
}
static inline v3 qrot(quat q, v3 v) {
    v3 u = V3(q.x, q.y, q.z);
    v3 t = vmul(vcross(u, v), 2.0f);
    return vadd(vadd(v, vmul(t, q.w)), vcross(u, t));
}
static inline quat qeuler(float pitch, float yaw, float roll) { // yaw(Y) * pitch(X) * roll(Z)
    return qmul(qmul(qaxis(V3(0, 1, 0), yaw), qaxis(V3(1, 0, 0), pitch)), qaxis(V3(0, 0, 1), roll));
}
static inline float qyaw(quat q) { // heading of the -Z axis around Y
    v3 f = qrot(q, V3(0, 0, -1));
    return atan2f(-f.x, -f.z);
}

static inline xf XF(v3 p, quat q) { xf r = {p, q}; return r; }
static inline xf xfid(void) { return XF(V3(0, 0, 0), qid()); }
static inline xf xfmul(xf a, xf b) { return XF(vadd(a.p, qrot(a.q, b.p)), qmul(a.q, b.q)); } // a then-applied-to b (parent * child)
static inline v3 xfpt(xf a, v3 p) { return vadd(a.p, qrot(a.q, p)); }
static inline xf xfinv(xf a) { quat c = qconj(a.q); return XF(vmul(qrot(c, a.p), -1), c); }

static inline m4 m4id(void) { m4 r = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}; return r; }
static inline m4 m4mul(m4 a, m4 b) {
    m4 r;
    for (int c = 0; c < 4; c++)
        for (int w = 0; w < 4; w++)
            r.m[c * 4 + w] = a.m[0 * 4 + w] * b.m[c * 4 + 0] + a.m[1 * 4 + w] * b.m[c * 4 + 1] +
                             a.m[2 * 4 + w] * b.m[c * 4 + 2] + a.m[3 * 4 + w] * b.m[c * 4 + 3];
    return r;
}
static inline m4 m4xf(xf t) {
    v3 x = qrot(t.q, V3(1, 0, 0)), y = qrot(t.q, V3(0, 1, 0)), z = qrot(t.q, V3(0, 0, 1));
    m4 r = {{x.x, x.y, x.z, 0, y.x, y.y, y.z, 0, z.x, z.y, z.z, 0, t.p.x, t.p.y, t.p.z, 1}};
    return r;
}
// Projection from tangent half-angles (OpenXR asymmetric fov), GL clip space.
static inline m4 m4proj(float l, float r, float d, float u, float n, float f) {
    m4 p = {{0}};
    p.m[0] = 2 / (r - l);
    p.m[5] = 2 / (u - d);
    p.m[8] = (r + l) / (r - l);
    p.m[9] = (u + d) / (u - d);
    p.m[10] = -(f + n) / (f - n);
    p.m[11] = -1;
    p.m[14] = -(2 * f * n) / (f - n);
    return p;
}
#endif
