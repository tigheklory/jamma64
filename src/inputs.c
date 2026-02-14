#include "inputs.h"
#include "pico/stdlib.h"

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

  [IN_P1_START] = 16,
  [IN_COIN1]    = 17,
  [IN_SERVICE]  = 18,
  [IN_TEST]     = 19,

  // P2 placeholders (set once wired)
  [IN_P2_UP]    = 20,
  [IN_P2_DOWN]  = 21,
  [IN_P2_LEFT]  = 22,
  [IN_P2_RIGHT] = 26,

  [IN_P2_B1] = 27,
  [IN_P2_B2] = 28,
  [IN_P2_B3] = 0,  // TODO
  [IN_P2_B4] = 0,  // TODO
  [IN_P2_B5] = 0,  // TODO
  [IN_P2_B6] = 0,  // TODO

  [IN_P2_START] = 0, // TODO
  [IN_COIN2]    = 0, // TODO
};

void inputs_init(void) {
  for (int i = 0; i < IN_COUNT; i++) {
    uint8_t pin = pin_for_input[i];
    if (pin == 0) continue; // not assigned yet
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);
  }
}

inputs_t inputs_read(void) {
  inputs_t r = {0};
  for (int i = 0; i < IN_COUNT; i++) {
    uint8_t pin = pin_for_input[i];
    if (pin == 0) continue;
    // active-low: pressed == 1
    bool pressed = (gpio_get(pin) == 0);
    if (pressed) r.bits |= (1ull << i);
  }
  return r;
}
