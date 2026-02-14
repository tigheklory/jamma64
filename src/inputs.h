#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "profile.h"

typedef struct {
  // 1 bit per phys_in_t
  uint64_t bits;
} inputs_t;

void inputs_init(void);
inputs_t inputs_read(void);
static inline bool inputs_get(inputs_t in, phys_in_t p) {
  return (in.bits >> p) & 1u;
}
