// Board definition: STM32F103C8T6 minimal system board
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "stm32f1xx.h"

// ---------------------------------------------------------------------------
// F1 GPIO 4-bit pin configurations: CNF[1:0] MODE[1:0] (outputs = 50 MHz)
// ---------------------------------------------------------------------------
#define GPIO_CFG_ANALOG   0x0u
#define GPIO_CFG_IN_FLOAT 0x4u
#define GPIO_CFG_IN_PULL  0x8u  // pull-up / pull-down selected via ODR
#define GPIO_CFG_OUT_PP   0x3u
#define GPIO_CFG_OUT_OD   0x7u
#define GPIO_CFG_AF_PP    0xBu
#define GPIO_CFG_AF_OD    0xFu

static inline void gpio_config(GPIO_TypeDef *port, unsigned pin, uint32_t cfg) {
  volatile uint32_t *reg = (pin < 8u) ? &port->CRL : &port->CRH;
  unsigned sh = (pin & 7u) * 4u;
  *reg = (*reg & ~(0xFu << sh)) | (cfg << sh);
}
static inline void gpio_set(GPIO_TypeDef *port, unsigned pin) { port->BSRR = 1u << pin; }
static inline void gpio_clr(GPIO_TypeDef *port, unsigned pin) { port->BRR = 1u << pin; }
static inline bool gpio_read(GPIO_TypeDef *port, unsigned pin) { return (port->IDR >> pin) & 1u; }

// ---------------------------------------------------------------------------
// Logic analyzer inputs: PA0..PA7 = CH0..CH7 (low byte of GPIOA->IDR)
// NOTE: PA0..PA7 are NOT 5 V tolerant.
// ---------------------------------------------------------------------------
#define LA_PORT           GPIOA
#define LA_NUM_CH         8
#define LA_INPUT_PULLDOWN 1  // weak (~40k) pull-down so open inputs read 0

// On-board LED (PC13, active low on most minimal boards)
#define LED_PORT       GPIOC
#define LED_PIN        13
#define LED_ACTIVE_LOW 1

// 128x64 I2C OLED on I2C1: PB6 = SCL, PB7 = SDA.
// The module turned out to use an SH1106-compatible controller (132-column RAM,
// visible area starts at column 2). With an SSD1306 the offset of 2 merely
// shifts the picture 2 px to the right, so it is safe for both.
#define OLED_I2C_ADDR   0x3C
#define OLED_COL_OFFSET 2
#define OLED_RAM_W      132

// Built-in test signals (connect with a jumper wire to any channel):
//   PB0  TIM3_CH3   100 kHz square wave
//   PB8  TIM4_CH3   1 kHz square wave
//   PA9  USART1_TX  115200 8N1 text, one line every 10 ms

static inline void led_set(bool on) {
  if (on ^ LED_ACTIVE_LOW) gpio_set(LED_PORT, LED_PIN);
  else gpio_clr(LED_PORT, LED_PIN);
}
