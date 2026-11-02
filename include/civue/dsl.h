#ifndef CIVUE_DSL_H
#define CIVUE_DSL_H

#include "civue.h"

#define id(name) civue_id_of(name)
#define id_in(parent, name) civue_ids(parent, name)
#define id_at(parent, n) civue_idn(parent, n)
#define rgb(c) civue_hex(c)
#define rgba(c) civue_hexa(c)
#define pt(x, y) civue_v2(x, y)
#define xywh(x, y, w, h) civue_rect_xywh(x, y, w, h)

#define clipped CIVUE_CLIPPED
#define overlay CIVUE_OVERLAY
#define scrolling CIVUE_SCROLLING

#define fill(...) civue_fill(__VA_ARGS__)
#define label(...) civue_text(__VA_ARGS__)
#define button(...) civue_button(__VA_ARGS__)
#define checkbox(...) civue_checkbox(__VA_ARGS__)
#define slider(...) civue_slider(__VA_ARGS__)
#define text_field(...) civue_text_field(__VA_ARGS__)
#define tooltip(...) civue_tooltip(__VA_ARGS__)
#define spinner(...) civue_spinner(__VA_ARGS__)

#endif
