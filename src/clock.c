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

void clock_init(void) {
  RCC->CR |= RCC_CR_HSEON;
  for (uint32_t t = 200000; t && !(RCC->CR & RCC_CR_HSERDY); t--) {
  }
  g_clock_hse_ok = (RCC->CR & RCC_CR_HSERDY) != 0;

  // 2 wait states for 48 < SYSCLK <= 72 MHz, prefetch buffer on
  FLASH->ACR = FLASH_ACR_PRFTBE | (2u << FLASH_ACR_LATENCY_Pos);

  uint32_t cfgr = RCC_CFGR_HPRE_DIV1 | RCC_CFGR_PPRE1_DIV2 | RCC_CFGR_PPRE2_DIV1;
  if (g_clock_hse_ok) {
    // 8 MHz x 9 = 72 MHz; USBPRE = 0 -> USB clock = 72 / 1.5 = 48 MHz
    cfgr |= RCC_CFGR_PLLSRC | RCC_CFGR_PLLMULL9;
  } else {
    // Fallback: HSI/2 x 16 = 64 MHz. USB cannot run from this.
    RCC->CR &= ~RCC_CR_HSEON;
    cfgr |= RCC_CFGR_PLLMULL16;
  }
  RCC->CFGR = cfgr;
  RCC->CR |= RCC_CR_PLLON;
  while (!(RCC->CR & RCC_CR_PLLRDY)) {
  }
  RCC->CFGR = cfgr | RCC_CFGR_SW_PLL;
  while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {
  }

  SystemCoreClockUpdate();
  SysTick_Config(SystemCoreClock / 1000u);
}
