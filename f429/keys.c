#include "keys.h"

#include "board.h"
#include "clock.h"

#define DEBOUNCE_MS 30u

static uint8_t s_stable, s_last_raw;
static uint32_t s_change_ms;

static uint8_t raw(void) {
  uint8_t k = 0;
  if (!gpio_read(KEY1_PORT, KEY1_PIN)) k |= KEY_1;  // active low
  if (!gpio_read(KEY2_PORT, KEY2_PIN)) k |= KEY_2;
  return k;
}

void keys_init(void) {
  RCC->AHB1ENR |= RCC_AHB1ENR_GPIOGEN;
  (void)RCC->AHB1ENR;
  gpio_mode(KEY1_PORT, KEY1_PIN, GPIO_IN);  // external 10k pull-ups
  gpio_mode(KEY2_PORT, KEY2_PIN, GPIO_IN);
  s_stable = s_last_raw = raw();
}

uint8_t keys_poll(void) {
  uint32_t now = millis();
  uint8_t r = raw();
  if (r != s_last_raw) {
    s_last_raw = r;
    s_change_ms = now;
    return 0;
  }
  if (r == s_stable || (uint32_t)(now - s_change_ms) < DEBOUNCE_MS) return 0;
  uint8_t pressed = r & (uint8_t)~s_stable;
  s_stable = r;
  return pressed;
}
