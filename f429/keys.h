// KEY1 (PG6) / KEY2 (PG7), debounced press events.
#pragma once

#include <stdint.h>

#define KEY_1 1u
#define KEY_2 2u

void keys_init(void);
uint8_t keys_poll(void);  // bit mask of keys pressed since the last call
void keys_sync(void);     // forget presses that happened while not polled
