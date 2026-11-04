#include "stream.h"

#include <stdlib.h>
#include <string.h>

#include "capture.h"
#include "clock.h"
#include "la_status.h"

// "SRPICO,A<analog count:2><bytes per analog sample:1>D<digital count:2>,<version:02>"
#define ID_STRING "SRPICO,A001D08,02"

#define FLUSH_MS 20u        // push pending run-lengths at least this often
#define STREAM_PIECE 512u   // samples encoded between overflow checks

typedef enum { START_NONE, START_FIXED, START_CONTINUOUS } start_t;

static const stream_io_t *s_io;
static char s_line[24];
static uint8_t s_len;
static uint8_t s_chmask = 0xFF;  // enabled digital channels
static uint32_t s_limit = 1000;
static uint32_t s_rate = 5000;
static start_t s_start;

void stream_init(const stream_io_t *io) { s_io = io; }
bool stream_pending(void) { return s_start != START_NONE; }

static void reply(const char *s) {
  s_io->write((const uint8_t *)s, (uint32_t)strlen(s));
  s_io->flush();
}

static bool rate_ok(uint32_t rate) {
  if (rate == 0 || rate > CAPTURE_MAX_RATE_HZ) return false;
  return capture_timing_hz(rate).exact;
}

static void exec_line(void) {
  s_line[s_len] = 0;
  switch (s_line[0]) {
    case 'i':  // identify
      reply(ID_STRING);
      break;
    case 'a':  // analog scale/offset query (no analog channels): 1.0 x 0
      reply("1000000x0");
      break;
    case 'D': {  // D<enable><index>
      unsigned idx = (unsigned)atoi(&s_line[2]);
      if (idx < LA_NUM_CH) {
        if (s_line[1] == '1') s_chmask |= (uint8_t)(1u << idx);
        else s_chmask &= (uint8_t)~(1u << idx);
      }
      reply("*");
      break;
    }
    case 'L':
      s_limit = (uint32_t)strtoul(&s_line[1], NULL, 10);
      reply("*");
      break;
    case 'R': {
      uint32_t rate = (uint32_t)strtoul(&s_line[1], NULL, 10);
      if (rate_ok(rate)) {
        s_rate = rate;
        reply("*");
      } else {
        reply(LA_RATE_ERROR);  // anything but '*' refuses the start
      }
      break;
    }
    case 'A':  // analog enable, trigger and pre-trigger hints: accepted, the
    case 't':  // host does the (software) triggering itself
    case 'p':
      reply("*");
      break;
    case 'F':
      s_start = START_FIXED;
      break;
    case 'C':
      s_start = START_CONTINUOUS;
      break;
    default:
      break;
  }
  s_len = 0;
}

void stream_rx(uint8_t b) {
  switch (b) {
    case '*':  // reset
      s_len = 0;
      s_start = START_NONE;
      return;
    case '+':  // stop request while not streaming: nothing to stop
    case '\r':
      return;
    case '\n':
      if (s_len) exec_line();
      return;
    default:
      if (s_len < sizeof s_line - 1u) s_line[s_len++] = (char)b;
      return;
  }
}

// ---------------------------------------------------------------------------
// Encoder
//
// Slice mode (more than 4 channels): a sample is one byte 0x80|ch0..6, plus
// 0x80|ch7 if channel 7 is enabled. Bytes 48..79 repeat the previous sample
// 1..32 times, bytes 80..127 repeat it (b-78)*32 = 64..1568 times.
// D4 mode (channels 0..3 only): a sample is 0x80|(r<<4)|ch0..3, where r = 0..7
// repeats of the previous sample come first; bytes 48..127 add (b-47)*8 repeats.
// The stream ends with '$', the decimal byte count and '+'. '!' = overflow.
// ---------------------------------------------------------------------------

static struct {
  uint8_t out[128];
  uint32_t n;      // bytes waiting in out[]
  uint32_t bytes;  // data bytes sent so far (reported at the end)
  int16_t prev;    // previous sample, -1 = none yet
  uint32_t rle;    // pending repeats of prev
  bool d4, two;
} e;

#define OUT_RESERVE 8u  // room kept free in out[] for one sample's bytes

static void out_flush(void) {
  if (e.n) {
    s_io->write(e.out, e.n);
    e.bytes += e.n;
  }
  e.n = 0;
}

// Encode r repeats of the previous sample as run-length bytes. In D4 mode a
// remainder < 8 cannot be expressed this way and is returned.
static uint32_t put_runs(uint32_t r) {
  if (e.d4) {
    while (r >= 8u) {
      if (e.n > sizeof e.out - OUT_RESERVE) out_flush();
      uint32_t k = r >> 3;
      if (k > 80u) k = 80u;
      e.out[e.n++] = (uint8_t)(47u + k);
      r -= k << 3;
    }
    return r;
  }
  while (r) {
    if (e.n > sizeof e.out - OUT_RESERVE) out_flush();
    uint32_t k;
    if (r >= 64u) {
      k = r >> 5;
      if (k > 49u) k = 49u;
      e.out[e.n++] = (uint8_t)(78u + k);
      r -= k << 5;
    } else {
      k = r > 32u ? 32u : r;
      e.out[e.n++] = (uint8_t)(47u + k);
      r -= k;
    }
  }
  return 0;
}

// Emit all pending repeats of the previous sample (periodically and at the end).
static void flush_rle(void) {
  uint32_t r = put_runs(e.rle);
  e.rle = 0;
  if (r) {  // D4 remainder: r-1 inline repeats + one more copy of the sample
    if (e.n > sizeof e.out - OUT_RESERVE) out_flush();
    e.out[e.n++] = (uint8_t)(0x80u | ((r - 1u) << 4) | (uint8_t)e.prev);
  }
}

typedef uint32_t __attribute__((may_alias)) u32_alias;

// Encode the samples in [p, end) (contiguous in the ring buffer). Kept out of
// line so the hot loop gets the registers to itself.
static void __attribute__((noinline)) encode(const uint8_t *p, const uint8_t *end, uint8_t mask) {
  const uint32_t m4 = mask * 0x01010101u;
  const bool d4 = e.d4, two = e.two;
  int prev = e.prev;
  uint32_t prev4 = prev < 0 ? 0xFFFFFFFFu : (uint32_t)prev * 0x01010101u;
  uint32_t rle = e.rle;
  // Last word boundary in [p, end): the fast path reads aligned words only
  // (an unaligned load is split into several bus accesses, which is slow on
  // SDRAM).
  const u32_alias *const wend = (const u32_alias *)((uintptr_t)end & ~(uintptr_t)3u);

  while (p < end) {
    uint8_t v = *p++ & mask;
    if ((int)v == prev) {  // unchanged: count it, then skip whole words
      rle++;
      if (!((uintptr_t)p & 3u)) {
        // Two words per step. (Longer runs of loads were measured to starve
        // the sampling DMA when both use the SDRAM.)
        const u32_alias *q = (const u32_alias *)p;
        while (q + 2 <= wend && (q[0] & m4) == prev4 && (q[1] & m4) == prev4) q += 2;
        if (q < wend && (q[0] & m4) == prev4) q++;
        rle += (uint32_t)((const uint8_t *)q - p);
        p = (const uint8_t *)q;
      }
      continue;
    }

    // Changed: pending run, then the new sample.
    if (e.n > sizeof e.out - OUT_RESERVE) out_flush();
    uint32_t n = e.n;
    if (d4) {
      if (rle >= 8u) {
        if (rle < 648u) {  // one run byte (8..640) + inline remainder
          e.out[n++] = (uint8_t)(47u + (rle >> 3));
          rle &= 7u;
        } else {
          e.n = n;
          rle = put_runs(rle);
          n = e.n;
        }
      }
      e.out[n++] = (uint8_t)(0x80u | (rle << 4) | v);
    } else {
      if (rle) {
        if (rle <= 32u) {
          e.out[n++] = (uint8_t)(47u + rle);
        } else {
          e.n = n;
          put_runs(rle);
          n = e.n;
        }
      }
      e.out[n++] = (uint8_t)(0x80u | (v & 0x7Fu));
      if (two) e.out[n++] = (uint8_t)(0x80u | (v >> 7));
    }
    e.n = n;
    rle = 0;
    prev = v;
    prev4 = (uint32_t)v * 0x01010101u;
  }
  e.prev = (int16_t)prev;
  e.rle = rle;
}

static void append_u32(char *s, uint32_t v) {
  char tmp[11];
  int k = 0;
  do {
    tmp[k++] = (char)('0' + v % 10u);
    v /= 10u;
  } while (v);
  while (k) *s++ = tmp[--k];
  *s = 0;
}

void stream_run(void) {
  bool fixed = s_start == START_FIXED;
  s_start = START_NONE;

  uint8_t mask = s_chmask;
  memset(&e, 0, sizeof e);
  e.prev = -1;
  e.d4 = (mask & 0xF0u) == 0;
  e.two = (mask & 0x80u) != 0;

  capture_timing_t t = capture_timing_hz(s_rate);
  g_la.streaming = true;
  g_la.rate_hz = t.rate_hz;
  g_la.rate_exact = t.exact;
  g_la.samples = fixed ? s_limit : 0;
  g_la.num_stages = 0;
  g_la.stream_channels = (uint8_t)__builtin_popcount(mask);
  g_la.state = LA_STREAMING;
  s_io->on_state();

  capture_stream_begin(&t);
  uint64_t rd = 0;
  bool stop = false, abort = false, overflow = false;
  bool sampling = true;  // the DMA is still filling the ring
  uint32_t last_flush = millis();

  while (!stop) {
    uint64_t w = sampling ? capture_stream_readable() : s_limit;

    // Encode in pieces of the ring buffer. After each piece make sure the DMA
    // has not overwritten anything we were still about to read (encoding can
    // block on USB for a long time), and stop sampling as soon as a
    // fixed-length stream is complete.
    for (;;) {
      if (sampling) {
        uint64_t now = capture_written();
        if (now - rd > CAPTURE_RING_SIZE() - CAPTURE_GUARD - 64u) {  // the DMA lapped us: data lost
          overflow = true;
          break;
        }
        if (fixed && now >= s_limit) {
          // Everything the host asked for is in the ring: stop sampling, so
          // the DMA cannot overwrite what is still to be sent. With a large
          // ring a fixed-length stream can be captured faster than USB
          // carries it.
          capture_stream_end();
          sampling = false;
        }
      }
      if (fixed && w > s_limit) w = s_limit;
      if (rd >= w) break;
      uint32_t n;
      const uint8_t *p = capture_span(rd, &n);
      if (n > w - rd) n = (uint32_t)(w - rd);
      if (n > STREAM_PIECE) n = STREAM_PIECE;
      encode(p, p + n, mask);
      // Was the piece overwritten while we were encoding it?
      if (sampling && capture_written() - rd > CAPTURE_RING_SIZE() - CAPTURE_GUARD) {
        overflow = true;
        break;
      }
      rd += n;
    }
    if (overflow) break;
    if (fixed && rd >= s_limit) stop = true;

    uint32_t now = millis();
    if ((uint32_t)(now - last_flush) >= FLUSH_MS) {  // let the host see progress
      last_flush = now;
      flush_rle();
      out_flush();
      s_io->flush();
    }

    if (!s_io->service()) {
      abort = true;
      break;
    }
    for (int c; (c = s_io->read()) >= 0;) {
      if (c == '+') stop = true;  // host has enough samples
      else if (c == '*') {        // reset: drop everything
        abort = true;
        stop = true;
      }
    }
  }
  if (sampling) capture_stream_end();

  if (overflow) {
    out_flush();
    // libsigrok only recognises a terminator that is not the last byte of a
    // read; a lone '!' would be misparsed as a run length and hang the host.
    reply("!!!!!!!!");
  } else if (!abort) {
    flush_rle();
    out_flush();
    char end[16] = "$";
    append_u32(&end[1], e.bytes);
    strcat(end, "+");
    reply(end);
  }

  g_la.stream_samples = rd;
  g_la.state = overflow ? LA_STREAM_OVERFLOW : (abort ? LA_ABORTED : LA_STREAM_DONE);
  s_io->on_state();
}
