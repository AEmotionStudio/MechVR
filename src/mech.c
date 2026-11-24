#include "mech.h"
#include "audio.h"
#include "world.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

Shape shapes[S_COUNT];
v3 pnt[PNT_COUNT];
// Bodies: the shapes for hips, torso, thigh, shin, foot, and the points on the torso and foot.
static const unsigned char chassisShape[CH_COUNT][5] = {
    {S_PELVIS, S_TORSO, S_THIGH, S_SHIN, S_FOOT}, {S_BIO_PELVIS, S_BIO_TORSO, S_BIO_THIGH, S_BIO_SHIN, S_BIO_FOOT}};
static const unsigned char chassisPoint[CH_COUNT][4] = {
    {PNT_TORSO_EYE, PNT_TORSO_AIM, PNT_TORSO_SHOULDER, PNT_FOOT_SOLE},
    {PNT_BIO_TORSO_EYE, PNT_BIO_TORSO_AIM, PNT_BIO_TORSO_SHOULDER, PNT_BIO_FOOT_SOLE}};
// Arms. bolt: 0 cannon round, 1 plasma, 3 spore (lobbed, bursts on landing), 4 bone needle.
const ArmType armType[ARM_COUNT] = {
    //  shape          muzzle                   side bolt shots snd      dmg speed interval spread range burst pause
    {S_ARM_L,       PNT_ARM_L_MUZZLE,       0, 0, 1, SND_CANNON, 6,  230, 0.11f, 0.004f, 230, 5, 3.0f},
    {S_ARM_R,       PNT_ARM_R_MUZZLE,       1, 1, 1, SND_PLASMA, 34, 120, 0.85f, 0.002f, 170, 1, 5.0f},
    {S_ARM_NEEDLER, PNT_ARM_NEEDLER_MUZZLE, 0, 4, 3, SND_PISTOL, 7,  190, 0.50f, 0.030f, 200, 2, 3.0f},
    {S_ARM_SPORE,   PNT_ARM_SPORE_MUZZLE,   0, 3, 1, SND_WHOOSH, 26, 55,  1.20f, 0.006f, 160, 1, 4.0f},
};
int mech_shape(const Mech *m, int part) {
    static const unsigned char slot[P_COUNT] = {0, 1, 0, 0, 2, 3, 4, 2, 3, 4};
    if (part == P_ARM_L || part == P_ARM_R) return armType[m->arm[part - P_ARM_L]].shape;
    return chassisShape[m->chassis][slot[part]];
}
int mech_flip(const Mech *m, int part) {
    return (part == P_ARM_L || part == P_ARM_R) && armType[m->arm[part - P_ARM_L]].side != part - P_ARM_L;
}
v3 mech_point(const Mech *m, int which) { return pnt[chassisPoint[m->chassis][which]]; }
v3 mech_muzzle(const Mech *m, int arm) {
    v3 p = pnt[armType[m->arm[arm]].muzzle];
    return mech_flip(m, P_ARM_L + arm) ? V3(-p.x, p.y, p.z) : p;
}
static const signed char cockpitShape[CH_COUNT][CKP_COUNT] = {
    {S_CK_SHELL, S_CK_DASH, S_CK_SEAT, S_CK_SIDE_L, S_CK_SIDE_R, S_CK_THROTTLE, S_CK_STICK, S_CK_EJECT, S_CK_STRUTS, S_CK_DIALS, -1},
    {S_CKB_SHELL, S_CKB_DASH, S_CKB_SEAT, S_CKB_SIDE_L, S_CKB_SIDE_R, S_CKB_THROTTLE, S_CKB_STICK, S_CKB_EJECT, S_CKB_RIBS, S_CKB_DIALS, S_CKB_TEETH}};
int mech_cockpit(const Mech *m, int piece) { return cockpitShape[m->chassis][piece]; }
v3 mech_grip(const Mech *m, int stick) {
    static const unsigned char grip[CH_COUNT][2] = {{PNT_CK_THROTTLE_GRIP, PNT_CK_STICK_GRIP}, {PNT_CKB_THROTTLE_GRIP, PNT_CKB_STICK_GRIP}};
    return pnt[grip[m->chassis][stick]];
}
int mech_card_layer(const Mech *m, int which) {
    return which < 2 ? m->chassis * 2 + which : CH_COUNT * 2 + m->arm[which - 2];
}

enum { T_BOX, T_CYL, T_SPHERE, T_CAPSULE, T_CONE };

// ---- shapes: read from assets/shapes.txt, which is embedded at build time (the format is described there) ----
extern const char shapes_txt[];
#define X(n) #n " "
static const char shapeNames[] = SHAPE_LIST(X);
#undef X
#define X(s, n) #s "." #n " "
static const char pointNames[] = POINT_LIST(X);
#undef X
// What the game expects each attached shape to be attached to.
static const unsigned char joints[][2] = {
    {S_TORSO, S_PELVIS}, {S_THIGH, S_PELVIS}, {S_SHIN, S_THIGH}, {S_FOOT, S_SHIN},
    {S_BIO_TORSO, S_BIO_PELVIS}, {S_BIO_THIGH, S_BIO_PELVIS}, {S_BIO_SHIN, S_BIO_THIGH}, {S_BIO_FOOT, S_BIO_SHIN},
    {S_CK_DASH, S_CK_SHELL}, {S_CK_SEAT, S_CK_SHELL}, {S_CK_SIDE_L, S_CK_SHELL}, {S_CK_SIDE_R, S_CK_SHELL},
    {S_CK_THROTTLE, S_CK_SHELL}, {S_CK_STICK, S_CK_SHELL}, {S_CK_EJECT, S_CK_SHELL},
    {S_CRAWLER_LEG, S_CRAWLER}, {S_TURRET_HEAD, S_TURRET_BASE},
    {S_CKB_DASH, S_CKB_SHELL}, {S_CKB_SEAT, S_CKB_SHELL}, {S_CKB_SIDE_L, S_CKB_SHELL}, {S_CKB_SIDE_R, S_CKB_SHELL},
    {S_CKB_THROTTLE, S_CKB_SHELL}, {S_CKB_STICK, S_CKB_SHELL}, {S_CKB_EJECT, S_CKB_SHELL}, {S_CKB_TEETH, S_CKB_SHELL},
    {S_CKB_RIBS, S_CKB_SHELL}, {S_CK_STRUTS, S_CK_SHELL}, {S_CK_DIALS, S_CK_SHELL}, {S_CKB_DIALS, S_CKB_SHELL},
};
static const char *livePath;
static long long liveStamp;

static int word(const char *list, const char *w) { // position of w in a space-separated list, or -1
    size_t n = strlen(w);
    for (int i = 0; *list; i++) {
        size_t len = strcspn(list, " ");
        if (len == n && !memcmp(list, w, n)) return i;
        list += len;
        while (*list == ' ') list++;
    }
    return -1;
}

static void finish(Shape *s) { // bounding box of everything that adds volume
    v3 lo = V3(1e9f, 1e9f, 1e9f), hi = V3(-1e9f, -1e9f, -1e9f);
    s->cone = s->bio = 0;
    for (int i = 0; i < s->count; i++) {
        const float *f = s->prim[i].f;
        if ((int)f[3] == T_CONE) s->cone = 1;
        if ((int)f[14] >= 13) s->bio = 1;
        if (f[12] == 1 || f[12] == 3) continue;
        v3 e = V3(f[4], f[5], f[6]);
        if ((int)f[3] == T_CAPSULE) e.y += e.x;
        if ((int)f[3] == T_CONE) { e.x = e.z = maxf(f[4], f[6]); e.y += e.x; }
        quat q = Q(f[8], f[9], f[10], f[11]);
        v3 x = vabs(qrot(q, V3(1, 0, 0))), y = vabs(qrot(q, V3(0, 1, 0))), z = vabs(qrot(q, V3(0, 0, 1)));
        if ((int)f[14] == 6) { // screen material: the shader maps its picture onto this piece's front face
            float scr[4] = {f[0], f[1], 0.5f / (e.x * x.x), 0.5f / (e.y * y.y)};
            memcpy(s->scr, scr, sizeof scr);
        }
        v3 h = vadd(vadd(vmul(x, e.x), vmul(y, e.y)), vmul(z, e.z));
        v3 c = V3(f[0], f[1], f[2]);
        lo = V3(minf(lo.x, c.x - h.x), minf(lo.y, c.y - h.y), minf(lo.z, c.z - h.z));
        hi = V3(maxf(hi.x, c.x + h.x), maxf(hi.y, c.y + h.y), maxf(hi.z, c.z + h.z));
    }
    if (lo.x > hi.x) lo = hi = V3(0, 0, 0);
    s->boxC = vmul(vadd(lo, hi), 0.5f);
    s->boxH = vadd(vmul(vsub(hi, lo), 0.5f), V3(0.02f, 0.02f, 0.02f));
    s->radius = vlen(vadd(vabs(s->boxC), s->boxH));
}

static void shapes_parse(const char *t) {
    Shape *cur = NULL; // a shape or point the text does not mention keeps what it had
    char curName[32] = "", on[32];
    for (int line = 1; *t; line++) {
        char buf[200], kind[16], op[16], mat[16], name[32];
        float v[11];
        size_t n = strcspn(t, "\n"), use = strcspn(t, "#\n");
        if (use > sizeof buf - 1) use = sizeof buf - 1;
        memcpy(buf, t, use);
        buf[use] = 0;
        t += n + (t[n] == '\n');
        if (sscanf(buf, " shape %31s", name) == 1) {
            int id = word(shapeNames, name);
            cur = id < 0 ? NULL : &shapes[id];
            strcpy(curName, name);
            if (id < 0) { fprintf(stderr, "shapes: line %d: the game has no shape called %s, skipped\n", line, name); continue; }
            cur->count = 0;
            cur->at = V3(0, 0, 0);
            cur->on = -1;
            int got = sscanf(buf, " shape %*s at %f %f %f on %31s", v, v + 1, v + 2, on);
            if (got >= 3) cur->at = V3(v[0], v[1], v[2]);
            if (got == 4 && (cur->on = word(shapeNames, on)) < 0) fprintf(stderr, "shapes: line %d: %s is on %s, which the game does not have\n", line, name, on);
        } else if (sscanf(buf, " point %31s %f %f %f", name, v, v + 1, v + 2) == 4) {
            char key[64];
            snprintf(key, sizeof key, "%s.%s", curName, name);
            int id = word(pointNames, key);
            if (id >= 0) pnt[id] = V3(v[0], v[1], v[2]);
            else if (cur) fprintf(stderr, "shapes: line %d: the game does not use a point called %s on %s\n", line, name, curName);
        } else if (sscanf(buf, " %15s %f %f %f %f %f %f %f %f %f %f %15s %f %15s", kind, v, v + 1, v + 2, v + 3, v + 4, v + 5, v + 6, v + 7,
                          v + 8, v + 9, op, v + 10, mat) == 14) {
            int ty = word("box cyl sphere capsule cone", kind), o = word("add sub smooth and", op);
            int m = word("armor armor2 frame accent glass glow screen rock hazard seat glove plasma cabin flesh bone sac", mat);
            if (!cur) continue;
            if (ty < 0 || o < 0 || m < 0) { fprintf(stderr, "shapes: line %d: unknown %s\n", line, ty < 0 ? kind : o < 0 ? op : mat); continue; }
            if (cur->count == MAX_PRIMS) { fprintf(stderr, "shapes: line %d: more than %d pieces in one shape, skipped\n", line, MAX_PRIMS); continue; }
            const float D = PI / 180;
            quat q = qeuler(v[7] * D, v[8] * D, v[9] * D);
            float f[16] = {v[0], v[1], v[2], (float)ty, v[3], v[4], v[5], v[6], q.x, q.y, q.z, q.w, (float)o, v[10], (float)m, 0};
            if (ty == T_CONE) { // its slope, worked out once here so the shader does not have to
                f[15] = (v[3] - v[5]) / (2 * v[4]);
                f[7] = sqrtf(maxf(1 - f[15] * f[15], 0));
            }
            memcpy(cur->prim[cur->count++].f, f, sizeof f);
        } else if (buf[strspn(buf, " \t\r")]) fprintf(stderr, "shapes: line %d: cannot read \"%s\"\n", line, buf);
    }
    const char *nm = shapeNames;
    for (int i = 0; i < S_COUNT; i++, nm += strcspn(nm, " ") + 1) {
        if (!shapes[i].count) fprintf(stderr, "shapes: %.*s has no pieces\n", (int)strcspn(nm, " "), nm);
        finish(&shapes[i]);
    }
    for (size_t j = 0; j < sizeof joints / sizeof joints[0]; j++) { // a joint measured from the wrong shape would put the part in the wrong place
        const char *a = shapeNames, *b = shapeNames;
        if (shapes[joints[j][0]].on == joints[j][1]) continue;
        for (int i = 0; i < joints[j][0]; i++) a += strcspn(a, " ") + 1;
        for (int i = 0; i < joints[j][1]; i++) b += strcspn(b, " ") + 1;
        fprintf(stderr, "shapes: %.*s must be \"on %.*s\"\n", (int)strcspn(a, " "), a, (int)strcspn(b, " "), b);
    }
}

void mech_shapes_live(const char *path) { livePath = path; }

// With --shapes FILE the file is read again whenever it changes, so edits show up while the game runs.
int mech_shapes_poll(void) {
    static char buf[1 << 16];
    struct stat st;
    if (!livePath || stat(livePath, &st)) return 0;
#ifdef _WIN32 // Windows' stat has whole seconds only, so the size is mixed in to catch two saves within a second
    long long stamp = (long long)st.st_mtime * 1000000007LL + st.st_size;
#else
    long long stamp = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
    if (stamp == liveStamp) return 0;
    FILE *f = fopen(livePath, "rb");
    if (!f) return 0;
    buf[fread(buf, 1, sizeof buf - 1, f)] = 0;
    fclose(f);
    shapes_parse(buf);
    printf("shapes: %s %s\n", liveStamp ? "reloaded" : "loaded", livePath);
    liveStamp = stamp;
    return 1;
}

void mech_shapes_init(void) {
    shapes_parse(shapes_txt);
    mech_shapes_poll();
}

// ---- distance to a shape on the CPU (mirrors the shader), used so shots land exactly on the surface ---
static float primDist(const float *f, v3 p) {
    v3 l = vsub(p, V3(f[0], f[1], f[2]));
    if (f[11] < 0.99999f) l = qrot(Q(-f[8], -f[9], -f[10], f[11]), l);
    switch ((int)f[3]) {
    case T_BOX: {
        v3 d = vsub(vabs(l), V3(f[4] - f[7], f[5] - f[7], f[6] - f[7]));
        return vlen(vmaxf(d, 0)) + minf(maxf(d.x, maxf(d.y, d.z)), 0) - f[7];
    }
    case T_CYL: {
        float dx = sqrtf(l.x * l.x + l.z * l.z) - (f[4] - f[7]), dy = fabsf(l.y) - (f[5] - f[7]);
        float ox = maxf(dx, 0), oy = maxf(dy, 0);
        return minf(maxf(dx, dy), 0) + sqrtf(ox * ox + oy * oy) - f[7];
    }
    case T_SPHERE: return vlen(l) - f[4];
    case T_CONE: { // mirrors the shader: radius f[4] at the bottom end, f[6] at the top end
        float h = 2 * f[5], k1 = f[15], k2 = f[7];
        float qx = sqrtf(l.x * l.x + l.z * l.z), qy = l.y + f[5], k = -qx * k1 + qy * k2;
        if (k < 0) return sqrtf(qx * qx + qy * qy) - f[4];
        if (k > k2 * h) return sqrtf(qx * qx + (qy - h) * (qy - h)) - f[6];
        return qx * k2 + qy * k1 - f[4];
    }
    default: l.y -= clampf(l.y, -f[5], f[5]); return vlen(l) - f[4];
    }
}
static float shapeDist(const Shape *s, v3 p, const float (*dent)[4], int ndent) {
    float d = 1e9f;
    for (int i = 0; i < s->count; i++) {
        const float *f = s->prim[i].f;
        float di = primDist(f, p);
        int op = (int)f[12];
        if (op == 0) d = minf(d, di);
        else if (op == 1) d = maxf(d, -di);
        else if (op == 2) {
            float h = clampf(0.5f + 0.5f * (d - di) / f[13], 0, 1);
            d = mixf(d, di, h) - f[13] * h * (1 - h);
        } else d = maxf(d, di);
    }
    for (int j = 0; j < ndent; j++) d = maxf(d, -(vlen(vsub(p, V3(dent[j][0], dent[j][1], dent[j][2]))) - dent[j][3] * 0.84f));
    return d;
}

int shape_ray(const Shape *s, xf x, v3 ro, v3 rd, float tmax, const float (*dent)[4], int ndent, float *tOut, v3 *local) {
    xf inv = xfinv(x);
    v3 o = xfpt(inv, ro), d = qrot(inv.q, rd);
    float t0 = 0, t1 = tmax;
    const float oc[3] = {o.x - s->boxC.x, o.y - s->boxC.y, o.z - s->boxC.z}, dd[3] = {d.x, d.y, d.z};
    const float hh[3] = {s->boxH.x, s->boxH.y, s->boxH.z};
    for (int a = 0; a < 3; a++) {
        if (fabsf(dd[a]) < 1e-7f) { if (fabsf(oc[a]) > hh[a]) return 0; continue; }
        float ta = (-hh[a] - oc[a]) / dd[a], tb = (hh[a] - oc[a]) / dd[a];
        if (ta > tb) { float tt = ta; ta = tb; tb = tt; }
        t0 = maxf(t0, ta);
        t1 = minf(t1, tb);
        if (t0 > t1) return 0;
    }
    float t = t0;
    for (int i = 0; i < 64 && t <= t1; i++) {
        v3 p = vadd(o, vmul(d, t));
        float dist = shapeDist(s, p, dent, ndent);
        if (dist < 0.01f) { *tOut = t; *local = p; return 1; }
        t += dist;
    }
    return 0;
}

int mech_ray(const Mech *m, v3 ro, v3 rd, float tmax, float *tOut, int *part, v3 *local) {
    int hit = 0;
    for (int i = 0; i < P_COUNT; i++) {
        if (m->gone[i]) continue;
        float t;
        v3 l;
        xf x = m->world[i];
        v3 o = ro, d = rd;
        if (mech_flip(m, i)) { // a mirrored part: mirror the ray into the shape's own space instead
            xf inv = xfinv(x);
            o = xfpt(inv, ro);
            d = qrot(inv.q, rd);
            o.x = -o.x;
            d.x = -d.x;
            x = XF(V3(0, 0, 0), qid());
        }
        if (shape_ray(&shapes[mech_shape(m, i)], x, o, d, tmax, m->dent[i], m->ndent[i], &t, &l)) {
            tmax = t; *tOut = t; *part = i; *local = l; hit = 1;
        }
    }
    return hit;
}

// ---- skeleton and walk cycle -------------------------------------------------------------------
void mech_init(Mech *m, v3 pos, float yaw, int team) {
    memset(m, 0, sizeof *m);
    m->pos = pos;
    m->yaw = yaw;
    m->team = team;
    m->alive = 1;
    m->seed = frand() * 40;
    m->phase = frand() * 6;
    for (int i = 0; i < P_COUNT; i++) m->hp[i] = PART_HP(i);
    m->arm[0] = ARM_CANNON;
    m->arm[1] = ARM_PLASMA;
    mech_pose(m, 0);
}
void mech_outfit(Mech *m, int chassis, int armL, int armR) {
    m->chassis = chassis;
    m->arm[0] = armL;
    m->arm[1] = armR;
    mech_pose(m, 0);
}

void mech_pose(Mech *m, float dt) {
    const float D = PI / 180;
    float amp = clampf(fabsf(m->speed) / 2.5f, 0, 1);
    m->phase += dt * m->speed * (2 * PI / 6.5f); // one full cycle covers 6.5 m
    const unsigned char *cs = chassisShape[m->chassis];
    const v3 X = V3(1, 0, 0), hip = shapes[cs[2]].at, knee = shapes[cs[3]].at, ankle = shapes[cs[4]].at;
    const v3 sole = mech_point(m, MP_SOLE), sh = mech_point(m, MP_SHOULDER);
    float a1[2], a2[2], low = 1e9f;
    xf leg[2][3]; // thigh, shin and foot of each leg, measured from the hips
    for (int s = 0; s < 2; s++) {
        float ph = m->phase + s * PI, lift = maxf(0, cosf(ph)) * amp;
        a1[s] = 35 * D + 24 * D * amp * sinf(ph);
        a2[s] = -75 * D - 30 * D * lift;
        leg[s][0] = XF(V3(s ? -hip.x : hip.x, hip.y, hip.z), qaxis(X, a1[s])); // the right leg mirrors the left
        leg[s][1] = xfmul(leg[s][0], XF(knee, qaxis(X, a2[s])));
        leg[s][2] = xfmul(leg[s][1], XF(ankle, qaxis(X, -(a1[s] + a2[s]))));
        low = minf(low, xfpt(leg[s][2], sole).y);
    }
    m->hipH = -low; // the lower sole rests on the ground
    m->pos.y = world_height(m->pos.x, m->pos.z);
    xf root = XF(V3(m->pos.x, m->pos.y + m->hipH + m->drop, m->pos.z),
                 qmul(qaxis(V3(0, 1, 0), m->yaw), qaxis(V3(0, 0, 1), 0.03f * amp * sinf(m->phase))));
    m->world[P_PELVIS] = root;
    xf torso = xfmul(root, XF(shapes[cs[1]].at, qmul(qaxis(V3(0, 1, 0), m->torsoYaw), qaxis(V3(0, 0, 1), -0.02f * amp * sinf(m->phase)))));
    m->world[P_TORSO] = torso;
    m->world[P_ARM_L] = xfmul(torso, XF(sh, qaxis(X, m->aimPitch + 0.03f * amp * cosf(m->phase))));
    m->world[P_ARM_R] = xfmul(torso, XF(V3(-sh.x, sh.y, sh.z), qaxis(X, m->aimPitch - 0.03f * amp * cosf(m->phase)))); // the right shoulder mirrors the left
    for (int s = 0; s < 2; s++)
        for (int k = 0; k < 3; k++) m->world[(s ? P_THIGH_R : P_THIGH_L) + k] = xfmul(root, leg[s][k]);
}

// ---- distant mechs as flat cards ---------------------------------------------------------------------
// Each sheet row is one pose seen from all around. Sizes come from the shapes, so edited mechs still fit.
static void grow(const Shape *s, xf x, float *rad, float *lo, float *hi) {
    for (int c = 0; c < 8; c++) {
        v3 p = xfpt(x, V3(s->boxC.x + (c & 1 ? s->boxH.x : -s->boxH.x), s->boxC.y + (c & 2 ? s->boxH.y : -s->boxH.y),
                          s->boxC.z + (c & 4 ? s->boxH.z : -s->boxH.z)));
        *rad = maxf(*rad, sqrtf(p.x * p.x + p.z * p.z));
        *lo = minf(*lo, p.y);
        *hi = maxf(*hi, p.y);
    }
}

void mech_impostors_bake(void) {
    static const unsigned char LEGS[7] = {P_PELVIS, P_THIGH_L, P_SHIN_L, P_FOOT_L, P_THIGH_R, P_SHIN_R, P_FOOT_R};
    const float texel = 0.1f; // metres per pixel on the largest sheet: a little finer than one screen pixel at IMP_DIST
    static xf pose[IMP_PHASES + 1][7];
    const xf here = XF(V3(0, 0, 0), qid());
    for (int c = 0; c < CH_COUNT; c++) { // each body: a sheet of leg poses and a sheet for the torso
        const Shape *sh[7];
        float rad = 0, lo = 0, hi = 0;
        for (int r = 0; r <= IMP_PHASES; r++) { // walk cycle poses, then standing still
            Mech m;
            memset(&m, 0, sizeof m);
            m.chassis = c;
            m.phase = r * 2 * PI / IMP_PHASES;
            m.speed = r < IMP_PHASES ? 2.5f : 0;
            mech_pose(&m, 0);
            xf inv = xfinv(m.world[P_PELVIS]);
            for (int k = 0; k < 7; k++) {
                sh[k] = &shapes[mech_shape(&m, LEGS[k])];
                pose[r][k] = xfmul(inv, m.world[LEGS[k]]);
                grow(sh[k], pose[r][k], &rad, &lo, &hi);
            }
        }
        render_imp_layout(c * 2, IMP_PHASES + 1, 2 * rad + 0.2f, lo - 0.1f, hi - lo + 0.2f, texel);
        for (int r = 0; r <= IMP_PHASES; r++) render_imp_bake(c * 2, r, sh, pose[r], 7);
        const Shape *torso = &shapes[chassisShape[c][1]];
        rad = lo = hi = 0;
        grow(torso, here, &rad, &lo, &hi);
        render_imp_layout(c * 2 + 1, 1, 2 * rad + 0.2f, lo - 0.1f, hi - lo + 0.2f, texel);
        render_imp_bake(c * 2 + 1, 0, &torso, &here, 1);
    }
    for (int t = 0; t < ARM_COUNT; t++) { // each kind of arm: one sheet, shown mirrored on the other side
        const Shape *arm = &shapes[armType[t].shape];
        float rad = 0, lo = 0, hi = 0;
        grow(arm, here, &rad, &lo, &hi);
        render_imp_layout(CH_COUNT * 2 + t, 1, 2 * rad + 0.2f, lo - 0.1f, hi - lo + 0.2f, texel);
        render_imp_bake(CH_COUNT * 2 + t, 0, &arm, &here, 1);
    }
}

int mech_imp_row(const Mech *m) {
    if (fabsf(m->speed) < 1.25f) return IMP_PHASES;
    return (int)floorf(m->phase / (2 * PI) * IMP_PHASES + 0.5f) & (IMP_PHASES - 1);
}

// ---- damage ---------------------------------------------------------------------------------------
int mech_damage(Mech *m, int part, v3 local, float dmg) {
    float max = PART_HP(part);
    m->hp[part] -= dmg;
    m->flash[part] = 1;
    float (*d)[4] = m->dent[part];
    int n = m->ndent[part], best = -1;
    for (int i = 0; i < n; i++)
        if (vlen(vsub(local, V3(d[i][0], d[i][1], d[i][2]))) < d[i][3] * 0.9f) best = i;
    if (best >= 0) { // hitting an existing hole widens it
        d[best][3] = minf(d[best][3] + 0.006f * dmg, 0.7f);
        for (int a = 0; a < 3; a++) d[best][a] = mixf(d[best][a], (&local.x)[a], 0.25f);
    } else {
        if (n < 8) best = m->ndent[part]++;
        else { // reuse the smallest hole
            best = 0;
            for (int i = 1; i < 8; i++) if (d[i][3] < d[best][3]) best = i;
        }
        d[best][0] = local.x; d[best][1] = local.y; d[best][2] = local.z;
        d[best][3] = 0.13f + 0.011f * dmg;
    }
    m->heat[part][best] = 1;
    float f = m->hp[part] / max;
    int st = f > 0.66f ? 0 : f > 0.33f ? 1 : f > 0 ? 2 : 3;
    if (st != m->stage[part]) { m->stage[part] = st; return st; }
    return -1;
}

int mech_repair(Mech *m, int part, v3 local, float hp) {
    float max = PART_HP(part), missing = max - m->hp[part], add = minf(hp, maxf(missing, 0)), holes = 0;
    if (m->gone[part]) return -1;
    float (*d)[4] = m->dent[part];
    int best = -1;
    for (int i = 0; i < m->ndent[part]; i++) {
        holes += d[i][3];
        if (best < 0 || vlen(vsub(local, V3(d[i][0], d[i][1], d[i][2]))) - d[i][3] <
                            vlen(vsub(local, V3(d[best][0], d[best][1], d[best][2]))) - d[best][3]) best = i;
    }
    m->hp[part] += add;
    if (best >= 0) { // the nearest hole closes, glowing at the edges while it is worked on
        d[best][3] -= missing > 0.5f ? holes * add / missing : hp * 0.006f;
        m->heat[part][best] = maxf(m->heat[part][best], 0.4f);
        if (d[best][3] < 0.05f) { // closed
            int last = --m->ndent[part];
            memcpy(d[best], d[last], sizeof d[best]);
            m->heat[part][best] = m->heat[part][last];
        }
    }
    float f = m->hp[part] / max;
    int st = f > 0.66f ? 0 : f > 0.33f ? 1 : 2;
    if (st < m->stage[part]) { m->stage[part] = st; return st; }
    return -1;
}
