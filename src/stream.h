// Streaming mode: the text protocol of sigrok-pico, spoken by libsigrok's
// "raspberrypi-pico" driver (libsigrok >= 2023, i.e. PulseView nightly).
// Samples are run-length encoded and sent while sampling continues, so the
// capture length is limited only by the host.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  void (*write)(const uint8_t *data, uint32_t len);  // blocking
  void (*flush)(void);
  int (*read)(void);       // next received byte or -1
  bool (*service)(void);   // keep USB alive; false = host gone
  void (*on_state)(void);  // status changed (redraw UI)
} stream_io_t;

void stream_init(const stream_io_t *io);
void stream_rx(uint8_t byte);  // feed received bytes (idle state)
bool stream_pending(void);     // host asked to start streaming
void stream_run(void);         // stream until done / stopped
