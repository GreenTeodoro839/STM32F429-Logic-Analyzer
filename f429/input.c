#include "input.h"

#include "board.h"
#include "ch455.h"
#include "clock.h"
#include "keys.h"

#define SCAN_MS      20u   // keypad poll period (one 2-byte bus transfer, ~0.3 ms)
#define REPEAT_DELAY 450u  // ms before a held key repeats
#define REPEAT_EVERY 90u   // ms between repeats

static const char s_legend[16] = {'1', '2', '3', 'A', '4', '5', '6', 'B', '7', '8', '9', 'C', '*', '0', '#', 'D'};

static uint8_t s_queue[8];
static uint8_t s_head, s_tail;
static int s_held = -1;  // keypad index being held
static uint32_t s_scan_ms, s_held_ms, s_repeat_ms;
static int s_last_code = -1;
static bool s_keypad;

// Debug hook: write a key code here (e.g. with OpenOCD) to inject an event.
volatile uint8_t g_input_inject;

static void push(uint8_t k) {
  uint8_t next = (uint8_t)((s_head + 1u) % sizeof s_queue);
  if (next == s_tail) return;  // full: drop
  s_queue[s_head] = k;
  s_head = next;
}

void input_inject(uint8_t key) { push(key); }

void input_init(void) {
  keys_init();
  s_keypad = ch455_init();
}

bool input_keypad_ok(void) { return s_keypad; }
int input_last_code(void) { return s_last_code; }

static bool repeats(char c) { return (c >= '0' && c <= '9') || c == 'C' || c == 'D'; }

static void scan_keypad(uint32_t now) {
  int code = ch455_read_key();
  if (code < 0) return;
  s_last_code = code;
  unsigned row = ((unsigned)code >> 3) & 7u, col = (unsigned)code & 3u;
  bool pressed = (code & 0x40) && row < 4u;
  if (!pressed) {
    s_held = -1;
    return;
  }
  int idx = (int)(row * 4u + col);
  if (idx != s_held) {
    s_held = idx;
    s_held_ms = s_repeat_ms = now;
    push((uint8_t)s_legend[idx]);
  } else if (repeats(s_legend[idx]) && (uint32_t)(now - s_held_ms) >= REPEAT_DELAY &&
             (uint32_t)(now - s_repeat_ms) >= REPEAT_EVERY) {
    s_repeat_ms = now;
    push((uint8_t)s_legend[idx]);
  }
}

uint8_t input_poll(void) {
  uint32_t now = millis();
  uint8_t b = keys_poll();
  if (b & KEY_1) push(KEY_BTN1);
  if (b & KEY_2) push(KEY_BTN2);
  if (g_input_inject) {
    push(g_input_inject);
    g_input_inject = 0;
  }
  if (s_keypad && (uint32_t)(now - s_scan_ms) >= SCAN_MS) {
    s_scan_ms = now;
    scan_keypad(now);
  }
  if (s_head == s_tail) return 0;
  uint8_t k = s_queue[s_tail];
  s_tail = (uint8_t)((s_tail + 1u) % sizeof s_queue);
  return k;
}

void input_discard(void) {
  s_head = s_tail = 0;
  keys_sync();
  if (s_keypad) {
    int code = ch455_read_key();
    if (code >= 0 && (code & 0x40) && (((unsigned)code >> 3) & 7u) < 4u)
      s_held = (int)((((unsigned)code >> 3) & 7u) * 4u + ((unsigned)code & 3u));
    else
      s_held = -1;
  }
}

bool input_cancel_requested(void) {
  return !gpio_read(KEY2_PORT, KEY2_PIN) || (s_keypad && ch455_int_active()) || g_input_inject;
}
