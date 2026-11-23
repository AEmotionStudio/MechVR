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
    X(DRONE) X(CRAWLER) X(CRAWLER_LEG) X(TURRET_BASE) X(TURRET_HEAD) X(SPIT)
#define X(n) S_##n,
enum { SHAPE_LIST(X) S_COUNT };
#undef X
extern Shape shapes[S_COUNT];
extern const int partShape[P_COUNT];

// Named points on shapes, also from assets/shapes.txt ("point NAME x y z" under the shape).
#define POINT_LIST(X) \
    X(TORSO, EYE) X(TORSO, AIM) X(ARM_L, MUZZLE) X(ARM_R, MUZZLE) X(FOOT, SOLE) X(CK_THROTTLE, GRIP) X(CK_STICK, GRIP) \
    X(DRONE, MUZZLE) X(CRAWLER, MUZZLE) X(CRAWLER, SOLE) X(TURRET_HEAD, MUZZLE)
#define X(s, n) PNT_##s##_##n,
enum { POINT_LIST(X) PNT_COUNT };
#undef X
extern v3 pnt[PNT_COUNT];
// Joints are not listed here: a shape's "at" is where it attaches to the shape it is "on".
// TORSO on PELVIS, ARM_L and ARM_R on TORSO, THIGH on PELVIS (left hip; the right one is mirrored), SHIN on THIGH,
// FOOT on SHIN. The cockpit pieces are on CK_SHELL, whose origin is the pilot's eye (TORSO's EYE point).
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
void mech_init(Mech *m, v3 pos, float yaw, int team);
void mech_pose(Mech *m, float dt);
// Ray against one shape placed at x. Returns 1 and the distance / hit point (in shape space) on a hit.
int shape_ray(const Shape *s, xf x, v3 ro, v3 rd, float tmax, const float (*dent)[4], int ndent, float *t, v3 *local);
int mech_ray(const Mech *m, v3 ro, v3 rd, float tmax, float *t, int *part, v3 *local);
// Applies damage. Returns the part's new stage if it changed, otherwise -1.
int mech_damage(Mech *m, int part, v3 local, float dmg);
#endif
