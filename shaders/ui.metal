// every primitive of the ui is an instanced quad in physical pixels. buffer
// 0 holds the globals, buffer 1 the instances, textures 0 to 2 the mask
// atlas, the image and the color atlas, samplers 0 and 1 linear and nearest.

#include <metal_stdlib>
using namespace metal;

struct Globals {
    float2 size;
    float2 pad;
};

struct Instance {
    // x, y, width, height.
    float4 rect [[attribute(0)]];
    // x0, y0, x1, y1.
    float4 clip [[attribute(1)]];
    // premultiplied fill.
    float4 color [[attribute(2)]];
    // premultiplied border.
    float4 border [[attribute(3)]];
    // top left, top right, bottom right, bottom left.
    float4 radii [[attribute(4)]];
    // x is the border width, y the kind, z the blur sigma, text gamma or 1
    // for a pixel grid, w nearest sampling.
    float4 params [[attribute(5)]];
    // u0, v0, u1, v1, or a rect's border widths from the top clockwise
    // when its border width is negative.
    float4 uv [[attribute(6)]];
    // cos, sin, pivot x, pivot y of a turn, none when cos and sin are zero.
    float4 turn [[attribute(7)]];
    // x, y, width, height of a rounded clip, then its radii, none when
    // zero.
    float4 round_rect [[attribute(8)]];
    float4 round_radii [[attribute(9)]];
};

struct Out {
    float4 pos [[position]];
    float2 local;
    float4 rect [[flat]];
    float4 clip [[flat]];
    float4 color [[flat]];
    float4 border [[flat]];
    float4 radii [[flat]];
    float4 params [[flat]];
    float4 sides [[flat]];
    float4 round_rect [[flat]];
    float4 round_radii [[flat]];
    float2 uv;
};

constant float SHADOW = 1.0;
constant float MASK = 2.0;
constant float IMAGE = 3.0;
constant float LINE = 4.0;
constant float COLOR_GLYPH = 5.0;

vertex Out ui_vs(uint vi [[vertex_id]], Instance inst [[stage_in]], constant Globals& g [[buffer(0)]]) {
    float2 corner = float2(float(vi & 1u), float(vi >> 1u));
    float4 rect = inst.rect;
    // shadows spread past their rect by three sigmas.
    if (inst.params.y == SHADOW) {
        float m = inst.params.z * 3.0;
        rect = float4(rect.xy - m, rect.zw + 2.0 * m);
    }
    float2 p = rect.xy + corner * rect.zw;
    // a turned quad is shaded in its own unturned space.
    float2 q = p;
    if (inst.turn.x != 0.0 || inst.turn.y != 0.0) {
        float2 d = p - inst.turn.zw;
        q = inst.turn.zw + float2(d.x * inst.turn.x - d.y * inst.turn.y, d.x * inst.turn.y + d.y * inst.turn.x);
    }
    Out o;
    o.pos = float4(q.x / g.size.x * 2.0 - 1.0, 1.0 - q.y / g.size.y * 2.0, 0.0, 1.0);
    o.local = p;
    o.rect = inst.rect;
    o.clip = inst.clip;
    o.color = inst.color;
    o.border = inst.border;
    o.radii = inst.radii;
    o.params = inst.params;
    o.sides = inst.uv;
    o.round_rect = inst.round_rect;
    o.round_radii = inst.round_radii;
    float2 t = (p - inst.rect.xy) / inst.rect.zw;
    o.uv = mix(inst.uv.xy, inst.uv.zw, t);
    return o;
}

// figma's pixel grid, from its canvas shader: the first pixel after each
// canvas unit's edge moves 0.05 darker when light, lighter towards white
// when dark. grid is the origin modulo a unit and the pixels per unit.
static float4 pixel_grid(float4 c, float2 p, float4 grid) {
    float2 f = fract((p - grid.xy) / grid.z);
    float d = 1.0 / grid.z;
    if (f.x >= d && f.y >= d) {
        return c;
    }
    float3 b = c.rgb;
    float3 h = float3(0.299, 0.587, 0.114);
    float i = dot(b * b, h);
    float j = sqrt(i);
    float3 g;
    if (j > 0.5) {
        g = b * ((j - 0.05) / j);
    } else {
        float k = j * 0.8 + 0.15;
        float l = dot(b, h);
        float m = 1.0 - 2.0 * l + i;
        float n = i - l;
        g = mix(b, float3(1.0), (n + sqrt(n * n - m * (i - k * k))) / m);
    }
    return float4(g, c.a);
}

// signed distance to a rounded rect centered at the origin.
static float sd_rounded(float2 p, float2 hs, float4 radii) {
    float r = p.x > 0.0 ? radii.y : radii.x;
    if (p.y > 0.0) {
        r = p.x > 0.0 ? radii.z : radii.w;
    }
    r = min(r, min(hs.x, hs.y));
    float2 q = abs(p) - hs + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, float2(0.0))) - r;
}

static float coverage(float2 local, float4 rect, float4 radii) {
    float2 hs = rect.zw * 0.5;
    float d = sd_rounded(local - rect.xy - hs, hs, radii);
    return clamp(0.5 - d, 0.0, 1.0);
}

// evan wallace's closed form for a gaussian blurred rounded rect.
static float gaussian(float x, float sigma) {
    return exp(-(x * x) / (2.0 * sigma * sigma)) / (2.50662827 * sigma);
}

static float2 erf2(float2 x) {
    float2 s = sign(x);
    float2 a = abs(x);
    float2 r = 1.0 + (0.278393 + (0.230389 + 0.078108 * (a * a)) * a) * a;
    r *= r;
    return s - s / (r * r);
}

static float shadow_x(float x, float y, float sigma, float corner, float2 hs) {
    float delta = min(hs.y - corner - abs(y), 0.0);
    float curved = hs.x - corner + sqrt(max(0.0, corner * corner - delta * delta));
    float2 integral = 0.5 + 0.5 * erf2((x + float2(-curved, curved)) * (0.70710678 / sigma));
    return integral.y - integral.x;
}

static float shadow(float2 local, float4 rect, float sigma, float corner) {
    float2 hs = rect.zw * 0.5;
    float2 p = local - rect.xy - hs;
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

static float4 shade(
    Out in,
    texture2d<float> mask_atlas,
    texture2d<float> image,
    texture2d<float> color_atlas,
    sampler linear_sampler,
    sampler nearest_sampler
) {
    float2 p = in.local;
    if (p.x < in.clip.x || p.y < in.clip.y || p.x >= in.clip.z || p.y >= in.clip.w) {
        discard_fragment();
    }
    float kind = in.params.y;
    if (kind == SHADOW) {
        return in.color * shadow(p, in.rect, max(in.params.z, 0.01), in.radii.x);
    }
    if (kind == MASK) {
        float a = mask_atlas.sample(nearest_sampler, in.uv, level(0.0)).r;
        // a gamma under 1 thickens light text on dark backgrounds, like
        // browsers adjust text contrast.
        a = pow(a, in.params.z);
        return in.color * a;
    }
    if (kind == COLOR_GLYPH) {
        // color glyphs keep their colors, premultiplied, and only fade.
        return color_atlas.sample(linear_sampler, in.uv, level(0.0)) * in.color.a;
    }
    if (kind == IMAGE) {
        float4 c = in.params.w > 0.5 ? image.sample(nearest_sampler, in.uv, level(0.0)) : image.sample(linear_sampler, in.uv, level(0.0));
        if (in.params.z > 0.5) {
            return pixel_grid(c, p, in.border);
        }
        // canvas tiles cover their pixels whole, so neighbors meet seamlessly.
        if (in.params.x > 0.5) {
            return c * in.color.a;
        }
        return c * in.color.a * coverage(p, in.rect, in.radii);
    }
    if (kind == LINE) {
        // radii holds the segment's ends, params.x its width.
        float2 a = in.radii.xy;
        float2 ab = in.radii.zw - a;
        // square ends: a band as long as the segment, any angle.
        if (in.params.z > 0.5) {
            float len = max(length(ab), 1e-6);
            float along = dot(p - a, ab) / len;
            float across = abs(ab.x * (p.y - a.y) - ab.y * (p.x - a.x)) / len;
            float ends = clamp(min(along, len - along) + 0.5, 0.0, 1.0);
            return in.color * clamp(in.params.x * 0.5 + 0.5 - across, 0.0, 1.0) * ends;
        }
        float t = clamp(dot(p - a, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
        float d = length(p - a - ab * t);
        return in.color * clamp(in.params.x * 0.5 + 0.5 - d, 0.0, 1.0);
    }
    // a rounded rect with an inside border.
    float2 hs = in.rect.zw * 0.5;
    float2 q = p - in.rect.xy - hs;
    float d = sd_rounded(q, hs, in.radii);
    float outer = clamp(0.5 - d, 0.0, 1.0);
    float bw = in.params.x;
    if (in.params.z > 0.5) {
        return pixel_grid(in.color, p, in.border);
    }
    if (bw == 0.0) {
        return in.color * outer;
    }
    float inner;
    if (bw < 0.0) {
        // borders of their own width per side, the inner corners rounded by
        // what the wider of their two sides leaves, as browsers do. a side
        // without a border has its inner edge a pixel past the outer one, so
        // their antialiasing doesn't meet.
        float4 s = select(in.sides, float4(-1.0), in.sides == 0.0);
        float2 inner_half = max((in.rect.zw - float2(s.w + s.y, s.x + s.z)) * 0.5, float2(0.0));
        float2 center = in.rect.xy + float2(s.w, s.x) + inner_half;
        float4 wider = float4(max(s.x, s.w), max(s.x, s.y), max(s.z, s.y), max(s.z, s.w));
        float4 radii = max(in.radii - wider, float4(0.0));
        inner = clamp(0.5 - sd_rounded(p - center, inner_half, radii), 0.0, 1.0);
        if (inner_half.x <= 0.0 || inner_half.y <= 0.0) {
            inner = 0.0;
        }
    } else {
        float2 inner_half = max(hs - bw, float2(0.0));
        inner = clamp(0.5 - sd_rounded(q, inner_half, max(in.radii - bw, float4(0.0))), 0.0, 1.0);
    }
    float4 fill = in.color * inner;
    return (in.border * (1.0 - inner) + fill) * outer;
}

fragment float4 ui_fs(
    Out in [[stage_in]],
    texture2d<float> mask_atlas [[texture(0)]],
    texture2d<float> image [[texture(1)]],
    texture2d<float> color_atlas [[texture(2)]],
    sampler linear_sampler [[sampler(0)]],
    sampler nearest_sampler [[sampler(1)]]
) {
    float4 c = shade(in, mask_atlas, image, color_atlas, linear_sampler, nearest_sampler);
    // a rounded clip cuts the corners its rect clip leaves.
    if (max(max(in.round_radii.x, in.round_radii.y), max(in.round_radii.z, in.round_radii.w)) > 0.0) {
        c *= coverage(in.local, in.round_rect, in.round_radii);
    }
    return c;
}
