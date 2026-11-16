#ifndef LAYOUT_LOOK_H
#define LAYOUT_LOOK_H

#include <civue/civue.h>

typedef struct {
  civue_color ground;
  civue_color panel;
  civue_color raised;
  civue_color text;
  civue_color faint;
  civue_color accent;
  civue_color line;
  civue_color thumb;
} Look;

Look look_of(bool is_dark);

#endif
