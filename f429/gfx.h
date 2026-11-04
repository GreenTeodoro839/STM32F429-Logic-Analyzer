// Drawing into the 240x240 RGB565 frame buffer. Drawing marks rows dirty;
// the UI sends dirty rows to the LCD when the bus is free.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "font.h"

#define GFX_W 240
#define GFX_H 240

#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | ((b) >> 3)))

extern uint16_t g_fb[GFX_H][GFX_W];

void gfx_fill(int x, int y, int w, int h, uint16_t c);  // clipped to the screen
void gfx_hline(int x, int y, int w, uint16_t c);
void gfx_vline(int x, int y, int h, uint16_t c);
void gfx_rect(int x, int y, int w, int h, uint16_t c);  // outline

// Draw text (cells filled with bg). Returns the x after the last character.
int gfx_text(int x, int y, const font_t *f, const char *s, uint16_t fg, uint16_t bg);
int gfx_text_width(const font_t *f, const char *s);
// Draw text in the box [x, x+w) and clear the rest of the box with bg.
void gfx_text_box(int x, int y, int w, const font_t *f, const char *s, uint16_t fg, uint16_t bg);

// Dirty rows since the last call: returns false if nothing changed.
bool gfx_take_dirty(unsigned *y0, unsigned *y1);
void gfx_mark_dirty(int y0, int y1);  // rows [y0, y1)
