#include "profile.h"

volatile profile_t g_profile = {
  .map = {
    [N64_A] = IN_P1_B1,
    [N64_B] = IN_P1_B2,
    [N64_Z] = IN_P1_B3,
    [N64_START] = IN_P1_START,
    [N64_L] = IN_P1_B5,
    [N64_R] = IN_P1_B6,
    [N64_CU] = IN_P1_B4,   // placeholder defaults; change later
    [N64_CD] = 0xFF,
    [N64_CL] = 0xFF,
    [N64_CR] = 0xFF,

    // DPAD defaults to joystick switches
    [N64_DU] = IN_P1_UP,
    [N64_DD] = IN_P1_DOWN,
    [N64_DL] = IN_P1_LEFT,
    [N64_DR] = IN_P1_RIGHT,
  },
  .p1_stick_mode = STICK_MODE_DPAD,
  .p2_stick_mode = STICK_MODE_DPAD,
  .analog_throw = 80
};
