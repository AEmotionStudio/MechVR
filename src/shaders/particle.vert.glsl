// Smoke, fire, sparks and flashes: cards that face the player's head (the same way for both eyes).
layout(location = 0) in vec2 aCorner;
layout(location = 1) in vec4 iPosSize;
layout(location = 2) in vec4 iData; // age 0..1, type, seed, spin
uniform mat4 uVP;
uniform vec3 uHead;
out vec2 vUV;
out vec4 vData;
out vec3 vW;
out vec3 vFace;
void main() {
    vec3 f = normalize(iPosSize.xyz - uHead);
    vec3 r = normalize(cross(f, vec3(0, 1, 0)));
    vec3 u = cross(r, f);
    float a = iData.w, c = cos(a), s = sin(a);
    vec2 q = vec2(aCorner.x * c - aCorner.y * s, aCorner.x * s + aCorner.y * c);
    vW = iPosSize.xyz + (r * q.x + u * q.y) * iPosSize.w;
    vUV = aCorner;
    vData = iData;
    vFace = -f;
    gl_Position = uVP * vec4(vW, 1.0);
}
