#include "ch455.h"

#include "board.h"
#include "clock.h"

// PC13..PC15 are low-speed pins (backup domain); with 10k pull-ups and 100 pF
// the bus rises in ~1 us, so it is clocked at about 100 kHz.
#define SCL_PIN 14
#define SDA_PIN 15
#define INT_PIN 13
#define HALF_US 5u

// Command bytes (CH455 datasheet): the first byte works like an I2C address.
#define CMD_SYS  0x48u  // system parameters: data = brightness | 8-seg | enable
#define CMD_DIG0 0x68u  // + 2 * digit
#define CMD_KEY  0x4Fu  // read key code

static bool s_present;
static uint8_t s_level = 4;

static void scl(bool hi) { gpio_write(GPIOC, SCL_PIN, hi); }
static void sda(bool hi) { gpio_write(GPIOC, SDA_PIN, hi); }
static bool sda_in(void) { return gpio_read(GPIOC, SDA_PIN); }
static void half(void) { delay_us(HALF_US); }

static void bus_start(void) {
  sda(1);
  scl(1);
  half();
  sda(0);
  half();
  scl(0);
  half();
}

static void bus_stop(void) {
  sda(0);
  half();
  scl(1);
  half();
  sda(1);
  half();
}

static bool bus_write(uint8_t b) {
  for (int i = 7; i >= 0; i--) {
    sda((b >> i) & 1u);
    half();
    scl(1);
    half();
    scl(0);
  }
  sda(1);  // release for ACK
  half();
  scl(1);
  half();
  bool ack = !sda_in();
  scl(0);
  half();
  return ack;
}

static uint8_t bus_read(void) {
  uint8_t b = 0;
  sda(1);
  for (int i = 0; i < 8; i++) {
    half();
    scl(1);
    half();
    b = (uint8_t)((b << 1) | sda_in());
    scl(0);
  }
  half();  // NACK: end of read
  scl(1);
  half();
  scl(0);
  half();
  return b;
}

static bool cmd(uint8_t c, uint8_t data) {
  bus_start();
  bool ok = bus_write(c) && bus_write(data);
  bus_stop();
  return ok;
}

static bool sys_on(void) {
  // brightness 1..8 -> INTENS bits 6:4 (8 is encoded as 0), 8-segment mode, on
  return cmd(CMD_SYS, (uint8_t)(((s_level & 7u) << 4) | 0x01u));
}

bool ch455_init(void) {
  RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN;
  (void)RCC->AHB1ENR;
  GPIOC->OTYPER |= (1u << SCL_PIN) | (1u << SDA_PIN);  // open drain, external pull-ups
  sda(1);
  scl(1);
  gpio_mode(GPIOC, SCL_PIN, GPIO_OUT);
  gpio_mode(GPIOC, SDA_PIN, GPIO_OUT);
  gpio_mode(GPIOC, INT_PIN, GPIO_IN);
  // Clock out a transfer the chip might still be in, then STOP
  for (int i = 0; i < 9; i++) {
    scl(0);
    half();
    scl(1);
    half();
  }
  bus_stop();
  s_present = sys_on();
  if (s_present) {
    static const uint8_t blank[4] = {0, 0, 0, 0};
    ch455_digits(blank);
  }
  return s_present;
}

bool ch455_present(void) { return s_present; }

void ch455_brightness(unsigned level) {
  if (level < 1) level = 1;
  if (level > 8) level = 8;
  s_level = (uint8_t)level;
  if (s_present) sys_on();
}

bool ch455_digits(const uint8_t seg[4]) {
  if (!s_present) return false;
  bool ok = true;
  for (unsigned d = 0; d < 4; d++) ok &= cmd((uint8_t)(CMD_DIG0 + 2u * d), seg[d]);
  return ok;
}

// 7-segment patterns: bit0 A (top) .. bit5 F, bit6 G (middle)
static uint8_t seg_of(char c) {
  static const uint8_t digit[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
  static const uint8_t alpha[26] = {
      0x77, 0x7C, 0x39, 0x5E, 0x79, 0x71, 0x3D, 0x76, 0x30, 0x1E, 0x75, 0x38, 0x15,  // A..M
      0x54, 0x5C, 0x73, 0x67, 0x50, 0x6D, 0x78, 0x3E, 0x1C, 0x2A, 0x76, 0x6E, 0x5B,  // N..Z
  };
  if (c >= '0' && c <= '9') return digit[c - '0'];
  if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
  if (c >= 'A' && c <= 'Z') return alpha[c - 'A'];
  if (c == '-') return 0x40;
  if (c == '_') return 0x08;
  return 0;
}

// The points of this board's display (CS32841A) are not where the digit
// registers suggest (found on the board): the point after the second digit
// is the DP bit of digit 0, the colon is the DP bit of digit 1, digits 2 and
// 3 light their own point; there is no point after the first digit.
static const int8_t s_point_reg[4] = {-1, 0, 2, 3};  // register for the point after digit n
#define COLON_REG 1

bool ch455_text(const char *s) {
  uint8_t seg[4] = {0, 0, 0, 0}, dp = 0;
  unsigned d = 0;
  for (; *s; s++) {
    if (*s == '.') {  // point after the previous digit (or after a blank one)
      if (d == 0) d = 1;
      int r = s_point_reg[d - 1];
      if (r >= 0) dp |= (uint8_t)(1u << r);
    } else if (*s == ':') {
      dp |= 1u << COLON_REG;
    } else if (d < 4) {
      seg[d++] = seg_of(*s);
    } else {
      break;
    }
  }
  for (unsigned k = 0; k < 4; k++)
    if ((dp >> k) & 1u) seg[k] |= 0x80;
  return ch455_digits(seg);
}

bool ch455_int_active(void) { return !gpio_read(GPIOC, INT_PIN); }

int ch455_read_key(void) {
  if (!s_present) return -1;
  bus_start();
  if (!bus_write(CMD_KEY)) {
    bus_stop();
    return -1;
  }
  uint8_t k = bus_read();
  bus_stop();
  return k;
}
