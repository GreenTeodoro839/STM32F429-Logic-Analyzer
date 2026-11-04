#include "decode.h"

#include <string.h>

#include "analyze.h"

dec_t g_dec;

void dec_default_cfg(dec_cfg_t *c) {
  memset(c, 0, sizeof *c);
  c->proto = DEC_UART;
  c->uart_rx = 3;  // the UART test signal is wired to CH3
  c->uart_baud = 0;
  c->i2c_scl = 0;
  c->i2c_sda = 1;
  c->spi_clk = 0;
  c->spi_mosi = 1;
  c->spi_miso = 2;
  c->spi_cs = 3;
  c->spi_mode = 0;
}

static void add(uint32_t at, uint8_t kind, uint8_t data, uint8_t data2, uint8_t flags) {
  if (g_dec.count >= DEC_MAX_ITEMS) {
    g_dec.truncated = true;
    return;
  }
  g_dec.items[g_dec.count++] = (dec_item_t){at, kind, data, data2, flags};
}

static const uint32_t s_bauds[] = {300,    600,    1200,   2400,   4800,   9600,    14400,   19200,   28800,
                                   38400,  57600,  76800,  115200, 128000, 230400,  250000,  256000,  460800,
                                   500000, 576000, 921600, 1000000, 1500000, 2000000, 3000000};

uint32_t dec_snap_baud(uint32_t baud, uint32_t spb) {
  // Accept 4 %, or a quarter sample per bit for very short bits.
  uint32_t tol = 40;  // permille
  if (spb && 250u / spb > tol) tol = 250u / spb;
  uint32_t best = baud, best_err = 0xFFFFFFFFu;
  for (unsigned i = 0; i < sizeof s_bauds / sizeof s_bauds[0]; i++) {
    uint32_t b = s_bauds[i];
    uint32_t diff = baud > b ? baud - b : b - baud;
    uint32_t err = (uint32_t)((uint64_t)diff * 1000u / b);
    if (err <= tol && err < best_err) {
      best = b;
      best_err = err;
    }
  }
  return best;
}

// Bit time on channel ch in 1/256 samples, from the first `max_edges` edges
// after `from`, or 0 without at least two edges. The shortest pulse gives a
// first estimate; every pulse up to 9 bits long is then divided by its
// (rounded) number of bits and the results are averaged, which removes most
// of the +-1 sample quantisation of a single pulse.
static uint32_t bit_time_fp(const capture_window_t *w, uint32_t from, unsigned ch, unsigned max_edges) {
  uint8_t mask = (uint8_t)(1u << ch);
  uint32_t shortest = ANA_NONE;
  for (int pass = 0; pass < 2; pass++) {
    uint8_t v = ana_sample(w, from) & mask;
    uint32_t pos = from, last = ANA_NONE;
    uint64_t sum_w = 0, sum_k = 0;
    for (unsigned e = 0; e < max_edges; e++) {
      uint32_t j = ana_next_change(w, pos, w->total, mask, v);
      if (j >= w->total) break;
      if (last != ANA_NONE) {
        uint32_t width = j - last;
        if (pass == 0) {
          if (width < shortest) shortest = width;
        } else {
          uint32_t k = (width + shortest / 2u) / shortest;
          if (k >= 1 && k <= 9) {
            sum_w += width;
            sum_k += k;
          }
        }
      }
      last = j;
      v ^= mask;
      pos = j;
    }
    if (shortest == ANA_NONE) return 0;
    if (pass == 1 && sum_k) return (uint32_t)((sum_w * 256u + sum_k / 2u) / sum_k);
  }
  return shortest * 256u;
}

// --- UART --------------------------------------------------------------------

// Sample point of bit k (0 = start bit) of a frame starting at s: s + (k + 0.5) bits
static uint32_t uart_at(uint32_t s, unsigned k) { return s + (uint32_t)(((uint64_t)(2u * k + 1u) * g_dec.bit_fp) >> 9); }

static bool uart_step(void) {
  dec_t *d = &g_dec;
  const capture_window_t *w = &d->win;
  const uint8_t mask = (uint8_t)(1u << d->cfg.uart_rx);
  const uint8_t idle = d->cfg.uart_invert ? 0 : mask;

  if ((ana_sample(w, d->pos) & mask) != idle) {  // inside a frame or a break: wait for idle
    uint32_t j = ana_next_change(w, d->pos, w->total, mask, idle ^ mask);
    if (j >= w->total) return false;
    d->pos = j;
  }
  uint32_t s = ana_next_change(w, d->pos, w->total, mask, idle);  // start bit
  if (s >= w->total || uart_at(s, 9) >= w->total) return false;
  if ((ana_sample(w, uart_at(s, 0)) & mask) == idle) {  // glitch, not a start bit
    d->pos = s + 1u;
    return true;
  }
  uint8_t byte = 0;
  for (unsigned k = 0; k < 8; k++)
    if (((ana_sample(w, uart_at(s, k + 1u)) & mask) == idle) != 0) byte |= (uint8_t)(1u << k);  // idle level = 1
  bool stop_ok = (ana_sample(w, uart_at(s, 9)) & mask) == idle;
  add(s, DEC_K_BYTE, byte, 0, stop_ok ? 0 : DEC_F_ERROR);
  d->pos = uart_at(s, 9);
  return true;
}

// --- I2C ---------------------------------------------------------------------

static bool i2c_step(void) {
  dec_t *d = &g_dec;
  const capture_window_t *w = &d->win;
  const uint8_t scl = (uint8_t)(1u << d->cfg.i2c_scl), sda = (uint8_t)(1u << d->cfg.i2c_sda), mask = scl | sda;
  uint32_t j = ana_next_change(w, d->pos, w->total, mask, d->prev);
  if (j >= w->total) return false;
  uint8_t v = ana_sample(w, j) & mask, p = d->prev;

  if ((p & scl) && (v & scl) && (p & sda) && !(v & sda)) {  // SDA falls while SCL high
    add(j, DEC_K_START, 0, 0, 0);
    d->in_frame = true;
    d->addr_next = true;
    d->bits = 0;
    d->word = 0;
  } else if ((p & scl) && (v & scl) && !(p & sda) && (v & sda)) {  // SDA rises while SCL high
    add(j, DEC_K_STOP, 0, 0, 0);
    d->in_frame = false;
  } else if (!(p & scl) && (v & scl) && d->in_frame) {  // SCL rises: sample SDA
    unsigned bit = (v & sda) ? 1u : 0u;
    if (d->bits == 0) d->word_at = j;
    if (d->bits < 8) {
      d->word = (uint8_t)((d->word << 1) | bit);
      d->bits++;
    } else {  // 9th clock: acknowledge
      uint8_t ack = bit ? 0 : DEC_F_ACK;
      if (d->addr_next)
        add(d->word_at, DEC_K_ADDR, (uint8_t)(d->word >> 1), 0, (uint8_t)(ack | ((d->word & 1u) ? DEC_F_READ : 0)));
      else
        add(d->word_at, DEC_K_DATA, d->word, 0, ack);
      d->addr_next = false;
      d->bits = 0;
      d->word = 0;
    }
  }
  d->prev = v;
  d->pos = j + 1u;
  return true;
}

// --- SPI ---------------------------------------------------------------------

static bool spi_step(void) {
  dec_t *d = &g_dec;
  const dec_cfg_t *c = &d->cfg;
  const capture_window_t *w = &d->win;
  const uint8_t clk = (uint8_t)(1u << c->spi_clk);
  const uint8_t cs = c->spi_cs == DEC_NO_CH ? 0 : (uint8_t)(1u << c->spi_cs);
  const uint8_t mask = clk | cs;
  uint32_t j = ana_next_change(w, d->pos, w->total, mask, d->prev);
  if (j >= w->total) return false;
  uint8_t s = ana_sample(w, j), v = s & mask, p = d->prev;

  if (cs && ((p ^ v) & cs)) {  // CS edge: a new frame starts, or the frame ends
    d->in_frame = !(v & cs);
    d->bits = 0;
    d->word = d->word2 = 0;
  }
  if ((p ^ v) & clk) {
    bool rising = (v & clk) != 0;
    bool sample_on_rising = c->spi_mode == 0 || c->spi_mode == 3;
    if ((!cs || d->in_frame) && rising == sample_on_rising) {
      unsigned mo = (s >> c->spi_mosi) & 1u;
      unsigned mi = c->spi_miso == DEC_NO_CH ? 0 : (s >> c->spi_miso) & 1u;
      if (d->bits == 0) d->word_at = j;
      if (c->spi_lsb_first) {
        d->word |= (uint8_t)(mo << d->bits);
        d->word2 |= (uint8_t)(mi << d->bits);
      } else {
        d->word = (uint8_t)((d->word << 1) | mo);
        d->word2 = (uint8_t)((d->word2 << 1) | mi);
      }
      if (++d->bits == 8) {
        add(d->word_at, DEC_K_BYTE, d->word, d->word2, c->spi_miso == DEC_NO_CH ? 0 : DEC_F_MISO);
        d->bits = 0;
        d->word = d->word2 = 0;
      }
    }
  }
  d->prev = v;
  d->pos = j + 1u;
  return true;
}

// -----------------------------------------------------------------------------

void dec_start(const dec_cfg_t *cfg, const capture_window_t *w, uint32_t from) {
  dec_t *d = &g_dec;
  d->cfg = *cfg;
  d->win = *w;
  d->count = 0;
  d->truncated = false;
  d->error = NULL;
  d->baud = 0;
  d->pos = from < w->total ? from : w->total;
  d->bits = 0;
  d->word = d->word2 = 0;
  d->in_frame = false;
  d->addr_next = false;
  d->active = false;
  d->done = true;
  if (!w->valid || w->total < 2 || !w->rate_hz) {
    d->error = "No capture";
    return;
  }

  switch (cfg->proto) {
    case DEC_UART: {
      uint32_t baud = cfg->uart_baud;
      if (!baud) {
        uint32_t bit = bit_time_fp(w, d->pos, cfg->uart_rx, 4096);
        if (!bit) {
          d->error = "No activity on RX";
          return;
        }
        baud = dec_snap_baud((uint32_t)(((uint64_t)w->rate_hz * 256u + bit / 2u) / bit), bit / 256u);
      }
      if ((uint64_t)baud * 4u > w->rate_hz) {
        d->error = "Rate too low for baud";
        return;
      }
      d->baud = baud;
      d->bit_fp = (uint32_t)(((uint64_t)w->rate_hz << 8) / baud);
      break;
    }
    case DEC_I2C:
      if (cfg->i2c_scl == cfg->i2c_sda) {
        d->error = "SCL = SDA";
        return;
      }
      d->prev = ana_sample(w, d->pos) & (uint8_t)((1u << cfg->i2c_scl) | (1u << cfg->i2c_sda));
      d->pos++;
      break;
    case DEC_SPI: {
      if (cfg->spi_clk == cfg->spi_mosi || cfg->spi_clk == cfg->spi_cs) {
        d->error = "Channel clash";
        return;
      }
      uint8_t cs = cfg->spi_cs == DEC_NO_CH ? 0 : (uint8_t)(1u << cfg->spi_cs);
      uint8_t s = ana_sample(w, d->pos);
      d->prev = s & (uint8_t)((1u << cfg->spi_clk) | cs);
      d->in_frame = cs ? !(s & cs) : true;
      d->pos++;
      break;
    }
    default:
      d->error = "Bad protocol";
      return;
  }
  d->active = true;
  d->done = false;
}

bool dec_run(uint32_t max_events) {
  dec_t *d = &g_dec;
  if (!d->active) return d->done;
  while (max_events--) {
    bool more;
    switch (d->cfg.proto) {
      case DEC_UART: more = uart_step(); break;
      case DEC_I2C: more = i2c_step(); break;
      default: more = spi_step(); break;
    }
    if (!more || d->truncated || d->pos >= d->win.total) {
      d->active = false;
      d->done = true;
      break;
    }
  }
  return d->done;
}
