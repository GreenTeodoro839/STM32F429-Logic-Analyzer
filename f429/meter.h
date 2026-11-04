// Live frequency meter: samples all 8 inputs at 10 MHz in the background
// and publishes per-channel statistics every gate period (~0.5 s).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "analyze.h"

#define METER_RATE_HZ 10000000u

typedef struct {
  ana_stats_t st;     // frequencies, duty and pulses over the measured time
  uint32_t rate_hz;
  uint32_t gates;     // gates published since start
  unsigned coverage;  // share of the gate actually measured, 0.1 % (busy signals are sampled)
} meter_result_t;

void meter_start(void);
void meter_stop(void);
bool meter_running(void);
// Copy the latest result if it is newer than *seq (updates *seq); false if
// nothing new.
bool meter_get(meter_result_t *out, uint32_t *seq);
