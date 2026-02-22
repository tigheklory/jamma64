#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize single-controller N64 emulation (player 1).
bool n64_init(void);

// Must run very frequently from the main loop.
void n64_task(void);

#ifdef __cplusplus
}
#endif
