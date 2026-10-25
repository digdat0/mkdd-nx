/* Battle routes for the computer's karts (route.c). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define ROUTE_MAX 512       /* points in a network (or loop) */
#define ROUTE_NETS 3        /* networks loaded at once: one for each computer's kart (a battle has 4 karts) */
#define ROUTE_FILE_NETS 32  /* networks a route file may hold */
#define ROUTE_POINTS (ROUTE_MAX * ROUTE_NETS)

/* the built-in route files (switch/CMakeLists.txt's routes_data.S; the PC harness: none), ended by a
 * NULL key: {key, the file's first byte, past its last} */
struct route_file { const char *key, *start, *end; };
extern const struct route_file route_builtin[];

void route_course(uint32_t id, uint32_t hash, bool battle); /* the course a race loads */
/* the current arena's route: points, each one's CL group ID (-1 inside a group), 0 none. Up to want
 * different networks, *got of them, network k at points start[k] to start[k + 1] (start: want + 1) */
int route_load(float (*pts)[3], int16_t *group, int max, int want, int *start, int *got);
