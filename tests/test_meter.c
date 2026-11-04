// Host test for f429/meter.c: gated statistics within the CPU budget.
#include <stdio.h>
#include <string.h>

#include "capture_hw.h"
#include "meter.h"

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

uint32_t g_cycle_step = 1000;
static uint32_t s_cyc;
uint32_t cycles(void) { return s_cyc += g_cycle_step; }
void capture_meter_start(uint32_t ticks, capture_chunk_fn cb) { (void)ticks, (void)cb; }
void capture_meter_stop(void) {}
bool capture_meter_running(void) { return true; }
void meter_test_chunk(const uint8_t *p, uint32_t n);

#define GATE 153u
static uint64_t s_t;  // absolute sample time (10 MHz)

static void chunk(bool lost) {
  static uint8_t buf[CAPTURE_CHUNK];
  for (uint32_t i = 0; i < CAPTURE_CHUNK; i++, s_t++) {
    uint8_t v = 0;
    if (s_t % 10000u < 5000u) v |= 1;      // ch0 1 kHz
    if (s_t % 270270u < 135135u) v |= 2;   // ch1 37.000037 Hz (period > a chunk)
    if (s_t % 100u < 25u) v |= 4;          // ch2 100 kHz, 25 %
    v |= 8;                                // ch3 high
    buf[i] = v;
  }
  if (lost) meter_test_chunk(0, 0);
  else meter_test_chunk(buf, CAPTURE_CHUNK);
}

static void gate(meter_result_t *r, uint32_t *seq, int lost_at) {
  for (unsigned k = 0; k < GATE; k++) chunk((int)k == lost_at);
  CHECK(meter_get(r, seq), "no result published");
}

int main(void) {
  meter_result_t r;
  uint32_t seq = 0;
  meter_start();

  // light load: the whole gate is one segment
  g_cycle_step = 1000;
  gate(&r, &seq, -1);
  gate(&r, &seq, -1);  // second gate starts mid-signal
  CHECK(r.coverage == 1000, "coverage %u", r.coverage);
  uint64_t f0 = ana_freq_mhz(&r.st, 0, r.rate_hz), f1 = ana_freq_mhz(&r.st, 1, r.rate_hz);
  CHECK(f0 == 1000000, "ch0 %llu mHz", (unsigned long long)f0);
  CHECK(f1 >= 36999 && f1 <= 37001, "ch1 %llu mHz", (unsigned long long)f1);
  CHECK(ana_duty_permille(&r.st, 2) == 250, "ch2 duty %u", ana_duty_permille(&r.st, 2));
  CHECK(ana_min_pulse(&r.st, 2) == 25, "ch2 pulse %u", ana_min_pulse(&r.st, 2));
  CHECK(ana_freq_mhz(&r.st, 3, r.rate_hz) == 0 && ((r.st.first >> 3) & 1u), "ch3 static high");

  // heavy load: each chunk costs 80 % of a chunk time -> one in three measured
  g_cycle_step = 32768u * 12u * 80u / 100u;
  gate(&r, &seq, -1);
  CHECK(r.coverage > 300 && r.coverage < 370, "coverage under load %u", r.coverage);
  f0 = ana_freq_mhz(&r.st, 0, r.rate_hz);
  CHECK(f0 == 1000000, "ch0 under load %llu mHz", (unsigned long long)f0);
  CHECK(ana_duty_permille(&r.st, 2) == 250, "ch2 duty under load %u", ana_duty_permille(&r.st, 2));

  // a lost chunk in the middle of a gate is left out
  g_cycle_step = 1000;
  gate(&r, &seq, 70);
  CHECK(r.coverage < 1000 && r.coverage >= 985, "coverage with a loss %u", r.coverage);
  CHECK(ana_freq_mhz(&r.st, 0, r.rate_hz) == 1000000, "ch0 after loss");
  CHECK(ana_duty_permille(&r.st, 0) == 500, "ch0 duty after loss %u", ana_duty_permille(&r.st, 0));

  if (g_fail) {
    printf("%d check(s) failed\n", g_fail);
    return 1;
  }
  printf("meter: all checks passed\n");
  return 0;
}
