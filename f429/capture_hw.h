// F429-specific extensions of the sampling engine (common/capture.h).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "capture.h"

#define CAPTURE_CHUNK      32768u               // samples per DMA buffer (double-buffer mode)
#define CAPTURE_SRAM_RING  (64u * 1024u)        // SRAM3
#define CAPTURE_SRAM_MARGIN  4096u              // slack for post-trigger stop latency
#define CAPTURE_SDRAM_RING (32u * 1024u * 1024u)
#define CAPTURE_SDRAM_MARGIN (64u * 1024u)

typedef enum { CAPTURE_MEM_SRAM, CAPTURE_MEM_SDRAM } capture_mem_t;

void capture_init(bool sdram_ok);
bool capture_sdram_ok(void);

// The last snapshot window, for the LCD preview. trigger is the window
// index of the trigger sample, or -1 for an untriggered capture.
typedef struct {
  bool valid;
  capture_mem_t mem;
  uint32_t start, total;  // ring index of the first sample, length
  int32_t trigger;
  uint32_t rate_hz;
  int32_t lost;  // samples the DMA missed (-1 = not measured)
} capture_window_t;
capture_window_t capture_last_window(void);

// OR and AND of the ring samples [index, index + n) (index wraps).
void capture_or_and(uint32_t index, uint32_t n, uint8_t *or_out, uint8_t *and_out);

// Live meter: free-running sampling at `ticks` per sample into the SRAM ring.
// cb(p, n) receives every completed chunk from a low-priority interrupt, or
// (0, 0) if chunks were lost because processing fell behind. Any snapshot or
// stream stops the meter.
typedef void (*capture_chunk_fn)(const uint8_t *p, uint32_t n);
void capture_meter_start(uint32_t ticks, capture_chunk_fn cb);
void capture_meter_stop(void);
bool capture_meter_running(void);

// Raw sampling for the DMA self-test: free-running into the given ring.
void capture_hw_start(capture_mem_t mem, const capture_timing_t *t);
void capture_hw_stop(void);
uint32_t capture_hw_chunks(void);         // DMA buffers completed so far
uint32_t capture_hw_errors(void);         // DMA error flags seen since start
int32_t capture_hw_lost(void);            // samples lost in the last run (-1 = unknown)
const uint8_t *capture_hw_ring(capture_mem_t mem, uint32_t *size);
