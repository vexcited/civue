#include "internal.h"

#include <civue/icons.h>

typedef struct {
  f32 uv[4];
  u64 generation;
} Entry;

// a width and a height read from an svg's header once: bit 0 width written, 1
// height written, 2 both read, 3 cached.
typedef struct {
  f32 size[2];
  u8 known;
  // the view box's width over its height, 0 until read.
  f32 aspect;
} Natural;

struct civue_icons {
  // icon, width and height to entry + 1.
  civue_map index;
  Vec(Entry) entries;
  const civue_icon_svg *svgs;
  u32 count;
  // the natural sizes of the app's icons.
  Natural *natural;
};

static const civue_icon_svg *preset_svgs;
static u32 preset_count;

void civue_icons_preset(const civue_icon_svg *svgs, uint32_t count) {
  preset_svgs = svgs;
  preset_count = count;
}

civue_icons *civue_icons_new() {
  civue_icons *ic = own_new(sizeof(civue_icons));
  if (preset_svgs) {
    civue_icons_use(ic, preset_svgs, preset_count);
  }
  return ic;
}

void civue_icons_use(civue_icons *ic, const civue_icon_svg *svgs,
                     uint32_t count) {
  own_free(ic->natural);
  ic->svgs = svgs;
  ic->count = count;
  ic->natural = own_new(count * sizeof(Natural));
  map_clear(&ic->index);
  ic->entries.len = 0;
}

void civue_icons_free(civue_icons *ic) {
  if (!ic) {
    return;
  }
  map_free(&ic->index);
  vec_free(&ic->entries);
  own_free(ic->natural);
  own_free(ic);
}

// the svg of an icon and where its natural size is kept, nullptr for an icon
// nobody registered.
static const civue_icon_svg *svg_of(civue_icons *ic, civue_icon icon,
                                    Natural **natural) {
  const civue_icon_svg *s = icon < ic->count ? &ic->svgs[icon] : nullptr;
  if (natural) {
    *natural = s ? &ic->natural[icon] : nullptr;
  }
  return s;
}

static bool attr(const char *tag, usize n, const char *name, f32 *out) {
  usize k = strlen(name);
  for (usize i = 0; i + k + 3 < n; i++) {
    if (tag[i] != ' ' || memcmp(tag + i + 1, name, k) ||
        tag[i + 1 + k] != '=' || tag[i + 2 + k] != '"') {
      continue;
    }
    char *end;
    *out = strtof(tag + i + 3 + k, &end);
    return end != tag + i + 3 + k;
  }
  return false;
}

bool civue_icon_natural_size(civue_icons *ic, civue_icon icon, float *w,
                             float *h) {
  Natural *nat;
  const civue_icon_svg *s = svg_of(ic, icon, &nat);
  if (!s) {
    return false;
  }
  // read once from the svg header: asked for every icon of every frame.
  if (!nat->known) {
    const char *close = memchr(s->svg, '>', s->len);
    usize n = close ? (usize)(close - s->svg) : 0;
    f32 *v = nat->size;
    v[0] = v[1] = NAN;
    bool ok =
      attr(s->svg, n, "width", &v[0]) && attr(s->svg, n, "height", &v[1]);
    nat->known = 8 | !isnan(v[0]) | !isnan(v[1]) << 1 | ok << 2;
  }
  // what the svg lacks keeps the caller's default.
  if (nat->known & 1) {
    *w = nat->size[0];
  }
  if (nat->known & 2) {
    *h = nat->size[1];
  }
  return nat->known & 4;
}

float civue_icon_aspect(civue_icons *ic, civue_icon icon) {
  Natural *nat;
  const civue_icon_svg *s = svg_of(ic, icon, &nat);
  if (!s) {
    return 1;
  }
  if (!nat->aspect) {
    nat->aspect = 1;
    const char *close = memchr(s->svg, '>', s->len);
    usize n = close ? (usize)(close - s->svg) : 0;
    const char *box = n ? memmem(s->svg, n, "viewBox=\"", 9) : nullptr;
    if (box) {
      char *end;
      f32 v[4];
      const char *at = box + 9;
      for (u32 i = 0; i < 4; i++, at = end) {
        v[i] = strtof(at, &end);
      }
      if (v[2] > 0 && v[3] > 0) {
        nat->aspect = v[2] / v[3];
      }
    }
  }
  return nat->aspect;
}

void civue_icons_draw(civue_icons *ic, civue_draw_list *d,
                      civue_atlas *mask_atlas, civue_atlas *color_atlas,
                      civue_icon icon, civue_rect r, civue_color c,
                      bool mirror) {
  const civue_icon_svg *s = svg_of(ic, icon, nullptr);
  if (!s) {
    return;
  }
  civue_atlas *atlas = s->color ? color_atlas : mask_atlas;
  u32 w = (u32)fmax(round(rect_w(r)), 1), h = (u32)fmax(round(rect_h(r)), 1);
  u64 key = ((u64)icon << 40 | (u64)w << 20 | h) + 1;
  u64 at;
  Entry *e = map_get(&ic->index, key, &at) ? &ic->entries.v[at - 1] : nullptr;
  if (!e || e->generation != atlas->generation) {
    civue_bytes rgba = {};
    if (!civue_svg_rasterize(s->svg, s->len, w, h, &rgba)) {
      vec_free(&rgba);
      return;
    }
    // the tint comes from the color, only coverage is kept, unless the icon
    // has colors of its own.
    u8 *mask = nullptr;
    if (!s->color) {
      mask = cilo_malloc((usize)w * h);
      for (usize i = 0; i < (usize)w * h; i++) {
        mask[i] = rgba.v[i * 4 + 3];
      }
    }
    const u8 *pixels = mask ? mask : rgba.v;
    f32 uv[4];
    bool ok = civue_atlas_insert(atlas, w, h, pixels, uv);
    if (!ok) {
      civue_atlas_clear(atlas);
      ok = civue_atlas_insert(atlas, w, h, pixels, uv);
    }
    cilo_mfree(mask);
    vec_free(&rgba);
    if (!ok) {
      return;
    }
    Entry n = {{uv[0], uv[1], uv[2], uv[3]}, atlas->generation};
    if (e) {
      *e = n;
    }
    else {
      vec_push(&ic->entries, n);
      map_put(&ic->index, key, ic->entries.len);
      e = &vec_last(&ic->entries);
    }
  }
  f32 uv[4] = {e->uv[0], e->uv[1], e->uv[2], e->uv[3]};
  if (mirror) {
    uv[0] = e->uv[2], uv[2] = e->uv[0];
  }
  civue_rect to = rect_xywh(round(r.x0), round(r.y0), w, h);
  if (s->color) {
    civue_dl_color_glyph(d, to, uv, c.a);
  }
  else {
    civue_dl_mask(d, to, uv, c, 1);
  }
}
