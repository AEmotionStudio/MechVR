in vec2 vUV;
in vec4 vData;
in vec3 vW;
in vec3 vFace;
layout(location = 0) out vec4 oCol;
layout(location = 1) out vec4 oNrm;
uniform vec3 uCamW;
void main() {
    float age = vData.x, seed = vData.z, r = length(vUV);
    int type = int(vData.y + 0.5);
    float n = vnoise(vec3(vUV * 2.2 + seed * 17.0, age * 1.5 + seed));
    vec3 col;
    if (type == 0) { // smoke puff: solid cel-shaded blob that erodes as it ages
        float shape = 1.0 - r - n * 0.55 - age * age * 0.75;
        if (shape < 0.0) discard;
        float up = vUV.y * 0.5 + 0.5 + (n - 0.5) * 0.5;
        col = mix(vec3(0.17, 0.15, 0.19), vec3(0.34, 0.31, 0.36), step(0.55, up));
        col = mix(col, vec3(0.52, 0.48, 0.50), step(0.92, up) * step(0.2, shape));
        col = mix(vec3(0.95, 0.45, 0.12), col, smoothstep(0.0, 0.12, age)); // born glowing
    } else if (type == 1) { // fire
        float shape = 1.0 - r * (1.0 + age) - n * 0.6 - age * 0.5;
        if (shape < 0.0) discard;
        col = shape > 0.42 ? vec3(1.0, 0.95, 0.60) : shape > 0.18 ? vec3(1.0, 0.62, 0.12) : vec3(0.86, 0.22, 0.06);
    } else if (type == 2) { // spark
        if (r > 1.0 - age * 0.8) discard;
        col = mix(vec3(1.0, 0.95, 0.7), vec3(1.0, 0.5, 0.1), age);
    } else if (type == 3) { // flash: a spiky star
        float a = atan(vUV.y, vUV.x);
        float star = 0.45 + 0.55 * pow(abs(cos(a * 3.0 + seed * 6.0)), 3.0);
        if (r > star * (1.0 - age)) discard;
        col = r < 0.35 * (1.0 - age) ? vec3(1.0, 1.0, 0.9) : vec3(1.0, 0.75, 0.25);
    } else if (type == 5) { // aiming reticle: ring and center dot
        if (!(abs(r - 0.72) < 0.1 || r < 0.14 || (abs(vUV.x) < 0.05 && abs(vUV.y) > 0.5) || (abs(vUV.y) < 0.05 && abs(vUV.x) > 0.5)) || r > 1.0) discard;
        col = vec3(0.45, 1.0, 0.85);
    } else if (type == 6) { // ichor: what a living part sheds when it is hit
        float shape = 1.0 - r - n * 0.5 - age * 0.9;
        if (shape < 0.0) discard;
        col = shape > 0.3 ? vec3(0.62, 0.95, 0.30) : vec3(0.28, 0.55, 0.16);
    } else { // dust
        float shape = 1.0 - r - n * 0.6 - age * 0.8;
        if (shape < 0.0) discard;
        col = mix(vec3(0.72, 0.54, 0.38), vec3(0.88, 0.72, 0.52), step(0.5, vUV.y * 0.5 + 0.5 + (n - 0.5) * 0.4));
    }
    float dist = length(vW - uCamW);
    if (type == 0 || type == 4) col = haze(col, dist, (vW - uCamW) / dist);
    oCol = vec4(col, 1.0);
    oNrm = vec4(vFace, type == 0 || type == 4 ? dist : -dist); // glowing things get no ink outline
}
