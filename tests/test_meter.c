// Host test for f429/meter.c: gated statistics within the CPU budget.
//
// The CPU is simulated: ana_stats_feed_masked is wrapped (ld --wrap) and
// advances a cycle clock by a cost model (per sample and per edge), and the
// driver below hands chunks to the meter the way capture.c does with its
// two-chunk SRAM ring: chunk k arrives when it is complete and is overwritten
// once chunk k + 1 is complete.
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

#define CHUNK_CYC  (CAPTURE_CHUNK * 12ull)  // CPU cycles per chunk at 10 MHz
#define GATE       153u

// --- simulated CPU ------------------------------------------------------------

// cost model, as measured on the board (LA-F429, 120 MHz, code in flash)
static unsigned s_cyc_sample = 2, s_cyc_change = 150, s_cyc_edge = 50;
static uint64_t s_clock;     // CPU cycles
static uint64_t s_deadline;  // the chunk being processed is overwritten from here on
static uint64_t s_busy_cycles;

uint32_t g_cycle_step;  // (unused: meter_stub/clock.h)
uint32_t cycles(void) { return (uint32_t)(s_clock += 20u); }

void __real_ana_stats_feed_masked(ana_stats_t *s, const uint8_t *p, uint32_t n, uint8_t mask);
void __wrap_ana_stats_feed_masked(ana_stats_t *s, const uint8_t *p, uint32_t n, uint8_t mask) {
  uint8_t prev = s->prev;
  uint64_t cost = (uint64_t)n * s_cyc_sample;
  for (uint32_t i = 0; i < n; i++) {
    uint8_t v = p[i] & mask;
    if (v != prev) cost += s_cyc_change + (uint64_t)__builtin_popcount(v ^ prev) * s_cyc_edge;
    prev = v;
  }
  s_clock += cost;
  __real_ana_stats_feed_masked(s, p, n, mask);
}

void capture_meter_start(uint32_t ticks, capture_chunk_fn cb) { (void)ticks, (void)cb; }
void capture_meter_stop(void) {}
bool capture_meter_running(void) { return true; }
bool capture_meter_intact(void) { return s_clock < s_deadline; }
void meter_test_chunk(const uint8_t *p, uint32_t n);

// --- signals and driver -------------------------------------------------------

static uint64_t s_t;      // absolute sample time (10 MHz)
static uint64_t s_k;      // chunks produced
static bool s_fast = false;  // ch2 at 1 MHz instead of 10 kHz
static bool s_all_fast;      // every channel toggling every sample (5 MHz)
static uint64_t s_stall_at = ~0ull;  // an interrupt storm stealing this chunk's time
static uint64_t s_lost;

static void make_chunk(uint8_t *buf) {
  for (uint32_t i = 0; i < CAPTURE_CHUNK; i++, s_t++) {
    uint8_t v = 0;
    if (s_all_fast) {
      buf[i] = (s_t & 1u) ? 0xFF : 0x00;
      continue;
    }
    if (s_t % 10000u < 5000u) v |= 1;       // ch0 1 kHz
    if (s_t % 270270u < 135135u) v |= 2;    // ch1 37.000037 Hz (period > a chunk)
    if (s_fast) {
      if (s_t % 10u < 5u) v |= 4;           // ch2 1 MHz
    } else if (s_t % 1000u < 250u) {
      v |= 4;                               // ch2 10 kHz, 25 %
    }
    v |= 8;                                 // ch3 high
    if (s_t % 1000u < 1u) v |= 16;          // ch4 10 kHz, 100 ns pulses
    buf[i] = v;
  }
}

static void chunk(bool drop) {
  static uint8_t buf[CAPTURE_CHUNK];
  make_chunk(buf);
  uint64_t arrive = (s_k + 1u) * CHUNK_CYC;  // chunk complete
  s_k++;
  if (drop) {  // (as if the driver had fallen behind)
    meter_test_chunk(0, 1);
    s_lost++;
    return;
  }
  if (s_clock < arrive) s_clock = arrive;     // idle until then
  if (s_clock >= arrive + CHUNK_CYC) {        // still busy with older chunks: overwritten
    meter_test_chunk(0, 1);
    s_lost++;
    return;
  }
  s_deadline = arrive + CHUNK_CYC;
  uint64_t t0 = s_clock;
  if (s_k - 1u == s_stall_at) s_clock += CHUNK_CYC * 6u / 5u;  // preempted for 1.2 chunk times
  meter_test_chunk(buf, CAPTURE_CHUNK);
  s_busy_cycles += s_clock - t0;
}

static bool gate(meter_result_t *r, uint32_t *seq, int drop_at) {
  for (unsigned k = 0; k < GATE; k++) chunk((int)k == drop_at);
  return meter_get(r, seq);
}

static uint64_t freq(const meter_result_t *r, unsigned ch) { return ana_freq_mhz(&r->st, ch, r->rate_hz); }

static void check_slow_channels(const meter_result_t *r, const char *what) {
  CHECK(freq(r, 0) == 1000000, "%s: ch0 %llu mHz", what, (unsigned long long)freq(r, 0));
  CHECK(freq(r, 1) >= 36999 && freq(r, 1) <= 37001, "%s: ch1 %llu mHz", what, (unsigned long long)freq(r, 1));
  CHECK(ana_duty_permille(&r->st, 0) == 500, "%s: ch0 duty %u", what, ana_duty_permille(&r->st, 0));
  CHECK(ana_min_pulse(&r->st, 0) == 5000, "%s: ch0 pulse %u", what, ana_min_pulse(&r->st, 0));
  CHECK(freq(r, 3) == 0 && ((r->st.first >> 3) & 1u), "%s: ch3 static high", what);
  CHECK(freq(r, 4) == 10000000, "%s: ch4 %llu mHz", what, (unsigned long long)freq(r, 4));
  CHECK(ana_min_pulse(&r->st, 4) == 1, "%s: ch4 pulse %u", what, ana_min_pulse(&r->st, 4));
}

int main(void) {
  meter_result_t r;
  uint32_t seq = 0;
  meter_start();

  // light load: everything measured over the whole gate
  CHECK(gate(&r, &seq, -1), "no result");
  CHECK(gate(&r, &seq, -1), "no result");  // (the second gate starts mid-signal)
  CHECK(r.coverage == 1000 && r.sampled == 0, "light: coverage %u sampled %02x", r.coverage, r.sampled);
  check_slow_channels(&r, "light");
  CHECK(freq(&r, 2) == 10000000, "light: ch2 %llu mHz", (unsigned long long)freq(&r, 2));
  CHECK(ana_duty_permille(&r.st, 2) == 250, "light: ch2 duty %u", ana_duty_permille(&r.st, 2));
  CHECK(ana_min_pulse(&r.st, 2) == 250, "light: ch2 pulse %u", ana_min_pulse(&r.st, 2));
  printf("light: load %u.%u%%\n", r.load / 10u, r.load % 10u);
  CHECK(r.load < 450, "light: load %u", r.load);

  // ch2 at 1 MHz: it becomes busy and is sampled, the others stay exact
  s_fast = true;
  CHECK(gate(&r, &seq, -1), "no result");  // (switch-over gate: ch2 promoted)
  CHECK(r.sampled == 0x04, "fast: sampled %02x after the first gate", r.sampled);
  check_slow_channels(&r, "fast, switch-over");
  for (int g = 0; g < 2; g++) CHECK(gate(&r, &seq, -1), "no result");
  CHECK(r.sampled == 0x04, "fast: sampled %02x", r.sampled);
  CHECK(r.coverage == 1000, "fast: coverage %u", r.coverage);
  CHECK(r.sampled_coverage >= 50 && r.sampled_coverage < 1000, "fast: sampled coverage %u", r.sampled_coverage);
  check_slow_channels(&r, "fast");
  CHECK(freq(&r, 2) == 1000000000, "fast: ch2 %llu mHz", (unsigned long long)freq(&r, 2));
  CHECK(ana_duty_permille(&r.st, 2) == 500, "fast: ch2 duty %u", ana_duty_permille(&r.st, 2));
  CHECK(ana_min_pulse(&r.st, 2) == 5, "fast: ch2 pulse %u", ana_min_pulse(&r.st, 2));
  printf("fast: load %u.%u%%, ch2 sampled %u.%u%%\n", r.load / 10u, r.load % 10u, r.sampled_coverage / 10u,
         r.sampled_coverage % 10u);
  CHECK(r.load <= 480, "fast: load %u", r.load);
  CHECK(s_lost == 0, "fast: %llu chunks lost", (unsigned long long)s_lost);

  // the fast signal stops: ch2 goes back to the full pass
  s_fast = false;
  for (int g = 0; g < 3; g++) CHECK(gate(&r, &seq, -1), "no result");
  CHECK(r.sampled == 0 && r.coverage == 1000, "calm again: sampled %02x coverage %u", r.sampled, r.coverage);
  CHECK(freq(&r, 2) == 10000000, "calm again: ch2 %llu mHz", (unsigned long long)freq(&r, 2));

  // a lost chunk in the middle of a gate is left out
  CHECK(gate(&r, &seq, 70), "no result");
  CHECK(r.coverage < 1000 && r.coverage >= 985, "loss: coverage %u", r.coverage);
  check_slow_channels(&r, "loss");

  // the CPU is taken away for longer than a chunk: the chunk is overwritten
  // while measured and must be left out, not mixed in
  s_lost = 0;
  s_stall_at = s_k + 40u;
  CHECK(gate(&r, &seq, -1), "no result");
  CHECK(r.coverage < 1000 && r.coverage >= 980, "stall: coverage %u", r.coverage);
  check_slow_channels(&r, "stall");
  CHECK(freq(&r, 2) == 10000000, "stall: ch2 %llu mHz", (unsigned long long)freq(&r, 2));
  s_stall_at = ~0ull;

  // worst case, every channel toggling at 5 MHz: the load stays bounded
  s_all_fast = true;
  s_lost = 0;
  s_busy_cycles = 0;
  uint64_t c0 = s_k;
  for (int g = 0; g < 3; g++) CHECK(gate(&r, &seq, -1), "no result");
  unsigned load = (unsigned)(s_busy_cycles * 1000u / ((s_k - c0) * CHUNK_CYC));
  printf("all fast: load %u.%u%%, sampled %02x %u.%u%%, lost %llu\n", load / 10u, load % 10u, r.sampled,
         r.sampled_coverage / 10u, r.sampled_coverage % 10u, (unsigned long long)s_lost);
  CHECK(load < 550, "all fast: load %u", load);
  CHECK(s_lost == 0, "all fast: %llu chunks lost", (unsigned long long)s_lost);
  CHECK(r.sampled == 0xFF, "all fast: sampled %02x", r.sampled);
  for (unsigned ch = 0; ch < 8; ch++)
    CHECK(freq(&r, ch) == 5000000000ull, "all fast: ch%u %llu mHz", ch, (unsigned long long)freq(&r, ch));
  s_all_fast = false;

  if (g_fail) {
    printf("%d check(s) failed\n", g_fail);
    return 1;
  }
  printf("meter: all checks passed\n");
  return 0;
}
