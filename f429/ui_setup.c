// Capture setup page: settings of a standalone capture (no PC needed).
// Keys: 2/8 field, 4/6 change, # or B run.
#include <stdio.h>
#include <string.h>

#include "ui_priv.h"

static const uint32_t s_rates[] = {1000,   2000,   5000,    10000,   20000,   50000,    100000,
                                   200000, 500000, 1000000, 2000000, 5000000, 10000000, 20000000};
static const uint32_t s_depths[] = {1000,   2000,   5000,    10000,   20000,   50000,    100000,   200000,
                                    500000, 1000000, 2000000, 5000000, 10000000, 20000000, 0 /* max */};
static const uint8_t s_pre[] = {0, 10, 25, 50, 75, 90};

#define NRATES  (sizeof s_rates / sizeof s_rates[0])
#define NDEPTHS (sizeof s_depths / sizeof s_depths[0])
#define NPRE    (sizeof s_pre / sizeof s_pre[0])
#define NTRIG   (1u + 2u * LA_NUM_CH)  // none, then CHn rising / falling

enum { F_RATE, F_DEPTH, F_TRIG, F_PRE, F_COUNT };

static struct {
  unsigned rate, depth, trig, pre;
} s_cfg = {10, 9, 1, 1};  // 2 MHz, 1M samples, CH0 rising, 10 %
static unsigned s_field;

static uint32_t depth_samples(unsigned i) {
  uint32_t max = capture_max_samples();
  uint32_t d = s_depths[i] ? s_depths[i] : max;
  return d > max ? max : d;
}

static void trig_text(char *out, size_t n, unsigned t) {
  if (!t) snprintf(out, n, "none (immediate)");
  else snprintf(out, n, "CH%u %s", (t - 1u) / 2u, ((t - 1u) & 1u) ? G_DOWN " falling" : G_UP " rising");
}

void setup_describe(char *out, size_t n) {
  char r[12], d[12], t[24];
  ui_fmt_si(r, sizeof r, s_rates[s_cfg.rate], "Hz");
  ui_fmt_si(d, sizeof d, depth_samples(s_cfg.depth), "");
  trig_text(t, sizeof t, s_cfg.trig);
  snprintf(out, n, "%s %s %s", r, d, t);
}

bool setup_take_request(capture_cfg_t *cfg) {
  memset(cfg, 0, sizeof *cfg);
  uint32_t rate = s_rates[s_cfg.rate];
  cfg->divider = 100000000u / rate - 1u;  // every preset divides 100 MHz
  cfg->total = depth_samples(s_cfg.depth);
  if (s_cfg.trig) {
    unsigned ch = (s_cfg.trig - 1u) / 2u;
    bool rising = !((s_cfg.trig - 1u) & 1u);
    cfg->num_stages = 1;
    cfg->mask[0] = (uint8_t)(1u << ch);
    cfg->value[0] = rising ? cfg->mask[0] : 0;
    cfg->pre = (uint32_t)((uint64_t)cfg->total * s_pre[s_cfg.pre] / 100u);
  }
  return true;
}

static void setup_draw(void) {
  char v[32], line[64];
  gfx_fill(0, 0, GFX_W, GFX_H, C_BG);
  ui_title("Capture", "standalone");
  for (unsigned f = 0; f < F_COUNT; f++) {
    switch (f) {
      case F_RATE: ui_fmt_si(v, sizeof v, s_rates[s_cfg.rate], "Hz"); break;
      case F_DEPTH: ui_fmt_si(v, sizeof v, depth_samples(s_cfg.depth), " samples"); break;
      case F_TRIG: trig_text(v, sizeof v, s_cfg.trig); break;
      default:
        if (s_cfg.trig) snprintf(v, sizeof v, "%u%%", s_pre[s_cfg.pre]);
        else snprintf(v, sizeof v, "-");
        break;
    }
    static const char *const name[F_COUNT] = {"Rate", "Depth", "Trigger", "Pre-trig"};
    snprintf(line, sizeof line, "%-9s %s", name[f], v);
    ui_row(34 + (int)f * 24, line, C_TEXT, f == s_field);
  }
  // what this means
  uint32_t total = depth_samples(s_cfg.depth);
  ui_fmt_time(v, sizeof v, total, s_rates[s_cfg.rate]);
  snprintf(line, sizeof line, "Window %s", v);
  gfx_text(4, 140, &font_s, line, C_DIM, C_BG);
  if (s_cfg.trig) {
    ui_fmt_time(v, sizeof v, (uint64_t)total * s_pre[s_cfg.pre] / 100u, s_rates[s_cfg.rate]);
    snprintf(line, sizeof line, "  %s before the trigger", v);
    gfx_text(4, 158, &font_s, line, C_DIM, C_BG);
  }
  gfx_text(4, 184, &font_s, "B or # run, B/KEY2 abort", C_TEXT, C_BG);
  ui_hint("2/8 field  4/6 change");
}

static bool setup_key(uint8_t k) {
  int d = 0;
  switch (k) {
    case '2': s_field = (s_field + F_COUNT - 1u) % F_COUNT; break;
    case '8': s_field = (s_field + 1u) % F_COUNT; break;
    case '4': d = -1; break;
    case '6': d = 1; break;
    case '#': ui_request_capture(); return true;
    default: return false;
  }
  if (d) {
    unsigned *v = s_field == F_RATE ? &s_cfg.rate : s_field == F_DEPTH ? &s_cfg.depth
                                                 : s_field == F_TRIG    ? &s_cfg.trig
                                                                        : &s_cfg.pre;
    unsigned n = s_field == F_RATE ? NRATES : s_field == F_DEPTH ? NDEPTHS : s_field == F_TRIG ? NTRIG : NPRE;
    *v = (*v + n + (unsigned)d) % n;
  }
  setup_draw();
  return true;
}

const ui_page_t ui_page_setup = {"Capture setup", 0, 0, setup_draw, setup_key, 0, 0};
