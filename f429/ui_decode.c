// Decode page: UART / I2C / SPI decoding of the last capture.
// Setup:  2/8 field, 4/6 change, # decode.
// List:   2/8 scroll, 4/6 page, 5 show on the waveform page, # back to setup.
// Decoding starts at the left edge of the waveform view.
#include <stdio.h>
#include <string.h>

#include "decode.h"
#include "ui_priv.h"

#define LIST_Y    46
#define LIST_ROWS 9

static dec_cfg_t s_cfg;
static bool s_cfg_init, s_list;
static unsigned s_field, s_top, s_cur;

static const uint32_t s_bauds[] = {0, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 1000000};
#define NBAUDS (sizeof s_bauds / sizeof s_bauds[0])

// --- Setup fields ------------------------------------------------------------

typedef enum { FLD_PROTO, FLD_CH, FLD_BAUD, FLD_INVERT, FLD_MODE, FLD_ORDER } fld_kind_t;
typedef struct {
  fld_kind_t kind;
  const char *name;
  uint8_t *ch;  // FLD_CH
  bool optional;
} field_t;

static unsigned fields(field_t *f) {
  unsigned n = 0;
  f[n++] = (field_t){FLD_PROTO, "Protocol", 0, false};
  switch (s_cfg.proto) {
    case DEC_UART:
      f[n++] = (field_t){FLD_CH, "RX", &s_cfg.uart_rx, false};
      f[n++] = (field_t){FLD_BAUD, "Baud", 0, false};
      f[n++] = (field_t){FLD_INVERT, "Idle", 0, false};
      break;
    case DEC_I2C:
      f[n++] = (field_t){FLD_CH, "SCL", &s_cfg.i2c_scl, false};
      f[n++] = (field_t){FLD_CH, "SDA", &s_cfg.i2c_sda, false};
      break;
    default:
      f[n++] = (field_t){FLD_CH, "CLK", &s_cfg.spi_clk, false};
      f[n++] = (field_t){FLD_CH, "MOSI", &s_cfg.spi_mosi, false};
      f[n++] = (field_t){FLD_CH, "MISO", &s_cfg.spi_miso, true};
      f[n++] = (field_t){FLD_CH, "CS", &s_cfg.spi_cs, true};
      f[n++] = (field_t){FLD_MODE, "Mode", 0, false};
      f[n++] = (field_t){FLD_ORDER, "Bit order", 0, false};
      break;
  }
  return n;
}

static void field_value(const field_t *f, char *out, size_t n) {
  static const char *const proto[DEC_PROTO_COUNT] = {"UART 8N1", "I2C", "SPI 8-bit"};
  switch (f->kind) {
    case FLD_PROTO: snprintf(out, n, "%s", proto[s_cfg.proto]); break;
    case FLD_CH:
      if (*f->ch == DEC_NO_CH) snprintf(out, n, "none");
      else snprintf(out, n, "CH%u", *f->ch);
      break;
    case FLD_BAUD:
      if (s_cfg.uart_baud) snprintf(out, n, "%lu", (unsigned long)s_cfg.uart_baud);
      else snprintf(out, n, "auto");
      break;
    case FLD_INVERT: snprintf(out, n, "%s", s_cfg.uart_invert ? "low (inverted)" : "high"); break;
    case FLD_MODE:
      snprintf(out, n, "%u (CPOL %u CPHA %u)", s_cfg.spi_mode, s_cfg.spi_mode >> 1, s_cfg.spi_mode & 1u);
      break;
    default: snprintf(out, n, "%s first", s_cfg.spi_lsb_first ? "LSB" : "MSB"); break;
  }
}

static void field_change(const field_t *f, int d) {
  switch (f->kind) {
    case FLD_PROTO: s_cfg.proto = (dec_proto_t)((s_cfg.proto + DEC_PROTO_COUNT + d) % DEC_PROTO_COUNT); break;
    case FLD_CH: {
      unsigned n = f->optional ? LA_NUM_CH + 1u : LA_NUM_CH;  // the extra value is "none"
      unsigned v = *f->ch == DEC_NO_CH ? LA_NUM_CH : *f->ch;
      v = (v + n + (unsigned)d) % n;
      *f->ch = v == LA_NUM_CH ? DEC_NO_CH : (uint8_t)v;
      break;
    }
    case FLD_BAUD: {
      unsigned i = 0;
      while (i < NBAUDS && s_bauds[i] != s_cfg.uart_baud) i++;
      if (i == NBAUDS) i = 0;
      s_cfg.uart_baud = s_bauds[(i + NBAUDS + (unsigned)d) % NBAUDS];
      break;
    }
    case FLD_INVERT: s_cfg.uart_invert = !s_cfg.uart_invert; break;
    case FLD_MODE: s_cfg.spi_mode = (uint8_t)((s_cfg.spi_mode + 4u + (unsigned)d) % 4u); break;
    default: s_cfg.spi_lsb_first = !s_cfg.spi_lsb_first; break;
  }
}

static void draw_setup(void) {
  field_t f[8];
  unsigned n = fields(f);
  char v[32], line[48];
  gfx_fill(0, TITLE_H, GFX_W, GFX_H - TITLE_H, C_BG);
  if (s_field >= n) s_field = n - 1u;
  for (unsigned i = 0; i < n; i++) {
    field_value(&f[i], v, sizeof v);
    snprintf(line, sizeof line, "%-9s %s", f[i].name, v);
    ui_row(30 + (int)i * 20, line, C_TEXT, i == s_field);
  }
  capture_window_t w = capture_last_window();
  gfx_text(4, 196, &font_s, w.valid ? "# decode the capture" : "No capture yet (B = run)", w.valid ? C_TEXT : C_WARN,
           C_BG);
  ui_hint("2/8 field  4/6 change  # go");
}

// --- Result list -------------------------------------------------------------

static void item_text(const dec_item_t *it, char *out, size_t n) {
  char t[16];
  const capture_window_t *w = &g_dec.win;
  ui_fmt_stime(t, sizeof t, (int64_t)it->at - (w->trigger >= 0 ? w->trigger : 0), w->rate_hz);
  switch (it->kind) {
    case DEC_K_START: snprintf(out, n, "%-10s START", t); break;
    case DEC_K_STOP: snprintf(out, n, "%-10s STOP", t); break;
    case DEC_K_ADDR:
      snprintf(out, n, "%-10s @%02X %s %s", t, it->data, (it->flags & DEC_F_READ) ? "R" : "W",
               (it->flags & DEC_F_ACK) ? "ACK" : "NAK");
      break;
    case DEC_K_DATA: snprintf(out, n, "%-10s  %02X %s", t, it->data, (it->flags & DEC_F_ACK) ? "ACK" : "NAK"); break;
    default:
      if (g_dec.cfg.proto == DEC_SPI) {
        if (it->flags & DEC_F_MISO) snprintf(out, n, "%-10s %02X / %02X", t, it->data, it->data2);
        else snprintf(out, n, "%-10s %02X", t, it->data);
      } else {
        char c = (it->data >= 0x20 && it->data < 0x7F) ? (char)it->data : '.';
        snprintf(out, n, "%-10s %02X '%c'%s", t, it->data, c, (it->flags & DEC_F_ERROR) ? " ERR" : "");
      }
      break;
  }
}

static void draw_list(void) {
  char line[48];
  gfx_fill(0, TITLE_H, GFX_W, GFX_H - TITLE_H, C_BG);
  if (g_dec.error) {
    gfx_text(4, 30, &font_s, g_dec.error, C_ERR, C_BG);
  } else {
    static const char *const proto[DEC_PROTO_COUNT] = {"UART", "I2C", "SPI"};
    if (g_dec.cfg.proto == DEC_UART)
      snprintf(line, sizeof line, "UART CH%u %lu%s: %lu%s", g_dec.cfg.uart_rx, (unsigned long)g_dec.baud,
               g_dec.cfg.uart_baud ? "" : " auto", (unsigned long)g_dec.count, g_dec.truncated ? "+" : "");
    else
      snprintf(line, sizeof line, "%s: %lu item%s%s%s", proto[g_dec.cfg.proto], (unsigned long)g_dec.count,
               g_dec.count == 1 ? "" : "s", g_dec.truncated ? " (full)" : "", g_dec.done ? "" : " ...");
    gfx_text(4, 28, &font_s, line, C_TEXT, C_BG);
  }
  if (s_cur >= g_dec.count && g_dec.count) s_cur = g_dec.count - 1u;
  if (s_cur < s_top) s_top = s_cur;
  if (s_cur >= s_top + LIST_ROWS) s_top = s_cur - LIST_ROWS + 1u;
  for (unsigned r = 0; r < LIST_ROWS && s_top + r < g_dec.count; r++) {
    const dec_item_t *it = &g_dec.items[s_top + r];
    item_text(it, line, sizeof line);
    uint16_t c = (it->flags & DEC_F_ERROR) ? C_ERR : it->kind == DEC_K_START || it->kind == DEC_K_STOP ? C_DIM : C_TEXT;
    ui_row(LIST_Y + (int)r * LINE_H, line, c, s_top + r == s_cur);
  }
  if (!g_dec.error && g_dec.done && !g_dec.count) gfx_text(4, LIST_Y, &font_s, "Nothing decoded", C_WARN, C_BG);
  ui_hint("2/8 4/6 scroll 5 show # setup");
}

static void decode_draw(void) {
  gfx_fill(0, 0, GFX_W, TITLE_H, C_BG);
  ui_title("Decode", s_list ? "result" : "setup");
  if (s_list) draw_list();
  else draw_setup();
}

static void decode_enter(void) {
  if (!s_cfg_init) {
    dec_default_cfg(&s_cfg);
    s_cfg_init = true;
  }
}

static void start(void) {
  capture_window_t w = capture_last_window();
  uint32_t from = 0, count;
  wave_view(&from, &count);
  dec_start(&s_cfg, &w, from);
  s_top = s_cur = 0;
  s_list = true;
}

static bool decode_key(uint8_t k) {
  if (!s_list) {
    field_t f[8];
    unsigned n = fields(f);
    switch (k) {
      case '2': s_field = (s_field + n - 1u) % n; break;
      case '8': s_field = (s_field + 1u) % n; break;
      case '4': field_change(&f[s_field], -1); break;
      case '6': field_change(&f[s_field], 1); break;
      case '#': start(); break;
      default: return false;
    }
    decode_draw();
    return true;
  }
  switch (k) {
    case '2': if (s_cur) s_cur--; break;
    case '8': if (s_cur + 1u < g_dec.count) s_cur++; break;
    case '4': s_cur = s_cur > LIST_ROWS ? s_cur - LIST_ROWS : 0; break;
    case '6': s_cur = s_cur + LIST_ROWS < g_dec.count ? s_cur + LIST_ROWS : (g_dec.count ? g_dec.count - 1u : 0); break;
    case '#': s_list = false; break;
    case '5':
      if (g_dec.count) {
        wave_show(g_dec.items[s_cur].at);
        ui_goto(PAGE_WAVE);
      }
      return true;
    default: return false;
  }
  decode_draw();
  return true;
}

static void decode_poll(void) {
  if (!s_list || !g_dec.active) return;
  uint32_t before = g_dec.count;
  bool done = dec_run(64);
  if (done || (g_dec.count != before && g_dec.count <= LIST_ROWS)) draw_list();
}

const ui_page_t ui_page_decode = {"Decode", decode_enter, 0, decode_draw, decode_key, decode_poll, 0};
