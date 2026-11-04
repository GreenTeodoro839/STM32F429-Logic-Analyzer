// Built-in test signals for self-checking the analyzer:
//   PB0 = 100 kHz square (TIM3_CH3), PB8 = 1 kHz square (TIM4_CH3),
//   PA9 = USART1_TX 115200 8N1, one numbered text line every 10 ms.
#pragma once

void testsig_init(void);
void testsig_poll(void);  // call often (also during captures)
