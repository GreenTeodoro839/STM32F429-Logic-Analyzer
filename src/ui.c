#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "capture.h"
#include "clock.h"
#include "font6x8.h"
#include "la_status.h"
#include "oled.h"
#include "tusb.h"

la_status_t g_la;

#define FRAME_MS 100u
#define PROBE_MS 2000u

static uint8_t s_seen_hi, s_seen_lo;  // accumulated since the last frame
static uint8_t s_lv_hi, s_lv_lo;      // snapshot drawn in this frame
static uint8_t s_pending;             // pages still to render in this frame
static uint32_t s_sum[OLED_PAGES];    // checksum of what is on the screen
static uint32_t s_frame_ms, s_probe_ms;

void ui_levels(uint8_t v) {
  s_seen_hi |= v;
  s_seen_lo |= (uint8_t)~v;
}

static uint32_t checksum(const uint8_t *b) {
  uint32_t h = 2166136261u;  // FNV-1a
  for (unsigned i = 0; i < OLED_W; i++) h = (h ^ b[i]) * 16777619u;
  return h;
}

static void fmt_hz(char *out, size_t n, uint32_t hz) {
  const char *unit = "Hz";
  uint32_t div = 1;
  if (hz >= 1000000u) {
    unit = "MHz";
    div = 1000000u;
  } else if (hz >= 1000u) {
    unit = "kHz";
    div = 1000u;
  }
  uint32_t ip = hz / div;
  uint32_t frac = (uint32_t)(((uint64_t)(hz % div) * 1000u + div / 2u) / div);
  if (div == 1u || frac == 0u) {
    snprintf(out, n, "%lu%s", (unsigned long)ip, unit);
    return;
  }
  if (frac >= 1000u) {
    snprintf(out, n, "%lu%s", (unsigned long)(ip + 1u), unit);
    return;
  }
  char f[4];
  snprintf(f, sizeof f, "%03lu", (unsigned long)frac);
  for (int i = 2; i > 0 && f[i] == '0'; i--) f[i] = 0;
  snprintf(out, n, "%lu.%s%s", (unsigned long)ip, f, unit);
}

static void render_state(char *line, size_t n) {
  switch (g_la.state) {
    case LA_WAIT_TRIGGER: snprintf(line, n, "Waiting for trigger"); break;
    case LA_CAPTURING: snprintf(line, n, "Capturing..."); break;
    case LA_SENDING: snprintf(line, n, "Uploading..."); break;
    case LA_DONE:
      snprintf(line, n, "Done #%lu%s", (unsigned long)g_la.captures, g_la.overrun ? " OVERRUN" : "");
      break;
    case LA_ABORTED: snprintf(line, n, "Aborted"); break;
    case LA_STREAMING: snprintf(line, n, "Streaming..."); break;
    case LA_STREAM_DONE:
      snprintf(line, n, "Streamed %lu%s", (unsigned long)(g_la.stream_samples >= 10000000u ? g_la.stream_samples / 1000000u
                                                         : g_la.stream_samples >= 10000u ? g_la.stream_samples / 1000u
                                                                                        : g_la.stream_samples),
               g_la.stream_samples >= 10000000u ? "M" : g_la.stream_samples >= 10000u ? "k" : "");
      break;
    case LA_STREAM_OVERFLOW: snprintf(line, n, "Stream OVERFLOW"); break;
    default: snprintf(line, n, "Idle"); break;
  }
}

static void render_rate(char *line, size_t n) {
  char hz[16];
  if (!g_la.rate_hz) {
    fmt_hz(hz, sizeof hz, CAPTURE_MAX_RATE_HZ);
    snprintf(line, n, "Max %s %lu smp", hz, (unsigned long)CAPTURE_MAX_SAMPLES);
    return;
  }
  fmt_hz(hz, sizeof hz, g_la.rate_hz);
  if (g_la.streaming) {
    snprintf(line, n, "%s stream %uch", hz, (unsigned)g_la.stream_channels);
    return;
  }
  snprintf(line, n, "%s%s %lu smp", g_la.rate_exact ? "" : "~", hz, (unsigned long)g_la.samples);
}

static void render_trigger(char *line, size_t n) {
  if (g_la.streaming) {
    snprintf(line, n, "Trig: in PulseView");
    return;
  }
  if (!g_la.num_stages) {
    snprintf(line, n, "Trig: none");
    return;
  }
  char pat[LA_NUM_CH + 1];
  for (unsigned ch = 0; ch < LA_NUM_CH; ch++)
    pat[ch] = (g_la.mask0 >> ch) & 1u ? (((g_la.value0 >> ch) & 1u) ? 'R' : 'F') : 'x';  // edge
  pat[LA_NUM_CH] = 0;
  if (g_la.num_stages > 1)
    snprintf(line, n, "Trig %s +%u", pat, (unsigned)(g_la.num_stages - 1u));
  else
    snprintf(line, n, "Trig %s", pat);
}

static void render_hint(char *line, size_t n) {
  if (!g_clock_hse_ok) snprintf(line, n, "HSE fail: no USB!");
  else if (g_la.rle_requested) snprintf(line, n, "RLE unsupp: CH7=0");
  else if (g_la.rate_hz && !g_la.rate_exact && !g_la.streaming) snprintf(line, n, "Rate not exact!");
  else snprintf(line, n, "PB0:100k PB8:1k PA9:U");
}

// 8 cells of 16 px; digits centred in page 4, boxes over pages 5 and 6.
static void render_channels(uint8_t page, uint8_t *buf) {
  memset(buf, 0, OLED_W);
  for (unsigned ch = 0; ch < LA_NUM_CH; ch++) {
    uint8_t *cell = &buf[ch * 16u];
    if (page == 4) {
      memcpy(&cell[5], font6x8['0' + ch - FONT6X8_FIRST], 5);
      continue;
    }
    bool hi = (s_lv_hi >> ch) & 1u, lo = (s_lv_lo >> ch) & 1u;
    bool top = page == 5;
    uint8_t side = top ? 0xFE : 0x7F;  // rows 1..14 of the 16-row band
    uint8_t edge = top ? 0x02 : 0x40;  // top / bottom outline row
    for (unsigned x = 1; x <= 14; x++) {
      uint8_t b;
      if (x == 1 || x == 14 || (hi && !lo)) b = side;  // outline or solid (HIGH)
      else if (hi && lo) b = edge | ((x & 1u) ? (top ? 0xA8 : 0x2A) : (top ? 0x54 : 0x15));  // toggling
      else b = edge;  // LOW: hollow
      cell[x] = b;
    }
  }
}

static void render_page(uint8_t p, uint8_t *buf) {
  char line[40];  // only the first OLED_COLS characters are shown
  switch (p) {
    case 0:
      snprintf(line, sizeof line, " LA-F103     %s", tud_mounted() ? "USB OK" : "NO USB");
      oled_render_text(buf, line, true);
      return;
    case 1: render_state(line, sizeof line); break;
    case 2: render_rate(line, sizeof line); break;
    case 3: render_trigger(line, sizeof line); break;
    case 7: render_hint(line, sizeof line); break;
    default: render_channels(p, buf); return;
  }
  oled_render_text(buf, line, false);
}

static void draw_page(uint8_t p) {
  uint8_t buf[OLED_W];
  render_page(p, buf);
  uint32_t sum = checksum(buf);
  if (sum == s_sum[p]) return;
  if (oled_write_page(p, buf)) s_sum[p] = sum;
}

void ui_init(void) {
  memset(&g_la, 0, sizeof g_la);
  oled_init();
  memset(s_sum, 0, sizeof s_sum);
  s_probe_ms = millis();
}

void ui_poll(void) {
  uint32_t now = millis();
  if (!oled_present()) {
    if ((uint32_t)(now - s_probe_ms) >= PROBE_MS) {
      s_probe_ms = now;
      if (oled_init()) memset(s_sum, 0, sizeof s_sum);
    }
    return;
  }
  if (!s_pending) {
    if ((uint32_t)(now - s_frame_ms) < FRAME_MS) return;
    s_frame_ms = now;
    s_lv_hi = s_seen_hi;
    s_lv_lo = s_seen_lo;
    s_seen_hi = s_seen_lo = 0;
    s_pending = 0xFF;
  }
  uint8_t p = (uint8_t)__builtin_ctz(s_pending);
  s_pending &= (uint8_t)~(1u << p);
  draw_page(p);
}

void ui_redraw_status(void) {
  for (uint8_t p = 0; p < 4; p++) draw_page(p);
  draw_page(7);
}
