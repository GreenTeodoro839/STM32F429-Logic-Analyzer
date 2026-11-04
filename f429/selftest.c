#include "selftest.h"

#include "capture_hw.h"
#include "clock.h"

selftest_t g_selftest;

#define SELFTEST_SAMPLES (2u * CAPTURE_CHUNK)

static const uint8_t s_ticks[SELFTEST_RATES] = {12, 6, 5, 4, 3};  // 10, 20, 24, 30, 40 MHz

typedef uint32_t __attribute__((may_alias)) u32_alias;

// Sample SELFTEST_SAMPLES at `ticks` and return the number of lost samples
// (-1 = the DMA did not run). With `busy` the CPU reads the ring meanwhile,
// like the trigger scanner does.
static int32_t run(capture_mem_t mem, uint32_t ticks, bool busy) {
  capture_timing_t t = {0, (uint16_t)(ticks - 1u), ticks, 0, true};
  uint32_t size;
  const uint8_t *ring = capture_hw_ring(mem, &size);
  volatile uint32_t sink = 0;

  capture_hw_start(mem, &t);
  uint32_t t0 = millis();
  while (capture_hw_chunks() < SELFTEST_SAMPLES / CAPTURE_CHUNK) {
    if (busy) {
      uint32_t s = 0;
      for (uint32_t i = 0; i < 4096u; i += 4u) s += *(const u32_alias *)(ring + i);
      sink += s;
    }
    if ((uint32_t)(millis() - t0) > 100u) break;  // DMA not running at all
  }
  bool done = capture_hw_chunks() >= SELFTEST_SAMPLES / CAPTURE_CHUNK;
  capture_hw_stop();
  g_selftest.errors |= capture_hw_errors();
  (void)sink;
  return done ? capture_hw_lost() : -1;
}

void selftest_dma(void) {
  g_selftest.errors = 0;
  bool ok[2] = {true, true};  // every slower rate passed as well
  for (int m = 0; m < 2; m++) g_selftest.max_ok_ticks[m] = 0;
  for (unsigned r = 0; r < SELFTEST_RATES; r++) {
    g_selftest.ticks[r] = s_ticks[r];
    for (int m = 0; m < 2; m++) {
      for (int busy = 0; busy < 2; busy++) {
        int32_t lost = -1;
        if (m == 0 || capture_sdram_ok()) lost = run(m ? CAPTURE_MEM_SDRAM : CAPTURE_MEM_SRAM, s_ticks[r], busy);
        g_selftest.lost[r][m][busy] = lost;
      }
      ok[m] = ok[m] && g_selftest.lost[r][m][0] == 0 && g_selftest.lost[r][m][1] == 0;
      if (ok[m]) g_selftest.max_ok_ticks[m] = s_ticks[r];
    }
  }
}
