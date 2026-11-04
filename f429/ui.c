#include "ui.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "capture_hw.h"
#include "clock.h"
#include "gfx.h"
#include "keys.h"
#include "la_status.h"
#include "lcd.h"
#include "selftest.h"
#include "tusb.h"

la_status_t g_la;

// ---------------------------------------------------------------------------
// Look
// ---------------------------------------------------------------------------

#define C_BG   RGB(10, 13, 18)
#define C_BAR  RGB(30, 38, 50)
#define C_TEXT RGB(230, 237, 243)
#define C_DIM  RGB(120, 130, 142)
#define C_GRID RGB(36, 44, 56)
#define C_OK   RGB(70, 200, 90)
#define C_WARN RGB(245, 185, 50)
#define C_BUSY RGB(90, 170, 255)
#define C_ERR  RGB(250, 85, 75)
#define C_TRIG RGB(120, 96, 40)

// Channel colours: resistor colour code as in PulseView (black -> grey)
static const uint16_t s_ch_color[LA_NUM_CH] = {
    RGB(175, 175, 175), RGB(200, 130, 80), RGB(240, 75, 75),  RGB(245, 150, 45),
    RGB(240, 220, 50),  RGB(85, 210, 105), RGB(85, 155, 245), RGB(185, 125, 245),
};

// Main page geometry
#define TITLE_H 24
#define STATE_Y 26
#define INFO1_Y 52
#define INFO2_Y 68
#define STATUS_H 86  // rows 0..85 belong to the status block
#define ROWS_Y  90
#define ROW_H   16
#define LEVEL_X 12
#define WAVE_X  28
#define WAVE_W  (GFX_W - WAVE_X - 2)
#define FOOT_Y  (ROWS_Y + LA_NUM_CH * ROW_H + 6)

#define FRAME_MS   100u
#define JOB_CYCLES (HCLK_HZ / 1000u * 3u)  // preview work per ui_poll call: 3 ms

typedef enum { PAGE_MAIN, PAGE_PINS, PAGE_INFO, PAGE_COUNT } page_t;

static page_t s_page;
static uint8_t s_seen_hi, s_seen_lo;  // live levels accumulated since the last frame
static uint8_t s_lv_hi, s_lv_lo;      // what the level boxes show
static bool s_levels_drawn;
static uint32_t s_frame_ms;

// Waveform preview of the last capture: OR / AND of every column's samples
static struct {
  capture_window_t win;
  uint32_t zoom;         // 1, 4, 16, ...
  uint32_t from, count;  // window-relative sample range shown
  unsigned col;          // next column to compute
  bool active, ready;
  uint8_t col_or[WAVE_W], col_and[WAVE_W];
} s_view;

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------

// v / scale with 3 significant digits, trailing zeros dropped: 1.23, 12.5, 100
static void fmt3(char *out, size_t n, uint64_t v, uint64_t scale, const char *suffix) {
  uint64_t ip = v / scale;
  unsigned dec = scale == 1u ? 0 : ip >= 100u ? 0 : ip >= 10u ? 1 : 2;
  uint64_t p = dec == 2 ? 100u : dec == 1 ? 10u : 1u;
  uint64_t q = (v * p + scale / 2u) / scale;
  unsigned long whole = (unsigned long)(q / p), frac = (unsigned long)(q % p);
  if (!frac) snprintf(out, n, "%lu%s", whole, suffix);
  else if (dec == 2 && frac % 10u) snprintf(out, n, "%lu.%02lu%s", whole, frac, suffix);
  else snprintf(out, n, "%lu.%lu%s", whole, dec == 2 ? frac / 10u : frac, suffix);
}

// 1.23k, 12.5MHz, 33.5M ...
static void fmt_si(char *out, size_t n, uint64_t v, const char *unit) {
  static const char *const pre[] = {"", "k", "M", "G"};
  unsigned e = 0;
  uint64_t scale = 1;
  while (e < 3 && v >= scale * 1000u) {
    scale *= 1000u;
    e++;
  }
  char suffix[8];
  snprintf(suffix, sizeof suffix, "%s%s", pre[e], unit);
  fmt3(out, n, v, scale, suffix);
}

// Duration of `samples` at rate_hz: 850ns, 1.6ms, 2.5s ...
static void fmt_time(char *out, size_t n, uint64_t samples, uint32_t rate_hz) {
  static const char *const unit[] = {"ns", G_MICRO "s", "ms", "s"};
  if (!rate_hz) {
    snprintf(out, n, "-");
    return;
  }
  uint64_t ns = samples * 1000000000ull / rate_hz;
  unsigned e = 0;
  uint64_t scale = 1;
  while (e < 3 && ns >= scale * 1000u) {
    scale *= 1000u;
    e++;
  }
  fmt3(out, n, ns, scale, unit[e]);
}

// ---------------------------------------------------------------------------
// Status block
// ---------------------------------------------------------------------------

static void draw_title(const char *title, const char *right) {
  gfx_fill(0, 0, GFX_W, TITLE_H, C_BAR);
  gfx_text(6, 0, &font_l, title, C_TEXT, C_BAR);
  if (right) gfx_text(GFX_W - 6 - gfx_text_width(&font_s, right), 4, &font_s, right, C_DIM, C_BAR);
}

static void draw_usb_badge(void) {
  bool up = tud_mounted();
  const char *s = up ? "USB" : "NO USB";
  int w = gfx_text_width(&font_s, s) + 10;
  int x = GFX_W - 4 - w;
  uint16_t bg = up ? RGB(25, 90, 45) : RGB(110, 30, 30);
  gfx_fill(x, 3, w, 18, bg);
  gfx_text(x + 5, 4, &font_s, s, C_TEXT, bg);
}

static void state_text(char *s, size_t n, uint16_t *color) {
  *color = C_TEXT;
  switch (g_la.state) {
    case LA_WAIT_TRIGGER: snprintf(s, n, "Wait trigger"); *color = C_WARN; break;
    case LA_CAPTURING: snprintf(s, n, "Capturing"); *color = C_BUSY; break;
    case LA_SENDING: snprintf(s, n, "Uploading"); *color = C_BUSY; break;
    case LA_DONE:
      snprintf(s, n, "Done #%lu", (unsigned long)g_la.captures);
      *color = g_la.overrun ? C_WARN : C_OK;
      break;
    case LA_ABORTED: snprintf(s, n, "Aborted"); *color = C_WARN; break;
    case LA_STREAMING: snprintf(s, n, "Streaming"); *color = C_BUSY; break;
    case LA_STREAM_DONE: snprintf(s, n, "Stream done"); *color = C_OK; break;
    case LA_STREAM_OVERFLOW: snprintf(s, n, "Overflow"); *color = C_ERR; break;
    default: snprintf(s, n, "Ready"); break;
  }
}

static void info1_text(char *s, size_t n) {
  char hz[16], cnt[16];
  if (!g_la.rate_hz) {
    fmt_si(hz, sizeof hz, CAPTURE_MAX_RATE_HZ, "Hz");
    fmt_si(cnt, sizeof cnt, capture_max_samples(), "");
    snprintf(s, n, "Max %s, %s samples", hz, cnt);
    return;
  }
  fmt_si(hz, sizeof hz, g_la.rate_hz, "Hz");
  if (g_la.streaming) {
    if (g_la.state == LA_STREAMING) {
      snprintf(s, n, "%s stream, %uch", hz, (unsigned)g_la.stream_channels);
    } else {
      fmt_si(cnt, sizeof cnt, g_la.stream_samples, "");
      snprintf(s, n, "%s %uch, %s smp", hz, (unsigned)g_la.stream_channels, cnt);
    }
    return;
  }
  fmt_si(cnt, sizeof cnt, g_la.samples, "");
  if (g_la.num_stages && g_la.samples)
    snprintf(s, n, "%s%s  %s smp  pre %lu%%", g_la.rate_exact ? "" : "~", hz, cnt,
             (unsigned long)((uint64_t)g_la.pre * 100u / g_la.samples));
  else
    snprintf(s, n, "%s%s  %s samples", g_la.rate_exact ? "" : "~", hz, cnt);
}

// Second line: problems first, otherwise the trigger setup
static void info2_text(char *s, size_t n, uint16_t *color) {
  *color = C_DIM;
  if (!g_clock_hse_ok) {
    snprintf(s, n, "HSE failed: USB unreliable");
    *color = C_ERR;
  } else if (!capture_sdram_ok()) {
    snprintf(s, n, "SDRAM failed: 60k max");
    *color = C_ERR;
  } else if (g_la.state == LA_DONE && g_la.overrun) {
    capture_window_t w = capture_last_window();
    if (w.lost > 0) snprintf(s, n, "OVERRUN: %ld samples lost", (long)w.lost);
    else snprintf(s, n, "OVERRUN: samples lost");
    *color = C_ERR;
  } else if (g_la.rle_requested && !g_la.streaming) {
    snprintf(s, n, "RLE unsupported: CH7 = 0");
    *color = C_WARN;
  } else if (g_la.rate_hz && !g_la.rate_exact && !g_la.streaming) {
    snprintf(s, n, "Rate not exact: time axis off");
    *color = C_WARN;
  } else if (g_la.streaming) {
    snprintf(s, n, "Trigger: in PulseView");
  } else if (!g_la.rate_hz) {
    snprintf(s, n, "PulseView: OLS / RPi Pico");
  } else if (!g_la.num_stages) {
    snprintf(s, n, "Trigger: none");
  } else {
    size_t k = (size_t)snprintf(s, n, "Trig");
    for (unsigned ch = 0; ch < LA_NUM_CH && k + 4 < n; ch++) {
      if (!((g_la.mask0 >> ch) & 1u)) continue;
      k += (size_t)snprintf(s + k, n - k, " %u%s", ch, ((g_la.value0 >> ch) & 1u) ? G_UP : G_DOWN);
    }
    if (g_la.num_stages > 1 && k < n) snprintf(s + k, n - k, " +%u", (unsigned)(g_la.num_stages - 1u));
    *color = C_TEXT;
  }
}

static void draw_status(void) {
  char line[64];
  uint16_t color;
  draw_title(LA_FW_NAME, NULL);
  draw_usb_badge();
  gfx_fill(0, TITLE_H, GFX_W, STATUS_H - TITLE_H, C_BG);
  state_text(line, sizeof line, &color);
  gfx_text_box(6, STATE_Y, GFX_W - 12, &font_l, line, color, C_BG);
  info1_text(line, sizeof line);
  gfx_text_box(6, INFO1_Y, GFX_W - 12, &font_s, line, C_TEXT, C_BG);
  info2_text(line, sizeof line, &color);
  gfx_text_box(6, INFO2_Y, GFX_W - 12, &font_s, line, color, C_BG);
}

// ---------------------------------------------------------------------------
// Channel rows: live level + waveform
// ---------------------------------------------------------------------------

static uint16_t dim(uint16_t c, unsigned a16) {  // c scaled towards C_BG by a16/16
  int r = (c >> 11) * (int)a16 / 16, g = ((c >> 5) & 63) * (int)a16 / 16, b = (c & 31) * (int)a16 / 16;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static void draw_level(unsigned ch) {
  int y = ROWS_Y + (int)ch * ROW_H + 3;
  bool hi = (s_lv_hi >> ch) & 1u, lo = (s_lv_lo >> ch) & 1u;
  uint16_t c = s_ch_color[ch];
  gfx_fill(LEVEL_X, y, 12, 10, C_BG);
  if (hi && lo) {  // toggling: striped
    for (int i = 0; i < 12; i += 3) gfx_fill(LEVEL_X + i, y, 2, 10, c);
  } else if (hi) {
    gfx_fill(LEVEL_X, y, 12, 10, c);
  } else {
    gfx_rect(LEVEL_X, y, 12, 10, dim(c, 9));
  }
}

static void draw_row_labels(void) {
  for (unsigned ch = 0; ch < LA_NUM_CH; ch++) {
    char d[2] = {(char)('0' + ch), 0};
    gfx_text(1, ROWS_Y + (int)ch * ROW_H, &font_s, d, s_ch_color[ch], C_BG);
    draw_level(ch);
  }
}

static void draw_footer(void) {
  char line[48], span[16];
  gfx_fill(0, FOOT_Y, GFX_W, GFX_H - FOOT_Y, C_BG);
  if (s_view.ready) {
    // view span and zoom on the left, key hint on the right
    fmt_time(span, sizeof span, s_view.count, s_view.win.rate_hz);
    snprintf(line, sizeof line, "%s " G_TIMES "%lu", span, (unsigned long)s_view.zoom);
    gfx_text(2, FOOT_Y, &font_s, line, C_TEXT, C_BG);
  }
  const char *keys = s_view.ready ? "K2 zoom" : "K1 page  K2 zoom";
  gfx_text(GFX_W - 2 - gfx_text_width(&font_s, keys), FOOT_Y, &font_s, keys, C_DIM, C_BG);
}

static void draw_waves(void) {
  gfx_fill(WAVE_X, ROWS_Y - 2, WAVE_W, LA_NUM_CH * ROW_H + 2, C_BG);
  if (!s_view.ready) {
    const char *msg = s_view.active ? "Preparing preview..." : "No capture yet";
    int x = WAVE_X + (WAVE_W - gfx_text_width(&font_s, msg)) / 2;
    gfx_text(x, ROWS_Y + 3 * ROW_H + ROW_H / 2, &font_s, msg, C_DIM, C_BG);
    return;
  }
  // Trigger position
  int tcol = -1;
  if (s_view.win.trigger >= 0) {
    int64_t rel = (int64_t)s_view.win.trigger - s_view.from;
    if (rel >= 0 && rel < (int64_t)s_view.count) tcol = (int)(rel * WAVE_W / s_view.count);
  }
  if (tcol >= 0) {
    for (int y = ROWS_Y - 2; y < ROWS_Y + LA_NUM_CH * ROW_H; y += 2) gfx_fill(WAVE_X + tcol, y, 1, 1, C_TRIG);
    gfx_fill(WAVE_X + tcol - 2, ROWS_Y - 4, 5, 2, C_WARN);
  }
  for (unsigned ch = 0; ch < LA_NUM_CH; ch++) {
    int top = ROWS_Y + (int)ch * ROW_H;
    int yh = top + 3, yl = top + 12;
    uint16_t c = s_ch_color[ch], fill = dim(c, 4);
    int prev = -1;  // 0 low, 1 high, 2 toggling
    for (int col = 0; col < WAVE_W; col++) {
      int x = WAVE_X + col;
      bool o = (s_view.col_or[col] >> ch) & 1u, a = (s_view.col_and[col] >> ch) & 1u;
      int st = a ? 1 : !o ? 0 : 2;
      if (st == 1) {
        gfx_vline(x, yh + 1, yl - yh, fill);
        g_fb[yh][x] = c;
      } else if (st == 0) {
        g_fb[yl][x] = c;
      }
      if (st == 2 || (prev >= 0 && prev != 2 && st != prev)) gfx_vline(x, yh, yl - yh + 1, c);
      prev = st;
    }
  }
}

// Start computing the preview for the current zoom level.
static void view_start(void) {
  capture_window_t w = s_view.win;
  uint32_t count = w.total / s_view.zoom;
  if (count < WAVE_W) count = WAVE_W;
  if (count > w.total) count = w.total;
  uint32_t centre = w.trigger >= 0 ? (uint32_t)w.trigger : w.total / 2u;
  uint32_t from = centre > count / 2u ? centre - count / 2u : 0;
  if (from + count > w.total) from = w.total - count;
  s_view.from = from;
  s_view.count = count;
  s_view.col = 0;
  s_view.active = w.total >= 2u;
  s_view.ready = false;
}

static void view_work(void) {
  if (!s_view.active) return;
  uint32_t t0 = cycles();
  capture_window_t *w = &s_view.win;
  while (s_view.col < WAVE_W && (uint32_t)(cycles() - t0) < JOB_CYCLES) {
    unsigned c = s_view.col;
    // Columns cover [c * count / W, (c + 1) * count / W); with fewer samples
    // than columns, each sample is repeated.
    uint64_t a = (uint64_t)c * s_view.count / WAVE_W, b = (uint64_t)(c + 1u) * s_view.count / WAVE_W;
    if (b <= a) b = a + 1u;
    capture_or_and(w->start + s_view.from + (uint32_t)a, (uint32_t)(b - a), &s_view.col_or[c], &s_view.col_and[c]);
    s_view.col++;
  }
  if (s_view.col >= WAVE_W) {
    s_view.active = false;
    s_view.ready = true;
    if (s_page == PAGE_MAIN) {
      draw_waves();
      draw_footer();
    }
  }
}

// ---------------------------------------------------------------------------
// Other pages
// ---------------------------------------------------------------------------

static void draw_pins(void) {
  static const char *const pin[LA_NUM_CH] = {"PI0 JD2-15", "PI1 JD2-9",  "PI2 JD2-11", "PI3 JD2-13",
                                             "PI4 JD1-13", "PI5 JD1-3",  "PI6 JD1-14", "PI7 JD1-15"};
  gfx_fill(0, 0, GFX_W, GFX_H, C_BG);
  draw_title("Pins", "2/3");
  for (unsigned ch = 0; ch < LA_NUM_CH; ch++) {
    int x = ch < 4 ? 4 : 122, y = 30 + (int)(ch % 4u) * 18;
    char d[2] = {(char)('0' + ch), 0};
    gfx_text(x, y, &font_s, d, s_ch_color[ch], C_BG);
    gfx_text(x + 12, y, &font_s, pin[ch], C_TEXT, C_BG);
  }
  gfx_text(4, 104, &font_s, "GND JD1-2, JD2-3/23/24", C_TEXT, C_BG);
  gfx_text(4, 122, &font_s, "Inputs: 3.3 V logic only", C_WARN, C_BG);
  gfx_hline(4, 144, GFX_W - 8, C_GRID);
  gfx_text(4, 150, &font_s, "Test signals", C_DIM, C_BG);
  gfx_text(4, 168, &font_s, "PB0 JD2-10  100 kHz", C_TEXT, C_BG);
  gfx_text(4, 186, &font_s, "PB1 JD2-12  1 MHz", C_TEXT, C_BG);
  gfx_text(4, 204, &font_s, "PA5 JD2-16  1 kHz", C_TEXT, C_BG);
  gfx_text(4, 222, &font_s, "PA9 JD2-7   UART 115200", C_TEXT, C_BG);
}

static void draw_info(void) {
  char line[48], a[16], b[16];
  gfx_fill(0, 0, GFX_W, GFX_H, C_BG);
  draw_title("Info", "3/3");
  int y = 30;
  snprintf(line, sizeof line, "%s v%s", LA_FW_NAME, LA_FW_VERSION);
  gfx_text(4, y, &font_s, line, C_TEXT, C_BG);
  y += 18;
  gfx_text(4, y, &font_s, "CPU 120 MHz, HSE", C_TEXT, C_BG);
  gfx_text(4 + 17 * 8, y, &font_s, g_clock_hse_ok ? "ok" : "FAIL", g_clock_hse_ok ? C_OK : C_ERR, C_BG);
  y += 18;
  gfx_text(4, y, &font_s, "SDRAM 32 MB", C_TEXT, C_BG);
  gfx_text(4 + 12 * 8, y, &font_s, capture_sdram_ok() ? "ok" : "FAIL", capture_sdram_ok() ? C_OK : C_ERR, C_BG);
  y += 18;
  fmt_si(a, sizeof a, CAPTURE_MAX_RATE_HZ, "Hz");
  fmt_si(b, sizeof b, capture_max_samples(), "");
  snprintf(line, sizeof line, "Max %s, %s samples", a, b);
  gfx_text(4, y, &font_s, line, C_TEXT, C_BG);
  y += 24;

  gfx_text(4, y, &font_s, "DMA test (MHz)", C_DIM, C_BG);
  y += 18;
  static const char *const mem[2] = {"SRAM ", "SDRAM"};
  for (int m = 0; m < 2; m++) {
    int x = gfx_text(4, y, &font_s, mem[m], C_TEXT, C_BG) + 8;
    for (unsigned r = 0; r < SELFTEST_RATES; r++) {
      int32_t l0 = g_selftest.lost[r][m][0], l1 = g_selftest.lost[r][m][1];
      snprintf(line, sizeof line, "%lu", (unsigned long)(CAPTURE_TIMER_HZ / g_selftest.ticks[r] / 1000000u));
      uint16_t c = (l0 < 0 || l1 < 0) ? C_DIM : (l0 == 0 && l1 == 0) ? C_OK : C_ERR;
      x = gfx_text(x, y, &font_s, line, c, C_BG) + 8;
    }
    y += 18;
  }
  y += 6;
  snprintf(line, sizeof line, "Captures %lu", (unsigned long)g_la.captures);
  gfx_text(4, y, &font_s, line, C_TEXT, C_BG);
  y += 18;
  gfx_text(4, y, &font_s, "USB 1209:0001 CDC", C_TEXT, C_BG);
}

static void draw_page(void) {
  if (s_page == PAGE_MAIN) {
    gfx_fill(0, 0, GFX_W, GFX_H, C_BG);
    draw_status();
    draw_row_labels();
    draw_waves();
    draw_footer();
    s_levels_drawn = true;
  } else if (s_page == PAGE_PINS) {
    draw_pins();
  } else {
    draw_info();
  }
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

static void flush(bool wait) {
  unsigned y0, y1;
  if (lcd_busy()) {
    if (!wait) return;
    lcd_wait();
  }
  if (gfx_take_dirty(&y0, &y1)) lcd_flush_rows(&g_fb[0][0], y0, y1);
  if (wait) lcd_wait();
}

void ui_init(void) {
  memset(&g_la, 0, sizeof g_la);
  memset(&s_view, 0, sizeof s_view);
  s_page = PAGE_MAIN;
  draw_page();
  flush(true);
  lcd_display_on();
}

void ui_levels(uint8_t v) {
  s_seen_hi |= v;
  s_seen_lo |= (uint8_t)~v;
}

void ui_redraw_status(void) {
  la_state_t st = g_la.state;
  if (st == LA_WAIT_TRIGGER || st == LA_CAPTURING || st == LA_STREAMING) s_view.active = false;  // ring reused
  if (st == LA_DONE) {
    capture_window_t w = capture_last_window();
    if (w.valid) {
      s_view.win = w;
      s_view.zoom = 1;
      view_start();
    }
  }
  if (s_page == PAGE_MAIN) {
    draw_status();
    if (st == LA_DONE) {
      draw_waves();
      draw_footer();
    }
  }
  flush(true);  // the LCD DMA must be idle before sampling starts
}

void ui_poll(void) {
  uint8_t k = keys_poll();
  if (k & KEY_1) {
    s_page = (page_t)((s_page + 1) % PAGE_COUNT);
    draw_page();
  }
  if ((k & KEY_2) && s_page == PAGE_MAIN && s_view.win.valid && !s_view.active) {
    // Zoom in 4x around the trigger until one sample per pixel, then back out
    s_view.zoom = s_view.win.total / s_view.zoom / 4u >= WAVE_W ? s_view.zoom * 4u : 1u;
    view_start();
    draw_waves();
    draw_footer();
  }

  view_work();

  uint32_t now = millis();
  if ((uint32_t)(now - s_frame_ms) >= FRAME_MS) {
    s_frame_ms = now;
    uint8_t hi = s_seen_hi, lo = s_seen_lo;
    s_seen_hi = s_seen_lo = 0;
    if (s_page == PAGE_MAIN) {
      uint8_t changed = (uint8_t)((hi ^ s_lv_hi) | (lo ^ s_lv_lo));
      s_lv_hi = hi;
      s_lv_lo = lo;
      for (unsigned ch = 0; ch < LA_NUM_CH; ch++)
        if (((changed >> ch) & 1u) || !s_levels_drawn) draw_level(ch);
      s_levels_drawn = true;
      static bool usb_was;
      bool usb = tud_mounted();
      if (usb != usb_was) {
        usb_was = usb;
        draw_usb_badge();
      }
    }
  }
  flush(false);
}
