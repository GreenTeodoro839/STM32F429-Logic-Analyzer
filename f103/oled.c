#include "oled.h"

#include <string.h>

#include "board.h"
#include "font6x8.h"

#define SCL_PIN 6
#define SDA_PIN 7
#define I2C_TIMEOUT 20000u  // polling iterations (~3 ms)

static bool s_present;

static void short_delay(void) {
  for (volatile int i = 0; i < 100; i++) {
  }
}

// Clock out a slave that may still be holding SDA low (e.g. after a reset in
// the middle of a transfer), then issue a STOP.
static void i2c_bus_recover(void) {
  gpio_set(GPIOB, SCL_PIN);
  gpio_set(GPIOB, SDA_PIN);
  gpio_config(GPIOB, SCL_PIN, GPIO_CFG_OUT_OD);
  gpio_config(GPIOB, SDA_PIN, GPIO_CFG_OUT_OD);
  short_delay();
  for (int i = 0; i < 9 && !gpio_read(GPIOB, SDA_PIN); i++) {
    gpio_clr(GPIOB, SCL_PIN);
    short_delay();
    gpio_set(GPIOB, SCL_PIN);
    short_delay();
  }
  gpio_clr(GPIOB, SDA_PIN);
  short_delay();
  gpio_set(GPIOB, SDA_PIN);
  short_delay();
}

static void i2c_init(void) {
  RCC->APB1ENR |= RCC_APB1ENR_I2C1EN;
  i2c_bus_recover();
  gpio_config(GPIOB, SCL_PIN, GPIO_CFG_AF_OD);
  gpio_config(GPIOB, SDA_PIN, GPIO_CFG_AF_OD);

  I2C1->CR1 = I2C_CR1_SWRST;
  I2C1->CR1 = 0;
  I2C1->CR2 = 36;                // PCLK1 = 36 MHz
  I2C1->CCR = I2C_CCR_FS | 30;   // fast mode, Tlow = 2*Thigh -> 400 kHz
  I2C1->TRISE = 11;              // 300 ns * 36 MHz + 1
  I2C1->CR1 = I2C_CR1_PE;
}

static bool wait_sr1(uint32_t mask) {
  for (uint32_t t = I2C_TIMEOUT; t; t--) {
    uint32_t sr1 = I2C1->SR1;
    if (sr1 & mask) return true;
    if (sr1 & (I2C_SR1_AF | I2C_SR1_BERR | I2C_SR1_ARLO)) return false;
  }
  return false;
}

// One transaction: START, addr+W, control byte, payload, STOP.
static bool i2c_write(uint8_t addr, uint8_t ctrl, const uint8_t *data, uint32_t len) {
  uint32_t t = I2C_TIMEOUT;
  while (I2C1->SR2 & I2C_SR2_BUSY) {
    if (!--t) goto fail;
  }
  I2C1->CR1 |= I2C_CR1_START;
  if (!wait_sr1(I2C_SR1_SB)) goto fail;
  I2C1->DR = (uint32_t)addr << 1;
  if (!wait_sr1(I2C_SR1_ADDR)) goto fail;
  (void)I2C1->SR2;  // clears ADDR
  I2C1->DR = ctrl;
  for (uint32_t i = 0; i < len; i++) {
    if (!wait_sr1(I2C_SR1_TXE)) goto fail;
    I2C1->DR = data[i];
  }
  if (!wait_sr1(I2C_SR1_BTF)) goto fail;
  I2C1->CR1 |= I2C_CR1_STOP;
  return true;

fail:
  I2C1->CR1 |= I2C_CR1_STOP;
  I2C1->SR1 = 0;  // clear AF/BERR/ARLO
  return false;
}

static bool oled_cmds(const uint8_t *cmds, uint32_t n) { return i2c_write(OLED_I2C_ADDR, 0x00, cmds, n); }

// Page addressing only (0xB0+page, column low/high nibble): supported by both
// SSD1306 and SH1106. The SSD1306-only window commands (0x21/0x22) are avoided.
static bool set_cursor(uint8_t page, uint8_t col) {
  const uint8_t c[] = {(uint8_t)(0xB0u | page), (uint8_t)(col & 0x0Fu), (uint8_t)(0x10u | (col >> 4))};
  return oled_cmds(c, sizeof c);
}

bool oled_init(void) {
  // Every command here is valid on SSD1306 and harmless on SH1106 (where the
  // SSD1306-only ones decode as column-address commands that we overwrite).
  static const uint8_t init_seq[] = {
      0xAE,        // display off
      0xD5, 0x80,  // clock divide ratio / oscillator
      0xA8, 0x3F,  // multiplex ratio 64
      0xD3, 0x00,  // display offset 0
      0x40,        // start line 0
      0x8D, 0x14,  // SSD1306: charge pump on
      0x20, 0x02,  // SSD1306: page addressing mode
      0xA1,        // segment remap
      0xC8,        // COM scan direction remapped
      0xDA, 0x12,  // COM pins configuration
      0x81, 0xCF,  // contrast
      0xD9, 0xF1,  // pre-charge period
      0xDB, 0x40,  // VCOMH deselect level
      0xA4,        // display follows RAM
      0xA6,        // normal (non-inverted)
  };
  static const uint8_t on = 0xAF;

  i2c_init();
  s_present = oled_cmds(init_seq, sizeof init_seq);
  if (!s_present) return false;

  // Clear the whole controller RAM (132 columns on SH1106) so no power-up
  // garbage remains outside the area we draw.
  uint8_t blank[OLED_RAM_W];
  memset(blank, 0, sizeof blank);
  for (uint8_t p = 0; p < OLED_PAGES; p++) {
    if (!set_cursor(p, 0) || !i2c_write(OLED_I2C_ADDR, 0x40, blank, sizeof blank)) {
      s_present = false;
      return false;
    }
  }
  s_present = oled_cmds(&on, 1);
  return s_present;
}

bool oled_present(void) { return s_present; }

bool oled_write_page(uint8_t page, const uint8_t buf[OLED_W]) {
  if (!s_present) return false;
  if (!set_cursor(page, OLED_COL_OFFSET) || !i2c_write(OLED_I2C_ADDR, 0x40, buf, OLED_W)) {
    s_present = false;  // will be re-probed by the UI
    return false;
  }
  return true;
}

void oled_render_text(uint8_t buf[OLED_W], const char *s, bool invert) {
  memset(buf, 0, OLED_W);
  for (unsigned col = 0; col < OLED_COLS && *s; col++, s++) {
    char c = *s;
    if (c < FONT6X8_FIRST || c > FONT6X8_LAST) c = '?';
    memcpy(&buf[col * 6], font6x8[c - FONT6X8_FIRST], 5);
  }
  if (invert)
    for (unsigned i = 0; i < OLED_W; i++) buf[i] ^= 0xFF;
}
