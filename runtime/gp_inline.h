/* Small in-line helpers the natives share (native.c, hud.c): FIFO writes, virtual calls and
 * hw_gather_copy's linked path */
#pragma once
#include "gc_core.h"
#include "gx.h"
#include <string.h>

/* Gekko single-precision multiply(-add): exact product in double, one rounding to single. */
ALWAYS_INLINE float madd(float a, float b, float c) { return (float)((double)a * b + c); }
ALWAYS_INLINE float mul(float a, float b) { return (float)((double)a * b); }
ALWAYS_INLINE uint64_t dbits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
ALWAYS_INLINE double rs(double x) { return (double)(float)x; } /* a paired-single lane's rounding */
/* hw_gather_copy's linked path (hw.c) in line: most callers here send a few bytes at a time.
 * 16384 = hw.c's STAGE_FLUSH; anything else (capture, unlinked, a full stage) goes to hw.c. */
ALWAYS_INLINE void gp_copy(const uint8_t *src, size_t n) {
    size_t s = g_gx_gp.stage_len;
    unsigned g = g_gx_gp.gather_len;
    if (LIKELY(!g_gx_cap && g_gx_link && s + g + n <= 16384)) {
        memcpy(g_gx_stage + s + g, src, n);
        unsigned t = g + (unsigned)n, done = t & ~31u;
        g_gx_gp.stage_len = s + done;
        g_gx_gp.gather_len = t - done;
        g_gx_gp.pending += done;
        return;
    }
    hw_gather_copy(src, n);
}
#define hw_gather_copy gp_copy

/* FIFO bytes written big-endian at *p */
ALWAYS_INLINE void fifo8(uint8_t **p, uint32_t v) { *(*p)++ = (uint8_t)v; }
ALWAYS_INLINE void fifo16(uint8_t **p, uint32_t v) { put_be(*p, 2, v); *p += 2; }
ALWAYS_INLINE void fifo32(uint8_t **p, uint32_t v) { put_be(*p, 4, v); *p += 4; }

/* Virtual call fn(r3[, r4]). */
ALWAYS_INLINE void vcall1(Ctx *c, uint32_t fn, uint32_t r3) { ctx_set_gpr(c, 3, r3); ctx_call(c, fn); }
ALWAYS_INLINE void vcall2(Ctx *c, uint32_t fn, uint32_t r3, uint32_t r4) { ctx_set_gpr(c, 3, r3); ctx_set_gpr(c, 4, r4); ctx_call(c, fn); }
