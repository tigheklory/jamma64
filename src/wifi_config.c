#include "wifi_config.h"
#include <string.h>
#include <ctype.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#define CFG_MAGIC 0x4A364D41u  // "AM6J" arbitrary
typedef struct {
  uint32_t magic;
  uint32_t crc; // simple checksum
  char ssid[WIFI_SSID_MAX + 1];
  char password[WIFI_PASS_MAX + 1];
} wifi_cfg_flash_t;

// Use last 4KB sector of flash (safe for many projects; adjust later if needed)
#ifndef PICO_FLASH_SIZE_BYTES
#error "PICO_FLASH_SIZE_BYTES not defined"
#endif

#define CFG_SECTOR_SIZE (4096u)
#define CFG_OFFSET (PICO_FLASH_SIZE_BYTES - CFG_SECTOR_SIZE)
#define CFG_ADDR   (XIP_BASE + CFG_OFFSET)

static uint32_t simple_crc(const wifi_cfg_flash_t *c) {
  // Not cryptographic; just to detect blank/garbage.
  const uint8_t *p = (const uint8_t*)c;
  uint32_t s = 0;
  for (size_t i = 0; i < sizeof(*c); i++) s = (s * 131u) + p[i];
  return s ^ 0xA5A5A5A5u;
}

bool wifi_config_load(wifi_creds_t *out) {
  if (!out) return false;
  const wifi_cfg_flash_t *cfg = (const wifi_cfg_flash_t *)CFG_ADDR;
  if (cfg->magic != CFG_MAGIC) {
    out->valid = false;
    return false;
  }
  wifi_cfg_flash_t tmp = *cfg;
  uint32_t crc = tmp.crc;
  tmp.crc = 0;
  if (crc != simple_crc(&tmp)) {
    out->valid = false;
    return false;
  }
  strncpy(out->ssid, cfg->ssid, WIFI_SSID_MAX);
  out->ssid[WIFI_SSID_MAX] = 0;
  strncpy(out->password, cfg->password, WIFI_PASS_MAX);
  out->password[WIFI_PASS_MAX] = 0;
  out->valid = (out->ssid[0] != 0);
  return out->valid;
}

bool wifi_config_save(const wifi_creds_t *in) {
  if (!in) return false;
  if (!in->ssid[0]) return false;

  wifi_cfg_flash_t cfg = {0};
  cfg.magic = CFG_MAGIC;
  strncpy(cfg.ssid, in->ssid, WIFI_SSID_MAX);
  strncpy(cfg.password, in->password, WIFI_PASS_MAX);
  cfg.crc = 0;
  cfg.crc = simple_crc(&cfg);

  uint32_t ints = save_and_disable_interrupts();
  flash_range_erase(CFG_OFFSET, CFG_SECTOR_SIZE);
  flash_range_program(CFG_OFFSET, (const uint8_t*)&cfg, sizeof(cfg));
  restore_interrupts(ints);

  return true;
}

void wifi_config_erase(void) {
  uint32_t ints = save_and_disable_interrupts();
  flash_range_erase(CFG_OFFSET, CFG_SECTOR_SIZE);
  restore_interrupts(ints);
}

static void trim(char *s) {
  // trim leading
  char *p = s;
  while (*p && isspace((unsigned char)*p)) p++;
  if (p != s) memmove(s, p, strlen(p) + 1);
  // trim trailing
  size_t n = strlen(s);
  while (n && isspace((unsigned char)s[n-1])) s[--n] = 0;
}

bool wifi_parse_txt(const char *buf, size_t len, wifi_creds_t *out) {
  if (!buf || !out) return false;
  memset(out, 0, sizeof(*out));

  // Copy into a scratch buffer we can mutate
  char tmp[256];
  if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
  memcpy(tmp, buf, len);
  tmp[len] = 0;

  // Parse lines KEY=VALUE
  char *saveptr = NULL;
  for (char *line = strtok_r(tmp, "\n\r", &saveptr); line; line = strtok_r(NULL, "\n\r", &saveptr)) {
    trim(line);
    if (!line[0] || line[0] == '#') continue;

    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    char *key = line;
    char *val = eq + 1;
    trim(key);
    trim(val);

    // uppercase key compare
    for (char *k = key; *k; k++) *k = (char)toupper((unsigned char)*k);

    if (!strcmp(key, "SSID")) {
      strncpy(out->ssid, val, WIFI_SSID_MAX);
      out->ssid[WIFI_SSID_MAX] = 0;
    } else if (!strcmp(key, "PASSWORD")) {
      strncpy(out->password, val, WIFI_PASS_MAX);
      out->password[WIFI_PASS_MAX] = 0;
    } else if (!strcmp(key, "FORGET") && (!strcmp(val, "1") || !strcmp(val, "TRUE"))) {
      // user can drop wifi.txt with FORGET=1
      out->ssid[0] = 0;
      out->password[0] = 0;
      out->valid = false;
      return true;
    }
  }

  out->valid = (out->ssid[0] != 0);
  return out->valid;
}
