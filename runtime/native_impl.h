/* Shared by native.c and battle.c: the natives' argument list, guest memory access, guest stack
 * frames, each native's original address, and battle.c's hooks. */
#pragma once
#include <string.h>
#include "native.h"

#define ARGS uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7
#define UNUSED_ARGS (void)a0, (void)a1, (void)a2, (void)a3, (void)a4, (void)a5, (void)a6, (void)a7
/* ---- guest memory access through M (MEM1 fast path, MMIO / locked cache otherwise) ---- */
ALWAYS_INLINE float ldf(Ctx *c, uint8_t *M, uint32_t a) {
    uint32_t u = (uint32_t)mld(c, M, a, 4);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
ALWAYS_INLINE void stf(Ctx *c, uint8_t *M, uint32_t a, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    mst(c, M, a, 4, u);
}
ALWAYS_INLINE void ldfn(Ctx *c, uint8_t *M, uint32_t a, float *v, int n) {
    if (LIKELY(IS_RAM(a) && IS_RAM(a + 4u * n - 1))) {
        const uint8_t *p = M + (a & 0x01FFFFFFu);
        for (int i = 0; i < n; i++) { uint32_t u = (uint32_t)get_be(p + 4 * i, 4); memcpy(&v[i], &u, 4); }
    } else {
        for (int i = 0; i < n; i++) v[i] = ldf(c, M, a + 4 * i);
    }
}
ALWAYS_INLINE void stfn(Ctx *c, uint8_t *M, uint32_t a, const float *v, int n) {
    if (LIKELY(IS_RAM(a) && IS_RAM(a + 4u * n - 1))) {
        uint8_t *p = M + (a & 0x01FFFFFFu);
        for (int i = 0; i < n; i++) { uint32_t u; memcpy(&u, &v[i], 4); put_be(p + 4 * i, 4, u); }
    } else {
        for (int i = 0; i < n; i++) stf(c, M, a + 4 * i, v[i]);
    }
}
ALWAYS_INLINE uint32_t ld32(Ctx *c, uint8_t *M, uint32_t a) { return (uint32_t)mld(c, M, a, 4); }
ALWAYS_INLINE uint32_t ld16(Ctx *c, uint8_t *M, uint32_t a) { return (uint32_t)mld(c, M, a, 2); }
/* A stack frame like the original's, for natives that call guest code (callees save LR in it). */
ALWAYS_INLINE uint32_t frame_push(Ctx *c, uint8_t *M, uint32_t size) {
    uint32_t old_sp = ctx_gpr(c, 1), sp = old_sp - size;
    mst(c, M, sp, 4, old_sp);
    ctx_set_gpr(c, 1, sp);
    return old_sp;
}
ALWAYS_INLINE void frame_pop(Ctx *c, uint32_t old_sp) { ctx_set_gpr(c, 1, old_sp); }
/* a guest string, "" none */
static inline void guest_str(Ctx *c, uint8_t *M, uint32_t a, char *out, size_t n) {
    size_t k = 0;
    for (; IS_RAM(a) && k < n - 1 && (out[k] = (char)mld(c, M, a + k, 1)); k++) {}
    out[k] = 0;
}
#define CP_DRIVER_NAMES 0x803512C0u /* DriverLoader::mscpaDriverName: char *[20] */

/* a guest pointer to RAM, not null */
ALWAYS_INLINE bool ram_ptr(uint32_t a) { return a && IS_RAM(a); }

/* each native's guest address (NAT_*), from the function names at start-up */
extern uint32_t g_native_addr[NATIVE_COUNT];

/* battle.c: once a frame, from the race camera (FUN_802b6bd4) */
void bai_tick(Ctx *c, uint8_t *M);
/* battle.c's natives */
uint64_t i_FUN_8018c878(Ctx *c, uint8_t *M, ARGS);
uint64_t i_loadCrsData__6CourseFb(Ctx *c, uint8_t *M, ARGS);
uint64_t i_FUN_802b1f60(Ctx *c, uint8_t *M, ARGS);
uint64_t i_FUN_802b2174(Ctx *c, uint8_t *M, ARGS);
uint64_t i_FUN_802be4cc(Ctx *c, uint8_t *M, ARGS);
uint64_t i_FUN_802be8d4(Ctx *c, uint8_t *M, ARGS);
uint64_t i_comebackRescure__13RivalBodyCtrlFP7CLPointb(Ctx *c, uint8_t *M, ARGS);
uint64_t i_calc__10GeoItemBoxFv(Ctx *c, uint8_t *M, ARGS);
uint64_t i_FUN_80226f1c(Ctx *c, uint8_t *M, ARGS);
uint64_t i_doItemCtrl__13RivalItemCtrlFv(Ctx *c, uint8_t *M, ARGS);
uint64_t i___ct__11BombStart2DFP7JKRHeap(Ctx *c, uint8_t *M, ARGS);
uint64_t i_doSterrCtrl__13RivalBodyCtrlFv(Ctx *c, uint8_t *M, ARGS);
uint64_t i_mode_init__9SceneMenuFv(Ctx *c, uint8_t *M, ARGS);
uint64_t i_mode__9SceneMenuFv(Ctx *c, uint8_t *M, ARGS);
uint64_t i_mode_end__9SceneMenuFv(Ctx *c, uint8_t *M, ARGS);
uint64_t i_setRaceData__9SceneMenuFv(Ctx *c, uint8_t *M, ARGS);
uint64_t i_getCharSel__9SceneMenuFRiRi(Ctx *c, uint8_t *M, ARGS);
uint64_t i_FUN_801f1cf4(Ctx *c, uint8_t *M, ARGS);
