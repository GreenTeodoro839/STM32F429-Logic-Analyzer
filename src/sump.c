#include "sump.h"

#include <string.h>

#include "la_status.h"

// Short (1-byte) commands
#define CMD_RESET        0x00
#define CMD_ARM          0x01
#define CMD_ID           0x02
#define CMD_METADATA     0x04
#define CMD_FINISH_NOW   0x05
// Long (5-byte) commands: opcode with bit 7 set + 32-bit little-endian argument
#define CMD_SET_DIVIDER  0x80
#define CMD_CAPTURE_SIZE 0x81
#define CMD_SET_FLAGS    0x82
#define CMD_DELAYCOUNT   0x83
#define CMD_READCOUNT    0x84
#define CMD_TRIG_BASE    0xC0  // 0xC0 + 4*stage: mask, value, config

#define FLAG_RLE           (1u << 8)
#define FLAG_GROUPS_SHIFT  2  // bits 2..5: 1 = channel group disabled
#define TRIG_CFG_START     (1u << 27)

#define FW_NAME    "LA-F103"
#define FW_VERSION "0.1"

static const sump_io_t *s_io;

static struct {
  uint32_t divider;
  uint32_t read_count;   // samples
  uint32_t delay_count;  // samples after trigger
  uint32_t flags;
  uint8_t mask[CAPTURE_MAX_STAGES];
  uint8_t value[CAPTURE_MAX_STAGES];
  uint32_t config[CAPTURE_MAX_STAGES];
} s_cfg;

static uint8_t s_cmd, s_args[4], s_nargs;
static bool s_armed;

void sump_init(const sump_io_t *io) {
  s_io = io;
  memset(&s_cfg, 0, sizeof s_cfg);
  s_cfg.divider = 99;  // 1 MHz
  s_cfg.read_count = s_cfg.delay_count = 4096;
  s_cfg.flags = 0x38;  // only group 0 (8 channels) enabled
}

bool sump_armed(void) { return s_armed; }
bool sump_busy(void) { return s_nargs != 0; }

static void put_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static void send_metadata(void) {
  uint8_t m[48];
  uint32_t n = 0;
  m[n++] = 0x01;  // device name
  memcpy(&m[n], FW_NAME, sizeof FW_NAME);
  n += sizeof FW_NAME;
  m[n++] = 0x02;  // firmware version
  memcpy(&m[n], FW_VERSION, sizeof FW_VERSION);
  n += sizeof FW_VERSION;
  m[n++] = 0x20;  // number of probes
  put_be32(&m[n], 8);
  n += 4;
  m[n++] = 0x21;  // sample memory (bytes)
  put_be32(&m[n], CAPTURE_MAX_SAMPLES);
  n += 4;
  m[n++] = 0x23;  // max sample rate
  put_be32(&m[n], CAPTURE_MAX_RATE_HZ);
  n += 4;
  m[n++] = 0x24;  // protocol version
  put_be32(&m[n], 2);
  n += 4;
  m[n++] = 0x00;  // end
  s_io->write(m, n);
  s_io->flush();
}

static void long_command(uint8_t cmd, const uint8_t *a) {
  uint32_t v = (uint32_t)a[0] | ((uint32_t)a[1] << 8) | ((uint32_t)a[2] << 16) | ((uint32_t)a[3] << 24);
  switch (cmd) {
    case CMD_SET_DIVIDER:
      s_cfg.divider = v & 0x00FFFFFFu;
      break;
    case CMD_CAPTURE_SIZE:  // two 16-bit counts in units of 4 samples
      s_cfg.read_count = ((v & 0xFFFFu) + 1u) * 4u;
      s_cfg.delay_count = ((v >> 16) + 1u) * 4u;
      break;
    case CMD_READCOUNT:
      s_cfg.read_count = (v + 1u) * 4u;
      break;
    case CMD_DELAYCOUNT:
      s_cfg.delay_count = (v + 1u) * 4u;
      break;
    case CMD_SET_FLAGS:
      s_cfg.flags = v;
      break;
    default:
      if (cmd >= CMD_TRIG_BASE && cmd < CMD_TRIG_BASE + 4u * CAPTURE_MAX_STAGES) {
        unsigned stage = (cmd - CMD_TRIG_BASE) >> 2;
        switch ((cmd - CMD_TRIG_BASE) & 3u) {
          case 0: s_cfg.mask[stage] = (uint8_t)v; break;
          case 1: s_cfg.value[stage] = (uint8_t)v; break;
          case 2: s_cfg.config[stage] = v; break;
          default: break;
        }
      }
      break;  // advanced trigger (0x9E/0x9F) and others are ignored
  }
}

void sump_rx(uint8_t b) {
  if (s_nargs) {
    s_args[4 - s_nargs] = b;
    if (--s_nargs == 0) long_command(s_cmd, s_args);
    return;
  }
  if (b & 0x80u) {
    s_cmd = b;
    s_nargs = 4;
    return;
  }
  switch (b) {
    case CMD_RESET:
      s_armed = false;
      break;
    case CMD_ARM:
      s_armed = true;
      break;
    case CMD_ID:
      s_io->write((const uint8_t *)"1ALS", 4);
      s_io->flush();
      break;
    case CMD_METADATA:
      send_metadata();
      break;
    default:  // FINISH_NOW, XON/XOFF, self test, ...: ignored
      break;
  }
}

// Build the capture configuration from the SUMP registers.
static void make_capture_cfg(capture_cfg_t *c) {
  memset(c, 0, sizeof *c);
  c->divider = s_cfg.divider;
  c->total = s_cfg.read_count > CAPTURE_MAX_SAMPLES ? CAPTURE_MAX_SAMPLES : s_cfg.read_count;
  uint32_t post = s_cfg.delay_count > c->total ? c->total : s_cfg.delay_count;
  c->pre = c->total - post;

  // The trigger fires on the first stage that has the START bit set.
  uint8_t n = 0;
  for (unsigned i = 0; i < CAPTURE_MAX_STAGES; i++) {
    if (s_cfg.config[i] & TRIG_CFG_START) {
      n = (uint8_t)(i + 1u);
      break;
    }
  }
  // libsigrok <= 0.5.2 (PulseView 0.4.2) sends one stage too many: the user's
  // stages followed by an always-true (mask 0) stage carrying START. Such
  // trailing stages would delay the trigger by one sample each, and the
  // driver's marker position only counts the user's stages, so drop them.
  while (n > 1 && s_cfg.mask[n - 1u] == 0) n--;
  bool any_mask = false;
  for (unsigned i = 0; i < n; i++) {
    c->mask[i] = s_cfg.mask[i];
    c->value[i] = s_cfg.value[i] & s_cfg.mask[i];
    any_mask |= c->mask[i] != 0;
  }
  // libsigrok forces "stage 0, mask 0" when no trigger is set: start now.
  c->num_stages = any_mask ? n : 0;
  if (!c->num_stages) c->pre = 0;
}

// Upload the window newest-first, as the OLS protocol requires.
static void send_samples(uint32_t start, uint32_t total) {
  uint8_t groups = (uint8_t)(~(s_cfg.flags >> FLAG_GROUPS_SHIFT) & 0x0Fu);
  if (!groups) groups = 1;
  uint8_t keep = (s_cfg.flags & FLAG_RLE) ? 0x7F : 0xFF;  // no RLE: never send a count marker

  uint8_t chunk[64];
  uint32_t k = 0;
  for (uint32_t i = total; i-- > 0;) {
    uint8_t v = capture_sample(start + i) & keep;
    for (unsigned g = 0; g < 4; g++) {
      if (!(groups & (1u << g))) continue;
      chunk[k++] = g == 0 ? v : 0;
      if (k == sizeof chunk) {
        s_io->write(chunk, k);
        k = 0;
      }
    }
  }
  if (k) s_io->write(chunk, k);
  s_io->flush();
}

void sump_run(void) {
  s_armed = false;

  capture_cfg_t c;
  make_capture_cfg(&c);
  capture_timing_t t = capture_timing(c.divider);

  g_la.streaming = false;
  g_la.rate_hz = t.rate_hz;
  g_la.rate_exact = t.exact;
  g_la.samples = c.total;
  g_la.pre = c.pre;
  g_la.num_stages = c.num_stages;
  g_la.mask0 = c.mask[0];
  g_la.value0 = c.value[0];
  g_la.rle_requested = (s_cfg.flags & FLAG_RLE) != 0;
  g_la.state = c.num_stages ? LA_WAIT_TRIGGER : LA_CAPTURING;
  s_io->on_state();

  uint32_t start;
  capture_result_t r = capture_run(&c, s_io->poll, &start);
  if (r == CAPTURE_ABORTED) {
    g_la.state = LA_ABORTED;
    s_io->on_state();
    return;
  }
  g_la.overrun = r == CAPTURE_OVERRUN;
  g_la.state = LA_SENDING;
  s_io->on_state();

  send_samples(start, c.total);

  g_la.captures++;
  g_la.state = LA_DONE;
  s_io->on_state();
}
