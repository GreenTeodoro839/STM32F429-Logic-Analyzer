// SUMP / OpenBench Logic Sniffer protocol, as spoken by libsigrok's
// "openbench-logic-sniffer" (ols) driver used by PulseView and sigrok-cli.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "capture.h"

typedef struct {
  void (*write)(const uint8_t *data, uint32_t len);  // blocking
  void (*flush)(void);
  capture_poll_fn poll;      // service routine while capturing (abort = true)
  void (*on_state)(void);    // status changed (redraw UI)
} sump_io_t;

void sump_init(const sump_io_t *io);
void sump_rx(uint8_t byte);    // feed received bytes
bool sump_busy(void);          // in the middle of a 5-byte command
bool sump_armed(void);         // a capture has been requested
void sump_run(void);           // run the requested capture and upload it
