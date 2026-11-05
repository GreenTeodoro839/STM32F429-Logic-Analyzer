// Generator page: the three square-wave outputs and the UART output on JD2.
// Keys: 2/8 output, 4/6 field, C/D value down/up, 0 on/off,
//       # type a value (digits, * deletes / cancels, # sets).
// The selected output's frequency is shown on the 4-digit display (Hz, or kHz with a point).
#include <stdio.h>
#include <string.h>

#include "gen.h"
#include "ui_priv.h"

#define OUTS   (GEN_PWM_OUTPUTS + 1u)  // + UART
#define ROW_Y0 30
#define ROW_DY 44

enum { FLD_ON, FLD_VALUE, FLD_AUX, FLD_COUNT };  // PWM: on, Hz, duty; UART: on, baud, pattern

static unsigned s_out, s_field = FLD_VALUE;
static char s_edit[10];
static bool s_editing;

static const uint32_t s_bauds[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 1000000};
#define NBAUDS (sizeof s_bauds / sizeof s_bauds[0])

// 1-2-5 steps from 1 Hz to GEN_MAX_HZ
static uint32_t step_freq(uint32_t f, int d) {
  uint32_t dec = 1;
  while (dec * 10u <= f && dec < 1000000000u) dec *= 10u;
  static const uint32_t m[] = {1, 2, 5, 10};
  // next / previous value of the sequence strictly above / below f
  if (d > 0) {
    for (unsigned i = 0; i < 4; i++)
      if (m[i] * dec > f) return m[i] * dec > GEN_MAX_HZ ? GEN_MAX_HZ : m[i] * dec;
    return f;
  }
  for (int i = 3; i >= 0; i--) {
    uint32_t v = m[i] * dec;
    if (v < f) return v;
  }
  return dec >= 10u ? 5u * (dec / 10u) : 1u;
}

static void value_text(unsigned out, unsigned field, char *s, size_t n) {
  if (out < GEN_PWM_OUTPUTS) {
    if (field == FLD_VALUE) ui_fmt_hz_milli(s, n, gen_actual_mhz(out), 6);
    else snprintf(s, n, "%u%%", g_gen.pwm[out].duty);
  } else {
    if (field == FLD_VALUE) snprintf(s, n, "%lu Bd", (unsigned long)g_gen.uart.baud);
    else snprintf(s, n, "%s", gen_pattern_name[g_gen.uart.pattern]);
  }
}

static void draw_out(unsigned i) {
  char line[48], v[20], a[24];
  int y = ROW_Y0 + (int)i * ROW_DY;
  bool sel = i == s_out;
  uint16_t bg = sel ? C_SEL : C_BG;
  gfx_fill(0, y - 2, GFX_W, ROW_DY - 4, bg);
  bool on = i < GEN_PWM_OUTPUTS ? g_gen.pwm[i].on : g_gen.uart.on;
  snprintf(line, sizeof line, "%s %s", gen_pin_name[i], i < GEN_PWM_OUTPUTS ? "square" : "UART");
  gfx_text(4, y, &font_s, line, C_DIM, bg);
  // fields on the second line: [on] [value] [aux]; the selected one inverted
  int x = 12;
  for (unsigned f = 0; f < FLD_COUNT; f++) {
    if (f == FLD_ON) snprintf(v, sizeof v, "%s", on ? "ON" : "off");
    else if (sel && s_editing && f == s_field) snprintf(v, sizeof v, "%s_", s_edit);
    else value_text(i, f, v, sizeof v);
    bool hi = sel && f == s_field;
    uint16_t fg = f == FLD_ON ? (on ? C_OK : C_DIM) : (on ? C_TEXT : C_DIM);
    uint16_t fbg = hi ? (s_editing ? C_WARN : C_BUSY) : bg;
    if (hi) fg = C_BG;
    snprintf(a, sizeof a, " %s ", v);
    x = gfx_text(x, y + 18, &font_s, a, fg, fbg) + 6;
  }
}

static void gen_draw(void) {
  gfx_fill(0, 0, GFX_W, GFX_H, C_BG);
  ui_title("Generator", NULL);
  for (unsigned i = 0; i < OUTS; i++) draw_out(i);
  ui_hint(s_editing ? "digits, * delete, # set" : "4/6 field C/D +- 0 on # type");
}

static void apply(void) {
  gen_apply();
  for (unsigned i = 0; i < OUTS; i++) draw_out(i);
}

static void change(int d) {
  if (s_out < GEN_PWM_OUTPUTS) {
    if (s_field == FLD_VALUE) {
      g_gen.pwm[s_out].freq_hz = step_freq(g_gen.pwm[s_out].freq_hz, d);
    } else if (s_field == FLD_AUX) {
      int duty = (int)g_gen.pwm[s_out].duty + 5 * d;
      if (d > 0 && duty % 5) duty -= duty % 5;
      g_gen.pwm[s_out].duty = (uint8_t)(duty < 5 ? 5 : duty > 95 ? 95 : duty);
    } else {
      g_gen.pwm[s_out].on = !g_gen.pwm[s_out].on;
    }
  } else {
    if (s_field == FLD_VALUE) {
      // first preset >= the current rate, then one step up or down from it
      uint32_t b = g_gen.uart.baud;
      unsigned i = 0;
      while (i + 1u < NBAUDS && s_bauds[i] < b) i++;
      if (d > 0 && s_bauds[i] <= b && i + 1u < NBAUDS) i++;
      if (d < 0 && s_bauds[i] >= b && i > 0) i--;
      g_gen.uart.baud = s_bauds[i];
    } else if (s_field == FLD_AUX) {
      g_gen.uart.pattern = (uint8_t)((g_gen.uart.pattern + GEN_UART_PATTERNS + d) % GEN_UART_PATTERNS);
    } else {
      g_gen.uart.on = !g_gen.uart.on;
    }
  }
  apply();
}

static void edit_commit(void) {
  uint32_t v = 0;
  for (const char *p = s_edit; *p; p++) v = v * 10u + (uint32_t)(*p - '0');
  s_editing = false;
  if (!s_edit[0]) return;
  if (s_out < GEN_PWM_OUTPUTS) {
    if (s_field == FLD_VALUE) g_gen.pwm[s_out].freq_hz = v < 1 ? 1 : v > GEN_MAX_HZ ? GEN_MAX_HZ : v;
    else g_gen.pwm[s_out].duty = (uint8_t)(v < 1 ? 1 : v > 99 ? 99 : v);
  } else if (s_field == FLD_VALUE) {
    g_gen.uart.baud = v < 300 ? 300 : v > 1000000 ? 1000000 : v;
  }
}

static bool gen_key(uint8_t k) {
  if (s_editing) {
    size_t len = strlen(s_edit);
    if (k >= '0' && k <= '9') {
      if (len < 8 && !(len == 0 && k == '0')) {
        s_edit[len] = (char)k;
        s_edit[len + 1] = 0;
      }
    } else if (k == '*') {
      if (len) s_edit[len - 1] = 0;
      else s_editing = false;
    } else if (k == '#') {
      edit_commit();
      gen_apply();
    } else {
      return true;  // other keys are ignored while typing
    }
    gen_draw();
    return true;
  }
  switch (k) {
    case '2': s_out = (s_out + OUTS - 1u) % OUTS; break;
    case '8': s_out = (s_out + 1u) % OUTS; break;
    case '4': s_field = (s_field + FLD_COUNT - 1u) % FLD_COUNT; break;
    case '6': s_field = (s_field + 1u) % FLD_COUNT; break;
    case 'C': change(-1); return true;
    case 'D': change(1); return true;
    case '0':
      if (s_out < GEN_PWM_OUTPUTS) g_gen.pwm[s_out].on = !g_gen.pwm[s_out].on;
      else g_gen.uart.on = !g_gen.uart.on;
      apply();
      return true;
    case '#':
      // numbers: frequency, duty and baud rate
      if (s_field == FLD_ON || (s_out == GEN_PWM_OUTPUTS && s_field == FLD_AUX)) change(1);
      else {
        s_editing = true;
        s_edit[0] = 0;
      }
      break;
    default: return false;
  }
  gen_draw();
  return true;
}

static void gen_seg7(char *out) {
  if (s_out < GEN_PWM_OUTPUTS) {
    if (g_gen.pwm[s_out].on) ui_seg7_freq(out, gen_actual_mhz(s_out));
    else strcpy(out, " OFF");
  } else {
    ui_seg7_freq(out, (uint64_t)g_gen.uart.baud * 1000u);  // baud like a frequency: 9600, 115.2
  }
}

const ui_page_t ui_page_gen = {"Generator", 0, 0, gen_draw, gen_key, 0, gen_seg7};
