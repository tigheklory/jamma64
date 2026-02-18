#pragma once

#include <stddef.h>
#include <stdbool.h>

void event_log_init(void);
void event_log_appendf(const char *fmt, ...);
void event_log_flush_if_needed(void);
void event_log_flush_now(void);
void event_log_clear(void);
bool event_log_dirty(void);
size_t event_log_copy(char *out, size_t out_max);
