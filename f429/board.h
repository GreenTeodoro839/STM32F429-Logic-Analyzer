// Board definition: STM32F429IGT6 development board (25 MHz HSE, 32 MB
// SDRAM, 1.3" 240x240 ST7789VW LCD on SPI5). Only JD1, JD2 and JD3 can be
// reached with jumper wires; J1/J2 are fine-pitch board-to-board connectors.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "la_config.h"
#include "stm32f4xx.h"

// ---------------------------------------------------------------------------
// GPIO helpers
// ---------------------------------------------------------------------------
enum { GPIO_IN = 0, GPIO_OUT = 1, GPIO_AF = 2, GPIO_ANALOG = 3 };
enum { GPIO_NOPULL = 0, GPIO_PULLUP = 1, GPIO_PULLDOWN = 2 };
enum { GPIO_SPEED_LOW = 0, GPIO_SPEED_MED = 1, GPIO_SPEED_HIGH = 2, GPIO_SPEED_MAX = 3 };

static inline void gpio_mode(GPIO_TypeDef *p, unsigned pin, unsigned mode) {
  p->MODER = (p->MODER & ~(3u << (2u * pin))) | (mode << (2u * pin));
}
static inline void gpio_pull(GPIO_TypeDef *p, unsigned pin, unsigned pull) {
  p->PUPDR = (p->PUPDR & ~(3u << (2u * pin))) | (pull << (2u * pin));
}
static inline void gpio_speed(GPIO_TypeDef *p, unsigned pin, unsigned speed) {
  p->OSPEEDR = (p->OSPEEDR & ~(3u << (2u * pin))) | (speed << (2u * pin));
}
// Alternate function `af` (push-pull, no pull), with the given output speed.
static inline void gpio_af(GPIO_TypeDef *p, unsigned pin, unsigned af, unsigned speed) {
  volatile uint32_t *afr = &p->AFR[pin >> 3];
  unsigned sh = (pin & 7u) * 4u;
  *afr = (*afr & ~(0xFu << sh)) | (af << sh);
  gpio_speed(p, pin, speed);
  gpio_mode(p, pin, GPIO_AF);
}
static inline void gpio_set(GPIO_TypeDef *p, unsigned pin) { p->BSRR = 1u << pin; }
static inline void gpio_clr(GPIO_TypeDef *p, unsigned pin) { p->BSRR = 1u << (pin + 16u); }
static inline void gpio_write(GPIO_TypeDef *p, unsigned pin, bool v) { p->BSRR = v ? 1u << pin : 1u << (pin + 16u); }
static inline bool gpio_read(GPIO_TypeDef *p, unsigned pin) { return (p->IDR >> pin) & 1u; }

// ---------------------------------------------------------------------------
// Logic analyzer inputs: PI0..PI7 = CH0..CH7 (low byte of GPIOI->IDR).
// Weak (~40k) pull-downs, so open inputs read 0. 3.3 V signals only.
//   CH0 PI0 JD2-15   CH4 PI4 JD1-13
//   CH1 PI1 JD2-9    CH5 PI5 JD1-3
//   CH2 PI2 JD2-11   CH6 PI6 JD1-14
//   CH3 PI3 JD2-13   CH7 PI7 JD1-15
//   GND: JD1-2, JD2-3, JD2-23, JD2-24
// ---------------------------------------------------------------------------
#define LA_PORT GPIOI

// LEDs (active high, through 2.2k): LED1 PB6, LED2 PB7, LED3 PB14, LED4 PA0
#define LED1_PORT GPIOB
#define LED1_PIN  6
#define LED2_PORT GPIOB
#define LED2_PIN  7

// Keys (active low, 10k pull-ups on the board)
#define KEY1_PORT GPIOG
#define KEY1_PIN  6
#define KEY2_PORT GPIOG
#define KEY2_PIN  7

// 1.3" 240x240 ST7789VW: SPI5 SCK PH6 / MOSI PF9 (AF5), D/C PH7, RESET PH13.
// CS is tied low and the backlight is always on.
#define LCD_DC_PORT  GPIOH
#define LCD_DC_PIN   7
#define LCD_RST_PORT GPIOH
#define LCD_RST_PIN  13

// Built-in test signals (connect with a jumper wire to any channel):
//   PB0 JD2-10  TIM3_CH3   100 kHz square wave    (next to CH1 PI1 JD2-9)
//   PB1 JD2-12  TIM1_CH3N  1 MHz square wave      (next to CH2 PI2 JD2-11)
//   PA5 JD2-16  TIM2_CH1   1 kHz square wave      (next to CH0 PI0 JD2-15)
//   PA9 JD2-7   USART1_TX  115200 8N1 text, one line every 10 ms

static inline void led_set(bool on) { gpio_write(LED1_PORT, LED1_PIN, on); }
