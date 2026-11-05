// LA-F429: 8-channel logic analyzer for an STM32F429IGT6 development board.
// One USB CDC serial port (OTG_FS, USB2 connector) speaks two protocols,
// picked per command byte:
//  - SUMP / OLS (binary): snapshot captures up to 20 MHz and 33 M samples
//    (SDRAM), device-side trigger. sigrok driver "ols".
//  - sigrok-pico (ASCII): RLE streaming, length limited only by the host.
//    sigrok driver "raspberrypi-pico" (PulseView nightly).
// The on-board 240x240 LCD and the 4x4 keypad also make it a standalone
// instrument: captures without a PC, waveform cursors, a live frequency meter,
// UART/I2C/SPI decoding and a signal generator on the JD2 test pins.

#include "board.h"
#include "capture_hw.h"
#include "clock.h"
#include "gen.h"
#include "input.h"
#include "la_status.h"
#include "lcd.h"
#include "sdram.h"
#include "selftest.h"
#include "stream.h"
#include "sump.h"
#include "tusb.h"
#include "ui.h"

// --- TinyUSB glue ----------------------------------------------------------

void OTG_FS_IRQHandler(void) { tud_int_handler(0); }

uint32_t tusb_time_millis_api(void) { return millis(); }

// The board has a fixed 1.5k pull-up (R3) on D+, so the host does not notice
// a reset or a reflash by itself. gpio_init() holds D+ low from the start of
// boot; releasing it here makes the host enumerate the new firmware.
static void usb_init(void) {
  RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
  RCC->AHB2ENR |= RCC_AHB2ENR_OTGFSEN;
  (void)RCC->AHB2ENR;
  delay_ms(10);  // D+ low for at least 10 ms in any case
  gpio_af(GPIOA, 11, 10, GPIO_SPEED_MAX);  // OTG_FS_DM
  gpio_af(GPIOA, 12, 10, GPIO_SPEED_MAX);  // OTG_FS_DP
  NVIC_SetPriority(OTG_FS_IRQn, 2);

  const tusb_rhport_init_t dev_init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_AUTO};
  tusb_init(0, &dev_init);
}

// --- SUMP I/O over CDC -----------------------------------------------------

static void cdc_write(const uint8_t *data, uint32_t len) {
  while (len) {
    tud_task();
    if (!tud_mounted()) return;  // host gone: drop the data
    uint32_t avail = tud_cdc_write_available();
    if (!avail) {
      tud_cdc_write_flush();
      continue;
    }
    uint32_t n = len < avail ? len : avail;
    n = tud_cdc_write(data, n);
    data += n;
    len -= n;
  }
}

static void cdc_flush(void) { tud_cdc_write_flush(); }

// LED1 write only on change: every peripheral access while sampling competes
// with the DMA for the AHB1 bus.
static void led(bool on) {
  static bool s_on;
  if (on != s_on) {
    s_on = on;
    led_set(on);
  }
}

// Runs about once per millisecond while a capture is in progress.
static bool capture_poll(void) {
  tud_task();
  gen_poll();
  led((millis() / 100u) & 1u);  // fast blink while capturing
  // Any byte from the host (PulseView sends a reset on "Stop") aborts.
  return !tud_mounted() || tud_cdc_available();
}

static const sump_io_t sump_io = {
    .write = cdc_write,
    .flush = cdc_flush,
    .poll = capture_poll,
    .on_state = ui_redraw_status,
};

static int cdc_read_byte(void) {
  uint8_t b;
  return (tud_cdc_available() && tud_cdc_read(&b, 1) == 1) ? b : -1;
}

static bool stream_service(void) {
  tud_task();
  gen_poll();
  led((millis() / 100u) & 1u);
  return tud_mounted();
}

// --- Standalone capture (keypad) ----------------------------------------------

// Abort with any keypad key (CH455 INT line) or KEY2, or when the host
// starts talking.
// The keypad line must first be seen released: the B press that started the
// capture may still be held.
static bool s_cancel_armed;

static bool standalone_poll(void) {
  tud_task();
  gen_poll();
  led((millis() / 100u) & 1u);
  if (tud_cdc_available()) return true;
  bool cancel = input_cancel_requested();
  if (!cancel) s_cancel_armed = true;
  return s_cancel_armed && cancel;
}

static void standalone_capture(const capture_cfg_t *cfg) {
  capture_timing_t t = capture_timing(cfg->divider);
  g_la.streaming = false;
  g_la.rate_hz = t.rate_hz;
  g_la.rate_exact = t.exact;
  g_la.samples = cfg->total;
  g_la.pre = cfg->pre;
  g_la.num_stages = cfg->num_stages;
  g_la.mask0 = cfg->mask[0];
  g_la.value0 = cfg->value[0];
  g_la.rle_requested = false;
  g_la.state = cfg->num_stages ? LA_WAIT_TRIGGER : LA_CAPTURING;
  ui_redraw_status();

  uint32_t start;
  s_cancel_armed = false;
  capture_result_t r = capture_run(cfg, standalone_poll, &start);
  if (r == CAPTURE_ABORTED) {
    g_la.state = LA_ABORTED;
  } else {
    g_la.overrun = r == CAPTURE_OVERRUN;
    g_la.captures++;
    g_la.state = LA_DONE;
  }
  input_discard();  // the key that aborted must not start the next capture
  ui_redraw_status();
}

static const stream_io_t stream_io = {
    .write = cdc_write,
    .flush = cdc_flush,
    .read = cdc_read_byte,
    .service = stream_service,
    .on_state = ui_redraw_status,
};

// SUMP commands and their arguments are binary (opcodes 0x00-0x13 and
// 0x80-0xFF); the sigrok-pico protocol is printable ASCII lines. Route each
// byte accordingly, keeping multi-byte SUMP commands together.
static void dispatch(uint8_t b) {
  if (sump_busy() || b >= 0x80u || (b < 0x20u && b != '\n' && b != '\r')) sump_rx(b);
  else stream_rx(b);
}

// --- main ------------------------------------------------------------------

static void gpio_init(void) {
  RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN | RCC_AHB1ENR_GPIOIEN;
  (void)RCC->AHB1ENR;

  // USB D+ (PA12) low until usb_init(): the host sees a disconnect
  gpio_clr(GPIOA, 12);
  gpio_mode(GPIOA, 12, GPIO_OUT);

  // LED1..LED4 off
  gpio_clr(GPIOB, 6);
  gpio_clr(GPIOB, 7);
  gpio_clr(GPIOB, 14);
  gpio_clr(GPIOA, 0);
  gpio_mode(GPIOB, 6, GPIO_OUT);
  gpio_mode(GPIOB, 7, GPIO_OUT);
  gpio_mode(GPIOB, 14, GPIO_OUT);
  gpio_mode(GPIOA, 0, GPIO_OUT);

  for (unsigned pin = 0; pin < LA_NUM_CH; pin++) {
    gpio_mode(LA_PORT, pin, GPIO_IN);
    gpio_pull(LA_PORT, pin, GPIO_PULLDOWN);
  }
}

int main(void) {
  clock_init();
  gpio_init();
  input_init();  // KEY1/KEY2 and the CH455 keypad / display

  sdram_init();
  g_selftest.sdram_ok = sdram_test();
  capture_init(g_selftest.sdram_ok);
  selftest_dma();  // before anything else uses the buses

  lcd_init();
  ui_init();
  gen_init();
  sump_init(&sump_io);
  stream_init(&stream_io);
  usb_init();

  for (;;) {
    tud_task();

    uint8_t buf[64];
    while (tud_cdc_available()) {
      uint32_t n = tud_cdc_read(buf, sizeof buf);
      for (uint32_t i = 0; i < n; i++) dispatch(buf[i]);
    }
    if (sump_armed()) sump_run();
    if (stream_pending()) stream_run();
    capture_cfg_t cfg;
    if (ui_take_capture(&cfg)) standalone_capture(&cfg);

    ui_levels((uint8_t)LA_PORT->IDR);
    ui_poll();
    gen_poll();
    led((millis() % 1000u) < 50u);  // heartbeat
    gpio_write(LED2_PORT, LED2_PIN, tud_mounted());
  }
}
