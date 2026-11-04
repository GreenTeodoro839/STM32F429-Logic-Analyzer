#pragma once

#include <stdint.h>

// Classic 5x7 ASCII font, one byte per column, bit0 = top row.
// Rendered in a 6x8 cell (one blank column between glyphs).
#define FONT6X8_FIRST ' '
#define FONT6X8_LAST  '~'

extern const uint8_t font6x8[FONT6X8_LAST - FONT6X8_FIRST + 1][5];
