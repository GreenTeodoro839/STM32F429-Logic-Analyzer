// Test stand-in for f429/clock.h: cycles() advances by a settable step.
#pragma once
#include <stdint.h>
extern uint32_t g_cycle_step;
uint32_t cycles(void);
