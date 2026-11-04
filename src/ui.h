// OLED user interface. Pages (8 px rows):
//   0 title / USB status     4 channel numbers
//   1 state                  5-6 live level boxes (logic probe)
//   2 sample rate / depth    7 hints / warnings
//   3 trigger
#pragma once

#include <stdint.h>

void ui_init(void);
void ui_levels(uint8_t sample);  // accumulate live input levels (call often)
void ui_poll(void);              // incremental redraw, at most one page per call
void ui_redraw_status(void);     // redraw the status pages now
