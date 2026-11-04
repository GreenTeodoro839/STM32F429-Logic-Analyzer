// Simulator stand-in for f429/clock.h
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define HCLK_HZ 120000000u
extern bool g_clock_hse_ok;
uint32_t millis(void);
uint32_t cycles(void);
