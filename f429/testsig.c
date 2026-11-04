#include "testsig.h"

#include <stdbool.h>

#include "board.h"
#include "clock.h"

#define UART_PERIOD_MS 10u

static char s_line[32];
static volatile uint8_t s_pos, s_len;
static volatile bool s_busy;
static uint32_t s_last_ms, s_counter;

// PWM mode 1, 50 % duty on channel `ch` (1..4)
static void pwm(TIM_TypeDef *tim, unsigned ch, uint32_t psc, uint32_t arr) {
  tim->CR1 = 0;
  tim->PSC = psc;
  tim->ARR = arr;
  volatile uint32_t *ccr = &tim->CCR1 + (ch - 1u);
  *ccr = (arr + 1u) / 2u;
  volatile uint32_t *ccmr = ch <= 2u ? &tim->CCMR1 : &tim->CCMR2;
  unsigned sh = ((ch - 1u) & 1u) * 8u;
  *ccmr = (*ccmr & ~(0xFFu << sh)) | ((6u << TIM_CCMR1_OC1M_Pos) | TIM_CCMR1_OC1PE) << sh;
  tim->EGR = TIM_EGR_UG;
  tim->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN;
}

// TXE is the only interrupt source. Peripheral accesses compete with the
// sampling DMA for the AHB1 bus, so this is kept to one read and one write per
// character. The SR read is needed: the DR write is posted, and without it the
// interrupt is re-entered before TXE has cleared, overwriting a character.
void USART1_IRQHandler(void) {
  if (!(USART1->SR & USART_SR_TXE)) return;
  if (s_pos < s_len) {
    USART1->DR = (uint8_t)s_line[s_pos++];
  } else {
    USART1->CR1 = USART_CR1_UE | USART_CR1_TE;  // TXEIE off
    s_busy = false;
  }
}

static void fmt_line(uint32_t n) {
  static const char head[] = "LA-F429 test ";
  unsigned k = 0;
  for (; head[k]; k++) s_line[k] = head[k];
  for (int d = 4; d >= 0; d--) {
    s_line[k + (unsigned)d] = (char)('0' + n % 10u);
    n /= 10u;
  }
  k += 5;
  s_line[k++] = '\r';
  s_line[k++] = '\n';
  s_len = (uint8_t)k;
}

void testsig_init(void) {
  RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN;
  RCC->APB1ENR |= RCC_APB1ENR_TIM2EN | RCC_APB1ENR_TIM3EN;
  RCC->APB2ENR |= RCC_APB2ENR_TIM1EN | RCC_APB2ENR_USART1EN;
  (void)RCC->APB2ENR;

  gpio_af(GPIOB, 0, 2, GPIO_SPEED_MED);                // TIM3_CH3
  pwm(TIM3, 3, 0, TIMER1_CLK_HZ / 100000u - 1u);       // 100 kHz
  TIM3->CCER = TIM_CCER_CC3E;

  gpio_af(GPIOB, 1, 1, GPIO_SPEED_HIGH);               // TIM1_CH3N
  pwm(TIM1, 3, 0, TIMER_CLK_HZ / 1000000u - 1u);       // 1 MHz
  TIM1->CCER = TIM_CCER_CC3NE;
  TIM1->BDTR = TIM_BDTR_MOE;

  gpio_af(GPIOA, 5, 1, GPIO_SPEED_LOW);                // TIM2_CH1
  pwm(TIM2, 1, TIMER1_CLK_HZ / 1000000u - 1u, 999u);   // 1 MHz / 1000 = 1 kHz
  TIM2->CCER = TIM_CCER_CC1E;

  gpio_af(GPIOA, 9, 7, GPIO_SPEED_LOW);                // USART1_TX
  USART1->BRR = (PCLK2_HZ + 115200u / 2u) / 115200u;   // APB2 = 60 MHz, 16x oversampling
  USART1->CR1 = USART_CR1_UE | USART_CR1_TE;
  NVIC_SetPriority(USART1_IRQn, 5);
  NVIC_EnableIRQ(USART1_IRQn);
}

void testsig_poll(void) {
  uint32_t now = millis();
  if ((uint32_t)(now - s_last_ms) < UART_PERIOD_MS) return;
  if (s_busy) return;  // still sending
  s_last_ms = now;
  fmt_line(s_counter++ % 100000u);
  s_pos = 0;
  s_busy = true;
  USART1->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_TXEIE;
}
