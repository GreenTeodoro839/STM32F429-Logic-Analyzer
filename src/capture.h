// Sampling engine: TIM1 update events trigger DMA1 channel 5, which copies the
// low byte of GPIOA->IDR (PA0..PA7) into a RAM buffer. Triggers are detected by
// the CPU scanning the samples behind the DMA write pointer.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CAPTURE_BUF_SIZE    (16u * 1024u)
#define CAPTURE_MARGIN      256u  // slack for post-trigger stop latency
#define CAPTURE_MAX_SAMPLES (CAPTURE_BUF_SIZE - CAPTURE_MARGIN)
#define CAPTURE_MIN_TICKS   18u   // 72 MHz / 18 = 4 MHz
#define CAPTURE_MAX_RATE_HZ 4000000u
#define CAPTURE_MAX_STAGES  4u

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
  uint16_t psc, arr;  // TIM1 prescaler / auto-reload
  uint32_t ticks;     // timer ticks per sample = (psc+1)*(arr+1)
  uint32_t rate_hz;   // actual sample rate (rounded)
  bool exact;         // true if the requested rate is reproduced exactly
} capture_timing_t;

// Called periodically while capturing; return true to abort.
typedef bool (*capture_poll_fn)(void);

// Timing for a SUMP divider (rate = 100 MHz / (divider + 1)).
capture_timing_t capture_timing(uint32_t divider);
// Timing for a rate in Hz; .exact is false if 72 MHz / rate is not realisable.
capture_timing_t capture_timing_hz(uint32_t rate_hz);

// Snapshot capture. Trigger stage 0 fires when its condition *becomes* true
// (edge semantics); later stages are level conditions, as in OLS hardware.
// On success *start is the buffer index of the oldest sample of the window;
// read samples with capture_sample(*start + i), i < cfg->total.
capture_result_t capture_run(const capture_cfg_t *cfg, capture_poll_fn poll, uint32_t *start);
uint8_t capture_sample(uint32_t index);

// Free-running circular sampling for streaming.
void capture_stream_begin(const capture_timing_t *t);
uint64_t capture_written(void);  // samples written since begin (never wraps)
const uint8_t *capture_buffer(void);
void capture_stream_end(void);
