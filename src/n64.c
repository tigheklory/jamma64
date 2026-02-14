#include "n64.h"

#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "pico/time.h"

#include "inputs.h"
#include "profile.h"

#include "n64_tx.pio.h"

// Player 1 N64 data pin (3-pin port data line).
#define N64_P1_DATA_PIN 2

// PIO resources used for TX timing.
#define N64_PIO pio0
#define N64_SM  0

// RX frame timeout (high gap that ends a command frame).
#define N64_CMD_GAP_US 24u

typedef struct {
  volatile bool receiving;
  volatile uint32_t last_fall_us;
  volatile uint32_t last_edge_us;
  volatile uint32_t bits;
  volatile uint8_t bit_count;
  volatile bool frame_ready;
  volatile uint32_t frame_bits;
  volatile uint8_t frame_len;
} n64_rx_state_t;

static n64_rx_state_t g_rx;
static uint g_pio_offset;

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

  // Put count then payload for PIO frame send.
  pio_sm_put_blocking(N64_PIO, N64_SM, (uint32_t)(bit_count - 1u));
  pio_sm_put_blocking(N64_PIO, N64_SM, packed);
}

static void n64_handle_command(uint8_t cmd, uint32_t raw_bits, uint8_t raw_len) {
  (void)raw_bits;
  (void)raw_len;

  // 0x00 and 0xFF are commonly used identify/status probes.
  if (cmd == 0x00 || cmd == 0xFF) {
    const uint8_t id[3] = {0x05, 0x00, 0x01};
    n64_send_bytes(id, 3);
    return;
  }

  // 0x01 polls controller state.
  if (cmd == 0x01) {
    uint8_t report[4];
    n64_build_p1_report(report);
    n64_send_bytes(report, 4);
    return;
  }

  // Other commands are ignored for now (mempak/rumble not implemented yet).
}

static void n64_gpio_irq(uint gpio, uint32_t events) {
  if (gpio != N64_P1_DATA_PIN) return;

  uint32_t now = n64_now_us();

  if (events & GPIO_IRQ_EDGE_FALL) {
    if (!g_rx.receiving) {
      g_rx.receiving = true;
      g_rx.bits = 0;
      g_rx.bit_count = 0;
    }
    g_rx.last_fall_us = now;
    g_rx.last_edge_us = now;
  }

  if (events & GPIO_IRQ_EDGE_RISE) {
    if (!g_rx.receiving) return;

    uint32_t low_us = now - g_rx.last_fall_us;
    // Console->controller encoding: 0 = 3us low, 1 = 1us low
    uint32_t bit = (low_us >= 2u) ? 0u : 1u;
    g_rx.bits = (g_rx.bits << 1) | bit;
    g_rx.bit_count++;
    g_rx.last_edge_us = now;
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
  if (g_rx.receiving) {
    uint32_t now = n64_now_us();
    if ((now - g_rx.last_edge_us) > N64_CMD_GAP_US) {
      g_rx.receiving = false;
      g_rx.frame_ready = true;
      g_rx.frame_bits = g_rx.bits;
      g_rx.frame_len = g_rx.bit_count;
    }
  }

  if (g_rx.frame_ready) {
    uint32_t bits = g_rx.frame_bits;
    uint8_t len = g_rx.frame_len;
    g_rx.frame_ready = false;

    if (len >= 8) {
      uint8_t cmd = (uint8_t)(bits >> (len - 8));
      n64_handle_command(cmd, bits, len);
    }
  }
}
