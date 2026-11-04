// Power-on self-test: SDRAM check and measured DMA sampling limits.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SELFTEST_RATES 5

typedef struct {
  bool sdram_ok;
  // DMA sampling test: for each rate, samples lost out of 65536
  // ([rate][SRAM, SDRAM][CPU idle, CPU reading the ring]); -1 = not run.
  uint8_t ticks[SELFTEST_RATES];  // timer ticks per sample (120 MHz / ticks)
  int32_t lost[SELFTEST_RATES][2][2];
  uint32_t errors;  // DMA error flags seen during the test
  uint32_t max_ok_ticks[2];  // fastest lossless setting per memory (CPU busy), 0 = none
} selftest_t;

extern selftest_t g_selftest;

void selftest_dma(void);  // ~50 ms
