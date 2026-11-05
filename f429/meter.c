#include "meter.h"

#include <string.h>

#include "capture_hw.h"
#include "clock.h"
#include "la_config.h"

#define METER_TICKS  (CAPTURE_TIMER_HZ / METER_RATE_HZ)
#define GATE_CHUNKS  153u  // 153 * 32768 samples = 0.501 s at 10 MHz
#define CHUNK_CYCLES (CAPTURE_CHUNK * METER_TICKS)  // CPU cycles per chunk (HCLK = timer clock)
#define QUIET_PIECE  512u  // samples fed between two looks at the clock
#define BUSY_PIECE   256u
#define QUIET_PCT    30u   // CPU share the full pass over the quiet channels may take
#define BUDGET_PCT   45u   // CPU share of both passes together
#define DEMOTE_EDGES 1024u // a busy channel with fewer edges per chunk goes back to the full pass

// The statistics cost tens of cycles per edge, so a 1 MHz signal alone would
// need the whole CPU. The channels are therefore split in two groups:
// - quiet channels: every sample is looked at, with the busy channels masked
//   out (which keeps the scan cheap); their segments run on across chunks, so
//   slow signals are measured exactly next to fast ones;
// - busy channels, promoted when the quiet pass overruns its budget: only the
//   start of every chunk is measured, as far as the rest of the budget goes.
// Frequencies come from the rising edges inside contiguous segments
// (reciprocal counting), summed up over a gate.
typedef struct {
  uint64_t n;        // samples in the segments
  uint64_t nch[8];   // of those, samples in which the channel was measured here
  uint64_t high[8], span[8];
  uint32_t intervals[8], rises[8], falls[8], min_pulse[8];
  uint8_t level;  // levels after the last measured sample
} agg_t;

typedef struct {
  ana_stats_t seg, ckpt;  // the open segment, and as it was before this chunk
  bool open, ckpt_open;
  uint8_t mask;  // channels of the open segment
  agg_t agg;
} pass_t;

static pass_t s_quiet, s_busy;
static uint8_t s_busy_mask;
static uint32_t s_gate_chunks, s_gates, s_piece_cycles;
static uint64_t s_gate_cycles;  // spent measuring in this gate

// Diagnostics (read with a debugger): per gate
struct {
  uint32_t quiet_max, quiet_overruns, rollbacks, lost, chunks;
  uint32_t quiet_cycles, quiet_samples, busy_cycles, busy_samples;
} g_meter_dbg, g_meter_dbg_last;

static meter_result_t s_pub;
static volatile uint32_t s_seq;  // sequence lock: odd = being written

static void pass_reset(pass_t *ps) {
  ps->open = false;
  memset(&ps->agg, 0, sizeof ps->agg);
  for (unsigned ch = 0; ch < 8; ch++) ps->agg.min_pulse[ch] = ANA_NONE;
}

static void pass_feed(pass_t *ps, const uint8_t *p, uint32_t n, uint8_t mask) {
  if (!ps->open) {
    ana_stats_reset(&ps->seg, p[0] & mask);
    ps->open = true;
    ps->mask = mask;  // changes of the masks end both segments first
  }
  ana_stats_feed_masked(&ps->seg, p, n, mask);
}

static void pass_checkpoint(pass_t *ps) {
  ps->ckpt_open = ps->open;
  if (ps->open) ps->ckpt = ps->seg;
}

static void pass_rollback(pass_t *ps) {
  ps->open = ps->ckpt_open;
  if (ps->open) ps->seg = ps->ckpt;
}

// Ends the open segment and adds it to the gate sums.
static void pass_end(pass_t *ps) {
  if (!ps->open) return;
  ps->open = false;
  ana_stats_t *s = &ps->seg;
  agg_t *a = &ps->agg;
  ana_stats_finish(s);
  a->n += s->n;
  a->level = s->prev;
  for (unsigned ch = 0; ch < 8; ch++) {
    if (!((ps->mask >> ch) & 1u)) continue;
    const ana_chan_t *c = &s->ch[ch];
    a->nch[ch] += s->n;
    if (c->rises >= 2) {
      a->intervals[ch] += c->rises - 1u;
      a->span[ch] += c->last_rise - c->first_rise;
    }
    a->rises[ch] += c->rises;
    a->falls[ch] += c->falls;
    a->high[ch] += c->high;
    uint32_t mp = c->min_high < c->min_low ? c->min_high : c->min_low;
    if (mp < a->min_pulse[ch]) a->min_pulse[ch] = mp;
  }
}

// The quiet pass overran: the channel with the most edges in this chunk
// becomes busy, and so do those with nearly as many.
static void promote(uint8_t quiet) {
  const pass_t *ps = &s_quiet;
  uint32_t e[8], best = 0;
  for (unsigned ch = 0; ch < 8; ch++) {
    e[ch] = 0;
    if (!((quiet >> ch) & 1u)) continue;
    e[ch] = ps->seg.ch[ch].rises + ps->seg.ch[ch].falls;
    if (ps->ckpt_open) e[ch] -= ps->ckpt.ch[ch].rises + ps->ckpt.ch[ch].falls;
    if (e[ch] > best) best = e[ch];
  }
  for (unsigned ch = 0; best && ch < 8; ch++)
    if (e[ch] >= best - best / 4u) s_busy_mask |= (uint8_t)(1u << ch);
}

// At the end of a gate: busy channels that calmed down go back.
static void demote(void) {
  const agg_t *a = &s_busy.agg;
  for (unsigned ch = 0; ch < 8; ch++) {
    if (!((s_busy_mask >> ch) & 1u) || !a->nch[ch]) continue;  // (not measured yet: keep)
    uint64_t per_chunk = (uint64_t)(a->rises[ch] + a->falls[ch]) * CAPTURE_CHUNK / a->nch[ch];
    if (per_chunk < DEMOTE_EDGES) s_busy_mask &= (uint8_t)~(1u << ch);
  }
}

static void measure(const uint8_t *p, uint32_t n) {
  const uint32_t quiet_budget = CHUNK_CYCLES / 100u * QUIET_PCT, budget = CHUNK_CYCLES / 100u * BUDGET_PCT;
  uint32_t t0 = cycles();
  uint8_t busy = s_busy_mask, quiet = (uint8_t)~busy;
  bool end_quiet = false, end_busy = false;
  pass_checkpoint(&s_quiet);
  pass_checkpoint(&s_busy);
  if (quiet) {
    uint32_t i = 0;
    for (; i < n; i += QUIET_PIECE) {
      if (i && cycles() - t0 > quiet_budget) {
        promote(quiet);  // takes effect with the next chunk
        end_quiet = end_busy = true;
        g_meter_dbg.quiet_overruns++;
        break;
      }
      pass_feed(&s_quiet, p + i, n - i < QUIET_PIECE ? n - i : QUIET_PIECE, quiet);
    }
    uint32_t dt = cycles() - t0;
    if (dt > g_meter_dbg.quiet_max) g_meter_dbg.quiet_max = dt;
    g_meter_dbg.quiet_cycles += dt;
    g_meter_dbg.quiet_samples += i;
  }
  uint32_t tb = cycles();
  if (busy) {
    uint32_t i = 0;
    while (i < n) {
      uint32_t t = cycles();
      if (i && t - t0 + s_piece_cycles > budget) break;  // the next piece would not fit
      uint32_t k = n - i < BUSY_PIECE ? n - i : BUSY_PIECE;
      pass_feed(&s_busy, p + i, k, busy);
      i += k;
      s_piece_cycles = cycles() - t;
    }
    if (i < n) end_busy = true;  // the rest of the chunk is a gap
    g_meter_dbg.busy_cycles += cycles() - tb;
    g_meter_dbg.busy_samples += i;
  }
  if (!capture_meter_intact()) {  // overwritten while being measured: leave the chunk out
    pass_rollback(&s_quiet);
    pass_rollback(&s_busy);
    end_quiet = end_busy = true;
    g_meter_dbg.rollbacks++;
  }
  if (end_quiet) pass_end(&s_quiet);
  if (end_busy) pass_end(&s_busy);
  s_gate_cycles += cycles() - t0;
}

// The sums expressed as ana_stats_t, so the UI can use the analyze helpers.
// A channel promoted during the gate was measured by both passes; both
// measurements are valid and add up.
static void publish(void) {
  const agg_t *q = &s_quiet.agg, *b = &s_busy.agg;
  uint64_t total = (uint64_t)s_gate_chunks * CAPTURE_CHUNK;
  ana_stats_t st;
  ana_stats_reset(&st, q->level);
  st.n = total;
  for (unsigned ch = 0; ch < 8; ch++) {
    ana_chan_t *c = &st.ch[ch];
    uint64_t nch = q->nch[ch] + b->nch[ch];
    c->high = nch ? (q->high[ch] + b->high[ch]) * total / nch : 0;  // duty over the samples seen
    c->falls = q->falls[ch] + b->falls[ch];
    uint32_t intervals = q->intervals[ch] + b->intervals[ch];
    uint64_t span = q->span[ch] + b->span[ch];
    if (intervals && span) {  // (rises - 1) / (last - first) = intervals / span
      c->rises = intervals + 1u;
      c->first_rise = 0;
      c->last_rise = span;
    } else {
      c->rises = q->rises[ch] + b->rises[ch];
    }
    c->min_high = c->min_low = q->min_pulse[ch] < b->min_pulse[ch] ? q->min_pulse[ch] : b->min_pulse[ch];
  }
  s_seq++;
  __asm volatile("" ::: "memory");
  s_pub.st = st;
  s_pub.rate_hz = METER_RATE_HZ;
  s_pub.gates = ++s_gates;
  s_pub.coverage = (unsigned)(q->n * 1000u / total);
  s_pub.sampled = s_busy_mask;
  s_pub.sampled_coverage = (unsigned)(b->n * 1000u / total);
  s_pub.load = (unsigned)(s_gate_cycles * 1000u / (total * METER_TICKS));
  __asm volatile("" ::: "memory");
  s_seq++;
}

static void gate_end(void) {
  pass_end(&s_quiet);
  pass_end(&s_busy);
  if (s_quiet.agg.n || s_busy.agg.n) publish();
  demote();
  pass_reset(&s_quiet);
  pass_reset(&s_busy);
  s_gate_chunks = 0;
  s_gate_cycles = 0;
  g_meter_dbg_last = g_meter_dbg;
  memset(&g_meter_dbg, 0, sizeof g_meter_dbg);
}

// Called by the capture driver for every completed chunk, from the
// lowest-priority interrupt; p = 0 means n chunks were lost.
static void on_chunk(const uint8_t *p, uint32_t n) {
  if (!p) {  // the segments end before the gap
    pass_end(&s_quiet);
    pass_end(&s_busy);
    s_gate_chunks += n;
    g_meter_dbg.lost += n;
  } else {
    measure(p, n);
    s_gate_chunks++;
    g_meter_dbg.chunks++;
  }
  if (s_gate_chunks >= GATE_CHUNKS) gate_end();
}

void meter_start(void) {
  pass_reset(&s_quiet);
  pass_reset(&s_busy);
  s_busy_mask = 0;
  s_gate_chunks = s_gates = s_piece_cycles = 0;
  s_gate_cycles = 0;
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
