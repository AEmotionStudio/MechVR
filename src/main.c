// MechVR style test. Runs in the headset by default; --flat runs in a window with mouse and keyboard.
#include "audio.h"
#include "game.h"
#include "gfx.h"
#include "mech.h"
#include "render.h"
#include "world.h"
#include "xr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(d) _mkdir(d)
#else
#include <sys/stat.h>
#define MKDIR(d) mkdir(d, 0755)
#endif

static void save_ppm(const char *path, int w, int h) {
    unsigned char *px = malloc((size_t)w * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = h - 1; y >= 0; y--)
            for (int x = 0; x < w; x++) fwrite(px + ((size_t)y * w + x) * 4, 1, 3, f);
        fclose(f);
    }
    free(px);
}

// ---- recording videos ------------------------------------------------------------------------------
// --video writes raw RGB frames (pipe them into ffmpeg) at a fixed rate, the world stepping exactly 1/90 s at a
// time, and --wav writes the matching sound. A scripted camera and a scripted pilot make the footage.
static void write_wav_header(FILE *f, unsigned frames) {
    unsigned bytes = frames * 4, v;
    unsigned short h;
    fwrite("RIFF", 1, 4, f); v = 36 + bytes; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    v = 16; fwrite(&v, 4, 1, f); h = 1; fwrite(&h, 2, 1, f); h = 2; fwrite(&h, 2, 1, f);
    v = 48000; fwrite(&v, 4, 1, f); v = 48000 * 4; fwrite(&v, 4, 1, f); h = 4; fwrite(&h, 2, 1, f); h = 16; fwrite(&h, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
}
static float step_to(float v, float target, float step) { return v + clampf(target - v, -step, step); }
static v3 ease(v3 v, v3 target, float k) { return vadd(v, vmul(vsub(target, v), k)); }
static quat qnlerp(quat a, quat b, float t) { // part way from a towards b, the short way round
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0) b = Q(-b.x, -b.y, -b.z, -b.w);
    return qnorm(Q(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t));
}

// ---- recording what you play ------------------------------------------------------------------------
// The world changes only through each frame's Input, so --record saves the options that shaped the world and then
// every frame's Input (head, hands, buttons, frame time): about 25 KB a second, nothing the headset will notice.
// --replay steps a fresh world through the same inputs, which plays the fight again exactly, and --video above can
// film it from your own eyes (steadied by --smooth) or from any scripted camera.
#define REC_MAGIC "MechVRr1"
typedef struct { Input in; v3 check; } RecFrame; // check: where you were after the step, to notice a replay drifting
static const struct { const char *opt; int args; } worldOpts[] = { // the options a recording keeps
    {"--foot", 0}, {"--demo", 1}, {"--look", 1}, {"--horde", 1}, {"--swarm", 1}, {"--worm", 1},
    {"--build", 3}, {"--calm", 0}, {"--calm-mechs", 0}, {"--nocards", 0}, {"--shapes", 1}, {"--sandbox", 0}, {"--hurt", 1}, {"--wave", 1}};

static FILE *rec_open(const char *path, int argc, char **argv, char *name, size_t nameSize) {
    if (!*path) { // no name given: recordings/mechvr-<date>-<time>.mvr
        time_t now = time(NULL);
        char stamp[32];
        strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&now));
        MKDIR("recordings");
        snprintf(name, nameSize, "recordings/mechvr-%s.mvr", stamp);
    } else snprintf(name, nameSize, "%s", path);
    FILE *f = fopen(name, "wb");
    if (!f) { perror(name); return NULL; }
    char opts[1024];
    unsigned n = 0;
    for (int i = 1; i < argc; i++)
        for (unsigned k = 0; k < sizeof worldOpts / sizeof worldOpts[0]; k++)
            if (!strcmp(argv[i], worldOpts[k].opt) && i + worldOpts[k].args < argc)
                for (int j = 0; j <= worldOpts[k].args; j++) {
                    size_t len = strlen(argv[i + j]) + 1;
                    if (n + len <= sizeof opts) { memcpy(opts + n, argv[i + j], len); n += (unsigned)len; }
                }
    unsigned hdr[2] = {(unsigned)sizeof(RecFrame), n};
    fwrite(REC_MAGIC, 1, 8, f);
    fwrite(hdr, sizeof hdr, 1, f);
    fwrite(opts, 1, n, f);
    return f;
}

// Opens a recording and puts the options it was made with in front of the command line's own.
static FILE *replay_open(const char *path, int *argc, char ***argv) {
    static char opts[1025];
    static char *args[320];
    char magic[8];
    unsigned hdr[2];
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, REC_MAGIC, 8) || fread(hdr, sizeof hdr, 1, f) != 1 ||
        hdr[0] != sizeof(RecFrame) || hdr[1] > 1024 || fread(opts, 1, hdr[1], f) != hdr[1]) {
        fprintf(stderr, "replay: %s is not a recording this build of MechVR can play\n", path);
        fclose(f);
        return NULL;
    }
    int n = 0;
    args[n++] = (*argv)[0];
    printf("replay: %s, recorded with:", path);
    for (unsigned p = 0; p < hdr[1] && n < 64; p += (unsigned)strlen(opts + p) + 1) printf(" %s", args[n++] = opts + p);
    long start = ftell(f), frames = 0;
    double len = 0;
    RecFrame fr;
    int vr = 0;
    while (fread(&fr, sizeof fr, 1, f) == 1) { len += fr.in.dt; vr |= fr.in.vr; frames++; }
    fseek(f, start, SEEK_SET);
    printf("%s\nreplay: %ld frames, %.1f s, played %s\n", n > 1 ? "" : " (no options)", frames, len, vr ? "in the headset" : "in a window");
    for (int i = 1; i < *argc && n < 319; i++) args[n++] = (*argv)[i];
    args[n] = NULL;
    *argc = n;
    *argv = args;
    return f;
}

int main(int argc, char **argv) {
    int flat = 0, maxFrames = 0, demo = 0, bench = 0, fw = 1280, fh = 720, haveCam = 0, look = 0, foot = 0;
    float headYaw = 0, holdX = 0, simT = 0;
    int autoplay = 0;
    const char *shot = NULL;
    float sim = 0, cam[5] = {0};
    // recording
    const char *videoPath = NULL, *wavPath = NULL;
    float videoLen = -1, fovDeg = 0, orbit[4] = {0}, chase[3] = {0}, lookMix = 0, ejectAt = -1;
    int fps = 30, camMode = 0, lookWhat = -1, pilot = 0; // camMode: 0 your own eyes, 1 placed, 2 orbiting, 3 riding behind your mech
    v3 camPos = {0, 0, 0}, camDrift = {0, 0, 0}, lookAt = {0, 0, 0};
    // recording what you play, and playing it back
    const char *recPath = NULL;
    char recName[256] = "";
    FILE *rec = NULL, *rep = NULL;
    float smooth = 0.15f;
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--replay")) {
            if (!(rep = replay_open(argv[i + 1], &argc, &argv))) return 1;
            break;
        }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--flat")) flat = 1;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) maxFrames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) { shot = argv[++i]; flat = 1; }
        else if (!strcmp(argv[i], "--demo") && i + 1 < argc) demo = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--look") && i + 1 < argc) look = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sim") && i + 1 < argc) sim = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--foot")) foot = 1;
        else if (!strcmp(argv[i], "--shapes") && i + 1 < argc) mech_shapes_live(argv[++i]);
        else if (!strcmp(argv[i], "--horde") && i + 1 < argc) game_horde(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--sandbox")) game_waves(0);
        else if (!strcmp(argv[i], "--hurt") && i + 1 < argc) game_hurt(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--wave") && i + 1 < argc) game_first_wave(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--swarm") && i + 1 < argc) game_swarm(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--worm") && i + 1 < argc) game_worm((float)atof(argv[++i]));
        else if (!strcmp(argv[i], "--build") && i + 3 < argc) { game_loadout(atoi(argv[i + 1]), atoi(argv[i + 2]), atoi(argv[i + 3])); i += 3; }
        else if (!strcmp(argv[i], "--nocards")) game_cards = 0;
        else if (!strcmp(argv[i], "--calm")) game_calm = 1;
        else if (!strcmp(argv[i], "--calm-mechs")) game_calm = 2;
        else if (!strcmp(argv[i], "--auto") && i + 1 < argc) autoplay = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--head") && i + 2 < argc) { headYaw = (float)atof(argv[++i]); cam[4] = (float)atof(argv[++i]); }
        else if (!strcmp(argv[i], "--bench")) { bench = flat = 1; fw = 1628; fh = 1940; game_waves(0); } // measured in the fixed layout, to compare builds
        else if (!strcmp(argv[i], "--video") && i + 1 < argc) { videoPath = argv[++i]; flat = 1; }
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wavPath = argv[++i];
        else if (!strcmp(argv[i], "--len") && i + 1 < argc) videoLen = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 2 < argc) { fw = atoi(argv[i + 1]); fh = atoi(argv[i + 2]); i += 2; }
        else if (!strcmp(argv[i], "--fov") && i + 1 < argc) fovDeg = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--campos") && i + 3 < argc) { camPos = V3(atof(argv[i + 1]), atof(argv[i + 2]), atof(argv[i + 3])); i += 3; if (!camMode) camMode = 1; }
        else if (!strcmp(argv[i], "--drift") && i + 3 < argc) { camDrift = V3(atof(argv[i + 1]), atof(argv[i + 2]), atof(argv[i + 3])); i += 3; }
        else if (!strcmp(argv[i], "--orbit") && i + 4 < argc) { for (int k = 0; k < 4; k++) orbit[k] = (float)atof(argv[++i]); camMode = 2; }
        else if (!strcmp(argv[i], "--chase") && i + 3 < argc) { for (int k = 0; k < 3; k++) chase[k] = (float)atof(argv[++i]); camMode = 3; }
        else if (!strcmp(argv[i], "--lookat") && i + 1 < argc) {
            const char *w = argv[++i];
            lookWhat = !strcmp(w, "worm") ? FOCUS_WORM : !strcmp(w, "me") ? FOCUS_ME : !strcmp(w, "enemy") ? FOCUS_ENEMY : -1;
            if (lookWhat < 0 && i + 2 < argc) { lookAt = V3(atof(w), atof(argv[i + 1]), atof(argv[i + 2])); i += 2; }
        }
        else if (!strcmp(argv[i], "--pilot")) pilot = 1;
        else if (!strcmp(argv[i], "--lookmix") && i + 1 < argc) lookMix = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--eject-at") && i + 1 < argc) ejectAt = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--cam") && i + 5 < argc) { for (int k = 0; k < 5; k++) cam[k] = (float)atof(argv[++i]); haveCam = 1; }
        else if (!strcmp(argv[i], "--record")) recPath = i + 1 < argc && strncmp(argv[i + 1], "--", 2) ? argv[++i] : "";
        else if (!strcmp(argv[i], "--replay") && i + 1 < argc) { i++; flat = 1; } // opened above
        else if (!strcmp(argv[i], "--smooth") && i + 1 < argc) smooth = (float)atof(argv[++i]);
        else { printf("usage: mechvr [--flat] [--foot] [--look 0-2] [--demo 0-4] [--shot file.ppm] [--sim seconds] [--cam x y z yaw pitch] [--bench] [--frames n] [--shapes file] [--horde n] [--swarm n] [--worm metres] [--build body armL armR] [--calm] [--nocards] [--sandbox] [--wave n] [--hurt 1-3]\n"
                                "playing back: --record [file] --replay file --smooth s\n"
                                "recording: --video file --wav file --len s --fps n --size w h --fov deg --campos x y z --drift x y z --orbit r h deg/s start\n"
                                "           --chase back up side --lookat x y z|worm|me|enemy --lookmix k --pilot --eject-at s\n"); return 0; }
    }
    if (videoLen < 0) videoLen = rep ? 0 : 20; // a replay is filmed to its end unless --len says otherwise
    if (rep) autoplay = 0;                      // the recording already holds everything that was done
    if (recPath && !rep) {
        if (!(rec = rec_open(recPath, argc, argv, recName, sizeof recName))) return 1;
        printf("record: saving what you play to %s\n", recName);
    }
    Gfx g;
    if (!gfx_init(&g, flat && !bench ? fw : 960, flat && !bench ? fh : 540, "MechVR")) return 1;
    if (!flat && !xr_init(&g)) { fprintf(stderr, "No headset session. Start SteamVR, or run with --flat.\n"); return 1; }
    g.mx = g.w / 2;
    g.my = g.h / 2;
    FILE *vout = NULL, *wout = NULL;
    unsigned wavFrames = 0;
    unsigned char *pix = NULL;
    if (videoPath) {
        if (90 % fps) { fprintf(stderr, "--fps must divide 90 (30, 45 or 90)\n"); return 1; }
        vout = fopen(videoPath, "wb");
        if (!vout) { perror(videoPath); return 1; }
        pix = malloc((size_t)fw * fh * 4);
        if (wavPath && (wout = fopen(wavPath, "wb"))) { write_wav_header(wout, 0); audio_offline(); }
    } else if (!shot && !bench) audio_init();
    game_init(demo, foot);
    render_init(world_h, HN);
    double tBake = gfx_time();
    mech_impostors_bake();
    glFinish();
    printf("cards: drew the sheets of distant-mech views in %.0f ms\n", (gfx_time() - tBake) * 1000);
    game_look = look;

    GLuint flatTex = gfx_tex(fw, fh, GL_RGBA8, GL_LINEAR), blitFbo;
    glGenFramebuffers(1, &blitFbo);
    static Scene scene;
    float pitch = cam[4];
    if (haveCam) game_set_cam(V3(cam[0], cam[1], cam[2]), cam[3]);
    float tanH = fovDeg > 0 ? tanf(fovDeg * PI / 360) : 0.84f, tanV = tanH * fh / fw;
    if (bench) { tanH = 1.0f; tanV = 1.19f; }

    double last = gfx_time(), tStat = last, worst = 0, gpuSum = 0, prevStart = last;
    int frames = 0, statFrames = 0;
    float simLeft = sim;
    int written = 0, total = rep && videoLen <= 0 ? 1 << 30 : (int)(videoLen * fps + 0.5f);
    double clock = 0, audioAcc = 0;
    v3 lookSm = {0, 0, 0}, posSm = {0, 0, 0};
    int camFresh = 1;
    long recFrames = 0;
    Input repIn = {0};                                   // the input of the last recorded frame played
    double replayT0 = gfx_time() - sim, lastRender = replayT0; // watching a replay: it runs in real time from here (--sim skips ahead)
    quat headSm = qid();
    int headFresh = 1, repEnd = 0, drifted = 0;
    while (!g.quit && (flat || xr_poll())) {
        gfx_poll(&g);
        if (mech_shapes_poll()) mech_impostors_bake();
        if (g.key[K_ESC]) break;
        if (videoPath) { // recording: the script drives, so the mouse and keyboard are ignored
            memset(g.key, 0, sizeof g.key);
            memset(g.pressed, 0, sizeof g.pressed);
            memset(g.mbtn, 0, sizeof g.mbtn);
            g.mx = g.w / 2;
            g.my = g.h / 2;
            g.mdx = g.mdy = 0;
        }
        float sSx = 0, sSy = 0, sFire = 0; // what the scripted pilot does this frame
        double now = gfx_time();
        Input in = {0};
        XrFrame xf_ = {0};
        if (rep) {
            in = repIn; // the recording drives the world, frame by frame in the stepping below
        } else if (flat) {
            in.dt = shot || bench || videoPath ? 1.0f / 90 : (float)clampf((float)(now - prevStart), 0.001f, 0.05f);
            prevStart = now;
            if (g.mbtn[3]) { in.flatYaw = -g.mdx * 0.004f; pitch = clampf(pitch - g.mdy * 0.004f, -1.5f, 1.5f); }
            in.flatYaw += (g.key[K_LEFT] - g.key[K_RIGHT]) * 1.8f * in.dt;
            if (game_piloting()) { headYaw = clampf(headYaw + in.flatYaw, -2.4f, 2.4f); in.flatYaw = 0; } // in the cockpit you turn your head, not the mech
            else headYaw = 0;
            pitch = clampf(pitch + (g.key[K_UP] - g.key[K_DOWN]) * 1.2f * in.dt, -1.5f, 1.5f);
            if (pilot && game_piloting()) { // the scripted pilot: look at the nearest enemy, turn towards it, close in, fire
                const float fdt = 1.0f / fps;
                xf r = game_rig();
                v3 eye = xfpt(r, V3(0, 1.7f, 0)), t;
                int has = game_focus(FOCUS_ENEMY, 240, &t);
                float yawErr = 0.45f * sinf((float)clock * 0.5f), pitchErr = -0.06f, dist = 0;
                if (has) {
                    v3 l = qrot(qconj(r.q), vsub(t, eye));
                    yawErr = atan2f(-l.x, -l.z);
                    dist = sqrtf(l.x * l.x + l.z * l.z);
                    pitchErr = atan2f(l.y, dist);
                }
                headYaw = step_to(headYaw, clampf(yawErr, -0.5f, 0.5f), 1.2f * fdt); // the head leads a little, the mech turns to follow
                pitch = step_to(pitch, clampf(pitchErr, -0.5f, 0.5f), 0.8f * fdt);
                sSx = fabsf(yawErr) > 0.08f ? clampf(-yawErr * 2.5f, -1, 1) : 0;
                float want = !has ? 0.55f : dist > 90 ? 0.7f : dist > 45 ? 0.3f : 0;
                sSy = clampf((want - game_throttle()) * 6, -1, 1);
                sFire = has && dist < 220 && fabsf(headYaw - yawErr) < 0.05f && fabsf(pitch - pitchErr) < 0.06f;
            }
            in.head = XF(V3(0, haveCam ? 0 : 1.7f, 0), qmul(qaxis(V3(0, 1, 0), headYaw), qaxis(V3(1, 0, 0), pitch)));
            in.hand[0].sx = (float)(g.key['d'] - g.key['a']);
            in.hand[0].sy = (float)(g.key['w'] - g.key['s']);
            in.flyUp = (float)(g.key[' '] - g.key[K_SHIFT]);
            v3 dir = vnorm(V3(mixf(-tanH, tanH, (float)g.mx / maxf(1, (float)g.w)), mixf(tanV, -tanV, (float)g.my / maxf(1, (float)g.h)), -1));
            v3 wd = qrot(in.head.q, dir);
            in.hand[1].tracked = 1;
            in.hand[1].aim = XF(vadd(in.head.p, qrot(in.head.q, V3(0.2f, -0.18f, -0.4f))),
                                qeuler(asinf(clampf(wd.y, -1, 1)), atan2f(-wd.x, -wd.z), 0));
            in.hand[1].grip = in.hand[1].aim;
            in.hand[1].trigger = (float)g.mbtn[1];
            in.hand[0].trigger = (float)g.key[' '];
            in.resetKey = g.pressed['r'];
            in.toggleFly = g.pressed['f'];
            in.boardKey = g.pressed['e'];
            in.hand[1].a = g.key['e']; // held: climb out of your mech
            in.ejectKey = g.pressed['x'];
            for (int k = 1; k <= 3; k++) if (g.pressed['0' + k]) in.lookKey = k;
            if (ejectAt >= 0 && clock >= ejectAt && game_piloting()) { in.ejectKey = 1; ejectAt = -1; }
            if (pilot && !game_piloting()) { headYaw = 0; pitch = step_to(pitch, -0.55f, 0.6f / fps); } // thrown clear: look down at what you left
            if (pilot) {
                in.hand[0].sx = sSx;
                in.hand[0].sy = sSy;
                in.hand[1].trigger = sFire;
                in.hand[0].trigger = game_piloting() ? sFire : 0; // on foot the left trigger is the repair torch, which the pilot leaves alone
            }
        } else {
            xr_begin(&xf_);
            in.vr = 1;
            in.headValid = xf_.render;
            in.dt = xf_.dt > 0 ? (float)xf_.dt : 1.0f / 90;
            in.head = xf_.head;
            in.hand[0] = xf_.hand[0];
            in.hand[1] = xf_.hand[1];
            if (xf_.hand[1].bDown) in.lookKey = (game_look + 1) % 3 + 1;
            in.boardKey = xf_.hand[1].aDown;
            in.recenterKey = xf_.hand[0].bDown;
            in.resetKey = xf_.menu && xf_.hand[0].squeeze > 0.5f;
            holdX = xf_.hand[0].a ? holdX + in.dt : 0; // backup eject: hold X for one second
            if (holdX > 1.0f) { in.ejectKey = 1; holdX = 0; }
        }
        int steps = videoPath ? 90 / fps : 1, recording = !videoPath || clock >= sim - 1e-4;
        // A replay steps through the recording until the world reaches the time of this picture (or of --sim).
        double until = videoPath ? clock + 1.0 / fps - 1e-6 : shot ? sim : gfx_time() - replayT0;
        for (int first = 1;; first = 0) { // --sim fast-forwards the world before the first picture
            RecFrame fr = {0};
            if (rep) {
                if ((videoPath ? clock : simT) >= until) break;
                if (fread(&fr, sizeof fr, 1, rep) != 1) { repEnd = 1; break; }
                in = repIn = fr.in;
            } else if (!first && !((shot && simLeft > 0) || --steps > 0)) break;
            simT += in.dt;
            if (autoplay == 1) { in.hand[0].sy = simT < 2.5f ? 1 : 0; in.hand[0].trigger = in.hand[1].trigger = 1; } // self-test: advance and fire
            if (autoplay == 2) in.ejectKey = simT > 1.0f && simT < 1.02f;                                              // self-test: eject
            if (autoplay == 3) in.boardKey = simT > 0.5f && simT < 0.52f;                                              // self-test: board
            if (autoplay == 5) { in.hand[0].sy = simT < 2.5f ? 1 : 0; in.boardKey = fmodf(simT, 0.5f) < 0.02f; }                // self-test: walk ahead, grafting any arm in reach
            if (autoplay == 4) { in.hand[0].sy = 1; in.hand[0].sx = 0.35f; }                                            // self-test: walk into the patrols
            if (autoplay == 6) { in.hand[1].a = simT < 1.0f; in.hand[0].trigger = simT > 2.0f; in.hand[1].aim = in.head; } // self-test: climb out, then repair straight ahead (aim with --head)
            game_update(&in);
            v3 where = game_rig().p;
            if (rec) {
                RecFrame f = {in, where};
                fwrite(&f, sizeof f, 1, rec);
                if (++recFrames % 90 == 0) fflush(rec); // a crash or a killed game loses a second at most
            }
            if (rep && !drifted && memcmp(&where, &fr.check, sizeof where)) {
                drifted = 1;
                fprintf(stderr, "replay: the fight went its own way %.1f s in, so from here on it may not match what was played\n"
                                "        (a recording plays back exactly only with the build and shapes it was made with)\n", simT);
            }
            simLeft -= in.dt;
            if (videoPath) { // the sound of exactly this step, kept only once recording has begun
                clock += in.dt;
                audioAcc += 48000.0 * in.dt;
                int n = (int)audioAcc;
                audioAcc -= n;
                short buf[2 * 600];
                for (int k; wout && n > 0; n -= k) { // in pieces: a replayed frame played in a window can be 50 ms long
                    k = n < 600 ? n : 600;
                    audio_render(buf, k);
                    if (recording) { fwrite(buf, 4, k, wout); wavFrames += k; }
                }
            }
        }
        if (repEnd) break;
        if (!recording) continue;

        if (camMode) { // the scripted camera
            const float fdt = 1.0f / fps, t = written * fdt;
            v3 target = lookAt, f, me;
            game_focus(FOCUS_ME, 0, &me);
            xf r = game_rig();
            v3 fwd = qrot(r.q, V3(0, 0, -1));
            fwd = vnorm(V3(fwd.x, 0, fwd.z));
            if (lookWhat >= 0 && game_focus(lookWhat, 260, &f)) target = f;
            else if (lookWhat >= 0) target = camFresh ? vadd(me, vmul(fwd, 30)) : lookSm;
            if (camMode == 3 && lookWhat < 0) target = vadd(me, vadd(vmul(fwd, 30), V3(0, -2, 0)));
            target = vadd(target, vmul(vsub(me, target), lookMix)); // part way towards your mech, to keep both in frame
            lookSm = camFresh ? target : ease(lookSm, target, 1 - expf(-fdt / 0.45f));
            v3 pos = camPos;
            if (camMode == 1) pos = vadd(camPos, vmul(camDrift, t));
            if (camMode == 2) {
                float a = (orbit[3] + orbit[2] * t) * PI / 180;
                v3 c = lookWhat >= 0 ? lookSm : lookAt;
                pos = vadd(c, V3(sinf(a) * orbit[0], orbit[1], cosf(a) * orbit[0]));
            }
            if (camMode == 3) pos = vadd(me, vadd(vmul(fwd, -chase[0]), V3(fwd.z * -chase[2], chase[1], fwd.x * chase[2])));
            pos.y = maxf(pos.y, world_height(pos.x, pos.z) + 1.2f);
            posSm = camFresh || camMode != 3 ? pos : ease(posSm, pos, 1 - expf(-fdt / 0.35f));
            camFresh = 0;
            v3 d = vnorm(vsub(lookSm, posSm));
            game_outside = 1;
            game_cam = XF(posSm, qeuler(asinf(clampf(d.y, -1, 1)), atan2f(-d.x, -d.z), 0));
        }
        game_scene(&scene, &in);
        xf rig = game_rig();

        double g0 = gfx_time();
        if (flat) {
            int views = bench ? 2 : 1;
            xf head = in.head;
            if (rep && smooth > 0) { // your own view, steadied: every small turn of a head is too shaky to watch on a screen
                float fdt = videoPath ? 1.0f / fps : (float)(now - lastRender);
                headSm = headFresh ? in.head.q : qnlerp(headSm, in.head.q, 1 - expf(-fdt / smooth));
                headFresh = 0;
                head.q = headSm;
            }
            lastRender = now;
            for (int e = 0; e < views; e++) {
                View v = {camMode ? game_cam : xfmul(rig, XF(vadd(head.p, V3(e * 0.064f, 0, 0)), head.q)), -tanH, tanH, -tanV, tanV};
                render_view(&scene, &v, flatTex, fw, fh, 0);
            }
            if (bench) { glFinish(); gpuSum += gfx_time() - g0; }
        } else if (xf_.render) {
            for (int e = 0; e < 2; e++) {
                xr_eye_acquire(&xf_, e);
                View v = {xfmul(rig, xf_.eye[e].pose), xf_.eye[e].l, xf_.eye[e].r, xf_.eye[e].d, xf_.eye[e].u};
                render_view(&scene, &v, xf_.eye[e].tex, xf_.eye[e].w, xf_.eye[e].h, !xr_srgb());
                if (e == 0) { // mirror the left eye to the desktop window
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, blitFbo);
                    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, xf_.eye[e].tex, 0);
                    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
                    int mh = g.h, mw = g.h * xf_.eye[e].w / xf_.eye[e].h;
                    glBlitFramebuffer(0, 0, xf_.eye[e].w, xf_.eye[e].h, (g.w - mw) / 2, 0, (g.w + mw) / 2, mh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
                }
                xr_eye_release(e);
            }
            glFinish();
            gpuSum += gfx_time() - g0;
        }
        if (!flat) {
            float hl, hr;
            game_haptics(&hl, &hr);
            if (hl > 0) xr_haptic(0, hl, 0.06f);
            if (hr > 0) xr_haptic(1, hr, 0.06f);
            xr_end(&xf_);
        } else {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, blitFbo);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, flatTex, 0);
            if (shot) { save_ppm(shot, fw, fh); break; }
            if (vout) {
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, fw, fh, GL_RGB, GL_UNSIGNED_BYTE, pix);
                for (int y = fh - 1; y >= 0; y--) fwrite(pix + (size_t)y * fw * 3, 1, (size_t)fw * 3, vout);
                if (++written >= total) break;
            }
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            glBlitFramebuffer(0, 0, fw, fh, 0, 0, g.w, g.h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        }
        gfx_swap(&g);

        double end = gfx_time();
        if (frames && end - last > worst) worst = end - last;
        last = end;
        frames++;
        statFrames++;
        if (end - tStat >= 2.0) {
            printf("%s: frame avg %.2f ms, worst %.2f ms, drawing both eyes %.2f ms, look %d\n", flat ? (bench ? "bench" : "flat") : "vr",
                   (end - tStat) * 1000 / statFrames, worst * 1000, gpuSum * 1000 / statFrames, game_look);
            fflush(stdout);
            tStat = end; statFrames = 0; worst = 0; gpuSum = 0;
        }
        if (maxFrames && frames >= maxFrames) break;
    }
    if (rec) {
        fclose(rec);
        printf("record: %ld frames, %.0f s, in %s\n        watch it again: mechvr --replay %s\n", recFrames, simT, recName, recName);
    }
    if (rep) {
        fclose(rep);
        if (repEnd) printf("replay: reached the end of the recording\n");
    }
    if (vout) fclose(vout);
    if (wout) { fseek(wout, 0, SEEK_SET); write_wav_header(wout, wavFrames); fclose(wout); }
    if (videoPath) fprintf(stderr, "recorded %d frames (%.1f s)\n", written, written / (float)fps);
    audio_shutdown();
    if (!flat) xr_shutdown();
    if (bench || shot || videoPath || rec || rep) game_report();
    return 0;
}
