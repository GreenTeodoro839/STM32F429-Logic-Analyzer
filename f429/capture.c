#include "capture.h"

#include "board.h"
#include "capture_hw.h"
#include "clock.h"

// ---------------------------------------------------------------------------
// Hardware: TIM8 update -> DMA2 stream 1 channel 7.
//
// The DMA reads GPIOI->IDR as a byte (PI0..PI7) on every update event. Its
// FIFO packs the samples into half-words and writes 8-byte bursts, so the
// memory side (SDRAM in particular) needs one bus transaction per 8 samples
// and its latency is hidden by the FIFO. Double-buffer mode walks the ring in
// CAPTURE_CHUNK pieces: when a chunk is done, the ISR points the idle buffer
// register at the chunk after the next one.
//
// The CPU never busy-polls DMA registers: those sit on the AHB1 bus next to
// GPIOI and every access would delay a sample. Waits use DWT-timed pauses.
// ---------------------------------------------------------------------------

typedef uint32_t __attribute__((may_alias)) u32_alias;

static uint8_t s_sram_ring[CAPTURE_SRAM_RING] __attribute__((section(".sram3"), aligned(1024)));
static uint8_t s_sdram_ring[CAPTURE_SDRAM_RING] __attribute__((section(".sdram"), aligned(1024)));

#define FIFO_SLACK 16u  // samples that may sit in the DMA FIFO (4 words)

static bool s_sdram_ok;
static struct {
  uint8_t *base;
  uint32_t size, mask, nchunks;
  capture_mem_t mem;
} s_ring = {s_sram_ring, CAPTURE_SRAM_RING, CAPTURE_SRAM_RING - 1u, CAPTURE_SRAM_RING / CAPTURE_CHUNK, CAPTURE_MEM_SRAM};

static volatile uint32_t s_chunks;  // completed DMA buffers since start
static uint32_t s_start_cycles;
static volatile uint32_t s_dma_errors;
static uint32_t s_ticks, s_start_ms;  // sample period and start time of the running capture
static volatile uint32_t s_tc_cycles;  // DWT time of the last chunk completion
static bool s_timed;                   // capture_written() from DWT time instead of NDTR
static int32_t s_lost;                // requests lost in the last run (-1 = not measured)
static capture_window_t s_last;

void capture_init(bool sdram_ok) { s_sdram_ok = sdram_ok; }
bool capture_sdram_ok(void) { return s_sdram_ok; }

uint32_t capture_max_samples(void) {
  return s_sdram_ok ? CAPTURE_SDRAM_RING - CAPTURE_SDRAM_MARGIN : CAPTURE_SRAM_RING - CAPTURE_SRAM_MARGIN;
}
uint32_t capture_ring_size(void) { return s_ring.size; }
capture_window_t capture_last_window(void) { return s_last; }

// Byte offset of logical chunk c (0 <= c < nchunks) in the ring memory. In the
// SDRAM, consecutive chunks rotate through the four banks (bank = address
// bits 24:23, 8 MiB each), so a reader lagging the DMA by one to three chunks
// works in a different bank. Measured: CPU reads in the bank the DMA writes to
// are 1.7x slower (row conflicts), and long read bursts there can starve the
// DMA.
static inline uint32_t chunk_offset(uint32_t c) {
  if (s_ring.mem != CAPTURE_MEM_SDRAM) return c * CAPTURE_CHUNK;
  return ((c & 3u) * (s_ring.nchunks / 4u) + (c >> 2)) * CAPTURE_CHUNK;
}

const uint8_t *capture_span(uint64_t index, uint32_t *contiguous) {
  uint32_t i = (uint32_t)index & s_ring.mask;
  uint32_t off = i % CAPTURE_CHUNK;
  *contiguous = CAPTURE_CHUNK - off;
  return s_ring.base + chunk_offset(i / CAPTURE_CHUNK) + off;
}

uint8_t capture_sample(uint32_t index) {
  uint32_t n;
  return *capture_span(index, &n);
}

const uint8_t *capture_hw_ring(capture_mem_t mem, uint32_t *size) {
  *size = mem == CAPTURE_MEM_SDRAM ? CAPTURE_SDRAM_RING : CAPTURE_SRAM_RING;
  return mem == CAPTURE_MEM_SDRAM ? s_sdram_ring : s_sram_ring;
}

static void select_ring(capture_mem_t mem) {
  if (mem == CAPTURE_MEM_SDRAM && !s_sdram_ok) mem = CAPTURE_MEM_SRAM;
  s_ring.mem = mem;
  s_ring.base = mem == CAPTURE_MEM_SDRAM ? s_sdram_ring : s_sram_ring;
  s_ring.size = mem == CAPTURE_MEM_SDRAM ? CAPTURE_SDRAM_RING : CAPTURE_SRAM_RING;
  s_ring.mask = s_ring.size - 1u;
  s_ring.nchunks = s_ring.size / CAPTURE_CHUNK;
}

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
  t.rate_hz = (CAPTURE_TIMER_HZ + n / 2u) / n;
  t.exact = exact;
  return t;
}

// SUMP rates are 100 MHz / (d + 1); with a 120 MHz timer the ideal tick count
// is 1.2 * (d + 1) = 6 * (d + 1) / 5, an integer whenever (d + 1) is a
// multiple of 5: every 1-2-5 rate up to 20 MHz.
capture_timing_t capture_timing(uint32_t divider) {
  uint64_t num = 6ull * ((uint64_t)divider + 1u);
  uint64_t ticks = (num + 2u) / 5u;
  if (ticks > 0xFFFFFFFFull) ticks = 0xFFFFFFFFull;
  return timing_from_ticks((uint32_t)ticks, (num % 5u) == 0);
}

capture_timing_t capture_timing_hz(uint32_t rate_hz) {
  if (!rate_hz) rate_hz = 1;
  return timing_from_ticks((CAPTURE_TIMER_HZ + rate_hz / 2u) / rate_hz, (CAPTURE_TIMER_HZ % rate_hz) == 0);
}

// ---------------------------------------------------------------------------
// DMA / timer
// ---------------------------------------------------------------------------

#define DMA_S1_ERRORS (DMA_LISR_TEIF1 | DMA_LISR_DMEIF1 | DMA_LISR_FEIF1)
#define DMA_S1_FLAGS  (DMA_LISR_TCIF1 | DMA_LISR_HTIF1 | DMA_S1_ERRORS)

// Kept to three DMA register accesses: every CPU access to AHB1 can delay a
// sample read by the DMA, and at 20 MHz there is only about one spare cycle.
void DMA2_Stream1_IRQHandler(void) {
  uint32_t now = cycles();
  uint32_t isr = DMA2->LISR;
  DMA2->LIFCR = isr & DMA_S1_FLAGS;  // clear bits sit where the flags are
  if (isr & DMA_LISR_TCIF1) {
    uint32_t done = s_chunks + 1u;
    s_chunks = done;
    s_tc_cycles = now;
    // The DMA now fills chunk `done`; the register it just left (M0 for even
    // chunks) gets chunk `done + 1`.
    uint32_t next = (uint32_t)s_ring.base + chunk_offset((done + 1u) & (s_ring.nchunks - 1u));
    if (done & 1u) DMA2_Stream1->M0AR = next;
    else DMA2_Stream1->M1AR = next;
  }
  if (isr & DMA_S1_ERRORS) s_dma_errors |= isr & DMA_S1_ERRORS;
}

static void hw_start(capture_mem_t mem, const capture_timing_t *t) {
  select_ring(mem);
  RCC->APB2ENR |= RCC_APB2ENR_TIM8EN;
  RCC->AHB1ENR |= RCC_AHB1ENR_DMA2EN;
  (void)RCC->AHB1ENR;

  TIM8->CR1 = 0;
  TIM8->DIER = 0;
  TIM8->PSC = t->psc;
  TIM8->ARR = t->arr;
  TIM8->RCR = 0;
  TIM8->EGR = TIM_EGR_UG;  // load PSC/ARR (UDE still off: no DMA request)
  TIM8->SR = 0;

  DMA2_Stream1->CR = 0;
  while (DMA2_Stream1->CR & DMA_SxCR_EN) {
  }
  DMA2->LIFCR = DMA_LIFCR_CTCIF1 | DMA_LIFCR_CHTIF1 | DMA_LIFCR_CTEIF1 | DMA_LIFCR_CDMEIF1 | DMA_LIFCR_CFEIF1;
  s_chunks = 0;
  s_dma_errors = 0;
  DMA2_Stream1->PAR = (uint32_t)&LA_PORT->IDR;  // byte read: PI0..PI7
  DMA2_Stream1->M0AR = (uint32_t)s_ring.base + chunk_offset(0);
  DMA2_Stream1->M1AR = (uint32_t)s_ring.base + chunk_offset(1);
  DMA2_Stream1->NDTR = CAPTURE_CHUNK;
  // FIFO mode, threshold 1/2: every 8 samples one 4-beat half-word burst.
  // Measured lossless at 20 MHz into SRAM and SDRAM, even while the CPU
  // reads the SDRAM (a full-FIFO threshold loses one sample per burst at
  // 24 MHz and some at 20 MHz with SDRAM contention).
  DMA2_Stream1->FCR = DMA_SxFCR_DMDIS | DMA_SxFCR_FTH_0 | DMA_SxFCR_FEIE;
  DMA2_Stream1->CR = (7u << DMA_SxCR_CHSEL_Pos) | DMA_SxCR_MBURST_0 | DMA_SxCR_PL_0 | DMA_SxCR_PL_1 |
                     DMA_SxCR_MSIZE_0 | DMA_SxCR_MINC | DMA_SxCR_DBM | DMA_SxCR_TCIE | DMA_SxCR_TEIE |
                     DMA_SxCR_DMEIE;  // PSIZE = byte, peripheral-to-memory
  NVIC_SetPriority(DMA2_Stream1_IRQn, 0);
  NVIC_ClearPendingIRQ(DMA2_Stream1_IRQn);
  NVIC_EnableIRQ(DMA2_Stream1_IRQn);
  DMA2_Stream1->CR |= DMA_SxCR_EN;

  TIM8->DIER = TIM_DIER_UDE;
  s_ticks = t->ticks;
  // A chunk must take less than 2^32 cycles for the DWT-based position.
  s_timed = t->ticks <= 65536u;
  s_start_ms = millis();
  __disable_irq();  // timer start and time stamp together
  s_start_cycles = cycles();
  TIM8->CR1 = TIM_CR1_CEN;
  __enable_irq();
}

// DMA transfers (= samples read from the port) so far, including any still
// in the FIFO.
static uint64_t transfers(void) {
  __disable_irq();
  uint32_t c = s_chunks;
  uint32_t pos = CAPTURE_CHUNK - DMA2_Stream1->NDTR;
  bool pending = (DMA2->LISR & DMA_LISR_TCIF1) != 0;
  __enable_irq();
  if (pos >= CAPTURE_CHUNK) pos = 0;
  if (pending && pos < CAPTURE_CHUNK / 2u) c++;
  return (uint64_t)c * CAPTURE_CHUNK + pos;
}

static void hw_stop(void) {
  __disable_irq();
  uint32_t end = cycles();
  TIM8->CR1 = 0;
  __enable_irq();
  TIM8->DIER = 0;
  delay_us(2);  // let the DMA serve a request raised just before the stop
  // Lost requests: update events that occurred (timer and DWT both count
  // HCLK) minus transfers done. A request is lost when the DMA could not
  // serve it within one sample period. Only checked while DWT cannot wrap.
  s_lost = -1;
  if ((uint32_t)(millis() - s_start_ms) < 15000u && s_ticks) {
    uint64_t events = (uint32_t)(end - s_start_cycles) / s_ticks;
    uint64_t done = transfers();
    s_lost = events > done + 1u ? (int32_t)(events - done) : 0;  // +-1: time stamp vs. timer start
  }
  DMA2_Stream1->CR &= ~DMA_SxCR_EN;  // flushes the FIFO
  while (DMA2_Stream1->CR & DMA_SxCR_EN) {
  }
  NVIC_DisableIRQ(DMA2_Stream1_IRQn);
}

void capture_hw_start(capture_mem_t mem, const capture_timing_t *t) {
  s_last.valid = false;
  hw_start(mem, t);
}
void capture_hw_stop(void) { hw_stop(); }
uint32_t capture_hw_chunks(void) { return s_chunks; }
uint32_t capture_hw_errors(void) { return s_dma_errors; }
int32_t capture_hw_lost(void) { return s_lost; }

// Samples guaranteed to be in RAM since hw_start (never wraps).
//
// Normally the position comes from the time since the last chunk completed
// (DWT, on the private bus): reading NDTR is an AHB1 access, and polling it
// every few microseconds measurably costs samples at 20 MHz. Chunk ends are
// counted by the transfer-complete interrupt, which also re-anchors the time
// base to the real DMA progress. Should the DMA ever lose requests, the time
// estimate runs ahead by that many samples until the next chunk end; such a
// capture is reported as overrun anyway (see hw_stop()).
#define TIMED_SLACK 32u  // FIFO contents + DMA latency, in samples

uint64_t capture_written(void) {
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  uint32_t c = s_chunks;
  uint64_t w;
  if (s_timed) {
    uint32_t ref = c ? s_tc_cycles : s_start_cycles;
    uint32_t since = (uint32_t)(cycles() - ref) / s_ticks;
    __set_PRIMASK(primask);
    if (since >= CAPTURE_CHUNK) since = CAPTURE_CHUNK - 1u;  // chunk end not yet counted
    w = (uint64_t)c * CAPTURE_CHUNK + since;
    w = w > TIMED_SLACK ? w - TIMED_SLACK : 0;
  } else {
    // Slow rates: read the DMA position (consistent snapshot with IRQs masked)
    uint32_t pos = CAPTURE_CHUNK - DMA2_Stream1->NDTR;
    bool pending = (DMA2->LISR & DMA_LISR_TCIF1) != 0;
    __set_PRIMASK(primask);
    if (pos >= CAPTURE_CHUNK) pos = 0;
    // Chunk finished but the IRQ has not run yet. (A large pos means it
    // finished after NDTR was read, so it is not complete in our snapshot.)
    if (pending && pos < CAPTURE_CHUNK / 2u) c++;
    w = (uint64_t)c * CAPTURE_CHUNK + pos;
    w = w > FIFO_SLACK ? w - FIFO_SLACK : 0;
  }
  __asm volatile("" ::: "memory");  // buffer changed behind our back
  return w;
}

void capture_stream_begin(const capture_timing_t *t) {
  s_last.valid = false;  // the ring is about to be overwritten
  hw_start(CAPTURE_MEM_SDRAM, t);
}
void capture_stream_end(void) { hw_stop(); }

// At high rates the encoder reads only chunks the DMA has finished: the chunk
// being written is in the same SDRAM bank, where CPU reads are measured 1.7x
// slower (24 instead of 14.5 cycles per word), too slow to keep up with
// 20 MHz. A chunk is 1.6 ms at 20 MHz; at low rates, where it would delay the
// data for seconds and speed does not matter, read right behind the DMA.
#define STREAM_CHUNKED_TICKS 24u  // 5 MHz and faster

uint64_t capture_stream_readable(void) {
  uint64_t w = capture_written();
  if (s_ring.mem == CAPTURE_MEM_SDRAM && s_ticks <= STREAM_CHUNKED_TICKS) w -= w % CAPTURE_CHUNK;
  return w;
}

// Pause between looks at the DMA position (DWT-timed: no bus traffic).
static void short_pause(void) { delay_us(4); }

// ---------------------------------------------------------------------------
// Trigger scanning (same algorithm as the F103 version)
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

// Wait until `end` samples are in RAM.
static bool wait_written(uint64_t end, capture_poll_fn poll, uint32_t *last_ms) {
  while (capture_written() < end) {
    if (poll_due(poll, last_ms)) return false;
    short_pause();
  }
  return true;
}

capture_result_t capture_run(const capture_cfg_t *cfg, capture_poll_fn poll, uint32_t *start) {
  capture_timing_t t = capture_timing(cfg->divider);
  uint32_t total = cfg->total > CAPTURE_MAX_SAMPLES ? CAPTURE_MAX_SAMPLES : cfg->total;
  uint32_t pre = cfg->pre > total ? total : cfg->pre;
  uint32_t last_ms = millis();

  // Short captures stay on chip; long ones go to the SDRAM.
  bool sram = total <= CAPTURE_SRAM_RING - CAPTURE_SRAM_MARGIN;
  uint32_t margin = sram ? CAPTURE_SRAM_MARGIN : CAPTURE_SDRAM_MARGIN;

  *start = 0;
  s_last.valid = false;
  hw_start(sram ? CAPTURE_MEM_SRAM : CAPTURE_MEM_SDRAM, &t);
  const uint32_t size = s_ring.size;

  bool overrun = false;
  uint64_t first = 0;
  int32_t trigger_index = -1;

  if (cfg->num_stages) {
    trig_state_t st = {cfg, 0, false};
    // Start one sample early so that sample `pre` itself can be the edge.
    uint64_t scan_at = pre ? pre - 1u : 0u;
    uint64_t trig = 0;
    bool found = false;

    while (!found) {
      uint64_t w = capture_written();
      if (w > scan_at && w - scan_at > size - margin) {
        overrun = true;  // samples were overwritten before we could look at them
        scan_at = w - (size - margin) / 2u;
        st.level = 0;
        st.armed = false;
      }
      if (w <= scan_at) {  // caught up with the DMA
        if (poll_due(poll, &last_ms)) {
          hw_stop();
          return CAPTURE_ABORTED;
        }
        short_pause();
        continue;
      }
      // One contiguous piece (at most a chunk) per pass, so USB keeps being serviced
      uint32_t seg;
      const uint8_t *p = capture_span(scan_at, &seg);
      if (seg > w - scan_at) seg = (uint32_t)(w - scan_at);
      int32_t r = scan(p, seg, &st);
      if (r >= 0) {
        trig = scan_at + (uint32_t)r;
        found = true;
      } else {
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
    first = trig - pre + shift;
    trigger_index = (int32_t)(pre - shift);
  }

  if (!wait_written(first + total, poll, &last_ms)) {
    hw_stop();
    return CAPTURE_ABORTED;
  }
  hw_stop();
  // Everything after `first` must still be in the ring (the DMA may have run
  // up to FIFO_SLACK samples beyond capture_written()).
  if (capture_written() + FIFO_SLACK - first > size) overrun = true;
  if (s_lost > 0 || (s_dma_errors & (DMA_LISR_TEIF1 | DMA_LISR_DMEIF1))) overrun = true;

  *start = (uint32_t)(first & s_ring.mask);
  s_last = (capture_window_t){true, s_ring.mem, *start, total, trigger_index, t.rate_hz, s_lost};
  return overrun ? CAPTURE_OVERRUN : CAPTURE_OK;
}

// ---------------------------------------------------------------------------
// Preview support
// ---------------------------------------------------------------------------

void capture_or_and(uint32_t index, uint32_t n, uint8_t *or_out, uint8_t *and_out) {
  uint32_t o = 0, a = 0xFFFFFFFFu;
  while (n) {
    uint32_t seg;
    const uint8_t *p = capture_span(index, &seg);
    if (seg > n) seg = n;
    uint32_t i = 0;
    for (; i < seg && ((uintptr_t)(p + i) & 3u); i++) {
      o |= p[i];
      a &= p[i] | 0xFFFFFF00u;
    }
    for (; i + 4u <= seg; i += 4u) {
      uint32_t w = *(const u32_alias *)(p + i);
      o |= w;
      a &= w;
    }
    for (; i < seg; i++) {
      o |= p[i];
      a &= p[i] | 0xFFFFFF00u;
    }
    index += seg;
    n -= seg;
  }
  *or_out = (uint8_t)(o | o >> 8 | o >> 16 | o >> 24);
  *and_out = (uint8_t)(a & a >> 8 & a >> 16 & a >> 24);
}
