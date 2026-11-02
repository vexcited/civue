#include <civue/base.h>

#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) ((x) ? (void)0 : (fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x), exit(1)))

int main(void) {
  const char *s = "aé中\U0001F44B";
  size_t n = strlen(s), at = 0;
  uint32_t want[] = {'a', 0xe9, 0x4e2d, 0x1f44b};
  for (int i = 0; i < 4; i++) {
    uint32_t cp = civue_utf8_next(s, n, &at);
    CHECK(cp == want[i]);
    char out[4];
    size_t k = civue_utf8_put(out, cp);
    CHECK(!memcmp(out, s + at - k, k));
  }
  CHECK(at == n);
  size_t bad = 0;
  CHECK(civue_utf8_next("\xff", 1, &bad) == 0xfffd && bad == 1);

  CHECK(civue_hash_str("civue") == civue_hash("civue", 5));
  CHECK(civue_hash_str("civue") != civue_hash_str("civuf"));

  civue_color c = civue_hex(0x0d99ff);
  CHECK(c.r == 13 / 255.0f && c.g == 153 / 255.0f && c.b == 1 && c.a == 1);
  CHECK(civue_hexa(0xffffff80).a == 128 / 255.0f);

  civue_rect r = civue_rect_xywh(10, 20, 30, 40);
  CHECK(civue_rect_w(r) == 30 && civue_rect_h(r) == 40);
  CHECK(civue_rect_contains(r, civue_v2(10, 20)) && !civue_rect_contains(r, civue_v2(40, 20)));
  CHECK(civue_rect_empty(civue_rect_intersect(r, civue_rect_xywh(50, 0, 10, 10))));

  double t = civue_now();
  CHECK(t > 0 && civue_now() >= t);
  puts("base: ok");
  return 0;
}
