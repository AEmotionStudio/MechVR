// A part is drawn as a box; the fragment shader traces the real shape inside it.
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;
uniform vec3 uBoxC, uBoxH;
out vec3 vLocal;
void main() {
    vLocal = uBoxC + aPos * uBoxH;
    gl_Position = uMVP * vec4(vLocal, 1.0);
}
