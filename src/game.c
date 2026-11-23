#include "game.h"
#include <stdio.h>
#include "audio.h"
#include "mech.h"
#include "world.h"
#include <string.h>

int game_look = 0;

typedef struct { v3 p, v; float age, life, size, grow, grav, drag, seed, spin; int type; } Particle;
typedef struct { int active, owner, kind; v3 p, v; float life, dmg; } Bolt; // kind: 0 cannon tracer, 1 plasma, 2 crawler spit
typedef struct {
    int active, shape, team, ndent, rest;
    xf x;
    v3 v, w;
    float dent[8][4], heat[8], seed, age, radius, emit;
} Debris;
typedef struct { float t; int mech, part, rock, ground; v3 local; int bot; } Hit; // bot: index of a small enemy, or -1
enum { PT_SMOKE, PT_FIRE, PT_SPARK, PT_FLASH, PT_DUST, PT_RETICLE };
enum { MODE_FOOT, MODE_PILOT, MODE_EJECT };
enum { GRAB_NONE, GRAB_THROTTLE, GRAB_STICK, GRAB_EJECT };

#define NMECH 133 // 5 in the normal game; the rest are for --horde
#define MAXPTC 3000
#define MAXBOLT 128
#define MAXDEB 64
#define TOP_SPEED 7.5f
static Mech mech[NMECH];
static int nmech = 5, horde, swarm;
int game_cards = 1; // draw distant mechs as flat cards
int game_calm;      // test option: 1 = nobody attacks, 2 = enemy mechs do not attack but the small enemies do
static Particle ptc[MAXPTC];
static int nptc;
static Bolt bolt[MAXBOLT];
static Debris deb[MAXDEB];
static int debNext, demoLevel, footStart;
static float gtime, hapL, hapR;
static xf rigNow;
static struct {
    int mode, fly, mech;
    v3 pos;              // on foot: where the play-space floor origin is in the world
    float yaw;
    float cool, moving;
    int snapHeld;
    xf calib;            // head position and heading captured when you sat down
    float footLift;      // raises a seated player to standing eye height on foot
    float throttle, stickX, stickY, ejectPull, bob;
    int grab[2], sqPrev[2], barrel;
    v3 grabStart[2];
    float hp, hurt, fade, fadeDir;
    int pendingBoard, dead;
    v3 ejV, aim;
    float aimDist, alarm;
    float wornTime;      // seconds the headset has been worn; the seat is fitted to you shortly after
    int fitted;
} pl;

static const v3 TINT[3][2] = {
    {{0.44f, 0.52f, 0.30f}, {0.84f, 0.80f, 0.66f}}, // player: olive and cream
    {{0.66f, 0.17f, 0.15f}, {0.27f, 0.26f, 0.31f}}, // enemy: crimson and charcoal
    {{0.42f, 0.55f, 0.64f}, {0.86f, 0.86f, 0.84f}}, // spare: steel blue and white
};
static const v3 WIND = {1.3f, 0, 0.5f};
static const v3 UP = {0, 1, 0};

static v3 yawFwd(float yaw) { return V3(-sinf(yaw), 0, -cosf(yaw)); }
// Gloves sit at the palm (grip pose) but point where the controller points (aim pose). The grip pose's own
// forward axis runs up through the fist, which made the gloves point at the ceiling.
static xf hand_xf(xf rig, const XrHand *h) { return xfmul(rig, XF(h->grip.p, h->aim.q)); }
// The pistol is placed from the hand, so its handle sits inside the fist whatever the controller model.
static xf pistol_xf(xf rig, const XrHand *h) { return xfmul(rig, XF(vadd(h->grip.p, qrot(h->aim.q, V3(0, 0.06f, -0.03f))), h->aim.q)); }
static quat qlook(v3 d) { return qeuler(asinf(clampf(d.y, -1, 1)), atan2f(-d.x, -d.z), 0); }
static float wrap(float a) { while (a > PI) a -= 2 * PI; while (a < -PI) a += 2 * PI; return a; }
static float approach(float v, float target, float step) { return v + clampf(target - v, -step, step); }

// ---- particles -----------------------------------------------------------------------------------
static void spawn(int type, v3 p, v3 v, float life, float size, float grow, float grav, float drag) {
    if (nptc >= MAXPTC) return;
    Particle *q = &ptc[nptc++];
    q->type = type; q->p = p; q->v = v; q->age = 0; q->life = life; q->size = size; q->grow = grow;
    q->grav = grav; q->drag = drag; q->seed = frand(); q->spin = frand() * 6.28f;
}
static v3 rv(float s) { return V3(frands() * s, frands() * s, frands() * s); }
static void smoke(v3 p, float size) {
    spawn(PT_SMOKE, vadd(p, rv(size * 0.3f)), vadd(V3(WIND.x, 2.2f + frand(), WIND.z), rv(0.4f)), 2.4f + frand(), size, size * 0.5f, 0.4f, 0.3f);
}
static void fire(v3 p, float size) {
    spawn(PT_FIRE, vadd(p, rv(size * 0.4f)), vadd(V3(0.4f, 2.6f + frand() * 1.5f, 0.2f), rv(0.5f)), 0.45f + frand() * 0.35f, size, -size * 0.6f, 1.5f, 0.5f);
}
static void sparks(v3 p, v3 n, int count, float speed, float size) {
    for (int i = 0; i < count; i++)
        spawn(PT_SPARK, p, vadd(vmul(n, speed * (0.4f + frand())), rv(speed * 0.6f)), 0.3f + frand() * 0.5f, size * (0.6f + frand()), 0, -9.8f, 0.6f);
}
static void burst(v3 p, float scale) { // explosion
    spawn(PT_FLASH, p, V3(0, 0, 0), 0.16f, 3.2f * scale, 0, 0, 0);
    for (int i = 0; i < (int)(16 * scale); i++) fire(vadd(p, rv(scale)), 1.1f * scale);
    for (int i = 0; i < (int)(7 * scale); i++) smoke(vadd(p, rv(scale * 1.2f)), 0.8f * scale);
    sparks(p, UP, (int)(24 * scale), 9, 0.07f);
    audio_play(SND_BOOM, p, 0.9f * scale);
}

// ---- cockpit placement ---------------------------------------------------------------------------
// The cabin follows the mech's position and heading only (never its lean), and only part of its bounce.
static xf cockpit_anchor(const Mech *m) {
    quat q = qaxis(UP, m->yaw + m->torsoYaw);
    v3 base = vadd(V3(m->pos.x, m->pos.y + pl.bob, m->pos.z), qrot(qaxis(UP, m->yaw), shapes[S_TORSO].at));
    return XF(vadd(base, qrot(q, pnt[PNT_TORSO_EYE])), q);
}
static void calibrate(const Input *in) {
    pl.calib = XF(in->head.p, qaxis(UP, qyaw(in->head.q)));
    pl.footLift = maxf(0, 1.65f - in->head.p.y);
}
static void update_rig(void) {
    if (pl.mode == MODE_PILOT) rigNow = xfmul(cockpit_anchor(&mech[pl.mech]), xfinv(pl.calib));
    else rigNow = XF(V3(pl.pos.x, pl.pos.y + (pl.fly ? 0 : pl.footLift), pl.pos.z), qaxis(UP, pl.yaw));
}
xf game_rig(void) { return rigNow; }
int game_piloting(void) { return pl.mode == MODE_PILOT; }
void game_haptics(float *l, float *r) { *l = hapL; *r = hapR; hapL = hapR = 0; }
static int deaths;
void game_swarm(int n) { swarm = n < 0 ? 0 : n > 64 ? 64 : n; }
void game_horde(int n) { horde = n < 0 ? 0 : n > NMECH - 5 ? NMECH - 5 : n; }
void game_set_cam(v3 pos, float yaw) { pl.pos = pos; pl.yaw = yaw; pl.fly = 1; pl.mode = MODE_FOOT; mech[pl.mech].piloted = 0; update_rig(); }

// ---- debris, death, damage -----------------------------------------------------------------------
static Debris *add_debris(int shape, xf x, int team, v3 v) {
    Debris *d = &deb[debNext++ % MAXDEB];
    memset(d, 0, sizeof *d);
    d->active = 1; d->shape = shape; d->team = team; d->x = x;
    d->radius = shapes[shape].radius;
    d->v = vadd(v, rv(1.5f));
    d->w = rv(2.5f);
    d->seed = frand() * 30;
    return d;
}
static void detach(Mech *m, int part, v3 push) {
    Debris *d = add_debris(partShape[part], m->world[part], m->team, push);
    d->ndent = m->ndent[part];
    memcpy(d->dent, m->dent[part], sizeof d->dent);
    memcpy(d->heat, m->heat[part], sizeof d->heat);
    d->seed = m->seed + part;
    m->gone[part] = 1;
}
static void start_eject(void);
static void die(Mech *m) {
    if (!m->alive) return;
    if (pl.mode == MODE_PILOT && m == &mech[pl.mech]) start_eject(); // thrown clear at the last moment
    m->alive = 0;
    v3 c = xfpt(m->world[P_TORSO], V3(0, 1.3f, 0));
    burst(c, 2.2f);
    for (int i = 0; i < P_COUNT; i++)
        if (!m->gone[i]) {
            m->stage[i] = 3;
            detach(m, i, vadd(vmul(vnorm(vsub(m->world[i].p, c)), 3.0f), V3(0, i == P_TORSO ? 1 : 3, 0)));
        }
    hapL = hapR = 1;
}
static void apply_hit(Mech *m, int part, v3 local, v3 wpos, v3 dir, float dmg) {
    int st = mech_damage(m, part, local, dmg);
    v3 back = vmul(dir, -1);
    m->aggro = 1;
    spawn(PT_FLASH, vadd(wpos, vmul(back, 0.15f)), V3(0, 0, 0), 0.1f, 0.5f + dmg * 0.03f, 0, 0, 0);
    sparks(wpos, back, 5, 6, 0.06f);
    audio_play(SND_HIT, wpos, 0.35f + dmg * 0.02f);
    if (pl.mode == MODE_PILOT && m == &mech[pl.mech]) { // you feel hits to your own mech
        hapL = hapR = clampf(0.3f + dmg * 0.03f, 0, 1);
        pl.hurt = maxf(pl.hurt, 0.35f);
        if (part == P_TORSO) { // sparks jump from the dashboard
            xf a = cockpit_anchor(m);
            sparks(xfpt(a, V3(frands() * 0.6f, -0.45f, -0.8f)), qrot(a.q, V3(0, 0.3f, 1)), 6, 1.6f, 0.012f);
        }
    }
    if (st < 0) return;
    if (st == 1) sparks(wpos, back, 10, 8, 0.06f);
    if (st == 2) { for (int i = 0; i < 5; i++) smoke(wpos, 0.6f); sparks(wpos, back, 14, 9, 0.06f); }
    if (st == 3) {
        if (part == P_ARM_L || part == P_ARM_R) {
            burst(wpos, 1.0f);
            detach(m, part, vadd(vmul(dir, 4), V3(0, 3, 0)));
        } else die(m); // torso, hips or a leg: the mech goes down
    }
}

// ---- small enemies: drones, crawlers and turrets ----------------------------------------------------
// Each is one traced body (the turret: a base and a turning head) with hit points, not a jointed mech.
enum { B_DRONE, B_CRAWLER, B_TURRET };
#define NBOT 160
#define BOT_OWNER 1000 // a shot's owner is a mech index, -1 for your pistol, or BOT_OWNER + bot index
typedef struct {
    int kind, alive, wreck, aggro, burst, ndent;
    v3 pos, vel, home;   // pos: a drone's centre; the ground point under a crawler or turret
    float yaw, aim, pitch, roll, walk;
    float hp, flash, cool, phase, seed;
    float dent[4][4], heat[4];
} Bot;
static Bot bot[NBOT];
static int nbot;
static const struct { float hp, hit, aggro, dent, dmg, foot, splash; int shape; } BOT[3] = {
    // hit: shots passing this close count (rotors and legs are not traced). dent: hole size against a mech's.
    // dmg: damage to armour. foot: damage to you on foot (half that within 'splash' metres of where it lands).
    {20, 0.6f, 120, 0.4f, 0.5f, 2.5f, 0, S_DRONE},       // light guns: a nuisance to a mech, dangerous on foot
    {45, 0.75f, 75, 0.45f, 2.5f, 6, 1.8f, S_CRAWLER},    // slow plasma spit you can sidestep
    {220, 0, 220, 1.0f, 22, 26, 3.5f, S_TURRET_HEAD}};   // heavy shells

static xf bot_xf(const Bot *b) { // where the body (turret: the head) is
    if (b->kind == B_DRONE) return XF(b->pos, qmul(qaxis(UP, b->yaw), qmul(qaxis(V3(1, 0, 0), b->pitch), qaxis(V3(0, 0, 1), b->roll))));
    if (b->kind == B_CRAWLER)
        return XF(vadd(b->pos, V3(0, -pnt[PNT_CRAWLER_SOLE].y + 0.03f * sinf(b->walk * 2), 0)), qaxis(UP, b->yaw));
    return XF(vadd(b->pos, qrot(qaxis(UP, b->yaw), shapes[S_TURRET_HEAD].at)), qmul(qaxis(UP, b->aim), qaxis(V3(1, 0, 0), b->pitch)));
}
static void bot_die(Bot *b, v3 push) {
    if (!b->alive) return;
    xf x = bot_xf(b);
    b->alive = 0;
    b->wreck = b->kind == B_TURRET; // the base stays, burnt out
    burst(x.p, b->kind == B_TURRET ? 1.6f : 0.7f);
    Debris *d = add_debris(BOT[b->kind].shape, x, 1, vadd(vmul(push, 4), V3(0, b->kind == B_TURRET ? 6 : 3, 0)));
    d->ndent = b->ndent;
    memcpy(d->dent, b->dent, sizeof b->dent);
    memcpy(d->heat, b->heat, sizeof b->heat);
    if (b->kind == B_CRAWLER)
        for (int k = 0; k < 4; k++) add_debris(S_CRAWLER_LEG, XF(x.p, qaxis(UP, b->yaw + k * 1.57f)), 1, vadd(rv(4), V3(0, 4, 0)));
}
// part 0: the traced body, where a hole is left. Anything else (near miss, turret base) only does damage.
static void bot_hit(Bot *b, int part, v3 local, v3 wpos, v3 dir, float dmg) {
    v3 back = vmul(dir, -1);
    b->hp -= dmg;
    b->flash = 1;
    spawn(PT_FLASH, vadd(wpos, vmul(back, 0.15f)), V3(0, 0, 0), 0.1f, 0.4f + dmg * 0.03f, 0, 0, 0);
    sparks(wpos, back, 5, 6, 0.05f);
    audio_play(SND_HIT, wpos, 0.3f + dmg * 0.02f);
    for (int i = 0; i < nbot; i++) // the rest of its group notices
        if (bot[i].alive && vlen(vsub(bot[i].pos, b->pos)) < 70) bot[i].aggro = 1;
    if (part == 0) {
        int j = b->ndent < 4 ? b->ndent++ : (int)(frand() * 3.99f);
        b->dent[j][0] = local.x; b->dent[j][1] = local.y; b->dent[j][2] = local.z;
        b->dent[j][3] = (0.13f + 0.011f * dmg) * BOT[b->kind].dent;
        b->heat[j] = 1;
    }
    if (b->hp <= 0) bot_die(b, dir);
    else if (b->hp < BOT[b->kind].hp * 0.4f) smoke(wpos, 0.4f);
}

// ---- setup -----------------------------------------------------------------------------------------
void game_init(int demo, int startOnFoot) {
    demoLevel = demo;
    footStart = startOnFoot;
    static int once;
    xf keepCalib = pl.calib;
    float keepLift = pl.footLift, keepWorn = pl.wornTime;
    int keepFit = pl.fitted;
    if (!once) { world_init(); mech_shapes_init(); }
    memset(&pl, 0, sizeof pl);
    memset(bolt, 0, sizeof bolt);
    memset(deb, 0, sizeof deb);
    nptc = 0;
    mech_init(&mech[0], V3(0, 0, 0), 0, 0);            // your mech
    mech_init(&mech[1], V3(0, 0, -55), PI, 1);         // practice target, never fights back
    mech_init(&mech[2], V3(-70, 0, -120), 1.0f, 1);    // patrols
    mech_init(&mech[3], V3(85, 0, -140), -2.0f, 1);
    mech_init(&mech[4], V3(-18, 0, 12), 0.6f, 2);      // spare mech
    mech[1].dummy = 1;
    mech[2].throttle = mech[3].throttle = 3.2f;
    mech[2].aiTurn = 0.13f;
    mech[3].aiTurn = -0.11f;
    mech[2].aiFire = mech[3].aiFire = 2;
    nmech = 5 + horde;
    for (int i = 5; i < nmech; i++) { // test crowd: enemy patrols scattered ahead, 130 to 600 m out
        float a = frands() * 1.3f, r = 130 + frand() * 470;
        mech_init(&mech[i], V3(sinf(a) * r, 0, -cosf(a) * r), frand() * 6.28f, 1);
        mech[i].throttle = 2.6f + frand();
        mech[i].aiTurn = frands() * 0.15f;
        mech[i].aiFire = 2;
    }
    // Small enemies: three turrets far out, crawler packs and drone flights between you and the patrols.
    static const struct { int kind, n; float x, z; } GROUP[] = {
        {B_TURRET, 1, -150, -210}, {B_TURRET, 1, 130, -240}, {B_TURRET, 1, 10, -330},
        {B_CRAWLER, 4, 45, -95}, {B_CRAWLER, 4, -95, -70}, {B_CRAWLER, 3, 0, -190},
        {B_DRONE, 3, -60, -190}, {B_DRONE, 3, 110, -170}};
    memset(bot, 0, sizeof bot);
    nbot = 0;
    for (unsigned g = 0; g < sizeof GROUP / sizeof GROUP[0] + 2 * swarm; g++) {
        int extra = g >= sizeof GROUP / sizeof GROUP[0]; // --swarm: single drones and crawlers scattered ahead
        float ea = extra ? frands() * 1.3f : 0, er = extra ? 80 + frand() * 320 : 0;
        int kind = extra ? (int)(g & 1) : GROUP[g].kind, n = extra ? 1 : GROUP[g].n;
        for (int k = 0; k < n && nbot < NBOT; k++) {
            Bot *b = &bot[nbot++];
            float a = k * 2.1f + g;
            b->kind = kind;
            b->alive = 1;
            b->hp = BOT[kind].hp;
            b->seed = nbot * 7.3f;
            b->phase = nbot * 1.7f;
            b->yaw = b->aim = a;
            b->home = extra ? V3(sinf(ea) * er, 0, -cosf(ea) * er) : V3(GROUP[g].x, 0, GROUP[g].z);
            b->pos = vadd(b->home, n > 1 ? V3(sinf(a) * 5, 0, cosf(a) * 5) : V3(0, 0, 0));
            b->home.y = world_height(b->home.x, b->home.z);
            b->pos.y = world_height(b->pos.x, b->pos.z) + (kind == B_DRONE ? 13 : 0);
        }
    }
    pl.hp = 100;
    pl.fade = 1;
    pl.fadeDir = -1;
    pl.pendingBoard = -1;
    pl.calib = XF(V3(0, 1.7f, 0), qid());
    if (once) { pl.calib = keepCalib; pl.footLift = keepLift; pl.wornTime = keepWorn; pl.fitted = keepFit; } // a restart keeps your seat fit
    once = 1;
    pl.bob = mech[0].hipH;
    if (startOnFoot) { pl.mode = MODE_FOOT; pl.pos = V3(5, 0, 5); }
    else { pl.mode = MODE_PILOT; mech[0].piloted = 1; }
    for (int i = 0; i < demo * 8; i++) { // demo damage for screenshots: synthetic shots at the practice target
        Mech *m = &mech[1];
        int want = i % 3 == 0 ? P_ARM_R : i % 3 == 1 ? P_ARM_L : P_TORSO;
        v3 ro = V3(frands() * 3, 5 + frands() * 2, -30), tgt = vadd(xfpt(m->world[want], shapes[partShape[want]].boxC), rv(0.5f));
        v3 rd = vnorm(vsub(tgt, ro)), l;
        float t;
        int part;
        if (m->alive && mech_ray(m, ro, rd, 200, &t, &part, &l)) apply_hit(m, part, l, vadd(ro, vmul(rd, t)), rd, 9);
    }
    update_rig();
}

static void collide_rocks(v3 *p, float radius) {
    for (int i = 0; i < NROCK; i++) {
        float dx = p->x - rocks[i].pos.x, dz = p->z - rocks[i].pos.z, d = sqrtf(dx * dx + dz * dz), r = rocks[i].radius + radius;
        if (d < r && d > 1e-4f) { p->x += dx / d * (r - d); p->z += dz / d * (r - d); }
    }
}

// Nearest thing along a ray: mechs (except `skip`), rocks or the ground.
static Hit trace(v3 ro, v3 rd, float tmax, int skip) {
    Hit h = {tmax, -1, 0, 0, 0, {0, 0, 0}, -1};
    for (int k = 0; k < nmech; k++) {
        if (k == skip || !mech[k].alive) continue;
        v3 oc = vsub(vadd(mech[k].pos, V3(0, 5, 0)), ro);
        float along = vdot(oc, rd);
        if (along < -9 || along > h.t + 9 || vdot(oc, oc) - along * along > 81) continue;
        float t;
        int part;
        v3 l;
        if (mech_ray(&mech[k], ro, rd, h.t, &t, &part, &l)) { h.t = t; h.mech = k; h.part = part; h.local = l; h.rock = 0; }
    }
    for (int k = 0; k < nbot; k++) {
        const Bot *b = &bot[k];
        if (!(b->alive || b->wreck) || skip == BOT_OWNER + k) continue;
        xf x = bot_xf(b);
        const Shape *sp = &shapes[BOT[b->kind].shape];
        v3 oc = vsub(xfpt(x, sp->boxC), ro), l = V3(0, 0, 0);
        float along = vdot(oc, rd), miss = vdot(oc, oc) - along * along, r = vlen(sp->boxH) + 1.5f, near = BOT[b->kind].hit, t;
        if (along < -r || along > h.t + r || miss > r * r) continue;
        int part = -1;
        if (b->alive && shape_ray(sp, x, ro, rd, h.t, b->dent, b->ndent, &t, &l)) part = 0;
        else if (b->kind == B_TURRET && shape_ray(&shapes[S_TURRET_BASE], XF(b->pos, qaxis(UP, b->yaw)), ro, rd, h.t, NULL, 0, &t, &l)) part = 1;
        else if (miss < near * near && (t = along - sqrtf(near * near - miss)) > 0 && t < h.t) part = 1;
        if (part >= 0) { h.t = t; h.mech = -1; h.bot = b->alive ? k : -1; h.part = part; h.local = l; h.rock = !b->alive; }
    }
    for (int k = 0; k < NROCK; k++) {
        v3 oc = vsub(rocks[k].pos, ro), l;
        float along = vdot(oc, rd), t;
        if (along < -7 || along > h.t + 7 || vdot(oc, oc) - along * along > 49) continue;
        if (shape_ray(&shapes[S_ROCK0 + rocks[k].shape], XF(rocks[k].pos, qaxis(UP, rocks[k].yaw)), ro, rd, h.t, NULL, 0, &t, &l)) {
            h.t = t; h.mech = -1; h.bot = -1; h.rock = 1;
        }
    }
    float prev = 0;
    for (float t = 0; t < h.t; prev = t, t += maxf(0.8f, t * 0.03f)) {
        v3 p = vadd(ro, vmul(rd, t));
        if (p.y < world_height(p.x, p.z)) {
            float a = prev, b = t;
            for (int i = 0; i < 6; i++) {
                float m = (a + b) * 0.5f;
                v3 q = vadd(ro, vmul(rd, m));
                if (q.y < world_height(q.x, q.z)) b = m; else a = m;
            }
            h.t = b; h.mech = -1; h.bot = -1; h.rock = 0; h.ground = 1;
            break;
        }
    }
    return h;
}

static void fire_bolt(int owner, int kind, v3 p, v3 dir, float speed, float dmg) {
    for (int i = 0; i < MAXBOLT; i++)
        if (!bolt[i].active) {
            bolt[i] = (Bolt){1, owner, kind, p, vmul(dir, speed), 3.0f, dmg};
            return;
        }
}
// Fires one of a mech's arm weapons at a point. arm 0: twin autocannon, arm 1: plasma pod.
static int mech_fire(int idx, int arm, v3 target, float spread) {
    Mech *m = &mech[idx];
    if (m->gone[P_ARM_L + arm]) return 0;
    v3 mz = pnt[arm ? PNT_ARM_R_MUZZLE : PNT_ARM_L_MUZZLE];
    if (!arm) mz.x += (pl.barrel++ & 1) ? 0.2f : -0.2f;
    v3 muzzle = xfpt(m->world[P_ARM_L + arm], mz);
    v3 to = vsub(target, muzzle);
    v3 dir = vnorm(vadd(to, rv(spread * vlen(to))));
    fire_bolt(idx, arm, muzzle, dir, arm ? 120 : 230, arm ? 34 : 6);
    spawn(PT_FLASH, vadd(muzzle, vmul(dir, 0.3f)), V3(0, 0, 0), arm ? 0.12f : 0.07f, arm ? 1.5f : 0.8f, 0, 0, 0);
    audio_play(arm ? SND_PLASMA : SND_CANNON, muzzle, arm ? 0.9f : 0.55f);
    return 1;
}

static void start_eject(void) {
    Mech *m = &mech[pl.mech];
    xf a = cockpit_anchor(m);
    update_rig();
    pl.yaw = qyaw(rigNow.q);
    pl.pos = V3(rigNow.p.x, rigNow.p.y - pl.footLift, rigNow.p.z);
    pl.mode = MODE_EJECT;
    pl.ejV = vadd(V3(0, 14, 0), vmul(yawFwd(m->yaw), -6));
    pl.grab[0] = pl.grab[1] = GRAB_NONE;
    pl.ejectPull = 0;
    m->piloted = 0;
    m->throttle = 0;
    if (m->stage[P_TORSO] >= 2) m->cook = 2.4f; // a crippled reactor goes up shortly after you leave
    add_debris(S_CK_SHELL, a, m->team, vadd(V3(0, 9, 0), vmul(yawFwd(m->yaw), 3)));
    for (int i = 0; i < 8; i++) smoke(vadd(a.p, rv(0.8f)), 0.6f);
    sparks(a.p, UP, 16, 6, 0.05f);
    audio_play(SND_WHOOSH, a.p, 1.2f);
    audio_play(SND_BOOM, a.p, 0.5f);
    hapL = hapR = 1;
}

static void enemy_ai(int idx, float dt, v3 tgt, int tgtOnFoot) {
    Mech *m = &mech[idx];
    v3 to = vsub(tgt, m->pos);
    float dist = sqrtf(to.x * to.x + to.z * to.z);
    int limp = m->stage[P_THIGH_L] >= 2 || m->stage[P_SHIN_L] >= 2 || m->stage[P_THIGH_R] >= 2 || m->stage[P_SHIN_R] >= 2;
    if (dist < 120) m->aggro = 1;
    if (game_calm) m->aggro = 0;
    if (!m->aggro || dist > 320) { // patrol in a wide circle
        m->throttle = limp ? 1.5f : 3.2f;
        m->yaw += m->aiTurn * dt * clampf(m->speed / 3, 0, 1);
        m->torsoYaw = approach(m->torsoYaw, 0, dt);
        return;
    }
    float bearing = atan2f(-to.x, -to.z);
    float desired = dist > 85 ? bearing : bearing + ((idx & 1) ? 1.25f : -1.25f); // close in, then circle
    m->yaw += clampf(wrap(desired - m->yaw), -0.55f * dt, 0.55f * dt);
    m->throttle = (dist > 50 ? 4.2f : 2.0f) * (limp ? 0.45f : 1);
    m->torsoYaw = clampf(m->torsoYaw + clampf(wrap(bearing - m->yaw - m->torsoYaw), -1.3f * dt, 1.3f * dt), -1.5f, 1.5f);
    float muzzleY = m->pos.y + 6.0f;
    m->aimPitch = approach(m->aimPitch, clampf(atan2f(tgt.y - muzzleY, maxf(dist, 1)), -0.6f, 0.5f), dt);
    int aligned = fabsf(wrap(bearing - m->yaw - m->torsoYaw)) < 0.12f && dist < 230;
    float spread = tgtOnFoot ? 0.045f : 0.022f;
    m->aiFire -= dt;
    if (m->aiFire <= 0 && aligned) { m->aiBurst = 5; m->aiFire = 3.0f + 2.5f * frand(); }
    m->cool[0] -= dt;
    if (m->aiBurst > 0 && m->cool[0] <= 0) { mech_fire(idx, 0, tgt, spread); m->aiBurst--; m->cool[0] = 0.14f; }
    m->cool[1] -= dt;
    if (aligned && m->cool[1] <= 0 && dist < 170) { mech_fire(idx, 1, tgt, spread); m->cool[1] = 5.0f + 3.0f * frand(); }
}

// Drones circle and strafe from the air, crawlers run you down on the ground, turrets hold a spot.
static void bots_update(float dt, v3 tgt, int onFoot) {
    for (int i = 0; i < nbot; i++) {
        Bot *b = &bot[i];
        if (!b->alive) {
            if (b->wreck && frand() < dt * 3) smoke(vadd(b->pos, V3(0, 1.8f, 0)), 0.7f);
            continue;
        }
        b->flash = maxf(0, b->flash - dt * 7);
        for (int j = 0; j < b->ndent; j++) b->heat[j] = maxf(0, b->heat[j] - dt * 0.45f);
        b->cool -= dt;
        b->phase += dt;
        v3 to = vsub(tgt, b->pos);
        float dist = sqrtf(to.x * to.x + to.z * to.z), bearing = atan2f(-to.x, -to.z);
        float side = (i & 1) ? 1.0f : -1.0f;
        if (dist < BOT[b->kind].aggro) b->aggro = 1;
        if (game_calm == 1 || pl.dead || pl.fly) b->aggro = 0;
        int hunt = b->aggro && dist < 320;
        int facing = fabsf(wrap(bearing - (b->kind == B_TURRET ? b->aim : b->yaw))) < (b->kind == B_TURRET ? 0.05f : 0.25f);
        if (b->hp < BOT[b->kind].hp * 0.4f && frand() < dt * 4) smoke(bot_xf(b).p, 0.35f);

        if (b->kind == B_DRONE) {
            v3 want; // the spot it is flying to: a ring around you when hunting, around its post otherwise
            if (hunt) {
                float a = b->seed + gtime * 0.3f * side, ring = onFoot ? 22.0f : 34.0f;
                want = V3(tgt.x + sinf(a) * ring, tgt.y + (onFoot ? 5.5f : 4.0f) + sinf(b->phase * 1.3f) * 1.2f, tgt.z + cosf(a) * ring);
            } else {
                float a = b->seed + gtime * 0.25f * side;
                want = V3(b->home.x + sinf(a) * 25, b->home.y + 13 + sinf(b->phase) * 1.5f, b->home.z + cosf(a) * 25);
            }
            want.y = maxf(want.y, world_height(want.x, want.z) + 3);
            v3 dv = vsub(want, b->pos);
            float dl = maxf(vlen(dv), 1e-3f);
            b->vel = vadd(b->vel, vmul(vsub(vmul(dv, minf(16.0f, dl * 1.2f) / dl), b->vel), minf(1, 2.5f * dt)));
            b->pos = vadd(b->pos, vmul(b->vel, dt));
            b->pos.y = maxf(b->pos.y, world_height(b->pos.x, b->pos.z) + 2);
            b->yaw += clampf(wrap((hunt ? bearing : atan2f(-b->vel.x, -b->vel.z)) - b->yaw), -2.5f * dt, 2.5f * dt);
            b->roll = approach(b->roll, clampf(-(b->vel.x * cosf(b->yaw) - b->vel.z * sinf(b->yaw)) * 0.04f, -0.5f, 0.5f), dt * 2); // leans into turns
            b->pitch = approach(b->pitch, hunt ? clampf(atan2f(to.y, maxf(dist, 1)), -0.9f, 0.5f) : 0, dt * 2);
            if (hunt && b->cool <= 0 && dist < 75 && facing) { // short bursts of light fire
                v3 mz = xfpt(bot_xf(b), pnt[PNT_DRONE_MUZZLE]), aim = vsub(tgt, mz);
                v3 d = vnorm(vadd(aim, rv(0.03f * vlen(aim))));
                fire_bolt(BOT_OWNER + i, 0, mz, d, 170, BOT[B_DRONE].dmg);
                spawn(PT_FLASH, vadd(mz, vmul(d, 0.2f)), V3(0, 0, 0), 0.06f, 0.4f, 0, 0, 0);
                audio_play(SND_CANNON, mz, 0.3f);
                b->cool = ++b->burst % 3 ? 0.13f : 2.2f + frand() * 1.5f;
            }
        } else if (b->kind == B_CRAWLER) {
            float keep = onFoot ? 13.0f : 8.0f, fwd = 0, strafe = 0; // closes to this range, then circles
            if (hunt) {
                fwd = dist > keep + 2 ? 8.0f : dist < keep - 3 ? -3.0f : 0;
                strafe = dist < keep + 6 ? 2.5f * side : 0;
                b->yaw += clampf(wrap(bearing - b->yaw), -3 * dt, 3 * dt);
            }
            b->vel.x = approach(b->vel.x, strafe, 12 * dt);
            b->vel.z = approach(b->vel.z, fwd, 16 * dt);
            v3 f = yawFwd(b->yaw);
            b->pos = vadd(b->pos, vmul(vadd(vmul(f, b->vel.z), vmul(V3(-f.z, 0, f.x), b->vel.x)), dt));
            collide_rocks(&b->pos, 0.8f);
            for (int k = 0; k < nbot; k++) { // they do not pile onto one spot
                v3 d = vsub(b->pos, bot[k].pos);
                float l = sqrtf(d.x * d.x + d.z * d.z);
                if (k != i && bot[k].alive && bot[k].kind == B_CRAWLER && l < 1.8f && l > 1e-3f) { b->pos.x += d.x / l * (1.8f - l) * 0.5f; b->pos.z += d.z / l * (1.8f - l) * 0.5f; }
            }
            b->pos.y = world_height(b->pos.x, b->pos.z);
            b->walk += (fabsf(b->vel.z) + fabsf(b->vel.x)) * dt * 1.5f;
            if (hunt && b->cool <= 0 && dist < 45 && facing) {
                v3 mz = xfpt(bot_xf(b), pnt[PNT_CRAWLER_MUZZLE]), aim = vsub(tgt, mz);
                v3 d = vnorm(vadd(aim, rv(0.04f * vlen(aim))));
                fire_bolt(BOT_OWNER + i, 2, mz, d, 30, BOT[B_CRAWLER].dmg);
                spawn(PT_FLASH, vadd(mz, vmul(d, 0.2f)), V3(0, 0, 0), 0.08f, 0.5f, 0, 0, 0);
                audio_play(SND_PLASMA, mz, 0.35f);
                b->cool = 2.6f + frand() * 1.4f;
            }
            if (pl.mode == MODE_PILOT) { // your mech can simply walk over them
                const Mech *m = &mech[pl.mech];
                for (int k = P_FOOT_L; k <= P_FOOT_R; k += P_FOOT_R - P_FOOT_L) {
                    v3 d = vsub(m->world[k].p, b->pos);
                    if (fabsf(m->speed) > 1 && d.x * d.x + d.z * d.z < 2.9f && d.y < 2.0f) { bot_die(b, yawFwd(m->yaw)); hapL = hapR = 0.7f; }
                }
            }
        } else {
            if (hunt) {
                b->aim += clampf(wrap(bearing - b->aim), -0.8f * dt, 0.8f * dt);
                b->pitch = approach(b->pitch, clampf(atan2f(to.y - 1.9f, maxf(dist, 1)), -0.25f, 0.5f), 0.5f * dt);
            } else b->aim += 0.2f * side * dt; // sweeps while idle
            if (hunt && b->cool <= 0 && dist < 220 && facing) { // slow, heavy shells from alternating barrels
                v3 mz = xfpt(bot_xf(b), vadd(pnt[PNT_TURRET_HEAD_MUZZLE], V3((b->burst++ & 1) ? 0.3f : -0.3f, 0, 0))), aim = vsub(tgt, mz);
                v3 d = vnorm(vadd(aim, rv(0.012f * vlen(aim))));
                fire_bolt(BOT_OWNER + i, 1, mz, d, 110, BOT[B_TURRET].dmg);
                spawn(PT_FLASH, vadd(mz, vmul(d, 0.3f)), V3(0, 0, 0), 0.12f, 1.5f, 0, 0, 0);
                audio_play(SND_PLASMA, mz, 0.9f);
                b->cool = 2.6f;
            }
        }
    }
}

// ---- update ----------------------------------------------------------------------------------------
static void update_pilot(const Input *in, float dt) {
    Mech *m = &mech[pl.mech];
    xf anchor = cockpit_anchor(m), inv = xfinv(anchor);
    if (in->recenterKey) calibrate(in);
    float turnIn = in->vr ? in->hand[1].sx : in->hand[0].sx;
    float thrIn = in->hand[0].sy;
    for (int h = 0; h < 2 && in->vr; h++) { // grabbing the physical controls
        v3 hp = xfpt(inv, xfpt(rigNow, in->hand[h].grip.p));
        int sq = in->hand[h].squeeze > 0.5f && in->hand[h].tracked;
        if (sq && !pl.sqPrev[h] && !pl.grab[h]) {
            v3 thr = vadd(shapes[S_CK_THROTTLE].at, qrot(qaxis(V3(1, 0, 0), -pl.throttle * 0.6f), pnt[PNT_CK_THROTTLE_GRIP]));
            v3 stk = vadd(shapes[S_CK_STICK].at, pnt[PNT_CK_STICK_GRIP]);
            if (vlen(vsub(hp, thr)) < 0.14f) pl.grab[h] = GRAB_THROTTLE;
            else if (vlen(vsub(hp, stk)) < 0.14f) pl.grab[h] = GRAB_STICK;
            else if (vlen(vsub(hp, shapes[S_CK_EJECT].at)) < 0.15f) pl.grab[h] = GRAB_EJECT;
            if (pl.grab[h]) { pl.grabStart[h] = hp; audio_play(SND_CLICK, xfpt(rigNow, in->hand[h].grip.p), 1.0f); if (h) hapR = 0.6f; else hapL = 0.6f; }
        }
        if (!sq) { if (pl.grab[h] == GRAB_EJECT) pl.ejectPull = 0; pl.grab[h] = GRAB_NONE; }
        pl.sqPrev[h] = sq;
        if (pl.grab[h] == GRAB_THROTTLE) {
            v3 d = vsub(hp, shapes[S_CK_THROTTLE].at);
            pl.throttle = clampf(atan2f(-d.z, maxf(d.y, 0.02f)) / 0.6f, -0.4f, 1.0f);
            thrIn = 0;
        } else if (pl.grab[h] == GRAB_STICK) {
            v3 d = vsub(hp, pl.grabStart[h]);
            pl.stickX = clampf(d.x / 0.07f, -1, 1);
            pl.stickY = clampf(-d.z / 0.07f, -1, 1);
            turnIn = pl.stickX;
        } else if (pl.grab[h] == GRAB_EJECT) {
            pl.ejectPull = clampf(pl.grabStart[h].y - hp.y, 0, 0.2f);
            if (pl.ejectPull > 0.12f) { start_eject(); return; }
        }
    }
    if (pl.grab[0] != GRAB_STICK && pl.grab[1] != GRAB_STICK) { pl.stickX = approach(pl.stickX, turnIn, dt * 8); pl.stickY = approach(pl.stickY, 0, dt * 8); }
    if (fabsf(thrIn) > 0.2f) pl.throttle = clampf(pl.throttle + thrIn * dt * 0.9f, -0.4f, 1.0f);
    else if (fabsf(pl.throttle) < 0.06f && pl.grab[0] != GRAB_THROTTLE && pl.grab[1] != GRAB_THROTTLE) pl.throttle = 0;
    if (in->ejectKey) { start_eject(); return; }

    int limp = m->stage[P_THIGH_L] >= 2 || m->stage[P_SHIN_L] >= 2 || m->stage[P_THIGH_R] >= 2 || m->stage[P_SHIN_R] >= 2;
    m->throttle = pl.throttle * TOP_SPEED * (limp ? 0.45f : 1);
    m->yaw -= pl.stickX * 0.75f * dt;

    // aim where you look (headset) or where the cursor points (flat)
    xf src = in->vr ? xfmul(rigNow, in->head) : xfmul(rigNow, in->hand[1].aim);
    v3 dir = qrot(src.q, V3(0, 0, -1));
    Hit h = trace(src.p, dir, 600, pl.mech);
    pl.aimDist = h.t;
    pl.aim = vadd(src.p, vmul(dir, h.t));
    v3 to = vsub(pl.aim, xfpt(m->world[P_TORSO], pnt[PNT_TORSO_AIM]));
    m->aimPitch = approach(m->aimPitch, clampf(atan2f(to.y, sqrtf(to.x * to.x + to.z * to.z)), -0.6f, 0.6f), dt * 2.5f);
    m->cool[0] -= dt;
    m->cool[1] -= dt;
    if (in->hand[0].trigger > 0.5f && m->cool[0] <= 0) { if (mech_fire(pl.mech, 0, pl.aim, 0.004f)) hapL = 0.35f; m->cool[0] = 0.11f; }
    if (in->hand[1].trigger > 0.5f && m->cool[1] <= 0) { if (mech_fire(pl.mech, 1, pl.aim, 0.002f)) hapR = 1.0f; m->cool[1] = 0.85f; }
    pl.alarm = m->stage[P_TORSO] >= 2 || limp ? 1.0f : (m->stage[P_TORSO] == 1 ? 0.3f : 0);
    if (m->stage[P_TORSO] >= 2 && frand() < dt * 5) { // a critical reactor fills the cabin with smoke wisps
        spawn(PT_SMOKE, xfpt(anchor, V3(frands() * 0.5f, -0.5f, -0.75f)), V3(0, 0.25f, 0), 1.2f, 0.035f, 0.03f, 0.1f, 0.5f);
    }
}

static void update_foot(const Input *in, float dt) {
    float headYaw = pl.yaw + qyaw(in->head.q);
    v3 f = yawFwd(headYaw), r = V3(-f.z, 0, f.x);
    float mx = in->hand[0].sx, my = in->hand[0].sy;
    pl.moving = sqrtf(mx * mx + my * my);
    float turn = in->flatYaw, sx = in->hand[1].sx;
    if (in->vr) { // snap turn
        if (fabsf(sx) > 0.7f && !pl.snapHeld) { turn = sx > 0 ? -PI / 6 : PI / 6; pl.snapHeld = 1; }
        if (fabsf(sx) < 0.3f) pl.snapHeld = 0;
    }
    if (turn != 0) { // turn around your head, not around the room center
        v3 h = V3(in->head.p.x, 0, in->head.p.z);
        pl.pos = vadd(pl.pos, vsub(qrot(qaxis(UP, pl.yaw), h), qrot(qaxis(UP, pl.yaw + turn), h)));
        pl.yaw += turn;
    }
    if (pl.fly) {
        v3 d = qrot(qmul(qaxis(UP, pl.yaw), in->head.q), V3(0, 0, -1));
        pl.pos = vadd(pl.pos, vmul(vadd(vadd(vmul(d, my), vmul(r, mx)), V3(0, in->flyUp, 0)), 22 * dt));
    } else {
        pl.pos = vadd(pl.pos, vmul(vadd(vmul(f, my), vmul(r, mx)), 5.5f * dt));
        collide_rocks(&pl.pos, 0.4f);
        pl.pos.y = world_height(pl.pos.x, pl.pos.z);
    }
    update_rig();
    pl.cool -= dt;
    if (in->hand[1].trigger > 0.5f && pl.cool <= 0 && in->hand[1].tracked) {
        xf aim = pistol_xf(rigNow, &in->hand[1]);
        v3 d = qrot(aim.q, V3(0, 0, -1));
        fire_bolt(-1, 1, vadd(aim.p, vmul(d, 0.25f)), d, 160, 8);
        spawn(PT_FLASH, vadd(aim.p, vmul(d, 0.22f)), V3(0, 0, 0), 0.06f, 0.07f, 0, 0, 0);
        pl.cool = 0.16f;
        hapR = 0.5f;
        audio_play(SND_PISTOL, aim.p, 0.5f);
    }
    // climb into a friendly mech when close to it
    int near = -1;
    for (int i = 0; i < nmech; i++) {
        v3 d = vsub(mech[i].pos, pl.pos);
        if (mech[i].alive && mech[i].team != 1 && mech[i].cook <= 0 && sqrtf(d.x * d.x + d.z * d.z) < 8) near = i;
    }
    if (in->boardKey && near >= 0 && pl.pendingBoard < 0 && !pl.fly) { pl.pendingBoard = near; pl.fadeDir = 1; }
}

void game_update(const Input *in) {
    float dt = in->dt;
    gtime += dt;
    if (in->lookKey) game_look = in->lookKey - 1;
    if (in->toggleFly) { pl.fly = !pl.fly; if (pl.mode == MODE_PILOT) start_eject(); }
    if (in->resetKey) { int look = game_look; game_init(demoLevel, footStart); game_look = look; return; }
    if (in->vr && in->headValid) { // fit the seat to where your head really is: at once, and again when you have settled
        if (pl.wornTime == 0 || (!pl.fitted && pl.wornTime > 1.5f)) { calibrate(in); pl.fitted = pl.wornTime > 1.5f; }
        pl.wornTime += dt;
    }
    update_rig();

    // fades: start of game, boarding, death
    pl.fade = clampf(pl.fade + pl.fadeDir * dt * 3.5f, 0, 1);
    pl.hurt = maxf(0, pl.hurt - dt * 1.6f);
    if (pl.fade >= 1 && pl.fadeDir > 0) {
        if (pl.dead) { int look = game_look; deaths++; game_init(demoLevel, 1); game_look = look; return; }
        if (pl.pendingBoard >= 0) {
            pl.mech = pl.pendingBoard;
            pl.pendingBoard = -1;
            pl.mode = MODE_PILOT;
            pl.throttle = pl.stickX = 0;
            pl.bob = mech[pl.mech].hipH;
            mech[pl.mech].piloted = 1;
            mech[pl.mech].torsoYaw = 0;
            calibrate(in);
        }
        pl.fadeDir = -1;
    }

    if (pl.mode == MODE_PILOT) update_pilot(in, dt);
    else if (pl.mode == MODE_FOOT) update_foot(in, dt);
    else { // flying out of the cockpit, then drifting down
        pl.ejV.y = maxf(pl.ejV.y - 9.8f * dt, -5.5f);
        pl.ejV.x *= 1 - 0.4f * dt;
        pl.ejV.z *= 1 - 0.4f * dt;
        pl.pos = vadd(pl.pos, vmul(pl.ejV, dt));
        float gy = world_height(pl.pos.x, pl.pos.z);
        if (pl.pos.y <= gy && pl.ejV.y < 0) {
            pl.pos.y = gy;
            pl.mode = MODE_FOOT;
            hapL = hapR = 0.8f;
            for (int k = 0; k < 6; k++) spawn(PT_DUST, vadd(pl.pos, rv(0.5f)), vadd(V3(0, 1, 0), rv(1.5f)), 1.0f, 0.4f, 0.6f, -0.5f, 1);
        }
    }

    // mechs
    v3 tgt = pl.mode == MODE_PILOT ? xfpt(mech[pl.mech].world[P_TORSO], V3(0, 1.3f, 0)) : vadd(rigNow.p, V3(0, 1.0f, 0));
    for (int i = 0; i < nmech; i++) {
        Mech *m = &mech[i];
        if (!m->alive) continue;
        if (m->team == 1 && !m->dummy && !pl.dead && !pl.fly) enemy_ai(i, dt, tgt, pl.mode != MODE_PILOT);
        if (m->cook > 0) { m->cook -= dt; if (m->cook <= 0) { die(m); continue; } }
        m->speed = approach(m->speed, m->throttle, 3 * dt);
        m->pos = vadd(m->pos, vmul(yawFwd(m->yaw), m->speed * dt));
        collide_rocks(&m->pos, 2.2f);
        for (int k = 0; k < nmech; k++) { // mechs do not walk through each other
            v3 d = vsub(m->pos, mech[k].pos);
            float l = sqrtf(d.x * d.x + d.z * d.z);
            if (k != i && mech[k].alive && l < 6.5f && l > 1e-3f) { m->pos.x += d.x / l * (6.5f - l) * 0.5f; m->pos.z += d.z / l * (6.5f - l) * 0.5f; }
        }
        float before = floorf(m->phase / PI);
        mech_pose(m, dt);
        if (floorf(m->phase / PI) != before && fabsf(m->speed) > 0.5f) { // a foot comes down
            int own = pl.mode == MODE_PILOT && i == pl.mech;
            audio_play(SND_STEP, m->pos, own ? 0.55f : 1.0f);
            if (own) hapL = hapR = maxf(hapL, 0.25f);
            for (int k = 0; k < 2; k++) spawn(PT_DUST, vadd(m->pos, rv(1.5f)), vadd(V3(0, 0.8f, 0), rv(0.8f)), 1.0f, 0.5f, 0.7f, -0.3f, 1);
        }
        for (int p = 0; p < P_COUNT; p++) {
            if (m->gone[p]) continue;
            m->flash[p] = maxf(0, m->flash[p] - dt * 7);
            int big = -1;
            for (int j = 0; j < m->ndent[p]; j++) {
                m->heat[p][j] = maxf(0, m->heat[p][j] - dt * 0.45f);
                if (big < 0 || m->dent[p][j][3] > m->dent[p][big][3]) big = j;
            }
            if (m->stage[p] >= 2 && big >= 0) { // critical parts trail smoke from their worst hole
                m->emit[p] -= dt;
                if (m->emit[p] <= 0) {
                    m->emit[p] = 0.14f;
                    v3 at = xfpt(m->world[p], V3(m->dent[p][big][0], m->dent[p][big][1], m->dent[p][big][2]));
                    smoke(at, 0.4f);
                    if (m->cook > 0) fire(at, 0.6f);
                }
            }
        }
    }
    if (pl.mode == MODE_PILOT) { // the cabin takes only part of the walking bounce
        float hip = mech[pl.mech].hipH;
        static float avg;
        avg += (hip - avg) * minf(1, dt * 1.5f);
        pl.bob = avg + (hip - avg) * 0.3f;
    }

    bots_update(dt, tgt, pl.mode != MODE_PILOT);

    // shots
    for (int i = 0; i < MAXBOLT; i++) {
        Bolt *b = &bolt[i];
        if (!b->active) continue;
        float step = vlen(b->v) * dt;
        v3 dir = vnorm(b->v);
        Hit h = trace(b->p, dir, step, b->owner);
        v3 end = vadd(b->p, vmul(dir, h.t));
        if (b->owner >= 0 && pl.mode != MODE_PILOT && !pl.fly && !pl.dead) { // enemy fire against you on foot
            v3 c = vadd(rigNow.p, V3(0, 0.9f, 0)), oc = vsub(c, b->p);
            float along = clampf(vdot(oc, dir), 0, h.t);
            float miss = vlen(vsub(oc, vmul(dir, along)));
            int kb = b->owner >= BOT_OWNER ? bot[b->owner - BOT_OWNER].kind : -1; // small enemies have their own damage on foot
            float direct = kb >= 0 ? BOT[kb].foot : b->dmg * 1.2f, reach = kb >= 0 ? BOT[kb].splash : b->kind == 1 ? 3.5f : 0;
            float splash = (h.ground || h.rock) && reach > 0 ? vlen(vsub(end, c)) : 99;
            if (miss < 0.6f || splash < reach) {
                pl.hp -= miss < 0.6f ? direct : direct * 0.5f;
                pl.hurt = 1;
                hapL = hapR = 1;
                if (pl.hp <= 0 && pl.mode == MODE_FOOT) { pl.dead = 1; pl.fadeDir = 1; }
                if (miss < 0.6f) { b->active = 0; continue; }
            }
        }
        if (h.mech >= 0) {
            apply_hit(&mech[h.mech], h.part, h.local, end, dir, b->dmg);
            b->active = 0;
        } else if (h.bot >= 0) {
            bot_hit(&bot[h.bot], h.part, h.local, end, dir, b->dmg);
            b->active = 0;
        } else if (h.rock || h.ground) {
            int n = b->kind ? 6 : 2;
            for (int k = 0; k < n; k++) spawn(PT_DUST, vadd(end, rv(0.3f)), vadd(V3(0, 1.5f, 0), rv(1.0f)), 0.9f, b->kind ? 0.7f : 0.35f, 0.5f, -1, 1);
            sparks(end, UP, 3, 4, 0.05f);
            if (b->kind) spawn(PT_FLASH, vadd(end, V3(0, 0.3f, 0)), V3(0, 0, 0), 0.1f, 1.2f, 0, 0, 0);
            b->active = 0;
        } else {
            b->p = end;
            b->life -= dt;
            if (b->life <= 0) b->active = 0;
        }
    }

    // debris
    for (int i = 0; i < MAXDEB; i++) {
        Debris *d = &deb[i];
        if (!d->active) continue;
        d->age += dt;
        for (int j = 0; j < d->ndent; j++) d->heat[j] = maxf(0, d->heat[j] - dt * 0.2f);
        if (!d->rest) {
            d->v.y -= 9.8f * dt;
            d->x.p = vadd(d->x.p, vmul(d->v, dt));
            float wl = vlen(d->w);
            if (wl > 1e-3f) d->x.q = qnorm(qmul(qaxis(vmul(d->w, 1 / wl), wl * dt), d->x.q));
            float gy = world_height(d->x.p.x, d->x.p.z) + d->radius * 0.35f;
            if (d->x.p.y < gy) {
                d->x.p.y = gy;
                for (int k = 0; k < 4; k++) spawn(PT_DUST, vadd(d->x.p, rv(d->radius * 0.6f)), vadd(V3(0, 1.2f, 0), rv(1.2f)), 1.2f, 0.7f, 0.8f, -0.5f, 1);
                if (fabsf(d->v.y) > 2.0f) { audio_play(SND_STEP, d->x.p, 0.5f); d->v = V3(d->v.x * 0.5f, -d->v.y * 0.28f, d->v.z * 0.5f); d->w = vmul(d->w, 0.5f); }
                else d->rest = 1;
            }
        }
        d->emit -= dt;
        if (d->emit <= 0 && d->age < 40 && d->shape != S_CK_SHELL) { // wrecks burn for a while, then just smoke
            int big = d->radius > 2.0f; // torso, hips and arms burn; small pieces only flicker briefly
            d->emit = big ? 0.11f : 0.3f;
            v3 c = xfpt(d->x, shapes[d->shape].boxC);
            if (d->age < (big ? 22 : 7)) fire(vadd(c, rv(d->radius * 0.25f)), 0.35f + d->radius * 0.12f);
            if (big && frand() < 0.45f) smoke(vadd(c, V3(0, d->radius * 0.3f, 0)), 0.4f + d->radius * 0.12f);
        }
    }

    // particles
    for (int i = 0; i < nptc;) {
        Particle *q = &ptc[i];
        q->age += dt;
        if (q->age >= q->life) { ptc[i] = ptc[--nptc]; continue; }
        q->v.y += q->grav * dt;
        q->v = vmul(q->v, 1 - q->drag * dt);
        q->p = vadd(q->p, vmul(q->v, dt));
        q->size = maxf(0.005f, q->size + q->grow * dt);
        i++;
    }
    update_rig();
}

// ---- drawing ---------------------------------------------------------------------------------------
static DrawPart *push(Scene *s, int shape, xf x, int team) {
    if (s->npart >= MAX_DRAW_PARTS) return &s->part[MAX_DRAW_PARTS - 1];
    DrawPart *d = &s->part[s->npart++];
    memset(d, 0, sizeof *d);
    d->shape = &shapes[shape];
    d->world = x;
    d->tintA = TINT[team][0];
    d->tintB = TINT[team][1];
    return d;
}
static xf cabin_at(xf anchor, int shape) { return xfmul(anchor, XF(shapes[shape].at, qid())); } // a fixed cockpit piece at its place in the cabin
static DrawPart *push_cabin(Scene *s, int shape, xf x, int team) {
    DrawPart *d = push(s, shape, x, team);
    d->interior = 1;
    d->texel = 0.0125f;
    return d;
}
static void shadow(Scene *s, v3 p, float r) {
    if (s->nshadow >= MAX_SHADOW) return;
    float *o = s->shadow[s->nshadow++];
    o[0] = p.x; o[1] = p.y; o[2] = p.z; o[3] = r;
}

static void bots_scene(Scene *s) {
    for (int i = 0; i < nbot; i++) {
        const Bot *b = &bot[i];
        if (!b->alive && !b->wreck) continue;
        float dist = vlen(vsub(b->pos, s->head));
        xf x = bot_xf(b);
        if (b->kind == B_TURRET) {
            DrawPart *base = push(s, S_TURRET_BASE, XF(b->pos, qaxis(UP, b->yaw)), 1);
            base->seed = b->seed;
            base->flash = b->flash;
            base->stage = b->alive ? 0 : 3;
            if (dist < 200) shadow(s, vadd(b->pos, V3(0, 1.3f, 0)), 1.6f);
        } else if (dist < 130) shadow(s, x.p, b->kind == B_DRONE ? 0.7f : 0.9f);
        if (!b->alive) continue;
        DrawPart *d = push(s, BOT[b->kind].shape, x, 1);
        d->ndent = b->ndent;
        memcpy(d->dent, b->dent, sizeof b->dent);
        memcpy(d->heat, b->heat, sizeof b->heat);
        d->seed = b->seed;
        d->flash = b->flash;
        d->stage = b->hp < BOT[b->kind].hp * 0.4f ? 2 : 0;
        if (b->kind != B_TURRET) d->texel = 0.03f;
        if (b->kind == B_CRAWLER && dist < 160) { // four legs from one shape; opposite corners step together
            v3 hip = shapes[S_CRAWLER_LEG].at;
            float amp = clampf((fabsf(b->vel.z) + fabsf(b->vel.x)) / 3, 0, 1);
            for (int k = 0; k < 4; k++) {
                int left = !(k & 1), front = !(k & 2);
                float ph = b->walk + (k == 0 || k == 3 ? 0 : PI), sw = 0.45f * amp * sinf(ph), lift = 0.3f * amp * maxf(0, cosf(ph));
                float yaw = left ? (front ? -0.5f : 0.5f) - sw : PI + (front ? 0.5f : -0.5f) + sw;
                DrawPart *leg = push(s, S_CRAWLER_LEG, xfmul(x, XF(V3(left ? hip.x : -hip.x, hip.y, front ? hip.z : -hip.z),
                                                               qmul(qaxis(UP, yaw), qaxis(V3(0, 0, 1), -lift)))), 1);
                leg->seed = b->seed + k;
                leg->flash = b->flash;
                leg->texel = 0.03f;
            }
        }
    }
}

void game_scene(Scene *s, const Input *in) {
    xf rig = rigNow;
    s->npart = s->nptc = s->nshadow = 0;
    s->time = gtime;
    s->look = game_look;
    s->head = xfpt(rig, in->head.p);
    {
        float sp = pl.mode == MODE_PILOT ? fabsf(mech[pl.mech].speed) / TOP_SPEED : 0;
        audio_listener(s->head, qrot(qmul(rig.q, in->head.q), V3(1, 0, 0)), pl.mode == MODE_PILOT ? 1.0f : 0.0f, sp, pl.mode == MODE_PILOT ? pl.alarm : 0);
    }
    s->vignette = in->vr && pl.mode == MODE_EJECT ? 0.7f : 0; // edge darkening only during the eject flight
    s->fade = maxf(pl.fade, pl.hurt * 0.4f);
    s->fadeCol = pl.fade > pl.hurt * 0.4f ? V3(0.02f, 0.02f, 0.03f) : V3(0.9f, 0.1f, 0.05f);
    int piloting = pl.mode == MODE_PILOT;

    s->nimp = 0;
    for (int i = 0; i < nmech; i++) {
        Mech *m = &mech[i];
        if (!m->alive) continue;
        // Far away and in one piece: two flat cards instead of ten traced parts. The switch distance differs a
        // little going out and coming back, so a mech hovering at the limit does not flicker between the two.
        float dist = vlen(vsub(m->pos, s->head));
        int whole = 1;
        for (int p = 0; p < P_COUNT; p++) whole &= !m->gone[p];
        m->far = game_cards && whole && s->nimp + 2 <= MAX_DRAW_IMP && dist > (m->far ? IMP_DIST - 8 : IMP_DIST + 8);
        if (m->far) {
            for (int layer = 0; layer < 2; layer++) {
                DrawImp *d = &s->imp[s->nimp++];
                d->pos = m->world[layer ? P_TORSO : P_PELVIS].p;
                d->yaw = m->yaw + (layer ? m->torsoYaw : 0);
                d->layer = layer;
                d->row = layer ? 0 : mech_imp_row(m);
                d->tintA = TINT[m->team][0];
                d->tintB = TINT[m->team][1];
                d->flash = 0;
                for (int p = 0; p < P_COUNT; p++)
                    if ((p == P_TORSO || p == P_ARM_L || p == P_ARM_R) == layer) d->flash = maxf(d->flash, m->flash[p]);
            }
            if (dist < 260) {
                shadow(s, xfpt(m->world[P_TORSO], V3(0, 1.3f, 0)), 2.6f);
                shadow(s, vadd(m->pos, V3(0, 1.5f, 0)), 2.0f);
            }
        }
        for (int p = 0; p < P_COUNT && !m->far; p++) {
            if (m->gone[p] || (piloting && i == pl.mech && p == P_TORSO)) continue; // you sit inside your own hull
            DrawPart *d = push(s, partShape[p], m->world[p], m->team);
            d->stage = m->stage[p];
            d->ndent = m->ndent[p];
            memcpy(d->dent, m->dent[p], sizeof d->dent);
            memcpy(d->heat, m->heat[p], sizeof d->heat);
            d->seed = m->seed + p;
            d->flash = m->flash[p];
        }
        static const struct { int part; v3 off; float r; } SH[] = {
            {P_TORSO, {0, 1.3f, 0.1f}, 2.0f}, {P_PELVIS, {0, 0, 0}, 1.1f}, {P_ARM_L, {-0.35f, -0.4f, -1.0f}, 1.0f},
            {P_ARM_R, {0.4f, -0.4f, -0.6f}, 1.0f}, {P_SHIN_L, {0, -1, 0}, 0.7f}, {P_SHIN_R, {0, -1, 0}, 0.7f},
            {P_FOOT_L, {0, -0.2f, -0.4f}, 0.8f}, {P_FOOT_R, {0, -0.2f, -0.4f}, 0.8f}};
        if (dist < 260 && !m->far)
            for (unsigned k = 0; k < sizeof SH / sizeof SH[0]; k++)
                if (!m->gone[SH[k].part]) shadow(s, xfpt(m->world[SH[k].part], SH[k].off), SH[k].r);
        if (!piloting && m->team != 1 && m->cook <= 0) { // light pillar over mechs you can board
            float pulse = 0.5f + 0.5f * sinf(gtime * 4);
            push(s, S_BEACON, XF(vadd(m->pos, V3(0, 17 + pulse, 0)), qid()), 0);
        }
    }

    if (piloting) {
        Mech *m = &mech[pl.mech];
        xf a = cockpit_anchor(m);
        push_cabin(s, S_CK_SHELL, a, m->team);
        DrawPart *dash = push_cabin(s, S_CK_DASH, cabin_at(a, S_CK_DASH), m->team);
        memcpy(dash->scr, shapes[S_CK_DASH].scr, sizeof dash->scr);
        for (int p = 0; p < P_COUNT; p++) dash->stat[p] = m->gone[p] ? 3 : (float)m->stage[p];
        dash->aux[0] = m->speed / TOP_SPEED;
        dash->aux[2] = pl.alarm;
        dash->aux[3] = clampf(m->hp[P_TORSO] / 160.0f, 0, 1);
        push_cabin(s, S_CK_SEAT, cabin_at(a, S_CK_SEAT), m->team);
        push_cabin(s, S_CK_SIDE_L, cabin_at(a, S_CK_SIDE_L), m->team);
        push_cabin(s, S_CK_SIDE_R, cabin_at(a, S_CK_SIDE_R), m->team);
        xf thr = xfmul(a, XF(shapes[S_CK_THROTTLE].at, qaxis(V3(1, 0, 0), -pl.throttle * 0.6f)));
        xf stk = xfmul(a, XF(shapes[S_CK_STICK].at, qmul(qaxis(V3(0, 0, 1), -pl.stickX * 0.35f), qaxis(V3(1, 0, 0), -pl.stickY * 0.35f))));
        xf ej = xfmul(a, XF(vadd(shapes[S_CK_EJECT].at, V3(0, -pl.ejectPull, 0)), qid()));
        push_cabin(s, S_CK_THROTTLE, thr, m->team);
        push_cabin(s, S_CK_STICK, stk, m->team);
        push_cabin(s, S_CK_EJECT, ej, m->team);
        for (int h = 0; h < 2 && in->vr; h++) { // gloves; a hand holding a control is drawn on that control
            if (!in->hand[h].tracked) continue;
            xf g = hand_xf(rig, &in->hand[h]);
            if (pl.grab[h] == GRAB_THROTTLE) g.p = xfpt(thr, pnt[PNT_CK_THROTTLE_GRIP]);
            if (pl.grab[h] == GRAB_STICK) g.p = xfpt(stk, pnt[PNT_CK_STICK_GRIP]);
            if (pl.grab[h] == GRAB_EJECT) g.p = ej.p;
            push_cabin(s, pl.grab[h] || in->hand[h].squeeze > 0.5f ? S_FIST : S_HAND, g, 0)->texel = 0.006f;
        }
        if (s->nptc < MAX_DRAW_PTC) { // aiming reticle, drawn at the distance of whatever it rests on
            v3 toHead = vnorm(vsub(s->head, pl.aim));
            v3 rp = vadd(pl.aim, vmul(toHead, minf(1.0f, pl.aimDist * 0.05f)));
            s->ptc[s->nptc++] = (DrawParticle){rp.x, rp.y, rp.z, maxf(0.05f, pl.aimDist * 0.014f), 0, PT_RETICLE, 0, 0};
        }
    } else {
        if (in->hand[1].tracked && pl.mode == MODE_FOOT) push(s, S_PISTOL, pistol_xf(rig, &in->hand[1]), 0)->texel = 0.006f;
        for (int h = 0; h < 2; h++) { // the pistol hand is always closed
            int fist = (h && pl.mode == MODE_FOOT) || in->hand[h].squeeze > 0.5f;
            if (in->hand[h].tracked) push(s, fist ? S_FIST : S_HAND, hand_xf(rig, &in->hand[h]), 0)->texel = 0.006f;
        }
    }

    bots_scene(s);
    for (int i = 0; i < MAXDEB; i++) {
        Debris *b = &deb[i];
        if (!b->active) continue;
        DrawPart *d = push(s, b->shape, b->x, b->team);
        d->stage = 3;
        d->ndent = b->ndent;
        memcpy(d->dent, b->dent, sizeof d->dent);
        memcpy(d->heat, b->heat, sizeof d->heat);
        d->seed = b->seed;
    }
    for (int i = 0; i < NROCK; i++) {
        DrawPart *d = push(s, S_ROCK0 + rocks[i].shape, XF(rocks[i].pos, qaxis(UP, rocks[i].yaw)), 0);
        d->seed = (float)i;
        d->texel = 0.125f;
        if (vlen(vsub(rocks[i].pos, s->head)) < 120) shadow(s, vadd(rocks[i].pos, V3(0, rocks[i].radius * 0.5f, 0)), rocks[i].radius * 0.8f);
    }
    for (int i = 0; i < MAXBOLT; i++)
        if (bolt[i].active) push(s, bolt[i].kind == 2 ? S_SPIT : bolt[i].kind ? S_BOLT : S_TRACER, XF(bolt[i].p, qlook(vnorm(bolt[i].v))), 0);

    for (int i = 0; i < nptc && s->nptc < MAX_DRAW_PTC; i++) {
        const Particle *q = &ptc[i];
        s->ptc[s->nptc++] = (DrawParticle){q->p.x, q->p.y, q->p.z, q->size, q->age / q->life, (float)q->type, q->seed, q->spin};
    }
}

void game_report(void) { // one line for test runs: how the fight is going
    int kind[3] = {0, 0, 0}, mechs = 0;
    for (int i = 0; i < nbot; i++) kind[bot[i].kind] += bot[i].alive;
    for (int i = 0; i < nmech; i++) mechs += mech[i].alive && mech[i].team == 1;
    printf("report: %s, your mech torso %.0f of 160, you %.0f of 100, restarts %d; left: %d enemy mechs, %d drones, %d crawlers, %d turrets\n",
           pl.mode == MODE_PILOT ? "piloting" : "on foot", mech[0].alive ? mech[0].hp[P_TORSO] : 0, pl.hp, deaths, mechs, kind[0], kind[1], kind[2]);
}
