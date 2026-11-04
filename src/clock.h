#pragma once

#include <stdbool.h>
#include <stdint.h>

// SYSCLK = 72 MHz from 8 MHz HSE, AHB 72, APB1 36, APB2 72, USB 48 MHz.
// All timers therefore run from a 72 MHz clock.
#define TIMER_CLK_HZ 72000000u

extern bool g_clock_hse_ok;  // false: HSE failed, running from HSI (no USB)

void clock_init(void);
uint32_t millis(void);
void delay_ms(uint32_t ms);
