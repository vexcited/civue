#include "internal.h"

#include <civue/icons.h>

typedef struct {
  f32 uv[4];
  u64 generation;
} Entry;

// a width and a height read from an svg's header once: bit 0 width written, 1 height written, 2 both read, 3
// cached.
typedef struct {
  f32 size[2];
  u8 known;
} Natural;

struct civue_icons {
  // icon, width and height to entry + 1.
  civue_map index;
  Vec(Entry) entries;
  const civue_icon_svg *svgs;
  u32 count;
  // the natural sizes of the app's icons, then of the built in ones.
  Natural *natural;
};

static const char caret_down[] = {
#embed "../assets/icons/caret-down.svg"
};
static const char caret_up[] = {
#embed "../assets/icons/caret-up.svg"
};
static const char caret_right[] = {
#embed "../assets/icons/caret-right.svg"
};
static const char check[] = {
#embed "../assets/icons/check.svg"
};
static const char close_[] = {
#embed "../assets/icons/close.svg"
};
static const char spinner[] = {
#embed "../assets/icons/spinner.svg"
};
static const char tooltip_arrow[] = {
#embed "../assets/icons/tooltip-arrow.svg"
};
static const char tooltip_arrow_left[] = {
#embed "../assets/icons/tooltip-arrow-left.svg"
};
static const char tooltip_arrow_up[] = {
#embed "../assets/icons/tooltip-arrow-up.svg"
};

// in the order of their numbers from CIVUE_ICON_BUILTIN.
static const civue_icon_svg builtin[] = {
    {caret_down, sizeof caret_down},
    {caret_up, sizeof caret_up},
    {caret_right, sizeof caret_right},
    {check, sizeof check},
    {close_, sizeof close_},
    {spinner, sizeof spinner},
    {tooltip_arrow, sizeof tooltip_arrow},
    {tooltip_arrow_left, sizeof tooltip_arrow_left},
    {tooltip_arrow_up, sizeof tooltip_arrow_up},
};
static_assert(countof(builtin) == CIVUE_ICON_TOOLTIP_ARROW_UP - CIVUE_ICON_BUILTIN + 1);

civue_icons *civue_icons_new(void) {
  civue_icons *ic = own_new(sizeof(civue_icons));
  ic->natural = own_new(countof(builtin) * sizeof(Natural));
  return ic;
}

void civue_icons_use(civue_icons *ic, const civue_icon_svg *svgs, uint32_t count) {
  own_free(ic->natural);
  ic->svgs = svgs;
  ic->count = count;
  ic->natural = own_new((count + countof(builtin)) * sizeof(Natural));
  map_clear(&ic->index);
  ic->entries.len = 0;
}

void civue_icons_free(civue_icons *ic) {
  if (!ic) return;
  map_free(&ic->index);
  vec_free(&ic->entries);
  own_free(ic->natural);
  own_free(ic);
}

// the svg of an icon and where its natural size is kept, nullptr for an icon nobody registered.
static const civue_icon_svg *svg_of(civue_icons *ic, civue_icon icon, Natural **natural) {
  u32 at;
  const civue_icon_svg *s = nullptr;
  if (icon >= CIVUE_ICON_BUILTIN) {
    at = icon - CIVUE_ICON_BUILTIN;
    if (at < countof(builtin)) s = &builtin[at];
    at += ic->count;
  } else {
    at = icon;
    if (at < ic->count) s = &ic->svgs[at];
  }
  if (natural) *natural = s ? &ic->natural[at] : nullptr;
  return s;
}

static bool attr(const char *tag, usize n, const char *name, f32 *out) {
  usize k = strlen(name);
  for (usize i = 0; i + k + 3 < n; i++) {
    if (tag[i] != ' ' || memcmp(tag + i + 1, name, k) || tag[i + 1 + k] != '=' || tag[i + 2 + k] != '"') continue;
    char *end;
    *out = strtof(tag + i + 3 + k, &end);
    return end != tag + i + 3 + k;
  }
  return false;
}

bool civue_icon_natural_size(civue_icons *ic, civue_icon icon, float *w, float *h) {
  Natural *nat;
  const civue_icon_svg *s = svg_of(ic, icon, &nat);
  if (!s) return false;
  // read once from the svg header: asked for every icon of every frame.
  if (!nat->known) {
    const char *close = memchr(s->svg, '>', s->len);
    usize n = close ? (usize)(close - s->svg) : 0;
    f32 *v = nat->size;
    v[0] = v[1] = NAN;
    bool ok = attr(s->svg, n, "width", &v[0]) && attr(s->svg, n, "height", &v[1]);
    nat->known = 8 | !isnan(v[0]) | !isnan(v[1]) << 1 | ok << 2;
  }
  // what the svg lacks keeps the caller's default.
  if (nat->known & 1) *w = nat->size[0];
  if (nat->known & 2) *h = nat->size[1];
  return nat->known & 4;
}

void civue_icons_draw(civue_icons *ic, civue_draw_list *d, civue_atlas *atlas, civue_icon icon, civue_rect r,
                      civue_color c, bool mirror) {
  const civue_icon_svg *s = svg_of(ic, icon, nullptr);
  if (!s) return;
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
    // the tint comes from the color, only coverage is kept.
    u8 *mask = cilo_malloc((usize)w * h);
    for (usize i = 0; i < (usize)w * h; i++) mask[i] = rgba.v[i * 4 + 3];
    vec_free(&rgba);
    f32 uv[4];
    bool ok = civue_atlas_insert(atlas, w, h, mask, uv);
    if (!ok) {
      civue_atlas_clear(atlas);
      ok = civue_atlas_insert(atlas, w, h, mask, uv);
    }
    cilo_mfree(mask);
    if (!ok) return;
    Entry n = {{uv[0], uv[1], uv[2], uv[3]}, atlas->generation};
    if (e)
      *e = n;
    else {
      vec_push(&ic->entries, n);
      map_put(&ic->index, key, ic->entries.len);
      e = &vec_last(&ic->entries);
    }
  }
  f32 uv[4] = {e->uv[0], e->uv[1], e->uv[2], e->uv[3]};
  if (mirror) uv[0] = e->uv[2], uv[2] = e->uv[0];
  civue_dl_mask(d, rect_xywh(round(r.x0), round(r.y0), w, h), uv, c, 1);
}
