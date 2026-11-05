#include "ring_stub.h"

uint8_t g_ring[RING_SIZE];

const uint8_t *capture_span(uint64_t index, uint32_t *contiguous) {
  uint32_t i = (uint32_t)index & (RING_SIZE - 1u);
  *contiguous = CAPTURE_CHUNK - i % CAPTURE_CHUNK;
  return &g_ring[i];
}

uint8_t capture_sample(uint32_t index) { return g_ring[index & (RING_SIZE - 1u)]; }

const uint8_t *capture_window_span(const capture_window_t *w, uint32_t i, uint32_t *contiguous) {
  return capture_span((uint64_t)w->start + i, contiguous);
}

capture_window_t ring_window(uint32_t start, uint32_t n, uint32_t rate_hz) {
  capture_window_t w;
  memset(&w, 0, sizeof w);
  w.valid = true;
  w.mem = CAPTURE_MEM_SDRAM;
  w.start = start & (RING_SIZE - 1u);
  w.total = n;
  w.trigger = -1;
  w.rate_hz = rate_hz;
  return w;
}

void ring_put(const capture_window_t *w, uint32_t i, uint8_t v) { g_ring[(w->start + i) & (RING_SIZE - 1u)] = v; }
