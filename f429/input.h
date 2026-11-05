// Key events from the 4x4 keypad (via CH455), and from KEY1/KEY2 on J2 (if a
// carrier board has them).
//
// Keypad legend (KEY1..KEY16 of the schematic, row by row):
//     1 2 3 A
//     4 5 6 B
//     7 8 9 C
//     * 0 # D
// Events are the legend characters; the two board buttons give KEY_BTN1 and
// KEY_BTN2. Digits and C/D repeat while held.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define KEY_BTN1 0x81u
#define KEY_BTN2 0x82u

void input_init(void);
uint8_t input_poll(void);  // next key event, 0 if none
bool input_keypad_ok(void);
int input_last_code(void);  // last raw CH455 key code (for the key test), -1 none
// Cheap check while sampling (two GPIO reads): KEY2 or the keypad INT line.
bool input_cancel_requested(void);
// Drop queued events and treat keys still held as already handled (after a
// capture was aborted with a key).
void input_discard(void);
// For tests and remote control: queue a key event.
void input_inject(uint8_t key);
