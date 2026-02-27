#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/watchdog.h"
#include "cyw43.h"

#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

#include "tusb.h"

#include "inputs.h"
#include "web.h"
#include "profile.h"
#include "mapping_store.h"
#include "n64.h"

#include "wifi_config.h"
#include "usb_msc.h"
#include "n64_virtual.h"

#ifndef JAMMA64_FW_VERSION
#define JAMMA64_FW_VERSION "dev"
#endif

#ifndef JAMMA64_ENABLE_WIFI
#define JAMMA64_ENABLE_WIFI 0
#endif

#ifndef JAMMA64_ENABLE_STATUS_PRINTS
#define JAMMA64_ENABLE_STATUS_PRINTS 0
#endif
#ifndef JAMMA64_ENABLE_USB_DEBUG
#define JAMMA64_ENABLE_USB_DEBUG 0
#endif

#if JAMMA64_ENABLE_USB_DEBUG
#define DBG_PRINTF(...) printf(__VA_ARGS__)
#else
#define DBG_PRINTF(...) ((void)0)
#endif

#define WIFI_STACK_INIT_DELAY_MS 2500u
#define WIFI_STACK_INIT_RETRY_MS 5000u
#define N64_BOOT_PRIORITY_MS 800u
#define USB_TASK_PERIOD_US 250u
#define WIFI_TASK_PERIOD_US 1000u
#define WIFI_LINK_CHECK_PERIOD_MS 2000u

#if JAMMA64_ENABLE_WIFI
// Helper to print IP once connected
static void print_ip(void) {
  struct netif *n = netif_list;
  while (n) {
    if (netif_is_up(n) && !ip4_addr_isany_val(*netif_ip4_addr(n))) {
      DBG_PRINTF("IP: %s\n", ip4addr_ntoa(netif_ip4_addr(n)));
      return;
    }
    n = n->next;
  }
  DBG_PRINTF("IP: (not assigned yet)\n");
}

static bool connect_wifi_if_configured(uint32_t timeout_ms) {
  wifi_creds_t creds;
  if (!wifi_config_load(&creds) || !creds.valid) {
    DBG_PRINTF("No Wi-Fi creds saved.\n");
    DBG_PRINTF("Plug into a PC and drop wifi.txt on the JAMMA64 USB drive.\n");
    DBG_PRINTF("Format:\nSSID=yourssid\nPASSWORD=yourpassword\n");
    return false;
  }

  DBG_PRINTF("Connecting to Wi-Fi SSID '%s'...\n", creds.ssid);
  int r = cyw43_arch_wifi_connect_timeout_ms(
    creds.ssid,
    creds.password,
    CYW43_AUTH_WPA2_AES_PSK,
    timeout_ms
  );

  if (r) {
    DBG_PRINTF("Wi-Fi connect failed: %d\n", r);
    DBG_PRINTF("You can update creds by dropping a new wifi.txt on the drive.\n");
    return false;
  }

  DBG_PRINTF("Wi-Fi connected.\n");
  web_init();
  return true;
}
#endif

int main() {
  stdio_init_all();
  DBG_PRINTF("\nJAMMA64 starting (fw=%s)\n", JAMMA64_FW_VERSION);
  bool fresh_fw = mapping_store_init_apply(&g_profile);
  if (fresh_fw) {
    // On new UF2 firmware, wipe persisted Wi-Fi config as part of full factory reset.
    wifi_config_erase();
  }
  inputs_init();
  n64_virtual_clear();
  n64_init();

  // Prioritize N64 servicing immediately at boot so early console probe traffic
  // (e.g. SM64 startup) is handled before slower subsystem init.
  absolute_time_t n64_boot_priority_until = make_timeout_time_ms(N64_BOOT_PRIORITY_MS);
  while (absolute_time_diff_us(get_absolute_time(), n64_boot_priority_until) < 0) {
    n64_task();
    tight_loop_contents();
  }

  // Start USB (Drive + Serial)
  usb_msc_init();

  // Do not block startup on Wi-Fi init. N64 probing happens immediately at console boot.
  #if JAMMA64_ENABLE_WIFI
    bool wifi_stack_ready = false;
    absolute_time_t next_wifi_stack_init = make_timeout_time_ms(WIFI_STACK_INIT_DELAY_MS);
    bool web_running = false;
    absolute_time_t next_wifi_try = make_timeout_time_ms(10000);
    absolute_time_t next_wifi_link_check = make_timeout_time_ms(WIFI_LINK_CHECK_PERIOD_MS);
  #endif

#if JAMMA64_ENABLE_STATUS_PRINTS
  absolute_time_t next = make_timeout_time_ms(1000);
#endif
  bool reboot_notice_printed = false;
  uint32_t last_usb_task_us = 0;
  uint32_t last_wifi_task_us = 0;
  while (true) {
    // Keep N64 command/response handling low latency.
    n64_task();

    uint32_t now_us = time_us_32();
    if ((uint32_t)(now_us - last_usb_task_us) >= USB_TASK_PERIOD_US) {
      // Required for USB drive + serial to function, but throttled so it
      // doesn't dominate the high-rate N64 polling loop.
      tud_task();
      last_usb_task_us = now_us;
    }

    #if JAMMA64_ENABLE_WIFI
      if ((uint32_t)(now_us - last_wifi_task_us) >= WIFI_TASK_PERIOD_US) {
        // Defer potentially slow Wi-Fi chip init until N64 protocol handling is already active.
        if (!wifi_stack_ready && absolute_time_diff_us(get_absolute_time(), next_wifi_stack_init) < 0) {
          int wifi_init = cyw43_arch_init();
          if (wifi_init == 0) {
            cyw43_arch_enable_sta_mode();
            wifi_stack_ready = true;
            DBG_PRINTF("Wi-Fi stack ready.\n");
          } else {
            DBG_PRINTF("cyw43_arch_init failed: %d\n", wifi_init);
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

        if (wifi_stack_ready && web_running &&
            absolute_time_diff_us(get_absolute_time(), next_wifi_link_check) < 0) {
          int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
          if (link != CYW43_LINK_UP && link != CYW43_LINK_JOIN && link != CYW43_LINK_NOIP) {
            DBG_PRINTF("Wi-Fi link dropped (status=%d); scheduling reconnect.\n", link);
            web_running = false;
            next_wifi_try = make_timeout_time_ms(250);
          }
          next_wifi_link_check = delayed_by_ms(next_wifi_link_check, WIFI_LINK_CHECK_PERIOD_MS);
        }
        last_wifi_task_us = now_us;
      }
    #endif

    // Status prints
#if JAMMA64_ENABLE_STATUS_PRINTS
    if (absolute_time_diff_us(get_absolute_time(), next) < 0) {
      #if JAMMA64_ENABLE_WIFI
        if (web_running) print_ip();
      #endif

      if (usb_msc_reboot_required() && !reboot_notice_printed) {
        DBG_PRINTF("Wi-Fi saved; reboot required to apply changes.\n");
        reboot_notice_printed = true;
      }

      DBG_PRINTF("Mode P1=%s P2=%s throw=%u diag=%u%%\n",
             g_profile.p1_stick_mode == STICK_MODE_DPAD ? "DPAD" : "ANALOG",
             g_profile.p2_stick_mode == STICK_MODE_DPAD ? "DPAD" : "ANALOG",
             g_profile.analog_throw,
             g_profile.diagonal_scale_pct);

      inputs_t in = inputs_read();
      DBG_PRINTF("P1 UDLR=%d%d%d%d B1-6=%d%d%d%d%d%d Start=%d Coin=%d\n",
             inputs_get(in, IN_P1_UP), inputs_get(in, IN_P1_DOWN),
             inputs_get(in, IN_P1_LEFT), inputs_get(in, IN_P1_RIGHT),
             inputs_get(in, IN_P1_B1), inputs_get(in, IN_P1_B2), inputs_get(in, IN_P1_B3),
             inputs_get(in, IN_P1_B4), inputs_get(in, IN_P1_B5), inputs_get(in, IN_P1_B6),
             inputs_get(in, IN_P1_START), inputs_get(in, IN_COIN1));

      next = delayed_by_ms(next, 1000);
    }
#endif

    // One extra N64 service pass after background tasks to reduce jitter.
    n64_task();

    tight_loop_contents();
  }
}
