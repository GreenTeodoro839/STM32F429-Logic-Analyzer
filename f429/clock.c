#include "clock.h"

#include "board.h"

bool g_clock_hse_ok;
static volatile uint32_t s_ms;

void SysTick_Handler(void) { s_ms++; }

uint32_t millis(void) { return s_ms; }

void delay_ms(uint32_t ms) {
  uint32_t t0 = s_ms;
  while ((uint32_t)(s_ms - t0) < ms) {
  }
}

void delay_us(uint32_t us) {
  uint32_t t0 = cycles();
  while ((uint32_t)(cycles() - t0) < us * (HCLK_HZ / 1000000u)) {
  }
}

void clock_init(void) {
  RCC->APB1ENR |= RCC_APB1ENR_PWREN;
  PWR->CR |= PWR_CR_VOS;  // regulator scale 1 (reset default on F42x)

  RCC->CR |= RCC_CR_HSEON;
  for (uint32_t t = 500000; t && !(RCC->CR & RCC_CR_HSERDY); t--) {
  }
  g_clock_hse_ok = (RCC->CR & RCC_CR_HSERDY) != 0;
  if (!g_clock_hse_ok) RCC->CR &= ~RCC_CR_HSEON;

  // 3 wait states for 90 < HCLK <= 120 MHz at 2.7-3.6 V; ART accelerator on.
  FLASH->ACR = FLASH_ACR_ICEN | FLASH_ACR_DCEN | FLASH_ACR_PRFTEN | FLASH_ACR_LATENCY_3WS;
  while ((FLASH->ACR & FLASH_ACR_LATENCY) != FLASH_ACR_LATENCY_3WS) {
  }

  // PLL input 1 MHz (HSE 25 MHz / 25 or HSI 16 MHz / 16), VCO 240 MHz,
  // SYSCLK = VCO / 2 = 120 MHz, USB = VCO / 5 = 48 MHz.
  uint32_t m = g_clock_hse_ok ? 25u : 16u;
  RCC->PLLCFGR = (m << RCC_PLLCFGR_PLLM_Pos) | (240u << RCC_PLLCFGR_PLLN_Pos) | (0u << RCC_PLLCFGR_PLLP_Pos) |
                 (5u << RCC_PLLCFGR_PLLQ_Pos) | (g_clock_hse_ok ? RCC_PLLCFGR_PLLSRC_HSE : RCC_PLLCFGR_PLLSRC_HSI);
  RCC->CR |= RCC_CR_PLLON;
  while (!(RCC->CR & RCC_CR_PLLRDY)) {
  }

  RCC->CFGR = RCC_CFGR_HPRE_DIV1 | RCC_CFGR_PPRE1_DIV4 | RCC_CFGR_PPRE2_DIV2 | RCC_CFGR_SW_PLL;
  while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {
  }

  SystemCoreClockUpdate();
  SysTick_Config(SystemCoreClock / 1000u);
  NVIC_SetPriority(SysTick_IRQn, 3);

  // Cycle counter for delay_us() and measurements
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}
