// Signal generator arithmetic (no hardware access, unit-tested on the host).
#pragma once

#include <stdint.h>

typedef struct {
  uint32_t psc;          // prescaler register value (divide by psc + 1)
  uint32_t arr;          // auto-reload (period = arr + 1 counts)
  uint32_t ccr;          // compare value (high counts in PWM mode 1)
  uint64_t actual_mhz;   // resulting frequency in millihertz
  unsigned actual_duty;  // resulting duty in 0.1 %
} gen_timing_t;

// Timer setting for `freq_hz` (clamped to 1 .. clk/2) and `duty_pct`
// (1..99) on a timer clocked at clk_hz whose ARR holds up to max_arr.
// Picks the period closest to clk/freq that the 16-bit prescaler allows.
gen_timing_t gen_timer_calc(uint32_t clk_hz, uint32_t max_arr, uint32_t freq_hz, unsigned duty_pct);

// USART BRR for 16x oversampling; *actual receives the real baud rate.
uint32_t gen_uart_brr(uint32_t pclk_hz, uint32_t baud, uint32_t *actual);
