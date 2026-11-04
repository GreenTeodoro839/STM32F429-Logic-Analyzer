// Internal interface between the UI core (ui.c) and the pages (ui_*.c).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "capture_hw.h"
#include "font.h"
#include "gfx.h"
#include "input.h"
#include "la_status.h"

// --- Look --------------------------------------------------------------------

#define C_BG   RGB(10, 13, 18)
#define C_BAR  RGB(30, 38, 50)
#define C_SEL  RGB(38, 58, 92)
#define C_TEXT RGB(230, 237, 243)
#define C_DIM  RGB(120, 130, 142)
#define C_GRID RGB(36, 44, 56)
#define C_OK   RGB(70, 200, 90)
#define C_WARN RGB(245, 185, 50)
#define C_BUSY RGB(90, 170, 255)
#define C_ERR  RGB(250, 85, 75)
#define C_TRIG RGB(120, 96, 40)
#define C_CURA RGB(80, 220, 230)
#define C_CURB RGB(240, 110, 220)

extern const uint16_t ui_ch_color[LA_NUM_CH];

#define TITLE_H 24
#define LINE_H  18  // list rows with font_s
#define HINT_Y  (GFX_H - 16)

// --- Pages -------------------------------------------------------------------

typedef enum {
  PAGE_WAVE,
  PAGE_SETUP,
  PAGE_METER,
  PAGE_DECODE,
  PAGE_GEN,
  PAGE_PINS,
  PAGE_INFO,
  PAGE_MENU,
  PAGE_COUNT
} page_id_t;

typedef struct {
  const char *name;
  void (*enter)(void);      // optional
  void (*leave)(void);      // optional
  void (*draw)(void);       // full redraw
  bool (*key)(uint8_t k);   // true if handled
  void (*poll)(void);       // optional, called every UI cycle
  void (*seg7)(char *out);  // optional, <= 8 chars for the 4-digit display
} ui_page_t;

extern const ui_page_t ui_page_wave, ui_page_setup, ui_page_meter, ui_page_decode, ui_page_gen, ui_page_pins,
    ui_page_info, ui_page_menu;

void ui_goto(page_id_t p);
page_id_t ui_current(void);

// --- Drawing helpers (ui.c) ------------------------------------------------------

void ui_title(const char *title, const char *right);
void ui_hint(const char *text);  // dim key help on the bottom line
// One list row: `selected` draws the highlight bar.
void ui_row(int y, const char *text, uint16_t color, bool selected);

// --- Formatting (ui.c) -----------------------------------------------------------

// v / scale with `digits` significant digits; trailing zeros dropped if trim.
void ui_fmt_digits(char *out, size_t n, uint64_t v, uint64_t scale, unsigned digits, bool trim, const char *suffix);
void ui_fmt_si(char *out, size_t n, uint64_t v, const char *unit);  // 1.23k
void ui_fmt_hz_milli(char *out, size_t n, uint64_t mhz, unsigned digits);  // from millihertz: 1.0000kHz
void ui_fmt_time(char *out, size_t n, uint64_t samples, uint32_t rate_hz);  // 1.6ms
void ui_fmt_stime(char *out, size_t n, int64_t samples, uint32_t rate_hz);  // +1.6ms / -20us
// 4-digit display text for a frequency (kHz, decimal point placed), e.g. "1.000"
void ui_seg7_khz(char *out, uint64_t mhz);

// --- Status block of the waveform page (ui.c) ----------------------------------

#define STATUS_H 86
void ui_draw_status(void);
void ui_draw_usb_badge(void);

// --- Cross-page services -----------------------------------------------------------

void ui_request_capture(void);  // standalone RUN (ui_setup.c settings)
bool ui_capture_busy(void);     // a capture or stream is in progress

// Waveform page (ui_wave.c)
void wave_capture_started(void);
void wave_capture_done(void);
bool wave_view(uint32_t *from, uint32_t *count);  // visible range of the last capture
void wave_show(uint32_t index);                   // center on a sample, put cursor A there
void wave_levels(uint8_t hi, uint8_t lo);         // live levels of the last frame

// Setup page (ui_setup.c)
bool setup_take_request(capture_cfg_t *cfg);  // pending standalone capture
void setup_describe(char *out, size_t n);     // "20MHz 1M CH0 rising"
