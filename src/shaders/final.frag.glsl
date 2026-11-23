// Last step: ink outlines, comfort vignette, fades.
in vec2 vUV;
layout(location = 0) out vec4 oCol;
uniform sampler2D uCol, uNrm;
uniform vec2 uSrcRes;
uniform float uVignette, uFade, uLinearize, uEdge;
uniform vec3 uFadeCol;
void main() {
    vec2 px = uEdge / uSrcRes;
    vec3 c = texture(uCol, vUV).rgb;
    vec4 g = texture(uNrm, vUV);
    vec4 a = texture(uNrm, vUV + vec2(px.x, 0)), b = texture(uNrm, vUV - vec2(px.x, 0));
    vec4 u = texture(uNrm, vUV + vec2(0, px.y)), d = texture(uNrm, vUV - vec2(0, px.y));
    // Depth edges use the second difference, so flat ground at a shallow angle makes no false lines.
    float glow = min(min(a.w, b.w), min(g.w, min(u.w, d.w))); // negative when a glowing thing is here
    a.w = abs(a.w); b.w = abs(b.w); u.w = abs(u.w); d.w = abs(d.w); g.w = abs(g.w);
    float de = max(abs(a.w + b.w - 2.0 * g.w), abs(u.w + d.w - 2.0 * g.w)) / g.w;
    float ne = min(min(dot(g.xyz, a.xyz), dot(g.xyz, b.xyz)), min(dot(g.xyz, u.xyz), dot(g.xyz, d.xyz)));
    float edge = max(smoothstep(0.012, 0.03, de), g.w < 1e4 ? smoothstep(0.55, 0.35, ne) : 0.0);
    edge *= 1.0 - smoothstep(50.0, 180.0, min(min(a.w, b.w), min(g.w, min(u.w, d.w))));
    c = mix(c, INK, edge * 0.88 * step(0.0, glow));
    vec2 v = vUV * 2.0 - 1.0;
    c *= 1.0 - uVignette * smoothstep(0.35, 1.05, length(v));
    c = mix(c, uFadeCol, uFade);
    if (uLinearize > 0.5) c = pow(c, vec3(2.2));
    oCol = vec4(c, 1.0);
}
