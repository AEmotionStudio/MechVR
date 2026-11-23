// Shared by every shader: noise, sky colors, haze and the banded "toon" lighting.
uniform float uTime;
uniform int uLook;       // 0 smooth toon, 1 surface texels (pixel-art look), 2 chunky low-res
uniform float uPixAngle; // approximate angle covered by one rendered pixel
uniform sampler2D uHeight; // terrain heights, one value every 4 m, wrapping

// Terrain height at a world position (exactly the same blend the game code uses for walking).
uniform vec4 uBulge;     // a mound pushed up by something moving under the sand: x, z, height, radius
float H(vec2 xz) {
    vec2 g = xz / 4.0, i = floor(g), f = g - i;
    ivec2 a = ivec2(i) & 511, b = (a + 1) & 511;
    float up = 0.0;
    if (uBulge.z > 0.0) { vec2 bd = (xz - uBulge.xy) / max(uBulge.w, 1.0); up = uBulge.z * exp(-dot(bd, bd)); }
    return up + mix(mix(texelFetch(uHeight, a, 0).r, texelFetch(uHeight, ivec2(b.x, a.y), 0).r, f.x),
               mix(texelFetch(uHeight, ivec2(a.x, b.y), 0).r, texelFetch(uHeight, b, 0).r, f.x), f.y);
}

const vec3 SUN = vec3(-0.7986, 0.5111, 0.3194);
const vec3 INK = vec3(0.09, 0.07, 0.13);

float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}
float hash12(vec2 p) {
    vec3 q = fract(vec3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}
float vnoise(vec3 p) {
    vec3 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash13(i), hash13(i + vec3(1, 0, 0)), f.x),
                   mix(hash13(i + vec3(0, 1, 0)), hash13(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(hash13(i + vec3(0, 0, 1)), hash13(i + vec3(1, 0, 1)), f.x),
                   mix(hash13(i + vec3(0, 1, 1)), hash13(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}
float vnoise2(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash12(i), hash12(i + vec2(1, 0)), f.x), mix(hash12(i + vec2(0, 1)), hash12(i + vec2(1, 1)), f.x), f.y);
}

vec3 skyBase(vec3 d) {
    float h = clamp(d.y, -0.2, 1.0);
    vec3 hor = vec3(0.99, 0.76, 0.55), mid = vec3(0.52, 0.68, 0.76), zen = vec3(0.17, 0.33, 0.56);
    vec3 c = mix(hor, mid, smoothstep(0.0, 0.28, h));
    return mix(c, zen, smoothstep(0.22, 0.85, h));
}

// Distance haze toward the horizon color.
vec3 haze(vec3 c, float dist, vec3 dir) {
    float f = 1.0 - exp(-dist * 0.0016);
    return mix(c, skyBase(vec3(dir.x, max(dir.y, 0.0) * 0.3, dir.z)), f * 0.92);
}

// Banded lighting: deep shade, shade, lit, highlight. Shadows go cool, light goes warm.
// l = sun facing (-1..1), ao = 0..1, spec = 0/1 highlight, d = per-texel offset for the texel look.
vec3 toon(vec3 base, float l, float ao, float spec, float d) {
    float v = (l * 0.5 + 0.5) * ao + d;
    float w = uLook == 1 ? 0.001 : 0.012;
    float b1 = smoothstep(0.30 - w, 0.30 + w, v);
    float b2 = smoothstep(0.53 - w, 0.53 + w, v);
    float b3 = max(smoothstep(0.88 - w, 0.88 + w, v), spec);
    vec3 deep = base * vec3(0.30, 0.28, 0.46);
    vec3 shade = base * vec3(0.56, 0.54, 0.74);
    vec3 lit = base * vec3(1.0, 0.96, 0.88);
    vec3 hi = min(base * 1.15 + vec3(0.10, 0.09, 0.05), vec3(1.0));
    vec3 c = mix(deep, shade, b1);
    c = mix(c, lit, b2);
    return mix(c, hi, b3);
}

// A unit direction packed into two numbers (octahedral mapping), for the sheets of distant-mech cards.
vec2 octEnc(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec2 e = n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return e * 0.5 + 0.5;
}
vec3 octDec(vec2 f) {
    f = f * 2.0 - 1.0;
    vec3 n = vec3(f, 1.0 - abs(f.x) - abs(f.y));
    float t = max(-n.z, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}
