// LCD user interface (240x240). Pages, switched with KEY1:
//   main  title / USB, state, rate + depth, trigger, then one row per channel
//         with its live level and the waveform of the last capture
//         (KEY2 zooms in around the trigger)
//   pins  channel pin-out and test signals
//   info  firmware, clocks, SDRAM and DMA self-test results
#pragma once

#include <stdint.h>

void ui_init(void);              // first frame, then the display is switched on
void ui_levels(uint8_t sample);  // accumulate live input levels (call often)
void ui_poll(void);              // keys, live levels, preview, LCD refresh
void ui_redraw_status(void);     // state changed: redraw and send now (blocking)
