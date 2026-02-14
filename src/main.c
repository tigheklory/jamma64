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

static bool connect_wifi_if_configured(void) {
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
    30000
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

int main() {
  stdio_init_all();
  sleep_ms(1500);
  printf("\nJAMMA64 starting...\n");

  event_log_init();
  inputs_init();
  n64_init();

  // Start USB (Drive + Serial)
  usb_msc_init();

  // Wi-Fi chip init
  if (cyw43_arch_init()) {
    printf("cyw43_arch_init failed\n");
    return 1;
  }
  cyw43_arch_enable_sta_mode();

  bool web_running = connect_wifi_if_configured();

  absolute_time_t next = make_timeout_time_ms(1000);
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

    // If we didn't have creds earlier, try occasionally (in case wifi.txt was just dropped)
    static absolute_time_t retry = {0};
    if (!web_running) {
      if (is_nil_time(retry)) retry = make_timeout_time_ms(2000);
      if (absolute_time_diff_us(get_absolute_time(), retry) < 0) {
        web_running = connect_wifi_if_configured();
        retry = delayed_by_ms(retry, 2000);
      }
    }

    // Status prints
    if (absolute_time_diff_us(get_absolute_time(), next) < 0) {
      if (web_running) print_ip();

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

    tight_loop_contents();
  }
}
