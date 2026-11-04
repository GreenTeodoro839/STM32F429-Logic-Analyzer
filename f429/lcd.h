// 1.3" 240x240 IPS LCD, ST7789VW, write-only 4-wire SPI on SPI5 (mode 3).
// Pixels are RGB565 in native uint16_t order: SPI runs in 16-bit frames
// (MSB first) while sending pixel data.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define LCD_W 240
#define LCD_H 240

// Orientation. The panel's vendor demo used MADCTL 0xC0 (MY|MX, 80-row
// offset into the 320-row GRAM), which is upside down on this board; 0x00
// turns it by 180 degrees and needs no offset.
#define LCD_MADCTL     0x00u
#define LCD_ROW_OFFSET 0u
#define LCD_COL_OFFSET 0u

void lcd_init(void);        // reset + register setup; the display stays off
void lcd_display_on(void);  // call after the first frame has been sent
// Start sending rows [y0, y1) of a 240-pixel-wide frame buffer by DMA.
// Waits for any previous transfer first.
void lcd_flush_rows(const uint16_t *fb, unsigned y0, unsigned y1);
bool lcd_busy(void);
void lcd_wait(void);  // block until the last transfer has finished
