#include "n64.h"

#include <cstring>

#include "hardware/pio.h"

#include "inputs.h"
#include "profile.h"

#include "N64Console.hpp"

namespace {

constexpr uint kN64DataPin = 2;

N64Console *g_console = nullptr;

static inline uint8_t clamp_analog(bool neg, bool pos, uint8_t mag) {
  if (neg && !pos) return static_cast<uint8_t>(256u - mag);
  if (pos && !neg) return mag;
  return 0;
}

static inline bool n64_map_pressed(inputs_t in, n64_out_t out) {
  uint8_t phys = g_profile.map[out];
  if (phys == 0xFFu || phys >= IN_COUNT) return false;
  return inputs_get(in, static_cast<phys_in_t>(phys));
}

static void build_report(n64_report_t *report) {
  std::memset(report, 0, sizeof(*report));

  inputs_t in = inputs_read();

  report->a = n64_map_pressed(in, N64_A);
  report->b = n64_map_pressed(in, N64_B);
  report->z = n64_map_pressed(in, N64_Z);
  report->start = n64_map_pressed(in, N64_START);
  if (g_profile.p1_stick_mode == STICK_MODE_DPAD) {
    report->dpad_up = n64_map_pressed(in, N64_DU);
    report->dpad_down = n64_map_pressed(in, N64_DD);
    report->dpad_left = n64_map_pressed(in, N64_DL);
    report->dpad_right = n64_map_pressed(in, N64_DR);
  }

  report->l = n64_map_pressed(in, N64_L);
  report->r = n64_map_pressed(in, N64_R);
  report->c_up = n64_map_pressed(in, N64_CU);
  report->c_down = n64_map_pressed(in, N64_CD);
  report->c_left = n64_map_pressed(in, N64_CL);
  report->c_right = n64_map_pressed(in, N64_CR);

  uint8_t sx = 0;
  uint8_t sy = 0;
  if (g_profile.p1_stick_mode == STICK_MODE_ANALOG) {
    bool su = n64_map_pressed(in, N64_DU);
    bool sd = n64_map_pressed(in, N64_DD);
    bool sl = n64_map_pressed(in, N64_DL);
    bool sr = n64_map_pressed(in, N64_DR);
    uint8_t mag = g_profile.analog_throw;
    sx = clamp_analog(sl, sr, mag);
    sy = clamp_analog(sd, su, mag);
  }

  report->stick_x = sx;
  report->stick_y = sy;
}

}  // namespace

bool n64_init(void) {
  if (!g_console) {
    g_console = new N64Console(kN64DataPin, pio0);
  }
  return g_console->Detect();
}

void n64_task(void) {
  if (!g_console) return;
  // Wait for the next host poll and reply immediately using the current input state.
  // Note: WaitForPoll blocks; this backend is best run on a dedicated core.
  bool rumble = g_console->WaitForPoll();
  (void)rumble;

  n64_report_t report{};
  build_report(&report);
  g_console->SendReport(&report);
}
