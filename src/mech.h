// Mech shapes, skeleton, walk cycle, hit testing and damage state.
#ifndef MECH_H
#define MECH_H
#include "render.h"

enum { P_PELVIS, P_TORSO, P_ARM_L, P_ARM_R, P_THIGH_L, P_SHIN_L, P_FOOT_L, P_THIGH_R, P_SHIN_R, P_FOOT_R, P_COUNT };
// Every shape the game draws. The pieces of each one are listed under the same name in assets/shapes.txt.
// Edit that file (or the Blender scene that writes it) to change shapes, joints and points; no code change is needed.
#define SHAPE_LIST(X) \
    X(PELVIS) X(TORSO) X(ARM_L) X(ARM_R) X(THIGH) X(SHIN) X(FOOT) X(ROCK0) X(ROCK1) X(ROCK2) X(BOLT) X(HAND) X(FIST) X(PISTOL) \
    X(CK_SHELL) X(CK_DASH) X(CK_SEAT) X(CK_SIDE_L) X(CK_SIDE_R) X(CK_THROTTLE) X(CK_STICK) X(CK_EJECT) X(BEACON) X(TRACER) \
    X(DRONE) X(CRAWLER) X(CRAWLER_LEG) X(TURRET_BASE) X(TURRET_HEAD) X(SPIT) \
    X(WORM_HEAD) X(WORM_SEG) X(WORM_TAIL) \
    X(BIO_PELVIS) X(BIO_TORSO) X(BIO_THIGH) X(BIO_SHIN) X(BIO_FOOT) X(ARM_NEEDLER) X(ARM_SPORE) X(NEEDLE) X(GLOB) \
    X(CKB_SHELL) X(CKB_DASH) X(CKB_SEAT) X(CKB_SIDE_L) X(CKB_SIDE_R) X(CKB_THROTTLE) X(CKB_STICK) X(CKB_EJECT) X(CKB_TEETH) X(CKB_RIBS) X(CK_STRUTS) X(CK_DIALS) X(CKB_DIALS)
#define X(n) S_##n,
enum { SHAPE_LIST(X) S_COUNT };
#undef X
extern Shape shapes[S_COUNT];

// A mech is a body (its chassis: hips, torso and legs) plus whatever arm is mounted on each shoulder.
// Any arm fits either side of either body; an arm made for the other side is drawn mirrored.
enum { CH_MACHINE, CH_BIO, CH_COUNT };
enum { ARM_CANNON, ARM_PLASMA, ARM_NEEDLER, ARM_SPORE, ARM_COUNT };
typedef struct {
    int shape, muzzle, side;      // its shape, the point shots leave from, and the side it was made for (0 left, 1 right)
    int bolt, shots, snd;         // what it fires (see Bolt in game.c), how many at once, and its sound
    float dmg, speed, interval, spread, range;
    int aiBurst;                  // enemy mechs fire this many times, then pause
    float aiPause;
} ArmType;
extern const ArmType armType[ARM_COUNT];

// Named points on shapes, also from assets/shapes.txt ("point NAME x y z" under the shape).
#define POINT_LIST(X) \
    X(TORSO, EYE) X(TORSO, AIM) X(TORSO, SHOULDER) X(ARM_L, MUZZLE) X(ARM_R, MUZZLE) X(FOOT, SOLE) \
    X(BIO_TORSO, EYE) X(BIO_TORSO, AIM) X(BIO_TORSO, SHOULDER) X(BIO_FOOT, SOLE) X(ARM_NEEDLER, MUZZLE) X(ARM_SPORE, MUZZLE) \
    X(CK_THROTTLE, GRIP) X(CK_STICK, GRIP) X(CKB_THROTTLE, GRIP) X(CKB_STICK, GRIP) \
    X(DRONE, MUZZLE) X(CRAWLER, MUZZLE) X(CRAWLER, SOLE) X(TURRET_HEAD, MUZZLE) X(WORM_HEAD, MAW)
#define X(s, n) PNT_##s##_##n,
enum { POINT_LIST(X) PNT_COUNT };
#undef X
extern v3 pnt[PNT_COUNT];
// Joints are not listed here: a shape's "at" is where it attaches to the shape it is "on".
// TORSO on PELVIS, THIGH on PELVIS (left hip; the right one is mirrored), SHIN on THIGH, FOOT on SHIN, and the
// same for the BIO_ body. Arms mount at the torso's SHOULDER point (the left one; the right is mirrored).
// The cockpit pieces are on CK_SHELL, whose origin is the pilot's eye (the torso's EYE point). The living body
// has its own cockpit, the CKB_ shapes, built the same way.
// CRAWLER_LEG on CRAWLER (front left hip; the other three are mirrored), TURRET_HEAD on TURRET_BASE (the pivot).

typedef struct {
    v3 pos;                // ground point under the hips
    float yaw, torsoYaw, aimPitch;
    float phase, speed;    // walk cycle phase, forward speed (m/s)
    float hipH;
    xf world[P_COUNT];
    float hp[P_COUNT];
    int stage[P_COUNT];
    unsigned char gone[P_COUNT];
    float dent[P_COUNT][8][4];
    float heat[P_COUNT][8];
    int ndent[P_COUNT];
    float flash[P_COUNT], emit[P_COUNT];
    int team, alive, piloted;
    float seed, cool[2];
    float aiTurn, aiFire, throttle, cook; // cook > 0: reactor countdown to explosion
    int aiBurst, dummy, aggro;
    int far; // drawn as a flat card this frame (see mech_impostors_bake)
    int chassis, arm[2], burst[2]; // which body, which arm on the left and right shoulder, shots into the current burst
} Mech;

void mech_shapes_init(void);
void mech_shapes_live(const char *path); // call before mech_shapes_init: read shapes from this file and follow its changes
int mech_shapes_poll(void); // returns 1 when the shapes were read again
// Distant mechs are drawn as two flat cards (legs, torso with arms) instead of ten traced parts.
// bake draws the sheets of views the cards show; call it once the renderer is up and after shapes change.
#define IMP_DIST 110.0f  // metres at which a mech switches between traced parts and cards
#define IMP_PHASES 16    // walk-cycle poses on the legs sheet; one more row holds the standing pose
void mech_impostors_bake(void);
int mech_imp_row(const Mech *m); // which legs pose matches the mech right now
void mech_init(Mech *m, v3 pos, float yaw, int team);       // a machine body with a cannon and a plasma arm
void mech_outfit(Mech *m, int chassis, int armL, int armR);  // change what it is built from
int mech_shape(const Mech *m, int part);                     // the shape drawn for one of its parts
int mech_flip(const Mech *m, int part);                      // 1 if that part is drawn mirrored
v3 mech_point(const Mech *m, int which);                     // a named point on its torso or feet
enum { MP_EYE, MP_AIM, MP_SHOULDER, MP_SOLE };
v3 mech_muzzle(const Mech *m, int arm);                      // where arm 0 / 1 fires from, in that arm's space
int mech_card_layer(const Mech *m, int which);               // which card sheet: 0 legs, 1 torso, 2 left arm, 3 right arm
// The cockpit you sit in depends on the body. The TRIM pieces hold the thin and small things (struts, ribs, teeth,
// the lights on the dashboard; -1 if none). They are separate shapes on purpose: a thin piece inside the shell slows the tracing
// of every pixel that looks out past it, while in a shape of its own it only costs the pixels it covers.
enum { CKP_SHELL, CKP_DASH, CKP_SEAT, CKP_SIDE_L, CKP_SIDE_R, CKP_THROTTLE, CKP_STICK, CKP_EJECT, CKP_TRIM, CKP_TRIM2, CKP_TRIM3, CKP_COUNT };
int mech_cockpit(const Mech *m, int piece);                  // the shape of one cockpit piece
v3 mech_grip(const Mech *m, int stick);                      // where the hand holds the throttle (0) or stick (1), in that control's space
void mech_pose(Mech *m, float dt);
// Ray against one shape placed at x. Returns 1 and the distance / hit point (in shape space) on a hit.
int shape_ray(const Shape *s, xf x, v3 ro, v3 rd, float tmax, const float (*dent)[4], int ndent, float *t, v3 *local);
int mech_ray(const Mech *m, v3 ro, v3 rd, float tmax, float *t, int *part, v3 *local);
// Applies damage. Returns the part's new stage if it changed, otherwise -1.
int mech_damage(Mech *m, int part, v3 local, float dmg);
#endif
