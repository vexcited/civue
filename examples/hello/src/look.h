#ifndef HELLO_LOOK_H
#define HELLO_LOOK_H

#include <civue/civue.h>

// the example's own look: civue paints nothing of its own.
typedef struct {
  civue_color ground;
  civue_color panel;
  civue_color text;
  civue_color faint;
  civue_color accent;
  civue_color track;
  civue_color line;
} Look;

Look look_of(bool is_dark);

#endif
