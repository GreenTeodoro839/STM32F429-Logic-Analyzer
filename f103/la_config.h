// LA-F103 limits and names used by the shared protocol code (common/).
#pragma once

#define LA_FW_NAME    "LA-F103"
#define LA_FW_VERSION "0.1"
#define LA_NUM_CH     8

// Sample buffer: 16 KB ring in SRAM, TIM1 at 72 MHz.
#define CAPTURE_BUF_SIZE    (16u * 1024u)
#define CAPTURE_MARGIN      256u  // slack for post-trigger stop latency
#define CAPTURE_MAX_SAMPLES (CAPTURE_BUF_SIZE - CAPTURE_MARGIN)
#define CAPTURE_MIN_TICKS   18u   // 72 MHz / 18 = 4 MHz
#define CAPTURE_MAX_RATE_HZ 4000000u
#define CAPTURE_MAX_STAGES  4u

// Streaming ring (common/stream.c): the same buffer. CAPTURE_GUARD = samples
// the DMA may already have stored beyond capture_written().
#define CAPTURE_RING_SIZE() CAPTURE_BUF_SIZE
#define CAPTURE_GUARD       0u

// Reply to a sigrok-pico rate the timer cannot produce exactly.
#define LA_RATE_ERROR "LA-F103: rate must be <=4MHz"
