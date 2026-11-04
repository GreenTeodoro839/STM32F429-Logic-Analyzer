#include "testsig.h"

#include <stdio.h>

#include "board.h"
#include "clock.h"

#define UART_PERIOD_MS 10u

static char s_line[32];
static uint32_t s_last_ms, s_counter;

static void pwm_ch3(TIM_TypeDef *tim, uint32_t psc, uint32_t arr) {
  tim->CR1 = 0;
  tim->PSC = psc;
  tim->ARR = arr;
  tim->CCR3 = (arr + 1u) / 2u;
  tim->CCMR2 = (6u << TIM_CCMR2_OC3M_Pos) | TIM_CCMR2_OC3PE;  // PWM mode 1
  tim->CCER = TIM_CCER_CC3E;
  tim->EGR = TIM_EGR_UG;
  tim->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN;
}

void testsig_init(void) {
  RCC->APB1ENR |= RCC_APB1ENR_TIM3EN | RCC_APB1ENR_TIM4EN;
  RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
  RCC->AHBENR |= RCC_AHBENR_DMA1EN;

  gpio_config(GPIOB, 0, GPIO_CFG_AF_PP);
  pwm_ch3(TIM3, 0, TIMER_CLK_HZ / 100000u - 1u);  // 100 kHz
  gpio_config(GPIOB, 8, GPIO_CFG_AF_PP);
  pwm_ch3(TIM4, TIMER_CLK_HZ / 1000000u - 1u, 1000u - 1u);  // 1 MHz / 1000 = 1 kHz

  gpio_config(GPIOA, 9, GPIO_CFG_AF_PP);
  USART1->BRR = (TIMER_CLK_HZ + 115200u / 2u) / 115200u;  // APB2 = 72 MHz
  USART1->CR3 = USART_CR3_DMAT;
  USART1->CR1 = USART_CR1_UE | USART_CR1_TE;
}

void testsig_poll(void) {
  uint32_t now = millis();
  if ((uint32_t)(now - s_last_ms) < UART_PERIOD_MS) return;
  if ((DMA1_Channel4->CCR & DMA_CCR_EN) && DMA1_Channel4->CNDTR) return;  // still sending
  s_last_ms = now;

  int n = snprintf(s_line, sizeof s_line, "LA-F103 test %05lu\r\n", (unsigned long)(s_counter++ % 100000u));
  DMA1_Channel4->CCR = 0;
  DMA1->IFCR = DMA_IFCR_CGIF4;
  DMA1_Channel4->CPAR = (uint32_t)&USART1->DR;
  DMA1_Channel4->CMAR = (uint32_t)s_line;
  DMA1_Channel4->CNDTR = (uint32_t)n;
  DMA1_Channel4->CCR = DMA_CCR_MINC | DMA_CCR_DIR | DMA_CCR_EN;  // low priority
}
