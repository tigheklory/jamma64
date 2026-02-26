#include "n64.h"

#include <cstring>

#include "hardware/pio.h"
#include "pico/time.h"

#include "inputs.h"
#include "profile.h"
#include "n64_virtual.h"

#include "joybus.h"
#include "n64_definitions.h"

namespace {

constexpr uint kN64DataPin = 2;
constexpr uint64_t kReplyDelayUs = 0;
constexpr uint64_t kRxTimeoutUs = 50;
constexpr uint64_t kResetWaitUs = 50;

joybus_port_t g_port{};
bool g_port_ready = false;

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

static inline bool n64_out_pressed(inputs_t in, n64_out_t out) {
  return n64_map_pressed(in, out) || n64_virtual_pressed(out);
}

static void build_report(n64_report_t *report) {
  std::memset(report, 0, sizeof(*report));

  inputs_t in = inputs_read();

  report->a = n64_out_pressed(in, N64_A);
  report->b = n64_out_pressed(in, N64_B);
  report->z = n64_out_pressed(in, N64_Z);
  report->start = n64_out_pressed(in, N64_START);

  bool vdu = n64_virtual_dpad_pressed(N64_VDPAD_UP);
  bool vdd = n64_virtual_dpad_pressed(N64_VDPAD_DOWN);
  bool vdl = n64_virtual_dpad_pressed(N64_VDPAD_LEFT);
  bool vdr = n64_virtual_dpad_pressed(N64_VDPAD_RIGHT);

  if (g_profile.p1_stick_mode == STICK_MODE_DPAD) {
    report->dpad_up = n64_out_pressed(in, N64_DU) || vdu;
    report->dpad_down = n64_out_pressed(in, N64_DD) || vdd;
    report->dpad_left = n64_out_pressed(in, N64_DL) || vdl;
    report->dpad_right = n64_out_pressed(in, N64_DR) || vdr;
  } else {
    // Web virtual d-pad is always routed to N64 d-pad bits.
    report->dpad_up = vdu;
    report->dpad_down = vdd;
    report->dpad_left = vdl;
    report->dpad_right = vdr;
  }

  report->l = n64_out_pressed(in, N64_L);
  report->r = n64_out_pressed(in, N64_R);
  report->c_up = n64_out_pressed(in, N64_CU);
  report->c_down = n64_out_pressed(in, N64_CD);
  report->c_left = n64_out_pressed(in, N64_CL);
  report->c_right = n64_out_pressed(in, N64_CR);

  uint8_t sx = 0;
  uint8_t sy = 0;
  bool su = n64_virtual_analog_pressed(N64_VANALOG_UP);
  bool sd = n64_virtual_analog_pressed(N64_VANALOG_DOWN);
  bool sl = n64_virtual_analog_pressed(N64_VANALOG_LEFT);
  bool sr = n64_virtual_analog_pressed(N64_VANALOG_RIGHT);
  if (g_profile.p1_stick_mode == STICK_MODE_ANALOG) {
    su = su || n64_out_pressed(in, N64_DU);
    sd = sd || n64_out_pressed(in, N64_DD);
    sl = sl || n64_out_pressed(in, N64_DL);
    sr = sr || n64_out_pressed(in, N64_DR);
  }
  uint8_t mag = g_profile.analog_throw;
  sx = clamp_analog(sl, sr, mag);
  sy = clamp_analog(sd, su, mag);

  report->stick_x = sx;
  report->stick_y = sy;
}

}  // namespace

bool n64_init(void) {
  if (!g_port_ready) {
    joybus_port_init(&g_port, kN64DataPin, pio0, -1, -1);
    g_port_ready = true;
  }
  return true;
}

void n64_task(void) {
  if (!g_port_ready) return;

  uint8_t cmd = 0;
  // First byte timeout enabled so this function stays non-blocking and USB can be serviced.
  if (joybus_receive_bytes(&g_port, &cmd, 1, kRxTimeoutUs, true) != 1) {
    return;
  }

  switch ((N64Command)cmd) {
    case N64Command::RESET:
    case N64Command::PROBE: {
      busy_wait_us(kReplyDelayUs);
      n64_status_t status = default_n64_status;
      joybus_send_bytes(&g_port, reinterpret_cast<uint8_t *>(&status), sizeof(status));
      break;
    }
    case N64Command::POLL: {
      busy_wait_us(kReplyDelayUs);
      n64_report_t report{};
      build_report(&report);
      joybus_send_bytes(&g_port, reinterpret_cast<uint8_t *>(&report), sizeof(report));
      break;
    }
    default:
      busy_wait_us(kResetWaitUs);
      joybus_port_reset(&g_port);
      break;
  }
}
