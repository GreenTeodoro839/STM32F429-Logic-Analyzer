// SSD1306 128x64 OLED on I2C1 (PB6/PB7). No frame buffer: callers render one
// 8-pixel-high page (128 bytes, bit0 = top row) at a time and push it.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define OLED_W      128
#define OLED_PAGES  8
#define OLED_COLS   21  // text columns with the 6x8 font

bool oled_init(void);  // true if the display acknowledged
bool oled_present(void);
bool oled_write_page(uint8_t page, const uint8_t buf[OLED_W]);

// Render a string (6x8 font) into a 128-byte page buffer.
void oled_render_text(uint8_t buf[OLED_W], const char *s, bool invert);
