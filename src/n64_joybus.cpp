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

static inline uint8_t clamp_u8_range(uint8_t v, uint8_t lo, uint8_t hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static inline uint8_t clamp_analog_diagonal_safe(
    bool neg, bool pos, uint8_t mag, bool diagonal, uint8_t diagonal_scale_pct) {
  if (neg == pos) return 0;

  uint16_t axis = mag;
  if (diagonal) {
    // 1/sqrt(2) ~= 181/256 keeps diagonal vector magnitude at or below throw.
    axis = static_cast<uint16_t>((axis * 181u + 128u) >> 8);
    axis = static_cast<uint16_t>((axis * diagonal_scale_pct + 50u) / 100u);
  }
  if (axis > 127u) axis = 127u;
  return neg ? static_cast<uint8_t>(256u - axis) : static_cast<uint8_t>(axis);
}

static inline bool n64_map_pressed(inputs_t in, n64_out_t out) {
  uint8_t phys = g_profile.map[out];
  if (phys == 0xFFu || phys >= IN_COUNT) return false;
  return inputs_get(in, static_cast<phys_in_t>(phys));
}

static inline bool n64_out_pressed(inputs_t in, n64_out_t out) {
  return n64_map_pressed(in, out) || n64_virtual_pressed(out);
}

static inline bool is_p1_joystick_phys(uint8_t phys) {
  return phys == IN_P1_UP || phys == IN_P1_DOWN || phys == IN_P1_LEFT || phys == IN_P1_RIGHT;
}

static inline bool n64_dir_pressed_nonjoy(inputs_t in, n64_out_t out) {
  uint8_t phys = g_profile.map[out];
  if (phys == 0xFFu || phys >= IN_COUNT) return false;
  if (is_p1_joystick_phys(phys)) return false;
  return inputs_get(in, static_cast<phys_in_t>(phys));
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

  bool joy_up = inputs_get(in, IN_P1_UP);
  bool joy_down = inputs_get(in, IN_P1_DOWN);
  bool joy_left = inputs_get(in, IN_P1_LEFT);
  bool joy_right = inputs_get(in, IN_P1_RIGHT);

  report->dpad_up = n64_dir_pressed_nonjoy(in, N64_DU) || vdu;
  report->dpad_down = n64_dir_pressed_nonjoy(in, N64_DD) || vdd;
  report->dpad_left = n64_dir_pressed_nonjoy(in, N64_DL) || vdl;
  report->dpad_right = n64_dir_pressed_nonjoy(in, N64_DR) || vdr;

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
  su = su || n64_dir_pressed_nonjoy(in, N64_AU);
  sd = sd || n64_dir_pressed_nonjoy(in, N64_AD);
  sl = sl || n64_dir_pressed_nonjoy(in, N64_AL);
  sr = sr || n64_dir_pressed_nonjoy(in, N64_AR);

  if (g_profile.p1_stick_mode == STICK_MODE_DPAD) {
    report->dpad_up = report->dpad_up || joy_up;
    report->dpad_down = report->dpad_down || joy_down;
    report->dpad_left = report->dpad_left || joy_left;
    report->dpad_right = report->dpad_right || joy_right;
  } else {
    su = su || joy_up;
    sd = sd || joy_down;
    sl = sl || joy_left;
    sr = sr || joy_right;
  }
  uint8_t mag = g_profile.analog_throw;
  uint8_t diag_scale = clamp_u8_range(g_profile.diagonal_scale_pct, 70u, 100u);
  bool h_active = (sl != sr);
  bool v_active = (sd != su);
  bool diagonal = h_active && v_active;
  sx = clamp_analog_diagonal_safe(sl, sr, mag, diagonal, diag_scale);
  sy = clamp_analog_diagonal_safe(sd, su, mag, diagonal, diag_scale);

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
