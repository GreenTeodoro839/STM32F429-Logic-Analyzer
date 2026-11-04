#include "gfx.h"

#include <string.h>

// DMA-visible SRAM (the LCD is fed by DMA2)
uint16_t g_fb[GFX_H][GFX_W] __attribute__((section(".sram1"), aligned(4)));

static int s_dirty0 = GFX_H, s_dirty1 = 0;

void gfx_mark_dirty(int y0, int y1) {
  if (y0 < 0) y0 = 0;
  if (y1 > GFX_H) y1 = GFX_H;
  if (y0 >= y1) return;
  if (y0 < s_dirty0) s_dirty0 = y0;
  if (y1 > s_dirty1) s_dirty1 = y1;
}

bool gfx_take_dirty(unsigned *y0, unsigned *y1) {
  if (s_dirty0 >= s_dirty1) return false;
  *y0 = (unsigned)s_dirty0;
  *y1 = (unsigned)s_dirty1;
  s_dirty0 = GFX_H;
  s_dirty1 = 0;
  return true;
}

void gfx_fill(int x, int y, int w, int h, uint16_t c) {
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > GFX_W) w = GFX_W - x;
  if (y + h > GFX_H) h = GFX_H - y;
  if (w <= 0 || h <= 0) return;
  for (int j = y; j < y + h; j++) {
    uint16_t *p = &g_fb[j][x];
    for (int i = 0; i < w; i++) p[i] = c;
  }
  gfx_mark_dirty(y, y + h);
}

void gfx_hline(int x, int y, int w, uint16_t c) { gfx_fill(x, y, w, 1, c); }
void gfx_vline(int x, int y, int h, uint16_t c) { gfx_fill(x, y, 1, h, c); }

void gfx_rect(int x, int y, int w, int h, uint16_t c) {
  gfx_hline(x, y, w, c);
  gfx_hline(x, y + h - 1, w, c);
  gfx_vline(x, y, h, c);
  gfx_vline(x + w - 1, y, h, c);
}

// a = 0..15: bg -> fg
static uint16_t blend(uint16_t bg, uint16_t fg, unsigned a) {
  int br = bg >> 11, bgg = (bg >> 5) & 63, bb = bg & 31;
  int fr = fg >> 11, fgg = (fg >> 5) & 63, fb = fg & 31;
  int r = br + ((fr - br) * (int)a + 7) / 15;
  int g = bgg + ((fgg - bgg) * (int)a + 7) / 15;
  int b = bb + ((fb - bb) * (int)a + 7) / 15;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

int gfx_text_width(const font_t *f, const char *s) { return (int)strlen(s) * f->w; }

int gfx_text(int x, int y, const font_t *f, const char *s, uint16_t fg, uint16_t bg) {
  uint16_t lut[16];
  for (unsigned a = 0; a < 16; a++) lut[a] = blend(bg, fg, a);
  const unsigned gbytes = (unsigned)f->w * f->h / 2u;

  for (; *s; s++, x += f->w) {
    unsigned c = (uint8_t)*s;
    if (c < f->first || c > f->last) c = '?';
    const uint8_t *g = &f->bits[(c - f->first) * gbytes];
    for (int j = 0; j < f->h; j++) {
      int yy = y + j;
      if (yy < 0 || yy >= GFX_H) {
        g += f->w / 2;
        continue;
      }
      for (int i = 0; i < f->w; i += 2, g++) {
        int xx = x + i;
        if (xx >= 0 && xx < GFX_W) g_fb[yy][xx] = lut[*g >> 4];
        if (xx + 1 >= 0 && xx + 1 < GFX_W) g_fb[yy][xx + 1] = lut[*g & 15u];
      }
    }
  }
  gfx_mark_dirty(y, y + f->h);
  return x;
}

void gfx_text_box(int x, int y, int w, const font_t *f, const char *s, uint16_t fg, uint16_t bg) {
  char buf[64];
  int maxc = w / f->w;
  if (maxc > (int)sizeof buf - 1) maxc = (int)sizeof buf - 1;
  int n = 0;
  while (n < maxc && s[n]) {
    buf[n] = s[n];
    n++;
  }
  buf[n] = 0;
  int end = gfx_text(x, y, f, buf, fg, bg);
  gfx_fill(end, y, x + w - end, f->h, bg);
}
