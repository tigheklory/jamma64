#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/watchdog.h"

#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

#include "tusb.h"

#include "inputs.h"
#include "web.h"
#include "profile.h"
#include "n64.h"

#include "wifi_config.h"
#include "usb_msc.h"
#include "event_log.h"

#ifndef JAMMA64_FW_VERSION
#define JAMMA64_FW_VERSION "dev"
#endif

#ifndef JAMMA64_ENABLE_WIFI
#define JAMMA64_ENABLE_WIFI 0
#endif

#ifndef JAMMA64_ENABLE_STATUS_PRINTS
#define JAMMA64_ENABLE_STATUS_PRINTS 0
#endif

#define WIFI_STACK_INIT_DELAY_MS 2500u
#define WIFI_STACK_INIT_RETRY_MS 5000u

#if JAMMA64_ENABLE_WIFI
// Helper to print IP once connected
static void print_ip(void) {
  struct netif *n = netif_list;
  while (n) {
    if (netif_is_up(n) && !ip4_addr_isany_val(*netif_ip4_addr(n))) {
      printf("IP: %s\n", ip4addr_ntoa(netif_ip4_addr(n)));
      return;
    }
    n = n->next;
  }
  printf("IP: (not assigned yet)\n");
}

static bool connect_wifi_if_configured(uint32_t timeout_ms) {
  wifi_creds_t creds;
  if (!wifi_config_load(&creds) || !creds.valid) {
    printf("No Wi-Fi creds saved.\n");
    printf("Plug into a PC and drop wifi.txt on the JAMMA64 USB drive.\n");
    printf("Format:\nSSID=yourssid\nPASSWORD=yourpassword\n");
    return false;
  }

  printf("Connecting to Wi-Fi SSID '%s'...\n", creds.ssid);
  int r = cyw43_arch_wifi_connect_timeout_ms(
    creds.ssid,
    creds.password,
    CYW43_AUTH_WPA2_AES_PSK,
    timeout_ms
  );

  if (r) {
    printf("Wi-Fi connect failed: %d\n", r);
    printf("You can update creds by dropping a new wifi.txt on the drive.\n");
    return false;
  }

  printf("Wi-Fi connected.\n");
  web_init();
  return true;
}
#endif

int main() {
  stdio_init_all();
  // Start protocol handling quickly so the console sees us during its initial probe.
  sleep_ms(20);
  printf("\nJAMMA64 starting (fw=%s)\n", JAMMA64_FW_VERSION);

  event_log_init();
  event_log_appendf("BOOT JAMMA64 fw=%s", JAMMA64_FW_VERSION);
  inputs_init();
  n64_init();

  // Start USB (Drive + Serial)
  usb_msc_init();

  // Do not block startup on Wi-Fi init. N64 probing happens immediately at console boot.
  #if JAMMA64_ENABLE_WIFI
    bool wifi_stack_ready = false;
    absolute_time_t next_wifi_stack_init = make_timeout_time_ms(WIFI_STACK_INIT_DELAY_MS);
    bool web_running = false;
    absolute_time_t next_wifi_try = make_timeout_time_ms(10000);
  #endif

#if JAMMA64_ENABLE_STATUS_PRINTS
  absolute_time_t next = make_timeout_time_ms(1000);
#endif
  bool reboot_notice_printed = false;
  while (true) {
    // Keep N64 command/response handling low latency.
    n64_task();

    // Required for USB drive + serial to function
    tud_task();

    // Persist logs only while not mounted to a host filesystem.
    if (!tud_mounted()) {
      event_log_flush_if_needed();
    }

    #if JAMMA64_ENABLE_WIFI
      // Defer potentially slow Wi-Fi chip init until N64 protocol handling is already active.
      if (!wifi_stack_ready && absolute_time_diff_us(get_absolute_time(), next_wifi_stack_init) < 0) {
        int wifi_init = cyw43_arch_init();
        if (wifi_init == 0) {
          cyw43_arch_enable_sta_mode();
          wifi_stack_ready = true;
          printf("Wi-Fi stack ready.\n");
        } else {
          printf("cyw43_arch_init failed: %d\n", wifi_init);
          next_wifi_stack_init = make_timeout_time_ms(WIFI_STACK_INIT_RETRY_MS);
        }
      }

      // Background Wi-Fi connect attempts (short timeout to keep N64 loop responsive).
      if (wifi_stack_ready && !web_running) {
        if (absolute_time_diff_us(get_absolute_time(), next_wifi_try) < 0) {
          web_running = connect_wifi_if_configured(1200);
          next_wifi_try = delayed_by_ms(next_wifi_try, 10000);
        }
      }
    #endif

    // Status prints
#if JAMMA64_ENABLE_STATUS_PRINTS
    if (absolute_time_diff_us(get_absolute_time(), next) < 0) {
      #if JAMMA64_ENABLE_WIFI
        if (web_running) print_ip();
      #endif

      if (usb_msc_reboot_required() && !reboot_notice_printed) {
        printf("Wi-Fi saved; reboot required to apply changes.\n");
        reboot_notice_printed = true;
      }

      printf("Mode P1=%s P2=%s throw=%u\n",
             g_profile.p1_stick_mode == STICK_MODE_DPAD ? "DPAD" : "ANALOG",
             g_profile.p2_stick_mode == STICK_MODE_DPAD ? "DPAD" : "ANALOG",
             g_profile.analog_throw);

      inputs_t in = inputs_read();
      printf("P1 UDLR=%d%d%d%d B1-6=%d%d%d%d%d%d Start=%d Coin=%d\n",
             inputs_get(in, IN_P1_UP), inputs_get(in, IN_P1_DOWN),
             inputs_get(in, IN_P1_LEFT), inputs_get(in, IN_P1_RIGHT),
             inputs_get(in, IN_P1_B1), inputs_get(in, IN_P1_B2), inputs_get(in, IN_P1_B3),
             inputs_get(in, IN_P1_B4), inputs_get(in, IN_P1_B5), inputs_get(in, IN_P1_B6),
             inputs_get(in, IN_P1_START), inputs_get(in, IN_COIN1));

      next = delayed_by_ms(next, 1000);
    }
#endif

    tight_loop_contents();
  }
}
