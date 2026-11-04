// LA-F429 limits and names used by the shared protocol code (common/).
#pragma once

#include <stdint.h>

#define LA_FW_NAME    "LA-F429"
#define LA_FW_VERSION "0.1"
#define LA_NUM_CH     8

// TIM8 runs at 120 MHz. SUMP rates 100 MHz / (n+1) are exact when (n+1) is
// a multiple of 5, which includes every 1-2-5 step up to 20 MHz; the
// sigrok-pico rate list was made for a 120 MHz clock and is exact as well.
#define CAPTURE_TIMER_HZ    120000000u
#define CAPTURE_MIN_TICKS   6u  // 20 MHz
#define CAPTURE_MAX_RATE_HZ (CAPTURE_TIMER_HZ / CAPTURE_MIN_TICKS)
#define CAPTURE_MAX_STAGES  4u

// Sample rings: 32 MiB in SDRAM, or 64 KiB in SRAM3 for short captures (and
// as a fallback if the SDRAM fails its power-on test). The maximum depth is
// therefore only known at run time.
uint32_t capture_max_samples(void);
uint32_t capture_ring_size(void);
#define CAPTURE_MAX_SAMPLES capture_max_samples()

// Streaming ring (common/stream.c). The DMA FIFO holds up to 16 samples that
// are already counted by NDTR but not yet in RAM; capture_written() excludes
// them, so the DMA can be up to CAPTURE_GUARD samples ahead of it.
#define CAPTURE_RING_SIZE() capture_ring_size()
#define CAPTURE_GUARD       64u

// Reply to a sigrok-pico rate the timer cannot produce exactly.
#define LA_RATE_ERROR "LA-F429: max 20MHz, 120MHz/n"  // the driver shows <= 30 chars
