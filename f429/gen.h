// Signal generator on the JD2 test pins (replaces the fixed test signals;
// the power-on defaults are the same):
//   out 0  PB0 JD2-10  TIM3_CH3   square wave, 100 kHz
//   out 1  PB1 JD2-12  TIM1_CH3N  square wave, 1 MHz
//   out 2  PA5 JD2-16  TIM2_CH1   square wave, 1 kHz
//   UART   PA9 JD2-7   USART1_TX  115200 8N1, a numbered text line every 10 ms
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define GEN_PWM_OUTPUTS 3
#define GEN_MAX_HZ      10000000u

typedef enum { GEN_UART_TEXT, GEN_UART_0x55, GEN_UART_RAMP, GEN_UART_PATTERNS } gen_uart_pattern_t;

typedef struct {
  struct {
    bool on;
    uint32_t freq_hz;
    uint8_t duty;  // percent, 1..99
  } pwm[GEN_PWM_OUTPUTS];
  struct {
    bool on;
    uint32_t baud;
    uint8_t pattern;  // gen_uart_pattern_t
  } uart;
} gen_cfg_t;

extern gen_cfg_t g_gen;  // change, then call gen_apply()
extern const char *const gen_pin_name[GEN_PWM_OUTPUTS + 1];
extern const char *const gen_pattern_name[GEN_UART_PATTERNS];

void gen_init(void);   // pins, timers, UART; defaults
void gen_apply(void);  // program the hardware from g_gen
void gen_poll(void);   // starts the text lines of the UART output (call often)

uint64_t gen_actual_mhz(unsigned out);   // real frequency of a PWM output
unsigned gen_actual_duty(unsigned out);  // in 0.1 %
uint32_t gen_uart_actual(void);          // real baud rate
