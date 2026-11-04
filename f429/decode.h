// On-device protocol decoders (UART, I2C, SPI) over the last capture window.
// Decoding is incremental: dec_run() does a bounded amount of work per call.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "capture_hw.h"

#define DEC_MAX_ITEMS 1024u
#define DEC_NO_CH     0xFFu

typedef enum { DEC_UART, DEC_I2C, DEC_SPI, DEC_PROTO_COUNT } dec_proto_t;

typedef struct {
  dec_proto_t proto;
  // UART: 8 data bits, LSB first, no parity, 1 stop bit
  uint8_t uart_rx;
  uint32_t uart_baud;  // 0 = detect from the shortest pulse
  bool uart_invert;    // idle low
  // I2C
  uint8_t i2c_scl, i2c_sda;
  // SPI: 8-bit words
  uint8_t spi_clk, spi_mosi, spi_miso, spi_cs;  // miso / cs may be DEC_NO_CH
  uint8_t spi_mode;                             // 0..3 (CPOL, CPHA)
  bool spi_lsb_first;
} dec_cfg_t;

typedef enum {
  DEC_K_BYTE,   // UART byte / SPI word (data = MOSI, data2 = MISO)
  DEC_K_START,  // I2C start or repeated start
  DEC_K_ADDR,   // I2C address byte: data = 7-bit address, DEC_F_READ
  DEC_K_DATA,   // I2C data byte
  DEC_K_STOP,   // I2C stop
} dec_kind_t;

#define DEC_F_ERROR 0x01u  // UART framing error
#define DEC_F_ACK   0x02u  // I2C byte acknowledged
#define DEC_F_READ  0x04u  // I2C read address
#define DEC_F_MISO  0x08u  // SPI: data2 is valid

typedef struct {
  uint32_t at;  // window index where the item starts
  uint8_t kind, data, data2, flags;
} dec_item_t;

typedef struct {
  dec_cfg_t cfg;
  capture_window_t win;
  dec_item_t items[DEC_MAX_ITEMS];
  uint32_t count;
  uint32_t baud;    // UART: baud rate used (detected or configured)
  bool truncated;   // more items than DEC_MAX_ITEMS
  bool active, done;
  const char *error;  // set if decoding could not start
  // decoder state
  uint32_t pos;
  uint32_t bit_fp;  // UART: samples per bit, 16.16 fixed point
  uint8_t prev, bits, word, word2;
  uint32_t word_at;
  bool in_frame, addr_next;
} dec_t;

extern dec_t g_dec;

void dec_default_cfg(dec_cfg_t *c);
// Start decoding the window (from window index `from`).
void dec_start(const dec_cfg_t *cfg, const capture_window_t *w, uint32_t from);
// Decode up to max_events edges/frames; true when finished.
bool dec_run(uint32_t max_events);

// Standard rate closest to `baud` if within the tolerance, else `baud`.
uint32_t dec_snap_baud(uint32_t baud, uint32_t samples_per_bit);
