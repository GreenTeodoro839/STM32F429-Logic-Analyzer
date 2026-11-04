// Signal analysis over captured samples: edge search and per-channel
// statistics (frequency, duty cycle, pulse widths). Used by the waveform
// cursors, the meter and the protocol decoders.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "capture_hw.h"

#define ANA_NONE 0xFFFFFFFFu

// --- Window access (indices relative to the window start) -------------------

uint8_t ana_sample(const capture_window_t *w, uint32_t i);
// First j in [i, end) with (sample[j] & mask) != ref, or end.
uint32_t ana_next_change(const capture_window_t *w, uint32_t i, uint32_t end, uint8_t mask, uint8_t ref);
// Last j in [begin, i) with (sample[j] & mask) != ref, or ANA_NONE.
uint32_t ana_prev_change(const capture_window_t *w, uint32_t begin, uint32_t i, uint8_t mask, uint8_t ref);
// Next / previous edge of channel ch relative to sample i: the index of the
// first sample after the edge, or ANA_NONE.
uint32_t ana_next_edge(const capture_window_t *w, uint32_t i, unsigned ch);
uint32_t ana_prev_edge(const capture_window_t *w, uint32_t i, unsigned ch);

// --- Statistics --------------------------------------------------------------

typedef struct {
  uint32_t rises, falls;
  uint64_t first_rise, last_rise;  // sample positions
  uint64_t high;                   // samples spent high
  uint32_t min_high, min_low;      // shortest complete pulses, ANA_NONE = none seen
  uint32_t max_high, max_low;
} ana_chan_t;

typedef struct {
  ana_chan_t ch[8];
  uint64_t n;     // samples covered
  uint8_t first;  // first sample (levels at the start)
  // running state
  uint8_t prev;
  uint8_t seen;          // channels that had an edge (their `since` is an edge)
  uint64_t since[8];     // position of the last edge or of the start
} ana_stats_t;

void ana_stats_reset(ana_stats_t *s, uint8_t first_sample);
// Feed the next n samples (contiguous in memory) in time order.
void ana_stats_feed(ana_stats_t *s, const uint8_t *p, uint32_t n);
void ana_stats_finish(ana_stats_t *s);  // account for the time after the last edges

// Results for channel ch at sample rate `rate_hz`.
// Frequency in millihertz from the first to the last rising edge (0 if fewer
// than two rising edges); duty in 0.1 %; level: 0 low, 1 high, 2 toggling.
uint64_t ana_freq_mhz(const ana_stats_t *s, unsigned ch, uint32_t rate_hz);
unsigned ana_duty_permille(const ana_stats_t *s, unsigned ch);
uint32_t ana_min_pulse(const ana_stats_t *s, unsigned ch);  // samples, ANA_NONE if none

// Incremental statistics over a window range, for the UI (bounded work per call).
typedef struct {
  capture_window_t win;
  uint32_t pos, end;  // window indices still to do
  ana_stats_t st;
  bool active, done;
} ana_job_t;

void ana_job_start(ana_job_t *j, const capture_window_t *w, uint32_t from, uint32_t count);
bool ana_job_run(ana_job_t *j, uint32_t max_samples);  // true when done
