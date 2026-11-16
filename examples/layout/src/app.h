#ifndef LAYOUT_APP_H
#define LAYOUT_APP_H

#include "look.h"

typedef struct {
  bool dark;
  Look look;
  civue_scrollbar_look bars;
} App;

#endif
