// Meter page: frequency, duty cycle and shortest pulse of every channel.
// Live: background sampling at 10 MHz, updated every 0.5 s. Capture: over the
// range shown on the waveform page. The selected channel's frequency is also
// shown on the 4-digit display (Hz, or kHz with a point).
// Keys: 2/8 channel, # live / capture.
#include <stdio.h>
#include <string.h>

#include "analyze.h"
#include "meter.h"
#include "ui_priv.h"

#define TABLE_Y 50

static bool s_capture_mode;
static unsigned s_sel;
static meter_result_t s_res;
static bool s_have;
static uint32_t s_seq;
static ana_job_t s_job;

// mark '~': measured over a sample of the gate only (busy signal)
static void row_text(char *out, size_t n, const ana_stats_t *st, uint32_t rate, unsigned ch, char mark) {
  char f[16], dty[16], p[16];
  const ana_chan_t *c = &st->ch[ch];
  uint64_t mhz = ana_freq_mhz(st, ch, rate);
  if (mhz) ui_fmt_hz_milli(f, sizeof f, mhz, 5);
  else if (!c->rises && !c->falls) snprintf(f, sizeof f, "%s", ((st->first >> ch) & 1u) ? "high" : "low");
  else snprintf(f, sizeof f, "%lu edge%s", (unsigned long)(c->rises + c->falls), c->rises + c->falls > 1 ? "s" : "");
  // The duty cycle is only as precise as one sample in a period (a signal
  // locked to the sample clock does not average out): shown to 0.1 % from
  // 1000 samples per period, to 1 % from 20, below that as approximate.
  unsigned d = ana_duty_permille(st, ch);
  uint64_t per = mhz ? (uint64_t)rate * 1000u / mhz : ~0ull;  // samples per period
  if (per >= 1000u) snprintf(dty, sizeof dty, "%u.%u%%", d / 10u, d % 10u);
  else snprintf(dty, sizeof dty, "%s%u%%", per < 20u ? "~" : "", (d + 5u) / 10u);
  uint32_t mp = ana_min_pulse(st, ch);
  if (mp == ANA_NONE) strcpy(p, "-");
  else ui_fmt_time(p, sizeof p, mp, rate);
  snprintf(out, n, "%u%c%-11s %6s %s", ch, mark, f, dty, p);
}

static void draw_table(void) {
  char line[64];
  gfx_fill(0, TABLE_Y, GFX_W, LA_NUM_CH * LINE_H + 2, C_BG);
  const ana_stats_t *st = 0;
  uint32_t rate = 0;
  uint8_t sampled = 0;
  if (s_capture_mode) {
    if (s_job.done) {
      st = &s_job.st;
      rate = s_job.win.rate_hz;
    }
  } else if (s_have) {
    st = &s_res.st;
    rate = s_res.rate_hz;
    sampled = s_res.sampled;
  }
  for (unsigned ch = 0; ch < LA_NUM_CH; ch++) {
    int y = TABLE_Y + (int)ch * LINE_H;
    if (st) row_text(line, sizeof line, st, rate, ch, ((sampled >> ch) & 1u) ? '~' : ' ');
    else snprintf(line, sizeof line, "%u  ...", ch);
    ui_row(y, line, ui_ch_color[ch], ch == s_sel);
  }
}

static void draw_head(void) {
  char line[48], a[16];
  gfx_fill(0, TITLE_H, GFX_W, TABLE_Y - TITLE_H, C_BG);
  if (s_capture_mode) {
    uint32_t from, count;
    if (!wave_view(&from, &count)) snprintf(line, sizeof line, "Capture: none yet");
    else {
      ui_fmt_time(a, sizeof a, count, s_job.win.rate_hz);
      snprintf(line, sizeof line, "Capture view %s%s", a, s_job.done ? "" : " ...");
    }
  } else if (ui_capture_busy()) {
    snprintf(line, sizeof line, "Live: paused (capturing)");
  } else if (s_have && s_res.sampled && s_res.coverage >= 990u) {
    snprintf(line, sizeof line, "Live 10MHz, ~ %u%% sampled", (s_res.sampled_coverage + 5u) / 10u);
  } else if (s_have && s_res.coverage < 990u) {
    snprintf(line, sizeof line, "Live 10MHz, %u%% of 0.5s", (s_res.coverage + 5u) / 10u);
  } else {
    snprintf(line, sizeof line, "Live 10MHz, 0.5s gate");
  }
  gfx_text(4, 28, &font_s, line, C_TEXT, C_BG);
}

static void meter_draw(void) {
  gfx_fill(0, 0, GFX_W, GFX_H, C_BG);
  ui_title("Meter", s_capture_mode ? "capture" : "live");
  draw_head();
  draw_table();
  gfx_text(4, TABLE_Y + LA_NUM_CH * LINE_H + 6, &font_s, "  frequency    duty  shortest", C_DIM, C_BG);
  ui_hint("2/8 channel  # live/capture");
}

static void start_capture_job(void) {
  uint32_t from, count;
  capture_window_t w = capture_last_window();
  if (w.valid && wave_view(&from, &count)) ana_job_start(&s_job, &w, from, count);
  else memset(&s_job, 0, sizeof s_job);
}

static void meter_enter(void) {
  s_have = false;
  s_seq = 0;
  if (s_capture_mode) start_capture_job();
  else meter_start();
}

static void meter_leave(void) { meter_stop(); }

static bool meter_key(uint8_t k) {
  switch (k) {
    case '2': s_sel = (s_sel + LA_NUM_CH - 1u) % LA_NUM_CH; break;
    case '8': s_sel = (s_sel + 1u) % LA_NUM_CH; break;
    case '#':
      meter_leave();
      s_capture_mode = !s_capture_mode;
      meter_enter();
      meter_draw();
      return true;
    default: return false;
  }
  draw_table();
  return true;
}

static void meter_poll(void) {
  if (s_capture_mode) {
    if (s_job.active && ana_job_run(&s_job, 2000000u)) {
      draw_head();
      draw_table();
    }
    return;
  }
  if (!meter_running() && !ui_capture_busy()) {  // a capture stopped the meter
    meter_start();
    draw_head();
  }
  if (meter_get(&s_res, &s_seq)) {
    s_have = true;
    draw_head();
    draw_table();
  }
}

static void meter_seg7(char *out) {
  const ana_stats_t *st = s_capture_mode ? (s_job.done ? &s_job.st : 0) : (s_have ? &s_res.st : 0);
  uint32_t rate = s_capture_mode ? s_job.win.rate_hz : s_res.rate_hz;
  if (!st) {
    strcpy(out, "----");
    return;
  }
  uint64_t mhz = ana_freq_mhz(st, s_sel, rate);
  if (mhz) ui_seg7_freq(out, mhz);
  else strcpy(out, "----");
}

const ui_page_t ui_page_meter = {"Meter", meter_enter, meter_leave, meter_draw, meter_key, meter_poll, meter_seg7};
