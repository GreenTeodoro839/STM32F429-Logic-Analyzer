// Host tests for f429/gen_calc.c
#include <stdio.h>

#include "gen_calc.h"

static int g_fail;
#define CHECK(cond, ...)                          \
  do {                                            \
    if (!(cond)) {                                \
      printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                        \
      printf("\n");                               \
      g_fail++;                                   \
    }                                             \
  } while (0)

static void check(uint32_t clk, uint32_t max_arr, uint32_t f, unsigned duty) {
  gen_timing_t t = gen_timer_calc(clk, max_arr, f, duty);
  uint64_t counts = (uint64_t)(t.psc + 1u) * (t.arr + 1u);
  CHECK(t.psc <= 0xFFFF && t.arr <= max_arr && t.arr >= 1, "clk %u f %u: psc %u arr %u", clk, f, t.psc, t.arr);
  CHECK(t.ccr >= 1 && t.ccr <= t.arr, "clk %u f %u: ccr %u arr %u", clk, f, t.ccr, t.arr);
  // the period must be the representable one closest to the ideal clk / f
  double ideal = (double)clk / f;
  double err = (counts - ideal) / ideal;
  if (err < 0) err = -err;
  double bound = 0.5 / ideal + (ideal > 65536.0 * 16 ? 1e-4 : 1e-9);  // rounding of n (+ prescaler steps)
  CHECK(err <= bound + 1e-12, "clk %u f %u: %llu counts, ideal %.3f (err %.2e)", clk, f, (unsigned long long)counts,
        ideal, err);
  uint64_t want_mhz = ((uint64_t)clk * 1000u + counts / 2u) / counts;
  CHECK(t.actual_mhz == want_mhz, "clk %u f %u: actual %llu mHz, want %llu", clk, f, (unsigned long long)t.actual_mhz,
        (unsigned long long)want_mhz);
  // duty within one count of the request (when there are enough counts)
  if (t.arr >= 100) {
    int d = (int)t.actual_duty - (int)(duty * 10);
    CHECK(d >= -10 && d <= 10, "clk %u f %u duty %u: got %u permille", clk, f, duty, t.actual_duty);
  }
}

int main(void) {
  static const uint32_t steps[] = {1, 2, 5};
  for (uint32_t dec = 1; dec <= 10000000; dec *= 10)
    for (unsigned s = 0; s < 3; s++) {
      uint32_t f = dec * steps[s];
      if (f > 10000000) break;
      for (unsigned duty = 10; duty <= 90; duty += 40) {
        check(60000000, 0xFFFF, f, duty);       // TIM3
        check(120000000, 0xFFFF, f, duty);      // TIM1
        check(60000000, 0xFFFFFFFF, f, duty);   // TIM2
      }
    }
  uint32_t odd[] = {3, 7, 13, 999, 12345, 65537, 123457, 777777, 3333333, 7000000};
  for (unsigned i = 0; i < sizeof odd / sizeof odd[0]; i++) {
    check(60000000, 0xFFFF, odd[i], 50);
    check(120000000, 0xFFFF, odd[i], 33);
  }
  // exact factorisations are found where they exist
  gen_timing_t t = gen_timer_calc(60000000, 0xFFFF, 1, 50);
  CHECK(t.actual_mhz == 1000, "1 Hz on a 16-bit timer: %llu mHz", (unsigned long long)t.actual_mhz);
  t = gen_timer_calc(60000000, 0xFFFF, 100000, 50);
  CHECK(t.actual_mhz == 100000000 && t.ccr * 2 == t.arr + 1, "100 kHz: %llu mHz", (unsigned long long)t.actual_mhz);
  // clamping
  t = gen_timer_calc(60000000, 0xFFFF, 100000000, 50);
  CHECK(t.actual_mhz == 30000000000ull, "clamped to clk/2: %llu", (unsigned long long)t.actual_mhz);

  uint32_t act;
  CHECK(gen_uart_brr(60000000, 115200, &act) == 521 && act == 115163, "brr 115200 -> %u", act);
  gen_uart_brr(60000000, 1000000, &act);
  CHECK(act == 1000000, "1 Mbaud -> %u", act);

  if (g_fail) {
    printf("%d check(s) failed\n", g_fail);
    return 1;
  }
  printf("gen_calc: all checks passed\n");
  return 0;
}
