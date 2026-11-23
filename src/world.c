#include "world.h"

float world_h[HN * HN];
Rock rocks[NROCK];

static unsigned rngState = 0x1234567u;
float frand(void) {
    rngState ^= rngState << 13;
    rngState ^= rngState >> 17;
    rngState ^= rngState << 5;
    return (rngState & 0xffffff) / 16777216.0f;
}
float frands(void) { return frand() * 2 - 1; }

static unsigned hashi(int x, int y, unsigned s) {
    unsigned h = (unsigned)x * 374761393u + (unsigned)y * 668265263u + s * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
// Value noise on a lattice that repeats every `per` cells, so the world wraps without a seam.
static float pnoise(float x, float y, int per, unsigned s) {
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float fx = x - xi, fy = y - yi;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    int x0 = ((xi % per) + per) % per, y0 = ((yi % per) + per) % per, x1 = (x0 + 1) % per, y1 = (y0 + 1) % per;
#define L(a, b) ((hashi(a, b, s) & 0xffff) / 65535.0f)
    return mixf(mixf(L(x0, y0), L(x1, y0), fx), mixf(L(x0, y1), L(x1, y1), fx), fy);
#undef L
}
static float fbm(float u, float v, int per, int oct, unsigned s) {
    float a = 0.5f, sum = 0, norm = 0;
    for (int o = 0; o < oct; o++) {
        sum += a * pnoise(u * per, v * per, per, s + o * 17);
        norm += a;
        per *= 2;
        a *= 0.5f;
    }
    return sum / norm;
}

void world_init(void) {
    const float size = HN * CELL;
    for (int j = 0; j < HN; j++)
        for (int i = 0; i < HN; i++) {
            float u = (float)i / HN, v = (float)j / HN;
            float x = i * CELL, z = j * CELL;
            if (x > size / 2) x -= size;
            if (z > size / 2) z -= size;
            float dist = sqrtf(x * x + z * z);
            float dunes = 11.0f * (fbm(u, v, 8, 4, 1) - 0.5f) + 1.6f * (pnoise(u * 64, v * 64, 64, 9) - 0.5f);
            float m = smooth01((fbm(u, v, 6, 3, 40) - 0.53f) / 0.11f);
            m = (floorf(m * 2.0f) + smooth01((m * 2.0f - floorf(m * 2.0f) - 0.55f) / 0.45f)) / 2.0f; // two terraces
            float mesa = m * (24.0f + 16.0f * pnoise(u * 16, v * 16, 16, 77));
            float open = smooth01((dist - 60) / 150.0f); // flatter ground around the start
            world_h[j * HN + i] = dunes * (0.22f + 0.78f * open) + mesa * smooth01((dist - 150) / 120.0f);
        }
    for (int i = 0; i < NROCK; i++) {
        float a = frand() * 2 * PI, d = 28 + 260 * frand();
        Rock *r = &rocks[i];
        r->shape = i % 3;
        r->radius = r->shape == 0 ? 2.4f : r->shape == 1 ? 1.5f : 2.6f;
        r->yaw = frand() * 2 * PI;
        r->pos = V3(cosf(a) * d, 0, sinf(a) * d);
        r->pos.y = world_height(r->pos.x, r->pos.z) - 0.25f;
    }
}

float world_height(float x, float z) {
    float gx = x / CELL, gz = z / CELL;
    float ix = floorf(gx), iz = floorf(gz), fx = gx - ix, fz = gz - iz;
    int a = (int)ix & (HN - 1), b = (int)iz & (HN - 1), a1 = (a + 1) & (HN - 1), b1 = (b + 1) & (HN - 1);
    return mixf(mixf(world_h[b * HN + a], world_h[b * HN + a1], fx), mixf(world_h[b1 * HN + a], world_h[b1 * HN + a1], fx), fz);
}
