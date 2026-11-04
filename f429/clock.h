#pragma once

#include <stdbool.h>
#include <stdint.h>

// SYSCLK = HCLK = 120 MHz from the 25 MHz HSE (VCO 240 MHz, P = 2, Q = 5 ->
// 48 MHz for USB). APB2 = 60 MHz (timers 120 MHz), APB1 = 30 MHz (timers
// 60 MHz). 120 MHz is the fastest clock that gives both exact 48 MHz for USB
// and exact 1-2-5 sample rates up to 20 MHz.
#define HCLK_HZ       120000000u
#define PCLK1_HZ      30000000u
#define PCLK2_HZ      60000000u
#define TIMER_CLK_HZ  120000000u  // TIM1, TIM8 (APB2)
#define TIMER1_CLK_HZ 60000000u   // TIM2..TIM5, TIM12..TIM14 (APB1)

extern bool g_clock_hse_ok;  // false: HSE failed, running from HSI (USB unreliable)

void clock_init(void);
uint32_t millis(void);
void delay_ms(uint32_t ms);
void delay_us(uint32_t us);
static inline uint32_t cycles(void) { return *(volatile uint32_t *)0xE0001004u; }  // DWT->CYCCNT
