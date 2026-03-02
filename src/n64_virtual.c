#include "n64_virtual.h"

#include "pico/time.h"

static volatile uint32_t g_virtual_hold_until_us[N64_OUTPUT_COUNT];
static volatile uint32_t g_virtual_analog_hold_until_us[N64_VANALOG_COUNT];
static volatile uint32_t g_virtual_dpad_hold_until_us[N64_VDPAD_COUNT];

void n64_virtual_clear(void) {
  for (int i = 0; i < N64_OUTPUT_COUNT; i++) {
    g_virtual_hold_until_us[i] = 0;
  }
  for (int i = 0; i < N64_VANALOG_COUNT; i++) {
    g_virtual_analog_hold_until_us[i] = 0;
  }
  for (int i = 0; i < N64_VDPAD_COUNT; i++) {
    g_virtual_dpad_hold_until_us[i] = 0;
  }
}

void n64_virtual_press(n64_out_t out, uint32_t duration_ms) {
  if (out < 0 || out >= N64_OUTPUT_COUNT) return;
  if (duration_ms == 0) duration_ms = 80;
  if (duration_ms > 5000u) duration_ms = 5000u;

  uint32_t now = time_us_32();
  uint32_t hold_us = duration_ms * 1000u;
  uint32_t until = now + hold_us;
  // Keep 0 as a reserved "inactive" sentinel across timer wrap.
  if (until == 0u) until = 1u;
  g_virtual_hold_until_us[out] = until;
}

bool n64_virtual_pressed(n64_out_t out) {
  if (out < 0 || out >= N64_OUTPUT_COUNT) return false;
  uint32_t now = time_us_32();
  uint32_t until = g_virtual_hold_until_us[out];
  if (until == 0u) return false;
  return (int32_t)(until - now) > 0;
}

void n64_virtual_analog_press(n64_virtual_analog_dir_t dir, uint32_t duration_ms) {
  if (dir < 0 || dir >= N64_VANALOG_COUNT) return;
  if (duration_ms == 0) duration_ms = 80;
  if (duration_ms > 5000u) duration_ms = 5000u;

  uint32_t now = time_us_32();
  uint32_t hold_us = duration_ms * 1000u;
  uint32_t until = now + hold_us;
  if (until == 0u) until = 1u;
  g_virtual_analog_hold_until_us[dir] = until;
}

bool n64_virtual_analog_pressed(n64_virtual_analog_dir_t dir) {
  if (dir < 0 || dir >= N64_VANALOG_COUNT) return false;
  uint32_t now = time_us_32();
  uint32_t until = g_virtual_analog_hold_until_us[dir];
  if (until == 0u) return false;
  return (int32_t)(until - now) > 0;
}

void n64_virtual_dpad_press(n64_virtual_dpad_dir_t dir, uint32_t duration_ms) {
  if (dir < 0 || dir >= N64_VDPAD_COUNT) return;
  if (duration_ms == 0) duration_ms = 80;
  if (duration_ms > 5000u) duration_ms = 5000u;

  uint32_t now = time_us_32();
  uint32_t hold_us = duration_ms * 1000u;
  uint32_t until = now + hold_us;
  if (until == 0u) until = 1u;
  g_virtual_dpad_hold_until_us[dir] = until;
}

bool n64_virtual_dpad_pressed(n64_virtual_dpad_dir_t dir) {
  if (dir < 0 || dir >= N64_VDPAD_COUNT) return false;
  uint32_t now = time_us_32();
  uint32_t until = g_virtual_dpad_hold_until_us[dir];
  if (until == 0u) return false;
  return (int32_t)(until - now) > 0;
}
