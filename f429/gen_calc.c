#include "gen_calc.h"

static uint64_t udiff(uint64_t a, uint64_t b) { return a > b ? a - b : b - a; }

gen_timing_t gen_timer_calc(uint32_t clk_hz, uint32_t max_arr, uint32_t freq_hz, unsigned duty_pct) {
  gen_timing_t t;
  if (freq_hz < 1) freq_hz = 1;
  if (freq_hz > clk_hz / 2u) freq_hz = clk_hz / 2u;
  if (duty_pct < 1) duty_pct = 1;
  if (duty_pct > 99) duty_pct = 99;

  // Ideal period in timer counts, and the best p * q (p = prescaler, q = ARR + 1)
  uint64_t n = ((uint64_t)clk_hz + freq_hz / 2u) / freq_hz;
  if (n < 2) n = 2;
  uint64_t qmax = (uint64_t)max_arr + 1u;
  uint64_t pmin = (n + qmax - 1u) / qmax;
  if (pmin < 1) pmin = 1;
  uint64_t best_p = pmin, best_q = (n + pmin / 2u) / pmin;
  if (best_q > qmax) best_q = qmax;
  // A slightly larger prescaler often divides the period exactly
  for (uint64_t p = pmin; p <= 65536u && p < pmin + 4096u && udiff(best_p * best_q, n); p++) {
    uint64_t q = (n + p / 2u) / p;
    if (q < 2 || q > qmax) continue;
    if (udiff(p * q, n) < udiff(best_p * best_q, n)) {
      best_p = p;
      best_q = q;
    }
  }
  if (best_q < 2) best_q = 2;

  uint64_t ccr = (best_q * duty_pct + 50u) / 100u;
  if (ccr < 1) ccr = 1;
  if (ccr > best_q - 1u) ccr = best_q - 1u;

  t.psc = (uint32_t)(best_p - 1u);
  t.arr = (uint32_t)(best_q - 1u);
  t.ccr = (uint32_t)ccr;
  t.actual_mhz = ((uint64_t)clk_hz * 1000u + best_p * best_q / 2u) / (best_p * best_q);
  t.actual_duty = (unsigned)((ccr * 1000u + best_q / 2u) / best_q);
  return t;
}

uint32_t gen_uart_brr(uint32_t pclk_hz, uint32_t baud, uint32_t *actual) {
  if (baud < 1) baud = 1;
  uint32_t brr = (pclk_hz + baud / 2u) / baud;
  if (brr < 16) brr = 16;  // fastest 16x-oversampled rate
  if (brr > 0xFFFF) brr = 0xFFFF;
  if (actual) *actual = (pclk_hz + brr / 2u) / brr;
  return brr;
}
