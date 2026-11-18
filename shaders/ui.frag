#version 450
// the ui's quads shaded: textures 0 to 2 the mask atlas, the image and the
// color atlas, samplers linear and nearest. the twin of ui.metal's ui_fs.

layout(set = 0, binding = 0) uniform texture2D mask_atlas;
layout(set = 0, binding = 1) uniform texture2D image;
layout(set = 0, binding = 2) uniform texture2D color_atlas;
layout(set = 0, binding = 3) uniform sampler linear_sampler;
layout(set = 0, binding = 4) uniform sampler nearest_sampler;

layout(location = 0) in vec2 i_local;
layout(location = 1) flat in vec4 i_rect;
layout(location = 2) flat in vec4 i_clip;
layout(location = 3) flat in vec4 i_color;
layout(location = 4) flat in vec4 i_border;
layout(location = 5) flat in vec4 i_radii;
layout(location = 6) flat in vec4 i_params;
layout(location = 7) in vec2 i_uv;
layout(location = 8) flat in vec4 i_sides;
layout(location = 9) flat in vec4 i_round;
layout(location = 10) flat in vec4 i_round_radii;

layout(location = 0) out vec4 out_color;

const float SHADOW = 1.0;
const float MASK = 2.0;
const float IMAGE = 3.0;
const float LINE = 4.0;
const float COLOR_GLYPH = 5.0;

// figma's pixel grid, from its canvas shader: the first pixel after each
// canvas unit's edge moves 0.05 darker when light, lighter towards white
// when dark. grid is the origin modulo a unit and the pixels per unit.
vec4 pixel_grid(vec4 c, vec2 p, vec4 grid) {
    vec2 f = fract((p - grid.xy) / grid.z);
    float d = 1.0 / grid.z;
    if (f.x >= d && f.y >= d) {
        return c;
    }
    vec3 b = c.rgb;
    vec3 h = vec3(0.299, 0.587, 0.114);
    float i = dot(b * b, h);
    float j = sqrt(i);
    vec3 g;
    if (j > 0.5) {
        g = b * ((j - 0.05) / j);
    } else {
        float k = j * 0.8 + 0.15;
        float l = dot(b, h);
        float m = 1.0 - 2.0 * l + i;
        float n = i - l;
        g = mix(b, vec3(1.0), (n + sqrt(n * n - m * (i - k * k))) / m);
    }
    return vec4(g, c.a);
}

// signed distance to a rounded rect centered at the origin.
float sd_rounded(vec2 p, vec2 hs, vec4 radii) {
    float r = p.x > 0.0 ? radii.y : radii.x;
    if (p.y > 0.0) {
        r = p.x > 0.0 ? radii.z : radii.w;
    }
    r = min(r, min(hs.x, hs.y));
    vec2 q = abs(p) - hs + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;
}

float coverage(vec2 local, vec4 rect, vec4 radii) {
    vec2 hs = rect.zw * 0.5;
    float d = sd_rounded(local - rect.xy - hs, hs, radii);
    return clamp(0.5 - d, 0.0, 1.0);
}

// what a rounded clip leaves of a pixel: only its corners cut, the sides
// are the rect clip's.
float corners(vec2 local, vec4 rect, vec4 radii) {
    vec2 hs = rect.zw * 0.5;
    vec2 p = local - rect.xy - hs;
    float r = p.x > 0.0 ? radii.y : radii.x;
    if (p.y > 0.0) {
        r = p.x > 0.0 ? radii.z : radii.w;
    }
    r = min(r, min(hs.x, hs.y));
    vec2 q = abs(p) - hs + r;
    if (r <= 0.0 || q.x <= 0.0 || q.y <= 0.0) {
        return 1.0;
    }
    return clamp(0.5 + r - length(q), 0.0, 1.0);
}

// evan wallace's closed form for a gaussian blurred rounded rect.
float gaussian(float x, float sigma) {
    return exp(-(x * x) / (2.0 * sigma * sigma)) / (2.50662827 * sigma);
}

vec2 erf2(vec2 x) {
    vec2 s = sign(x);
    vec2 a = abs(x);
    vec2 r = 1.0 + (0.278393 + (0.230389 + 0.078108 * (a * a)) * a) * a;
    r *= r;
    return s - s / (r * r);
}

float shadow_x(float x, float y, float sigma, float corner, vec2 hs) {
    float delta = min(hs.y - corner - abs(y), 0.0);
    float curved = hs.x - corner + sqrt(max(0.0, corner * corner - delta * delta));
    vec2 integral = 0.5 + 0.5 * erf2((x + vec2(-curved, curved)) * (0.70710678 / sigma));
    return integral.y - integral.x;
}

float shadow(vec2 local, vec4 rect, float sigma, float corner) {
    vec2 hs = rect.zw * 0.5;
    vec2 p = local - rect.xy - hs;
    float low = p.y - hs.y;
    float high = p.y + hs.y;
    float start = clamp(-3.0 * sigma, low, high);
    float end = clamp(3.0 * sigma, low, high);
    float st = (end - start) / 4.0;
    float y = start + st * 0.5;
    float v = 0.0;
    for (int i = 0; i < 4; i += 1) {
        v += shadow_x(p.x, p.y - y, sigma, corner, hs) * gaussian(y, sigma) * st;
        y += st;
    }
    return v;
}

vec4 shade() {
    vec2 p = i_local;
    if (p.x < i_clip.x || p.y < i_clip.y || p.x >= i_clip.z || p.y >= i_clip.w) {
        discard;
    }
    float kind = i_params.y;
    if (kind == SHADOW) {
        return i_color * shadow(p, i_rect, max(i_params.z, 0.01), i_radii.x);
    }
    if (kind == MASK) {
        float a = textureLod(sampler2D(mask_atlas, nearest_sampler), i_uv, 0.0).r;
        // a gamma under 1 thickens light text on dark backgrounds, like
        // browsers adjust text contrast.
        a = pow(a, i_params.z);
        return i_color * a;
    }
    if (kind == COLOR_GLYPH) {
        // color glyphs keep their colors, premultiplied, and only fade.
        return textureLod(sampler2D(color_atlas, linear_sampler), i_uv, 0.0) * i_color.a;
    }
    if (kind == IMAGE) {
        vec4 c = i_params.w > 0.5 ? textureLod(sampler2D(image, nearest_sampler), i_uv, 0.0) : textureLod(sampler2D(image, linear_sampler), i_uv, 0.0);
        if (i_params.z > 0.5) {
            return pixel_grid(c, p, i_border);
        }
        // canvas tiles cover their pixels whole, so neighbors meet seamlessly.
        if (i_params.x > 0.5) {
            return c * i_color.a;
        }
        return c * i_color.a * coverage(p, i_rect, i_radii);
    }
    if (kind == LINE) {
        // radii holds the segment's ends, params.x its width.
        vec2 a = i_radii.xy;
        vec2 ab = i_radii.zw - a;
        // square ends: a band as long as the segment, any angle.
        if (i_params.z > 0.5) {
            float len = max(length(ab), 1e-6);
            float along = dot(p - a, ab) / len;
            float across = abs(ab.x * (p.y - a.y) - ab.y * (p.x - a.x)) / len;
            float ends = clamp(min(along, len - along) + 0.5, 0.0, 1.0);
            return i_color * clamp(i_params.x * 0.5 + 0.5 - across, 0.0, 1.0) * ends;
        }
        float t = clamp(dot(p - a, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
        float d = length(p - a - ab * t);
        return i_color * clamp(i_params.x * 0.5 + 0.5 - d, 0.0, 1.0);
    }
    // a rounded rect with an inside border.
    vec2 hs = i_rect.zw * 0.5;
    vec2 q = p - i_rect.xy - hs;
    float d = sd_rounded(q, hs, i_radii);
    float outer = clamp(0.5 - d, 0.0, 1.0);
    float bw = i_params.x;
    if (i_params.z > 0.5) {
        return pixel_grid(i_color, p, i_border);
    }
    if (bw == 0.0) {
        return i_color * outer;
    }
    float inner;
    if (bw < 0.0) {
        // borders of their own width per side, the inner corners rounded by
        // what the wider of their two sides leaves, as browsers do. a side
        // without a border has its inner edge a pixel past the outer one, so
        // their antialiasing doesn't meet.
        vec4 s = mix(i_sides, vec4(-1.0), equal(i_sides, vec4(0.0)));
        vec2 inner_half = max((i_rect.zw - vec2(s.w + s.y, s.x + s.z)) * 0.5, vec2(0.0));
        vec2 center = i_rect.xy + vec2(s.w, s.x) + inner_half;
        vec4 wider = vec4(max(s.x, s.w), max(s.x, s.y), max(s.z, s.y), max(s.z, s.w));
        vec4 radii = max(i_radii - wider, vec4(0.0));
        inner = clamp(0.5 - sd_rounded(p - center, inner_half, radii), 0.0, 1.0);
        if (inner_half.x <= 0.0 || inner_half.y <= 0.0) {
            inner = 0.0;
        }
    } else {
        vec2 inner_half = max(hs - bw, vec2(0.0));
        inner = clamp(0.5 - sd_rounded(q, inner_half, max(i_radii - bw, vec4(0.0))), 0.0, 1.0);
    }
    vec4 fill = i_color * inner;
    return (i_border * (1.0 - inner) + fill) * outer;
}

void main() {
    vec4 c = shade();
    // a rounded clip cuts the corners its rect clip leaves.
    if (max(max(i_round_radii.x, i_round_radii.y), max(i_round_radii.z, i_round_radii.w)) > 0.0) {
        c *= corners(i_local, i_round, i_round_radii);
    }
    out_color = c;
}
