#include "mech.h"
#include "world.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

Shape shapes[S_COUNT];
v3 pnt[PNT_COUNT];
const int partShape[P_COUNT] = {S_PELVIS, S_TORSO, S_ARM_L, S_ARM_R, S_THIGH, S_SHIN, S_FOOT, S_THIGH, S_SHIN, S_FOOT};

enum { T_BOX, T_CYL, T_SPHERE, T_CAPSULE };

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
    {S_TORSO, S_PELVIS}, {S_ARM_L, S_TORSO}, {S_ARM_R, S_TORSO}, {S_THIGH, S_PELVIS}, {S_SHIN, S_THIGH}, {S_FOOT, S_SHIN},
    {S_CK_DASH, S_CK_SHELL}, {S_CK_SEAT, S_CK_SHELL}, {S_CK_SIDE_L, S_CK_SHELL}, {S_CK_SIDE_R, S_CK_SHELL},
    {S_CK_THROTTLE, S_CK_SHELL}, {S_CK_STICK, S_CK_SHELL}, {S_CK_EJECT, S_CK_SHELL},
    {S_CRAWLER_LEG, S_CRAWLER}, {S_TURRET_HEAD, S_TURRET_BASE},
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
    for (int i = 0; i < s->count; i++) {
        const float *f = s->prim[i].f;
        if (f[12] == 1 || f[12] == 3) continue;
        v3 e = V3(f[4], f[5], f[6]);
        if ((int)f[3] == T_CAPSULE) e.y += e.x;
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
            int ty = word("box cyl sphere capsule", kind), o = word("add sub smooth and", op);
            int m = word("armor armor2 frame accent glass glow screen rock hazard seat glove plasma cabin", mat);
            if (!cur) continue;
            if (ty < 0 || o < 0 || m < 0) { fprintf(stderr, "shapes: line %d: unknown %s\n", line, ty < 0 ? kind : o < 0 ? op : mat); continue; }
            if (cur->count == MAX_PRIMS) { fprintf(stderr, "shapes: line %d: more than %d pieces in one shape, skipped\n", line, MAX_PRIMS); continue; }
            const float D = PI / 180;
            quat q = qeuler(v[7] * D, v[8] * D, v[9] * D);
            float f[16] = {v[0], v[1], v[2], (float)ty, v[3], v[4], v[5], v[6], q.x, q.y, q.z, q.w, (float)o, v[10], (float)m, 0};
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
    long long stamp = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
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
        if (shape_ray(&shapes[partShape[i]], m->world[i], ro, rd, tmax, m->dent[i], m->ndent[i], &t, &l)) {
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
    for (int i = 0; i < P_COUNT; i++) m->hp[i] = i == P_TORSO ? 160 : 100;
    mech_pose(m, 0);
}

void mech_pose(Mech *m, float dt) {
    const float D = PI / 180;
    float amp = clampf(fabsf(m->speed) / 2.5f, 0, 1);
    m->phase += dt * m->speed * (2 * PI / 6.5f); // one full cycle covers 6.5 m
    const v3 X = V3(1, 0, 0), hip = shapes[S_THIGH].at, knee = shapes[S_SHIN].at, ankle = shapes[S_FOOT].at;
    float a1[2], a2[2], low = 1e9f;
    xf leg[2][3]; // thigh, shin and foot of each leg, measured from the hips
    for (int s = 0; s < 2; s++) {
        float ph = m->phase + s * PI, lift = maxf(0, cosf(ph)) * amp;
        a1[s] = 35 * D + 24 * D * amp * sinf(ph);
        a2[s] = -75 * D - 30 * D * lift;
        leg[s][0] = XF(V3(s ? -hip.x : hip.x, hip.y, hip.z), qaxis(X, a1[s])); // the right leg mirrors the left
        leg[s][1] = xfmul(leg[s][0], XF(knee, qaxis(X, a2[s])));
        leg[s][2] = xfmul(leg[s][1], XF(ankle, qaxis(X, -(a1[s] + a2[s]))));
        low = minf(low, xfpt(leg[s][2], pnt[PNT_FOOT_SOLE]).y);
    }
    m->hipH = -low; // the lower sole rests on the ground
    m->pos.y = world_height(m->pos.x, m->pos.z);
    xf root = XF(V3(m->pos.x, m->pos.y + m->hipH, m->pos.z),
                 qmul(qaxis(V3(0, 1, 0), m->yaw), qaxis(V3(0, 0, 1), 0.03f * amp * sinf(m->phase))));
    m->world[P_PELVIS] = root;
    xf torso = xfmul(root, XF(shapes[S_TORSO].at, qmul(qaxis(V3(0, 1, 0), m->torsoYaw), qaxis(V3(0, 0, 1), -0.02f * amp * sinf(m->phase)))));
    m->world[P_TORSO] = torso;
    m->world[P_ARM_L] = xfmul(torso, XF(shapes[S_ARM_L].at, qaxis(X, m->aimPitch + 0.03f * amp * cosf(m->phase))));
    m->world[P_ARM_R] = xfmul(torso, XF(shapes[S_ARM_R].at, qaxis(X, m->aimPitch - 0.03f * amp * cosf(m->phase))));
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
    const Shape *sh[7];
    float rad = 0, lo = 0, hi = 0;
    for (int r = 0; r <= IMP_PHASES; r++) { // walk cycle poses, then standing still
        Mech m;
        memset(&m, 0, sizeof m);
        m.phase = r * 2 * PI / IMP_PHASES;
        m.speed = r < IMP_PHASES ? 2.5f : 0;
        mech_pose(&m, 0);
        xf inv = xfinv(m.world[P_PELVIS]);
        for (int k = 0; k < 7; k++) {
            sh[k] = &shapes[partShape[LEGS[k]]];
            pose[r][k] = xfmul(inv, m.world[LEGS[k]]);
            grow(sh[k], pose[r][k], &rad, &lo, &hi);
        }
    }
    render_imp_layout(0, IMP_PHASES + 1, 2 * rad + 0.2f, lo - 0.1f, hi - lo + 0.2f, texel);
    for (int r = 0; r <= IMP_PHASES; r++) render_imp_bake(0, r, sh, pose[r], 7);

    xf up[3] = {XF(V3(0, 0, 0), qid()), XF(shapes[S_ARM_L].at, qid()), XF(shapes[S_ARM_R].at, qid())};
    const Shape *us[3] = {&shapes[S_TORSO], &shapes[S_ARM_L], &shapes[S_ARM_R]};
    rad = lo = hi = 0;
    for (int k = 0; k < 3; k++) grow(us[k], up[k], &rad, &lo, &hi);
    render_imp_layout(1, 1, 2 * rad + 0.2f, lo - 0.1f, hi - lo + 0.2f, texel);
    render_imp_bake(1, 0, us, up, 3);
}

int mech_imp_row(const Mech *m) {
    if (fabsf(m->speed) < 1.25f) return IMP_PHASES;
    return (int)floorf(m->phase / (2 * PI) * IMP_PHASES + 0.5f) & (IMP_PHASES - 1);
}

// ---- damage ---------------------------------------------------------------------------------------
int mech_damage(Mech *m, int part, v3 local, float dmg) {
    float max = part == P_TORSO ? 160.0f : 100.0f;
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
