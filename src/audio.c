#ifdef _WIN32 // Windows plays through waveOut (winmm), Linux through ALSA; the synthesis is the same
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
static SRWLOCK lock = SRWLOCK_INIT;
static HANDLE thread, bufferDone;
static HWAVEOUT pcm;
#define LOCK() AcquireSRWLockExclusive(&lock)
#define UNLOCK() ReleaseSRWLockExclusive(&lock)
#else
#include <alsa/asoundlib.h>
#include <pthread.h>
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t thread;
static snd_pcm_t *pcm;
#define LOCK() pthread_mutex_lock(&lock)
#define UNLOCK() pthread_mutex_unlock(&lock)
#endif
#include "audio.h"

#define RATE 48000
#define NVOICE 32
#define CHUNK 256
typedef struct { int type, on; float t, gl, gr, ph, lp; } Voice;
static Voice voice[NVOICE];
static volatile int running;
static int offline; // recording a video: no sound device, the recorder asks for samples as the game runs
static v3 earPos, earRight = {1, 0, 0};
static float engTarget, engPitch, alarmOn; // set by the game
static float eng, engPh, engLp, alarmT, windLp, beatT;
static int living; // set by the game: the cockpit loop is a heartbeat instead of a reactor hum
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
    case SND_ROAR: dur = 2.8f; // the worm: a low growl that swells and fades
        v->ph += TAU * (34 + 26 * sinf(3.14159f * t / dur)) * dt;
        v->lp += (noise() - v->lp) * 0.03f;
        s = ((sinf(v->ph) > 0 ? 0.3f : -0.3f) + sinf(v->ph * 0.5f) * 0.9f + v->lp * 2.2f) * sinf(3.14159f * t / dur); break;
    case SND_HORN: { dur = 2.4f; // a wave is coming: two long blasts on a low horn
        float env = clampf(t / 0.08f, 0, 1) * clampf((0.95f - t) / 0.25f, 0, 1) + clampf((t - 1.2f) / 0.08f, 0, 1) * clampf((2.35f - t) / 0.3f, 0, 1);
        v->ph += TAU * 98 * dt;
        float tone = sinf(v->ph) + 0.5f * sinf(2 * v->ph) + 0.33f * sinf(3 * v->ph) + 0.6f * sinf(1.5f * v->ph);
        v->lp += (tone - v->lp) * 0.25f;
        s = v->lp * env * 0.42f; break; }
    case SND_CHIME: { dur = 1.0f; // three rising notes
        int k = t < 0.12f ? 0 : t < 0.24f ? 1 : 2;
        float tt = t - 0.12f * k;
        v->ph += TAU * (k == 0 ? 523.0f : k == 1 ? 659.0f : 784.0f) * dt;
        s = sinf(v->ph) * expf(-tt * 6) * 0.4f + sinf(2 * v->ph) * expf(-tt * 10) * 0.12f; break; }
    case SND_BEEP: dur = 0.14f; // the last seconds before a wave
        s = (sinf(TAU * 988 * t) > 0 ? 0.2f : -0.2f) * clampf((0.13f - t) / 0.02f, 0, 1); break;
    case SND_WELD: dur = 0.09f; // the repair torch: crackle over a buzz
        v->ph += TAU * 120 * dt;
        s = (noise() * (noise() > 0.55f ? 0.8f : 0.2f) + (sinf(v->ph) > 0 ? 0.1f : -0.1f)) * sinf(3.14159f * t / dur); break;
    default: dur = 0.03f;
        s = noise() * expf(-t * 160) * 0.5f; break;
    }
    if (t > dur) v->on = 0;
    return s;
}

static void mix(short *buf, int n) { // n stereo frames
    const float dt = 1.0f / RATE, TAU = 6.2831853f;
        for (int i = 0; i < n; i++) {
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
            if (living) { // a double thump that quickens as it runs, over slow breathing
                float period = 1 / (1.0f + 0.9f * engPitch), dub = beatT - 0.28f * period;
                beatT += dt;
                if (beatT > period) beatT -= period;
                float thump = sinf(TAU * 52 * beatT) * expf(-beatT * 22) + (dub > 0 ? sinf(TAU * 44 * dub) * expf(-dub * 26) * 0.7f : 0);
                mono = (thump * 0.8f + engLp * (0.5f + 0.5f * sinf(TAU * 0.22f * alarmT)) * 0.35f) * eng;
            }
            alarmT += dt;
            if (alarmOn > 0.5f && fmodf(alarmT, 0.5f) < 0.22f) mono += (sinf(TAU * (fmodf(alarmT, 1.0f) < 0.5f ? 880 : 660) * alarmT) > 0 ? 0.05f : -0.05f);
            windLp += (noise() - windLp) * 0.012f;
            mono += windLp * 0.10f;
            l += mono; r += mono;
            l = l / (1 + fabsf(l)); r = r / (1 + fabsf(r)); // soft limit
            buf[i * 2] = (short)(l * 30000);
            buf[i * 2 + 1] = (short)(r * 30000);
        }
}

#ifdef _WIN32
// A ring of short buffers: the thread refills each one as soon as the sound card hands it back.
#define NBUF 4
#define BUFFRAMES 512 // 4 x 512 frames is about 43 ms queued, close to the 40 ms asked of ALSA
static WAVEHDR hdr[NBUF];
static short wbuf[NBUF][BUFFRAMES * 2];

static DWORD WINAPI mix_thread(LPVOID arg) {
    for (int next = 0; running; next = (next + 1) % NBUF) {
        WAVEHDR *h = &hdr[next];
        while (running && (h->dwFlags & WHDR_INQUEUE)) WaitForSingleObject(bufferDone, 100);
        if (!running) break;
        LOCK();
        mix(wbuf[next], BUFFRAMES);
        UNLOCK();
        // If the device went away (a headset's audio switched off, say), keep the pace instead of spinning.
        if (waveOutWrite(pcm, h, sizeof *h) != MMSYSERR_NOERROR) Sleep(BUFFRAMES * 1000 / RATE);
    }
    return (DWORD)(INT_PTR)arg;
}

void audio_init(void) {
    WAVEFORMATEX wf = {0};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = RATE;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = RATE * 4;
    bufferDone = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (waveOutOpen(&pcm, WAVE_MAPPER, &wf, (DWORD_PTR)bufferDone, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        fprintf(stderr, "audio: cannot open the sound device; playing without sound\n");
        CloseHandle(bufferDone);
        pcm = NULL;
        return;
    }
    for (int i = 0; i < NBUF; i++) {
        hdr[i].lpData = (LPSTR)wbuf[i];
        hdr[i].dwBufferLength = sizeof wbuf[i];
        waveOutPrepareHeader(pcm, &hdr[i], sizeof hdr[i]);
    }
    running = 1;
    thread = CreateThread(NULL, 0, mix_thread, NULL, 0, NULL);
    SetThreadPriority(thread, THREAD_PRIORITY_HIGHEST);
}
void audio_shutdown(void) {
    if (!pcm) return;
    running = 0;
    SetEvent(bufferDone);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    waveOutReset(pcm);
    for (int i = 0; i < NBUF; i++) waveOutUnprepareHeader(pcm, &hdr[i], sizeof hdr[i]);
    waveOutClose(pcm);
    CloseHandle(bufferDone);
    pcm = NULL;
}
#else
static void *mix_thread(void *arg) {
    short buf[CHUNK * 2];
    while (running) {
        LOCK();
        mix(buf, CHUNK);
        UNLOCK();
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
#endif

void audio_offline(void) { offline = 1; }
void audio_render(short *out, int frames) {
    LOCK();
    mix(out, frames);
    UNLOCK();
}

void audio_play(int type, v3 pos, float gain) {
    if (!pcm && !offline) return;
    v3 d = vsub(pos, earPos);
    float dist = vlen(d), pan = dist > 0.3f ? vdot(d, earRight) / dist : 0;
    float g = gain * 14.0f / (14.0f + dist); // big machines: loud nearby, still audible far away
    if (g < 0.004f) return;
    LOCK();
    int slot = 0;
    for (int k = 0; k < NVOICE; k++) { if (!voice[k].on) { slot = k; break; } if (voice[k].t > voice[slot].t) slot = k; }
    voice[slot] = (Voice){type, 1, 0, g * (0.6f - 0.4f * pan), g * (0.6f + 0.4f * pan), 0, 0};
    UNLOCK();
}
void audio_body(int isLiving) { living = isLiving; }
void audio_listener(v3 pos, v3 right, float engine, float pitch, float alarm) {
    earPos = pos; earRight = right; engTarget = engine; engPitch = pitch; alarmOn = alarm;
}
