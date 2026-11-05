#include "analyze.h"

#include <string.h>

typedef uint32_t __attribute__((may_alias)) u32_alias;

// The ring is contiguous within aligned blocks of this many samples (ring
// sizes and SDRAM chunks are multiples of it).
#define BLK 1024u

// Pointer to window sample i and the number of samples contiguous from it
// up to the end of its block.
static const uint8_t *span(const capture_window_t *w, uint32_t i, uint32_t *n) {
  uint32_t r = w->start + i, c;
  const uint8_t *p = capture_span(r, &c);
  uint32_t b = BLK - (r % BLK);
  *n = c < b ? c : b;
  return p;
}

uint8_t ana_sample(const capture_window_t *w, uint32_t i) { return capture_sample(w->start + i); }

// First k in p[0..n) with (p[k] & mask) != ref, or -1.
static int32_t mismatch_fwd(const uint8_t *p, uint32_t n, uint8_t mask, uint8_t ref) {
  uint32_t i = 0;
  for (; i < n && ((uintptr_t)(p + i) & 3u); i++)
    if ((p[i] & mask) != ref) return (int32_t)i;
  const uint32_t m4 = mask * 0x01010101u, r4 = ref * 0x01010101u;
  for (; i + 4u <= n; i += 4u) {
    uint32_t x = (*(const u32_alias *)(p + i) ^ r4) & m4;
    if (x) return (int32_t)(i + ((uint32_t)__builtin_ctz(x) >> 3));
  }
  for (; i < n; i++)
    if ((p[i] & mask) != ref) return (int32_t)i;
  return -1;
}

// Last k in p[0..n) with (p[k] & mask) != ref, or -1.
static int32_t mismatch_bwd(const uint8_t *p, uint32_t n, uint8_t mask, uint8_t ref) {
  uint32_t i = n;
  for (; i > 0 && ((uintptr_t)(p + i) & 3u); i--)
    if ((p[i - 1] & mask) != ref) return (int32_t)(i - 1);
  const uint32_t m4 = mask * 0x01010101u, r4 = ref * 0x01010101u;
  for (; i >= 4u; i -= 4u) {
    uint32_t x = (*(const u32_alias *)(p + i - 4u) ^ r4) & m4;
    if (x) return (int32_t)(i - 4u + (uint32_t)((31 - __builtin_clz(x)) >> 3));
  }
  for (; i > 0; i--)
    if ((p[i - 1] & mask) != ref) return (int32_t)(i - 1);
  return -1;
}

uint32_t ana_next_change(const capture_window_t *w, uint32_t i, uint32_t end, uint8_t mask, uint8_t ref) {
  ref &= mask;
  while (i < end) {
    uint32_t n;
    const uint8_t *p = span(w, i, &n);
    if (n > end - i) n = end - i;
    int32_t k = mismatch_fwd(p, n, mask, ref);
    if (k >= 0) return i + (uint32_t)k;
    i += n;
  }
  return end;
}

uint32_t ana_prev_change(const capture_window_t *w, uint32_t begin, uint32_t i, uint8_t mask, uint8_t ref) {
  ref &= mask;
  while (i > begin) {
    uint32_t len = (w->start + i - 1u) % BLK + 1u;  // [i - len, i) lies in one block
    if (len > i - begin) len = i - begin;
    uint32_t n;
    const uint8_t *p = span(w, i - len, &n);
    int32_t k = mismatch_bwd(p, len, mask, ref);
    if (k >= 0) return i - len + (uint32_t)k;
    i -= len;
  }
  return ANA_NONE;
}

uint32_t ana_next_edge(const capture_window_t *w, uint32_t i, unsigned ch) {
  if (i + 1u >= w->total) return ANA_NONE;
  uint8_t mask = (uint8_t)(1u << ch);
  uint32_t j = ana_next_change(w, i + 1u, w->total, mask, ana_sample(w, i));
  return j < w->total ? j : ANA_NONE;
}

uint32_t ana_prev_edge(const capture_window_t *w, uint32_t i, unsigned ch) {
  if (i == 0 || i > w->total) return ANA_NONE;
  uint8_t mask = (uint8_t)(1u << ch);
  uint32_t j = ana_prev_change(w, 0, i - 1u, mask, ana_sample(w, i - 1u));
  return j == ANA_NONE ? ANA_NONE : j + 1u;
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

void ana_stats_reset(ana_stats_t *s, uint8_t first_sample) {
  memset(s, 0, sizeof *s);
  s->first = s->prev = first_sample;
  for (unsigned ch = 0; ch < 8; ch++) s->ch[ch].min_high = s->ch[ch].min_low = ANA_NONE;
}

// Kept out of line: the sample scan around it then has its registers to itself.
__attribute__((noinline)) static void edges(ana_stats_t *s, uint64_t pos, uint8_t old, uint8_t now) {
  uint8_t d = old ^ now;
  while (d) {
    unsigned ch = (unsigned)__builtin_ctz(d);
    d &= (uint8_t)(d - 1u);
    ana_chan_t *c = &s->ch[ch];
    uint64_t dur = pos - s->since[ch];
    bool was_high = (old >> ch) & 1u;
    if (was_high) {
      c->high += dur;
      c->falls++;
    } else {
      if (!c->rises) c->first_rise = pos;
      c->last_rise = pos;
      c->rises++;
    }
    if (s->seen & (1u << ch)) {  // a complete pulse between two edges
      uint32_t d32 = dur > 0xFFFFFFFEull ? 0xFFFFFFFEu : (uint32_t)dur;
      if (was_high) {
        if (d32 < c->min_high) c->min_high = d32;
        if (d32 > c->max_high) c->max_high = d32;
      } else {
        if (d32 < c->min_low) c->min_low = d32;
        if (d32 > c->max_low) c->max_low = d32;
      }
    }
    s->seen |= (uint8_t)(1u << ch);
    s->since[ch] = pos;
  }
}

void ana_stats_feed_masked(ana_stats_t *s, const uint8_t *p, uint32_t n, uint8_t mask) {
  const uint64_t base = s->n;
  const uint32_t mask4 = mask * 0x01010101u;
  uint8_t prev = s->prev;
  uint32_t i = 0;
  for (;;) {
    // Find the next sample that differs in the masked channels: bytewise up
    // to a word boundary, then 32 samples per step.
    const uint32_t prev4 = prev * 0x01010101u;
    while (i < n && (p[i] & mask) == prev) {
      if (!((uintptr_t)(p + ++i) & 3u)) {
        while (i + 32u <= n) {
          const u32_alias *w = (const u32_alias *)(p + i);
          uint32_t x = (w[0] ^ prev4) | (w[1] ^ prev4) | (w[2] ^ prev4) | (w[3] ^ prev4);
          x |= (w[4] ^ prev4) | (w[5] ^ prev4) | (w[6] ^ prev4) | (w[7] ^ prev4);
          if (x & mask4) break;
          i += 32u;
        }
        while (i + 4u <= n && !((*(const u32_alias *)(p + i) ^ prev4) & mask4)) i += 4u;
      }
    }
    if (i >= n) break;
    uint8_t v = p[i] & mask;
    edges(s, base + i, prev, v);
    prev = v;
    i++;
  }
  s->n = base + n;
  s->prev = prev;
}

void ana_stats_feed(ana_stats_t *s, const uint8_t *p, uint32_t n) { ana_stats_feed_masked(s, p, n, 0xFFu); }

void ana_stats_finish(ana_stats_t *s) {
  for (unsigned ch = 0; ch < 8; ch++)
    if ((s->prev >> ch) & 1u) s->ch[ch].high += s->n - s->since[ch];
  for (unsigned ch = 0; ch < 8; ch++) s->since[ch] = s->n;  // finishing twice adds nothing
}

uint64_t ana_freq_mhz(const ana_stats_t *s, unsigned ch, uint32_t rate_hz) {
  const ana_chan_t *c = &s->ch[ch];
  if (c->rises < 2 || c->last_rise <= c->first_rise) return 0;
  return ((uint64_t)(c->rises - 1u) * rate_hz * 1000u + (c->last_rise - c->first_rise) / 2u) /
         (c->last_rise - c->first_rise);
}

unsigned ana_duty_permille(const ana_stats_t *s, unsigned ch) {
  if (!s->n) return 0;
  return (unsigned)((s->ch[ch].high * 1000u + s->n / 2u) / s->n);
}

uint32_t ana_min_pulse(const ana_stats_t *s, unsigned ch) {
  const ana_chan_t *c = &s->ch[ch];
  return c->min_high < c->min_low ? c->min_high : c->min_low;
}

void ana_job_start(ana_job_t *j, const capture_window_t *w, uint32_t from, uint32_t count) {
  j->win = *w;
  if (from > w->total) from = w->total;
  if (count > w->total - from) count = w->total - from;
  j->pos = from;
  j->end = from + count;
  ana_stats_reset(&j->st, count ? ana_sample(w, from) : 0);
  j->active = count > 0;
  j->done = count == 0;
}

bool ana_job_run(ana_job_t *j, uint32_t max_samples) {
  if (!j->active) return j->done;
  while (j->pos < j->end && max_samples) {
    uint32_t n;
    const uint8_t *p = span(&j->win, j->pos, &n);
    if (n > j->end - j->pos) n = j->end - j->pos;
    if (n > max_samples) n = max_samples;
    ana_stats_feed(&j->st, p, n);
    j->pos += n;
    max_samples -= n;
  }
  if (j->pos >= j->end) {
    ana_stats_finish(&j->st);
    j->active = false;
    j->done = true;
  }
  return j->done;
}
