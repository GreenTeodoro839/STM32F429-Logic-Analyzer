#include "gen.h"

#include "board.h"
#include "clock.h"
#include "gen_calc.h"

#define UART_PERIOD_MS 10u

gen_cfg_t g_gen;
const char *const gen_pin_name[GEN_PWM_OUTPUTS + 1] = {"PB0 JD2-10", "PB1 JD2-12", "PA5 JD2-16", "PA9 JD2-7"};
const char *const gen_pattern_name[GEN_UART_PATTERNS] = {"text", "0x55", "ramp"};

static gen_timing_t s_timing[GEN_PWM_OUTPUTS];
static uint32_t s_uart_actual;

static char s_line[32];
static volatile uint8_t s_pos, s_len, s_ramp;
static volatile bool s_busy;
static uint8_t s_pattern;  // pattern the interrupt is producing
static uint32_t s_last_ms, s_counter;

// PWM mode 1 with preload on channel `ch` (1..4)
static void pwm_mode(TIM_TypeDef *tim, unsigned ch) {
  volatile uint32_t *ccmr = ch <= 2u ? &tim->CCMR1 : &tim->CCMR2;
  unsigned sh = ((ch - 1u) & 1u) * 8u;
  *ccmr = (*ccmr & ~(0xFFu << sh)) | (((6u << TIM_CCMR1_OC1M_Pos) | TIM_CCMR1_OC1PE) << sh);
}

static void pwm_set(TIM_TypeDef *tim, unsigned ch, const gen_timing_t *t, bool on) {
  tim->CR1 = 0;
  tim->PSC = t->psc;
  tim->ARR = t->arr;
  (&tim->CCR1)[ch - 1u] = on ? t->ccr : 0;  // CCR = 0: constantly low
  tim->EGR = TIM_EGR_UG;
  tim->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN;
}

// TXE is the only interrupt source. Peripheral accesses compete with the
// sampling DMA for the AHB1 bus, so this is one read and one write per
// character. The SR read is needed: the DR write is posted, and without it the
// interrupt is re-entered before TXE has cleared, overwriting a character.
void USART1_IRQHandler(void) {
  if (!(USART1->SR & USART_SR_TXE)) return;
  switch (s_pattern) {
    case GEN_UART_0x55: USART1->DR = 0x55; return;
    case GEN_UART_RAMP: USART1->DR = s_ramp++; return;
    default: break;
  }
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

static void uart_apply(void) {
  USART1->CR1 = 0;  // also stops the interrupt
  s_busy = false;
  s_pos = s_len = 0;
  USART1->BRR = gen_uart_brr(PCLK2_HZ, g_gen.uart.baud, &s_uart_actual);
  s_pattern = g_gen.uart.pattern;
  if (!g_gen.uart.on) {
    USART1->CR1 = USART_CR1_UE | USART_CR1_TE;  // idle high
    return;
  }
  if (s_pattern == GEN_UART_TEXT) USART1->CR1 = USART_CR1_UE | USART_CR1_TE;  // lines start in gen_poll()
  else USART1->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_TXEIE;           // continuous
}

void gen_apply(void) {
  static TIM_TypeDef *const tim[GEN_PWM_OUTPUTS] = {TIM3, TIM1, TIM2};
  static const uint8_t ch[GEN_PWM_OUTPUTS] = {3, 3, 1};
  static const uint32_t clk[GEN_PWM_OUTPUTS] = {TIMER1_CLK_HZ, TIMER_CLK_HZ, TIMER1_CLK_HZ};
  static const uint32_t max_arr[GEN_PWM_OUTPUTS] = {0xFFFFu, 0xFFFFu, 0xFFFFFFFFu};
  for (unsigned i = 0; i < GEN_PWM_OUTPUTS; i++) {
    if (g_gen.pwm[i].freq_hz > GEN_MAX_HZ) g_gen.pwm[i].freq_hz = GEN_MAX_HZ;
    if (g_gen.pwm[i].freq_hz < 1) g_gen.pwm[i].freq_hz = 1;
    s_timing[i] = gen_timer_calc(clk[i], max_arr[i], g_gen.pwm[i].freq_hz, g_gen.pwm[i].duty);
    pwm_set(tim[i], ch[i], &s_timing[i], g_gen.pwm[i].on);
  }
  uart_apply();
}

void gen_init(void) {
  RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN;
  RCC->APB1ENR |= RCC_APB1ENR_TIM2EN | RCC_APB1ENR_TIM3EN;
  RCC->APB2ENR |= RCC_APB2ENR_TIM1EN | RCC_APB2ENR_USART1EN;
  (void)RCC->APB2ENR;

  gpio_af(GPIOB, 0, 2, GPIO_SPEED_HIGH);  // TIM3_CH3
  pwm_mode(TIM3, 3);
  TIM3->CCER = TIM_CCER_CC3E;
  gpio_af(GPIOB, 1, 1, GPIO_SPEED_HIGH);  // TIM1_CH3N, inverted polarity: follows OC3REF
  pwm_mode(TIM1, 3);
  TIM1->CCER = TIM_CCER_CC3NE | TIM_CCER_CC3NP;
  TIM1->BDTR = TIM_BDTR_MOE;
  gpio_af(GPIOA, 5, 1, GPIO_SPEED_HIGH);  // TIM2_CH1
  pwm_mode(TIM2, 1);
  TIM2->CCER = TIM_CCER_CC1E;
  gpio_af(GPIOA, 9, 7, GPIO_SPEED_HIGH);  // USART1_TX
  NVIC_SetPriority(USART1_IRQn, 5);
  NVIC_EnableIRQ(USART1_IRQn);

  static const uint32_t def_hz[GEN_PWM_OUTPUTS] = {100000, 1000000, 1000};
  for (unsigned i = 0; i < GEN_PWM_OUTPUTS; i++) {
    g_gen.pwm[i].on = true;
    g_gen.pwm[i].freq_hz = def_hz[i];
    g_gen.pwm[i].duty = 50;
  }
  g_gen.uart.on = true;
  g_gen.uart.baud = 115200;
  g_gen.uart.pattern = GEN_UART_TEXT;
  gen_apply();
}

void gen_poll(void) {
  if (!g_gen.uart.on || s_pattern != GEN_UART_TEXT) return;
  uint32_t now = millis();
  if ((uint32_t)(now - s_last_ms) < UART_PERIOD_MS) return;
  if (s_busy) return;  // still sending
  s_last_ms = now;
  fmt_line(s_counter++ % 100000u);
  s_pos = 0;
  s_busy = true;
  USART1->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_TXEIE;
}

uint64_t gen_actual_mhz(unsigned out) { return out < GEN_PWM_OUTPUTS ? s_timing[out].actual_mhz : 0; }
unsigned gen_actual_duty(unsigned out) { return out < GEN_PWM_OUTPUTS ? s_timing[out].actual_duty : 0; }
uint32_t gen_uart_actual(void) { return s_uart_actual; }
