#include "event_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#define LOG_SECTOR_SIZE 4096u
#define LOG_MAGIC 0x4A4D4C47u  // "GLMJ"
#define LOG_FLUSH_INTERVAL_MS 5000u

#ifndef PICO_FLASH_SIZE_BYTES
#error "PICO_FLASH_SIZE_BYTES not defined"
#endif

// Keep clear of wifi_config sector (currently last sector).
#define LOG_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - (2u * LOG_SECTOR_SIZE))
#define LOG_FLASH_ADDR   (XIP_BASE + LOG_FLASH_OFFSET)
#define LOG_DATA_MAX     (LOG_SECTOR_SIZE - 16u)

typedef struct {
  uint32_t magic;
  uint32_t len;
  uint32_t crc;
  uint32_t reserved;
  char data[LOG_DATA_MAX];
} log_blob_t;

static log_blob_t g_log;
static bool g_dirty;
static absolute_time_t g_next_flush;

static uint32_t log_crc(const char *data, uint32_t len) {
  uint32_t s = 0xA5A55A5Au;
  for (uint32_t i = 0; i < len; i++) s = (s * 131u) + (uint8_t)data[i];
  return s;
}

void event_log_init(void) {
  memset(&g_log, 0, sizeof(g_log));
  g_log.magic = LOG_MAGIC;
  g_next_flush = make_timeout_time_ms(LOG_FLUSH_INTERVAL_MS);

  const log_blob_t *f = (const log_blob_t *)LOG_FLASH_ADDR;
  if (f->magic == LOG_MAGIC && f->len < LOG_DATA_MAX) {
    uint32_t crc = log_crc(f->data, f->len);
    if (crc == f->crc) {
      g_log.len = f->len;
      memcpy(g_log.data, f->data, f->len);
      g_log.data[g_log.len] = '\0';
    }
  }
}

bool event_log_dirty(void) {
  return g_dirty;
}

size_t event_log_copy(char *out, size_t out_max) {
  if (!out || out_max == 0) return 0;
  size_t n = g_log.len;
  if (n >= out_max) n = out_max - 1;
  if (n > 0) memcpy(out, g_log.data, n);
  out[n] = '\0';
  return n;
}

void event_log_appendf(const char *fmt, ...) {
  char line[128];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if (n <= 0) return;

  size_t use = (size_t)n;
  if (use >= sizeof(line)) use = sizeof(line) - 1;

  if (use + 1u + g_log.len >= LOG_DATA_MAX) {
    // Keep tail of buffer; drop oldest content.
    size_t keep = LOG_DATA_MAX / 2u;
    if (g_log.len > keep) {
      memmove(g_log.data, g_log.data + (g_log.len - keep), keep);
      g_log.len = (uint32_t)keep;
    } else {
      g_log.len = 0;
    }
  }

  memcpy(g_log.data + g_log.len, line, use);
  g_log.len += (uint32_t)use;
  g_log.data[g_log.len++] = '\n';
  g_log.data[g_log.len] = '\0';
  g_dirty = true;
}

void event_log_flush_now(void) {
  if (!g_dirty) return;
  log_blob_t out;
  memset(&out, 0xFF, sizeof(out));
  out.magic = LOG_MAGIC;
  out.len = g_log.len;
  out.crc = log_crc(g_log.data, g_log.len);
  out.reserved = 0;
  if (g_log.len > 0) memcpy(out.data, g_log.data, g_log.len);

  uint32_t ints = save_and_disable_interrupts();
  flash_range_erase(LOG_FLASH_OFFSET, LOG_SECTOR_SIZE);
  flash_range_program(LOG_FLASH_OFFSET, (const uint8_t *)&out, sizeof(out));
  restore_interrupts(ints);

  g_dirty = false;
  g_next_flush = delayed_by_ms(get_absolute_time(), LOG_FLUSH_INTERVAL_MS);
}

void event_log_clear(void) {
  g_log.magic = LOG_MAGIC;
  g_log.len = 0;
  g_log.crc = 0;
  g_log.reserved = 0;
  g_log.data[0] = '\0';
  g_dirty = true;
}

void event_log_flush_if_needed(void) {
  if (!g_dirty) return;
  if (absolute_time_diff_us(get_absolute_time(), g_next_flush) >= 0) return;
  event_log_flush_now();
}
