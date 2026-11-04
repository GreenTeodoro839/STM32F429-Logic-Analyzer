#include "capture.h"

#include "board.h"
#include "clock.h"

typedef uint32_t __attribute__((may_alias)) u32_alias;

static uint8_t s_buf[CAPTURE_BUF_SIZE] __attribute__((section(".noinit"), aligned(4)));
static volatile uint32_t s_turns;  // completed passes over the circular buffer

uint8_t capture_sample(uint32_t index) { return s_buf[index % CAPTURE_BUF_SIZE]; }
const uint8_t *capture_buffer(void) { return s_buf; }

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

// Split n timer ticks into (psc + 1) * (arr + 1), both <= 65536.
static capture_timing_t timing_from_ticks(uint32_t n, bool exact) {
  capture_timing_t t;
  if (n < CAPTURE_MIN_TICKS) {
    n = CAPTURE_MIN_TICKS;
    exact = false;
  }
  uint32_t p = (n + 65535u) / 65536u;
  if (!p) p = 1;
  uint32_t found = 0;
  for (uint32_t q = p; q <= 65536u && q <= n; q++) {
    if (n % q == 0 && n / q <= 65536u) {
      found = q;
      break;
    }
  }
  if (!found) {  // n has a large prime factor: round to a representable value
    found = p;
    n = ((n + found / 2u) / found) * found;
    exact = false;
  }
  t.psc = (uint16_t)(found - 1u);
  t.arr = (uint16_t)(n / found - 1u);
  t.ticks = n;
  t.rate_hz = (TIMER_CLK_HZ + n / 2u) / n;
  t.exact = exact;
  return t;
}

// SUMP rates are 100 MHz / (d + 1); the timer runs at 72 MHz, so the ideal
// tick count per sample is 72 * (d + 1) / 100 = 18 * (d + 1) / 25. It is an
// integer (exact rate) whenever (d + 1) is a multiple of 25, i.e. for every
// rate of the form 4 MHz / m - which covers all 1-2-5 presets up to 2 MHz.
capture_timing_t capture_timing(uint32_t divider) {
  uint64_t num = 18ull * ((uint64_t)divider + 1u);
  return timing_from_ticks((uint32_t)((num + 12u) / 25u), (num % 25u) == 0);
}

capture_timing_t capture_timing_hz(uint32_t rate_hz) {
  if (!rate_hz) rate_hz = 1;
  return timing_from_ticks((TIMER_CLK_HZ + rate_hz / 2u) / rate_hz, (TIMER_CLK_HZ % rate_hz) == 0);
}

// ---------------------------------------------------------------------------
// Hardware: TIM1 update -> DMA1 channel 5 (GPIOA->IDR low byte -> s_buf)
// ---------------------------------------------------------------------------

void DMA1_Channel5_IRQHandler(void) {
  if (DMA1->ISR & DMA_ISR_TCIF5) {
    DMA1->IFCR = DMA_IFCR_CTCIF5;
    s_turns++;
  }
}

static void hw_start(const capture_timing_t *t, uint32_t count, bool circular) {
  RCC->APB2ENR |= RCC_APB2ENR_TIM1EN;
  RCC->AHBENR |= RCC_AHBENR_DMA1EN;

  TIM1->CR1 = 0;
  TIM1->DIER = 0;
  TIM1->PSC = t->psc;
  TIM1->ARR = t->arr;
  TIM1->RCR = 0;
  TIM1->EGR = TIM_EGR_UG;  // load PSC/ARR (UDE still off: no DMA request)
  TIM1->SR = 0;

  DMA1_Channel5->CCR = 0;
  DMA1->IFCR = DMA_IFCR_CGIF5;
  s_turns = 0;
  NVIC_EnableIRQ(DMA1_Channel5_IRQn);
  DMA1_Channel5->CPAR = (uint32_t)&LA_PORT->IDR;
  DMA1_Channel5->CMAR = (uint32_t)s_buf;
  DMA1_Channel5->CNDTR = count;
  // Peripheral 32-bit read (GPIO must be word-accessed), memory 8-bit write:
  // the DMA stores the low byte = PA0..PA7.
  DMA1_Channel5->CCR = DMA_CCR_PL_1 | DMA_CCR_PL_0 | DMA_CCR_PSIZE_1 | DMA_CCR_MINC | DMA_CCR_TCIE |
                       (circular ? DMA_CCR_CIRC : 0u) | DMA_CCR_EN;

  TIM1->DIER = TIM_DIER_UDE;
  TIM1->CR1 = TIM_CR1_CEN;
}

static void hw_stop(void) {
  TIM1->CR1 = 0;
  TIM1->DIER = 0;
  DMA1_Channel5->CCR = 0;
  NVIC_DisableIRQ(DMA1_Channel5_IRQn);
}

// Absolute number of samples written in circular mode. Wrap-arounds are
// counted by the transfer-complete interrupt, so callers may be slow.
uint64_t capture_written(void) {
  // Read turn count, position and pending flag as one consistent snapshot:
  // with interrupts masked the ISR is either completely done or not started.
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  uint32_t t1 = s_turns;
  uint32_t pos = CAPTURE_BUF_SIZE - DMA1_Channel5->CNDTR;
  bool pending = (DMA1->ISR & DMA_ISR_TCIF5) != 0;
  __set_PRIMASK(primask);
  if (pos >= CAPTURE_BUF_SIZE) pos = 0;
  // Wrapped but the IRQ has not run yet. (A large pos means the wrap happened
  // after CNDTR was read, so this turn is not complete in our snapshot.)
  if (pending && pos < CAPTURE_BUF_SIZE / 2u) t1++;
  __asm volatile("" ::: "memory");                   // buffer changed behind our back
  return (uint64_t)t1 * CAPTURE_BUF_SIZE + pos;
}

void capture_stream_begin(const capture_timing_t *t) { hw_start(t, CAPTURE_BUF_SIZE, true); }
void capture_stream_end(void) { hw_stop(); }

// ---------------------------------------------------------------------------
// Trigger scanning
// ---------------------------------------------------------------------------

// First sample where (s & mask) == value, or -1. Tests 4 samples per word.
static int32_t find_match(const uint8_t *p, uint32_t n, uint8_t mask, uint8_t value) {
  uint32_t i = 0;
  for (; i < n && ((uintptr_t)(p + i) & 3u); i++)
    if ((p[i] & mask) == value) return (int32_t)i;
  const uint32_t m4 = mask * 0x01010101u, v4 = value * 0x01010101u;
  for (; i + 4u <= n; i += 4u) {
    uint32_t x = (*(const u32_alias *)(p + i) ^ v4) & m4;  // 0x00 byte = match
    uint32_t z = (x - 0x01010101u) & ~x & 0x80808080u;    // lowest set = first zero byte
    if (z) return (int32_t)(i + ((uint32_t)__builtin_ctz(z) >> 3));
  }
  for (; i < n; i++)
    if ((p[i] & mask) == value) return (int32_t)i;
  return -1;
}

// First sample where (s & mask) != value, or -1.
static int32_t find_mismatch(const uint8_t *p, uint32_t n, uint8_t mask, uint8_t value) {
  uint32_t i = 0;
  for (; i < n && ((uintptr_t)(p + i) & 3u); i++)
    if ((p[i] & mask) != value) return (int32_t)i;
  const uint32_t m4 = mask * 0x01010101u, v4 = value * 0x01010101u;
  for (; i + 4u <= n; i += 4u) {
    uint32_t x = (*(const u32_alias *)(p + i) ^ v4) & m4;  // non-zero byte = mismatch
    if (x) return (int32_t)(i + ((uint32_t)__builtin_ctz(x) >> 3));
  }
  for (; i < n; i++)
    if ((p[i] & mask) != value) return (int32_t)i;
  return -1;
}

typedef struct {
  const capture_cfg_t *cfg;
  uint8_t level;  // current stage
  bool armed;     // stage 0 only: a non-matching sample has been seen
} trig_state_t;

// Returns the offset of the trigger sample within p[0..n), or -1.
static int32_t scan(const uint8_t *p, uint32_t n, trig_state_t *st) {
  const capture_cfg_t *c = st->cfg;
  uint32_t i = 0;

  if (c->num_stages == 1) {  // fast path: single edge-style condition
    if (!st->armed) {
      int32_t r = find_mismatch(p, n, c->mask[0], c->value[0]);
      if (r < 0) return -1;
      st->armed = true;
      i = (uint32_t)r;
    }
    int32_t r = find_match(p + i, n - i, c->mask[0], c->value[0]);
    return r < 0 ? -1 : (int32_t)(i + (uint32_t)r);
  }

  for (; i < n; i++) {
    uint8_t lv = st->level;
    bool match = (p[i] & c->mask[lv]) == c->value[lv];
    if (!st->armed) {  // stage 0 must first see its condition false
      st->armed = !match;
      continue;
    }
    if (match) {
      if (lv + 1u >= c->num_stages) return (int32_t)i;
      st->level = lv + 1u;
    }
  }
  return -1;
}

// Call `poll` about once per millisecond.
static bool poll_due(capture_poll_fn poll, uint32_t *last_ms) {
  uint32_t now = millis();
  if (now == *last_ms) return false;
  *last_ms = now;
  return poll && poll();
}

capture_result_t capture_run(const capture_cfg_t *cfg, capture_poll_fn poll, uint32_t *start) {
  capture_timing_t t = capture_timing(cfg->divider);
  uint32_t total = cfg->total > CAPTURE_MAX_SAMPLES ? CAPTURE_MAX_SAMPLES : cfg->total;
  uint32_t pre = cfg->pre > total ? total : cfg->pre;
  uint32_t last_ms = millis();

  *start = 0;

  if (cfg->num_stages == 0) {
    // No trigger: fill exactly `total` samples, the DMA stops by itself.
    hw_start(&t, total, false);
    while (DMA1_Channel5->CNDTR != 0) {
      if (poll_due(poll, &last_ms)) {
        hw_stop();
        return CAPTURE_ABORTED;
      }
    }
    hw_stop();
    return CAPTURE_OK;
  }

  hw_start(&t, CAPTURE_BUF_SIZE, true);

  bool overrun = false;
  trig_state_t st = {cfg, 0, false};
  // Start one sample early so that sample `pre` itself can be the edge.
  uint64_t scan_at = pre ? pre - 1u : 0u;
  uint64_t trig = 0;
  bool found = false;

  while (!found) {
    uint64_t w = capture_written();
    if (w > scan_at && w - scan_at > CAPTURE_BUF_SIZE - CAPTURE_MARGIN) {
      overrun = true;  // samples were overwritten before we could look at them
      scan_at = w - (CAPTURE_BUF_SIZE - CAPTURE_MARGIN) / 2u;
      st.level = 0;
      st.armed = false;
    }
    while (scan_at < w) {
      uint32_t idx = (uint32_t)(scan_at % CAPTURE_BUF_SIZE);
      uint32_t seg = (uint32_t)(w - scan_at < CAPTURE_BUF_SIZE ? w - scan_at : CAPTURE_BUF_SIZE);
      if (seg > CAPTURE_BUF_SIZE - idx) seg = CAPTURE_BUF_SIZE - idx;
      int32_t r = scan(&s_buf[idx], seg, &st);
      if (r >= 0) {
        trig = scan_at + (uint32_t)r;
        found = true;
        break;
      }
      scan_at += seg;
    }
    if (!found && poll_due(poll, &last_ms)) {
      hw_stop();
      return CAPTURE_ABORTED;
    }
  }

  // libsigrok places the trigger marker at (pre - num_stages) (an OLS FPGA
  // pipeline quirk); shift the window so the marker lands on the real trigger.
  uint32_t shift = cfg->num_stages < pre ? cfg->num_stages : pre;
  uint64_t first = trig - pre + shift;
  uint64_t end = first + total;

  while (capture_written() < end) {
    if (poll_due(poll, &last_ms)) {
      hw_stop();
      return CAPTURE_ABORTED;
    }
  }
  hw_stop();
  if (capture_written() - first > CAPTURE_BUF_SIZE) overrun = true;

  *start = (uint32_t)(first % CAPTURE_BUF_SIZE);
  return overrun ? CAPTURE_OVERRUN : CAPTURE_OK;
}
