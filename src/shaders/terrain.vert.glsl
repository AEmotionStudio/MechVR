// Ground: a grid that follows the camera, lifted by the height map.
layout(location = 0) in vec2 aGrid;
uniform mat4 uVP;
uniform vec2 uOrigin;
out vec3 vW;
void main() {
    vec2 xz = uOrigin + aGrid;
    vW = vec3(xz.x, H(xz), xz.y);
    gl_Position = uVP * vec4(vW, 1.0);
}
