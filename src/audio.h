// All sound is synthesized while the game runs; there are no audio files.
#ifndef AUDIO_H
#define AUDIO_H
#include "mathx.h"
enum { SND_CANNON, SND_PLASMA, SND_PISTOL, SND_HIT, SND_BOOM, SND_STEP, SND_WHOOSH, SND_CLICK, SND_ROAR,
       SND_HORN, SND_CHIME, SND_BEEP, SND_WELD }; // horn: a wave is coming; chime: a wave beaten or a part made whole; weld: the repair torch
void audio_init(void);
void audio_shutdown(void);
void audio_play(int type, v3 pos, float gain);                                  // a sound at a place in the world
void audio_offline(void);                   // record instead of play: nothing is heard, audio_render hands out the samples
void audio_render(short *out, int frames);  // mix this many stereo frames (48 kHz, 16-bit) of what is playing now
void audio_body(int living);                                                    // what you are riding: 0 a machine (reactor hum), 1 a living mech (heartbeat and breath)
void audio_listener(v3 pos, v3 right, float engine, float pitch, float alarm);  // where your ears are, plus cockpit loops
#endif
