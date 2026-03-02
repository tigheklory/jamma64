#include "profile.h"

volatile profile_t g_profile = {
  .map = {
    [0 ... (IN_COUNT - 1)] = 0xFF,
    [IN_P1_B1] = N64_A,
    [IN_P1_B2] = N64_B,
    [IN_P1_B3] = N64_Z,
    [IN_P1_START] = N64_START,
    [IN_P1_B5] = N64_L,
    [IN_P1_B6] = N64_R,
    [IN_P1_B4] = N64_CU,   // placeholder default; change later

    // DPAD defaults to joystick switches.
    [IN_P1_UP] = N64_DU,
    [IN_P1_DOWN] = N64_DD,
    [IN_P1_LEFT] = N64_DL,
    [IN_P1_RIGHT] = N64_DR,
  },
  .p1_stick_mode = STICK_MODE_DPAD,
  .p2_stick_mode = STICK_MODE_DPAD,
  .analog_throw = 80,
  .diagonal_scale_pct = 95
};

static const profile_t k_profile_defaults = {
  .map = {
    [0 ... (IN_COUNT - 1)] = 0xFF,
    [IN_P1_B1] = N64_A,
    [IN_P1_B2] = N64_B,
    [IN_P1_B3] = N64_Z,
    [IN_P1_START] = N64_START,
    [IN_P1_B5] = N64_L,
    [IN_P1_B6] = N64_R,
    [IN_P1_B4] = N64_CU,
    [IN_P1_UP] = N64_DU,
    [IN_P1_DOWN] = N64_DD,
    [IN_P1_LEFT] = N64_DL,
    [IN_P1_RIGHT] = N64_DR,
  },
  .p1_stick_mode = STICK_MODE_DPAD,
  .p2_stick_mode = STICK_MODE_DPAD,
  .analog_throw = 80,
  .diagonal_scale_pct = 95
};

void profile_get_defaults(profile_t *out) {
  if (!out) return;
  *out = k_profile_defaults;
}
