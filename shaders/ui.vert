#version 450
// every primitive of the ui is an instanced quad in physical pixels, the
// twin of ui.metal's ui_vs.

layout(push_constant) uniform Globals {
    vec2 size;
    vec2 pad;
} g;

// x, y, width, height.
layout(location = 0) in vec4 rect;
// x0, y0, x1, y1.
layout(location = 1) in vec4 clip;
// premultiplied fill.
layout(location = 2) in vec4 color;
// premultiplied border.
layout(location = 3) in vec4 border;
// top left, top right, bottom right, bottom left.
layout(location = 4) in vec4 radii;
// x is the border width, y the kind, z the blur sigma, text gamma or 1
// for a pixel grid, w nearest sampling.
layout(location = 5) in vec4 params;
// u0, v0, u1, v1, or a rect's border widths from the top clockwise when
// its border width is negative.
layout(location = 6) in vec4 uv;
// cos, sin, pivot x, pivot y of a turn, none when cos and sin are zero.
layout(location = 7) in vec4 turn;
// x, y, width, height of a rounded clip, then its radii, none when zero.
layout(location = 8) in vec4 round_rect;
layout(location = 9) in vec4 round_radii;

layout(location = 0) out vec2 o_local;
layout(location = 1) flat out vec4 o_rect;
layout(location = 2) flat out vec4 o_clip;
layout(location = 3) flat out vec4 o_color;
layout(location = 4) flat out vec4 o_border;
layout(location = 5) flat out vec4 o_radii;
layout(location = 6) flat out vec4 o_params;
layout(location = 7) out vec2 o_uv;
layout(location = 8) flat out vec4 o_sides;
layout(location = 9) flat out vec4 o_round;
layout(location = 10) flat out vec4 o_round_radii;

const float SHADOW = 1.0;

void main() {
    uint vi = uint(gl_VertexIndex);
    vec2 corner = vec2(float(vi & 1u), float(vi >> 1u));
    vec4 r = rect;
    // shadows spread past their rect by three sigmas.
    if (params.y == SHADOW) {
        float m = params.z * 3.0;
        r = vec4(r.xy - m, r.zw + 2.0 * m);
    }
    vec2 p = r.xy + corner * r.zw;
    // a turned quad is shaded in its own unturned space.
    vec2 q = p;
    if (turn.x != 0.0 || turn.y != 0.0) {
        vec2 d = p - turn.zw;
        q = turn.zw + vec2(d.x * turn.x - d.y * turn.y, d.x * turn.y + d.y * turn.x);
    }
    gl_Position = vec4(q.x / g.size.x * 2.0 - 1.0, 1.0 - q.y / g.size.y * 2.0, 0.0, 1.0);
    o_local = p;
    o_rect = rect;
    o_clip = clip;
    o_color = color;
    o_border = border;
    o_radii = radii;
    o_params = params;
    o_sides = uv;
    o_round = round_rect;
    o_round_radii = round_radii;
    vec2 t = (p - rect.xy) / rect.zw;
    o_uv = mix(uv.xy, uv.zw, t);
}
