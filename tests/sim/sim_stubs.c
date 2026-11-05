// Hardware stand-ins for the UI simulator.
#include <stdio.h>
#include <string.h>

#include "analyze.h"
#include "capture_hw.h"
#include "clock.h"
#include "gen.h"
#include "gen_calc.h"
#include "input.h"
#include "lcd.h"
#include "meter.h"
#include "selftest.h"
#include "tusb.h"

bool g_clock_hse_ok = true;
selftest_t g_selftest;
uint32_t g_sim_ms;
static uint32_t s_cyc;
uint32_t millis(void) { return g_sim_ms; }
uint32_t cycles(void) { return s_cyc += 500; }
bool tud_mounted(void) { return true; }

void lcd_init(void) {}
void lcd_display_on(void) {}
bool lcd_busy(void) { return false; }
void lcd_wait(void) {}
void lcd_flush_rows(const uint16_t *fb, unsigned y0, unsigned y1) { (void)fb, (void)y0, (void)y1; }

char g_seg7[16];
bool ch455_digits(const uint8_t seg[4]) {
  snprintf(g_seg7, sizeof g_seg7, "raw %02x%02x%02x%02x", seg[0], seg[1], seg[2], seg[3]);
  return true;
}
bool ch455_text(const char *s) {
  snprintf(g_seg7, sizeof g_seg7, "%s", s);
  return true;
}

// --- input -----------------------------------------------------------------
static uint8_t s_q[64];
static unsigned s_h, s_t;
void input_inject(uint8_t k) { s_q[s_h++ % 64] = k; }
uint8_t input_poll(void) { return s_t == s_h ? 0 : s_q[s_t++ % 64]; }
bool input_keypad_ok(void) { return true; }
int input_last_code(void) { return 0x4C; }
bool input_cancel_requested(void) { return false; }
void input_discard(void) {}

// --- capture -----------------------------------------------------------------
capture_window_t g_sim_win;
capture_window_t capture_last_window(void) { return g_sim_win; }
uint32_t capture_max_samples(void) { return 32u * 1024 * 1024 - 65536; }
bool capture_sdram_ok(void) { return true; }
void capture_or_and(uint32_t index, uint32_t n, uint8_t *or_out, uint8_t *and_out) {
  uint8_t o = 0, a = 0xFF;
  while (n) {
    uint32_t c;
    const uint8_t *p = capture_span(index, &c);
    if (c > n) c = n;
    for (uint32_t i = 0; i < c; i++) o |= p[i], a &= p[i];
    index += c;
    n -= c;
  }
  *or_out = o;
  *and_out = a;
}

// --- meter: statistics of the simulated capture ---------------------------------
static bool s_meter;
void meter_start(void) { s_meter = true; }
void meter_stop(void) { s_meter = false; }
bool meter_running(void) { return s_meter; }
bool meter_get(meter_result_t *out, uint32_t *seq) {
  if (!s_meter || *seq) return false;
  ana_job_t j;
  ana_job_start(&j, &g_sim_win, 0, g_sim_win.total);
  while (!ana_job_run(&j, 1u << 20)) {
  }
  out->st = j.st;
  out->rate_hz = g_sim_win.rate_hz;
  out->gates = 1;
  out->coverage = 1000;
  *seq = 2;
  return true;
}

// --- generator (gen.c without the hardware) --------------------------------------
gen_cfg_t g_gen;
const char *const gen_pin_name[GEN_PWM_OUTPUTS + 1] = {"PB0 JD2-10", "PB1 JD2-12", "PA5 JD2-16", "PA9 JD2-7"};
const char *const gen_pattern_name[GEN_UART_PATTERNS] = {"text", "0x55", "ramp"};
static gen_timing_t s_tim[GEN_PWM_OUTPUTS];
static uint32_t s_baud;
void gen_apply(void) {
  static const uint32_t clk[3] = {60000000, 120000000, 60000000}, max[3] = {0xFFFF, 0xFFFF, 0xFFFFFFFF};
  for (unsigned i = 0; i < GEN_PWM_OUTPUTS; i++)
    s_tim[i] = gen_timer_calc(clk[i], max[i], g_gen.pwm[i].freq_hz, g_gen.pwm[i].duty);
  gen_uart_brr(60000000, g_gen.uart.baud, &s_baud);
}
void gen_init(void) {
  static const uint32_t f[3] = {100000, 1000000, 1000};
  for (unsigned i = 0; i < 3; i++) g_gen.pwm[i].on = true, g_gen.pwm[i].freq_hz = f[i], g_gen.pwm[i].duty = 50;
  g_gen.uart.on = true;
  g_gen.uart.baud = 115200;
  gen_apply();
}
uint64_t gen_actual_mhz(unsigned o) { return s_tim[o].actual_mhz; }
unsigned gen_actual_duty(unsigned o) { return s_tim[o].actual_duty; }
uint32_t gen_uart_actual(void) { return s_baud; }
