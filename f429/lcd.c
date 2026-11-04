#include "lcd.h"

#include "board.h"
#include "clock.h"

// SPI5 on APB2 (60 MHz) / 4 = 15 MHz: a full frame takes 61 ms. The panel's
// vendor demo ran at 22.5 MHz; partial updates make the margin unnecessary.
#define SPI_BR_DIV4 (1u << SPI_CR1_BR_Pos)

static bool s_dma_active;

// Register initialisation of the panel's vendor demo (same ST7789VW module),
// except MADCTL, which is set from LCD_MADCTL. Format: cmd, n, data[n].
static const uint8_t s_init[] = {
    0x36, 1, LCD_MADCTL,                    // memory access control
    0x3A, 1, 0x05,                          // 16-bit RGB565
    0xB2, 5, 0x0C, 0x0C, 0x00, 0x33, 0x33,  // porch
    0xB7, 1, 0x35,                          // gate control
    0xBB, 1, 0x19,                          // VCOM
    0xC0, 1, 0x2C,                          // LCM control
    0xC2, 1, 0x01,                          // VDV/VRH enable
    0xC3, 1, 0x12,                          // VRH
    0xC4, 1, 0x20,                          // VDV
    0xC6, 1, 0x0F,                          // 60 Hz frame rate
    0xD0, 2, 0xA4, 0xA1,                    // power control
    0xE0, 14, 0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F, 0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23,
    0xE1, 14, 0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F, 0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23,
    0x21, 0,  // display inversion on (IPS panel)
};

static void spi_wait_idle(void) {
  while (!(SPI5->SR & SPI_SR_TXE)) {
  }
  while (SPI5->SR & SPI_SR_BSY) {
  }
}

static void spi_frame16(bool on) {
  bool is16 = (SPI5->CR1 & SPI_CR1_DFF) != 0;
  if (is16 == on) return;
  SPI5->CR1 &= ~SPI_CR1_SPE;  // DFF may only change while disabled
  if (on) SPI5->CR1 |= SPI_CR1_DFF;
  else SPI5->CR1 &= ~SPI_CR1_DFF;
  SPI5->CR1 |= SPI_CR1_SPE;
}

static void write_cmd(uint8_t cmd, const uint8_t *data, unsigned n) {
  spi_frame16(false);
  gpio_clr(LCD_DC_PORT, LCD_DC_PIN);
  SPI5->DR = cmd;
  spi_wait_idle();
  gpio_set(LCD_DC_PORT, LCD_DC_PIN);
  for (unsigned i = 0; i < n; i++) {
    while (!(SPI5->SR & SPI_SR_TXE)) {
    }
    SPI5->DR = data[i];
  }
  spi_wait_idle();
}

static void set_window(unsigned x0, unsigned y0, unsigned x1, unsigned y1) {  // inclusive
  x0 += LCD_COL_OFFSET;
  x1 += LCD_COL_OFFSET;
  y0 += LCD_ROW_OFFSET;
  y1 += LCD_ROW_OFFSET;
  const uint8_t col[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1};
  const uint8_t row[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1};
  write_cmd(0x2A, col, 4);
  write_cmd(0x2B, row, 4);
  write_cmd(0x2C, 0, 0);  // memory write; pixel data follows with D/C high
}

bool lcd_busy(void) {
  if (!s_dma_active) return false;
  if (!(DMA2->HISR & DMA_HISR_TCIF4)) return true;
  // The DMA has handed over the last pixel; wait for it to leave the shifter.
  if (!(SPI5->SR & SPI_SR_TXE) || (SPI5->SR & SPI_SR_BSY)) return true;
  SPI5->CR2 &= ~SPI_CR2_TXDMAEN;
  DMA2_Stream4->CR = 0;
  s_dma_active = false;
  return false;
}

void lcd_wait(void) {
  while (lcd_busy()) {
  }
}

void lcd_flush_rows(const uint16_t *fb, unsigned y0, unsigned y1) {
  lcd_wait();
  if (y1 > LCD_H) y1 = LCD_H;
  if (y0 >= y1) return;
  set_window(0, y0, LCD_W - 1u, y1 - 1u);
  spi_frame16(true);

  // DMA2 stream 4 channel 2 = SPI5_TX; 16-bit, memory increment
  DMA2_Stream4->CR = 0;
  while (DMA2_Stream4->CR & DMA_SxCR_EN) {
  }
  DMA2->HIFCR = DMA_HIFCR_CTCIF4 | DMA_HIFCR_CHTIF4 | DMA_HIFCR_CTEIF4 | DMA_HIFCR_CDMEIF4 | DMA_HIFCR_CFEIF4;
  DMA2_Stream4->PAR = (uint32_t)&SPI5->DR;
  DMA2_Stream4->M0AR = (uint32_t)&fb[y0 * LCD_W];
  DMA2_Stream4->NDTR = (y1 - y0) * LCD_W;  // <= 57600
  DMA2_Stream4->FCR = 0;                     // direct mode
  DMA2_Stream4->CR = (2u << DMA_SxCR_CHSEL_Pos) | DMA_SxCR_MSIZE_0 | DMA_SxCR_PSIZE_0 | DMA_SxCR_MINC |
                     DMA_SxCR_DIR_0 | DMA_SxCR_EN;
  s_dma_active = true;
  SPI5->CR2 |= SPI_CR2_TXDMAEN;
}

void lcd_init(void) {
  RCC->AHB1ENR |= RCC_AHB1ENR_GPIOFEN | RCC_AHB1ENR_GPIOHEN | RCC_AHB1ENR_DMA2EN;
  RCC->APB2ENR |= RCC_APB2ENR_SPI5EN;
  (void)RCC->APB2ENR;

  gpio_set(LCD_DC_PORT, LCD_DC_PIN);
  gpio_mode(LCD_DC_PORT, LCD_DC_PIN, GPIO_OUT);
  gpio_speed(LCD_DC_PORT, LCD_DC_PIN, GPIO_SPEED_HIGH);
  gpio_clr(LCD_RST_PORT, LCD_RST_PIN);
  gpio_mode(LCD_RST_PORT, LCD_RST_PIN, GPIO_OUT);
  gpio_af(GPIOH, 6, 5, GPIO_SPEED_HIGH);  // SPI5_SCK
  gpio_af(GPIOF, 9, 5, GPIO_SPEED_HIGH);  // SPI5_MOSI

  // Master, transmit-only (bidirectional mode, output enabled), software NSS,
  // mode 3 (CPOL = 1, CPHA = 1), MSB first. CS is tied low on the board, so
  // the idle-high clock is set up while the panel is still held in reset.
  SPI5->CR1 = SPI_CR1_BIDIMODE | SPI_CR1_BIDIOE | SPI_CR1_SSM | SPI_CR1_SSI | SPI_CR1_MSTR | SPI_BR_DIV4 |
              SPI_CR1_CPOL | SPI_CR1_CPHA;
  SPI5->CR2 = 0;
  SPI5->CR1 |= SPI_CR1_SPE;

  delay_ms(20);
  gpio_set(LCD_RST_PORT, LCD_RST_PIN);
  delay_ms(120);
  write_cmd(0x11, 0, 0);  // sleep out
  delay_ms(120);
  for (unsigned i = 0; i < sizeof s_init;) {
    uint8_t cmd = s_init[i], n = s_init[i + 1];
    write_cmd(cmd, &s_init[i + 2], n);
    i += 2u + n;
  }
}

void lcd_display_on(void) {
  lcd_wait();
  write_cmd(0x29, 0, 0);
}
