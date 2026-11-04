// Sampling engine interface, implemented per board in <board>/capture.c.
// A timer paces a DMA stream that copies the input port into a RAM ring
// buffer. Triggers are detected by the CPU scanning the samples behind the
// DMA write pointer. Board limits (CAPTURE_MAX_SAMPLES, ...) come from the
// board's la_config.h.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "la_config.h"

typedef struct {
  uint32_t divider;      // SUMP divider: rate = 100 MHz / (divider + 1)
  uint32_t total;        // samples to capture (window length)
  uint32_t pre;          // samples before the trigger point
  uint8_t num_stages;    // 0 = start immediately
  uint8_t mask[CAPTURE_MAX_STAGES];
  uint8_t value[CAPTURE_MAX_STAGES];
} capture_cfg_t;

typedef enum {
  CAPTURE_OK,
  CAPTURE_ABORTED,
  CAPTURE_OVERRUN,  // trigger scanner fell behind; data may be incomplete
} capture_result_t;

typedef struct {
  uint16_t psc, arr;  // sampling timer prescaler / auto-reload
  uint32_t ticks;     // timer ticks per sample = (psc+1)*(arr+1)
  uint32_t rate_hz;   // actual sample rate (rounded)
  bool exact;         // true if the requested rate is reproduced exactly
} capture_timing_t;

// Called periodically while capturing; return true to abort.
typedef bool (*capture_poll_fn)(void);

// Timing for a SUMP divider (rate = 100 MHz / (divider + 1)).
capture_timing_t capture_timing(uint32_t divider);
// Timing for a rate in Hz; .exact is false if the rate is not realisable.
capture_timing_t capture_timing_hz(uint32_t rate_hz);

// Snapshot capture. Trigger stage 0 fires when its condition *becomes* true
// (edge semantics); later stages are level conditions, as in OLS hardware.
// On success *start is the buffer index of the oldest sample of the window;
// read samples with capture_sample(*start + i), i < cfg->total.
capture_result_t capture_run(const capture_cfg_t *cfg, capture_poll_fn poll, uint32_t *start);
uint8_t capture_sample(uint32_t index);

// Free-running circular sampling for streaming into a ring of
// CAPTURE_RING_SIZE() samples.
void capture_stream_begin(const capture_timing_t *t);
uint64_t capture_written(void);  // samples safely in RAM since begin (never wraps)
// How far the stream encoder should read: capture_written(), or less if
// reading right behind the DMA is slow on this board.
uint64_t capture_stream_readable(void);
void capture_stream_end(void);

// Where sample `index` (counted since begin, taken modulo the ring) is stored,
// and how many samples from there on are contiguous in memory (>= 1). The
// ring need not be one contiguous block (the F429 spreads it over SDRAM banks).
const uint8_t *capture_span(uint64_t index, uint32_t *contiguous);
