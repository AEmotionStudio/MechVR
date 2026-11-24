// Game state: player, mechs, shots, debris, particles.
#ifndef GAME_H
#define GAME_H
#include "render.h"
#include "xr.h"

typedef struct {
    int vr;
    int headValid;  // headset is being worn and tracked
    float dt;
    xf head;        // in play space
    XrHand hand[2]; // in play space
    float flatYaw;  // flat mode: turn this frame (radians)
    int lookKey;    // 1..3 picks a look preset, 0 leaves it
    int toggleFly;  // free camera on/off
    float flyUp;
    int boardKey, ejectKey, recenterKey, resetKey;
} Input;

void game_init(int demo, int startOnFoot);
int game_piloting(void);
float game_throttle(void);           // where your throttle lever sits, -0.4 .. 1
void game_update(const Input *in);
void game_scene(Scene *s, const Input *in);
xf game_rig(void);                    // play space -> world
void game_set_cam(v3 pos, float yaw); // put the free camera somewhere (screenshots)
void game_haptics(float *left, float *right);
extern int game_look;
// Recording: draw the world for a camera that is not the player's eyes (no cockpit, no hands, your mech drawn whole).
extern int game_outside;
extern xf game_cam;
enum { FOCUS_WORM, FOCUS_ME, FOCUS_ENEMY };
int game_focus(int what, float range, v3 *out); // where something worth looking at is; 0 if there is none (ENEMY: within range)
extern int game_calm;       // for measuring: 1 = enemies never attack, 2 = only the small ones do
extern int game_cards;      // 1 = draw distant mechs as flat cards (default)
void game_swarm(int n);     // call before game_init: add n extra drones and n extra crawlers for testing
void game_report(void);     // print a one-line summary of the fight (for test runs)
void game_worm(float dist); // call before game_init: how far ahead the sand worm lives (default 380 m); 0 = no worm
void game_loadout(int body, int armL, int armR); // call before game_init: your mech's body (0 machine, 1 living) and arms (0 cannon, 1 plasma, 2 needler, 3 spore)
void game_horde(int n);     // call before game_init: add n extra enemy mechs for testing
#endif
