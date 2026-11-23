// All sound is synthesized while the game runs; there are no audio files.
#ifndef AUDIO_H
#define AUDIO_H
#include "mathx.h"
enum { SND_CANNON, SND_PLASMA, SND_PISTOL, SND_HIT, SND_BOOM, SND_STEP, SND_WHOOSH, SND_CLICK, SND_ROAR };
void audio_init(void);
void audio_shutdown(void);
void audio_play(int type, v3 pos, float gain);                                  // a sound at a place in the world
void audio_listener(v3 pos, v3 right, float engine, float pitch, float alarm);  // where your ears are, plus cockpit loops
#endif
