#pragma once
#include <stdbool.h>
#include <stddef.h>

#define WIFI_SSID_MAX 32
#define WIFI_PASS_MAX 64

typedef struct {
  char ssid[WIFI_SSID_MAX + 1];
  char password[WIFI_PASS_MAX + 1];
  bool valid;
} wifi_creds_t;

bool wifi_config_load(wifi_creds_t *out);
bool wifi_config_save(const wifi_creds_t *in);
void wifi_config_erase(void);

// Parse "wifi.txt" content
bool wifi_parse_txt(const char *buf, size_t len, wifi_creds_t *out);
