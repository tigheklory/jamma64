#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "profile.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  N64_VANALOG_UP = 0,
  N64_VANALOG_DOWN,
  N64_VANALOG_LEFT,
  N64_VANALOG_RIGHT,
  N64_VANALOG_COUNT
} n64_virtual_analog_dir_t;

void n64_virtual_clear(void);
void n64_virtual_press(n64_out_t out, uint32_t duration_ms);
bool n64_virtual_pressed(n64_out_t out);
void n64_virtual_analog_press(n64_virtual_analog_dir_t dir, uint32_t duration_ms);
bool n64_virtual_analog_pressed(n64_virtual_analog_dir_t dir);

#ifdef __cplusplus
}
#endif
