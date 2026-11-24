// Traces one part: a short list of smooth shapes (boxes, cylinders, capsules, spheres) plus damage.
#define MAXP 16
in vec3 vLocal;
layout(location = 0) out vec4 oCol;
layout(location = 1) out vec4 oNrm; // world normal, view distance

uniform mat4 uMVP;
uniform mat3 uRot;       // part -> world rotation
uniform vec3 uRo;        // camera position in part space
uniform vec3 uCamW;      // camera position in world space
uniform vec3 uOriginW;   // part origin in world space
// This shader is built twice. The plain build draws almost everything. The EXTRA build adds cone pieces and
// shapes drawn enlarged (the sand worm); it is kept apart because those two features slow every traced pixel.
#ifdef EXTRA
uniform float uScale;    // the whole shape drawn this many times larger (uRo and uMVP already allow for it)
uniform float uFlip;     // -1 = the shape drawn mirrored left to right (an arm made for the other side), otherwise 1
#define FLIP(v) ((v) * vec3(uFlip, 1.0, 1.0))
#else
const float uScale = 1.0;
#define FLIP(v) (v)
#endif
uniform int uBio;        // 1 = a living part: holes show flesh and bone instead of a metal frame
uniform vec3 uBoxC, uBoxH;
uniform vec4 uP[MAXP * 4];
uniform int uCount;
uniform vec4 uDent[8];   // xyz = position in part space, w = radius
uniform float uHeat[8];  // 1 = just hit, cools to 0
uniform int uDentN;
uniform int uStage;      // 0 intact, 1 damaged, 2 critical, 3 destroyed
uniform vec3 uTintA, uTintB;
uniform float uSeed, uFlash;
uniform vec4 uAux;       // cockpit screen: x speed, y layout (0 cockpit, 1 wrist), z alarm, w power. The wrist: x the part under the torch, w your health
uniform vec4 uStat[3];   // cockpit screen: damage stage of each mech part; [2].z the wave, [2].w enemies left (or minus the seconds to the next wave)
uniform int uInterior;   // 1 = cockpit part: lit from the windshield, no haze
uniform float uTexel;    // size of one surface cell for the texel look
uniform vec4 uScr;       // cockpit screen: center xy and 1/size in part space
uniform int uBake;       // 1 = drawing a view for the sheet of distant-mech cards: parallel rays, unlit output
uniform vec3 uBakeDir;   // ray direction in part space while baking
uniform float uBakeEps;  // while baking: how close counts as a hit. Coarser sheets use more, so thin pieces do not vanish

vec3 qrotInv(vec4 q, vec3 v) {
    vec3 t = 2.0 * cross(-q.xyz, v);
    return v + q.w * t + cross(-q.xyz, t);
}

float prim(int i, vec3 p) {
    vec4 a = uP[i * 4], b = uP[i * 4 + 1], q = uP[i * 4 + 2];
    vec3 l = p - a.xyz;
    if (q.w < 0.99999) l = qrotInv(q, l);
    int ty = int(a.w);
    if (ty == 0) { // rounded box
        vec3 d = abs(l) - (b.xyz - b.w);
        return length(max(d, 0.0)) + min(max(d.x, max(d.y, d.z)), 0.0) - b.w;
    }
    if (ty == 1) { // rounded cylinder along Y: radius b.x, half height b.y
        vec2 d = abs(vec2(length(l.xz), l.y)) - (b.xy - b.w);
        return min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - b.w;
    }
    if (ty == 2) return length(l) - b.x; // sphere
#ifdef EXTRA
    if (ty == 4) { // round cone along Y: radius b.x at the bottom end, b.z at the top end, half length b.y.
        vec2 k = vec2(uP[i * 4 + 3].w, b.w), q = vec2(length(l.xz), l.y + b.y); // k: slope terms worked out when the shape is loaded
        float along = dot(q, vec2(-k.x, k.y)), h = 2.0 * b.y;
        if (along < 0.0) return length(q) - b.x;
        if (along > k.y * h) return length(q - vec2(0.0, h)) - b.z;
        return dot(q, vec2(k.y, k.x)) - b.x;
    }
#endif
    l.y -= clamp(l.y, -b.y, b.y);        // capsule along Y
    return length(l) - b.x;
}

float gMat;
float map(vec3 p) {
    float d = 1e9;
    gMat = 0.0;
    for (int i = 0; i < uCount; i++) {
        float di = prim(i, p);
        vec4 c = uP[i * 4 + 3];
        int op = int(c.x);
        if (op == 0) { if (di < d) { d = di; gMat = c.z; } }
        else if (op == 1) { if (-di > d) { d = -di; gMat = c.z; } }
        else if (op == 2) {
            float h = clamp(0.5 + 0.5 * (d - di) / c.y, 0.0, 1.0);
            d = mix(d, di, h) - c.y * h * (1.0 - h);
            if (h > 0.5) gMat = c.z;
        } else { if (di > d) { d = di; gMat = c.z; } }
    }
    for (int j = 0; j < uDentN; j++) { // battle damage: ragged holes carved where shots landed
        vec3 q = p - uDent[j].xyz;
        float r = uDent[j].w, l2 = dot(q, q);
        if (l2 < r * r * 2.6) {
            float dd = sqrt(l2) - r * (0.84 + 0.16 * sin(q.x * 23.0 + uSeed) * sin(q.y * 19.0 + float(j)) * sin(q.z * 29.0));
            if (-dd > d) { d = -dd; gMat = 20.0 + float(j); }
        }
    }
    return d;
}

vec3 normalAt(vec3 p) {
    const vec2 k = vec2(1, -1);
    const float e = 0.004;
    return normalize(k.xyy * map(p + k.xyy * e) + k.yyx * map(p + k.yyx * e) +
                     k.yxy * map(p + k.yxy * e) + k.xxx * map(p + k.xxx * e));
}

// 3 x 5 pixel digits, one bit per pixel, top row first
const int DIGIT[10] = int[10](31599, 11415, 29671, 29391, 23497, 31183, 31215, 29257, 31727, 31695);
// 1 where the number n (0..99) is lit: two digits whose lower left is cell o, each font pixel s cells across
float number(vec2 g, vec2 o, float s, float n) {
    vec2 q = floor((g - o) / s);
    if (q.x < 0.0 || q.x > 6.0 || q.y < 0.0 || q.y > 4.0 || q.x == 3.0) return 0.0;
    int v = int(clamp(n, 0.0, 99.0) + 0.5), tens = q.x < 3.0 ? 1 : 0;
    if (tens == 1 && v < 10) return 0.0;
    int d = tens == 1 ? v / 10 : v % 10, x = tens == 1 ? int(q.x) : int(q.x) - 4;
    return float((DIGIT[d] >> (int(q.y) * 3 + 2 - x)) & 1); // q.y counts up from the bottom row, the bits from the top
}

// Cockpit display: a little damage diagram of the mech, a speed bar and warning text blocks, and in the wave game the
// wave and how many are left. The wrist screen uses the same picture elements: your health, the torch's part, the wave.
vec3 screenColor(vec2 uv) {
    vec3 bg = vec3(0.03, 0.10, 0.09), on = vec3(0.35, 1.0, 0.65);
    vec2 g = floor(uv * vec2(48.0, 36.0));
    vec2 c = (g + 0.5) / vec2(48.0, 36.0);
    vec3 col = bg + 0.02 * mod(g.y, 2.0);
    float wave = uStat[2].z, left = uStat[2].w;
    vec3 countCol = left > 0.0 ? vec3(1.0, 0.35, 0.2) : vec3(1.0, 0.75, 0.2) * (left > -3.5 && fract(uTime * 2.0) > 0.5 ? 0.35 : 1.0);
    if (uAux.y > 0.5) { // the wrist
        if (c.x > 0.05 && c.x < 0.12 && c.y > 0.1 && c.y < 0.9) // your health
            col = (c.y - 0.1) / 0.8 < uAux.w ? (uAux.w < 0.3 ? vec3(1.0, 0.3, 0.2) : vec3(0.4, 0.8, 1.0)) : bg * 2.0;
        if (uAux.x >= 0.0 && c.x > 0.88 && c.x < 0.95 && c.y > 0.1 && c.y < 0.9) // the part under the repair torch
            col = (c.y - 0.1) / 0.8 < uAux.x ? on : bg * 2.0;
        if (wave > 0.5) {
            if (number(g, vec2(17.0, 20.0), 2.0, wave) > 0.5) col = on;
            if (number(g, vec2(17.0, 5.0), 2.0, abs(left)) > 0.5) col = countCol;
        }
        return col;
    }
    // part boxes: xy center, zw half size, in screen space
    const vec4 R[10] = vec4[10](vec4(.50, .50, .07, .05), vec4(.50, .68, .11, .11), vec4(.33, .68, .04, .10), vec4(.67, .68, .04, .10),
                                vec4(.43, .38, .035, .07), vec4(.42, .24, .03, .07), vec4(.41, .13, .05, .025),
                                vec4(.57, .38, .035, .07), vec4(.58, .24, .03, .07), vec4(.59, .13, .05, .025));
    for (int i = 0; i < 10; i++) {
        vec2 d = abs(c - R[i].xy) - R[i].zw;
        if (max(d.x, d.y) < 0.0) {
            float st = uStat[i / 4][i % 4];
            vec3 pc = st < 0.5 ? on : st < 1.5 ? vec3(1.0, 0.9, 0.2) : st < 2.5 ? vec3(1.0, 0.45, 0.1) : vec3(0.45, 0.05, 0.05);
            if (st > 1.5 && st < 2.5 && fract(uTime * 2.0) < 0.5) pc *= 0.5;
            col = pc;
        }
    }
    if (c.x > 0.88 && c.x < 0.95 && c.y > 0.1 && c.y < 0.9) // speed bar
        col = (c.y - 0.1) / 0.8 < abs(uAux.x) ? on : bg * 2.0;
    if (c.x > 0.05 && c.x < 0.12 && c.y > 0.1 && c.y < 0.9) // power bar
        col = (c.y - 0.1) / 0.8 < uAux.w ? vec3(0.4, 0.8, 1.0) : bg * 2.0;
    if (uAux.z > 0.5 && c.y > 0.86 && c.x > 0.2 && c.x < 0.8 && fract(uTime * 3.0) < 0.6) col = vec3(1.0, 0.2, 0.1);
    if (wave > 0.5) { // the wave on the left, how many are left (red) or the seconds to the next (amber) on the right
        if (number(g, vec2(8.0, 29.0), 1.0, wave) > 0.5) col = on;
        if (number(g, vec2(33.0, 29.0), 1.0, abs(left)) > 0.5) col = countCol;
    }
    float glitch = step(0.985 - uAux.z * 0.25, hash12(vec2(g.y, floor(uTime * 20.0))));
    return mix(col, col.gbr * 1.5, glitch * step(0.3, uAux.z));
}

void main() {
    vec3 ro = uRo, rd = normalize(vLocal - uRo);
    if (uBake == 1) { rd = uBakeDir; ro = vLocal - rd * (2.0 * length(uBoxH) + 0.1); }
    rd += step(abs(rd), vec3(1e-6)) * 1e-6;
    vec3 inv = 1.0 / rd, t0 = (uBoxC - uBoxH - ro) * inv, t1 = (uBoxC + uBoxH - ro) * inv;
    vec3 tmin = min(t0, t1), tmax = max(t0, t1);
    float t = max(max(tmin.x, max(tmin.y, tmin.z)), 0.0), tf = min(tmax.x, min(tmax.y, tmax.z));
    if (tf <= t) discard;
    bool hit = false;
    for (int i = 0; i < 80; i++) {
        float d = map(ro + rd * t);
        if (d < (uBake == 1 ? uBakeEps : 0.0006 * t + 0.0004)) { hit = true; break; }
        t += d * 0.92;
        if (t > tf) break;
    }
    if (!hit) discard;

    vec3 p = ro + rd * t;
    map(p);
    int mi = int(gMat + 0.5);

    // Texel look: light whole surface cells at once, so the surface reads as pixel art.
    float texel = uTexel, panel = uTexel * 12.0;
    bool tex = uLook == 1 && t * uPixAngle < texel * 0.55;
    vec3 ps = tex ? (floor(p / texel) + 0.5) * texel : p;
    vec3 n = normalAt(tex ? ps : p);
    float ao = clamp(1.0 - 1.6 * clamp(0.16 - map(p + n * 0.16), 0.0, 1.0) - 0.5 * clamp(0.45 - map(p + n * 0.45), 0.0, 1.0), 0.35, 1.0);

    vec3 nw = uRot * FLIP(n);
    vec3 pw = uOriginW + uRot * FLIP(p) * uScale;
    vec3 vw = normalize(uCamW - pw);
    float dither = tex ? (hash13(floor(p / texel) + uSeed) - 0.5) * (uInterior == 1 ? 0.03 : 0.09) : 0.0;

    vec3 base;
    float spec = 0.0, emis = 0.0, specPow = 60.0, specOn = 0.0;
    if (mi == 0 || mi == 1 || mi == 12) { // armor plates and cabin walls, with panel seams and slight per-panel variation
        base = mi == 0 ? uTintA : mi == 1 ? uTintB : vec3(0.46, 0.50, 0.52);
        if (mi == 12) panel *= 2.0;
        float lw = tex ? texel : 0.02;
        vec3 f = fract(ps / panel) * panel, an = abs(n);
        float seam = 0.0;
        if (an.x < 0.75 && f.x < lw) seam = 1.0;
        if (an.y < 0.75 && f.y < lw) seam = 1.0;
        if (an.z < 0.75 && f.z < lw) seam = 1.0;
        base *= 0.94 + 0.12 * hash13(floor(ps / panel) + uSeed);
        base *= 1.0 - 0.38 * seam;
        specOn = mi == 12 ? 0.0 : 1.0;
    } else if (mi == 2) { base = vec3(0.19, 0.20, 0.25); specOn = 1.0; specPow = 30.0; }  // joints and frame
    else if (mi == 3) { base = vec3(0.95, 0.44, 0.10); }                                   // accent paint
    else if (mi == 4) { base = vec3(0.13, 0.36, 0.45); specOn = 1.0; specPow = 16.0; }     // canopy glass
    else if (mi == 5) { base = vec3(1.0, 0.80, 0.35); emis = 1.0; }                        // glowing
    else if (mi == 6) { base = vec3(0.05); }                                               // screen (filled in below)
    else if (mi == 7) {                                                                    // rock
        float s = floor(pw.y / 0.9 + 0.6 * vnoise(pw * 0.35));
        base = mix(vec3(0.62, 0.38, 0.28), vec3(0.74, 0.50, 0.34), mod(s, 2.0));
        base *= 0.9 + 0.2 * vnoise(ps * 1.7);
    } else if (mi == 8) {                                                                  // hazard stripes
        base = mix(vec3(0.98, 0.80, 0.10), vec3(0.10, 0.09, 0.10), step(0.5, fract((ps.x + ps.y + ps.z) * 14.0)));
    } else if (mi == 9) { base = vec3(0.22, 0.20, 0.21); }                                 // seat, rubber
    else if (mi == 10) { base = vec3(0.80, 0.74, 0.62); }                                  // glove
    else if (mi == 11) { base = vec3(0.35, 0.95, 1.0); emis = 1.0; }                       // plasma bolt
    else if (mi == 13) {                                                                   // flesh: dark, wet, veined
        base = vec3(0.60, 0.20, 0.24) * (0.80 + 0.40 * step(0.56, vnoise(ps * 2.6 + uSeed)));
        specOn = 1.0; specPow = 18.0;
    } else if (mi == 14) { base = vec3(0.86, 0.80, 0.66) * (0.93 + 0.14 * vnoise(ps * 5.0)); } // bone
    else if (mi == 15) {                                                                   // glowing sac, slowly pulsing
        base = vec3(0.55, 0.95, 0.35) * (0.72 + 0.28 * sin(uTime * 2.6 + uSeed * 3.0));
        emis = 1.0;
    } else if (uBio == 1) {                                                                // a wound: meat with pale ribs
        float ribs = step(0.62, fract(ps.y * 3.0 + ps.x * 1.2 + ps.z));
        base = mix(vec3(0.46, 0.07, 0.09), vec3(0.82, 0.74, 0.60), ribs);
    } else {                                                                               // exposed frame inside a hole
        float ribs = step(0.5, fract(ps.y * 4.0 + ps.x * 1.5 + ps.z));
        base = mix(vec3(0.09, 0.08, 0.10), vec3(0.21, 0.19, 0.21), ribs);
    }

    // Scorching around holes, and a hot glow on fresh ones.
    float hot = 0.0;
    for (int j = 0; j < uDentN; j++) {
        float r = uDent[j].w, dj = length(ps - uDent[j].xyz);
        float edge = dj + (vnoise(ps * 9.0 + float(j)) - 0.5) * r * 0.7;
        float s = 1.0 - smoothstep(r * 1.05, r * 2.1, edge);
        if (mi < 20) base = mix(base, vec3(0.13, 0.11, 0.12), s * 0.85);
        hot = max(hot, uHeat[j] * smoothstep(r * 0.45, r * 0.95, dj) * (1.0 - smoothstep(r * 1.0, r * 1.3, dj)) * (mi >= 20 ? 1.0 : 0.7));
    }
    if (uStage >= 2 && mi < 2) base *= 0.82;

    if (uBake == 1) { // colour, kind of surface, normal and crease shading; imp.frag.glsl lights it later
        int id = emis > 0.5 ? 6 : mi == 0 ? 2 : mi == 1 ? 3 : mi == 2 ? 4 : mi == 4 ? 5 : 1;
        vec4 bc = uMVP * vec4(p, 1.0);
        oCol = vec4(base, float(id) / 7.0);
        oNrm = vec4(octEnc(nw), bc.z + 0.5, ao); // normal, depth through the card (0..1 across its width), crease shading
        gl_FragDepth = bc.z / bc.w * 0.5 + 0.5;
        return;
    }
    if (specOn > 0.5) spec = step(0.5, pow(max(dot(nw, normalize(SUN + vw)), 0.0), specPow));
    vec3 lightDir = uInterior == 1 ? uRot * vec3(0.0, 0.55, -0.835) : SUN;
    if (uInterior == 1) spec = 0.0;
    // cabin parts get soft, mostly even light so every surface stays readable
    vec3 col = toon(base, uInterior == 1 ? 0.26 + 0.62 * dot(nw, lightDir) : dot(nw, lightDir), ao, spec, dither);
    if (mi == 4) col += vec3(0.20, 0.30, 0.32) * step(0.72, 1.0 - max(dot(nw, vw), 0.0)); // glass rim sheen
    if (mi == 6) { col = screenColor((p.xy - uScr.xy) * uScr.zw + 0.5); }
    if (emis > 0.5) col = base;
    if (hot > 0.02) {
        float hb = floor(clamp(hot, 0.0, 0.999) * 4.0) / 3.0;
        col = mix(col, mix(vec3(0.75, 0.12, 0.04), vec3(1.0, 0.86, 0.35), hb), smoothstep(0.02, 0.2, hot));
    }
    col = mix(col, vec3(1.0, 0.96, 0.82), uFlash * 0.55);

    float dist = length(pw - uCamW);
    oCol = vec4(uInterior == 1 ? col : haze(col, dist, -vw), 1.0);
    oNrm = vec4(nw, emis > 0.5 ? -dist : dist); // negative distance = no ink outline
    vec4 clip = uMVP * vec4(p, 1.0);
    gl_FragDepth = clip.z / clip.w * 0.5 + 0.5;
}
