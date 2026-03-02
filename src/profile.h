#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  STICK_MODE_DPAD = 0,
  STICK_MODE_ANALOG = 1
} stick_mode_t;

// N64 logical outputs (subset to start; we'll expand)
typedef enum {
  N64_A, N64_B, N64_Z, N64_START,
  N64_L, N64_R,
  N64_CU, N64_CD, N64_CL, N64_CR,
  N64_DU, N64_DD, N64_DL, N64_DR,
  N64_AU, N64_AD, N64_AL, N64_AR,
  N64_OUTPUT_COUNT
} n64_out_t;

// Physical inputs (JAMMA/CPS2-ish). Expand as you wire more.
typedef enum {
  IN_P1_UP, IN_P1_DOWN, IN_P1_LEFT, IN_P1_RIGHT,
  IN_P1_B1, IN_P1_B2, IN_P1_B3, IN_P1_B4, IN_P1_B5, IN_P1_B6,
  IN_P1_START, IN_COIN1, IN_SERVICE, IN_TEST,

  IN_P2_UP, IN_P2_DOWN, IN_P2_LEFT, IN_P2_RIGHT,
  IN_P2_B1, IN_P2_B2, IN_P2_B3, IN_P2_B4, IN_P2_B5, IN_P2_B6,
  IN_P2_START, IN_COIN2,

  IN_COUNT
} phys_in_t;

typedef struct {
  // mapping: physical input -> N64 output index, or 0xFF = unassigned
  // This allows multiple physical inputs to target the same N64 output.
  uint8_t map[IN_COUNT];

  // shared joystick switches can act as DPAD or ANALOG depending on mode
  stick_mode_t p1_stick_mode;
  stick_mode_t p2_stick_mode;

  // analog throw strength (0..127). We'll use e.g. 80 default.
  uint8_t analog_throw;

  // Additional scaling for normalized diagonal axes (70..100, percent).
  uint8_t diagonal_scale_pct;
} profile_t;

extern volatile profile_t g_profile;
void profile_get_defaults(profile_t *out);

#ifdef __cplusplus
}
#endif
