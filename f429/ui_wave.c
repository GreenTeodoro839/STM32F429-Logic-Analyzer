// Waveform page: status, live levels and the last capture with pan / zoom
// and two cursors.
//
// Keys: 2/8 zoom in/out, 4/6 pan, 5 whole capture, D trigger,
//       1/3 active cursor to the previous/next edge of the selected channel,
//       7/9 move the active cursor by one pixel, 0 next channel,
//       # switch cursor A/B, C centre on the cursor, * remove the cursors.
#include <stdio.h>
#include <string.h>

#include "analyze.h"
#include "clock.h"
#include "tusb.h"
#include "ui_priv.h"

#define ROWS_Y  88
#define ROW_H   15
#define LEVEL_X 12
#define WAVE_X  28
#define WAVE_W  (GFX_W - WAVE_X - 2)
#define FOOT_Y  (ROWS_Y + LA_NUM_CH * ROW_H + 1)  // two lines: FOOT_Y, FOOT_Y + 16
#define JOB_CYCLES (120000000u / 1000u * 3u)       // preview work per poll: 3 ms
#define MIN_COUNT (WAVE_W / 16u)                    // deepest zoom: 16 px per sample

static struct {
  capture_window_t win;
  uint32_t from, count;  // window-relative sample range shown
  unsigned col;          // next column to compute
  bool active, ready;
  uint8_t col_or[WAVE_W], col_and[WAVE_W];
} s_view;

static uint32_t s_cur[2] = {ANA_NONE, ANA_NONE};  // cursors (window indices)
static unsigned s_active;                          // active cursor
static unsigned s_sel_ch;                          // channel for edge jumps
static uint8_t s_lv_hi, s_lv_lo;
static bool s_levels_drawn;

static uint16_t dim(uint16_t c, unsigned a16) {
  int r = (c >> 11) * (int)a16 / 16, g = ((c >> 5) & 63) * (int)a16 / 16, b = (c & 31) * (int)a16 / 16;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// ---------------------------------------------------------------------------
// View range and preview computation
// ---------------------------------------------------------------------------

static void view_clamp(void) {
  uint32_t total = s_view.win.total;
  uint32_t min = total < MIN_COUNT ? total : MIN_COUNT;
  if (s_view.count < min) s_view.count = min;
  if (s_view.count > total) s_view.count = total;
  if (s_view.from > total - s_view.count) s_view.from = total - s_view.count;
}

static void view_compute(void) {
  view_clamp();
  s_view.col = 0;
  s_view.active = s_view.win.valid && s_view.win.total >= 2u;
  s_view.ready = false;
}

static void view_work(void) {
  if (!s_view.active) return;
  uint32_t t0 = cycles();
  while (s_view.col < WAVE_W && (uint32_t)(cycles() - t0) < JOB_CYCLES) {
    unsigned c = s_view.col;
    // Column c covers [c * count / W, (c + 1) * count / W); with fewer samples
    // than columns a sample spans several columns.
    uint64_t a = (uint64_t)c * s_view.count / WAVE_W, b = (uint64_t)(c + 1u) * s_view.count / WAVE_W;
    if (b <= a) b = a + 1u;
    capture_or_and(s_view.win.start + s_view.from + (uint32_t)a, (uint32_t)(b - a), &s_view.col_or[c],
                   &s_view.col_and[c]);
    s_view.col++;
  }
  if (s_view.col >= WAVE_W) {
    s_view.active = false;
    s_view.ready = true;
  }
}

// Screen column of a window index, or -1 if outside the view.
static int col_of(uint32_t idx) {
  if (idx == ANA_NONE || idx < s_view.from || idx >= s_view.from + s_view.count) return -1;
  return (int)((uint64_t)(idx - s_view.from) * WAVE_W / s_view.count);
}

static uint32_t samples_per_col(void) {
  uint32_t n = s_view.count / WAVE_W;
  return n ? n : 1u;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

static void draw_level(unsigned ch) {
  int y = ROWS_Y + (int)ch * ROW_H + 3;
  bool hi = (s_lv_hi >> ch) & 1u, lo = (s_lv_lo >> ch) & 1u;
  uint16_t c = ui_ch_color[ch];
  gfx_fill(LEVEL_X, y, 12, 9, C_BG);
  if (hi && lo) {  // toggling: striped
    for (int i = 0; i < 12; i += 3) gfx_fill(LEVEL_X + i, y, 2, 9, c);
  } else if (hi) {
    gfx_fill(LEVEL_X, y, 12, 9, c);
  } else {
    gfx_rect(LEVEL_X, y, 12, 9, dim(c, 9));
  }
}

static void draw_labels(void) {
  for (unsigned ch = 0; ch < LA_NUM_CH; ch++) {
    char d[2] = {(char)('0' + ch), 0};
    bool sel = ch == s_sel_ch && (s_cur[0] != ANA_NONE || s_cur[1] != ANA_NONE);
    uint16_t bg = sel ? C_SEL : C_BG;
    gfx_fill(0, ROWS_Y + (int)ch * ROW_H, 10, ROW_H, bg);
    gfx_text(1, ROWS_Y + (int)ch * ROW_H - 1, &font_s, d, ui_ch_color[ch], bg);
    draw_level(ch);
  }
}

static int64_t rel(uint32_t idx) {  // sample offset from the trigger (or the start)
  return (int64_t)idx - (s_view.win.trigger >= 0 ? s_view.win.trigger : 0);
}

static void draw_footer(void) {
  char l1[48], l2[48], a[20], b[20];
  gfx_fill(0, FOOT_Y, GFX_W, GFX_H - FOOT_Y, C_BG);
  if (!s_view.ready) {
    gfx_text(2, FOOT_Y + 16, &font_s, "B run  A menu  K1 page", C_DIM, C_BG);
    return;
  }
  uint32_t rate = s_view.win.rate_hz;
  if (s_cur[0] == ANA_NONE && s_cur[1] == ANA_NONE) {
    ui_fmt_time(a, sizeof a, s_view.count, rate);
    ui_fmt_stime(b, sizeof b, rel(s_view.from), rate);
    snprintf(l1, sizeof l1, "View %s at %s", a, b);
    gfx_text(2, FOOT_Y, &font_s, l1, C_TEXT, C_BG);
    gfx_text(2, FOOT_Y + 16, &font_s, "2/8 zoom 4/6 pan 1/3 edge", C_DIM, C_BG);
    return;
  }
  int x = 2;
  for (unsigned i = 0; i < 2; i++) {
    if (s_cur[i] == ANA_NONE) continue;
    ui_fmt_stime(a, sizeof a, rel(s_cur[i]), rate);
    snprintf(l1, sizeof l1, "%c%s%s ", i ? 'B' : 'A', i == s_active ? "*" : " ", a);
    x = gfx_text(x, FOOT_Y, &font_s, l1, i ? C_CURB : C_CURA, C_BG);
  }
  if (s_cur[0] != ANA_NONE && s_cur[1] != ANA_NONE) {
    uint32_t d = s_cur[1] > s_cur[0] ? s_cur[1] - s_cur[0] : s_cur[0] - s_cur[1];
    ui_fmt_time(a, sizeof a, d, rate);
    if (d) ui_fmt_hz_milli(b, sizeof b, (uint64_t)rate * 1000u / d, 4);
    else strcpy(b, "-");
    snprintf(l2, sizeof l2, G_DELTA "%s  %s  ch%u", a, b, s_sel_ch);
    gfx_text(2, FOOT_Y + 16, &font_s, l2, C_TEXT, C_BG);
  } else {
    snprintf(l2, sizeof l2, "1/3 edge ch%u  # cursor B", s_sel_ch);
    gfx_text(2, FOOT_Y + 16, &font_s, l2, C_DIM, C_BG);
  }
}

static void draw_waves(void) {
  gfx_fill(WAVE_X, ROWS_Y - 4, WAVE_W, LA_NUM_CH * ROW_H + 4, C_BG);
  if (!s_view.ready) {
    const char *msg = s_view.active ? "Preparing preview..." : "No capture yet";
    int x = WAVE_X + (WAVE_W - gfx_text_width(&font_s, msg)) / 2;
    gfx_text(x, ROWS_Y + 3 * ROW_H + ROW_H / 2, &font_s, msg, C_DIM, C_BG);
    return;
  }
  const int y0 = ROWS_Y - 2, y1 = ROWS_Y + LA_NUM_CH * ROW_H;
  int tcol = s_view.win.trigger >= 0 ? col_of((uint32_t)s_view.win.trigger) : -1;
  if (tcol >= 0) {
    for (int y = y0; y < y1; y += 2) gfx_fill(WAVE_X + tcol, y, 1, 1, C_TRIG);
    gfx_fill(WAVE_X + tcol - 2, ROWS_Y - 4, 5, 2, C_WARN);
  }
  for (unsigned ch = 0; ch < LA_NUM_CH; ch++) {
    int top = ROWS_Y + (int)ch * ROW_H;
    int yh = top + 3, yl = top + 11;
    uint16_t c = ui_ch_color[ch], fill = dim(c, 4);
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
  for (unsigned i = 0; i < 2; i++) {
    int cc = col_of(s_cur[i]);
    if (cc < 0) continue;
    uint16_t c = i ? C_CURB : C_CURA;
    for (int y = y0; y < y1; y += (i == s_active ? 1 : 2)) gfx_fill(WAVE_X + cc, y, 1, 1, c);
    gfx_fill(WAVE_X + cc - 2, ROWS_Y - 4, 5, 2, c);
  }
}

static void wave_draw(void) {
  gfx_fill(0, STATUS_H, GFX_W, GFX_H - STATUS_H, C_BG);
  ui_draw_status();
  draw_labels();
  draw_waves();
  draw_footer();
  s_levels_drawn = true;
}

static void redraw_view(void) {
  draw_labels();
  draw_waves();
  draw_footer();
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

static void zoom(bool in) {
  uint32_t c = s_view.count;
  // keep the active cursor (or the middle) at the same place
  uint32_t anchor = s_cur[s_active] != ANA_NONE && col_of(s_cur[s_active]) >= 0 ? s_cur[s_active]
                                                                                  : s_view.from + c / 2u;
  uint32_t nc = in ? c / 2u : (c > 0x7FFFFFFFu ? c : c * 2u);
  if (nc < 1) nc = 1;
  uint64_t off = (uint64_t)(anchor - s_view.from) * nc / (c ? c : 1u);
  s_view.count = nc;
  s_view.from = anchor > off ? anchor - (uint32_t)off : 0;
  view_compute();
}

static void pan(int dir) {
  uint32_t step = s_view.count / 4u ? s_view.count / 4u : 1u;
  if (dir < 0) s_view.from = s_view.from > step ? s_view.from - step : 0;
  else s_view.from += step;
  view_compute();
}

static void center_on(uint32_t idx) {
  s_view.from = idx > s_view.count / 2u ? idx - s_view.count / 2u : 0;
  view_compute();
}

static void ensure_visible(uint32_t idx) {
  if (col_of(idx) < 0) center_on(idx);
}

static uint32_t cursor_or_center(void) {
  if (s_cur[s_active] == ANA_NONE) s_cur[s_active] = s_view.from + s_view.count / 2u;
  return s_cur[s_active];
}

static bool wave_key(uint8_t k) {
  if (!s_view.win.valid || !s_view.ready) return false;
  const capture_window_t *w = &s_view.win;
  switch (k) {
    case '2': zoom(true); break;
    case '8': zoom(false); break;
    case '4': pan(-1); break;
    case '6': pan(1); break;
    case '5':
      s_view.from = 0;
      s_view.count = w->total;
      view_compute();
      break;
    case 'D':
      if (w->trigger < 0) return true;
      center_on((uint32_t)w->trigger);
      break;
    case '1':
    case '3': {
      // Edge search is bounded per key press (a quiet channel in a 32M capture
      // would otherwise block for a second); without an edge the cursor
      // stops at the bound and the next press continues from there.
      const uint32_t limit = 8000000u;
      uint32_t c = cursor_or_center();
      uint8_t m = (uint8_t)(1u << s_sel_ch);
      if (k == '3' && c + 1u < w->total) {
        uint32_t end = w->total - c - 1u > limit ? c + 1u + limit : w->total;
        uint32_t j = ana_next_change(w, c + 1u, end, m, ana_sample(w, c));
        s_cur[s_active] = j < end ? j : end - 1u;
      } else if (k == '1' && c > 0) {
        uint32_t begin = c - 1u > limit ? c - 1u - limit : 0;
        uint32_t j = ana_prev_change(w, begin, c - 1u, m, ana_sample(w, c - 1u));
        s_cur[s_active] = j != ANA_NONE ? j + 1u : begin;
      }
      ensure_visible(s_cur[s_active]);
      break;
    }
    case '7':
    case '9': {
      uint32_t c = cursor_or_center(), step = samples_per_col();
      if (k == '7') c = c > step ? c - step : 0;
      else c = c + step < w->total ? c + step : w->total - 1u;
      s_cur[s_active] = c;
      ensure_visible(c);
      break;
    }
    case '0':
      s_sel_ch = (s_sel_ch + 1u) % LA_NUM_CH;
      break;
    case '#':
      s_active ^= 1u;
      if (s_cur[s_active] == ANA_NONE) s_cur[s_active] = s_cur[s_active ^ 1u];
      break;
    case 'C':
      if (s_cur[s_active] != ANA_NONE) center_on(s_cur[s_active]);
      break;
    case '*':
      if (s_cur[0] == ANA_NONE && s_cur[1] == ANA_NONE) return false;
      s_cur[0] = s_cur[1] = ANA_NONE;
      s_active = 0;
      break;
    default:
      return false;
  }
  redraw_view();
  return true;
}

static void wave_poll(void) {
  bool was = s_view.active;
  view_work();
  if (was && s_view.ready) redraw_view();
}

// 4-digit display: 1 / (B - A) in kHz when both cursors are set
static void wave_seg7(char *out) {
  if (s_cur[0] != ANA_NONE && s_cur[1] != ANA_NONE && s_cur[0] != s_cur[1]) {
    uint32_t d = s_cur[1] > s_cur[0] ? s_cur[1] - s_cur[0] : s_cur[0] - s_cur[1];
    ui_seg7_khz(out, (uint64_t)s_view.win.rate_hz * 1000u / d);
  } else {
    strcpy(out, "----");
  }
}

const ui_page_t ui_page_wave = {"Waveform", 0, 0, wave_draw, wave_key, wave_poll, wave_seg7};

// ---------------------------------------------------------------------------
// Services for the core and other pages
// ---------------------------------------------------------------------------

void wave_levels(uint8_t hi, uint8_t lo) {
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
    ui_draw_usb_badge();
  }
}

void wave_capture_started(void) { s_view.active = false; }  // the ring is being reused

void wave_capture_done(void) {
  capture_window_t w = capture_last_window();
  if (!w.valid) return;
  s_view.win = w;
  s_view.from = 0;
  s_view.count = w.total;
  s_cur[0] = s_cur[1] = ANA_NONE;
  s_active = 0;
  view_compute();
}

bool wave_view(uint32_t *from, uint32_t *count) {
  if (!s_view.win.valid) return false;
  *from = s_view.from;
  *count = s_view.count;
  return true;
}

void wave_show(uint32_t index) {
  if (!s_view.win.valid || index >= s_view.win.total) return;
  s_cur[0] = index;
  s_active = 0;
  if (s_view.count > 4000u) s_view.count = 4000u;  // close enough to see bits
  center_on(index);
}
