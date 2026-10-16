#pragma once
#include <stdint.h>

/* Sampling profiler: see prof_switch.c. */
void prof_start(const char *path, uint64_t image_base, uint64_t image_end);
void prof_stop(void);
