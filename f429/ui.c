#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "ch455.h"
#include "clock.h"
#include "gen.h"
#include "lcd.h"
#include "selftest.h"
#include "tusb.h"
#include "ui_priv.h"

la_status_t g_la;

// Channel colours: resistor colour code as in PulseView (black -> grey)
const uint16_t ui_ch_color[LA_NUM_CH] = {
    RGB(175, 175, 175), RGB(200, 130, 80), RGB(240, 75, 75),  RGB(245, 150, 45),
    RGB(240, 220, 50),  RGB(85, 210, 105), RGB(85, 155, 245), RGB(185, 125, 245),
};

#define FRAME_MS 100u  // live levels / USB badge
#define SEG7_MS  250u

static const ui_page_t *const s_pages[PAGE_COUNT] = {
    &ui_page_wave, &ui_page_setup, &ui_page_meter, &ui_page_decode,
    &ui_page_gen,  &ui_page_pins,  &ui_page_info,  &ui_page_menu,
};

static page_id_t s_page = PAGE_WAVE, s_back = PAGE_WAVE;
static uint8_t s_seen_hi, s_seen_lo;
static uint32_t s_frame_ms, s_seg7_ms;
static char s_seg7[9];
static bool s_capture_request;

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------

void ui_fmt_digits(char *out, size_t n, uint64_t v, uint64_t scale, unsigned digits, bool trim, const char *suffix) {
  uint64_t ip = v / scale;
  unsigned int_digits = 1;
  for (uint64_t x = ip; x >= 10u; x /= 10u) int_digits++;
  unsigned dec = digits > int_digits ? digits - int_digits : 0;
  uint64_t p = 1;
  for (unsigned i = 0; i < dec; i++) p *= 10u;
  if (scale == 1u) dec = 0, p = 1;
  uint64_t q = (v * p + scale / 2u) / scale;
  unsigned long whole = (unsigned long)(q / p);
  unsigned long frac = (unsigned long)(q % p);
  if (trim)
    while (dec && frac % 10u == 0) {
      frac /= 10u;
      dec--;
    }
  if (!dec) snprintf(out, n, "%lu%s", whole, suffix);
  else snprintf(out, n, "%lu.%0*lu%s", whole, (int)dec, frac, suffix);
}

static void fmt_scaled(char *out, size_t n, uint64_t v, const char *const *units, unsigned nunits, unsigned digits,
                       bool trim) {
  unsigned e = 0;
  uint64_t scale = 1;
  while (e + 1u < nunits && v >= scale * 1000u) {
    scale *= 1000u;
    e++;
  }
  ui_fmt_digits(out, n, v, scale, digits, trim, units[e]);
}

void ui_fmt_si(char *out, size_t n, uint64_t v, const char *unit) {
  char u[4][16];
  static const char *const pre[] = {"", "k", "M", "G"};
  const char *units[4];
  for (unsigned i = 0; i < 4; i++) {
    snprintf(u[i], sizeof u[i], "%s%s", pre[i], unit);
    units[i] = u[i];
  }
  fmt_scaled(out, n, v, units, 4, 3, true);
}

void ui_fmt_hz_milli(char *out, size_t n, uint64_t mhz, unsigned digits) {
  static const char *const units[] = {"mHz", "Hz", "kHz", "MHz"};
  fmt_scaled(out, n, mhz, units, 4, digits, false);
}

void ui_fmt_time(char *out, size_t n, uint64_t samples, uint32_t rate_hz) {
  static const char *const units[] = {"ns", G_MICRO "s", "ms", "s"};
  if (!rate_hz) {
    snprintf(out, n, "-");
    return;
  }
  fmt_scaled(out, n, samples * 1000000000ull / rate_hz, units, 4, 3, true);
}

void ui_fmt_stime(char *out, size_t n, int64_t samples, uint32_t rate_hz) {
  if (!samples) {
    snprintf(out, n, "0");
    return;
  }
  out[0] = samples < 0 ? '-' : '+';
  ui_fmt_time(out + 1, n - 1, (uint64_t)(samples < 0 ? -samples : samples), rate_hz);
}

void ui_seg7_khz(char *out, uint64_t mhz) {
  // kHz with 4 significant digits: 0.001 .. 9999.
  uint64_t hz1000 = mhz;  // millihertz = 1e-6 kHz units
  if (hz1000 >= 10000000000ull) {
    strcpy(out, "----");  // >= 10 MHz does not fit
    return;
  }
  // value in units of 1e-6 kHz; choose decimals so that 4 digits show
  uint64_t div = 1000000;  // 1 kHz
  unsigned dec = 0;
  uint64_t ip = hz1000 / div;
  if (ip < 10) dec = 3;
  else if (ip < 100) dec = 2;
  else if (ip < 1000) dec = 1;
  uint64_t p = dec == 3 ? 1000 : dec == 2 ? 100 : dec == 1 ? 10 : 1;
  uint64_t q = (hz1000 * p + div / 2) / div;
  if (dec) snprintf(out, 9, "%lu.%0*lu", (unsigned long)(q / p), (int)dec, (unsigned long)(q % p));
  else snprintf(out, 9, "%lu.", (unsigned long)q);
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

void ui_title(const char *title, const char *right) {
  gfx_fill(0, 0, GFX_W, TITLE_H, C_BAR);
  gfx_text(6, 0, &font_l, title, C_TEXT, C_BAR);
  if (right) gfx_text(GFX_W - 6 - gfx_text_width(&font_s, right), 4, &font_s, right, C_DIM, C_BAR);
}

void ui_hint(const char *text) { gfx_text_box(2, HINT_Y, GFX_W - 4, &font_s, text, C_DIM, C_BG); }

void ui_row(int y, const char *text, uint16_t color, bool selected) {
  uint16_t bg = selected ? C_SEL : C_BG;
  gfx_fill(0, y - 1, GFX_W, LINE_H, bg);
  gfx_text_box(4, y, GFX_W - 8, &font_s, text, color, bg);
}

void ui_draw_usb_badge(void) {
  bool up = tud_mounted();
  const char *s = up ? "USB" : "NO USB";
  int w = gfx_text_width(&font_s, s) + 10;
  int x = GFX_W - 4 - w;
  uint16_t bg = up ? RGB(25, 90, 45) : RGB(110, 30, 30);
  gfx_fill(x - 60, 3, 60, 18, C_BAR);
  gfx_fill(x, 3, w, 18, bg);
  gfx_text(x + 5, 4, &font_s, s, C_TEXT, bg);
}

// ---------------------------------------------------------------------------
// Status block (waveform page)
// ---------------------------------------------------------------------------

#define STATE_Y 26
#define INFO1_Y 52
#define INFO2_Y 68

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
    ui_fmt_si(hz, sizeof hz, CAPTURE_MAX_RATE_HZ, "Hz");
    ui_fmt_si(cnt, sizeof cnt, capture_max_samples(), "");
    snprintf(s, n, "Max %s, %s samples", hz, cnt);
    return;
  }
  ui_fmt_si(hz, sizeof hz, g_la.rate_hz, "Hz");
  if (g_la.streaming) {
    if (g_la.state == LA_STREAMING) {
      snprintf(s, n, "%s stream, %uch", hz, (unsigned)g_la.stream_channels);
    } else {
      ui_fmt_si(cnt, sizeof cnt, g_la.stream_samples, "");
      snprintf(s, n, "%s %uch, %s smp", hz, (unsigned)g_la.stream_channels, cnt);
    }
    return;
  }
  ui_fmt_si(cnt, sizeof cnt, g_la.samples, "");
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
    snprintf(s, n, "PulseView, or B = run here");
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

void ui_draw_status(void) {
  char line[64];
  uint16_t color;
  ui_title(LA_FW_NAME, NULL);
  ui_draw_usb_badge();
  gfx_fill(0, TITLE_H, GFX_W, STATUS_H - TITLE_H, C_BG);
  state_text(line, sizeof line, &color);
  gfx_text_box(6, STATE_Y, GFX_W - 12, &font_l, line, color, C_BG);
  info1_text(line, sizeof line);
  gfx_text_box(6, INFO1_Y, GFX_W - 12, &font_s, line, C_TEXT, C_BG);
  info2_text(line, sizeof line, &color);
  gfx_text_box(6, INFO2_Y, GFX_W - 12, &font_s, line, color, C_BG);
}

// ---------------------------------------------------------------------------
// Menu, pins and info pages
// ---------------------------------------------------------------------------

static const char *const s_menu_items[] = {"Waveform", "Capture setup", "Meter", "Decode", "Generator", "Pins", "Info"};

static void menu_draw(void) {
  char line[40];
  gfx_fill(0, 0, GFX_W, GFX_H, C_BG);
  ui_title("Menu", NULL);
  for (unsigned i = 0; i < sizeof s_menu_items / sizeof s_menu_items[0]; i++) {
    snprintf(line, sizeof line, "%u  %s", i + 1u, s_menu_items[i]);
    ui_row(32 + (int)i * 24, line, i == (unsigned)s_back ? C_BUSY : C_TEXT, false);
  }
  ui_hint("1-7 open   * back");
}

static bool menu_key(uint8_t k) {
  if (k >= '1' && k <= '7') {
    ui_goto((page_id_t)(k - '1'));
    return true;
  }
  if (k == '*' || k == 'A') {
    ui_goto(s_back);
    return true;
  }
  return false;
}

const ui_page_t ui_page_menu = {"Menu", 0, 0, menu_draw, menu_key, 0, 0};

static void pins_draw(void) {
  static const char *const pin[LA_NUM_CH] = {"PI0 JD2-15", "PI1 JD2-9",  "PI2 JD2-11", "PI3 JD2-13",
                                             "PI4 JD1-13", "PI5 JD1-3",  "PI6 JD1-14", "PI7 JD1-15"};
  char line[40], f[16];
  gfx_fill(0, 0, GFX_W, GFX_H, C_BG);
  ui_title("Pins", NULL);
  for (unsigned ch = 0; ch < LA_NUM_CH; ch++) {
    int x = ch < 4 ? 4 : 122, y = 30 + (int)(ch % 4u) * 18;
    char d[2] = {(char)('0' + ch), 0};
    gfx_text(x, y, &font_s, d, ui_ch_color[ch], C_BG);
    gfx_text(x + 12, y, &font_s, pin[ch], C_TEXT, C_BG);
  }
  gfx_text(4, 104, &font_s, "GND JD1-2, JD2-3/23/24", C_TEXT, C_BG);
  gfx_text(4, 122, &font_s, "Inputs: 3.3 V logic only", C_WARN, C_BG);
  gfx_hline(4, 144, GFX_W - 8, C_GRID);
  gfx_text(4, 148, &font_s, "Generator outputs", C_DIM, C_BG);
  for (unsigned i = 0; i < GEN_PWM_OUTPUTS; i++) {
    ui_fmt_hz_milli(f, sizeof f, gen_actual_mhz(i), 4);
    snprintf(line, sizeof line, "%s  %s", gen_pin_name[i], g_gen.pwm[i].on ? f : "off");
    gfx_text(4, 166 + (int)i * 18, &font_s, line, C_TEXT, C_BG);
  }
  snprintf(line, sizeof line, "%s   UART %lu", gen_pin_name[3], (unsigned long)g_gen.uart.baud);
  gfx_text(4, 220, &font_s, g_gen.uart.on ? line : "PA9 JD2-7   UART off", C_TEXT, C_BG);
}

const ui_page_t ui_page_pins = {"Pins", 0, 0, pins_draw, 0, 0, 0};

static void info_draw(void) {
  char line[48], a[16], b[16];
  gfx_fill(0, 0, GFX_W, GFX_H, C_BG);
  ui_title("Info", NULL);
  int y = 28;
  snprintf(line, sizeof line, "%s v%s, CPU 120 MHz", LA_FW_NAME, LA_FW_VERSION);
  gfx_text(4, y, &font_s, line, C_TEXT, C_BG);
  y += 18;
  gfx_text(4, y, &font_s, "HSE", C_TEXT, C_BG);
  gfx_text(4 + 4 * 8, y, &font_s, g_clock_hse_ok ? "ok" : "FAIL", g_clock_hse_ok ? C_OK : C_ERR, C_BG);
  gfx_text(4 + 8 * 8, y, &font_s, "SDRAM 32MB", C_TEXT, C_BG);
  gfx_text(4 + 19 * 8, y, &font_s, capture_sdram_ok() ? "ok" : "FAIL", capture_sdram_ok() ? C_OK : C_ERR, C_BG);
  y += 18;
  ui_fmt_si(a, sizeof a, CAPTURE_MAX_RATE_HZ, "Hz");
  ui_fmt_si(b, sizeof b, capture_max_samples(), "");
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
      snprintf(line, sizeof line, "%lu", (unsigned long)(g_selftest.ticks[r] ? CAPTURE_TIMER_HZ / g_selftest.ticks[r] / 1000000u : 0));
      uint16_t c = (l0 < 0 || l1 < 0) ? C_DIM : (l0 == 0 && l1 == 0) ? C_OK : C_ERR;
      x = gfx_text(x, y, &font_s, line, c, C_BG) + 8;
    }
    y += 18;
  }
  y += 6;
  snprintf(line, sizeof line, "Captures %lu", (unsigned long)g_la.captures);
  gfx_text(4, y, &font_s, line, C_TEXT, C_BG);
  y += 18;
  int code = input_last_code();
  if (!input_keypad_ok()) snprintf(line, sizeof line, "Keypad (CH455): not found");
  else if (code < 0) snprintf(line, sizeof line, "Keypad ok, press a key");
  else snprintf(line, sizeof line, "Keypad ok, last code 0x%02X", (unsigned)code);
  gfx_text(4, y, &font_s, line, input_keypad_ok() ? C_TEXT : C_ERR, C_BG);
  y += 18;
  gfx_text(4, y, &font_s, "USB 1209:0001 CDC", C_TEXT, C_BG);
}

// Key test: show the raw code of every key press
static bool info_key(uint8_t k) {
  (void)k;
  return false;
}

static void info_poll(void) {
  static int last = -2;
  int code = input_last_code();
  if (code != last) {
    last = code;
    info_draw();
  }
}

const ui_page_t ui_page_info = {"Info", 0, 0, info_draw, info_key, info_poll, 0};

// ---------------------------------------------------------------------------
// Core
// ---------------------------------------------------------------------------

page_id_t ui_current(void) { return s_page; }

void ui_goto(page_id_t p) {
  if (p >= PAGE_COUNT) p = PAGE_WAVE;
  if (p == PAGE_MENU && s_page != PAGE_MENU) s_back = s_page;
  if (s_pages[s_page]->leave) s_pages[s_page]->leave();
  s_page = p;
  if (s_pages[p]->enter) s_pages[p]->enter();
  s_pages[p]->draw();
  s_seg7_ms = millis() - SEG7_MS;  // refresh the digits now
}

bool ui_capture_busy(void) {
  la_state_t st = g_la.state;
  return st == LA_WAIT_TRIGGER || st == LA_CAPTURING || st == LA_SENDING || st == LA_STREAMING;
}

void ui_request_capture(void) { s_capture_request = true; }

bool ui_take_capture(capture_cfg_t *cfg) {
  if (!s_capture_request) return false;
  s_capture_request = false;
  if (!setup_take_request(cfg)) return false;
  if (s_page != PAGE_WAVE) ui_goto(PAGE_WAVE);  // show the state of the capture
  return true;
}

static void flush(bool wait) {
  unsigned y0, y1;
  if (lcd_busy()) {
    if (!wait) return;
    lcd_wait();
  }
  if (gfx_take_dirty(&y0, &y1)) lcd_flush_rows(&g_fb[0][0], y0, y1);
  if (wait) lcd_wait();
}

static void seg7_update(void) {
  char buf[9] = "----";
  const ui_page_t *pg = s_pages[s_page];
  if (pg->seg7) pg->seg7(buf);
  if (strcmp(buf, s_seg7) != 0) {
    strcpy(s_seg7, buf);
    ch455_text(buf);
  }
}

void ui_init(void) {
  memset(&g_la, 0, sizeof g_la);
  s_page = PAGE_WAVE;
  s_pages[s_page]->draw();
  flush(true);
  lcd_display_on();
  seg7_update();
}

void ui_levels(uint8_t v) {
  s_seen_hi |= v;
  s_seen_lo |= (uint8_t)~v;
}

void ui_redraw_status(void) {
  la_state_t st = g_la.state;
  if (st == LA_WAIT_TRIGGER || st == LA_CAPTURING || st == LA_STREAMING) {
    wave_capture_started();
    ch455_text(" run");
    strcpy(s_seg7, " run");
  }
  if (st == LA_DONE) wave_capture_done();
  if (s_page == PAGE_WAVE) {
    // Only the status block while a capture starts or uploads (keeps the
    // delay before sampling short); everything once it is over.
    if (st == LA_DONE || st == LA_ABORTED || st == LA_STREAM_DONE || st == LA_STREAM_OVERFLOW) ui_page_wave.draw();
    else ui_draw_status();
  }
  flush(true);  // the LCD DMA must be idle before sampling starts
}

void ui_poll(void) {
  uint8_t k = input_poll();
  if (k) {
    const ui_page_t *pg = s_pages[s_page];
    if (!(pg->key && pg->key(k))) {
      if (k == 'A') ui_goto(s_page == PAGE_MENU ? s_back : PAGE_MENU);
      else if (k == KEY_BTN1) ui_goto((page_id_t)((s_page + 1) % PAGE_MENU));
      else if (k == 'B' || k == KEY_BTN2) ui_request_capture();
      else if (k == '*' && s_page != PAGE_WAVE) ui_goto(PAGE_WAVE);
    }
  }

  if (s_pages[s_page]->poll) s_pages[s_page]->poll();

  uint32_t now = millis();
  if ((uint32_t)(now - s_frame_ms) >= FRAME_MS) {
    s_frame_ms = now;
    uint8_t hi = s_seen_hi, lo = s_seen_lo;
    s_seen_hi = s_seen_lo = 0;
    if (s_page == PAGE_WAVE) wave_levels(hi, lo);
  }
  if ((uint32_t)(now - s_seg7_ms) >= SEG7_MS) {
    s_seg7_ms = now;
    seg7_update();
  }
  flush(false);
}
