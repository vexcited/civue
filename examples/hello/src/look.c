#include "look.h"

#include <civue/dsl.h>

static Look light() {
  return (Look){
    .ground = rgb(0xf5f5f5),
    .panel = rgb(0xffffff),
    .text = rgb(0x1e1e1e),
    .faint = rgba(0x00000080),
    .accent = rgb(0x0d99ff),
    .track = rgb(0xe6e6e6),
    .line = rgba(0x0000001a),
  };
}

static Look dark() {
  return (Look){
    .ground = rgb(0x1e1e1e),
    .panel = rgb(0x2c2c2c),
    .text = rgb(0xffffff),
    .faint = rgba(0xffffff80),
    .accent = rgb(0x0c8ce9),
    .track = rgb(0x444444),
    .line = rgba(0xffffff33),
  };
}

Look look_of(bool is_dark) {
  return is_dark ? dark() : light();
}
