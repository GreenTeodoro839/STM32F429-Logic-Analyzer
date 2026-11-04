// Host stand-in for the F429 sample ring (tests only): a ring of RING_SIZE
// samples that is contiguous only within 32 KiB chunks, like the SDRAM ring.
#pragma once

#include <stdint.h>
#include <string.h>

#include "capture_hw.h"

#define RING_SIZE (1u << 22)

extern uint8_t g_ring[RING_SIZE];

// Make a window of `n` samples starting at ring index `start` (wraps).
capture_window_t ring_window(uint32_t start, uint32_t n, uint32_t rate_hz);
// Write window sample i.
void ring_put(const capture_window_t *w, uint32_t i, uint8_t v);
