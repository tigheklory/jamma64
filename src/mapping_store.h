#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "profile.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAP_STORE_NAME_MAX 15u
#define MAP_STORE_MAX_PROFILES 5u

void mapping_store_init_apply(volatile profile_t *active_profile);
bool mapping_store_save_named(const char *name, const volatile profile_t *src_profile);
bool mapping_store_load_named(const char *name, volatile profile_t *dst_profile);
size_t mapping_store_export_json(char *out, size_t out_max);

#ifdef __cplusplus
}
#endif

