#include "audio.h"
#include <alsa/asoundlib.h>
#include <pthread.h>

#define RATE 48000
#define NVOICE 32
#define CHUNK 256
typedef struct { int type, on; float t, gl, gr, ph, lp; } Voice;
static Voice voice[NVOICE];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t thread;
static snd_pcm_t *pcm;
static volatile int running;
static v3 earPos, earRight = {1, 0, 0};
static float engTarget, engPitch, alarmOn; // set by the game
static float eng, engPh, engLp, alarmT, windLp;
static unsigned rng = 22222;

static float noise(void) {
    rng = rng * 1664525u + 1013904223u;
    return (float)(rng >> 8) / 8388608.0f - 1.0f;
}

// One sample of a one-shot sound. Returns 0 and switches the voice off when it has finished.
static float voice_sample(Voice *v) {
    const float dt = 1.0f / RATE, TAU = 6.2831853f;
    float t = v->t, s = 0, dur = 1;
    v->t += dt;
    switch (v->type) {
    case SND_CANNON: dur = 0.22f;
        v->ph += TAU * (55 + 90 * expf(-t * 40)) * dt;
        s = noise() * expf(-t * 34) * 0.8f + sinf(v->ph) * expf(-t * 16) * 0.9f; break;
    case SND_PLASMA: dur = 0.6f;
        v->ph += TAU * (110 + 950 * expf(-t * 7)) * dt;
        s = sinf(v->ph + 2.2f * sinf(v->ph * 0.5f)) * expf(-t * 5) * 0.8f + noise() * expf(-t * 30) * 0.3f; break;
    case SND_PISTOL: dur = 0.16f;
        v->ph += TAU * (260 + 1500 * expf(-t * 28)) * dt;
        s = (sinf(v->ph) > 0 ? 0.5f : -0.5f) * expf(-t * 24) + noise() * expf(-t * 60) * 0.3f; break;
    case SND_HIT: dur = 0.3f;
        s = noise() * expf(-t * 45) * 0.7f + (sinf(TAU * 523 * t) + sinf(TAU * 811 * t) + sinf(TAU * 1277 * t)) * expf(-t * 13) * 0.22f; break;
    case SND_BOOM: dur = 1.8f;
        v->lp += (noise() - v->lp) * 0.06f;
        v->ph += TAU * (48 - 12 * t) * dt;
        s = v->lp * expf(-t * 2.4f) * 2.6f + sinf(v->ph) * expf(-t * 3.2f) * 0.9f; break;
    case SND_STEP: dur = 0.35f;
        v->ph += TAU * (30 + 60 * expf(-t * 11)) * dt;
        v->lp += (noise() - v->lp) * 0.1f;
        s = sinf(v->ph) * expf(-t * 11) + v->lp * expf(-t * 22) * 0.8f; break;
    case SND_WHOOSH: dur = 1.4f;
        v->lp += (noise() - v->lp) * (0.02f + 0.25f * t / dur);
        s = v->lp * sinf(3.14159f * t / dur) * 1.6f; break;
    default: dur = 0.03f;
        s = noise() * expf(-t * 160) * 0.5f; break;
    }
    if (t > dur) v->on = 0;
    return s;
}

static void *mix_thread(void *arg) {
    short buf[CHUNK * 2];
    const float dt = 1.0f / RATE, TAU = 6.2831853f;
    while (running) {
        pthread_mutex_lock(&lock);
        for (int i = 0; i < CHUNK; i++) {
            float l = 0, r = 0;
            for (int k = 0; k < NVOICE; k++)
                if (voice[k].on) { float s = voice_sample(&voice[k]); l += s * voice[k].gl; r += s * voice[k].gr; }
            // cockpit loops: reactor hum that rises with speed, warning beeper; plus faint desert wind everywhere
            eng += (engTarget - eng) * 0.0002f;
            engPh += (34 + 46 * engPitch) * dt;
            if (engPh > 1) engPh -= 1;
            float saw = (engPh * 2 - 1) * 0.6f + sinf(TAU * engPh * 0.5f) * 0.8f + noise() * 0.12f;
            engLp += (saw - engLp) * (0.02f + 0.03f * engPitch);
            float mono = engLp * eng * (0.26f + 0.12f * engPitch);
            alarmT += dt;
            if (alarmOn > 0.5f && fmodf(alarmT, 0.5f) < 0.22f) mono += (sinf(TAU * (fmodf(alarmT, 1.0f) < 0.5f ? 880 : 660) * alarmT) > 0 ? 0.05f : -0.05f);
            windLp += (noise() - windLp) * 0.012f;
            mono += windLp * 0.10f;
            l += mono; r += mono;
            l = l / (1 + fabsf(l)); r = r / (1 + fabsf(r)); // soft limit
            buf[i * 2] = (short)(l * 30000);
            buf[i * 2 + 1] = (short)(r * 30000);
        }
        pthread_mutex_unlock(&lock);
        snd_pcm_sframes_t n = snd_pcm_writei(pcm, buf, CHUNK);
        if (n < 0) snd_pcm_recover(pcm, (int)n, 1);
    }
    return arg;
}

void audio_init(void) {
    if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) { pcm = NULL; return; }
    if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, RATE, 1, 40000) < 0) {
        snd_pcm_close(pcm); pcm = NULL; return;
    }
    running = 1;
    pthread_create(&thread, NULL, mix_thread, NULL);
}
void audio_shutdown(void) {
    if (!pcm) return;
    running = 0;
    pthread_join(thread, NULL);
    snd_pcm_close(pcm);
    pcm = NULL;
}

void audio_play(int type, v3 pos, float gain) {
    if (!pcm) return;
    v3 d = vsub(pos, earPos);
    float dist = vlen(d), pan = dist > 0.3f ? vdot(d, earRight) / dist : 0;
    float g = gain * 14.0f / (14.0f + dist); // big machines: loud nearby, still audible far away
    if (g < 0.004f) return;
    pthread_mutex_lock(&lock);
    int slot = 0;
    for (int k = 0; k < NVOICE; k++) { if (!voice[k].on) { slot = k; break; } if (voice[k].t > voice[slot].t) slot = k; }
    voice[slot] = (Voice){type, 1, 0, g * (0.6f - 0.4f * pan), g * (0.6f + 0.4f * pan), 0, 0};
    pthread_mutex_unlock(&lock);
}
void audio_listener(v3 pos, v3 right, float engine, float pitch, float alarm) {
    earPos = pos; earRight = right; engTarget = engine; engPitch = pitch; alarmOn = alarm;
}
