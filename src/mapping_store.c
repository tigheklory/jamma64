#include "mapping_store.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"
#include "profile.h"

#define MAP_STORE_MAGIC 0x4D415036u
#define MAP_STORE_VERSION 3u

#define MAP_STORE_SECTOR_SIZE 4096u
#define MAP_STORE_OFFSET (PICO_FLASH_SIZE_BYTES - (2u * MAP_STORE_SECTOR_SIZE))
#define MAP_STORE_ADDR (XIP_BASE + MAP_STORE_OFFSET)

typedef struct {
  uint8_t used;
  char name[MAP_STORE_NAME_MAX + 1];
  profile_t profile;
} stored_profile_t;

typedef struct {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved0;
  char active_name[MAP_STORE_NAME_MAX + 1];
  uint8_t reserved1[3];
  stored_profile_t entries[MAP_STORE_MAX_PROFILES];
  uint32_t crc;
} mapping_store_blob_t;

static mapping_store_blob_t g_store;

static uint32_t simple_crc32(const mapping_store_blob_t *blob) {
  const uint8_t *p = (const uint8_t *)blob;
  uint32_t s = 0;
  for (size_t i = 0; i < sizeof(*blob); i++) s = (s * 131u) + p[i];
  return s ^ 0x5A5A5A5Au;
}

static void sanitize_name(char *dst, size_t dst_sz, const char *src) {
  if (!dst || !dst_sz) return;
  dst[0] = '\0';
  if (!src) return;
  size_t j = 0;
  for (size_t i = 0; src[i] && j + 1 < dst_sz; i++) {
    char c = src[i];
    if (isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.') {
      dst[j++] = c;
    } else if (isspace((unsigned char)c)) {
      if (j && dst[j - 1] != '_') dst[j++] = '_';
    }
  }
  dst[j] = '\0';
}

static int find_profile_index(const char *name) {
  if (!name || !name[0]) return -1;
  for (size_t i = 0; i < MAP_STORE_MAX_PROFILES; i++) {
    if (!g_store.entries[i].used) continue;
    if (strncmp(g_store.entries[i].name, name, MAP_STORE_NAME_MAX) == 0) return (int)i;
  }
  return -1;
}

static int first_free_index(void) {
  for (size_t i = 0; i < MAP_STORE_MAX_PROFILES; i++) {
    if (!g_store.entries[i].used) return (int)i;
  }
  return -1;
}

static int oldest_replace_index(void) {
  // Pragmatic eviction: keep slot 0 ("default") unless empty, replace first non-default used.
  for (size_t i = 1; i < MAP_STORE_MAX_PROFILES; i++) {
    if (g_store.entries[i].used) return (int)i;
  }
  return (g_store.entries[0].used ? 0 : -1);
}

static void store_set_defaults(const profile_t *defaults) {
  memset(&g_store, 0, sizeof(g_store));
  g_store.magic = MAP_STORE_MAGIC;
  g_store.version = MAP_STORE_VERSION;
  strncpy(g_store.active_name, "default", MAP_STORE_NAME_MAX);
  g_store.entries[0].used = 1u;
  strncpy(g_store.entries[0].name, "default", MAP_STORE_NAME_MAX);
  if (defaults) {
    g_store.entries[0].profile = *defaults;
  } else {
    memset(&g_store.entries[0].profile, 0, sizeof(profile_t));
  }
}

static bool store_valid(const mapping_store_blob_t *blob) {
  if (!blob) return false;
  if (blob->magic != MAP_STORE_MAGIC) return false;
  if (blob->version != MAP_STORE_VERSION) return false;
  mapping_store_blob_t tmp = *blob;
  uint32_t crc = tmp.crc;
  tmp.crc = 0;
  return crc == simple_crc32(&tmp);
}

static void persist_store(void) {
  mapping_store_blob_t tmp = g_store;
  tmp.crc = 0;
  tmp.crc = simple_crc32(&tmp);
  g_store.crc = tmp.crc;

  uint32_t ints = save_and_disable_interrupts();
  flash_range_erase(MAP_STORE_OFFSET, MAP_STORE_SECTOR_SIZE);
  flash_range_program(MAP_STORE_OFFSET, (const uint8_t *)&tmp, sizeof(tmp));
  restore_interrupts(ints);
}

void mapping_store_init_apply(volatile profile_t *active_profile) {
  profile_t defaults = {0};
  if (active_profile) defaults = *(const profile_t *)active_profile;

  const mapping_store_blob_t *flash_blob = (const mapping_store_blob_t *)MAP_STORE_ADDR;
  if (store_valid(flash_blob)) {
    g_store = *flash_blob;
  } else {
    store_set_defaults(&defaults);
    persist_store();
  }

  if (!active_profile) return;
  if (!mapping_store_load_named(g_store.active_name, active_profile)) {
    // Fall back to default profile if active name is stale.
    (void)mapping_store_load_named("default", active_profile);
  }
}

bool mapping_store_save_named(const char *name, const volatile profile_t *src_profile) {
  if (!name || !src_profile) return false;
  char clean[MAP_STORE_NAME_MAX + 1];
  sanitize_name(clean, sizeof(clean), name);
  if (!clean[0]) return false;

  int idx = find_profile_index(clean);
  if (idx < 0) idx = first_free_index();
  if (idx < 0) idx = oldest_replace_index();
  if (idx < 0) return false;

  g_store.entries[idx].used = 1u;
  memset(g_store.entries[idx].name, 0, sizeof(g_store.entries[idx].name));
  strncpy(g_store.entries[idx].name, clean, MAP_STORE_NAME_MAX);
  g_store.entries[idx].profile = *(const profile_t *)src_profile;
  memset(g_store.active_name, 0, sizeof(g_store.active_name));
  strncpy(g_store.active_name, clean, MAP_STORE_NAME_MAX);
  persist_store();
  return true;
}

bool mapping_store_load_named(const char *name, volatile profile_t *dst_profile) {
  if (!name || !dst_profile) return false;
  char clean[MAP_STORE_NAME_MAX + 1];
  sanitize_name(clean, sizeof(clean), name);
  if (!clean[0]) return false;

  int idx = find_profile_index(clean);
  if (idx < 0) return false;
  *dst_profile = g_store.entries[idx].profile;
  memset(g_store.active_name, 0, sizeof(g_store.active_name));
  strncpy(g_store.active_name, clean, MAP_STORE_NAME_MAX);
  persist_store();
  return true;
}

size_t mapping_store_export_json(char *out, size_t out_max) {
  if (!out || out_max == 0u) return 0u;
  size_t used = 0u;

  int n = snprintf(out, out_max, "{\"version\":1,\"active\":\"%s\",\"profiles\":[", g_store.active_name);
  if (n < 0 || (size_t)n >= out_max) return 0u;
  used = (size_t)n;

  bool first = true;
  for (size_t i = 0; i < MAP_STORE_MAX_PROFILES; i++) {
    if (!g_store.entries[i].used) continue;
    const profile_t *p = &g_store.entries[i].profile;
    n = snprintf(
      out + used,
      out_max - used,
      "%s{\"name\":\"%s\",\"p1\":%u,\"p2\":%u,\"throw\":%u,\"diag\":%u,\"map\":[",
      first ? "" : ",",
      g_store.entries[i].name,
      (unsigned)p->p1_stick_mode,
      (unsigned)p->p2_stick_mode,
      (unsigned)p->analog_throw,
      (unsigned)p->diagonal_scale_pct
    );
    if (n < 0 || (size_t)n >= (out_max - used)) {
      out[0] = '\0';
      return 0u;
    }
    used += (size_t)n;

    for (int m = 0; m < N64_OUTPUT_COUNT; m++) {
      n = snprintf(out + used, out_max - used, "%s%u", (m == 0 ? "" : ","), (unsigned)p->map[m]);
      if (n < 0 || (size_t)n >= (out_max - used)) {
        out[0] = '\0';
        return 0u;
      }
      used += (size_t)n;
    }

    n = snprintf(out + used, out_max - used, "]}");
    if (n < 0 || (size_t)n >= (out_max - used)) {
      out[0] = '\0';
      return 0u;
    }
    used += (size_t)n;
    first = false;
  }

  n = snprintf(out + used, out_max - used, "]}");
  if (n < 0 || (size_t)n >= (out_max - used)) {
    out[0] = '\0';
    return 0u;
  }
  used += (size_t)n;
  return used;
}

size_t mapping_store_list_names(
    char names[][MAP_STORE_NAME_MAX + 1],
    size_t max_names,
    char *active_name_out,
    size_t active_name_out_sz) {
  if (active_name_out && active_name_out_sz > 0u) {
    active_name_out[0] = '\0';
    strncpy(active_name_out, g_store.active_name, active_name_out_sz - 1u);
    active_name_out[active_name_out_sz - 1u] = '\0';
  }

  if (!names || max_names == 0u) return 0u;

  size_t count = 0u;
  for (size_t i = 0; i < MAP_STORE_MAX_PROFILES && count < max_names; i++) {
    if (!g_store.entries[i].used) continue;
    names[count][0] = '\0';
    strncpy(names[count], g_store.entries[i].name, MAP_STORE_NAME_MAX);
    names[count][MAP_STORE_NAME_MAX] = '\0';
    count++;
  }
  return count;
}

bool mapping_store_reset_defaults(volatile profile_t *active_profile) {
  profile_t defaults;
  profile_get_defaults(&defaults);
  store_set_defaults(&defaults);
  persist_store();
  if (active_profile) {
    *active_profile = defaults;
  }
  return true;
}
