#include "sdram.h"

#include "board.h"
#include "clock.h"

// FMC pins, all AF12:
//   A0-A5 PF0-5, A6-A9 PF12-15, A10-A12 PG0-2, BA0/BA1 PG4/PG5
//   D0/D1 PD14/15, D2/D3 PD0/1, D4-D12 PE7-15, D13-D15 PD8-10
//   NBL0/NBL1 PE0/PE1, SDCLK PG8, SDNCAS PG15, SDNRAS PF11, SDNWE PC0,
//   SDCKE0 PH2, SDNE0 PH3
static const struct {
  GPIO_TypeDef *port;
  uint16_t pins;
} s_fmc_pins[] = {
    {GPIOC, 1u << 0},
    {GPIOD, (1u << 0) | (1u << 1) | (1u << 8) | (1u << 9) | (1u << 10) | (1u << 14) | (1u << 15)},
    {GPIOE, (1u << 0) | (1u << 1) | 0xFF80u},
    {GPIOF, 0x003Fu | (1u << 11) | 0xF000u},
    {GPIOG, (1u << 0) | (1u << 1) | (1u << 2) | (1u << 4) | (1u << 5) | (1u << 8) | (1u << 15)},
    {GPIOH, (1u << 2) | (1u << 3)},
};

#define CMD_CLK_ENABLE  1u
#define CMD_PALL        2u
#define CMD_AUTOREFRESH 3u
#define CMD_LOAD_MODE   4u

static void sdram_cmd(uint32_t mode, uint32_t refresh, uint32_t mrd) {
  while (FMC_Bank5_6->SDSR & FMC_SDSR_BUSY) {
  }
  FMC_Bank5_6->SDCMR = mode | FMC_SDCMR_CTB1 | ((refresh - 1u) << FMC_SDCMR_NRFS_Pos) | (mrd << FMC_SDCMR_MRD_Pos);
  while (FMC_Bank5_6->SDSR & FMC_SDSR_BUSY) {
  }
}

void sdram_init(void) {
  RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN | RCC_AHB1ENR_GPIODEN | RCC_AHB1ENR_GPIOEEN | RCC_AHB1ENR_GPIOFEN |
                  RCC_AHB1ENR_GPIOGEN | RCC_AHB1ENR_GPIOHEN;
  RCC->AHB3ENR |= RCC_AHB3ENR_FMCEN;
  (void)RCC->AHB3ENR;

  for (unsigned i = 0; i < sizeof s_fmc_pins / sizeof s_fmc_pins[0]; i++)
    for (unsigned pin = 0; pin < 16; pin++)
      if (s_fmc_pins[i].pins & (1u << pin)) gpio_af(s_fmc_pins[i].port, pin, 12, GPIO_SPEED_MAX);

  // SDCLK = HCLK / 2 = 60 MHz (16.7 ns). 9 column bits, 13 row bits, 16-bit,
  // 4 banks, CAS latency 2, read burst (prefetch) on. SDCLK/RBURST/RPIPE
  // live in SDCR1 only.
  FMC_Bank5_6->SDCR[0] = (1u << FMC_SDCR1_NC_Pos) | (2u << FMC_SDCR1_NR_Pos) | (1u << FMC_SDCR1_MWID_Pos) |
                         FMC_SDCR1_NB | (2u << FMC_SDCR1_CAS_Pos) | (2u << FMC_SDCR1_SDCLK_Pos) |
                         FMC_SDCR1_RBURST | (0u << FMC_SDCR1_RPIPE_Pos);
  // Timings in SDCLK cycles minus 1. Datasheet (-6 grade): tMRD 2 clk,
  // tXSR 72 ns, tRAS 42 ns, tRC 60 ns, tWR 2 clk, tRP 15 ns, tRCD 15 ns.
  // Rounded up generously: MRD 2, XSR 7, RAS 4, RC 7, WR 3, RP 2, RCD 2.
  FMC_Bank5_6->SDTR[0] = ((2u - 1u) << FMC_SDTR1_TMRD_Pos) | ((7u - 1u) << FMC_SDTR1_TXSR_Pos) |
                         ((4u - 1u) << FMC_SDTR1_TRAS_Pos) | ((7u - 1u) << FMC_SDTR1_TRC_Pos) |
                         ((3u - 1u) << FMC_SDTR1_TWR_Pos) | ((2u - 1u) << FMC_SDTR1_TRP_Pos) |
                         ((2u - 1u) << FMC_SDTR1_TRCD_Pos);

  sdram_cmd(CMD_CLK_ENABLE, 1, 0);
  delay_us(200);  // power-up delay (>= 100 us with the clock running)
  sdram_cmd(CMD_PALL, 1, 0);
  sdram_cmd(CMD_AUTOREFRESH, 8, 0);
  // Mode register: burst length 1, sequential, CAS 2, single write burst
  sdram_cmd(CMD_LOAD_MODE, 1, 0x0220u);
  // 8192 refreshes per 64 ms: 7.81 us * 60 MHz = 468, minus 20 for safety
  FMC_Bank5_6->SDRTR = 448u << FMC_SDRTR_COUNT_Pos;
  while (FMC_Bank5_6->SDSR & FMC_SDSR_BUSY) {
  }
}

static uint32_t pattern(uint32_t i) { return i * 2654435761u ^ 0xA5C35A3Cu; }

bool sdram_test(void) {
  volatile uint32_t *const m = (volatile uint32_t *)SDRAM_BASE;
  const uint32_t words = SDRAM_SIZE / 4u;

  // Data bus: walking ones in one word
  for (unsigned b = 0; b < 32; b++) {
    m[0] = 1u << b;
    if (m[0] != 1u << b) return false;
  }
  // Address bus: one word at each power-of-two offset must be unique
  m[0] = 0x55555555u;
  for (uint32_t off = 1; off < words; off <<= 1) m[off] = 0x55555555u;
  m[0] = 0xAAAAAAAAu;
  for (uint32_t off = 1; off < words; off <<= 1)
    if (m[off] != 0x55555555u) return false;  // stuck / shorted address line
  m[0] = 0x55555555u;
  for (uint32_t off = 1; off < words; off <<= 1) {
    m[off] = 0xAAAAAAAAu;
    if (m[0] != 0x55555555u) return false;
    for (uint32_t o2 = 1; o2 < words; o2 <<= 1)
      if (o2 != off && m[o2] != 0x55555555u) return false;
    m[off] = 0x55555555u;
  }
  // Byte lanes (NBL0/NBL1)
  volatile uint8_t *const b8 = (volatile uint8_t *)SDRAM_BASE;
  m[1] = 0;
  b8[5] = 0x12;
  b8[6] = 0x34;
  if (m[1] != 0x00341200u) return false;
  // 256 KiB pseudo-random pattern, spread over all four banks
  for (uint32_t bank = 0; bank < 4; bank++) {
    volatile uint32_t *p = m + bank * (words / 4u);
    for (uint32_t i = 0; i < 16384u; i++) p[i] = pattern(i + bank);
  }
  for (uint32_t bank = 0; bank < 4; bank++) {
    volatile uint32_t *p = m + bank * (words / 4u);
    for (uint32_t i = 0; i < 16384u; i++)
      if (p[i] != pattern(i + bank)) return false;
  }
  return true;
}
