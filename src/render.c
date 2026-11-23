#include "render.h"
#include <stdio.h>
#include <string.h>

#define SH(n) extern const char shader_##n[]; extern const int shader_##n##_len;
SH(tri_vert) SH(sky_frag) SH(terrain_vert) SH(terrain_frag) SH(part_vert) SH(part_frag)
SH(particle_vert) SH(particle_frag) SH(final_frag) SH(imp_vert) SH(imp_frag)
#define PROG(v, f) gfx_program(#f, shader_##v, shader_##v##_len, shader_##f, shader_##f##_len)

static GLuint pSky, pTerrain, pPart, pPtc, pFinal, pImp;
typedef struct { int rows, cw, ch; float width, y0, height, texel; GLuint fbo, col, nrm, depth; } ImpLayer;
static ImpLayer imps[2];
static GLuint vaoEmpty, vaoCube, vaoGrid, vaoPtc, vboPtc, texHeight, dstFbo;
static int gridIndexCount;

typedef struct { int w, h; GLuint fbo, col, nrm, depth; } Target;
static Target targets[2];

// Uniform locations are looked up once per (program, name) and remembered.
static struct { GLuint p; const char *n; GLint loc; } ucache[256];
static int ucount;
static GLint U(GLuint p, const char *n) {
    for (int i = 0; i < ucount; i++)
        if (ucache[i].p == p && ucache[i].n == n) return ucache[i].loc;
    GLint loc = glGetUniformLocation(p, n);
    if (ucount < 256) { ucache[ucount].p = p; ucache[ucount].n = n; ucache[ucount].loc = loc; ucount++; }
    return loc;
}

#define GRID_N 400
void render_init(const float *height, int hn) {
    pSky = PROG(tri_vert, sky_frag);
    pTerrain = PROG(terrain_vert, terrain_frag);
    pPart = PROG(part_vert, part_frag);
    pPtc = PROG(particle_vert, particle_frag);
    pFinal = PROG(tri_vert, final_frag);
    pImp = PROG(imp_vert, imp_frag);
    glGenVertexArrays(1, &vaoEmpty);
    glGenFramebuffers(1, &dstFbo);

    // Unit cube; each part scales it to its bounding box.
    static const float cv[24] = {-1, -1, -1, 1, -1, -1, -1, 1, -1, 1, 1, -1, -1, -1, 1, 1, -1, 1, -1, 1, 1, 1, 1, 1};
    static const unsigned char ci[36] = {1, 3, 7, 1, 7, 5, 0, 4, 6, 0, 6, 2, 2, 6, 7, 2, 7, 3,
                                         0, 1, 5, 0, 5, 4, 4, 5, 7, 4, 7, 6, 0, 2, 3, 0, 3, 1};
    GLuint b[2];
    glGenVertexArrays(1, &vaoCube);
    glBindVertexArray(vaoCube);
    glGenBuffers(2, b);
    glBindBuffer(GL_ARRAY_BUFFER, b[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof cv, cv, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b[1]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof ci, ci, GL_STATIC_DRAW);

    // Ground grid: fine near the camera, stretched toward the horizon.
    static float gv[(GRID_N + 1) * (GRID_N + 1) * 2];
    static unsigned gi[GRID_N * GRID_N * 6];
    for (int j = 0; j <= GRID_N; j++)
        for (int i = 0; i <= GRID_N; i++) {
            float u = (float)i / GRID_N * 2 - 1, v = (float)j / GRID_N * 2 - 1;
            gv[(j * (GRID_N + 1) + i) * 2] = 190 * u + 1700 * u * u * u * fabsf(u);
            gv[(j * (GRID_N + 1) + i) * 2 + 1] = 190 * v + 1700 * v * v * v * fabsf(v);
        }
    int k = 0;
    for (int j = 0; j < GRID_N; j++)
        for (int i = 0; i < GRID_N; i++) {
            unsigned a = j * (GRID_N + 1) + i, c = a + GRID_N + 1;
            gi[k++] = a; gi[k++] = c; gi[k++] = a + 1;
            gi[k++] = a + 1; gi[k++] = c; gi[k++] = c + 1;
        }
    gridIndexCount = k;
    glGenVertexArrays(1, &vaoGrid);
    glBindVertexArray(vaoGrid);
    glGenBuffers(2, b);
    glBindBuffer(GL_ARRAY_BUFFER, b[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof gv, gv, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b[1]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof gi, gi, GL_STATIC_DRAW);

    // Particle cards: one quad, drawn many times.
    static const float qv[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glGenVertexArrays(1, &vaoPtc);
    glBindVertexArray(vaoPtc);
    glGenBuffers(1, b);
    glBindBuffer(GL_ARRAY_BUFFER, b[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof qv, qv, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glGenBuffers(1, &vboPtc);
    glBindBuffer(GL_ARRAY_BUFFER, vboPtc);
    glBufferData(GL_ARRAY_BUFFER, sizeof(DrawParticle) * MAX_DRAW_PTC, NULL, GL_STREAM_DRAW);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(DrawParticle), (void *)0);
    glVertexAttribDivisor(1, 1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(DrawParticle), (void *)16);
    glVertexAttribDivisor(2, 1);
    glBindVertexArray(0);

    glGenTextures(1, &texHeight);
    glBindTexture(GL_TEXTURE_2D, texHeight);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32F, hn, hn);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, hn, hn, GL_RED, GL_FLOAT, height);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
}

static Target *target(int w, int h) {
    Target *t = NULL;
    for (int i = 0; i < 2; i++)
        if (targets[i].w == w && targets[i].h == h) return &targets[i];
    static int next;
    t = &targets[next++ & 1];
    if (t->fbo) {
        glDeleteFramebuffers(1, &t->fbo);
        glDeleteTextures(1, &t->col);
        glDeleteTextures(1, &t->nrm);
        glDeleteTextures(1, &t->depth);
    }
    t->w = w;
    t->h = h;
    t->col = gfx_tex(w, h, GL_RGBA8, GL_NEAREST);
    t->nrm = gfx_tex(w, h, GL_RGBA16F, GL_NEAREST);
    t->depth = gfx_tex(w, h, GL_DEPTH_COMPONENT32F, GL_NEAREST);
    glGenFramebuffers(1, &t->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->col, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, t->nrm, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, t->depth, 0);
    GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, bufs);
    return t;
}

static void rot3(quat q, float *o) {
    v3 x = qrot(q, V3(1, 0, 0)), y = qrot(q, V3(0, 1, 0)), z = qrot(q, V3(0, 0, 1));
    float m[9] = {x.x, x.y, x.z, y.x, y.y, y.z, z.x, z.y, z.z};
    memcpy(o, m, sizeof m);
}

static void part_shape(const Shape *sp, xf world, m4 vp) {
    m4 mvp = m4mul(vp, m4xf(world));
    float r[9];
    rot3(world.q, r);
    glUniformMatrix4fv(U(pPart, "uMVP"), 1, GL_FALSE, mvp.m);
    glUniformMatrix3fv(U(pPart, "uRot"), 1, GL_FALSE, r);
    glUniform3f(U(pPart, "uOriginW"), world.p.x, world.p.y, world.p.z);
    glUniform3f(U(pPart, "uBoxC"), sp->boxC.x, sp->boxC.y, sp->boxC.z);
    glUniform3f(U(pPart, "uBoxH"), sp->boxH.x, sp->boxH.y, sp->boxH.z);
    glUniform4fv(U(pPart, "uP"), sp->count * 4, sp->prim[0].f);
    glUniform1i(U(pPart, "uCount"), sp->count);
}

// ---- distant-mech cards --------------------------------------------------------------------------
static GLuint imp_tex(int w, int h, GLenum fmt) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexStorage2D(GL_TEXTURE_2D, IMP_LODS, fmt, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}
static void imp_attach(const ImpLayer *L, int lod) {
    glBindFramebuffer(GL_FRAMEBUFFER, L->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, L->col, lod);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, L->nrm, lod);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, L->depth, lod);
}

void render_imp_layout(int layer, int rows, float width, float y0, float height, float texel) {
    ImpLayer *L = &imps[layer];
    const int step = 1 << (IMP_LODS - 1); // card sizes are multiples of this so every smaller size lines up
    if (L->fbo) {
        glDeleteFramebuffers(1, &L->fbo);
        glDeleteTextures(1, &L->col);
        glDeleteTextures(1, &L->nrm);
        glDeleteTextures(1, &L->depth);
    }
    L->rows = rows;
    L->texel = texel;
    L->cw = ((int)ceilf(width / texel) + step - 1) / step * step;
    L->ch = ((int)ceilf(height / texel) + step - 1) / step * step;
    L->width = L->cw * texel;
    L->height = L->ch * texel;
    L->y0 = y0;
    int w = L->cw * IMP_YAWS, h = L->ch * rows;
    L->col = imp_tex(w, h, GL_RGBA8);
    L->nrm = imp_tex(w, h, GL_RGBA8);
    L->depth = imp_tex(w, h, GL_DEPTH_COMPONENT32F);
    glGenFramebuffers(1, &L->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, L->fbo);
    GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, bufs);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glClearColor(0, 0, 0, 0);
    for (int lod = 0; lod < IMP_LODS; lod++) {
        imp_attach(L, lod);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void render_imp_bake(int layer, int row, const Shape *const *shape, const xf *x, int n) {
    const ImpLayer *L = &imps[layer];
    glDisable(GL_BLEND);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texHeight);
    glUseProgram(pPart);
    glUniform1i(U(pPart, "uBake"), 1);
    glUniform1i(U(pPart, "uLook"), 0);
    glUniform1i(U(pPart, "uHeight"), 0);
    glUniform1i(U(pPart, "uDentN"), 0);
    glUniform1i(U(pPart, "uStage"), 0);
    glUniform1i(U(pPart, "uInterior"), 0);
    glUniform1f(U(pPart, "uFlash"), 0);
    glUniform1f(U(pPart, "uTexel"), 0.0625f);
    glUniform3f(U(pPart, "uTintA"), 1, 1, 1); // team colours are applied when the card is drawn
    glUniform3f(U(pPart, "uTintB"), 1, 1, 1);
    glBindVertexArray(vaoCube);
    for (int lod = 0; lod < IMP_LODS; lod++) {
        imp_attach(L, lod);
        // A traced mech gets slightly fatter with distance (its hit distance grows), which keeps thin pieces
        // visible. Each smaller sheet is drawn with a matching margin, so cards do the same.
        glUniform1f(U(pPart, "uBakeEps"), 0.6f * L->texel * (float)(1 << lod));
        for (int k = 0; k < IMP_YAWS; k++) {
            float a = k * 2 * PI / IMP_YAWS, s = sinf(a), c = cosf(a); // the viewer stands in direction (s, 0, c) from the mech
            m4 vp = {{0}};
            vp.m[0] = c * 2 / L->width;   // x: along the card
            vp.m[8] = -s * 2 / L->width;
            vp.m[5] = 2 / L->height;      // y: up
            vp.m[13] = -(L->y0 + L->height * 0.5f) * 2 / L->height;
            vp.m[2] = -s / L->width;      // z: away from the viewer, -0.5 .. 0.5 across the card's width
            vp.m[10] = -c / L->width;
            vp.m[15] = 1;
            glViewport((k * L->cw) >> lod, (row * L->ch) >> lod, L->cw >> lod, L->ch >> lod);
            for (int i = 0; i < n; i++) {
                v3 rd = qrot(qconj(x[i].q), V3(-s, 0, -c));
                part_shape(shape[i], x[i], vp);
                glUniform3f(U(pPart, "uBakeDir"), rd.x, rd.y, rd.z);
                glUniform1f(U(pPart, "uSeed"), (float)i);
                glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_BYTE, 0);
            }
        }
    }
    glUniform1i(U(pPart, "uBake"), 0);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void common(GLuint p, const Scene *s, float pixAngle) {
    glUseProgram(p);
    glUniform1f(U(p, "uTime"), s->time);
    glUniform1i(U(p, "uLook"), s->look);
    glUniform1f(U(p, "uPixAngle"), pixAngle);
    glUniform1i(U(p, "uHeight"), 0);
}


void render_view(const Scene *s, const View *v, GLuint dstTex, int w, int h, int linearize) {
    int div = s->look == 2 ? 3 : 1;
    int sw = w / div, sh = h / div;
    Target *t = target(sw, sh);
    m4 proj = m4proj(v->l, v->r, v->d, v->u, 0.05f, 4000.0f);
    m4 vp = m4mul(proj, m4xf(xfinv(v->cam)));
    float pixAngle = (v->r - v->l) / sw;
    v3 cam = v->cam.p;
    float camRot[9];
    rot3(v->cam.q, camRot);
    v3 fwd = qrot(v->cam.q, V3(0, 0, -1));

    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, sw, sh);
    glDisable(GL_BLEND);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texHeight);

    // sky
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);
    common(pSky, s, pixAngle);
    glUniform4f(U(pSky, "uFov"), v->l, v->r, v->d, v->u);
    glUniformMatrix3fv(U(pSky, "uCamRot"), 1, GL_FALSE, camRot);
    glBindVertexArray(vaoEmpty);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // ground
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    common(pTerrain, s, pixAngle);
    glUniformMatrix4fv(U(pTerrain, "uVP"), 1, GL_FALSE, vp.m);
    glUniform2f(U(pTerrain, "uOrigin"), floorf(cam.x / 4) * 4, floorf(cam.z / 4) * 4);
    glUniform3f(U(pTerrain, "uCamW"), cam.x, cam.y, cam.z);
    glUniform4fv(U(pTerrain, "uShadow"), s->nshadow, &s->shadow[0][0]);
    glUniform1i(U(pTerrain, "uShadowN"), s->nshadow);
    glBindVertexArray(vaoGrid);
    glDrawElements(GL_TRIANGLES, gridIndexCount, GL_UNSIGNED_INT, 0);

    // parts: draw the inside faces of each box so it also works with the camera inside the box
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);
    common(pPart, s, pixAngle);
    glUniform3f(U(pPart, "uCamW"), cam.x, cam.y, cam.z);
    glBindVertexArray(vaoCube);
    for (int i = 0; i < s->npart; i++) {
        const DrawPart *d = &s->part[i];
        const Shape *sp = d->shape;
        v3 c = vsub(d->world.p, cam);
        if (vdot(c, fwd) < -sp->radius) continue; // behind the viewer
        v3 ro = xfpt(xfinv(d->world), cam);
        part_shape(sp, d->world, vp);
        glUniform3f(U(pPart, "uRo"), ro.x, ro.y, ro.z);
        glUniform1i(U(pPart, "uDentN"), d->ndent);
        if (d->ndent) {
            glUniform4fv(U(pPart, "uDent"), d->ndent, &d->dent[0][0]);
            glUniform1fv(U(pPart, "uHeat"), d->ndent, d->heat);
        }
        glUniform1i(U(pPart, "uStage"), d->stage);
        glUniform3f(U(pPart, "uTintA"), d->tintA.x, d->tintA.y, d->tintA.z);
        glUniform3f(U(pPart, "uTintB"), d->tintB.x, d->tintB.y, d->tintB.z);
        glUniform1f(U(pPart, "uSeed"), d->seed);
        glUniform1i(U(pPart, "uInterior"), d->interior);
        glUniform1f(U(pPart, "uFlash"), d->flash);
        glUniform1f(U(pPart, "uTexel"), d->texel > 0 ? d->texel : 0.0625f);
        glUniform4fv(U(pPart, "uAux"), 1, d->aux);
        glUniform4fv(U(pPart, "uStat"), 3, d->stat);
        glUniform4fv(U(pPart, "uScr"), 1, d->scr);
        glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_BYTE, 0);
    }

    // distant mechs: one card for the legs and one for the torso, each showing the stored view nearest to this one
    glDisable(GL_CULL_FACE);
    if (s->nimp) {
        common(pImp, s, pixAngle);
        glUniformMatrix4fv(U(pImp, "uVP"), 1, GL_FALSE, vp.m);
        glUniform3f(U(pImp, "uCamW"), cam.x, cam.y, cam.z);
        glUniform1i(U(pImp, "uAtlasC"), 3);
        glUniform1i(U(pImp, "uAtlasN"), 4);
        glBindVertexArray(vaoEmpty);
        int bound = -1;
        for (int i = 0; i < s->nimp; i++) {
            const DrawImp *d = &s->imp[i];
            const ImpLayer *L = &imps[d->layer];
            v3 to = vsub(d->pos, s->head); // both eyes pick the same view, from the head position
            if (!L->fbo || vdot(vsub(d->pos, cam), fwd) < -L->width) continue;
            float len = sqrtf(to.x * to.x + to.z * to.z) + 1e-6f, cs = cosf(d->yaw), sn = sinf(d->yaw);
            float ang = atan2f(-to.x * cs + to.z * sn, -to.x * sn - to.z * cs); // direction to the viewer in the mech's frame
            int k = (int)floorf(ang / (2 * PI) * IMP_YAWS + 0.5f) & (IMP_YAWS - 1);
            if (bound != d->layer) {
                bound = d->layer;
                glActiveTexture(GL_TEXTURE3);
                glBindTexture(GL_TEXTURE_2D, L->col);
                glActiveTexture(GL_TEXTURE4);
                glBindTexture(GL_TEXTURE_2D, L->nrm);
                glUniform3f(U(pImp, "uRect"), L->width, L->y0, L->height);
                glUniform1f(U(pImp, "uWidth"), L->width);
            }
            glUniform3f(U(pImp, "uCenter"), d->pos.x, d->pos.y, d->pos.z);
            glUniform3f(U(pImp, "uRight"), -to.z / len, 0, to.x / len);
            glUniform4f(U(pImp, "uCell"), (float)(k * L->cw), (float)(d->row * L->ch), (float)L->cw, (float)L->ch);
            glUniform2f(U(pImp, "uCS"), cs, sn);
            glUniform3f(U(pImp, "uTintA"), d->tintA.x, d->tintA.y, d->tintA.z);
            glUniform3f(U(pImp, "uTintB"), d->tintB.x, d->tintB.y, d->tintB.z);
            glUniform1f(U(pImp, "uFlash"), d->flash);
            int lod = 0; // the sheet size whose pixels are nearest to one screen pixel at this distance
            for (float px = vlen(to) * pixAngle / L->texel; px >= 2 && lod < IMP_LODS - 1; px *= 0.5f) lod++;
            glUniform1i(U(pImp, "uLod"), lod);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        glActiveTexture(GL_TEXTURE0);
    }

    // particles
    if (s->nptc) {
        common(pPtc, s, pixAngle);
        glUniformMatrix4fv(U(pPtc, "uVP"), 1, GL_FALSE, vp.m);
        glUniform3f(U(pPtc, "uHead"), s->head.x, s->head.y, s->head.z);
        glUniform3f(U(pPtc, "uCamW"), cam.x, cam.y, cam.z);
        glBindVertexArray(vaoPtc);
        glBindBuffer(GL_ARRAY_BUFFER, vboPtc);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(DrawParticle) * s->nptc, s->ptc);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, s->nptc);
    }

    // outlines + vignette into the destination image
    glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dstTex, 0);
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST);
    common(pFinal, s, pixAngle);
    glUniform1i(U(pFinal, "uCol"), 1);
    glUniform1i(U(pFinal, "uNrm"), 2);
    glUniform2f(U(pFinal, "uSrcRes"), (float)sw, (float)sh);
    glUniform1f(U(pFinal, "uVignette"), s->vignette);
    glUniform1f(U(pFinal, "uFade"), s->fade);
    glUniform3f(U(pFinal, "uFadeCol"), s->fadeCol.x, s->fadeCol.y, s->fadeCol.z);
    glUniform1f(U(pFinal, "uLinearize"), (float)linearize);
    glUniform1f(U(pFinal, "uEdge"), div == 1 ? 1.25f : 1.0f);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, t->col);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, t->nrm);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(vaoEmpty);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}
