/* Native Gekko paired-single instructions (C port of gc-core's ps.rs).
 *
 * The GameCubeLoader SLEIGH spec gets several of these wrong (e.g. ps_merge00 f0,f1,f0 writes
 * ps0 before reading it for ps1), so the generator emits ctx_ps(c, word) for every ps_* / psq_* /
 * dcbz_l instruction. All operands are read before writing (alias-safe) and arithmetic results
 * are rounded to single precision like the hardware. */
#include "gc_core.h"

#define FPR 0x4000
#define PS1 0x5000
#define GQR 0x1E40
#define CR 0x900

static inline double getd(const Ctx *c, unsigned off) { uint64_t u = RD(c, off, 8); double d; memcpy(&d, &u, 8); return d; }
static inline void setd(Ctx *c, unsigned off, double d) { uint64_t u; memcpy(&u, &d, 8); WR(c, off, 8, u); }
static inline double ps0(const Ctx *c, uint32_t r) { return getd(c, FPR + 8 * r); }
static inline double ps1(const Ctx *c, uint32_t r) { return getd(c, PS1 + 8 * r); }
static inline void set(Ctx *c, uint32_t r, double a, double b) { setd(c, FPR + 8 * r, a); setd(c, PS1 + 8 * r, b); }
static inline double single(double x) { return (double)(float)x; }

/* 2^n for the quantisation scales (n in -32..31), exactly. */
static inline double pow2(int n) { uint64_t u = (uint64_t)(1023 + n) << 52; double d; memcpy(&d, &u, 8); return d; }

/* Rust's f64::clamp followed by an `as` cast: NaN stays NaN and then converts to 0. */
static inline double clampd(double x, double lo, double hi) { return x != x ? x : x < lo ? lo : x > hi ? hi : x; }

static double load_q(Ctx *c, uint32_t ea, uint32_t ty, int scale, uint32_t *size) {
    switch (ty) {
    case 4: *size = 1; return (double)(uint8_t)ctx_ld(c, ea, 1) * pow2(-scale);
    case 5: *size = 2; return (double)(uint16_t)ctx_ld(c, ea, 2) * pow2(-scale);
    case 6: *size = 1; return (double)(int8_t)(uint8_t)ctx_ld(c, ea, 1) * pow2(-scale);
    case 7: *size = 2; return (double)(int16_t)(uint16_t)ctx_ld(c, ea, 2) * pow2(-scale);
    default: { *size = 4; uint32_t u = (uint32_t)ctx_ld(c, ea, 4); float f; memcpy(&f, &u, 4); return (double)f; }
    }
}

/* Quantise per GQR store type/scale (truncate, then saturate). */
static uint32_t store_q(Ctx *c, uint32_t ea, double v, uint32_t ty, int scale) {
    double x = v * pow2(scale);
    switch (ty) {
    case 4: ctx_st(c, ea, 1, sat_u64(clampd(x, 0.0, 255.0))); return 1;
    case 5: ctx_st(c, ea, 2, sat_u64(clampd(x, 0.0, 65535.0))); return 2;
    case 6: ctx_st(c, ea, 1, (uint64_t)sat_i64(clampd(x, -128.0, 127.0)) & 0xFF); return 1;
    case 7: ctx_st(c, ea, 2, (uint64_t)sat_i64(clampd(x, -32768.0, 32767.0)) & 0xFFFF); return 2;
    default: { float f = (float)v; uint32_t u; memcpy(&u, &f, 4); ctx_st(c, ea, 4, u); return 4; }
    }
}

static inline int sext6(uint32_t v) { return (int32_t)(v << 26) >> 26; }

static void psq_load(Ctx *c, uint32_t fd, uint32_t ea, bool w, uint32_t i) {
    uint32_t g = (uint32_t)RD(c, GQR + 4 * i, 4);
    uint32_t ty = (g >> 16) & 7;
    int scale = sext6((g >> 24) & 0x3F);
    uint32_t size;
    double a = load_q(c, ea, ty, scale, &size);
    double b = 1.0;
    if (!w) { uint32_t s2; b = load_q(c, ea + size, ty, scale, &s2); }
    set(c, fd, a, b);
}

static void psq_store(Ctx *c, uint32_t fs, uint32_t ea, bool w, uint32_t i) {
    uint32_t g = (uint32_t)RD(c, GQR + 4 * i, 4);
    uint32_t ty = g & 7;
    int scale = sext6((g >> 8) & 0x3F);
    double a = ps0(c, fs), b = ps1(c, fs);
    uint32_t size = store_q(c, ea, a, ty, scale);
    if (!w) store_q(c, ea + size, b, ty, scale);
}

static inline uint64_t cmp(double a, double b) {
    if (a != a || b != b) return 1;
    return a < b ? 8 : a > b ? 4 : 2;
}

__attribute__((cold, noinline)) static void ps_unknown(uint32_t w) { rt_log("ps: unhandled encoding %#010x", w); }

void ctx_ps(Ctx *c, uint32_t w) {
    uint32_t op = w >> 26, d = (w >> 21) & 31, a = (w >> 16) & 31, b = (w >> 11) & 31;
    if (op == 56 || op == 57 || op == 60 || op == 61) {
        uint32_t off = (uint32_t)((int32_t)((w & 0xFFF) << 20) >> 20);
        uint32_t base = (a == 0 && (op == 56 || op == 60)) ? 0 : ctx_gpr(c, a);
        uint32_t ea = base + off;
        bool wb = (w >> 15) & 1;
        uint32_t i = (w >> 12) & 7;
        if (op < 60) psq_load(c, d, ea, wb, i);
        else psq_store(c, d, ea, wb, i);
        if (op == 57 || op == 61) ctx_set_gpr(c, a, ea);
        return;
    }
    if (op != 4) { ps_unknown(w); return; }
    /* Indexed quantised loads/stores (XO in bits 1-6). */
    uint32_t xo6 = (w >> 1) & 0x3F;
    if (xo6 == 6 || xo6 == 7 || xo6 == 38 || xo6 == 39) {
        uint32_t base = (a == 0 && xo6 < 32) ? 0 : ctx_gpr(c, a);
        uint32_t ea = base + ctx_gpr(c, b);
        bool wb = (w >> 10) & 1;
        uint32_t i = (w >> 7) & 7;
        if (!(xo6 & 1)) psq_load(c, d, ea, wb, i);
        else psq_store(c, d, ea, wb, i);
        if (xo6 >= 32) ctx_set_gpr(c, a, ea);
        return;
    }
    uint32_t xo5 = (w >> 1) & 0x1F;
    double a0 = ps0(c, a), a1 = ps1(c, a), b0 = ps0(c, b), b1 = ps1(c, b);
    if (xo5 == 0 || xo5 == 8 || xo5 == 16 || xo5 == 22) {
        switch ((w >> 1) & 0x3FF) {
        case 0: case 32: WR(c, CR + (d >> 2), 1, cmp(a0, b0)); break;
        case 64: case 96: WR(c, CR + (d >> 2), 1, cmp(a1, b1)); break;
        case 40: set(c, d, -b0, -b1); break;
        case 72: set(c, d, b0, b1); break;
        case 136: set(c, d, -__builtin_fabs(b0), -__builtin_fabs(b1)); break;
        case 264: set(c, d, __builtin_fabs(b0), __builtin_fabs(b1)); break;
        case 528: set(c, d, a0, b0); break;
        case 560: set(c, d, a0, b1); break;
        case 592: set(c, d, a1, b0); break;
        case 624: set(c, d, a1, b1); break;
        case 1014: {
            uint32_t base = a == 0 ? 0 : ctx_gpr(c, a);
            uint64_t ea = (base + ctx_gpr(c, b)) & ~31u;
            for (int k = 0; k < 4; k++) ctx_st(c, ea + 8 * k, 8, 0);
            break;
        }
        default: ps_unknown(w);
        }
        return;
    }
    uint32_t cr = (w >> 6) & 31;
    double c0 = ps0(c, cr), c1 = ps1(c, cr), r0, r1;
    switch (xo5) {
    case 10: r0 = a0 + b1; r1 = c1; break;
    case 11: r0 = c0; r1 = a0 + b1; break;
    case 12: r0 = a0 * c0; r1 = a1 * c0; break;
    case 13: r0 = a0 * c1; r1 = a1 * c1; break;
    case 14: r0 = a0 * c0 + b0; r1 = a1 * c0 + b1; break;
    case 15: r0 = a0 * c1 + b0; r1 = a1 * c1 + b1; break;
    case 18: r0 = a0 / b0; r1 = a1 / b1; break;
    case 20: r0 = a0 - b0; r1 = a1 - b1; break;
    case 21: r0 = a0 + b0; r1 = a1 + b1; break;
    case 23: set(c, d, a0 >= 0.0 ? c0 : b0, a1 >= 0.0 ? c1 : b1); return;
    case 24: r0 = 1.0 / b0; r1 = 1.0 / b1; break;
    case 25: r0 = a0 * c0; r1 = a1 * c1; break;
    case 26: r0 = 1.0 / __builtin_sqrt(b0); r1 = 1.0 / __builtin_sqrt(b1); break;
    case 28: r0 = a0 * c0 - b0; r1 = a1 * c1 - b1; break;
    case 29: r0 = a0 * c0 + b0; r1 = a1 * c1 + b1; break;
    case 30: r0 = -(a0 * c0 - b0); r1 = -(a1 * c1 - b1); break;
    case 31: r0 = -(a0 * c0 + b0); r1 = -(a1 * c1 + b1); break;
    default: ps_unknown(w); return;
    }
    set(c, d, single(r0), single(r1));
}
