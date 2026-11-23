// A distant mech drawn as a flat card that turns to face the viewer. The picture on it comes from a
// sheet of views of the mech, drawn once at startup from the same shapes (see imp.frag.glsl).
uniform mat4 uVP;
uniform vec3 uCenter, uRight; // card anchor and its sideways direction, world space
uniform vec3 uRect;           // card width, bottom and height in metres
out vec2 vUV;
out vec3 vW;
void main() {
    vec2 c = vec2(gl_VertexID & 1, gl_VertexID >> 1);
    vUV = c;
    vW = uCenter + uRight * ((c.x - 0.5) * uRect.x) + vec3(0.0, uRect.y + c.y * uRect.z, 0.0);
    gl_Position = uVP * vec4(vW, 1.0);
}
