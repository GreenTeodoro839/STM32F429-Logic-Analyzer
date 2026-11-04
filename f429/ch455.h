// CH455H: 4-digit 7-segment display + 4x4 key matrix, on a 2-wire bus
// (SCL PC14, SDA PC15, INT# PC13, 10k pull-ups), driven by bit-banging.
//
// Keys: KEYn sits on row SEGr, column DIGc with n = 4r + c + 1 (schematic).
// Display: SEG0..SEG7 = segments A..G, DP; DIG0..DIG3 = the four digits.
#pragma once

#include <stdbool.h>
#include <stdint.h>

bool ch455_init(void);  // true if the chip acknowledged
bool ch455_present(void);

// Raw segment patterns for the four digits (bit0 = A ... bit6 = G, bit7 = DP).
bool ch455_digits(const uint8_t seg[4]);
// Up to 4 characters ('0'-'9', 'A'-'Z' as far as 7 segments allow, '-', ' ');
// a '.' after a character lights that digit's decimal point.
bool ch455_text(const char *s);
void ch455_brightness(unsigned level);  // 1..8

// Key code of the CH455 (bit 6 = currently pressed, bits 5:3 = row, bits 1:0
// = column), or -1 on a bus error.
int ch455_read_key(void);
bool ch455_int_active(void);  // INT# low: a key event is waiting
