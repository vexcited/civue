#include "../scenes.h"

#include <math.h>
#include <stdlib.h>

#include <civue/dsl.h>

#include "../words.h"

constexpr int PARAGRAPHS = 40;
constexpr int WORDS_EACH = 70;

static void write_paragraphs(Bench *b) {
  for (int i = 0; i < PARAGRAPHS; i++) {
    b->paragraphs[i] = malloc(1024);
    words(b->paragraphs[i], 1024, (unsigned)i + 1, WORDS_EACH);
  }
}

void document_scene(Bench *b, civue_ui *ui) {
  if (!b->paragraphs[0]) {
    write_paragraphs(b);
  }

  if (recorder_frame(&b->recorder, ui)) {
    b->frame++;
  }

  float width = roundf(600 + 150 * sinf((float)b->frame / 30));

  div(size(full), overflow_y_scroll, text_size(px(13)), line_height(px(18)),
      text_color(rgb(0x1e1e1e))) {
    div(flex, flex_col, gap(2), w(px(width)), p(3)) {
      for (int i = 0; i < PARAGRAPHS; i++) {
        text(b->paragraphs[i]);
      }
    }
  }
}
