// LCD user interface (240x240) driven by the 4x4 keypad (see input.h).
// Pages: waveform (status, live levels, last capture with cursors), capture
// setup (standalone capture), meter, decode, generator, pins, info; A opens
// the menu, B runs a standalone capture, KEY1 steps through the pages.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "capture.h"

void ui_init(void);              // first frame, then the display is switched on
void ui_levels(uint8_t sample);  // accumulate live input levels (call often)
void ui_poll(void);              // keys, live levels, background jobs, LCD refresh
void ui_redraw_status(void);     // state changed: redraw and send now (blocking)
// A standalone capture was requested on the keypad: fills *cfg.
bool ui_take_capture(capture_cfg_t *cfg);
