// every primitive of the ui is an instanced quad in physical pixels. buffer
// 0 holds the globals, buffer 1 the instances, textures 0 to 2 the mask
// atlas, the image and the color atlas, samplers 0 and 1 linear and nearest.
//
// a fragment reads its quad from the instances by its index, and only what
// its kind needs: the vertices tell it what that is in a few bits. handing
// the quad over as flat varyings cost more than all the shading at 6k,
// every fragment pays for every varying and every field it looks at.
//
// a frame is drawn twice over. first the inside of every quad that hides
// what is under it, with no blending and its depth written, later quads
// nearer: the gpu shades the one on top alone. then everything in order,
// blended, where nothing later hid it. what a quad hides costs nothing.

#include <metal_stdlib>
using namespace metal;

struct Globals {
    float2 size;
    // x is 1 when the image bound has no see through pixel.
    float2 image;
};

struct Instance {
    // x, y, width, height.
    float4 rect [[attribute(0)]];
    // x0, y0, x1, y1.
    float4 clip [[attribute(1)]];
    // premultiplied fill.
    float4 color [[attribute(2)]];
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

// an instance as the fragments read it, laid out as civue_quad.
struct Quad {
    float4 rect;
    float4 clip;
    // premultiplied fill.
    float4 color;
    // premultiplied border.
    float4 border;
    // top left, top right, bottom right, bottom left.
    float4 radii;
    float4 params;
    float4 uv;
    float4 turn;
    // x, y, width, height of a rounded clip, then its radii, none when
    // zero.
    float4 round_rect;
    float4 round_radii;
};

struct Out {
    float4 pos [[position]];
    float2 uv [[center_no_perspective]];
    // the index of the quad under what its fragments have to do.
    uint what [[flat]];
};

// the kind is in the low bits.
constant uint KIND = 7u;
// turned, so shaded and clipped in its own space.
constant uint TURNED = 8u;
// under a rounded clip.
constant uint ROUNDED = 16u;
// nothing but the common case of its kind: a fill without a border, a
// mask without a gamma, an image smoothed and cut to its rect.
constant uint PLAIN = 32u;
// square corners.
constant uint SQUARE = 64u;
// square, unturned, and its edges between pixels: it covers each of them
// whole.
constant uint WHOLE = 128u;
constant uint INDEX = 8u;

// how much nearer each quad is than the one before it. a float holds every
// step of it exactly, for four million quads.
constant float STEP = 1.0 / 4194304.0;
// a rect edge this close to a pixel's is on it.
constant float ON = 0.002;

constant uint RECT = 0u;
constant uint SHADOW = 1u;
constant uint MASK = 2u;
constant uint IMAGE = 3u;
constant uint LINE = 4u;
constant uint COLOR_GLYPH = 5u;
constant uint ARC = 6u;

static bool any_set(float4 v) {
    return max(max(v.x, v.y), max(v.z, v.w)) > 0.0;
}

// what the fragments of a quad have to do.
static uint what_of(Instance inst) {
    uint kind = uint(inst.params.y);
    uint what = kind;
    if (inst.turn.x != 0.0 || inst.turn.y != 0.0) {
        what |= TURNED;
    }
    if (kind == RECT && inst.params.x == 0.0 && inst.params.z <= 0.5) {
        what |= PLAIN;
    }
    if (kind == MASK && inst.params.z == 1.0) {
        what |= PLAIN;
    }
    if (kind == IMAGE && inst.params.x <= 0.5 && inst.params.z <= 0.5 && inst.params.w <= 0.5) {
        what |= PLAIN;
    }
    if (!any_set(inst.radii)) {
        what |= SQUARE;
        float4 edges = float4(inst.rect.xy, inst.rect.xy + inst.rect.zw);
        float4 off = abs(edges - round(edges));
        if ((what & TURNED) == 0u && max(max(off.x, off.y), max(off.z, off.w)) < ON) {
            what |= WHOLE;
        }
    }
    return what;
}

// whether a rounded clip cuts anything between `lo` and `hi`: it only does
// what reaches into one of its corners.
static bool cut_by(Instance inst, float2 lo, float2 hi) {
    if (!any_set(inst.round_radii)) {
        return false;
    }
    float2 a = inst.round_rect.xy;
    float2 b = a + inst.round_rect.zw;
    float4 r = min(inst.round_radii, 0.5 * min(inst.round_rect.z, inst.round_rect.w));
    return (r.x > 0.0 && lo.x < a.x + r.x && lo.y < a.y + r.x)
        || (r.y > 0.0 && hi.x > b.x - r.y && lo.y < a.y + r.y)
        || (r.z > 0.0 && hi.x > b.x - r.z && hi.y > b.y - r.z)
        || (r.w > 0.0 && lo.x < a.x + r.w && hi.y > b.y - r.w);
}

static float4 place(float2 q, uint index, constant Globals& g) {
    float z = max(1.0 - float(index + 1u) * STEP, 0.0);
    return float4(q.x / g.size.x * 2.0 - 1.0, 1.0 - q.y / g.size.y * 2.0, z, 1.0);
}

vertex Out ui_vs(
    uint vi [[vertex_id]],
    uint index [[instance_id]],
    Instance inst [[stage_in]],
    constant Globals& g [[buffer(0)]]
) {
    float2 corner = float2(float(vi & 1u), float(vi >> 1u));
    float4 rect = inst.rect;
    uint what = what_of(inst);
    // shadows spread past their rect by three sigmas.
    if ((what & KIND) == SHADOW) {
        float m = inst.params.z * 3.0;
        rect = float4(rect.xy - m, rect.zw + 2.0 * m);
    }
    float2 p = rect.xy + corner * rect.zw;
    // a turned quad is shaded in its own unturned space, and clipped there
    // by its fragments. any other ends where its clip does, which cuts the
    // same pixels and shades none of those left out.
    float2 q = p;
    if ((what & TURNED) != 0u) {
        float2 d = p - inst.turn.zw;
        q = inst.turn.zw + float2(d.x * inst.turn.x - d.y * inst.turn.y, d.x * inst.turn.y + d.y * inst.turn.x);
        if (any_set(inst.round_radii)) {
            what |= ROUNDED;
        }
    } else {
        p = min(max(p, inst.clip.xy), inst.clip.zw);
        q = p;
        if (cut_by(inst, max(rect.xy, inst.clip.xy), min(rect.xy + rect.zw, inst.clip.zw))) {
            what |= ROUNDED;
        }
    }
    Out o;
    o.pos = place(q, index, g);
    float2 t = (p - inst.rect.xy) / inst.rect.zw;
    o.uv = mix(inst.uv.xy, inst.uv.zw, t);
    o.what = index << INDEX | what;
    return o;
}

// the part of a quad that hides what is under it: the pixels a fill or an
// image without a see through pixel covers whole. nothing for any other.
vertex Out ui_opaque_vs(
    uint vi [[vertex_id]],
    uint index [[instance_id]],
    Instance inst [[stage_in]],
    constant Globals& g [[buffer(0)]]
) {
    float2 corner = float2(float(vi & 1u), float(vi >> 1u));
    uint what = what_of(inst);
    uint kind = what & KIND;
    bool hides = (what & (TURNED | PLAIN | SQUARE)) == (PLAIN | SQUARE)
        && inst.color.a >= 0.999
        && (kind == RECT || (kind == IMAGE && g.image.x > 0.5));
    float2 lo = max(ceil(inst.rect.xy - ON), inst.clip.xy);
    float2 hi = min(floor(inst.rect.xy + inst.rect.zw + ON), inst.clip.zw);
    // under a rounded clip, the rows between its corners or the columns
    // between them, whichever are more.
    if (cut_by(inst, lo, hi)) {
        float2 a = inst.round_rect.xy;
        float2 b = a + inst.round_rect.zw;
        float4 r = min(inst.round_radii, 0.5 * min(inst.round_rect.z, inst.round_rect.w));
        float2 rows_lo = float2(lo.x, max(lo.y, a.y + max(r.x, r.y)));
        float2 rows_hi = float2(hi.x, min(hi.y, b.y - max(r.z, r.w)));
        float2 columns_lo = float2(max(lo.x, a.x + max(r.x, r.w)), lo.y);
        float2 columns_hi = float2(min(hi.x, b.x - max(r.y, r.z)), hi.y);
        float2 rows = max(rows_hi - rows_lo, float2(0.0));
        float2 columns = max(columns_hi - columns_lo, float2(0.0));
        bool wide = rows.x * rows.y >= columns.x * columns.y;
        lo = wide ? rows_lo : columns_lo;
        hi = wide ? rows_hi : columns_hi;
    }
    hi = hides ? max(hi, lo) : lo;
    float2 p = lo + corner * (hi - lo);
    Out o;
    o.pos = place(p, index, g);
    float2 t = (p - inst.rect.xy) / inst.rect.zw;
    o.uv = mix(inst.uv.xy, inst.uv.zw, t);
    o.what = index << INDEX | what;
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

// the same for square corners, from inside the rect.
static float square(float2 local, float4 rect) {
    float2 hs = rect.zw * 0.5;
    float2 q = abs(local - rect.xy - hs) - hs;
    return clamp(0.5 - max(q.x, q.y), 0.0, 1.0);
}

// what a rounded clip leaves of a pixel: only its corners cut, the sides
// are the rect clip's.
static float corners(float2 local, float4 rect, float4 radii) {
    float2 hs = rect.zw * 0.5;
    float2 p = local - rect.xy - hs;
    float r = p.x > 0.0 ? radii.y : radii.x;
    if (p.y > 0.0) {
        r = p.x > 0.0 ? radii.z : radii.w;
    }
    r = min(r, min(hs.x, hs.y));
    float2 q = abs(p) - hs + r;
    if (r <= 0.0 || q.x <= 0.0 || q.y <= 0.0) {
        return 1.0;
    }
    return clamp(0.5 + r - length(q), 0.0, 1.0);
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

// how much of a pixel a part of a ring covers: radii holds where it starts
// and how far it goes, in turns clockwise from its top, and how thick it is
// of its radius. it fades from its color at the top to its border at the
// bottom.
static float4 arc(const device Quad& in, float2 p) {
    float outer = in.rect.z * 0.5;
    float2 d = p - in.rect.xy - outer;
    float r = length(d);
    float inner = outer * (1.0 - in.radii.z);
    float a = clamp(0.5 + outer - r, 0.0, 1.0) * clamp(0.5 + r - inner, 0.0, 1.0);
    float sweep = in.radii.y;
    if (sweep < 1.0) {
        float t = fract(atan2(d.x, -d.y) * 0.15915494 - in.radii.x);
        float e = abs(t - sweep);
        float near = min(min(t, 1.0 - t), min(e, 1.0 - e));
        float side = t <= sweep ? near : -near;
        a *= clamp(0.5 + side * 6.2831853 * r, 0.0, 1.0);
    }
    float down = clamp((p.y - in.rect.y) / in.rect.w, 0.0, 1.0);
    return mix(in.color, in.border, down) * a;
}

// every kind in full.
static float4 shade(
    const device Quad& in,
    uint kind,
    float2 p,
    float2 uv,
    texture2d<float> mask_atlas,
    texture2d<float> image,
    texture2d<float> color_atlas,
    sampler linear_sampler,
    sampler nearest_sampler
) {
    if (kind == SHADOW) {
        return in.color * shadow(p, in.rect, max(in.params.z, 0.01), in.radii.x);
    }
    if (kind == ARC) {
        return arc(in, p);
    }
    if (kind == MASK) {
        float a = mask_atlas.sample(nearest_sampler, uv, level(0.0)).r;
        // a gamma under 1 thickens light text on dark backgrounds, like
        // browsers adjust text contrast.
        a = pow(a, in.params.z);
        return in.color * a;
    }
    if (kind == COLOR_GLYPH) {
        // color glyphs keep their colors, premultiplied, and only fade.
        return color_atlas.sample(linear_sampler, uv, level(0.0)) * in.color.a;
    }
    if (kind == IMAGE) {
        float4 c = in.params.w > 0.5 ? image.sample(nearest_sampler, uv, level(0.0)) : image.sample(linear_sampler, uv, level(0.0));
        // revealed: its red is when a pixel comes, its green how much.
        if (in.params.z > 1.5) {
            return c.r < in.border.x ? in.color * c.g : float4(0.0);
        }
        if (in.params.z > 0.5) {
            return pixel_grid(c, p, in.border);
        }
        // canvas tiles cover their pixels whole, so neighbors meet seamlessly.
        if (in.params.x > 0.5) {
            return c * in.color;
        }
        return c * in.color * coverage(p, in.rect, in.radii);
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
        float4 s = select(in.uv, float4(-1.0), in.uv == 0.0);
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
    const device Quad *quads [[buffer(1)]],
    texture2d<float> mask_atlas [[texture(0)]],
    texture2d<float> image [[texture(1)]],
    texture2d<float> color_atlas [[texture(2)]],
    sampler linear_sampler [[sampler(0)]],
    sampler nearest_sampler [[sampler(1)]]
) {
    const device Quad& quad = quads[in.what >> INDEX];
    uint what = in.what;
    uint kind = what & KIND;
    // the center of the pixel, which is where an unturned quad is shaded.
    float2 p = in.pos.xy;
    if ((what & TURNED) != 0u) {
        float2 d = p - quad.turn.zw;
        p = quad.turn.zw + float2(d.x * quad.turn.x + d.y * quad.turn.y, d.y * quad.turn.x - d.x * quad.turn.y);
        if (p.x < quad.clip.x || p.y < quad.clip.y || p.x >= quad.clip.z || p.y >= quad.clip.w) {
            return float4(0.0);
        }
    }
    float4 c;
    if ((what & PLAIN) == 0u) {
        c = shade(quad, kind, p, in.uv, mask_atlas, image, color_atlas, linear_sampler, nearest_sampler);
    } else if (kind == MASK) {
        c = quad.color * mask_atlas.sample(nearest_sampler, in.uv, level(0.0)).r;
    } else {
        c = kind == IMAGE ? image.sample(linear_sampler, in.uv, level(0.0)) * quad.color : quad.color;
        if ((what & WHOLE) == 0u) {
            c *= (what & SQUARE) != 0u ? square(p, quad.rect) : coverage(p, quad.rect, quad.radii);
        }
    }
    // a rounded clip cuts the corners its rect clip leaves.
    if ((what & ROUNDED) != 0u) {
        c *= corners(p, quad.round_rect, quad.round_radii);
    }
    return c;
}

fragment float4 ui_opaque_fs(
    Out in [[stage_in]],
    const device Quad *quads [[buffer(1)]],
    texture2d<float> image [[texture(1)]],
    sampler linear_sampler [[sampler(0)]]
) {
    const device Quad& quad = quads[in.what >> INDEX];
    if ((in.what & KIND) == IMAGE) {
        return image.sample(linear_sampler, in.uv, level(0.0)) * quad.color;
    }
    return quad.color;
}
