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

int main(int argc, char **argv) {
    int flat = 0, maxFrames = 0, demo = 0, bench = 0, fw = 1280, fh = 720, haveCam = 0, look = 0, foot = 0;
    float headYaw = 0, holdX = 0, simT = 0;
    int autoplay = 0;
    const char *shot = NULL;
    float sim = 0, cam[5] = {0};
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
        else if (!strcmp(argv[i], "--swarm") && i + 1 < argc) game_swarm(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--worm") && i + 1 < argc) game_worm((float)atof(argv[++i]));
        else if (!strcmp(argv[i], "--build") && i + 3 < argc) { game_loadout(atoi(argv[i + 1]), atoi(argv[i + 2]), atoi(argv[i + 3])); i += 3; }
        else if (!strcmp(argv[i], "--nocards")) game_cards = 0;
        else if (!strcmp(argv[i], "--calm")) game_calm = 1;
        else if (!strcmp(argv[i], "--calm-mechs")) game_calm = 2;
        else if (!strcmp(argv[i], "--auto") && i + 1 < argc) autoplay = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--head") && i + 2 < argc) { headYaw = (float)atof(argv[++i]); cam[4] = (float)atof(argv[++i]); }
        else if (!strcmp(argv[i], "--bench")) { bench = flat = 1; fw = 1628; fh = 1940; }
        else if (!strcmp(argv[i], "--cam") && i + 5 < argc) { for (int k = 0; k < 5; k++) cam[k] = (float)atof(argv[++i]); haveCam = 1; }
        else { printf("usage: mechvr [--flat] [--foot] [--look 0-2] [--demo 0-4] [--shot file.ppm] [--sim seconds] [--cam x y z yaw pitch] [--bench] [--frames n] [--shapes file] [--horde n] [--swarm n] [--worm metres] [--build body armL armR] [--calm] [--nocards]\n"); return 0; }
    }
    Gfx g;
    if (!gfx_init(&g, flat && !bench ? fw : 960, flat && !bench ? fh : 540, "MechVR")) return 1;
    if (!flat && !xr_init(&g)) { fprintf(stderr, "No headset session. Start SteamVR, or run with --flat.\n"); return 1; }
    g.mx = g.w / 2;
    g.my = g.h / 2;
    if (!shot && !bench) audio_init();
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
    float tanH = 0.84f, tanV = tanH * fh / fw;
    if (bench) { tanH = 1.0f; tanV = 1.19f; }

    double last = gfx_time(), tStat = last, worst = 0, gpuSum = 0, prevStart = last;
    int frames = 0, statFrames = 0;
    float simLeft = sim;
    while (!g.quit && (flat || xr_poll())) {
        gfx_poll(&g);
        if (mech_shapes_poll()) mech_impostors_bake();
        if (g.key[K_ESC]) break;
        double now = gfx_time();
        Input in = {0};
        XrFrame xf_ = {0};
        if (flat) {
            in.dt = shot || bench ? 1.0f / 90 : (float)clampf((float)(now - prevStart), 0.001f, 0.05f);
            prevStart = now;
            if (g.mbtn[3]) { in.flatYaw = -g.mdx * 0.004f; pitch = clampf(pitch - g.mdy * 0.004f, -1.5f, 1.5f); }
            in.flatYaw += (g.key[K_LEFT] - g.key[K_RIGHT]) * 1.8f * in.dt;
            if (game_piloting()) { headYaw = clampf(headYaw + in.flatYaw, -2.4f, 2.4f); in.flatYaw = 0; } // in the cockpit you turn your head, not the mech
            else headYaw = 0;
            pitch = clampf(pitch + (g.key[K_UP] - g.key[K_DOWN]) * 1.2f * in.dt, -1.5f, 1.5f);
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
            in.ejectKey = g.pressed['x'];
            for (int k = 1; k <= 3; k++) if (g.pressed['0' + k]) in.lookKey = k;
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
        do { // --sim fast-forwards the world before the first picture
            simT += in.dt;
            if (autoplay == 1) { in.hand[0].sy = simT < 2.5f ? 1 : 0; in.hand[0].trigger = in.hand[1].trigger = 1; } // self-test: advance and fire
            if (autoplay == 2) in.ejectKey = simT > 1.0f && simT < 1.02f;                                              // self-test: eject
            if (autoplay == 3) in.boardKey = simT > 0.5f && simT < 0.52f;                                              // self-test: board
            if (autoplay == 5) { in.hand[0].sy = simT < 2.5f ? 1 : 0; in.boardKey = fmodf(simT, 0.5f) < 0.02f; }                // self-test: walk ahead, grafting any arm in reach
            if (autoplay == 4) { in.hand[0].sy = 1; in.hand[0].sx = 0.35f; }                                            // self-test: walk into the patrols
            game_update(&in);
            simLeft -= in.dt;
        } while (shot && simLeft > 0);
        game_scene(&scene, &in);
        xf rig = game_rig();

        double g0 = gfx_time();
        if (flat) {
            int views = bench ? 2 : 1;
            for (int e = 0; e < views; e++) {
                View v = {xfmul(rig, XF(vadd(in.head.p, V3(e * 0.064f, 0, 0)), in.head.q)), -tanH, tanH, -tanV, tanV};
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
    audio_shutdown();
    if (!flat) xr_shutdown();
    if (bench || shot) game_report();
    return 0;
}
