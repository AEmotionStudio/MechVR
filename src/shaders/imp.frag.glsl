// The sheet stores surface colour, kind of surface, normal and shading, not finished pixels,
// so a card is lit, tinted, hazed and outlined exactly like a traced part.
in vec2 vUV;
in vec3 vW;
layout(location = 0) out vec4 oCol;
layout(location = 1) out vec4 oNrm; // world normal, view distance
uniform sampler2D uAtlasC; // rgb colour, a = kind: 0 empty, 1 plain, 2 team colour A, 3 team colour B, 4 frame, 5 glass, 6 glowing
uniform sampler2D uAtlasN; // rg normal in the mech's frame, b = depth through the card, a = soft shadowing in creases
uniform float uWidth;      // card width in metres (the depth range)
uniform int uLod;          // which size of the sheet to read: farther cards use smaller ones
uniform float uMirror;     // 1 = show the view mirrored left to right (an arm made for the other side)
uniform vec4 uCell;        // this view's corner and size on the sheet, in texels
uniform vec2 uCS;          // cos and sin of the mech's heading
uniform vec3 uTintA, uTintB, uCamW;
uniform float uFlash;
void main() {
    vec2 uv = vec2(uMirror > 0.5 ? 1.0 - vUV.x : vUV.x, vUV.y);
    ivec2 tc = ivec2(uCell.xy + clamp(uv, vec2(0.0), vec2(0.9999)) * uCell.zw) >> uLod;
    vec4 c = texelFetch(uAtlasC, tc, uLod);
    int id = int(c.a * 7.0 + 0.5);
    if (id == 0) discard;
    vec4 nn = texelFetch(uAtlasN, tc, uLod);
    vec3 n = octDec(nn.xy);
    if (uMirror > 0.5) n.x = -n.x;
    vec3 nw = vec3(n.x * uCS.x + n.z * uCS.y, n.y, n.z * uCS.x - n.x * uCS.y);
    vec3 vw = normalize(uCamW - vW);
    vec3 base = c.rgb * (id == 2 ? uTintA : id == 3 ? uTintB : vec3(1.0));
    float spec = 0.0;
    if (id >= 2 && id <= 5) spec = step(0.5, pow(max(dot(nw, normalize(SUN + vw)), 0.0), id == 4 ? 30.0 : id == 5 ? 16.0 : 60.0));
    vec3 col = toon(base, dot(nw, SUN), nn.a, spec, 0.0);
    if (id == 5) col += vec3(0.20, 0.30, 0.32) * step(0.72, 1.0 - max(dot(nw, vw), 0.0)); // glass rim sheen
    if (id == 6) col = base;
    col = mix(col, vec3(1.0, 0.96, 0.82), uFlash * 0.55);
    float dist = length(vW - uCamW) + (nn.b - 0.5) * uWidth; // depth inside the card, so ink lines between its parts survive
    oCol = vec4(haze(col, dist, -vw), 1.0);
    oNrm = vec4(nw, id == 6 ? -dist : dist);
}
