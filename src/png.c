#include "internal.h"

#include <civue/system.h>

static const u8 SIGNATURE[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

static u32 crc_of(const u8 *b, usize n, u32 crc) {
  static u32 table[256];

  if (!table[1]) {
    for (u32 i = 0; i < 256; i++) {
      u32 c = i;

      for (u32 k = 0; k < 8; k++) {
        c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
      }

      table[i] = c;
    }
  }

  for (usize i = 0; i < n; i++) {
    crc = table[(crc ^ b[i]) & 0xff] ^ (crc >> 8);
  }

  return crc;
}

static void put_be32(civue_bytes *out, u32 v) {
  u8 b[4] = {(u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v};

  buf_put(out, b, 4);
}

static void chunk(civue_bytes *out, const char *type, const u8 *data,
                  usize len) {
  put_be32(out, (u32)len);

  usize at = out->len;

  buf_put(out, type, 4);

  if (len) {
    buf_put(out, data, len);
  }

  put_be32(out, ~crc_of(out->v + at, len + 4, ~0u));
}

// premultiplied rgba8 as png: rows unfiltered, in stored deflate blocks.
// large, but what a clipboard holds for a moment.
static void encode(const u8 *rgba, u32 w, u32 h, civue_bytes *out) {
  usize row = (usize)w * 4 + 1;
  usize raw = row * h;
  usize blocks = raw / 65535 + 1;
  civue_bytes z = {};
  u32 a = 1, b = 0;

  vec_reserve(&z, (u32)(raw + blocks * 5 + 6));
  buf_put(&z, (u8[]){0x78, 0x01}, 2);

  u8 *line = xmalloc(row);
  usize left = raw, in_block = 0;

  for (u32 y = 0; y < h; y++) {
    const u8 *from = rgba + (usize)y * w * 4;

    line[0] = 0;

    for (u32 x = 0; x < w * 4; x += 4) {
      u32 alpha = from[x + 3];

      for (u32 c = 0; c < 3; c++) {
        u32 v = alpha ? (from[x + c] * 255u + alpha / 2) / alpha : 0;

        line[1 + x + c] = (u8)(v > 255 ? 255 : v);
      }

      line[1 + x + 3] = (u8)alpha;
    }

    for (usize i = 0; i < row;) {
      if (in_block == 0) {
        u16 len = (u16)(left < 65535 ? left : 65535);

        buf_put(&z,
                (u8[]){left == len, (u8)len, (u8)(len >> 8), (u8)~len,
                       (u8)(~len >> 8)},
                5);
        in_block = len;
      }

      usize take = row - i < in_block ? row - i : in_block;

      buf_put(&z, line + i, take);

      for (usize k = 0; k < take; k++) {
        a = (a + line[i + k]) % 65521;
        b = (b + a) % 65521;
      }

      i += take;
      in_block -= take;
      left -= take;
    }
  }

  free(line);
  put_be32(&z, b << 16 | a);

  u8 header[13] = {(u8)(w >> 24), (u8)(w >> 16), (u8)(w >> 8), (u8)w,
                   (u8)(h >> 24), (u8)(h >> 16), (u8)(h >> 8), (u8)h,
                   8,             6,             0,            0,
                   0};

  buf_put(out, SIGNATURE, sizeof SIGNATURE);
  chunk(out, "IHDR", header, sizeof header);
  chunk(out, "IDAT", z.v, z.len);
  chunk(out, "IEND", nullptr, 0);
  vec_free(&z);
}

bool civue_clipboard_set_image(const uint8_t *bytes, size_t n) {
  if (n >= sizeof SIGNATURE && !memcmp(bytes, SIGNATURE, sizeof SIGNATURE)) {
    civue_clipboard_set_png(bytes, n);
    return true;
  }

  u32 w = 0, h = 0;
  u8 *rgba = civue__image_decode(bytes, n, 0, &w, &h);

  if (!rgba || !w || !h) {
    free(rgba);
    return false;
  }

  civue_bytes png = {};

  encode(rgba, w, h, &png);
  free(rgba);
  civue_clipboard_set_png(png.v, png.len);
  vec_free(&png);
  return true;
}
