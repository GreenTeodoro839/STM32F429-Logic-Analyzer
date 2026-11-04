// LA-F103: 8-channel logic analyzer for an STM32F103C8T6 minimal board.
// One USB CDC serial port speaks two protocols, picked per command byte:
//  - SUMP / OLS (binary): snapshot captures up to 4 MHz, device-side trigger.
//    sigrok driver "ols" (PulseView: "Openbench Logic Sniffer & SUMP compatibles").
//  - sigrok-pico (ASCII): RLE streaming, length limited only by the host.
//    sigrok driver "raspberrypi-pico" (PulseView nightly).

#include "board.h"
#include "clock.h"
#include "la_status.h"
#include "stream.h"
#include "sump.h"
#include "testsig.h"
#include "tusb.h"
#include "ui.h"

// --- TinyUSB glue ----------------------------------------------------------

void USB_HP_CAN1_TX_IRQHandler(void) { tud_int_handler(0); }
void USB_LP_CAN1_RX0_IRQHandler(void) { tud_int_handler(0); }

uint32_t tusb_time_millis_api(void) { return millis(); }

// The F103 has no internal D+ pull-up: the board has a fixed 1.5k resistor.
void dcd_connect(uint8_t rhport) { (void)rhport; }
void dcd_disconnect(uint8_t rhport) { (void)rhport; }

static void usb_init(void) {
  RCC->APB1ENR |= RCC_APB1ENR_USBEN;
  // Pull D+ (PA12) low briefly so the host notices a fresh connection after a
  // reset / reflash, then hand the pin back to the USB peripheral.
  gpio_clr(GPIOA, 12);
  gpio_config(GPIOA, 12, GPIO_CFG_OUT_PP);
  delay_ms(10);
  gpio_config(GPIOA, 12, GPIO_CFG_IN_FLOAT);

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

// Runs about once per millisecond while a capture is in progress.
static bool capture_poll(void) {
  tud_task();
  testsig_poll();
  led_set((millis() / 100u) & 1u);  // fast blink while capturing
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
  testsig_poll();
  led_set((millis() / 100u) & 1u);
  return tud_mounted();
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
  RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN | RCC_APB2ENR_IOPCEN | RCC_APB2ENR_AFIOEN;

  led_set(false);
  gpio_config(LED_PORT, LED_PIN, GPIO_CFG_OUT_PP);

  for (unsigned pin = 0; pin < LA_NUM_CH; pin++)
    gpio_config(LA_PORT, pin, LA_INPUT_PULLDOWN ? GPIO_CFG_IN_PULL : GPIO_CFG_IN_FLOAT);
  if (LA_INPUT_PULLDOWN) LA_PORT->BRR = 0xFFu;  // ODR = 0 selects pull-down
}

int main(void) {
  clock_init();
  gpio_init();
  testsig_init();
  ui_init();
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

    ui_levels((uint8_t)LA_PORT->IDR);
    ui_poll();
    testsig_poll();
    led_set((millis() % 1000u) < 50u);  // heartbeat
  }
}
