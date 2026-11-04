// Host tests for f429/analyze.c and f429/decode.c on synthetic signals.
#include <stdio.h>
#include <stdlib.h>

#include "analyze.h"
#include "decode.h"
#include "ring_stub.h"

static int g_fail;
#define CHECK(cond, ...)                                     \
  do {                                                       \
    if (!(cond)) {                                           \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
      printf(__VA_ARGS__);                                   \
      printf("\n");                                          \
      g_fail++;                                              \
    }                                                        \
  } while (0)

static uint32_t rng = 12345;
static uint32_t rnd(void) {
  rng = rng * 1664525u + 1013904223u;
  return rng >> 8;
}

// A window that wraps around the end of the ring and crosses chunks.
static capture_window_t make_window(uint32_t n, uint32_t rate) { return ring_window(RING_SIZE - 70001u, n, rate); }

// ---------------------------------------------------------------------------
// Edge search against brute force
// ---------------------------------------------------------------------------

static void test_edges(void) {
  const uint32_t n = 300000;
  capture_window_t w = make_window(n, 1000000);
  uint8_t v = 0;
  for (uint32_t i = 0; i < n;) {
    uint32_t run = 1u + rnd() % (rnd() % 4 ? 40u : 5000u);
    for (uint32_t k = 0; k < run && i < n; k++, i++) ring_put(&w, i, v);
    v ^= (uint8_t)(1u << (rnd() % 8));
  }
  for (int t = 0; t < 3000; t++) {
    uint32_t i = rnd() % n;
    unsigned ch = rnd() % 8;
    uint8_t m = (uint8_t)(1u << ch);
    // next edge: smallest e > i with s[e-1] != s[e]
    uint32_t want = ANA_NONE;
    for (uint32_t e = i + 1; e < n; e++)
      if ((ana_sample(&w, e) ^ ana_sample(&w, e - 1)) & m) {
        want = e;
        break;
      }
    uint32_t got = ana_next_edge(&w, i, ch);
    CHECK(got == want, "next_edge(%u, ch%u) = %u, want %u", i, ch, got, want);
    // previous edge: largest e < i with s[e-1] != s[e]
    want = ANA_NONE;
    for (uint32_t e = i; e-- > 1;)
      if ((ana_sample(&w, e) ^ ana_sample(&w, e - 1)) & m) {
        want = e;
        break;
      }
    got = ana_prev_edge(&w, i, ch);
    CHECK(got == want, "prev_edge(%u, ch%u) = %u, want %u", i, ch, got, want);
    // next_change with a random mask
    uint8_t mask = (uint8_t)(rnd() | 1u), ref = (uint8_t)rnd();
    uint32_t end = i + rnd() % (n - i + 1);
    uint32_t wc = end;
    for (uint32_t j = i; j < end; j++)
      if ((ana_sample(&w, j) & mask) != (ref & mask)) {
        wc = j;
        break;
      }
    CHECK(ana_next_change(&w, i, end, mask, ref) == wc, "next_change mismatch at %u", i);
    uint32_t begin = rnd() % (i + 1);
    uint32_t wp = ANA_NONE;
    for (uint32_t j = i; j-- > begin;)
      if ((ana_sample(&w, j) & mask) != (ref & mask)) {
        wp = j;
        break;
      }
    CHECK(ana_prev_change(&w, begin, i, mask, ref) == wp, "prev_change mismatch at %u", i);
  }
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

static void test_stats(void) {
  const uint32_t n = 1000000, rate = 20000000;
  capture_window_t w = make_window(n, rate);
  for (uint32_t i = 0; i < n; i++) {
    uint8_t v = 0;
    if ((i + 7) % 20 < 10) v |= 1;           // ch0: period 20, 50 %   -> 1 MHz
    if ((i + 100) % 1000 < 250) v |= 2;      // ch1: period 1000, 25 % -> 20 kHz
    v |= 4;                                  // ch2: always high
    if (i >= 5000 && i < 5003) v |= 8;       // ch3: one 3-sample pulse
    if ((i / 6667) % 3 == 0) v |= 16;        // ch4: period 20001, high 6667
    ring_put(&w, i, v);
  }
  ana_job_t j;
  ana_job_start(&j, &w, 0, n);
  while (!ana_job_run(&j, 1000 + rnd() % 70000)) {
  }
  const ana_stats_t *s = &j.st;
  CHECK(s->n == n, "n = %llu", (unsigned long long)s->n);
  CHECK(ana_freq_mhz(s, 0, rate) == 1000000000ull, "ch0 f = %llu mHz", (unsigned long long)ana_freq_mhz(s, 0, rate));
  CHECK(ana_duty_permille(s, 0) == 500, "ch0 duty %u", ana_duty_permille(s, 0));
  CHECK(ana_min_pulse(s, 0) == 10, "ch0 min pulse %u", ana_min_pulse(s, 0));
  CHECK(ana_freq_mhz(s, 1, rate) == 20000000ull, "ch1 f = %llu", (unsigned long long)ana_freq_mhz(s, 1, rate));
  CHECK(ana_duty_permille(s, 1) == 250, "ch1 duty %u", ana_duty_permille(s, 1));
  CHECK(s->ch[1].min_high == 250 && s->ch[1].min_low == 750, "ch1 pulses %u %u", s->ch[1].min_high, s->ch[1].min_low);
  CHECK(ana_freq_mhz(s, 2, rate) == 0 && ana_duty_permille(s, 2) == 1000, "ch2");
  CHECK(s->ch[3].rises == 1 && s->ch[3].falls == 1 && s->ch[3].min_high == 3, "ch3 pulse %u %u %u", s->ch[3].rises,
        s->ch[3].falls, s->ch[3].min_high);
  uint64_t f4 = ana_freq_mhz(s, 4, rate);  // 20 MHz / 20001 = 999.95 Hz
  CHECK(f4 >= 999949ull && f4 <= 999951ull, "ch4 f = %llu", (unsigned long long)f4);

  // Same data fed in one go must give identical results
  ana_stats_t one;
  ana_stats_reset(&one, ana_sample(&w, 0));
  for (uint32_t i = 0; i < n;) {
    uint32_t c;
    const uint8_t *p = capture_span(w.start + i, &c);
    if (c > n - i) c = n - i;
    ana_stats_feed(&one, p, c);
    i += c;
  }
  ana_stats_finish(&one);
  CHECK(memcmp(one.ch, s->ch, sizeof one.ch) == 0, "incremental and one-shot stats differ");
}

// ---------------------------------------------------------------------------
// UART
// ---------------------------------------------------------------------------

static uint32_t uart_signal(capture_window_t *w, unsigned ch, uint32_t rate, uint32_t baud, const char *text, bool invert,
                            int bad_stop_at) {
  uint8_t m = (uint8_t)(1u << ch);
  uint32_t i = 0;
  // idle, then frames with small random gaps; bit boundaries on an exact time base
  double spb = (double)rate / baud;
  double t = 37.0;
  for (; i < (uint32_t)t; i++) ring_put(w, i, invert ? 0 : m);
  for (int k = 0; text[k]; k++) {
    uint16_t frame = (uint16_t)(((uint8_t)text[k]) << 1 | 0x200);  // start 0, data, stop 1
    if (k == bad_stop_at) frame &= 0x1FF;
    for (int b = 0; b < 10; b++) {
      double t1 = t + spb;
      bool level = (frame >> b) & 1u;
      for (; i < (uint32_t)t1; i++) ring_put(w, i, (level ^ invert) ? m : 0);
      t = t1;
    }
    double gap = spb * (rnd() % 3) + (rnd() % 5);
    for (; i < (uint32_t)(t + gap); i++) ring_put(w, i, invert ? 0 : m);
    t += gap;
  }
  for (; i < w->total; i++) ring_put(w, i, invert ? 0 : m);
  return i;
}

static void check_uart(uint32_t rate, uint32_t baud, bool invert, bool autobaud) {
  const char *text = "LA-F429 test 00042\r\nHello, world! ~\x01\xff";
  uint32_t n = (uint32_t)((double)rate / baud * 10.5 * 40) + 200;
  capture_window_t w = make_window(n, rate);
  uart_signal(&w, 3, rate, baud, text, invert, -1);
  dec_cfg_t c;
  dec_default_cfg(&c);
  c.uart_rx = 3;
  c.uart_invert = invert;
  c.uart_baud = autobaud ? 0 : baud;
  dec_start(&c, &w, 0);
  CHECK(!g_dec.error, "uart start error %s", g_dec.error ? g_dec.error : "");
  while (!dec_run(7)) {
  }
  size_t len = strlen(text);
  CHECK(g_dec.baud == baud, "rate %u baud %u: detected %u", rate, baud, g_dec.baud);
  CHECK(g_dec.count == len, "rate %u baud %u inv %d: %u items, want %zu", rate, baud, invert, g_dec.count, len);
  for (uint32_t k = 0; k < g_dec.count && k < len; k++) {
    CHECK(g_dec.items[k].data == (uint8_t)text[k] && !(g_dec.items[k].flags & DEC_F_ERROR),
          "rate %u baud %u: item %u = %02x flags %x, want %02x", rate, baud, k, g_dec.items[k].data,
          g_dec.items[k].flags, (uint8_t)text[k]);
  }
}

static void test_uart(void) {
  check_uart(1000000, 115200, false, true);
  check_uart(1000000, 115200, false, false);
  check_uart(1000000, 9600, false, true);
  check_uart(2000000, 57600, true, true);
  check_uart(20000000, 1000000, false, true);
  check_uart(10000000, 921600, false, true);
  check_uart(100000, 300, false, true);
  check_uart(20000000, 9600, false, true);
  check_uart(20000000, 115200, true, true);

  // framing error on the third byte
  capture_window_t w = make_window(200000, 1000000);
  uart_signal(&w, 3, 1000000, 115200, "ABCDEF", false, 2);
  dec_cfg_t c;
  dec_default_cfg(&c);
  c.uart_baud = 115200;
  dec_start(&c, &w, 0);
  while (!dec_run(100)) {
  }
  CHECK(g_dec.count >= 3 && (g_dec.items[2].flags & DEC_F_ERROR), "framing error not flagged");
  CHECK(g_dec.count >= 4 && g_dec.items[3].data == 'D', "decoder did not recover after a framing error");
}

// ---------------------------------------------------------------------------
// I2C
// ---------------------------------------------------------------------------

static uint32_t g_i;
static void put2(capture_window_t *w, unsigned n, bool scl, bool sda, unsigned cs, unsigned cd) {
  for (unsigned k = 0; k < n; k++) ring_put(w, g_i++, (uint8_t)((scl << cs) | (sda << cd)));
}

static void i2c_byte(capture_window_t *w, unsigned H, uint8_t b, bool ack, unsigned cs, unsigned cd) {
  for (int k = 7; k >= -1; k--) {
    bool bit = k >= 0 ? (b >> k) & 1u : !ack;
    put2(w, H / 2, 0, bit, cs, cd);
    put2(w, H, 1, bit, cs, cd);
    put2(w, H / 2, 0, bit, cs, cd);
  }
}

static void test_i2c(void) {
  const unsigned H = 10, cs = 5, cd = 6;  // 100 kHz at 2 MHz, SCL on CH5, SDA on CH6
  capture_window_t w = make_window(4000, 2000000);
  g_i = 0;
  put2(&w, 40, 1, 1, cs, cd);
  put2(&w, H, 1, 0, cs, cd);  // START
  put2(&w, H / 2, 0, 0, cs, cd);
  i2c_byte(&w, H, 0x3C << 1, true, cs, cd);
  i2c_byte(&w, H, 0x00, true, cs, cd);
  i2c_byte(&w, H, 0xAF, true, cs, cd);
  put2(&w, H / 2, 0, 1, cs, cd);  // repeated start
  put2(&w, H / 2, 1, 1, cs, cd);
  put2(&w, H / 2, 1, 0, cs, cd);
  put2(&w, H / 2, 0, 0, cs, cd);
  i2c_byte(&w, H, (0x3C << 1) | 1, true, cs, cd);
  i2c_byte(&w, H, 0x55, false, cs, cd);
  put2(&w, H / 2, 0, 0, cs, cd);  // STOP
  put2(&w, H / 2, 1, 0, cs, cd);
  while (g_i < w.total) put2(&w, 1, 1, 1, cs, cd);

  dec_cfg_t c;
  dec_default_cfg(&c);
  c.proto = DEC_I2C;
  c.i2c_scl = cs;
  c.i2c_sda = cd;
  dec_start(&c, &w, 0);
  while (!dec_run(3)) {
  }
  static const struct {
    uint8_t kind, data, flags;
  } want[] = {
      {DEC_K_START, 0, 0},          {DEC_K_ADDR, 0x3C, DEC_F_ACK}, {DEC_K_DATA, 0x00, DEC_F_ACK},
      {DEC_K_DATA, 0xAF, DEC_F_ACK}, {DEC_K_START, 0, 0},          {DEC_K_ADDR, 0x3C, DEC_F_ACK | DEC_F_READ},
      {DEC_K_DATA, 0x55, 0},        {DEC_K_STOP, 0, 0},
  };
  CHECK(g_dec.count == 8, "i2c: %u items", g_dec.count);
  for (unsigned k = 0; k < 8 && k < g_dec.count; k++)
    CHECK(g_dec.items[k].kind == want[k].kind && g_dec.items[k].data == want[k].data &&
              g_dec.items[k].flags == want[k].flags,
          "i2c item %u: kind %u data %02x flags %x", k, g_dec.items[k].kind, g_dec.items[k].data, g_dec.items[k].flags);
}

// ---------------------------------------------------------------------------
// SPI
// ---------------------------------------------------------------------------

static void spi_put(capture_window_t *w, unsigned n, bool clk, bool mo, bool mi, bool cs) {
  for (unsigned k = 0; k < n; k++)
    ring_put(w, g_i++, (uint8_t)((clk << 0) | (mo << 1) | (mi << 2) | (cs << 3)));
}

static void check_spi(unsigned mode, bool lsb) {
  const uint8_t mosi[] = {0x9F, 0x00, 0xA5, 0x3C}, miso[] = {0xFF, 0xEF, 0x5A, 0x81};
  bool cpol = mode >> 1, cpha = mode & 1u;
  capture_window_t w = make_window(2000, 1000000);
  g_i = 0;
  spi_put(&w, 30, cpol, 0, 0, 1);
  spi_put(&w, 5, cpol, 0, 0, 0);  // CS asserted
  for (unsigned b = 0; b < sizeof mosi; b++) {
    for (int k = 0; k < 8; k++) {
      int bit = lsb ? k : 7 - k;
      bool mo = (mosi[b] >> bit) & 1u, mi = (miso[b] >> bit) & 1u;
      if (!cpha) {
        spi_put(&w, 4, cpol, mo, mi, 0);
        spi_put(&w, 4, !cpol, mo, mi, 0);
      } else {
        spi_put(&w, 4, !cpol, mo, mi, 0);
        spi_put(&w, 4, cpol, mo, mi, 0);
      }
    }
    spi_put(&w, 3, cpol, 0, 0, 0);
  }
  spi_put(&w, 5, cpol, 0, 0, 1);
  while (g_i < w.total) spi_put(&w, 1, cpol, 1, 1, 1);

  dec_cfg_t c;
  dec_default_cfg(&c);
  c.proto = DEC_SPI;
  c.spi_clk = 0;
  c.spi_mosi = 1;
  c.spi_miso = 2;
  c.spi_cs = 3;
  c.spi_mode = (uint8_t)mode;
  c.spi_lsb_first = lsb;
  dec_start(&c, &w, 0);
  while (!dec_run(5)) {
  }
  CHECK(g_dec.count == sizeof mosi, "spi mode %u: %u items", mode, g_dec.count);
  for (unsigned k = 0; k < g_dec.count && k < sizeof mosi; k++)
    CHECK(g_dec.items[k].data == mosi[k] && g_dec.items[k].data2 == miso[k],
          "spi mode %u lsb %d item %u: %02x/%02x", mode, lsb, k, g_dec.items[k].data, g_dec.items[k].data2);
}

static void test_spi(void) {
  for (unsigned m = 0; m < 4; m++) check_spi(m, false);
  check_spi(0, true);
  check_spi(3, true);
}

int main(void) {
  test_edges();
  test_stats();
  test_uart();
  test_i2c();
  test_spi();
  if (g_fail) {
    printf("%d check(s) failed\n", g_fail);
    return 1;
  }
  printf("analyze/decode: all checks passed\n");
  return 0;
}
