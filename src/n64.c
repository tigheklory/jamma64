#include "n64.h"

#include <stdio.h>

#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "pico/time.h"
#include "pico/stdlib.h"

#include "inputs.h"
#include "profile.h"
#include "n64_virtual.h"

#include "n64_io.pio.h"

#ifndef JAMMA64_ENABLE_N64_DIAG
#define JAMMA64_ENABLE_N64_DIAG 0
#endif

#define N64_DIAG_ENABLE JAMMA64_ENABLE_N64_DIAG

#if N64_DIAG_ENABLE
#define N64_DIAG_PRINTF(...) printf(__VA_ARGS__)
#define N64_DIAG_LOG(...) printf(__VA_ARGS__)
#else
#define N64_DIAG_PRINTF(...) ((void)0)
#define N64_DIAG_LOG(...) ((void)0)
#endif

#define N64_P1_DATA_PIN 2
#define N64_DBG_TX_PIN 3

#define N64_PIO pio0
#define N64_SM 0

#define N64_RX_TIMEOUT_US 250000u
// Delay from decoded RX frame to TX start to avoid overlapping host stop-bit high.
#ifndef JAMMA64_RX_TO_TX_DELAY_US
#define JAMMA64_RX_TO_TX_DELAY_US 0
#endif
#define N64_RX_TO_TX_DELAY_US ((uint32_t)JAMMA64_RX_TO_TX_DELAY_US)
#define N64_TEST_BOOTSEL_DEBOUNCE_US 200000u
#define N64_TIMEOUT_LOG_EVERY 32u
#define N64_RECOVERY_TIMEOUT_BURST 64u
#define N64_RECOVERY_FRAME_BURST 12u

// Disable BOOTSEL-driven synthetic button test mode during controller validation.
#define N64_ENABLE_BOOTSEL_TEST 0
#define N64_ID_BITCOUNT 24u
#define N64_REPORT_BITCOUNT 32u
#define N64_ID_PACKED 0x004000A0u

static uint g_io_offset;

static volatile bool g_dbg_cmd_pending;
static volatile uint8_t g_dbg_cmd;
static volatile bool g_dbg_tx_pending;
static volatile uint8_t g_dbg_tx_len;
static volatile uint8_t g_dbg_tx[4];
static volatile bool g_dbg_frame_pending;
static volatile uint16_t g_dbg_frame_raw;

static uint32_t g_poll_log_div;
static uint32_t g_frame_err_count;
static uint32_t g_frame_err_log_div;
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
static volatile uint32_t g_cached_report_packed;
static volatile uint8_t g_cached_report[4];

// Provided by TinyUSB RP2040 board support.
extern bool get_bootsel_button(void);
static void n64_handle_command(uint8_t cmd);
static void n64_rx_irq_handler(void);
static void n64_refresh_cached_report(void);
static uint32_t n64_pack_bits_lsb_first(const uint8_t *data, uint8_t len);
static void n64_send_packed(uint8_t bit_count, uint32_t packed);

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

static bool n64_decode_frame(uint8_t raw8, uint8_t *cmd_out) {
  if (is_known_cmd(raw8)) {
    *cmd_out = raw8;
    return true;
  }
  return false;
}

static inline void n64_handle_raw_frame(uint8_t raw8, uint32_t now) {
  uint8_t cmd = 0;
  g_last_rx_us = now;
  g_timeout_latched = false;

  if (n64_decode_frame(raw8, &cmd)) {
    g_cmd_ok_count++;
#if N64_DIAG_ENABLE
    g_dbg_cmd = cmd;
    g_dbg_cmd_pending = true;
#endif
    if ((g_cmd_ok_count & 0x3Fu) == 0u) {
      N64_DIAG_LOG("N64 RX CMD_OK raw=0x%02X cmd=0x%02X ok=%lu",
                   raw8, cmd, (unsigned long)g_cmd_ok_count);
    }
    if (N64_RX_TO_TX_DELAY_US) busy_wait_us_32(N64_RX_TO_TX_DELAY_US);
    n64_handle_command(cmd);
  } else {
    g_frame_err_count++;
    // RX can observe short misaligned artifacts between valid host frames.
    // Keep counting them for diagnostics, but don't flood serial/log output.
    if ((++g_frame_err_log_div % 64u) == 0u) {
      g_dbg_frame_raw = raw8;
      g_dbg_frame_pending = true;
    }
    // Single-SM pipeline must always be fed a response after each captured frame.
    n64_send_packed(N64_ID_BITCOUNT, N64_ID_PACKED);
  }
}

static void n64_rx_irq_handler(void) {
  while (!pio_sm_is_rx_fifo_empty(N64_PIO, N64_SM)) {
    uint32_t raw = pio_sm_get(N64_PIO, N64_SM);
    uint32_t now = n64_now_us();
    n64_handle_raw_frame((uint8_t)(raw & 0xFFu), now);
  }
}

static void n64_reset_rx_sm(bool log_event) {
  (void)log_event;
  // Single-SM path does not need RX SM resets.
  g_timeout_latched = false;
  g_last_rx_us = 0;
  g_last_recovery_timeout_count = g_timeout_count;
  g_last_recovery_frame_count = g_frame_err_count;
}

static inline uint8_t clamp_analog(bool neg, bool pos, uint8_t mag) {
  if (neg && !pos) return (uint8_t)(256u - mag);
  if (pos && !neg) return mag;
  return 0;
}

static inline bool n64_map_pressed(inputs_t in, n64_out_t out) {
  uint8_t phys = g_profile.map[out];
  if (phys == 0xFFu || phys >= IN_COUNT) return false;
  return inputs_get(in, (phys_in_t)phys);
}

static inline bool n64_out_pressed(inputs_t in, n64_out_t out) {
  return n64_map_pressed(in, out) || n64_virtual_pressed(out);
}

static void n64_update_bootsel_test_mode(void) {
#if !N64_ENABLE_BOOTSEL_TEST
  return;
#else
  uint32_t now = n64_now_us();
  // get_bootsel_button() returns raw CS level (high when not pressed).
  bool pressed = !get_bootsel_button();

  if (pressed && !g_bootsel_last &&
      (uint32_t)(now - g_bootsel_last_change_us) > N64_TEST_BOOTSEL_DEBOUNCE_US) {
    g_test_mode_active = true;
    g_test_step = (uint8_t)((g_test_step + 1u) % 16u);
    g_bootsel_last_change_us = now;
    const char *name = n64_test_step_name(g_test_step);
    N64_DIAG_PRINTF("N64 TEST step=%u (%s)\n", (unsigned)g_test_step, name);
    N64_DIAG_LOG("N64 TEST step=%u (%s)", (unsigned)g_test_step, name);
  }

  g_bootsel_last = pressed;
#endif
}

static void n64_apply_bootsel_test_override(uint8_t out[4]) {
#if !N64_ENABLE_BOOTSEL_TEST
  (void)out;
  return;
#else
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
#endif
}

static void n64_build_p1_report(uint8_t out[4]) {
  inputs_t in = inputs_read();

  uint8_t b0 = 0;
  if (n64_out_pressed(in, N64_A))     b0 |= 0x80;
  if (n64_out_pressed(in, N64_B))     b0 |= 0x40;
  if (n64_out_pressed(in, N64_Z))     b0 |= 0x20;
  if (n64_out_pressed(in, N64_START)) b0 |= 0x10;

  bool vdu = n64_virtual_dpad_pressed(N64_VDPAD_UP);
  bool vdd = n64_virtual_dpad_pressed(N64_VDPAD_DOWN);
  bool vdl = n64_virtual_dpad_pressed(N64_VDPAD_LEFT);
  bool vdr = n64_virtual_dpad_pressed(N64_VDPAD_RIGHT);

  if (g_profile.p1_stick_mode == STICK_MODE_DPAD) {
    if (n64_out_pressed(in, N64_DU) || vdu) b0 |= 0x08;
    if (n64_out_pressed(in, N64_DD) || vdd) b0 |= 0x04;
    if (n64_out_pressed(in, N64_DL) || vdl) b0 |= 0x02;
    if (n64_out_pressed(in, N64_DR) || vdr) b0 |= 0x01;
  } else {
    // Web virtual d-pad is always routed to N64 d-pad bits.
    if (vdu) b0 |= 0x08;
    if (vdd) b0 |= 0x04;
    if (vdl) b0 |= 0x02;
    if (vdr) b0 |= 0x01;
  }

  uint8_t b1 = 0;
  if (n64_out_pressed(in, N64_L))  b1 |= 0x20;
  if (n64_out_pressed(in, N64_R))  b1 |= 0x10;
  if (n64_out_pressed(in, N64_CU)) b1 |= 0x08;
  if (n64_out_pressed(in, N64_CD)) b1 |= 0x04;
  if (n64_out_pressed(in, N64_CL)) b1 |= 0x02;
  if (n64_out_pressed(in, N64_CR)) b1 |= 0x01;

  bool su = n64_virtual_analog_pressed(N64_VANALOG_UP);
  bool sd = n64_virtual_analog_pressed(N64_VANALOG_DOWN);
  bool sl = n64_virtual_analog_pressed(N64_VANALOG_LEFT);
  bool sr = n64_virtual_analog_pressed(N64_VANALOG_RIGHT);
  uint8_t mag = g_profile.analog_throw;

  uint8_t sx = 0;
  uint8_t sy = 0;
  if (g_profile.p1_stick_mode == STICK_MODE_ANALOG) {
    su = su || n64_out_pressed(in, N64_DU);
    sd = sd || n64_out_pressed(in, N64_DD);
    sl = sl || n64_out_pressed(in, N64_DL);
    sr = sr || n64_out_pressed(in, N64_DR);
  }
  sx = clamp_analog(sl, sr, mag);
  sy = clamp_analog(sd, su, mag);

  out[0] = b0;
  out[1] = b1;
  out[2] = sx;
  out[3] = sy;

  n64_apply_bootsel_test_override(out);
}

static void n64_refresh_cached_report(void) {
  uint8_t out[4];
  n64_build_p1_report(out);
  uint32_t packed = n64_pack_bits_lsb_first(out, 4);

  uint32_t ints = save_and_disable_interrupts();
  g_cached_report_packed = packed;
  g_cached_report[0] = out[0];
  g_cached_report[1] = out[1];
  g_cached_report[2] = out[2];
  g_cached_report[3] = out[3];
  restore_interrupts(ints);
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

static void n64_send_packed(uint8_t bit_count, uint32_t packed) {
  if (bit_count == 0 || bit_count > 32) return;

  #if N64_DIAG_ENABLE
  // Optional scope strobe for timing debug; compiled out in normal builds.
  gpio_put(N64_DBG_TX_PIN, 1);
  busy_wait_us_32(2u);
  gpio_put(N64_DBG_TX_PIN, 0);
  #endif

  pio_sm_put_blocking(N64_PIO, N64_SM, (uint32_t)(bit_count - 1u));
  pio_sm_put_blocking(N64_PIO, N64_SM, packed);
}

static void n64_handle_command(uint8_t cmd) {
  if (cmd == 0x00u || cmd == 0xFFu) {
#if N64_DIAG_ENABLE
    const uint8_t id[3] = {0x05, 0x00, 0x02};
    g_dbg_tx_len = 3;
    g_dbg_tx[0] = id[0];
    g_dbg_tx[1] = id[1];
    g_dbg_tx[2] = id[2];
    g_dbg_tx_pending = true;
#endif
    n64_send_packed(N64_ID_BITCOUNT, N64_ID_PACKED);
    return;
  }

  if (cmd == 0x01u) {
    uint32_t packed;
#if N64_DIAG_ENABLE
    uint8_t report[4];
#endif
    uint32_t ints = save_and_disable_interrupts();
    packed = g_cached_report_packed;
#if N64_DIAG_ENABLE
    report[0] = g_cached_report[0];
    report[1] = g_cached_report[1];
    report[2] = g_cached_report[2];
    report[3] = g_cached_report[3];
#endif
    restore_interrupts(ints);

#if N64_DIAG_ENABLE
    g_dbg_tx_len = 4;
    g_dbg_tx[0] = report[0];
    g_dbg_tx[1] = report[1];
    g_dbg_tx[2] = report[2];
    g_dbg_tx[3] = report[3];
    g_dbg_tx_pending = true;
#endif
    n64_send_packed(N64_REPORT_BITCOUNT, packed);
    return;
  }
}

bool n64_init(void) {
#if N64_DIAG_ENABLE
  gpio_init(N64_DBG_TX_PIN);
  gpio_set_dir(N64_DBG_TX_PIN, GPIO_OUT);
  gpio_put(N64_DBG_TX_PIN, 0);
#endif

  gpio_init(N64_P1_DATA_PIN);
  gpio_set_dir(N64_P1_DATA_PIN, GPIO_IN);
  gpio_pull_up(N64_P1_DATA_PIN);

  g_io_offset = pio_add_program(N64_PIO, &n64_io_program);

  pio_sm_config c = n64_io_program_get_default_config(g_io_offset);
  sm_config_set_set_pins(&c, N64_P1_DATA_PIN, 1);
  sm_config_set_in_pins(&c, N64_P1_DATA_PIN);
  sm_config_set_jmp_pin(&c, N64_P1_DATA_PIN);
  sm_config_set_out_shift(&c, true, false, 32);
  sm_config_set_in_shift(&c, false, false, 32);
  sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / 1000000.0f);

  pio_gpio_init(N64_PIO, N64_P1_DATA_PIN);
  pio_sm_init(N64_PIO, N64_SM, g_io_offset, &c);
  pio_sm_set_pins_with_mask(N64_PIO, N64_SM, 0u, 1u << N64_P1_DATA_PIN);
  pio_sm_set_pindirs_with_mask(N64_PIO, N64_SM, 0u, 1u << N64_P1_DATA_PIN);
  pio_sm_clear_fifos(N64_PIO, N64_SM);
  pio_sm_restart(N64_PIO, N64_SM);
  pio_sm_set_enabled(N64_PIO, N64_SM, true);

  // Drive RX handling from PIO IRQ for deterministic turnaround.
  irq_set_exclusive_handler(PIO0_IRQ_0, n64_rx_irq_handler);
  pio_set_irq0_source_enabled(N64_PIO, pis_sm0_rx_fifo_not_empty, true);
  irq_set_priority(PIO0_IRQ_0, 0x00);
  irq_set_enabled(PIO0_IRQ_0, true);

  n64_refresh_cached_report();

  return true;
}

void n64_task(void) {
  uint32_t now = n64_now_us();
  n64_refresh_cached_report();
  n64_update_bootsel_test_mode();

  if (g_last_rx_us != 0u && !g_timeout_latched && (now - g_last_rx_us) > N64_RX_TIMEOUT_US) {
    g_timeout_count++;
    g_timeout_latched = true;
    if (g_timeout_count <= 4u || (g_timeout_count % N64_TIMEOUT_LOG_EVERY) == 0u) {
      N64_DIAG_LOG("N64 RX TIMEOUT cnt=%lu", (unsigned long)g_timeout_count);
    }
  }

  bool timeout_burst = (g_timeout_count - g_last_recovery_timeout_count) >= N64_RECOVERY_TIMEOUT_BURST;
  bool frame_burst = (g_frame_err_count - g_last_recovery_frame_count) >= N64_RECOVERY_FRAME_BURST;
  if (timeout_burst || frame_burst) {
    n64_reset_rx_sm(true);
  }

  // Temporary heartbeat for field debugging:
  // lets us distinguish "no console traffic" from "decode failure".
  if ((uint32_t)(now - g_last_diag_log_us) > 2000000u) {
    g_last_diag_log_us = now;
    N64_DIAG_LOG("N64 DIAG ok=%lu frame_err=%lu timeout=%lu tx=%u",
                 (unsigned long)g_cmd_ok_count,
                 (unsigned long)g_frame_err_count,
                 (unsigned long)g_timeout_count,
                 0u);
  }

  if (g_dbg_cmd_pending) {
    uint8_t cmd;
    uint32_t ints = save_and_disable_interrupts();
    cmd = g_dbg_cmd;
    g_dbg_cmd_pending = false;
    restore_interrupts(ints);

    bool log_poll = (cmd != 0x01u) || ((++g_poll_log_div % 32u) == 0u);
    if (log_poll) {
      N64_DIAG_PRINTF("N64 RX CMD: 0x%02X\n", cmd);
      N64_DIAG_LOG("N64 RX CMD: 0x%02X", cmd);
    }
    if ((g_cmd_ok_count % 64u) == 0u) {
      N64_DIAG_LOG("N64 RX STATS ok=%lu frame_err=%lu timeout=%lu",
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

    if (n == 3) {
      N64_DIAG_PRINTF("N64 TX: %02X %02X %02X\n", b[0], b[1], b[2]);
      N64_DIAG_LOG("N64 TX: %02X %02X %02X", b[0], b[1], b[2]);
    } else if (n == 4) {
      if ((g_poll_log_div % 32u) == 0u) {
        N64_DIAG_PRINTF("N64 TX: %02X %02X %02X %02X\n", b[0], b[1], b[2], b[3]);
        N64_DIAG_LOG("N64 TX: %02X %02X %02X %02X", b[0], b[1], b[2], b[3]);
      }
    } else {
      N64_DIAG_PRINTF("N64 TX:");
      for (uint8_t i = 0; i < n; i++) N64_DIAG_PRINTF(" %02X", b[i]);
      N64_DIAG_PRINTF("\n");
    }
  }

  if (g_dbg_frame_pending) {
    uint16_t raw9;
    uint32_t ints = save_and_disable_interrupts();
    raw9 = g_dbg_frame_raw;
    g_dbg_frame_pending = false;
    restore_interrupts(ints);
    N64_DIAG_PRINTF("N64 RX FRAME_ERR raw=0x%02X\n", (unsigned)(raw9 & 0xFFu));
    N64_DIAG_LOG("N64 RX FRAME_ERR raw=0x%02X cnt=%lu",
                 (unsigned)(raw9 & 0xFFu), (unsigned long)g_frame_err_count);
  }
}
