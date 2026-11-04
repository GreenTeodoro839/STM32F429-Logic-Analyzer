#include "meter.h"

#include <string.h>

#include "capture_hw.h"
#include "clock.h"
#include "la_config.h"

#define METER_TICKS  (CAPTURE_TIMER_HZ / METER_RATE_HZ)
#define GATE_CHUNKS  153u  // 153 * 32768 samples = 0.501 s at 10 MHz
#define CHUNK_CYCLES (CAPTURE_CHUNK * METER_TICKS)  // CPU cycles per chunk (HCLK = timer clock)
#define BUDGET_PCT   35u   // at most this share of the CPU for the meter

// The statistics cost about 60 cycles per edge, so a 1 MHz signal alone would
// need the whole CPU. Chunks are therefore processed within a CPU budget:
// after an expensive chunk the following ones are skipped. Contiguous chunks
// form a segment; frequencies come from the rising edges inside segments
// (reciprocal counting) and all segments of a gate are summed up.
typedef struct {
  uint64_t n, high[8], span[8];
  uint32_t intervals[8], rises[8], falls[8], min_pulse[8];
  uint8_t level;  // levels after the last measured sample
} agg_t;

static ana_stats_t s_seg;   // current segment
static ana_stats_t s_ckpt;  // the segment before its last chunk
static bool s_seg_open, s_ckpt_empty;
static agg_t s_agg;
static uint32_t s_gate_chunks, s_skip, s_gates;

static meter_result_t s_pub;
static volatile uint32_t s_seq;  // sequence lock: odd = being written

static void agg_reset(void) {
  memset(&s_agg, 0, sizeof s_agg);
  for (unsigned ch = 0; ch < 8; ch++) s_agg.min_pulse[ch] = ANA_NONE;
}

static void segment_end(bool keep) {
  if (!s_seg_open) return;
  s_seg_open = false;
  if (!keep) return;
  ana_stats_finish(&s_seg);
  s_agg.n += s_seg.n;
  s_agg.level = s_seg.prev;
  for (unsigned ch = 0; ch < 8; ch++) {
    const ana_chan_t *c = &s_seg.ch[ch];
    if (c->rises >= 2) {
      s_agg.intervals[ch] += c->rises - 1u;
      s_agg.span[ch] += c->last_rise - c->first_rise;
    }
    s_agg.rises[ch] += c->rises;
    s_agg.falls[ch] += c->falls;
    s_agg.high[ch] += c->high;
    uint32_t mp = c->min_high < c->min_low ? c->min_high : c->min_low;
    if (mp < s_agg.min_pulse[ch]) s_agg.min_pulse[ch] = mp;
  }
}

// The sums expressed as ana_stats_t, so the UI can use the analyze helpers.
static void publish(void) {
  ana_stats_t st;
  ana_stats_reset(&st, s_agg.level);
  st.n = s_agg.n;
  for (unsigned ch = 0; ch < 8; ch++) {
    ana_chan_t *c = &st.ch[ch];
    c->high = s_agg.high[ch];
    c->falls = s_agg.falls[ch];
    if (s_agg.intervals[ch] && s_agg.span[ch]) {  // (rises - 1) / (last - first) = intervals / span
      c->rises = s_agg.intervals[ch] + 1u;
      c->first_rise = 0;
      c->last_rise = s_agg.span[ch];
    } else {
      c->rises = s_agg.rises[ch];
    }
    c->min_high = c->min_low = s_agg.min_pulse[ch];
  }
  s_seq++;
  __asm volatile("" ::: "memory");
  s_pub.st = st;
  s_pub.rate_hz = METER_RATE_HZ;
  s_pub.gates = ++s_gates;
  s_pub.coverage = (unsigned)(s_agg.n * 1000u / ((uint64_t)GATE_CHUNKS * CAPTURE_CHUNK));
  __asm volatile("" ::: "memory");
  s_seq++;
}

// Called by the capture driver for every completed chunk, from the
// lowest-priority interrupt; p = 0 means chunks were lost.
static void on_chunk(const uint8_t *p, uint32_t n) {
  if (!p) {
    // Chunks were lost; the last one fed may have been overwritten while it
    // was processed, so the segment ends without it.
    if (s_seg_open) {
      if (s_ckpt_empty) s_seg_open = false;
      else s_seg = s_ckpt;
    }
    segment_end(true);
  } else if (s_skip) {
    s_skip--;
    segment_end(true);
  } else {
    uint32_t t0 = cycles();
    s_ckpt_empty = !s_seg_open;
    if (!s_seg_open) {
      ana_stats_reset(&s_seg, p[0]);
      s_seg_open = true;
    } else {
      s_ckpt = s_seg;
    }
    ana_stats_feed(&s_seg, p, n);
    uint32_t dt = cycles() - t0;
    uint32_t budget = CHUNK_CYCLES / 100u * BUDGET_PCT;
    if (dt > budget) {
      s_skip = dt / budget;  // keeps the average load near the budget
      segment_end(true);
    }
  }
  if (++s_gate_chunks >= GATE_CHUNKS) {
    segment_end(true);
    if (s_agg.n) publish();
    agg_reset();
    s_gate_chunks = 0;
  }
}

void meter_start(void) {
  s_seg_open = false;
  s_skip = s_gate_chunks = s_gates = 0;
  agg_reset();
  capture_meter_start(METER_TICKS, on_chunk);
}

void meter_stop(void) { capture_meter_stop(); }
bool meter_running(void) { return capture_meter_running(); }

bool meter_get(meter_result_t *out, uint32_t *seq) {
  for (;;) {
    uint32_t s1 = s_seq;
    if (s1 & 1u) continue;  // cannot happen: the writer is an interrupt
    if (s1 == *seq) return false;
    memcpy(out, &s_pub, sizeof *out);
    __asm volatile("" ::: "memory");
    if (s_seq == s1) {
      *seq = s1;
      return true;
    }
  }
}

#ifdef METER_TEST
void meter_test_chunk(const uint8_t *p, uint32_t n) { on_chunk(p, n); }
#endif
