// Shared device status, shown on the OLED.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  LA_IDLE,
  LA_WAIT_TRIGGER,
  LA_CAPTURING,
  LA_SENDING,
  LA_DONE,
  LA_ABORTED,
  LA_STREAMING,
  LA_STREAM_DONE,
  LA_STREAM_OVERFLOW,
} la_state_t;

typedef struct {
  la_state_t state;
  uint32_t rate_hz;    // actual sample rate of the last configuration
  bool rate_exact;     // false: PulseView's time axis will be slightly off
  uint32_t samples;    // window length
  uint32_t pre;        // pre-trigger samples
  uint8_t num_stages;  // 0 = no trigger
  uint8_t mask0, value0;
  bool overrun;        // last capture lost samples
  bool rle_requested;  // host asked for RLE (unsupported, CH7 dropped)
  uint32_t captures;   // completed captures since boot
  bool streaming;      // last session used the streaming protocol
  uint8_t stream_channels;
  uint64_t stream_samples;  // samples sent in the last streaming session
} la_status_t;

extern la_status_t g_la;
