// Ground shading: sand with contour bands, layered rock on slopes, shadows from mechs and rocks.
in vec3 vW;
layout(location = 0) out vec4 oCol;
layout(location = 1) out vec4 oNrm;
uniform vec3 uCamW;
uniform vec4 uShadow[48]; // xyz center, w radius
uniform int uShadowN;
void main() {
    float dist = length(vW - uCamW);
    float texel = 0.25;
    bool tex = uLook == 1 && dist * uPixAngle < texel * 0.55;
    vec2 xz = tex ? (floor(vW.xz / texel) + 0.5) * texel : vW.xz;
    float h = H(xz);
    vec3 n = normalize(vec3(H(xz - vec2(1.5, 0)) - H(xz + vec2(1.5, 0)), 3.0, H(xz - vec2(0, 1.5)) - H(xz + vec2(0, 1.5))));
    float dither = tex ? (hash12(floor(vW.xz / texel)) - 0.5) : 0.0;

    float big = vnoise2(xz * 0.012), fine = vnoise2(xz * 0.9);
    vec3 sand = mix(vec3(0.86, 0.66, 0.44), vec3(0.78, 0.55, 0.38), smoothstep(0.35, 0.65, big + dither * 0.15));
    float band = fract(h * 0.9 + big * 2.0 + dither * 0.06);        // contour bands: they read as motion cues when walking
    sand *= 1.0 - 0.07 * step(0.5, band) - 0.10 * step(0.94, band);
    sand *= 0.96 + 0.08 * step(0.5, fine + dither * 0.5);
    float strata = floor(h / 1.6 + 0.7 * vnoise2(xz * 0.05));
    vec3 rock = mix(vec3(0.60, 0.34, 0.26), vec3(0.76, 0.48, 0.32), mod(strata, 2.0));
    rock = mix(rock, vec3(0.50, 0.27, 0.24), step(2.5, mod(strata, 4.0)) * 0.6);
    float steep = smoothstep(0.80, 0.70, n.y + dither * 0.05);
    vec3 base = mix(sand, rock, steep);

    float lit = dot(n, SUN);
    float sh = 1.0; // shadows: each caster is approximated by a sphere
    for (int i = 0; i < uShadowN; i++) {
        vec3 oc = uShadow[i].xyz - vW;
        float along = dot(oc, SUN);
        if (along > 0.0) {
            float d = length(oc - SUN * along);
            sh = min(sh, smoothstep(uShadow[i].w * 0.82, uShadow[i].w * 1.02, d + dither * 0.1));
        }
    }
    float l = min(lit, mix(-0.25, 1.0, sh));
    vec3 col = toon(base, l, 1.0, 0.0, dither * 0.08);
    vec3 dir = (vW - uCamW) / max(dist, 1e-3);
    oCol = vec4(haze(col, dist, dir), 1.0);
    oNrm = vec4(n, dist);
}
