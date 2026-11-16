#include "look.h"

#include <civue/dsl.h>

static Look light() {
  return (Look){
    .ground = rgb(0xf3f3f3),
    .panel = rgb(0xffffff),
    .raised = rgb(0xf7f7f7),
    .text = rgb(0x1e1e1e),
    .faint = rgba(0x0000008c),
    .accent = rgb(0x0d99ff),
    .line = rgba(0x0000001a),
    .thumb = rgba(0x00000059),
  };
}

static Look dark() {
  return (Look){
    .ground = rgb(0x1a1a1a),
    .panel = rgb(0x242424),
    .raised = rgb(0x2e2e2e),
    .text = rgb(0xf2f2f2),
    .faint = rgba(0xffffff8c),
    .accent = rgb(0x0c8ce9),
    .line = rgba(0xffffff1f),
    .thumb = rgba(0xffffff66),
  };
}

Look look_of(bool is_dark) {
  return is_dark ? dark() : light();
}
