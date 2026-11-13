// BTL Print Bridge - state for decoding PNG shipping labels (see "picture labels" in btl-printer.ino).
#pragma once
#include <stdint.h>

static const int LBL_W = 812, LBL_H = 1218, LBL_ROWB = (LBL_W + 7) / 8;  // 4x6 in at 203 dpi

struct PngDec {
  uint32_t w = 0, h = 0; uint8_t depth = 0, ctype = 0, ch = 0;
  uint32_t stride = 0, bpp = 1;
  uint8_t palDark[256] = {};
  uint8_t *cur = nullptr, *prev = nullptr;  // filter byte + row
  uint32_t fill = 0, y = 0;
  bool rot = false; float sc = 1; int tw = 0, th = 0, ox = 0, oy = 0;
  int16_t *sx0 = nullptr, *sx1 = nullptr, *sy0 = nullptr, *sy1 = nullptr;
  int p0 = 0, p1 = 0;                        // next target row/col for each sample set
  uint8_t *out = nullptr;                    // LBL_ROWB * LBL_H, 1 = black
};
