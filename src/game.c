#include "game.h"
#include <stdio.h>
#include "audio.h"
#include "mech.h"
#include "world.h"
#include <string.h>

int game_look = 0;

typedef struct { v3 p, v; float age, life, size, grow, grav, drag, seed, spin; int type; } Particle;
typedef struct { int active, owner, kind; v3 p, v; float life, dmg; } Bolt; // kind: 0 cannon tracer, 1 plasma, 2 crawler spit, 3 spore (falls, bursts), 4 needle
typedef struct {
    int active, shape, team, ndent, rest;
    xf x;
    v3 v, w;
    float dent[8][4], heat[8], seed, age, radius, emit, scale;
    int flip, arm; // drawn mirrored; arm: 1 + the kind of arm this is (it can be grafted onto a mech), or 0
} Debris;
typedef struct { float t; int mech, part, rock, ground; v3 local; int bot, worm; } Hit; // bot: a small enemy, worm: a part of the worm, or -1
enum { PT_SMOKE, PT_FIRE, PT_SPARK, PT_FLASH, PT_DUST, PT_RETICLE, PT_ICHOR };
enum { MODE_FOOT, MODE_PILOT, MODE_EJECT };
enum { GRAB_NONE, GRAB_THROTTLE, GRAB_STICK, GRAB_EJECT };

#define NMECH 133 // 5 in the normal game; the rest are for --horde
#define MAXPTC 3000
#define MAXBOLT 128
#define MAXDEB 64
#define TOP_SPEED 7.5f
static Mech mech[NMECH];
static int nmech = 5, horde, swarm;
static int myBody = CH_MACHINE, myArm[2] = {ARM_CANNON, ARM_PLASMA}; // what your own mech starts as
static float wormDist = 380; // how far ahead of the start it lives; 0 = no worm
static int wormSet;          // --worm was given: in the wave game the worm is then there from the start as well
static int waves = 1;        // the wave game (default); 0 = the fixed sandbox layout
static int hurtLevel;        // test option: your mech starts damaged (1 to 3)
static int firstWave = 1;    // the wave the game starts with
int game_cards = 1; // draw distant mechs as flat cards
int game_outside;
xf game_cam;
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
    int pendingBoard, dead, graft; // graft: a fallen arm within reach of your mech, or -1
    v3 ejV, aim;
    float aimDist, alarm;
    float wornTime;      // seconds the headset has been worn; the seat is fitted to you shortly after
    int fitted;
    float lookYaw;       // which way you are looking, in the world
    float hatch;         // seconds A (E in a window) has been held to climb out
    int hatchBlock, pendingExit; // hatchBlock: A must be let go first (after grafting or boarding)
    int torchMech, torchPart; // what the repair torch is working on, or -1
    float weldT;         // until the next crackle of the torch
} pl;

// ---- waves -------------------------------------------------------------------------------------------
// The game: enemies come in waves that grow, with a break between waves to repair, take arms or change mechs.
#define FIRST_BREAK 15.0f
#define WAVE_BREAK 25.0f
#define HATCH_TIME 0.8f   // hold A this long to climb out
#define TORCH_RANGE 14.0f
#define TORCH_RATE 14.0f  // hit points a second the repair torch puts back
static struct {
    int n, fighting, left, kills, best, worm; // n: the wave under way, or the last one; worm: the worm came with it
    float timer, quiet, alone; // timer: break left. quiet: seconds since the last kill. alone: on foot with no mech to climb into
} wv;
static int imin(int a, int b) { return a < b ? a : b; }

static const v3 TINT[4][2] = {
    {{0.44f, 0.52f, 0.30f}, {0.84f, 0.80f, 0.66f}}, // player: olive and cream
    {{0.66f, 0.17f, 0.15f}, {0.27f, 0.26f, 0.31f}}, // enemy: crimson and charcoal
    {{0.42f, 0.55f, 0.64f}, {0.86f, 0.86f, 0.84f}}, // spare: steel blue and white
    {{0.62f, 0.42f, 0.30f}, {0.88f, 0.79f, 0.60f}}, // sand worm: hide and bone
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
static void ichor(v3 p, v3 n, int count) { // a living part's answer to sparks
    for (int i = 0; i < count; i++)
        spawn(PT_ICHOR, p, vadd(vmul(n, 3 * (0.4f + frand())), rv(2.5f)), 0.5f + frand() * 0.5f, 0.14f + 0.14f * frand(), 0.1f, -9.8f, 0.6f);
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
    v3 base = vadd(V3(m->pos.x, m->pos.y + pl.bob, m->pos.z), qrot(qaxis(UP, m->yaw), shapes[mech_shape(m, P_TORSO)].at));
    return XF(vadd(base, qrot(q, mech_point(m, MP_EYE))), q);
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
float game_throttle(void) { return pl.throttle; }
void game_haptics(float *l, float *r) { *l = hapL; *r = hapR; hapL = hapR = 0; }
static int deaths;
void game_loadout(int body, int armL, int armR) {
    myBody = body & 1;
    myArm[0] = (unsigned)armL % ARM_COUNT;
    myArm[1] = (unsigned)armR % ARM_COUNT;
}
void game_worm(float dist) { wormDist = dist; wormSet = 1; }
void game_waves(int on) { waves = on; }
void game_hurt(int level) { hurtLevel = level < 0 ? 0 : level > 3 ? 3 : level; }
void game_first_wave(int n) { firstWave = n < 1 ? 1 : n > 99 ? 99 : n; }
void game_swarm(int n) { swarm = n < 0 ? 0 : n > 64 ? 64 : n; }
void game_horde(int n) { horde = n < 0 ? 0 : n > NMECH - 5 ? NMECH - 5 : n; }
void game_set_cam(v3 pos, float yaw) { pl.pos = pos; pl.yaw = yaw; pl.fly = 1; pl.mode = MODE_FOOT; mech[pl.mech].piloted = 0; update_rig(); }

// ---- debris, death, damage -----------------------------------------------------------------------
static Debris *add_debris(int shape, xf x, int team, v3 v) {
    Debris *d = &deb[debNext++ % MAXDEB];
    memset(d, 0, sizeof *d);
    d->active = 1; d->shape = shape; d->team = team; d->x = x;
    d->radius = shapes[shape].radius;
    d->scale = 1;
    d->v = vadd(v, rv(1.5f));
    d->w = rv(2.5f);
    d->seed = frand() * 30;
    return d;
}
static void detach(Mech *m, int part, v3 push) {
    Debris *d = add_debris(mech_shape(m, part), m->world[part], m->team, push);
    d->flip = mech_flip(m, part);
    if (part == P_ARM_L || part == P_ARM_R) d->arm = 1 + m->arm[part - P_ARM_L];
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
    if (shapes[mech_shape(m, part)].bio) ichor(wpos, back, 5);
    else sparks(wpos, back, 5, 6, 0.06f);
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

// Something heavy dropped from the sky hits the ground: a ring of dust, a boom, and a jolt if you are close.
static void touchdown(v3 p, float size, float gain) {
    for (int i = 0; i < 30; i++)
        spawn(PT_DUST, vadd(p, V3(frands() * size, 0.4f, frands() * size)), V3(frands() * 2 * size, 1 + frand() * 4, frands() * 2 * size),
              1.6f + frand(), 0.5f * size, 0.5f * size, -2, 1);
    audio_play(SND_BOOM, p, gain);
    audio_play(SND_STEP, p, 1.5f * gain);
    float near = clampf(1 - vlen(vsub(p, rigNow.p)) / 90, 0, 1);
    hapL = maxf(hapL, near);
    hapR = maxf(hapR, near);
}

// ---- small enemies: drones, crawlers and turrets ----------------------------------------------------
// Each is one traced body (the turret: a base and a turning head) with hit points, not a jointed mech.
enum { B_DRONE, B_CRAWLER, B_TURRET };
#define NBOT 160
#define BOT_OWNER 1000 // a shot's owner is a mech index, -1 for your pistol, or BOT_OWNER + bot index
typedef struct {
    int kind, alive, wreck, aggro, burst, ndent;
    int wave;            // came with the current wave
    float drop;          // a turret being dropped in: metres still to fall (vel.y is how fast)
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

// ---- the sand worm ------------------------------------------------------------------------------------
// A head, a row of body rings and a tail, all following the path the head has taken. It cruises under the
// sand (you see a mound, dust and hear thumps), then breaches: a long arc onto its prey, or straight up
// from underneath when it is close. The inside of its mouth is its weak spot.
#define WSEG 22            // body rings between the head and the tail
#define WPARTS (WSEG + 2)
#define WPATH 256
#define WSTEP 0.6f         // metres between remembered head positions
typedef struct {
    int alive, breach, aggro, bit, pass, top; // pass: running clear after overshooting its prey. top: newest entry in path
    v3 p, v, home;
    float yaw, hp, timer, thump, strike, acc, chew;
    v3 path[WPATH];
    float dent[WPARTS][4][4], heat[WPARTS][4], flash[WPARTS];
    int ndent[WPARTS];
} Worm;
static Worm worm;
static xf wormX[WPARTS];     // where each part is this frame
static float wormS[WPARTS];  // and how large
static int wormVis[WPARTS];  // and whether any of it is above the sand

static int worm_shape(int k) { return k == 0 ? S_WORM_HEAD : k == WPARTS - 1 ? S_WORM_TAIL : S_WORM_SEG; }
static v3 worm_at(float d) { // the point d metres back along the body
    const Worm *w = &worm;
    if (d <= w->acc) return vadd(w->p, vmul(vsub(w->path[w->top], w->p), d / maxf(w->acc, 1e-4f)));
    d = (d - w->acc) / WSTEP;
    int i = (int)d;
    if (i >= WPATH - 2) i = WPATH - 2;
    v3 a = w->path[(w->top - i + WPATH) % WPATH], b = w->path[(w->top - i - 1 + WPATH) % WPATH];
    return vadd(a, vmul(vsub(b, a), minf(d - i, 1)));
}
static void worm_place(void) {
    float d = 0, prev = 1;
    for (int k = 0; k < WPARTS; k++) {
        float t = (float)k / (WPARTS - 1), s = t < 0.45f ? 1 : mixf(1, 0.42f, (t - 0.45f) / 0.55f);
        if (k == WPARTS - 1) s = prev;
        d += k == 0 ? 0 : k == 1 ? 3.7f : k == WPARTS - 1 ? 1.4f * s : 1.8f * (prev + s);
        v3 p = worm_at(d), dir = k == 0 ? worm.v : vsub(worm_at(maxf(d - 1.2f, 0)), worm_at(d + 1.2f));
        wormX[k] = XF(p, qlook(vlen(dir) > 1e-4f ? vnorm(dir) : yawFwd(worm.yaw)));
        wormS[k] = s;
        wormVis[k] = worm.alive && p.y + 3.6f * s > world_height(p.x, p.z) - 0.5f;
        prev = s;
    }
}
static void worm_die(void) {
    Worm *w = &worm;
    for (int k = 0; k < WPARTS; k++) {
        if (!wormVis[k]) continue;
        v3 p = wormX[k].p;
        float s = wormS[k];
        spawn(PT_FLASH, p, V3(0, 0, 0), 0.2f, 7 * s, 0, 0, 0);
        for (int i = 0; i < 5; i++) fire(vadd(p, rv(2.5f * s)), 2.2f * s);
        for (int i = 0; i < 3; i++) smoke(vadd(p, rv(3 * s)), 2.0f * s);
        Debris *d = add_debris(worm_shape(k), wormX[k], 3, vadd(rv(5), V3(0, 4 + frand() * 5, 0)));
        d->scale = s;
        d->radius *= s;
        d->ndent = w->ndent[k];
        memcpy(d->dent, w->dent[k], sizeof w->dent[k]);
        memcpy(d->heat, w->heat[k], sizeof w->heat[k]);
    }
    audio_play(SND_ROAR, w->p, 2.5f);
    audio_play(SND_BOOM, w->p, 2.0f);
    hapL = hapR = 1;
    w->alive = 0;
    worm_place();
}
static void worm_spawn(v3 at, v3 home, float yaw, float hp) { // under the sand at `at`, its body trailing behind
    memset(&worm, 0, sizeof worm);
    worm.alive = 1;
    worm.hp = hp;
    worm.home = home;
    worm.yaw = yaw;
    worm.p = V3(at.x, world_height(at.x, at.z) - 9, at.z);
    worm.timer = 4;
    for (int i = 0; i < WPATH; i++) worm.path[i] = vadd(worm.p, vmul(yawFwd(yaw), -WSTEP * (float)((WPATH - i) % WPATH)));
    worm_place();
}
static void worm_hit(int k, v3 local, v3 wpos, v3 dir, float dmg) {
    Worm *w = &worm;
    v3 back = vmul(dir, -1);
    int soft = k == 0 && local.z < -2.6f && local.x * local.x + local.y * local.y < 2.7f * 2.7f; // inside the mouth
    w->hp -= dmg * (soft ? 3 : 1);
    w->flash[k] = 1;
    w->aggro = 1;
    spawn(PT_FLASH, vadd(wpos, vmul(back, 0.3f)), V3(0, 0, 0), 0.1f, (soft ? 2.2f : 0.9f) + dmg * 0.03f, 0, 0, 0);
    sparks(wpos, back, soft ? 14 : 5, 7, 0.08f);
    audio_play(SND_HIT, wpos, 0.4f + dmg * 0.02f);
    int j = w->ndent[k] < 4 ? w->ndent[k]++ : (int)(frand() * 3.99f);
    w->dent[k][j][0] = local.x; w->dent[k][j][1] = local.y; w->dent[k][j][2] = local.z;
    w->dent[k][j][3] = clampf((0.13f + 0.011f * dmg) * 2.4f, 0.35f, 0.95f);
    w->heat[k][j] = 1;
    if (w->hp <= 0) worm_die();
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
    mech_outfit(&mech[0], myBody, myArm[0], myArm[1]);
    for (int a = 0; a < 2; a++) mech[2].cool[a] = mech[3].cool[a] = 2;
    mech_outfit(&mech[2], CH_MACHINE, ARM_NEEDLER, ARM_PLASMA); // each mech can be built differently
    mech_outfit(&mech[3], CH_BIO, ARM_SPORE, ARM_NEEDLER);
    mech_outfit(&mech[4], CH_BIO, ARM_NEEDLER, ARM_SPORE);       // the spare is a living one
    if (waves) { // the wave game starts with only your mech and the spare; everything else arrives in waves
        mech[2].alive = mech[3].alive = 0;
        if (!demo) mech[1].alive = 0; // the practice target stays only for --demo pictures
    }
    for (int p = 0; p < P_COUNT && hurtLevel; p++) { // test option: your mech starts battered, to try repairing it
        Mech *m = &mech[0];
        float keep = hurtLevel == 1 ? 0.6f : hurtLevel == 2 ? 0.45f : 0.25f;
        for (int k = 0; k < 60 && m->hp[p] > keep * PART_HP(p); k++) {
            v3 c = xfpt(m->world[p], shapes[mech_shape(m, p)].boxC), ro = vadd(c, vmul(vnorm(V3(frands(), frands() * 0.6f, frands())), 9)), l;
            float t;
            int part;
            if (mech_ray(m, ro, vnorm(vsub(c, ro)), 30, &t, &part, &l) && part == p) mech_damage(m, p, l, 9);
        }
    }
    nmech = 5 + horde;
    for (int i = 5; i < nmech; i++) { // test crowd: enemy patrols scattered ahead, 130 to 600 m out
        float a = frands() * 1.3f, r = 130 + frand() * 470;
        mech_init(&mech[i], V3(sinf(a) * r, 0, -cosf(a) * r), frand() * 6.28f, 1);
        mech[i].throttle = 2.6f + frand();
        mech[i].aiTurn = frands() * 0.15f;
        mech[i].cool[0] = mech[i].cool[1] = 2;
        mech_outfit(&mech[i], i & 1, (i * 7) % ARM_COUNT, (i * 3 + 1) % ARM_COUNT);
    }
    // Small enemies: three turrets far out, crawler packs and drone flights between you and the patrols.
    static const struct { int kind, n; float x, z; } GROUP[] = {
        {B_TURRET, 1, -150, -210}, {B_TURRET, 1, 130, -240}, {B_TURRET, 1, 10, -330},
        {B_CRAWLER, 4, 45, -95}, {B_CRAWLER, 4, -95, -70}, {B_CRAWLER, 3, 0, -190},
        {B_DRONE, 3, -60, -190}, {B_DRONE, 3, 110, -170}};
    memset(bot, 0, sizeof bot);
    nbot = 0;
    for (unsigned g = waves ? sizeof GROUP / sizeof GROUP[0] : 0; g < sizeof GROUP / sizeof GROUP[0] + 2 * swarm; g++) {
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
    memset(&worm, 0, sizeof worm);
    if (wormDist > 0 && (!waves || wormSet)) worm_spawn(V3(110, 0, -wormDist), V3(0, 0, -wormDist), 0, 900); // under the sand on its home ground, far ahead
    worm_place();
    pl.hp = 100;
    pl.fade = 1;
    pl.fadeDir = -1;
    pl.pendingBoard = -1;
    pl.graft = -1;
    pl.torchMech = -1;
    int best = wv.best;
    memset(&wv, 0, sizeof wv);
    wv.best = best;
    wv.n = firstWave - 1;
    wv.timer = FIRST_BREAK;
    pl.calib = XF(V3(0, 1.7f, 0), qid());
    if (once) { pl.calib = keepCalib; pl.footLift = keepLift; pl.wornTime = keepWorn; pl.fitted = keepFit; } // a restart keeps your seat fit
    once = 1;
    pl.bob = mech[0].hipH;
    if (startOnFoot) { pl.mode = MODE_FOOT; pl.pos = V3(5, 0, 5); }
    else { pl.mode = MODE_PILOT; mech[0].piloted = 1; }
    for (int i = 0; i < demo * 8; i++) { // demo damage for screenshots: synthetic shots at the practice target
        Mech *m = &mech[1];
        int want = i % 3 == 0 ? P_ARM_R : i % 3 == 1 ? P_ARM_L : P_TORSO;
        v3 ro = V3(frands() * 3, 5 + frands() * 2, -30), tgt = vadd(xfpt(m->world[want], shapes[mech_shape(m, want)].boxC), rv(0.5f));
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
    Hit h = {tmax, -1, 0, 0, 0, {0, 0, 0}, -1, -1};
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
    for (int k = 0; k < WPARTS; k++) {
        if (!wormVis[k]) continue;
        const Shape *sp = &shapes[worm_shape(k)];
        float sc = wormS[k], t, r = (vlen(sp->boxH) + vlen(sp->boxC)) * sc;
        v3 oc = vsub(wormX[k].p, ro), l;
        float along = vdot(oc, rd);
        if (along < -r || along > h.t + r || vdot(oc, oc) - along * along > r * r) continue;
        xf inv = xfinv(wormX[k]); // the shape is drawn enlarged, so shrink the ray instead
        if (shape_ray(sp, XF(V3(0, 0, 0), qid()), vmul(xfpt(inv, ro), 1 / sc), qrot(inv.q, rd), h.t / sc, worm.dent[k], worm.ndent[k], &t, &l)) {
            h.t = t * sc; h.mech = -1; h.bot = -1; h.rock = 0; h.worm = k; h.local = l;
        }
    }
    for (int k = 0; k < NROCK; k++) {
        v3 oc = vsub(rocks[k].pos, ro), l;
        float along = vdot(oc, rd), t;
        if (along < -7 || along > h.t + 7 || vdot(oc, oc) - along * along > 49) continue;
        if (shape_ray(&shapes[S_ROCK0 + rocks[k].shape], XF(rocks[k].pos, qaxis(UP, rocks[k].yaw)), ro, rd, h.t, NULL, 0, &t, &l)) {
            h.t = t; h.mech = -1; h.bot = -1; h.worm = -1; h.rock = 1;
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
            h.t = b; h.mech = -1; h.bot = -1; h.worm = -1; h.rock = 0; h.ground = 1;
            break;
        }
    }
    return h;
}

#define SPORE_FALL 18.0f // how fast a lobbed spore drops, m/s2
static void fire_bolt(int owner, int kind, v3 p, v3 dir, float speed, float dmg) {
    for (int i = 0; i < MAXBOLT; i++)
        if (!bolt[i].active) {
            bolt[i] = (Bolt){1, owner, kind, p, vmul(dir, speed), 3.0f, dmg};
            return;
        }
}
// Fires one of a mech's arm weapons at a point. arm 0: twin autocannon, arm 1: plasma pod.
// A spore bursts where it lands and hurts whatever is close.
static void spore_burst(v3 p, int owner) {
    spawn(PT_FLASH, p, V3(0, 0, 0), 0.12f, 2.4f, 0, 0, 0);
    ichor(p, UP, 16);
    for (int i = 0; i < 4; i++) smoke(p, 0.9f);
    audio_play(SND_HIT, p, 0.9f);
    for (int k = 0; k < nmech; k++) {
        Mech *m = &mech[k];
        v3 c = xfpt(m->world[P_TORSO], V3(0, 1, 0));
        float d = vlen(vsub(c, p));
        if (k == owner || !m->alive || d > 6) continue;
        int part = p.y < m->world[P_PELVIS].p.y ? P_THIGH_L + 3 * (int)(frand() * 1.99f) : P_TORSO;
        apply_hit(m, part, shapes[mech_shape(m, part)].boxC, vadd(c, vmul(vsub(p, c), 0.5f)), vnorm(vsub(c, p)), 12 * (1 - d / 6));
    }
    for (int k = 0; k < nbot; k++)
        if (bot[k].alive && owner != BOT_OWNER + k && vlen(vsub(bot_xf(&bot[k]).p, p)) < 4.5f)
            bot_hit(&bot[k], 1, V3(0, 0, 0), bot_xf(&bot[k]).p, UP, 12);
}

// Fires one of a mech's arms at a point. What comes out depends on the kind of arm mounted there.
static int mech_fire(int idx, int arm, v3 target, float spread) {
    Mech *m = &mech[idx];
    if (m->gone[P_ARM_L + arm]) return 0;
    const ArmType *t = &armType[m->arm[arm]];
    v3 mz = mech_muzzle(m, arm);
    if (m->arm[arm] == ARM_CANNON) mz.x += (pl.barrel++ & 1) ? 0.2f : -0.2f; // its two barrels take turns
    v3 muzzle = xfpt(m->world[P_ARM_L + arm], mz);
    v3 to = vsub(target, muzzle);
    float len = vlen(to);
    if (t->bolt == 3) { // lobbed: aim above the target so the arc comes down on it
        float flat = sqrtf(to.x * to.x + to.z * to.z);
        to.y += flat * tanf(0.5f * asinf(clampf(SPORE_FALL * flat / (t->speed * t->speed), 0, 1)));
    }
    for (int k = 0; k < t->shots; k++) {
        v3 fan = t->shots > 1 ? vmul(qrot(m->world[P_TORSO].q, V3(1, 0, 0)), (k - (t->shots - 1) * 0.5f) * 0.035f * len) : V3(0, 0, 0);
        v3 dir = vnorm(vadd(vadd(to, fan), rv(spread * len)));
        fire_bolt(idx, t->bolt, muzzle, dir, t->speed, t->dmg);
    }
    v3 fwd = vnorm(to);
    spawn(PT_FLASH, vadd(muzzle, vmul(fwd, 0.3f)), V3(0, 0, 0), t->dmg > 20 ? 0.12f : 0.07f, t->dmg > 20 ? 1.5f : 0.8f, 0, 0, 0);
    if (shapes[t->shape].bio) ichor(muzzle, fwd, 3);
    audio_play(t->snd, muzzle, t->dmg > 20 ? 0.9f : 0.55f);
    return 1;
}

// From the cockpit: stand over an arm lying on the ground and take it. It goes on an empty shoulder if you
// have one, otherwise it replaces the arm on the side it lies, and that arm drops in its place.
static void graft_update(const Input *in, Mech *m) {
    float best = 9;
    pl.graft = -1;
    for (int i = 0; i < MAXDEB; i++) {
        float dx = deb[i].x.p.x - m->pos.x, dz = deb[i].x.p.z - m->pos.z, d = sqrtf(dx * dx + dz * dz);
        if (deb[i].active && deb[i].arm && deb[i].rest && d < best) { best = d; pl.graft = i; }
    }
    if (pl.graft < 0 || !in->boardKey) return;
    Debris *d = &deb[pl.graft];
    v3 right = qrot(qaxis(UP, m->yaw + m->torsoYaw), V3(1, 0, 0));
    int side = vdot(vsub(d->x.p, m->pos), right) > 0;
    if (m->gone[P_ARM_L] != m->gone[P_ARM_R]) side = m->gone[P_ARM_R];
    int part = P_ARM_L + side, kind = d->arm - 1, n = d->ndent;
    float dent[8][4];
    memcpy(dent, d->dent, sizeof dent);
    d->active = 0;
    if (!m->gone[part]) detach(m, part, vadd(vmul(right, side ? 5.0f : -5.0f), V3(0, 3, 0)));
    m->gone[part] = 0;
    m->arm[side] = kind;
    m->hp[part] = 70; // it has been through a fight already
    m->stage[part] = 0;
    m->ndent[part] = n; // and still shows it
    memcpy(m->dent[part], dent, sizeof dent);
    memset(m->heat[part], 0, sizeof m->heat[part]);
    m->flash[part] = 1;
    m->cool[side] = 0.6f;
    m->burst[side] = 0;
    mech_pose(m, 0);
    v3 at = m->world[part].p;
    if (shapes[armType[kind].shape].bio) ichor(at, UP, 14);
    else sparks(at, UP, 14, 6, 0.06f);
    audio_play(SND_CLICK, at, 1.5f);
    audio_play(SND_WHOOSH, at, 0.8f);
    hapL = hapR = 0.8f;
    pl.graft = -1;
    pl.hatchBlock = 1; // the same press must not go on to climb out
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
    add_debris(mech_cockpit(m, CKP_SHELL), a, m->team, vadd(V3(0, 9, 0), vmul(yawFwd(m->yaw), 3)));
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
    if (dist < 120 || m->wave) m->aggro = 1; // a mech that came with a wave comes for you wherever you are
    if (game_calm) m->aggro = 0;
    if (!m->aggro || (dist > 320 && !m->wave)) { // patrol in a wide circle
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
    for (int a = 0; a < 2; a++) { // each arm fires a short burst, then rests
        const ArmType *t = &armType[m->arm[a]];
        m->cool[a] -= dt;
        if (m->cool[a] > 0 || !aligned || dist > t->range || m->gone[P_ARM_L + a]) continue;
        mech_fire(idx, a, tgt, spread);
        if (++m->burst[a] >= t->aiBurst) { m->burst[a] = 0; m->cool[a] = t->aiPause * (1 + 0.7f * frand()); }
        else m->cool[a] = maxf(t->interval, 0.14f);
    }
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
        if (b->drop > 0) { // a turret on its way down: braking jets for the last stretch, then a hard landing
            if (b->drop < 26) b->vel.y = approach(b->vel.y, -6, 34 * dt);
            b->drop = maxf(0, b->drop + b->vel.y * dt);
            b->pos.y = world_height(b->pos.x, b->pos.z) + b->drop;
            if (b->drop < 26 && frand() < dt * 40) fire(vadd(b->pos, V3(frands() * 1.4f, -0.4f, frands() * 1.4f)), 0.8f);
            if (b->drop <= 0) {
                b->vel = V3(0, 0, 0);
                touchdown(b->pos, 4, 1.0f);
            }
            continue;
        }
        b->cool -= dt;
        b->phase += dt;
        v3 to = vsub(tgt, b->pos);
        float dist = sqrtf(to.x * to.x + to.z * to.z), bearing = atan2f(-to.x, -to.z);
        float side = (i & 1) ? 1.0f : -1.0f;
        if (dist < BOT[b->kind].aggro || b->wave) b->aggro = 1;
        if (game_calm == 1 || pl.dead || pl.fly) b->aggro = 0;
        int hunt = b->aggro && (dist < 320 || b->wave);
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

static void worm_update(float dt, v3 tgt, int onFoot) {
    Worm *w = &worm;
    if (!w->alive) return;
    for (int k = 0; k < WPARTS; k++) {
        w->flash[k] = maxf(0, w->flash[k] - dt * 7);
        for (int j = 0; j < w->ndent[k]; j++) w->heat[k][j] = maxf(0, w->heat[k][j] - dt * 0.45f);
    }
    float ground = world_height(w->p.x, w->p.z), depth = ground - 9;
    v3 to = vsub(tgt, w->p);
    float dist = sqrtf(to.x * to.x + to.z * to.z);
    float near = clampf(1 - vlen(vsub(w->p, rigNow.p)) / 140, 0, 1); // how strongly you feel it
    int hunts = waves && wv.worm; // one that came with a wave hunts you wherever you are
    if (dist < 220 || hunts) w->aggro = 1;
    if (game_calm == 1 || pl.dead || pl.fly || (dist > 500 && !hunts)) w->aggro = 0;
    v3 f = yawFwd(w->yaw);
    if (!w->breach) {
        // Under the sand: head for its prey, or circle its home ground when it has none.
        v3 goal = w->aggro ? tgt : V3(w->home.x + sinf(gtime * 0.13f) * 110, 0, w->home.z + cosf(gtime * 0.13f) * 110);
        float want = atan2f(-(goal.x - w->p.x), -(goal.z - w->p.z));
        // It cannot turn tightly, so after passing under its prey it runs on, swings round and comes back.
        if (!w->aggro || dist > 75) w->pass = 0;
        else if (dist < 40 && fabsf(wrap(want - w->yaw)) > 1.0f) w->pass = 1;
        if (!w->pass) w->yaw = wrap(w->yaw + clampf(wrap(want - w->yaw), -0.5f * dt, 0.5f * dt));
        w->v = V3(f.x * 15, clampf((depth - w->p.y) * 1.5f, -8, 8), f.z * 15);
        w->timer -= dt;
        int under = w->aggro && dist < 14, lunge = !w->aggro || (dist > 50 && dist < 95 && fabsf(wrap(want - w->yaw)) < 0.3f);
        if (w->timer <= 0 && w->p.y < depth + 2 && (under || lunge)) {
            w->breach = 1;
            w->bit = 0;
            if (w->aggro && !under) w->yaw = want; // commits to where its prey is at this moment
            w->strike = under ? 4.0f : w->aggro ? clampf(dist / 4.9f, 9, 20) : 15.0f; // straight up from below, or an arc that comes down where the prey is now
            w->v.y = under ? 22.0f : 24.5f;
            audio_play(SND_ROAR, V3(w->p.x, ground, w->p.z), 2.2f);
        }
        w->thump -= dt;
        if (w->thump <= 0) { // what you hear and see of it while it is under: thumps, puffs of dust
            w->thump = 0.55f;
            audio_play(SND_STEP, V3(w->p.x, ground, w->p.z), 1.6f);
            for (int i = 0; i < 3; i++)
                spawn(PT_DUST, V3(w->p.x + frands() * 5, ground + 0.5f, w->p.z + frands() * 5), V3(frands(), 1.5f + frand(), frands()), 1.6f, 1.6f, 1.5f, -0.8f, 1);
            if (near > 0.3f) hapL = hapR = maxf(hapL, near * 0.35f);
        }
    } else {
        w->v = V3(f.x * w->strike, w->v.y - 9 * dt, f.z * w->strike);
        if (w->v.y < 0 && w->p.y < depth) { w->breach = 0; w->timer = 7 + frand() * 4; }
    }
    int wasUnder = w->p.y < ground;
    w->p = vadd(w->p, vmul(w->v, dt));
    w->acc += vlen(w->v) * dt;
    while (w->acc >= WSTEP) { w->top = (w->top + 1) % WPATH; w->path[w->top] = w->p; w->acc -= WSTEP; }
    float g2 = world_height(w->p.x, w->p.z);
    if (wasUnder != (w->p.y < g2)) { // the head bursts out of, or back into, the sand
        v3 c = V3(w->p.x, g2 + 1, w->p.z);
        for (int i = 0; i < 26; i++)
            spawn(PT_DUST, vadd(c, V3(frands() * 5, 0, frands() * 5)), V3(frands() * 7, 3 + frand() * 9, frands() * 7), 2.0f + frand() * 1.5f, 3.0f, 3.0f, -3.0f, 1);
        audio_play(SND_BOOM, c, 1.6f);
        hapL = hapR = maxf(hapL, near);
    }
    worm_place();
    for (int k = 0; k < WPARTS; k++) { // sand pours off wherever the body crosses the surface
        v3 p = wormX[k].p;
        float gy = world_height(p.x, p.z), s = wormS[k];
        if (fabsf(p.y - gy) < 3.2f * s && frand() < dt * 12)
            spawn(PT_DUST, V3(p.x + frands() * 3 * s, gy + 0.5f, p.z + frands() * 3 * s), V3(frands() * 2, 2 + frand() * 3, frands() * 2), 1.5f + frand(), 2.4f * s, 2.2f, -1.5f, 1);
    }
    // The bite: whatever is in its mouth while the head is out of the sand.
    v3 maw = xfpt(wormX[0], pnt[PNT_WORM_HEAD_MAW]), dir = vnorm(w->v);
    if (!w->breach || maw.y < world_height(maw.x, maw.z) - 1) return;
    if (!w->bit && !pl.dead && !pl.fly && vlen(vsub(tgt, maw)) < (onFoot ? 5.0f : 7.0f)) {
        w->bit = 1; // once per breach
        audio_play(SND_BOOM, maw, 1.5f);
        hapL = hapR = 1;
        pl.hurt = 1;
        if (onFoot) {
            pl.hp -= 75;
            if (pl.hp <= 0 && pl.mode == MODE_FOOT) { pl.dead = 1; pl.fadeDir = 1; }
        } else {
            Mech *m = &mech[pl.mech];
            apply_hit(m, P_TORSO, shapes[mech_shape(m, P_TORSO)].boxC, tgt, dir, 45);
            if (m->alive) apply_hit(m, P_THIGH_L, shapes[mech_shape(m, P_THIGH_L)].boxC, m->world[P_THIGH_L].p, dir, 30);
        }
    }
    w->chew -= dt;
    if (w->chew > 0) return;
    for (int i = 0; i < nbot; i++) // it is no friend of the machines either
        if (bot[i].alive && vlen(vsub(bot_xf(&bot[i]).p, maw)) < 5.5f) { bot_die(&bot[i], dir); w->chew = 0.5f; }
    for (int i = 0; i < nmech; i++) {
        Mech *m = &mech[i];
        if (!m->alive || (pl.mode == MODE_PILOT && i == pl.mech) || vlen(vsub(xfpt(m->world[P_TORSO], V3(0, 1.3f, 0)), maw)) > 7) continue;
        apply_hit(m, P_TORSO, shapes[mech_shape(m, P_TORSO)].boxC, m->world[P_TORSO].p, dir, 55);
        w->chew = 0.8f;
    }
}

// ---- waves ---------------------------------------------------------------------------------------------
static v3 player_ground(void) { // the point on the ground under you, or under the mech you are in
    if (pl.mode == MODE_PILOT) return mech[pl.mech].pos;
    return V3(rigNow.p.x, world_height(rigNow.p.x, rigNow.p.z), rigNow.p.z);
}
static v3 out_from(v3 from, float yaw, float dist) { // a point on the ground this far away in this direction
    v3 p = vadd(from, vmul(yawFwd(yaw), dist));
    p.y = world_height(p.x, p.z);
    return p;
}
static float yaw_to(v3 from, v3 to) { return atan2f(-(to.x - from.x), -(to.z - from.z)); }
// A mech dropped from the sky at `at`. It takes the slot of one that has been destroyed, or a new one.
static Mech *drop_mech(v3 at, float yaw, int team, int chassis, int armL, int armR) {
    int i = 1;
    while (i < nmech && (mech[i].alive || i == pl.mech)) i++;
    if (i == nmech) { if (nmech == NMECH) return NULL; nmech++; }
    Mech *m = &mech[i];
    mech_init(m, at, yaw, team);
    mech_outfit(m, chassis, armL, armR);
    m->drop = 120;
    m->dropV = -40;
    m->cool[0] = m->cool[1] = 2;
    mech_pose(m, 0);
    audio_play(SND_WHOOSH, vadd(at, V3(0, 60, 0)), 1.5f);
    return m;
}
static Bot *add_bot(int kind, v3 at, float yaw) {
    int i = 0;
    while (i < nbot && (bot[i].alive || bot[i].wreck)) i++;
    if (i == nbot) {
        if (nbot < NBOT) nbot++;
        else for (i = 0; i < nbot && bot[i].alive; i++) {} // a burnt-out turret makes way
        if (i == NBOT) return NULL;
    }
    Bot *b = &bot[i];
    memset(b, 0, sizeof *b);
    b->kind = kind;
    b->alive = 1;
    b->hp = BOT[kind].hp;
    b->seed = (i + 1) * 7.3f;
    b->phase = (i + 1) * 1.7f;
    b->yaw = b->aim = yaw;
    b->home = V3(at.x, world_height(at.x, at.z), at.z);
    b->pos = vadd(b->home, V3(0, kind == B_DRONE ? 20.0f : 0, 0));
    return b;
}
static void send_spare(void) { // a fresh mech of your own side, dropped a short walk away
    v3 at = out_from(player_ground(), pl.lookYaw + 0.7f, 24);
    collide_rocks(&at, 3);
    drop_mech(at, pl.lookYaw, 2, frand() < 0.5f, (int)(frand() * 3.99f), (int)(frand() * 3.99f));
}
static void wave_start(v3 ear) {
    int n = ++wv.n, wormWave = n % 5 == 0;
    int crawlers = imin(3 + n, 14), drones = n < 2 ? 0 : imin(n, 10), mechs = n < 3 ? 0 : imin((n - 1) / 2, 5), turrets = n < 4 ? 0 : imin((n - 1) / 3, 4);
    if (wormWave) { crawlers /= 2; drones /= 2; mechs /= 2; turrets = 0; }
    v3 me = player_ground();
    int spawned = 0;
    float front = pl.lookYaw + frands() * 0.8f;                                         // most come from where you are looking
    float flank = front + (frand() < 0.5f ? 1.0f : -1.0f) * (1.7f + 0.6f * frand());   // from wave 6, some from the side
    for (int k = 0; k < crawlers; k += 4) { // packs of up to four
        float a = (n >= 6 && ((k / 4) & 1) ? flank : front) + frands() * 0.2f;
        v3 c = out_from(me, a, 150 + 50 * frand());
        for (int j = k; j < k + 4 && j < crawlers; j++) {
            Bot *b = add_bot(B_CRAWLER, vadd(c, V3(frands() * 6, 0, frands() * 6)), a + PI);
            if (b) { b->wave = b->aggro = 1; spawned++; }
        }
    }
    for (int k = 0; k < drones; k += 3) { // flights of up to three
        float a = (n >= 6 && ((k / 3) & 1) ? flank : front) + frands() * 0.3f;
        v3 c = out_from(me, a, 200 + 40 * frand());
        for (int j = k; j < k + 3 && j < drones; j++) {
            Bot *b = add_bot(B_DRONE, vadd(c, V3(frands() * 10, 0, frands() * 10)), a + PI);
            if (b) { b->wave = b->aggro = 1; spawned++; }
        }
    }
    for (int k = 0; k < mechs; k++) { // dropped from the sky, close enough to see them come down
        v3 at = out_from(me, (n >= 6 && (k & 1) ? flank : front) + frands() * 0.45f, 130 + 40 * frand());
        collide_rocks(&at, 3);
        Mech *m = drop_mech(at, yaw_to(at, me), 1, frand() < 0.5f, (int)(frand() * 3.99f), (int)(frand() * 3.99f));
        if (m) { m->wave = m->aggro = 1; m->aiTurn = frands() * 0.15f; spawned++; }
    }
    for (int k = 0; k < turrets; k++) { // dropped where they can reach you
        v3 at = out_from(me, front + frands() * 0.9f, 95 + 45 * frand());
        collide_rocks(&at, 3);
        Bot *b = add_bot(B_TURRET, at, yaw_to(at, me));
        if (b) { b->wave = b->aggro = 1; b->drop = 110; b->vel.y = -38; b->pos.y += b->drop; spawned++; }
    }
    wv.worm = wormWave;
    if (wormWave) { // it rises from the sand far ahead, angrier each time
        v3 at = out_from(me, front + frands() * 0.6f, 230);
        worm_spawn(at, at, yaw_to(at, me), 900 + 300 * (n / 5 - 1));
        worm.aggro = 1;
    }
    wv.fighting = 1;
    wv.quiet = 0;
    wv.left = spawned + wormWave;
    audio_play(SND_HORN, ear, 1.0f);
    printf("waves: %.0f s: wave %d: %d crawlers, %d drones, %d mechs, %d turrets%s\n", gtime, n, crawlers, drones, mechs, turrets, wormWave ? " and the worm" : "");
}
static void waves_update(float dt, v3 ear) {
    if (!waves || pl.fly || pl.dead) return;
    int haveMech = 0; // on foot with no mech left to climb into: another is sent before long
    for (int i = 0; i < nmech; i++) haveMech |= mech[i].alive && mech[i].team != 1 && mech[i].cook <= 0;
    wv.alone = pl.mode == MODE_FOOT && !haveMech ? wv.alone + dt : 0;
    if (wv.alone > 25) { send_spare(); wv.alone = 0; printf("waves: %.0f s: you have no mech; a spare is on its way\n", gtime); }
    if (!wv.fighting) {
        float before = wv.timer;
        wv.timer -= dt;
        if (wv.timer > 0 && wv.timer < 3 && ceilf(wv.timer) != ceilf(before)) audio_play(SND_BEEP, ear, 0.8f); // three, two, one
        if (wv.timer <= 0) wave_start(ear);
        return;
    }
    int left = wv.worm && worm.alive;
    for (int i = 0; i < nmech; i++) left += mech[i].alive && mech[i].wave;
    for (int i = 0; i < nbot; i++) left += bot[i].alive && bot[i].wave;
    if (left < wv.left) { wv.kills += wv.left - left; wv.quiet = 0; }
    else wv.quiet += dt;
    wv.left = left;
    if (left) return;
    wv.fighting = 0; // beaten: a break, and a mech to change into if you have none to spare
    wv.timer = WAVE_BREAK;
    if (wv.n > wv.best) wv.best = wv.n;
    audio_play(SND_CHIME, ear, 1.2f);
    int spare = 0;
    for (int i = 0; i < nmech; i++) spare |= mech[i].alive && mech[i].team != 1 && mech[i].cook <= 0 && !(pl.mode == MODE_PILOT && i == pl.mech);
    if (!spare) send_spare();
    printf("waves: %.0f s: wave %d beaten, %d kills so far%s\n", gtime, wv.n, wv.kills, spare ? "" : "; a spare mech is on its way");
}
// A mech on its way down: braking jets for the last stretch, then a hard landing.
static void mech_fall(Mech *m, float dt) {
    for (int p = 0; p < P_COUNT; p++) m->flash[p] = maxf(0, m->flash[p] - dt * 7);
    if (m->drop < 36) m->dropV = approach(m->dropV, -7, 26 * dt);
    m->drop = maxf(0, m->drop + m->dropV * dt);
    if (m->drop < 36)
        for (int k = P_FOOT_L; k <= P_FOOT_R; k += P_FOOT_R - P_FOOT_L)
            if (frand() < dt * 40) fire(vadd(m->world[k].p, V3(frands() * 0.5f, -0.6f, frands() * 0.5f)), 1.0f);
    if (m->drop <= 0) { m->dropV = 0; touchdown(m->pos, 7, 1.4f); }
    mech_pose(m, 0);
}

// ---- update ----------------------------------------------------------------------------------------
// Puts you on foot with your head over `spot`, looking along `face`.
static void place_on_foot(const Input *in, v3 spot, float face) {
    pl.yaw = face - qyaw(in->head.q);
    v3 h = qrot(qaxis(UP, pl.yaw), V3(in->head.p.x, 0, in->head.p.z));
    pl.pos = V3(spot.x - h.x, world_height(spot.x, spot.z), spot.z - h.z);
    pl.mode = MODE_FOOT;
    update_rig();
}
// Climbing out leaves the mech standing, unlike the eject, so it can be repaired; you land beside it, facing it.
static void climb_out(const Input *in) {
    Mech *m = &mech[pl.mech];
    m->piloted = 0;
    m->throttle = 0;
    pl.throttle = pl.stickX = pl.stickY = pl.ejectPull = 0;
    pl.grab[0] = pl.grab[1] = GRAB_NONE;
    v3 spot = vadd(m->pos, vadd(qrot(qaxis(UP, m->yaw), V3(-6.5f, 0, 0)), vmul(yawFwd(m->yaw), 1.5f)));
    collide_rocks(&spot, 0.5f);
    place_on_foot(in, spot, yaw_to(spot, m->pos));
    audio_play(SND_STEP, spot, 0.8f);
    hapL = hapR = 0.5f;
}

static void update_pilot(const Input *in, float dt) {
    Mech *m = &mech[pl.mech];
    xf anchor = cockpit_anchor(m), inv = xfinv(anchor);
    if (in->recenterKey) calibrate(in);
    // climbing out: hold A (E in a window) when there is no arm in reach to take
    if (!in->hand[1].a) pl.hatchBlock = 0;
    if (in->hand[1].a && !pl.hatchBlock && pl.graft < 0 && pl.fadeDir < 0) {
        pl.hatch += dt;
        hapR = maxf(hapR, 0.1f + 0.4f * pl.hatch / HATCH_TIME);
        if (pl.hatch >= HATCH_TIME) {
            pl.hatch = 0;
            pl.hatchBlock = pl.pendingExit = 1;
            pl.fadeDir = 1;
            audio_play(SND_CLICK, anchor.p, 1.0f);
        }
    } else pl.hatch = 0;
    float turnIn = in->vr ? in->hand[1].sx : in->hand[0].sx;
    float thrIn = in->hand[0].sy;
    for (int h = 0; h < 2 && in->vr; h++) { // grabbing the physical controls
        v3 hp = xfpt(inv, xfpt(rigNow, in->hand[h].grip.p));
        int sq = in->hand[h].squeeze > 0.5f && in->hand[h].tracked;
        if (sq && !pl.sqPrev[h] && !pl.grab[h]) {
            v3 thr = vadd(shapes[mech_cockpit(m, CKP_THROTTLE)].at, qrot(qaxis(V3(1, 0, 0), -pl.throttle * 0.6f), mech_grip(m, 0)));
            v3 stk = vadd(shapes[mech_cockpit(m, CKP_STICK)].at, mech_grip(m, 1));
            if (vlen(vsub(hp, thr)) < 0.14f) pl.grab[h] = GRAB_THROTTLE;
            else if (vlen(vsub(hp, stk)) < 0.14f) pl.grab[h] = GRAB_STICK;
            else if (vlen(vsub(hp, shapes[mech_cockpit(m, CKP_EJECT)].at)) < 0.15f) pl.grab[h] = GRAB_EJECT;
            if (pl.grab[h]) { pl.grabStart[h] = hp; audio_play(SND_CLICK, xfpt(rigNow, in->hand[h].grip.p), 1.0f); if (h) hapR = 0.6f; else hapL = 0.6f; }
        }
        if (!sq) { if (pl.grab[h] == GRAB_EJECT) pl.ejectPull = 0; pl.grab[h] = GRAB_NONE; }
        pl.sqPrev[h] = sq;
        if (pl.grab[h] == GRAB_THROTTLE) {
            v3 d = vsub(hp, shapes[mech_cockpit(m, CKP_THROTTLE)].at);
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
    v3 to = vsub(pl.aim, xfpt(m->world[P_TORSO], mech_point(m, MP_AIM)));
    m->aimPitch = approach(m->aimPitch, clampf(atan2f(to.y, sqrtf(to.x * to.x + to.z * to.z)), -0.6f, 0.6f), dt * 2.5f);
    m->cool[0] -= dt;
    m->cool[1] -= dt;
    for (int a = 0; a < 2; a++) { // left trigger fires the left arm, right trigger the right, whatever is mounted there
        const ArmType *t = &armType[m->arm[a]];
        if (in->hand[a].trigger <= 0.5f || m->cool[a] > 0) continue;
        if (mech_fire(pl.mech, a, pl.aim, t->spread)) { if (a) hapR = t->dmg > 20 ? 1.0f : 0.35f; else hapL = t->dmg > 20 ? 1.0f : 0.35f; }
        m->cool[a] = t->interval;
    }
    graft_update(in, m);
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
    // The repair torch: hold the left trigger (Space in a window) and point it at a mech of your own side. It
    // closes holes and brings parts back a stage at a time, but only on foot, out in the open.
    pl.torchMech = -1;
    pl.weldT -= dt;
    if (in->hand[0].trigger > 0.5f && (in->hand[0].tracked || !in->vr) && !pl.fly) {
        xf src = in->vr ? hand_xf(rigNow, &in->hand[0]) : xfmul(rigNow, in->hand[1].aim);
        v3 d = qrot(src.q, V3(0, 0, -1)), o = vadd(src.p, vmul(d, in->vr ? 0.08f : 0.3f)), local = {0, 0, 0};
        float reach = TORCH_RANGE;
        int tm = -1, tp = -1;
        for (int i = 0; i < nmech; i++) {
            float t;
            int part;
            v3 l;
            if (mech[i].alive && mech[i].team != 1 && mech[i].drop <= 0 && mech_ray(&mech[i], o, d, reach, &t, &part, &l)) {
                reach = t; tm = i; tp = part; local = l;
            }
        }
        v3 end = vadd(o, vmul(d, reach)), back = vmul(d, -1);
        for (int k = 0; k < 2; k++) spawn(PT_SPARK, o, vmul(vadd(d, rv(0.015f)), reach / 0.1f), 0.1f, 0.016f, 0, 0, 0); // the flame
        hapL = maxf(hapL, 0.15f);
        if (tm >= 0) {
            Mech *m = &mech[tm];
            float hp0 = m->hp[tp];
            int holes = m->ndent[tp], st = m->cook > 0 ? -1 : mech_repair(m, tp, local, TORCH_RATE * dt);
            int whole = m->hp[tp] >= PART_HP(tp) && !m->ndent[tp] && (hp0 < PART_HP(tp) || holes);
            pl.torchMech = tm;
            pl.torchPart = tp;
            spawn(PT_FLASH, vadd(end, vmul(back, 0.1f)), V3(0, 0, 0), 0.05f, 0.2f + 0.3f * frand(), 0, 0, 0);
            if (m->cook <= 0 && (m->hp[tp] > hp0 || holes)) { // working: sparks off metal, a spray off flesh, and the crackle
                if (shapes[mech_shape(m, tp)].bio) ichor(end, back, 1);
                else sparks(end, back, 2, 4, 0.04f);
                hapL = maxf(hapL, 0.35f);
                if (pl.weldT <= 0) { pl.weldT = 0.07f; audio_play(SND_WELD, end, 0.4f); }
            }
            if (st >= 0 || whole) { // a stage back, or the part whole again
                sparks(end, back, 14, 6, 0.05f);
                audio_play(SND_CHIME, end, whole ? 0.9f : 0.5f);
                hapL = 0.8f;
            }
        }
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
    pl.lookYaw = qyaw(qmul(rigNow.q, in->head.q));

    // fades: start of game, boarding, climbing out, death
    pl.fade = clampf(pl.fade + pl.fadeDir * dt * 3.5f, 0, 1);
    pl.hurt = maxf(0, pl.hurt - dt * 1.6f);
    if (pl.fade >= 1 && pl.fadeDir > 0) {
        if (pl.dead) { int look = game_look; deaths++; game_init(demoLevel, 1); game_look = look; return; }
        if (pl.pendingBoard >= 0) {
            pl.mech = pl.pendingBoard;
            pl.pendingBoard = -1;
    pl.graft = -1;
            pl.mode = MODE_PILOT;
            pl.throttle = pl.stickX = 0;
            pl.bob = mech[pl.mech].hipH;
            mech[pl.mech].piloted = 1;
            mech[pl.mech].torsoYaw = 0;
            pl.hatchBlock = 1; // the press that boarded must not go on to climb out
            calibrate(in);
        }
        if (pl.pendingExit) { pl.pendingExit = 0; if (pl.mode == MODE_PILOT) climb_out(in); }
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
        if (m->drop > 0) { mech_fall(m, dt); continue; }
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
    worm_update(dt, tgt, pl.mode != MODE_PILOT);
    waves_update(dt, xfpt(rigNow, in->head.p));

    // shots
    for (int i = 0; i < MAXBOLT; i++) {
        Bolt *b = &bolt[i];
        if (!b->active) continue;
        if (b->kind == 3) b->v.y -= SPORE_FALL * dt;
        float step = vlen(b->v) * dt;
        v3 dir = vnorm(b->v);
        Hit h = trace(b->p, dir, step, b->owner);
        if (b->kind == 3 && h.t < step) spore_burst(vadd(b->p, vmul(dir, h.t)), b->owner);
        v3 end = vadd(b->p, vmul(dir, h.t));
        if (b->owner >= 0 && pl.mode != MODE_PILOT && !pl.fly && !pl.dead) { // enemy fire against you on foot
            v3 c = vadd(rigNow.p, V3(0, 0.9f, 0)), oc = vsub(c, b->p);
            float along = clampf(vdot(oc, dir), 0, h.t);
            float miss = vlen(vsub(oc, vmul(dir, along)));
            int kb = b->owner >= BOT_OWNER ? bot[b->owner - BOT_OWNER].kind : -1; // small enemies have their own damage on foot
            float direct = kb >= 0 ? BOT[kb].foot : b->dmg * 1.2f, reach = kb >= 0 ? BOT[kb].splash : b->kind == 1 ? 3.5f : b->kind == 3 ? 4.5f : 0;
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
        } else if (h.worm >= 0) {
            worm_hit(h.worm, h.local, end, dir, b->dmg);
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
        if (d->emit <= 0 && d->age < 40 && d->shape != S_CK_SHELL && d->shape != S_CKB_SHELL) { // wrecks burn for a while, then just smoke
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

static void worm_scene(Scene *s) {
    const Worm *w = &worm;
    memset(s->bulge, 0, sizeof s->bulge);
    if (!w->alive) return;
    for (int k = 0; k < WPARTS; k++) {
        if (!wormVis[k]) continue;
        DrawPart *d = push(s, worm_shape(k), wormX[k], 3);
        d->scale = wormS[k];
        d->ndent = w->ndent[k];
        memcpy(d->dent, w->dent[k], sizeof w->dent[k]);
        memcpy(d->heat, w->heat[k], sizeof w->heat[k]);
        d->seed = (float)k;
        d->flash = w->flash[k];
        d->texel = 0.25f;
        d->stage = w->hp < 300 ? 2 : 0;
        if (!(k & 1)) shadow(s, wormX[k].p, 3.0f * wormS[k]);
    }
    // the mound over its head while it is under the sand
    float ground = world_height(w->p.x, w->p.z), under = ground - w->p.y;
    float bulge[4] = {w->p.x, w->p.z, 1.9f * clampf(under / 3, 0, 1) * clampf((14 - under) / 4, 0, 1), 8};
    memcpy(s->bulge, bulge, sizeof bulge);
}

void game_scene(Scene *s, const Input *in) {
    xf rig = rigNow;
    s->npart = s->nptc = s->nshadow = 0;
    s->time = gtime;
    s->look = game_look;
    s->head = game_outside ? game_cam.p : xfpt(rig, in->head.p);
    {
        float sp = pl.mode == MODE_PILOT ? fabsf(mech[pl.mech].speed) / TOP_SPEED : 0;
        float hum = pl.mode != MODE_PILOT ? 0 : game_outside ? clampf(1 - vlen(vsub(game_cam.p, mech[pl.mech].pos)) / 45, 0, 1) * 0.5f : 1;
        v3 right = qrot(game_outside ? game_cam.q : qmul(rig.q, in->head.q), V3(1, 0, 0));
        audio_body(mech[pl.mech].chassis == CH_BIO); // a living mech has a heartbeat where a machine has a reactor hum
        audio_listener(s->head, right, hum, sp, pl.mode == MODE_PILOT && !game_outside ? pl.alarm : 0);
    }
    s->vignette = in->vr && pl.mode == MODE_EJECT ? 0.7f : 0; // edge darkening only during the eject flight
    float dark = maxf(pl.fade, pl.hatch / HATCH_TIME * 0.45f), red = pl.hurt * 0.4f; // holding A to climb out dims the view
    s->fade = game_outside ? pl.fade : maxf(dark, red); // the red flash when you are hit is only for your own eyes
    s->fadeCol = dark >= red ? V3(0.02f, 0.02f, 0.03f) : V3(0.9f, 0.1f, 0.05f);
    float waveNo = waves ? (float)(wv.fighting ? wv.n : wv.n + 1) : 0, waveCount = wv.fighting ? (float)wv.left : -ceilf(wv.timer);
    int piloting = pl.mode == MODE_PILOT, cabin = piloting && !game_outside; // cabin: drawn from inside the cockpit

    s->nimp = 0;
    for (int i = 0; i < nmech; i++) {
        Mech *m = &mech[i];
        if (!m->alive) continue;
        // Far away: a few flat cards instead of ten traced parts. The switch distance differs a
        // little going out and coming back, so a mech hovering at the limit does not flicker between the two.
        float dist = vlen(vsub(m->pos, s->head));
        m->far = game_cards && s->nimp + 4 <= MAX_DRAW_IMP && dist > (m->far ? IMP_DIST - 8 : IMP_DIST + 8);
        if (m->far) {
            static const int CARD[4] = {P_PELVIS, P_TORSO, P_ARM_L, P_ARM_R}; // legs, torso, and each arm it still has
            for (int c = 0; c < 4; c++) {
                if (m->gone[CARD[c]]) continue;
                DrawImp *d = &s->imp[s->nimp++];
                d->pos = m->world[CARD[c]].p;
                d->yaw = m->yaw + (c ? m->torsoYaw : 0);
                d->layer = mech_card_layer(m, c);
                d->row = c ? 0 : mech_imp_row(m);
                d->flip = mech_flip(m, CARD[c]);
                d->tintA = TINT[m->team][0];
                d->tintB = TINT[m->team][1];
                d->flash = m->flash[CARD[c]];
                for (int p = P_THIGH_L; p < P_COUNT && !c; p++) d->flash = maxf(d->flash, m->flash[p]);
            }
            if (dist < 260) {
                shadow(s, xfpt(m->world[P_TORSO], V3(0, 1.3f, 0)), 2.6f);
                shadow(s, vadd(m->pos, V3(0, 1.5f, 0)), 2.0f);
            }
        }
        for (int p = 0; p < P_COUNT && !m->far; p++) {
            if (m->gone[p] || (cabin && i == pl.mech && p == P_TORSO)) continue; // you sit inside your own hull
            DrawPart *d = push(s, mech_shape(m, p), m->world[p], m->team);
            d->flip = mech_flip(m, p);
            if (p == P_TORSO && d->shape->bio && dist < 90) d->scale = 1 + 0.012f * sinf(gtime * 1.7f + m->seed); // it breathes
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

    if (cabin) {
        Mech *m = &mech[pl.mech];
        xf a = cockpit_anchor(m);
        const int ckThr = mech_cockpit(m, CKP_THROTTLE), ckStk = mech_cockpit(m, CKP_STICK), ckEj = mech_cockpit(m, CKP_EJECT);
        push_cabin(s, mech_cockpit(m, CKP_SHELL), a, m->team);
        DrawPart *dash = push_cabin(s, mech_cockpit(m, CKP_DASH), cabin_at(a, mech_cockpit(m, CKP_DASH)), m->team);
        memcpy(dash->scr, shapes[mech_cockpit(m, CKP_DASH)].scr, sizeof dash->scr);
        for (int p = 0; p < P_COUNT; p++) dash->stat[p] = m->gone[p] ? 3 : (float)m->stage[p];
        dash->aux[0] = m->speed / TOP_SPEED;
        dash->aux[2] = pl.alarm;
        dash->aux[3] = clampf(m->hp[P_TORSO] / 160.0f, 0, 1);
        dash->stat[10] = waveNo;    // the wave game: which wave, and how many are left in it
        dash->stat[11] = waveCount; // (or, between waves, minus the seconds to the next)
        for (int k = CKP_SEAT; k <= CKP_SIDE_R; k++) push_cabin(s, mech_cockpit(m, k), cabin_at(a, mech_cockpit(m, k)), m->team)->seed = (float)k;
        for (int k = CKP_TRIM; k <= CKP_TRIM3; k++)
            if (mech_cockpit(m, k) >= 0) push_cabin(s, mech_cockpit(m, k), cabin_at(a, mech_cockpit(m, k)), m->team);
        xf thr = xfmul(a, XF(shapes[ckThr].at, qaxis(V3(1, 0, 0), -pl.throttle * 0.6f)));
        xf stk = xfmul(a, XF(shapes[ckStk].at, qmul(qaxis(V3(0, 0, 1), -pl.stickX * 0.35f), qaxis(V3(1, 0, 0), -pl.stickY * 0.35f))));
        xf ej = xfmul(a, XF(vadd(shapes[ckEj].at, V3(0, -pl.ejectPull, 0)), qid()));
        push_cabin(s, ckThr, thr, m->team);
        push_cabin(s, ckStk, stk, m->team);
        push_cabin(s, ckEj, ej, m->team);
        for (int h = 0; h < 2 && in->vr; h++) { // gloves; a hand holding a control is drawn on that control
            if (!in->hand[h].tracked) continue;
            xf g = hand_xf(rig, &in->hand[h]);
            if (pl.grab[h] == GRAB_THROTTLE) g.p = xfpt(thr, mech_grip(m, 0));
            if (pl.grab[h] == GRAB_STICK) g.p = xfpt(stk, mech_grip(m, 1));
            if (pl.grab[h] == GRAB_EJECT) g.p = ej.p;
            push_cabin(s, pl.grab[h] || in->hand[h].squeeze > 0.5f ? S_FIST : S_HAND, g, 0)->texel = 0.006f;
        }
        if (s->nptc < MAX_DRAW_PTC) { // aiming reticle, drawn at the distance of whatever it rests on
            v3 toHead = vnorm(vsub(s->head, pl.aim));
            v3 rp = vadd(pl.aim, vmul(toHead, minf(1.0f, pl.aimDist * 0.05f)));
            s->ptc[s->nptc++] = (DrawParticle){rp.x, rp.y, rp.z, maxf(0.05f, pl.aimDist * 0.014f), 0, PT_RETICLE, 0, 0};
        }
    } else if (!game_outside) {
        if (pl.mode == MODE_FOOT && !pl.fly && (in->hand[0].tracked || !in->vr)) { // the screen on your left wrist; in a window, low on the left
            xf w = in->vr ? xfmul(hand_xf(rig, &in->hand[0]), XF(V3(0, 0.05f, 0.11f), qmul(qaxis(UP, PI / 2), qaxis(V3(1, 0, 0), -PI / 2))))
                          : xfmul(xfmul(rig, in->head), XF(V3(-0.15f, -0.09f, -0.3f), qmul(qaxis(UP, 0.3f), qaxis(V3(1, 0, 0), -0.25f))));
            DrawPart *d = push(s, S_WRIST, w, 0);
            d->texel = 0.006f;
            memcpy(d->scr, shapes[S_WRIST].scr, sizeof d->scr);
            d->aux[0] = pl.torchMech >= 0 ? mech[pl.torchMech].hp[pl.torchPart] / PART_HP(pl.torchPart) : -1; // the part under the torch
            d->aux[1] = 1; // the wrist's layout
            d->aux[3] = clampf(pl.hp / 100, 0, 1);
            d->stat[10] = waveNo;
            d->stat[11] = waveCount;
        }
        if (in->hand[1].tracked && pl.mode == MODE_FOOT) push(s, S_PISTOL, pistol_xf(rig, &in->hand[1]), 0)->texel = 0.006f;
        for (int h = 0; h < 2; h++) { // the pistol hand is always closed
            int fist = (h && pl.mode == MODE_FOOT) || in->hand[h].squeeze > 0.5f;
            if (in->hand[h].tracked) push(s, fist ? S_FIST : S_HAND, hand_xf(rig, &in->hand[h]), 0)->texel = 0.006f;
        }
    }

    bots_scene(s);
    worm_scene(s);
    if (waves && wv.fighting && wv.left <= 3 && wv.quiet > 15) { // the last few have kept away: amber light over them
        v3 at[8];
        int n = 0;
        for (int i = 0; i < nmech && n < 8; i++)
            if (mech[i].alive && mech[i].wave) at[n++] = vadd(mech[i].pos, V3(0, 14, 0));
        for (int i = 0; i < nbot && n < 8; i++)
            if (bot[i].alive && bot[i].wave) at[n++] = vadd(bot_xf(&bot[i]).p, V3(0, 3, 0));
        if (wv.worm && worm.alive && n < 8) game_focus(FOCUS_WORM, 0, &at[n++]);
        for (int i = 0; i < n; i++) { // larger with distance, so it shows from far away
            float sc = maxf(1, vlen(vsub(at[i], s->head)) / 40);
            push(s, S_BEACON_FOE, XF(vadd(at[i], V3(0, 5.2f * sc, 0)), qid()), 0)->scale = sc;
        }
    }
    for (int i = 0; i < MAXDEB; i++) {
        Debris *b = &deb[i];
        if (!b->active) continue;
        DrawPart *d = push(s, b->shape, b->x, b->team);
        d->scale = b->scale;
        d->flip = b->flip;
        if (piloting && b->arm && b->rest && vlen(vsub(b->x.p, s->head)) < 150) { // a marker over arms you could take
            float reach = i == pl.graft ? 0.5f + 0.1f * sinf(gtime * 8) : 0.25f;
            push(s, S_BEACON, XF(vadd(b->x.p, V3(0, 2 + 5 * reach, 0)), qid()), 0)->scale = reach;
        }
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
        if (bolt[i].active) push(s, bolt[i].kind == 2 ? S_SPIT : bolt[i].kind == 3 ? S_GLOB : bolt[i].kind == 4 ? S_NEEDLE : bolt[i].kind ? S_BOLT : S_TRACER, XF(bolt[i].p, qlook(vnorm(bolt[i].v))), 0);

    for (int i = 0; i < nptc && s->nptc < MAX_DRAW_PTC; i++) {
        const Particle *q = &ptc[i];
        s->ptc[s->nptc++] = (DrawParticle){q->p.x, q->p.y, q->p.z, q->size, q->age / q->life, (float)q->type, q->seed, q->spin};
    }
}

void game_report(void) { // one line for test runs: how the fight is going
    int kind[3] = {0, 0, 0}, mechs = 0;
    for (int i = 0; i < nbot; i++) kind[bot[i].kind] += bot[i].alive;
    for (int i = 0; i < nmech; i++) mechs += mech[i].alive && mech[i].team == 1;
    static const char *const ARM[ARM_COUNT] = {"cannon", "plasma", "needler", "spore"};
    const Mech *me = &mech[pl.mech];
    printf("report: arms %s / %s; ", me->gone[P_ARM_L] ? "none" : ARM[me->arm[0]], me->gone[P_ARM_R] ? "none" : ARM[me->arm[1]]);
    printf("report: %s, your mech torso %.0f of 160, you %.0f of 100, restarts %d; left: %d enemy mechs, %d drones, %d crawlers, %d turrets, worm %.0f of 900 (head at %.0f %.0f %.0f, heading %.2f)\n",
           pl.mode == MODE_PILOT ? "piloting" : "on foot", mech[0].alive ? mech[0].hp[P_TORSO] : 0, pl.hp, deaths, mechs, kind[0], kind[1], kind[2], worm.alive ? worm.hp : 0, worm.p.x, worm.p.y, worm.p.z, worm.yaw);
    int holes = 0;
    float hp = 0;
    for (int p = 0; p < P_COUNT; p++) { holes += me->ndent[p]; hp += me->gone[p] ? 0 : maxf(me->hp[p], 0); }
    printf("report: mech %d%s: stages", pl.mech, me->alive ? "" : " (destroyed)");
    for (int p = 0; p < P_COUNT; p++) printf(" %c", me->gone[p] ? '-' : '0' + me->stage[p]);
    printf(" (pelvis torso arms legs), %.0f of 1060 hit points, %d holes\n", hp, holes);
    if (waves) printf("report: wave %d, %s, %d kills, best wave %d\n", wv.n, wv.fighting ? "fighting" : "between waves", wv.kills, wv.best);
}

int game_focus(int what, float range, v3 *out) {
    if (what == FOCUS_WORM) {
        if (!worm.alive) return 0;
        *out = worm.p;
        out->y = maxf(out->y, world_height(out->x, out->z) + 1); // under the sand: the mound over its head
        return 1;
    }
    v3 me = pl.mode == MODE_PILOT ? xfpt(mech[pl.mech].world[P_TORSO], V3(0, 1.3f, 0)) : vadd(rigNow.p, V3(0, 1.2f, 0));
    if (what == FOCUS_ME) { *out = me; return 1; }
    float best = range;
    int found = 0;
    for (int i = 0; i < nmech; i++) {
        const Mech *m = &mech[i];
        v3 c = xfpt(m->world[P_TORSO], V3(0, 1.0f, 0));
        float d = vlen(vsub(c, me));
        if (m->alive && m->team == 1 && m->cook <= 0 && d < best) { best = d; *out = c; found = 1; }
    }
    for (int i = 0; i < nbot; i++) {
        if (!bot[i].alive) continue;
        v3 c = bot_xf(&bot[i]).p;
        float d = vlen(vsub(c, me));
        if (d < best) { best = d; *out = c; found = 1; }
    }
    if (worm.alive && wormVis[0] && vlen(vsub(wormX[0].p, me)) < best) { *out = wormX[0].p; found = 1; } // the worm's head, while it is out of the sand
    return found;
}
