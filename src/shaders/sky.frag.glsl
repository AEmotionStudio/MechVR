// Sky: banded gradient, sun and flat stylized clouds.
in vec2 vUV;
layout(location = 0) out vec4 oCol;
layout(location = 1) out vec4 oNrm;
uniform vec4 uFov;    // tangents: left, right, down, up
uniform mat3 uCamRot;
void main() {
    vec3 d = normalize(uCamRot * vec3(mix(uFov.x, uFov.y, vUV.x), mix(uFov.z, uFov.w, vUV.y), -1.0));
    vec3 c = skyBase(d);
    float s = dot(d, SUN);
    c = mix(c, vec3(1.0, 0.86, 0.62), 0.35 * smoothstep(0.90, 0.985, s));
    c = mix(c, vec3(1.0, 0.97, 0.86), step(0.9965, s));
    if (d.y > 0.015) { // clouds on a flat layer overhead, drifting slowly
        vec2 q = d.xz / (d.y + 0.08) * 1.6 + vec2(uTime * 0.004, 0.0);
        float n = vnoise2(q) * 0.6 + vnoise2(q * 2.3 + 7.0) * 0.3 + vnoise2(q * 5.1) * 0.1;
        float fade = smoothstep(0.015, 0.12, d.y);
        vec3 cl = mix(vec3(0.93, 0.62, 0.52), vec3(1.0, 0.92, 0.80), step(0.60, n + 0.12 * s));
        c = mix(c, cl, step(0.54, n) * fade * 0.92);
    }
    oCol = vec4(c, 1.0);
    oNrm = vec4(0.0, 0.0, 0.0, 1e5);
}
