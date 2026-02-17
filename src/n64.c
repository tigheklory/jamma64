#include "n64.h"

#include <stdio.h>

#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "pico/time.h"
#include "pico/stdlib.h"

#include "inputs.h"
#include "profile.h"
#include "event_log.h"

#include "n64_tx.pio.h"
#include "n64_rx.pio.h"

#define N64_P1_DATA_PIN 2

#define N64_PIO pio0
#define N64_TX_SM 0
#define N64_RX_SM 1

#define N64_TX_GUARD_US 8u
#define N64_RX_TIMEOUT_US 250000u
// Delay from decoded RX frame to TX start to avoid overlapping host stop-bit high.
#define N64_RX_TO_TX_DELAY_US 2u
#define N64_TEST_BOOTSEL_DEBOUNCE_US 200000u
#define N64_TIMEOUT_LOG_EVERY 32u
#define N64_RECOVERY_TIMEOUT_BURST 64u
#define N64_RECOVERY_FRAME_BURST 12u

static uint g_tx_offset;
static uint g_rx_offset;

static volatile bool g_tx_active;
static volatile uint32_t g_tx_done_us;

static volatile bool g_dbg_cmd_pending;
static volatile uint8_t g_dbg_cmd;
static volatile bool g_dbg_tx_pending;
static volatile uint8_t g_dbg_tx_len;
static volatile uint8_t g_dbg_tx[4];
static volatile bool g_dbg_frame_pending;
static volatile uint16_t g_dbg_frame_raw;

static uint32_t g_poll_log_div;
static uint32_t g_frame_err_count;
static uint32_t g_timeout_count;
static uint32_t g_cmd_ok_count;
static uint32_t g_last_rx_us;
static bool g_timeout_latched;
static uint32_t g_last_diag_log_us;
static uint32_t g_last_recovery_timeout_count;
static uint32_t g_last_recovery_frame_count;
static bool g_bootsel_last;
static uint32_t g_bootsel_last_change_us;
static uint8_t g_test_step;
static bool g_test_mode_active;

// Provided by TinyUSB RP2040 board support.
extern bool get_bootsel_button(void);
static void n64_handle_command(uint8_t cmd);

static const char *n64_test_step_name(uint8_t step) {
  static const char *k_names[16] = {
    "Neutral",
    "A",
    "B",
    "Z",
    "Start",
    "D-Up",
    "D-Down",
    "D-Left",
    "D-Right",
    "L",
    "R",
    "C-Up",
    "Stick Up",
    "Stick Down",
    "Stick Left",
    "Stick Right"
  };
  return k_names[step & 0x0Fu];
}

static inline uint32_t n64_now_us(void) {
  return time_us_32();
}

static inline bool is_known_cmd(uint8_t cmd) {
  return (cmd == 0x00u || cmd == 0x01u || cmd == 0xFFu);
}

static bool n64_decode_frame(uint16_t raw9, uint8_t *cmd_out) {
  // Try only non-bit-reversed packings.
  // Stop bit must match the same bit alignment as the decoded command byte.
  uint8_t d0 = (uint8_t)(raw9 & 0xFFu);
  uint8_t s0 = (uint8_t)((raw9 >> 8) & 1u);
  uint8_t d1 = (uint8_t)((raw9 >> 1) & 0xFFu);
  uint8_t s1 = (uint8_t)(raw9 & 1u);

  // Alignment 0: d0 + s0
  if (s0 == 1u && is_known_cmd(d0)) {
    *cmd_out = d0;
    return true;
  }

  // Alignment 1: d1 + s1
  if (s1 == 1u && is_known_cmd(d1)) {
    *cmd_out = d1;
    return true;
  }

  return false;
}

static inline void n64_handle_raw_frame(uint16_t raw9, uint32_t now) {
  uint8_t cmd = 0;
  g_last_rx_us = now;
  g_timeout_latched = false;

  if (n64_decode_frame(raw9, &cmd)) {
    g_cmd_ok_count++;
    g_dbg_cmd = cmd;
    g_dbg_cmd_pending = true;
    if ((g_cmd_ok_count & 0x3Fu) == 0u) {
      event_log_appendf("N64 RX CMD_OK raw=0x%03X cmd=0x%02X ok=%lu",
                        raw9, cmd, (unsigned long)g_cmd_ok_count);
    }
    if (N64_RX_TO_TX_DELAY_US) busy_wait_us_32(N64_RX_TO_TX_DELAY_US);
    n64_handle_command(cmd);
  } else {
    g_frame_err_count++;
    g_dbg_frame_raw = raw9;
    g_dbg_frame_pending = true;
  }
}

static void n64_reset_rx_sm(bool log_event) {
  pio_sm_set_enabled(N64_PIO, N64_RX_SM, false);
  pio_sm_clear_fifos(N64_PIO, N64_RX_SM);
  pio_sm_restart(N64_PIO, N64_RX_SM);
  pio_sm_set_enabled(N64_PIO, N64_RX_SM, true);

  g_timeout_latched = false;
  g_last_rx_us = 0;
  g_last_recovery_timeout_count = g_timeout_count;
  g_last_recovery_frame_count = g_frame_err_count;

  if (log_event) {
    event_log_appendf("N64 RX RECOVER ok=%lu frame_err=%lu timeout=%lu",
                      (unsigned long)g_cmd_ok_count,
                      (unsigned long)g_frame_err_count,
                      (unsigned long)g_timeout_count);
  }
}

static inline uint8_t clamp_analog(bool neg, bool pos, uint8_t mag) {
  if (neg && !pos) return (uint8_t)(256u - mag);
  if (pos && !neg) return mag;
  return 0;
}

static void n64_update_bootsel_test_mode(void) {
  uint32_t now = n64_now_us();
  // get_bootsel_button() returns raw CS level (high when not pressed).
  bool pressed = !get_bootsel_button();

  if (pressed && !g_bootsel_last &&
      (uint32_t)(now - g_bootsel_last_change_us) > N64_TEST_BOOTSEL_DEBOUNCE_US) {
    g_test_mode_active = true;
    g_test_step = (uint8_t)((g_test_step + 1u) % 16u);
    g_bootsel_last_change_us = now;
    const char *name = n64_test_step_name(g_test_step);
    printf("N64 TEST step=%u (%s)\n", (unsigned)g_test_step, name);
    event_log_appendf("N64 TEST step=%u (%s)", (unsigned)g_test_step, name);
  }

  g_bootsel_last = pressed;
}

static void n64_apply_bootsel_test_override(uint8_t out[4]) {
  if (!g_test_mode_active) return;

  out[0] = 0;
  out[1] = 0;
  out[2] = 0;
  out[3] = 0;

  switch (g_test_step) {
    case 0:  break;              // neutral
    case 1:  out[0] |= 0x80; break; // A
    case 2:  out[0] |= 0x40; break; // B
    case 3:  out[0] |= 0x20; break; // Z
    case 4:  out[0] |= 0x10; break; // Start
    case 5:  out[0] |= 0x08; break; // D-Up
    case 6:  out[0] |= 0x04; break; // D-Down
    case 7:  out[0] |= 0x02; break; // D-Left
    case 8:  out[0] |= 0x01; break; // D-Right
    case 9:  out[1] |= 0x20; break; // L
    case 10: out[1] |= 0x10; break; // R
    case 11: out[1] |= 0x08; break; // C-Up
    case 12: out[3] = 80; break;    // Stick Up
    case 13: out[3] = (uint8_t)(256u - 80u); break; // Stick Down
    case 14: out[2] = (uint8_t)(256u - 80u); break; // Stick Left
    case 15: out[2] = 80; break;    // Stick Right
    default: break;
  }
}

static void n64_build_p1_report(uint8_t out[4]) {
  inputs_t in = inputs_read();

  uint8_t b0 = 0;
  if (inputs_get(in, IN_P1_B1))    b0 |= 0x80;
  if (inputs_get(in, IN_P1_B2))    b0 |= 0x40;
  if (inputs_get(in, IN_P1_B3))    b0 |= 0x20;
  if (inputs_get(in, IN_P1_START)) b0 |= 0x10;

  if (g_profile.p1_stick_mode == STICK_MODE_DPAD) {
    if (inputs_get(in, IN_P1_UP))    b0 |= 0x08;
    if (inputs_get(in, IN_P1_DOWN))  b0 |= 0x04;
    if (inputs_get(in, IN_P1_LEFT))  b0 |= 0x02;
    if (inputs_get(in, IN_P1_RIGHT)) b0 |= 0x01;
  }

  uint8_t b1 = 0;
  if (inputs_get(in, IN_P1_B5)) b1 |= 0x20;
  if (inputs_get(in, IN_P1_B6)) b1 |= 0x10;
  if (inputs_get(in, IN_P1_B4)) b1 |= 0x08;

  uint8_t throw_mag = g_profile.analog_throw;
  if (throw_mag > 127) throw_mag = 127;

  uint8_t sx = 0;
  uint8_t sy = 0;
  if (g_profile.p1_stick_mode == STICK_MODE_ANALOG) {
    sx = clamp_analog(inputs_get(in, IN_P1_LEFT), inputs_get(in, IN_P1_RIGHT), throw_mag);
    sy = clamp_analog(inputs_get(in, IN_P1_DOWN), inputs_get(in, IN_P1_UP), throw_mag);
  }

  out[0] = b0;
  out[1] = b1;
  out[2] = sx;
  out[3] = sy;

  n64_apply_bootsel_test_override(out);
}

static uint32_t n64_pack_bits_lsb_first(const uint8_t *data, uint8_t len) {
  uint32_t packed = 0;
  uint8_t out_pos = 0;
  for (uint8_t i = 0; i < len; i++) {
    for (int b = 7; b >= 0; b--) {
      uint32_t bit = (data[i] >> b) & 1u;
      packed |= (bit << out_pos);
      out_pos++;
    }
  }
  return packed;
}

static void n64_send_bytes(const uint8_t *data, uint8_t len) {
  uint8_t bit_count = (uint8_t)(len * 8u);
  if (bit_count == 0 || bit_count > 32) return;

  uint32_t packed = n64_pack_bits_lsb_first(data, len);
  uint32_t now = n64_now_us();

  g_tx_active = true;
  g_tx_done_us = now + ((uint32_t)bit_count * 4u) + 4u + N64_TX_GUARD_US;

  // Gate RX during TX to avoid self-decoding from our own output transitions.
  pio_sm_set_enabled(N64_PIO, N64_RX_SM, false);

  pio_sm_put_blocking(N64_PIO, N64_TX_SM, (uint32_t)(bit_count - 1u));
  pio_sm_put_blocking(N64_PIO, N64_TX_SM, packed);
}

static void n64_handle_command(uint8_t cmd) {
  if (cmd == 0x00u || cmd == 0xFFu) {
    // Standard controller identity.
    const uint8_t id[3] = {0x05, 0x00, 0x01};
    g_dbg_tx_len = 3;
    g_dbg_tx[0] = id[0];
    g_dbg_tx[1] = id[1];
    g_dbg_tx[2] = id[2];
    g_dbg_tx_pending = true;
    n64_send_bytes(id, 3);
    return;
  }

  if (cmd == 0x01u) {
    uint8_t report[4];
    n64_build_p1_report(report);
    g_dbg_tx_len = 4;
    g_dbg_tx[0] = report[0];
    g_dbg_tx[1] = report[1];
    g_dbg_tx[2] = report[2];
    g_dbg_tx[3] = report[3];
    g_dbg_tx_pending = true;
    n64_send_bytes(report, 4);
    return;
  }
}

bool n64_init(void) {
  gpio_init(N64_P1_DATA_PIN);
  gpio_set_dir(N64_P1_DATA_PIN, GPIO_IN);
  gpio_pull_up(N64_P1_DATA_PIN);

  g_tx_offset = pio_add_program(N64_PIO, &n64_tx_program);
  g_rx_offset = pio_add_program(N64_PIO, &n64_rx_program);

  // TX SM
  pio_sm_config txc = n64_tx_program_get_default_config(g_tx_offset);
  sm_config_set_set_pins(&txc, N64_P1_DATA_PIN, 1);
  sm_config_set_out_shift(&txc, true, false, 32);
  sm_config_set_fifo_join(&txc, PIO_FIFO_JOIN_TX);
  sm_config_set_clkdiv(&txc, (float)clock_get_hz(clk_sys) / 1000000.0f);

  pio_gpio_init(N64_PIO, N64_P1_DATA_PIN);
  pio_sm_init(N64_PIO, N64_TX_SM, g_tx_offset, &txc);
  pio_sm_set_pins_with_mask(N64_PIO, N64_TX_SM, 0u, 1u << N64_P1_DATA_PIN);
  pio_sm_set_pindirs_with_mask(N64_PIO, N64_TX_SM, 0u, 1u << N64_P1_DATA_PIN);
  pio_sm_set_enabled(N64_PIO, N64_TX_SM, true);

  // RX SM
  pio_sm_config rxc = n64_rx_program_get_default_config(g_rx_offset);
  sm_config_set_in_pins(&rxc, N64_P1_DATA_PIN);
  sm_config_set_jmp_pin(&rxc, N64_P1_DATA_PIN);
  sm_config_set_in_shift(&rxc, false, false, 32);
  sm_config_set_clkdiv(&rxc, (float)clock_get_hz(clk_sys) / 2000000.0f);

  pio_sm_init(N64_PIO, N64_RX_SM, g_rx_offset, &rxc);
  pio_sm_clear_fifos(N64_PIO, N64_RX_SM);
  pio_sm_restart(N64_PIO, N64_RX_SM);
  pio_sm_set_enabled(N64_PIO, N64_RX_SM, true);

  return true;
}

void n64_task(void) {
  uint32_t now = n64_now_us();
  n64_update_bootsel_test_mode();

  if (g_tx_active && (int32_t)(now - g_tx_done_us) >= 0) {
    g_tx_active = false;
    n64_reset_rx_sm(false);
  }

  // Fallback drain path (if IRQ delivery is delayed/disabled for any reason).
  while (!g_tx_active && !pio_sm_is_rx_fifo_empty(N64_PIO, N64_RX_SM)) {
    uint32_t raw = pio_sm_get(N64_PIO, N64_RX_SM);
    n64_handle_raw_frame((uint16_t)(raw & 0x1FFu), now);
  }

  if (g_last_rx_us != 0u && !g_timeout_latched && (now - g_last_rx_us) > N64_RX_TIMEOUT_US) {
    g_timeout_count++;
    g_timeout_latched = true;
    if (g_timeout_count <= 4u || (g_timeout_count % N64_TIMEOUT_LOG_EVERY) == 0u) {
      event_log_appendf("N64 RX TIMEOUT cnt=%lu", (unsigned long)g_timeout_count);
    }
  }

  if (!g_tx_active) {
    bool timeout_burst = (g_timeout_count - g_last_recovery_timeout_count) >= N64_RECOVERY_TIMEOUT_BURST;
    bool frame_burst = (g_frame_err_count - g_last_recovery_frame_count) >= N64_RECOVERY_FRAME_BURST;
    if (timeout_burst || frame_burst) {
      n64_reset_rx_sm(true);
    }
  }

  // Temporary heartbeat for field debugging:
  // lets us distinguish "no console traffic" from "decode failure".
  if ((uint32_t)(now - g_last_diag_log_us) > 2000000u) {
    g_last_diag_log_us = now;
    event_log_appendf("N64 DIAG ok=%lu frame_err=%lu timeout=%lu tx=%u",
                      (unsigned long)g_cmd_ok_count,
                      (unsigned long)g_frame_err_count,
                      (unsigned long)g_timeout_count,
                      g_tx_active ? 1u : 0u);
  }

  if (g_dbg_cmd_pending) {
    uint8_t cmd;
    uint32_t ints = save_and_disable_interrupts();
    cmd = g_dbg_cmd;
    g_dbg_cmd_pending = false;
    restore_interrupts(ints);

    printf("N64 RX CMD: 0x%02X\n", cmd);
    if (cmd != 0x01u || (++g_poll_log_div % 32u) == 0u) {
      event_log_appendf("N64 RX CMD: 0x%02X", cmd);
    }
    if ((g_cmd_ok_count % 64u) == 0u) {
      event_log_appendf("N64 RX STATS ok=%lu frame_err=%lu timeout=%lu",
                        (unsigned long)g_cmd_ok_count,
                        (unsigned long)g_frame_err_count,
                        (unsigned long)g_timeout_count);
    }
  }

  if (g_dbg_tx_pending) {
    uint8_t b[4];
    uint8_t n;
    uint32_t ints = save_and_disable_interrupts();
    n = g_dbg_tx_len;
    if (n > 4) n = 4;
    for (uint8_t i = 0; i < n; i++) b[i] = g_dbg_tx[i];
    g_dbg_tx_pending = false;
    restore_interrupts(ints);

    printf("N64 TX:");
    for (uint8_t i = 0; i < n; i++) printf(" %02X", b[i]);
    printf("\n");

    if (n == 3) {
      event_log_appendf("N64 TX: %02X %02X %02X", b[0], b[1], b[2]);
    } else if (n == 4) {
      if ((g_poll_log_div % 32u) == 0u) {
        event_log_appendf("N64 TX: %02X %02X %02X %02X", b[0], b[1], b[2], b[3]);
      }
    }
  }

  if (g_dbg_frame_pending) {
    uint16_t raw9;
    uint32_t ints = save_and_disable_interrupts();
    raw9 = g_dbg_frame_raw;
    g_dbg_frame_pending = false;
    restore_interrupts(ints);
    printf("N64 RX FRAME_ERR raw=0x%03X\n", raw9);
    event_log_appendf("N64 RX FRAME_ERR raw=0x%03X cnt=%lu", raw9, (unsigned long)g_frame_err_count);
  }
}
