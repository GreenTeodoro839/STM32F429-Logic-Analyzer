// Built-in test signals for self-checking the analyzer (all on JD2):
//   PB0 JD2-10 = 100 kHz square (TIM3_CH3)
//   PB1 JD2-12 = 1 MHz square (TIM1_CH3N)
//   PA5 JD2-16 = 1 kHz square (TIM2_CH1)
//   PA9 JD2-7  = USART1_TX 115200 8N1, one numbered text line every 10 ms
#pragma once

void testsig_init(void);
void testsig_poll(void);  // call often (also during captures)
