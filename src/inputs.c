#include "inputs.h"
#include "pico/stdlib.h"

#define PIN_UNUSED 0xFFu

// Pick GPIOs for your harness here.
// Convention: active-low switches to GND with pull-ups enabled.
static const uint8_t pin_for_input[IN_COUNT] = {
  [IN_P1_UP]    = 6,
  [IN_P1_DOWN]  = 7,
  [IN_P1_LEFT]  = 8,
  [IN_P1_RIGHT] = 9,

  [IN_P1_B1] = 10,
  [IN_P1_B2] = 11,
  [IN_P1_B3] = 12,
  [IN_P1_B4] = 13,
  [IN_P1_B5] = 14,
  [IN_P1_B6] = 15,

  [IN_P1_START] = 0,
  // Coin/service/test are intentionally not read by Pico now.
  // Wire these straight through to Aleck 64.
  [IN_COIN1]    = PIN_UNUSED,
  [IN_SERVICE]  = PIN_UNUSED,
  [IN_TEST]     = PIN_UNUSED,

  // P2 placeholders (set once wired)
  [IN_P2_UP]    = 28,
  [IN_P2_DOWN]  = 27,
  [IN_P2_LEFT]  = 26,
  [IN_P2_RIGHT] = 22,

  [IN_P2_B1] = 21,
  [IN_P2_B2] = 20,
  [IN_P2_B3] = 19,
  [IN_P2_B4] = 18,
  [IN_P2_B5] = 17,
  [IN_P2_B6] = 16,

  [IN_P2_START] = 1,
  [IN_COIN2]    = PIN_UNUSED,
};

void inputs_init(void) {
  for (int i = 0; i < IN_COUNT; i++) {
    uint8_t pin = pin_for_input[i];
    if (pin == PIN_UNUSED) continue; // not assigned yet
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);
  }
}

inputs_t inputs_read(void) {
  inputs_t r = {0};
  for (int i = 0; i < IN_COUNT; i++) {
    uint8_t pin = pin_for_input[i];
    if (pin == PIN_UNUSED) continue;
    // active-low: pressed == 1
    bool pressed = (gpio_get(pin) == 0);
    if (pressed) r.bits |= (1ull << i);
  }
  return r;
}
