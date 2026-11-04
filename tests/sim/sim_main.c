// UI simulator: renders the LA-F429 screens for a scripted key session on
// synthetic capture data into PPM files (build/screens/NN_name.ppm).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "gen.h"
#include "gfx.h"
#include "input.h"
#include "la_status.h"
#include "ring_stub.h"
#include "ui.h"

extern uint32_t g_sim_ms;
extern capture_window_t g_sim_win;
extern char g_seg7[16];

static const char *s_dir = "build/screens";
static int s_shot;

static void run(unsigned polls) {
  for (unsigned i = 0; i < polls; i++) {
    g_sim_ms += 10;
    ui_levels(i & 1 ? 0x17 : 0x13);
    ui_poll();
  }
}

static void keys(const char *k) {
  for (; *k; k++) {
    input_inject((uint8_t)*k);
    run(3);
  }
  run(60);
}

static void shot(const char *name) {
  char fn[256];
  snprintf(fn, sizeof fn, "%s/%02d_%s.ppm", s_dir, ++s_shot, name);
  FILE *f = fopen(fn, "wb");
  if (!f) {
    perror(fn);
    exit(1);
  }
  fprintf(f, "P6 %d %d 255\n", GFX_W, GFX_H);
  for (int y = 0; y < GFX_H; y++)
    for (int x = 0; x < GFX_W; x++) {
      uint16_t c = g_fb[y][x];
      unsigned char rgb[3] = {(unsigned char)((c >> 11) * 255 / 31), (unsigned char)(((c >> 5) & 63) * 255 / 63),
                              (unsigned char)((c & 31) * 255 / 31)};
      fwrite(rgb, 1, 3, f);
    }
  fclose(f);
  printf("%-28s 7-seg [%s]\n", fn + strlen(s_dir) + 1, g_seg7);
}

// --- synthetic capture: 2 MHz, 100 ms ---------------------------------------

#define RATE 2000000u
#define N    200000u

static uint8_t s_sig[N];

static void uart_text(unsigned ch, uint32_t t0, const char *text, uint32_t baud) {
  double spb = (double)RATE / baud, t = t0;
  for (const char *p = text; *p; p++) {
    uint16_t frame = (uint16_t)(((uint8_t)*p) << 1 | 0x200);
    for (int b = 0; b < 10; b++) {
      for (uint32_t i = (uint32_t)t; i < (uint32_t)(t + spb) && i < N; i++)
        s_sig[i] = (uint8_t)((s_sig[i] & ~(1u << ch)) | (((frame >> b) & 1u) << ch));
      t += spb;
    }
  }
}

static uint32_t s_i;
static void i2c_put(unsigned n, int scl, int sda) {
  for (unsigned k = 0; k < n && s_i < N; k++, s_i++)
    s_sig[s_i] = (uint8_t)((s_sig[s_i] & 0x9F) | (scl << 5) | (sda << 6));
}
static void i2c_byte(uint8_t b, int ack) {
  for (int k = 7; k >= -1; k--) {
    int bit = k >= 0 ? (b >> k) & 1 : !ack;
    i2c_put(5, 0, bit);
    i2c_put(10, 1, bit);
    i2c_put(5, 0, bit);
  }
}

static void make_capture(void) {
  for (uint32_t i = 0; i < N; i++) {
    uint8_t v = 0x08 | 0x10 | 0x60;           // UART idle, CH4 high, I2C idle
    if ((i % 2000u) < 1000u) v |= 0x01;       // CH0 1 kHz
    if ((i % 20u) < 10u) v |= 0x02;           // CH1 100 kHz
    if (i & 1u) v |= 0x04;                    // CH2 1 MHz
    if (i >= 150000u && i < 150040u) v |= 0x80;  // CH7: one 20 us pulse
    s_sig[i] = v;
  }
  for (unsigned line = 0; line < 10; line++) {
    char text[32];
    snprintf(text, sizeof text, "LA-F429 test %05u\r\n", 40u + line);
    uart_text(3, 1000u + line * 20000u, text, 115200);
  }
  s_i = 60000;  // I2C at 30 ms: write 0x00 0xAF to 0x3C, read one byte back
  i2c_put(10, 1, 0);
  i2c_put(5, 0, 0);
  i2c_byte(0x3C << 1, 1);
  i2c_byte(0x00, 1);
  i2c_byte(0xAF, 1);
  i2c_put(5, 0, 1);
  i2c_put(5, 1, 1);
  i2c_put(5, 1, 0);
  i2c_put(5, 0, 0);
  i2c_byte((0x3C << 1) | 1, 1);
  i2c_byte(0x55, 0);
  i2c_put(5, 0, 0);
  i2c_put(5, 1, 0);
  i2c_put(5, 1, 1);

  g_sim_win = ring_window(RING_SIZE - 50000u, N, RATE);
  for (uint32_t i = 0; i < N; i++) ring_put(&g_sim_win, i, s_sig[i]);
  g_sim_win.trigger = 20000;  // a CH0 rising edge
  g_sim_win.lost = 0;
}

int main(int argc, char **argv) {
  if (argc > 1) s_dir = argv[1];
  mkdir(s_dir, 0755);
  gen_init();
  ui_init();
  run(10);
  shot("ready");

  // a finished capture, as after PulseView or a standalone run
  make_capture();
  g_la.rate_hz = RATE;
  g_la.rate_exact = true;
  g_la.samples = N;
  g_la.pre = 20001;
  g_la.num_stages = 1;
  g_la.mask0 = g_la.value0 = 0x01;
  g_la.captures = 1;
  g_la.state = LA_DONE;
  ui_redraw_status();
  run(100);
  shot("wave");
  keys("222");
  shot("wave_zoom8");
  keys("3#33");
  shot("wave_cursors");
  keys("*5");

  keys("A");
  shot("menu");
  keys("3");
  shot("meter_live");
  keys("8888");
  shot("meter_sel_ch4");
  keys("#");
  run(50);
  shot("meter_capture");

  keys("A4");
  shot("decode_setup");
  keys("#");
  run(100);
  shot("decode_uart");
  keys("#6");  // back to setup, protocol -> I2C
  keys("866666" "866666");  // SCL CH0 -> CH5, SDA CH1 -> CH6
  shot("decode_setup_i2c");
  keys("#");
  run(100);
  shot("decode_i2c");
  keys("8885");
  shot("wave_at_item");

  keys("A5");
  shot("gen");
  keys("8#2500");
  shot("gen_typing");
  keys("00#");
  shot("gen_250k");
  keys("6D");
  shot("gen_duty_up");

  keys("A2");
  shot("setup");
  keys("4466");
  shot("setup_changed");
  // standalone capture request from the setup page
  keys("B");
  capture_cfg_t cfg;
  if (!ui_take_capture(&cfg)) {
    printf("FAIL: no capture request\n");
    return 1;
  }
  printf("standalone cfg: divider %u total %u pre %u stages %u mask %02x value %02x\n", cfg.divider, cfg.total,
         cfg.pre, cfg.num_stages, cfg.mask[0], cfg.value[0]);
  if (cfg.divider != 49 || cfg.total != 1000000 || cfg.pre != 100000 || cfg.num_stages != 1 || cfg.mask[0] != 1 ||
      cfg.value[0] != 1) {
    printf("FAIL: unexpected standalone configuration\n");
    return 1;
  }
  g_la.state = LA_WAIT_TRIGGER;
  g_la.rate_hz = 2000000;
  g_la.samples = cfg.total;
  g_la.pre = cfg.pre;
  ui_redraw_status();
  shot("standalone_wait");
  g_la.state = LA_DONE;
  g_la.captures = 2;
  ui_redraw_status();
  run(100);

  keys("A6");
  shot("pins");
  keys("A7");
  shot("info");
  return 0;
}
