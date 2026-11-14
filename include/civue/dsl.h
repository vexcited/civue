#ifndef CIVUE_DSL_H
#define CIVUE_DSL_H

#include "civue.h"

// short names, <civue/dsl_end.h> takes them back. none takes the ui: they
// work on the one whose frame the thread builds, or on CIVUE_DSL_UI when the
// app defines it, like `ui` for a variable of its own. the style names of div
// and text, gpui's, are only names inside them.

#ifndef CIVUE_DSL_UI
#define CIVUE_DSL_UI civue__ui
#endif

#define id(name) civue_id_of(name)
#define id_in(parent, name) civue_ids(parent, name)
#define id_at(parent, n) civue_idn(parent, n)
#define rgb(c) civue_hex(c)
#define rgba(c) civue_hexa(c)
#define point(x, y) civue_v2(x, y)
#define xywh(x, y, w, h) civue_rect_xywh(x, y, w, h)

#define clipped(r) CIVUE_CLIPPED(CIVUE_DSL_UI, r)
#define overlay() CIVUE_OVERLAY(CIVUE_DSL_UI)
#define scrolling(id, r, height, content) \
  CIVUE_SCROLLING(CIVUE_DSL_UI, id, r, height, content)

#define fill(...) civue_fill(CIVUE_DSL_UI, __VA_ARGS__)
#define stroke(...) civue_stroke(CIVUE_DSL_UI, __VA_ARGS__)
#define label(...) civue_text(CIVUE_DSL_UI, __VA_ARGS__)
#define tooltip(...) civue_tooltip(CIVUE_DSL_UI, __VA_ARGS__)
#define tooltip_at(...) civue_tooltip_at(CIVUE_DSL_UI, __VA_ARGS__)

#define div(...) CIVUE_DIV(CIVUE_DSL_UI, CIVUE__STYLES(__VA_ARGS__))
#define text(s, ...)               \
  civue_node_text(CIVUE_DSL_UI, s, \
                  &CIVUE_NODE_STYLE(CIVUE__STYLES(__VA_ARGS__)))
// an icon of any iconify set, like icon(mdi, account, size(5)), dashes in
// its name written as underscores, fetched when the app builds.
#define icon(set, name, ...)                                \
  civue_node_icon(CIVUE_DSL_UI, CIVUE_ICON_##set##__##name, \
                  &CIVUE_NODE_STYLE(CIVUE__STYLES(__VA_ARGS__)))
#define here() civue_node_rect(CIVUE_DSL_UI)
#define content() civue_node_content(CIVUE_DSL_UI)
#define last_size(id) civue_node_last_size(CIVUE_DSL_UI, id)
#define button() civue_node_button(CIVUE_DSL_UI)
#define toggle(on) civue_node_toggle(CIVUE_DSL_UI, on)
#define slider(...) civue_node_slider(CIVUE_DSL_UI, __VA_ARGS__)
#define text_field(...) civue_node_text_field(CIVUE_DSL_UI, __VA_ARGS__)

#endif
