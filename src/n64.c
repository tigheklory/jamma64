#include "n64.h"

#include <stdio.h>

#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "pico/time.h"

#include "inputs.h"
#include "profile.h"
#include "event_log.h"

#include "n64_tx.pio.h"

// Player 1 N64 data pin (3-pin port data line).
#define N64_P1_DATA_PIN 2

// PIO resources used for TX timing.
#define N64_PIO pio0
#define N64_SM  0

// RX frame timeout (high gap that ends a command frame).
#define N64_CMD_GAP_US 24u
// N64 bit cell is ~4us (1us/3us split). Add guard margin for TX complete.
#define N64_TX_GUARD_US 8u
// Wait for host to finish the current bit cell, then a short turnaround.
#define N64_TX_TURNAROUND_US 1u
// Command decode low-width thresholds (us) with jitter tolerance.
#define N64_LOW_ONE_MAX_US 2u
#define N64_LOW_ZERO_MIN_US 3u
#define N64_LOW_ZERO_MAX_US 5u

typedef struct {
  volatile bool receiving;
  volatile uint32_t last_fall_us;
  volatile uint32_t last_edge_us;
  volatile uint8_t cmd;
  volatile uint8_t data_bits;
} n64_rx_state_t;

static n64_rx_state_t g_rx;
static uint g_pio_offset;
static volatile bool g_tx_active;
static volatile uint32_t g_tx_done_us;
static volatile bool g_dbg_cmd_pending;
static volatile uint8_t g_dbg_cmd;
static volatile bool g_dbg_tx_pending;
static volatile uint8_t g_dbg_tx_len;
static volatile uint8_t g_dbg_tx[4];
static volatile bool g_dbg_invalid_pending;
static volatile uint8_t g_dbg_invalid_low_us;
static uint32_t g_poll_log_div;

static inline uint32_t n64_now_us(void) {
  return time_us_32();
}

static inline uint8_t clamp_analog(bool neg, bool pos, uint8_t mag) {
  if (neg && !pos) return (uint8_t)(256u - mag); // two's-complement negative
  if (pos && !neg) return mag;
  return 0;
}

static void n64_build_p1_report(uint8_t out[4]) {
  inputs_t in = inputs_read();

  // Byte 0: A B Z Start DUp DDown DLeft DRight
  uint8_t b0 = 0;
  if (inputs_get(in, IN_P1_B1))    b0 |= 0x80; // A
  if (inputs_get(in, IN_P1_B2))    b0 |= 0x40; // B
  if (inputs_get(in, IN_P1_B3))    b0 |= 0x20; // Z
  if (inputs_get(in, IN_P1_START)) b0 |= 0x10; // Start

  if (g_profile.p1_stick_mode == STICK_MODE_DPAD) {
    if (inputs_get(in, IN_P1_UP))    b0 |= 0x08;
    if (inputs_get(in, IN_P1_DOWN))  b0 |= 0x04;
    if (inputs_get(in, IN_P1_LEFT))  b0 |= 0x02;
    if (inputs_get(in, IN_P1_RIGHT)) b0 |= 0x01;
  }

  // Byte 1: 0 0 L R CUp CDown CLeft CRight
  uint8_t b1 = 0;
  if (inputs_get(in, IN_P1_B5)) b1 |= 0x20; // L
  if (inputs_get(in, IN_P1_B6)) b1 |= 0x10; // R
  if (inputs_get(in, IN_P1_B4)) b1 |= 0x08; // C-Up (temporary fixed mapping)

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
}

// Pack bytes MSB-first into a 32-bit word consumed LSB-first by the PIO program.
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

  // Mark TX active before feeding PIO so RX IRQ can ignore self-generated edges.
  g_tx_active = true;
  g_tx_done_us = now + ((uint32_t)bit_count * 4u) + 4u + N64_TX_GUARD_US;

  // Put count then payload for PIO frame send (PIO runs with 1us instruction timing).
  pio_sm_put_blocking(N64_PIO, N64_SM, (uint32_t)(bit_count - 1u));
  pio_sm_put_blocking(N64_PIO, N64_SM, packed);
}

static void n64_handle_command(uint8_t cmd) {
  // 0x00 and 0xFF are commonly used identify/status probes.
  if (cmd == 0x00 || cmd == 0xFF) {
    const uint8_t id[3] = {0x05, 0x00, 0x01};
    g_dbg_tx_len = 3;
    g_dbg_tx[0] = id[0];
    g_dbg_tx[1] = id[1];
    g_dbg_tx[2] = id[2];
    g_dbg_tx_pending = true;
    n64_send_bytes(id, 3);
    return;
  }

  // 0x01 polls controller state.
  if (cmd == 0x01) {
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

  // Other commands are ignored for now (mempak/rumble not implemented yet).
}

static void n64_gpio_irq(uint gpio, uint32_t events) {
  if (gpio != N64_P1_DATA_PIN) return;
  if (g_tx_active) return;

  uint32_t now = n64_now_us();

  if (events & GPIO_IRQ_EDGE_FALL) {
    if (!g_rx.receiving) {
      g_rx.receiving = true;
      g_rx.cmd = 0;
      g_rx.data_bits = 0;
    }
    g_rx.last_fall_us = now;
    g_rx.last_edge_us = now;
  }

  if (events & GPIO_IRQ_EDGE_RISE) {
    if (!g_rx.receiving) return;

    uint32_t low_us = now - g_rx.last_fall_us;
    bool sym_is_one;
    bool sym_valid = true;
    if (low_us < N64_LOW_ONE_MAX_US) {
      sym_is_one = true;
    } else if (low_us >= N64_LOW_ZERO_MIN_US && low_us <= N64_LOW_ZERO_MAX_US) {
      sym_is_one = false;
    } else {
      sym_valid = false;
      sym_is_one = false;
    }

    if (!sym_valid) {
      g_rx.receiving = false;
      g_rx.cmd = 0;
      g_rx.data_bits = 0;
      g_dbg_invalid_low_us = (uint8_t)((low_us > 255u) ? 255u : low_us);
      g_dbg_invalid_pending = true;
      return;
    }

    if (g_rx.data_bits < 8u) {
      g_rx.cmd = (uint8_t)((g_rx.cmd << 1) | (sym_is_one ? 1u : 0u));
      g_rx.data_bits++;
      g_rx.last_edge_us = now;
      if (g_rx.data_bits == 8u) {
        uint8_t cmd = g_rx.cmd;
        g_rx.receiving = false;
        g_dbg_cmd = cmd;
        g_dbg_cmd_pending = true;

        // Wait until current command bit high phase has completed.
        uint32_t remain_high_us = sym_is_one ? 3u : 1u;
        busy_wait_us_32(remain_high_us + N64_TX_TURNAROUND_US);
        n64_handle_command(cmd);
        g_rx.cmd = 0;
        g_rx.data_bits = 0;
      }
      return;
    }
  }
}

bool n64_init(void) {
  // Keep output latch low so enabling output drives low (open-drain behavior).
  gpio_init(N64_P1_DATA_PIN);
  gpio_set_dir(N64_P1_DATA_PIN, GPIO_IN);
  gpio_put(N64_P1_DATA_PIN, 0);
  gpio_pull_up(N64_P1_DATA_PIN);

  gpio_set_irq_enabled_with_callback(N64_P1_DATA_PIN,
                                     GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE,
                                     true,
                                     &n64_gpio_irq);

  g_pio_offset = pio_add_program(N64_PIO, &n64_tx_program);
  pio_sm_config c = n64_tx_program_get_default_config(g_pio_offset);

  sm_config_set_set_pins(&c, N64_P1_DATA_PIN, 1);
  sm_config_set_out_shift(&c, true, false, 32);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);

  // 1 MHz SM clock => 1us instruction timing.
  float div = (float)clock_get_hz(clk_sys) / 1000000.0f;
  sm_config_set_clkdiv(&c, div);

  pio_gpio_init(N64_PIO, N64_P1_DATA_PIN);
  pio_sm_init(N64_PIO, N64_SM, g_pio_offset, &c);

  // Initialize output low latch and released line.
  pio_sm_set_pins_with_mask(N64_PIO, N64_SM, 0u, 1u << N64_P1_DATA_PIN);
  pio_sm_set_pindirs_with_mask(N64_PIO, N64_SM, 0u, 1u << N64_P1_DATA_PIN);

  pio_sm_set_enabled(N64_PIO, N64_SM, true);
  return true;
}

void n64_task(void) {
  uint32_t now = n64_now_us();

  // Release RX gating once PIO TX frame is guaranteed complete.
  if (g_tx_active && (int32_t)(now - g_tx_done_us) >= 0) {
    g_tx_active = false;
    g_rx.receiving = false;
    g_rx.cmd = 0;
    g_rx.data_bits = 0;
  }

  // Fallback cleanup for malformed/partial commands.
  if (g_rx.receiving && ((now - g_rx.last_edge_us) > N64_CMD_GAP_US)) {
    g_rx.receiving = false;
    g_rx.cmd = 0;
    g_rx.data_bits = 0;
  }

  // Print debug outside IRQ context.
  if (g_dbg_cmd_pending) {
    uint8_t cmd;
    uint32_t ints = save_and_disable_interrupts();
    cmd = g_dbg_cmd;
    g_dbg_cmd_pending = false;
    restore_interrupts(ints);
    printf("N64 RX CMD: 0x%02X\n", cmd);
    if (cmd != 0x01 || (++g_poll_log_div % 32u) == 0u) {
      event_log_appendf("N64 RX CMD: 0x%02X", cmd);
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

    if (n == 0) {
      printf("N64 TX: <none>\n");
      event_log_appendf("N64 TX: <none>");
    } else {
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
  }

  if (g_dbg_invalid_pending) {
    uint8_t low_us;
    uint32_t ints = save_and_disable_interrupts();
    low_us = g_dbg_invalid_low_us;
    g_dbg_invalid_pending = false;
    restore_interrupts(ints);
    printf("N64 RX INVALID: low=%uus\n", low_us);
    event_log_appendf("N64 RX INVALID: low=%uus", low_us);
  }
}
