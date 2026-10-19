/* Native versions of hot guest functions (see native.h) and their verifier. */
#include "native.h"
#include <math.h>
#include <stdlib.h>
#include "rt.h"
#include "plat.h"
#include "table.h"
#include "gx.h"

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
/* Gekko single-precision multiply(-add): exact product in double, one rounding to single. */
ALWAYS_INLINE float madd(float a, float b, float c) { return (float)((double)a * b + c); }
ALWAYS_INLINE float mul(float a, float b) { return (float)((double)a * b); }
ALWAYS_INLINE uint64_t dbits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

#define ARGS uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7
#define UNUSED_ARGS (void)a0, (void)a1, (void)a2, (void)a3, (void)a4, (void)a5, (void)a6, (void)a7

/* Paired singles as the recompiled code computes them: each lane in double, rounded to single. */
typedef struct { double a, b; } P;
ALWAYS_INLINE double rs(double x) { return (double)(float)x; }
ALWAYS_INLINE P pl(Ctx *c, uint8_t *M, uint32_t ea) { return (P){ldf(c, M, ea), ldf(c, M, ea + 4)}; }  /* psq_l w=0 */
ALWAYS_INLINE P pl1(Ctx *c, uint8_t *M, uint32_t ea) { return (P){ldf(c, M, ea), 1.0}; }                 /* psq_l w=1 */
ALWAYS_INLINE P pmul(P x, P y) { return (P){rs(x.a * y.a), rs(x.b * y.b)}; }
ALWAYS_INLINE P pmuls0(P x, P y) { return (P){rs(x.a * y.a), rs(x.b * y.a)}; }
ALWAYS_INLINE P pmadd(P x, P y, P z) { return (P){rs(x.a * y.a + z.a), rs(x.b * y.b + z.b)}; }
ALWAYS_INLINE P pmsub(P x, P y, P z) { return (P){rs(x.a * y.a - z.a), rs(x.b * y.b - z.b)}; }
ALWAYS_INLINE P pnmadd(P x, P y, P z) { return (P){rs(-(x.a * y.a + z.a)), rs(-(x.b * y.b + z.b))}; }
ALWAYS_INLINE P pnmsub(P x, P y, P z) { return (P){rs(-(x.a * y.a - z.a)), rs(-(x.b * y.b - z.b))}; }
ALWAYS_INLINE P padd(P x, P y) { return (P){rs(x.a + y.a), rs(x.b + y.b)}; }
ALWAYS_INLINE P psum0(P x, P y, P z) { return (P){rs(x.a + y.b), rs(z.b)}; }
ALWAYS_INLINE P pmuls1(P x, P y) { return (P){rs(x.a * y.b), rs(x.b * y.b)}; }
ALWAYS_INLINE P pmadds0(P x, P y, P z) { return (P){rs(x.a * y.a + z.a), rs(x.b * y.a + z.b)}; }
ALWAYS_INLINE P psum1(P x, P y, P z) { return (P){rs(y.a), rs(x.a + z.b)}; } /* ps_sum1 d,x,y,z */
ALWAYS_INLINE P psub(P x, P y) { return (P){rs(x.a - y.a), rs(x.b - y.b)}; }
ALWAYS_INLINE P m01(P x, P y) { return (P){x.a, y.b}; }
ALWAYS_INLINE P sc(double v) { return (P){v, v}; } /* scalar op result: ps1 = ps0 */
ALWAYS_INLINE P m00(P x, P y) { return (P){x.a, y.a}; }
ALWAYS_INLINE P m10(P x, P y) { return (P){x.b, y.a}; }
ALWAYS_INLINE P m11(P x, P y) { return (P){x.b, y.b}; }
ALWAYS_INLINE void ps_st(Ctx *c, uint8_t *M, uint32_t ea, P v) { stf(c, M, ea, (float)v.a); stf(c, M, ea + 4, (float)v.b); }
ALWAYS_INLINE void ps_st1(Ctx *c, uint8_t *M, uint32_t ea, P v) { stf(c, M, ea, (float)v.a); }

/* Two lanes of double arithmetic for the matrix natives: NEON on AArch64, SSE2 on x86-64 (so the
 * PC verifier checks the same operation sequence the Switch runs). Every op is IEEE double;
 * d2_rs rounds each lane to single, as the recompiled code does after every multiply-add. */
#if defined(__aarch64__)
#include <arm_neon.h>
#define HAVE_D2 1
typedef float64x2_t D2;
ALWAYS_INLINE D2 d2_ld(const float *p) { return vcvt_f64_f32(vld1_f32(p)); }
ALWAYS_INLINE D2 d2_dup(float x) { return vdupq_n_f64((double)x); }
ALWAYS_INLINE D2 d2_mul(D2 a, D2 b) { return vmulq_f64(a, b); }
ALWAYS_INLINE D2 d2_add(D2 a, D2 b) { return vaddq_f64(a, b); }
ALWAYS_INLINE D2 d2_rs(D2 a) { return vcvt_f64_f32(vcvt_f32_f64(a)); }
ALWAYS_INLINE void d2_st(float *p, D2 a) { vst1_f32(p, vcvt_f32_f64(a)); }
#elif defined(__SSE2__)
#include <emmintrin.h>
#define HAVE_D2 1
typedef __m128d D2;
ALWAYS_INLINE D2 d2_ld(const float *p) { return _mm_set_pd((double)p[1], (double)p[0]); }
ALWAYS_INLINE D2 d2_dup(float x) { return _mm_set1_pd((double)x); }
ALWAYS_INLINE D2 d2_mul(D2 a, D2 b) { return _mm_mul_pd(a, b); }
ALWAYS_INLINE D2 d2_add(D2 a, D2 b) { return _mm_add_pd(a, b); }
ALWAYS_INLINE D2 d2_rs(D2 a) { return _mm_cvtps_pd(_mm_cvtpd_ps(a)); }
ALWAYS_INLINE void d2_st(float *p, D2 a) {
    double t[2];
    _mm_storeu_pd(t, a);
    p[0] = (float)t[0];
    p[1] = (float)t[1];
}
#endif

/* ---- MTX library (paired-single SDK assembly; results rounded to single after every
 * multiply-add, in the assembly's accumulation order; outputs may alias inputs) ---- */
static uint64_t i_PSMTXCopy(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    float v[12];
    ldfn(c, M, (uint32_t)a0, v, 12);
    stfn(c, M, (uint32_t)a1, v, 12);
    return 0;
}
/* PSMTXConcat on host copies (o may alias a or b) */
static void mtx_concat_h(const float *a, const float *b, float *out) {
    float o[12];
#ifdef HAVE_D2
    D2 B[3][2];
    for (int k = 0; k < 3; k++) {
        B[k][0] = d2_ld(b + 4 * k);
        B[k][1] = d2_ld(b + 4 * k + 2);
    }
    for (int r = 0; r < 3; r++) {
        D2 x = d2_dup(a[4 * r]), y = d2_dup(a[4 * r + 1]), z = d2_dup(a[4 * r + 2]);
        for (int h = 0; h < 2; h++) {
            D2 v = d2_rs(d2_mul(x, B[0][h]));
            v = d2_rs(d2_add(d2_mul(y, B[1][h]), v));
            v = d2_rs(d2_add(d2_mul(z, B[2][h]), v));
            d2_st(o + 4 * r + 2 * h, v);
        }
        o[4 * r + 3] = (float)((double)o[4 * r + 3] + a[4 * r + 3]);
    }
#else
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 4; k++) {
            float v = mul(a[4 * r], b[k]);
            v = madd(a[4 * r + 1], b[4 + k], v);
            v = madd(a[4 * r + 2], b[8 + k], v);
            if (k == 3) v = (float)((double)v + a[4 * r + 3]);
            o[4 * r + k] = v;
        }
#endif
    memcpy(out, o, sizeof o);
}
static uint64_t i_PSMTXConcat(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    /* MKDD_SLOW_NS=n (test): spin n ns per call, to load the game thread like the Switch does */
    static long slow = -1;
    if (slow < 0) slow = plat_getenv("MKDD_SLOW_NS") ? atol(plat_getenv("MKDD_SLOW_NS")) : 0;
    if (slow) { uint64_t t = plat_now_ns() + (uint64_t)slow; while (plat_now_ns() < t) {} }
    float a[12], b[12], o[12];
    ldfn(c, M, (uint32_t)a0, a, 12);
    ldfn(c, M, (uint32_t)a1, b, 12);
    mtx_concat_h(a, b, o);
    stfn(c, M, (uint32_t)a2, o, 12);
    return 0;
}
static uint64_t i_PSMTXMultVec(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t m = (uint32_t)a0, o = (uint32_t)a2;
    P v01 = pl(c, M, (uint32_t)a1), v2 = pl1(c, M, (uint32_t)a1 + 8);
    /* each row's result is stored before the next row is loaded (o may alias m or v) */
    for (int i = 0; i < 3; i++) {
        P t = pmadd(pl(c, M, m + 0x10 * i + 8), v2, pmul(pl(c, M, m + 0x10 * i), v01));
        ps_st1(c, M, o + 4 * i, psum0(t, t, t));
    }
    return 0;
}
static uint64_t i_PSVECCrossProduct(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    P b01 = pl(c, M, (uint32_t)a1), a01 = pl(c, M, (uint32_t)a0);
    double az = ldf(c, M, (uint32_t)a0 + 8), bz = ldf(c, M, (uint32_t)a1 + 8);
    P f6 = m10(b01, b01);
    P f4 = pmul(b01, (P){az, az});
    P f7 = pmuls0(b01, a01);
    P f5 = pmsub(a01, (P){bz, bz}, f4);
    P f8 = pmsub(a01, f6, f7);
    float x = (float)f5.b, y = (float)-f5.a, z = (float)-f8.b;
    uint32_t o = (uint32_t)a2;
    stf(c, M, o, x);
    stf(c, M, o + 4, y);
    stf(c, M, o + 8, z);
    return 0;
}
static uint64_t i_PSVECMag(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t r2 = (uint32_t)RD(c, REG_R2, 4), v = (uint32_t)a0;
    double half = ldf(c, M, r2 - 0x7408);
    P xy = pl(c, M, v);
    double z = ldf(c, M, v + 8);
    P sq = pmul(xy, xy);
    P t = pmadd((P){z, z}, (P){z, z}, sq);
    double d = rs(t.a + sq.b);
    if (d == rs(half - half)) return dbits(d);
    double e = 1.0 / sqrt(d); /* frsqrte as recompiled */
    double e2 = rs(e * e), eh = rs(e * half);
    double k = rs(-(e2 * d - (double)ldf(c, M, r2 - 0x7404)));
    return dbits(rs(d * rs(k * eh)));
}
static uint64_t i_PSMTXInverse(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t s = (uint32_t)a0, o = (uint32_t)a1;
    P f0 = pl1(c, M, s), f1 = pl(c, M, s + 4), f2 = pl1(c, M, s + 0x10);
    P f6 = m10(f1, f0);
    P f3 = pl(c, M, s + 0x14), f4 = pl1(c, M, s + 0x20);
    P f7 = m10(f3, f2);
    P f5 = pl(c, M, s + 0x24);
    P f11 = pmul(f3, f6), f13 = pmul(f5, f7);
    P f8 = m10(f5, f4);
    f11 = pmsub(f1, f7, f11);
    P f12 = pmul(f1, f8);
    f13 = pmsub(f3, f8, f13);
    P f10 = pmul(f3, f4);
    f12 = pmsub(f5, f6, f12);
    P f9 = pmul(f0, f5);
    f8 = pmul(f1, f2);
    f10 = pmsub(f2, f5, f10);
    f7 = pmul(f0, f13);
    f9 = pmsub(f1, f4, f9);
    f7 = pmadd(f2, f12, f7);
    f8 = pmsub(f0, f3, f8);
    f7 = pmadd(f4, f11, f7);
    if (f7.a == rs(f6.a - f6.a)) return 0; /* ps_cmpo0 against ps_sub f6,f6,f6 */
    double r = rs(1.0 / f7.a); /* fres */
    double r2 = rs(r + r), rr = rs(r * r);
    double inv = rs(-(f7.a * rr - r2));
    P k = {inv, inv};
    f13 = pmuls0(f13, k);
    f12 = pmuls0(f12, k);
    f11 = pmuls0(f11, k);
    f10 = pmuls0(f10, k);
    f9 = pmuls0(f9, k);
    f8 = pmuls0(f8, k);
    double t0 = ldf(c, M, s + 0xc), t1 = ldf(c, M, s + 0x1c), t2 = ldf(c, M, s + 0x2c);
    P T0 = {t0, t0}, T1 = {t1, t1}, T2 = {t2, t2};
    ps_st(c, M, o, m00(f13, f12));
    P g6 = pnmadd(f11, T2, pmadd(f12, T1, pmul(f13, T0)));
    ps_st(c, M, o + 0x10, m11(f13, f12));
    ps_st1(c, M, o + 0x20, f10);
    ps_st1(c, M, o + 0x24, f9);
    ps_st1(c, M, o + 0x28, f8);
    ps_st(c, M, o + 0x8, m00(f11, g6));
    ps_st(c, M, o + 0x18, m11(f11, g6));
    P g7 = pnmadd(f8, T2, pmadd(f9, T1, pmul(f10, T0)));
    ps_st1(c, M, o + 0x2c, g7);
    return 1;
}

/* PSMTXQuat(Mtx m, Quaternion *q): rotation matrix from a quaternion (paired-single SDK
 * assembly, operation for operation; stores in its order). */
static uint64_t i_PSMTXQuat(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t o = (uint32_t)a0, q = (uint32_t)a1;
    P f1 = sc(ldf(c, M, (uint32_t)RD(c, REG_R2, 4) - 0x7440));
    P f4 = pl(c, M, q), f5 = pl(c, M, q + 8);
    P f0 = sc(rs(f1.a - f1.a)), f2 = sc(rs(f1.a + f1.a));
    P f6 = pmul(f4, f4), f9 = m10(f4, f4);
    P f8 = pmadd(f5, f5, f6), f7 = pmul(f5, f5);
    P f3 = psum0(f8, f8, f8);
    P f10 = pmuls1(f9, f5);
    P f11 = sc(rs(1.0 / f3.a));
    f8 = psum1(f7, f8, f6);
    f3 = pnmsub(f3, f11, f2);
    f7 = pmuls1(f5, f5);
    f3 = pmul(f11, f3);
    f6 = psum0(f6, f6, f6);
    f3 = sc(rs(f3.a * f2.a));
    f11 = pmadd(f4, f9, f7);
    f7 = pmsub(f4, f9, f7);
    ps_st1(c, M, o + 0xc, f0);
    f6 = pnmsub(f6, f3, f1);
    f8 = pnmsub(f8, f3, f1);
    ps_st1(c, M, o + 0x2c, f0);
    f11 = pmul(f11, f3);
    f7 = pmul(f7, f3);
    ps_st1(c, M, o + 0x28, f6);
    f9 = pmadds0(f4, f5, f10);
    f5 = m00(f11, f8);
    f10 = pnmsub(f10, f2, f9);
    f4 = m10(f8, f7);
    ps_st(c, M, o + 0x10, f5);
    f9 = pmul(f9, f3);
    f10 = pmul(f10, f3);
    ps_st(c, M, o, f4);
    ps_st1(c, M, o + 8, f9);
    ps_st(c, M, o + 0x18, m10(f10, f0));
    ps_st(c, M, o + 0x20, m01(f10, f9));
    return 0;
}

/* ---- OS: interrupt enable (MSR.EE). The recompiled mtmsr keeps the old EE bit, so these are
 * always native (hle.c set_ee does the same for the hook path). Result: the previous EE. ---- */
static uint64_t set_ee(Ctx *c, int mode, uint32_t r3) {
    uint32_t msr = (uint32_t)RD(c, REG_MSR, 4);
    uint32_t nw = mode == 0 ? msr & ~(uint32_t)MSR_EE : mode == 1 ? msr | (uint32_t)MSR_EE : (r3 ? msr | (uint32_t)MSR_EE : msr & ~(uint32_t)MSR_EE);
    WR(c, REG_MSR, 4, nw);
    return (msr & MSR_EE) != 0;
}
static uint64_t i_OSDisableInterrupts(Ctx *c, uint8_t *M, ARGS) { (void)M; UNUSED_ARGS; return set_ee(c, 0, 0); }
static uint64_t i_OSEnableInterrupts(Ctx *c, uint8_t *M, ARGS) { (void)M; UNUSED_ARGS; return set_ee(c, 1, 0); }
static uint64_t i_OSRestoreInterrupts(Ctx *c, uint8_t *M, ARGS) { (void)M; UNUSED_ARGS; return set_ee(c, 2, (uint32_t)a0); }

/* ---- J3D ---- */
ALWAYS_INLINE uint32_t ld32(Ctx *c, uint8_t *M, uint32_t a) { return (uint32_t)mld(c, M, a, 4); }
ALWAYS_INLINE uint32_t ld16(Ctx *c, uint8_t *M, uint32_t a) { return (uint32_t)mld(c, M, a, 2); }

/* FUN_8005b564: J3DDeformer position blend (called from J3DDeformer::deform). For each of the
 * cluster's positions: zero it, then add every key's source position, its components negated by
 * the index's top three bits, times the key's weight.
 * a0 deformer, a1 vertex buffer, a2 cluster, a3 cluster keys, a4 weights (f32[]). */
static uint64_t i_FUN_8005b564(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t cl = (uint32_t)a2, key = (uint32_t)a3, wt = (uint32_t)a4, r2 = (uint32_t)RD(c, REG_R2, 4);
    uint32_t data = ld32(c, M, (uint32_t)a0);
    uint32_t dst = ld32(c, M, (uint32_t)a1 + 4);
    uint32_t n = ld16(c, M, cl + 0x12), keys = ld16(c, M, cl + 0x10);
    uint32_t src = ld32(c, M, data + 0x18);
    uint32_t idx = ld32(c, M, cl + 0x18);
    uint32_t zero = ld32(c, M, r2 - 0x79cc);
    for (uint32_t k = 0; k < n; k++) {
        uint32_t d = dst + ld16(c, M, idx + 2 * k) * 12;
        mst(c, M, d, 4, zero);
        mst(c, M, d + 4, 4, zero);
        mst(c, M, d + 8, 4, zero);
    }
    const float sign[2] = {ldf(c, M, r2 - 0x79d4), ldf(c, M, r2 - 0x79d0)};
    /* The original stores the position after every key and reloads it; the sources, weights and
     * key tables are other buffers, so the sums stay in registers (each position is still read
     * after the zeroing pass, so repeated indices add up as before). */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t d = dst + ld16(c, M, idx + 2 * i) * 12;
        float x = ldf(c, M, d), y = ldf(c, M, d + 4), z = ldf(c, M, d + 8);
        for (uint32_t j = 0; j < keys; j++) {
            float w = ldf(c, M, wt + 4 * j);
            uint32_t e = ld16(c, M, ld32(c, M, key + 12 * j + 4) + 2 * i);
            uint32_t s = src + (e & 0x1FFF) * 12;
            x = madd(mul(ldf(c, M, s), sign[(e >> 15) & 1]), w, x);
            y = madd(mul(ldf(c, M, s + 4), sign[(e >> 14) & 1]), w, y);
            z = madd(mul(ldf(c, M, s + 8), sign[(e >> 13) & 1]), w, z);
        }
        stf(c, M, d, x);
        stf(c, M, d + 4, y);
        stf(c, M, d + 8, z);
    }
    return 0;
}

/* J3DAnmTransformFull::getTransform(u16 joint, J3DTransformInfo *out): one value per frame per
 * component; frame < 0 takes the first, frames past the end the last. Table entries (12 bytes per
 * axis x/y/z): u16 count/offset for scale (f32 @0xc), rotation (s16 @0x10), translation (f32 @0x14).
 * Output: scale xyz @0, rotation xyz @0xc, translation xyz @0x14; stored per axis. */
static uint64_t i_getTransform__19J3DAnmTransformFullCFUsP16J3DTransformInfo(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t self = (uint32_t)a0, out = (uint32_t)a2, r2 = (uint32_t)RD(c, REG_R2, 4);
    uint32_t j = ((uint32_t)a1 * 3) & 0xFFFF;
    uint32_t tbl = ld32(c, M, self + 0x20);
    uint32_t ent[3] = {tbl + j * 12, tbl + (j + 1) * 12, tbl + (j + 2) * 12};
    float frame = ldf(c, M, self + 8);
    uint32_t sca = ld32(c, M, self + 0xc), rot = ld32(c, M, self + 0x10), tra = ld32(c, M, self + 0x14);
    if (frame < ldf(c, M, r2 - 0x7a0c)) {
        for (int k = 0; k < 3; k++) {
            stf(c, M, out + 4 * k, ldf(c, M, sca + 4 * ld16(c, M, ent[k] + 2)));
            mst(c, M, out + 0xc + 2 * k, 2, ld16(c, M, rot + 2 * ld16(c, M, ent[k] + 6)));
            stf(c, M, out + 0x14 + 4 * k, ldf(c, M, tra + 4 * ld16(c, M, ent[k] + 0xa)));
        }
        return 0;
    }
    float f = (float)((double)ldf(c, M, r2 - 0x79f8) + frame);
    uint32_t i = (uint32_t)trunc_(dbits(f), 8, 4);
#define PICK(e, o) ({ uint32_t n_ = ld16(c, M, (e) + (o)), off_ = ld16(c, M, (e) + (o) + 2); i >= n_ ? n_ + off_ - 1 : off_ + i; })
    for (int k = 0; k < 3; k++) {
        stf(c, M, out + 4 * k, ldf(c, M, sca + 4 * PICK(ent[k], 0)));
        mst(c, M, out + 0xc + 2 * k, 2, ld16(c, M, rot + 2 * PICK(ent[k], 4)));
        stf(c, M, out + 0x14 + 4 * k, ldf(c, M, tra + 4 * PICK(ent[k], 8)));
    }
#undef PICK
    return 0;
}

/* J3DGetKeyFrameInterpolation<s16>(f32 frame, J3DAnmKeyTableBase *tbl, s16 *keys): Hermite
 * interpolation over s16 keys (frame, value, tangent[s]): 3 per key when tbl->type (+4) is 0,
 * else 4 (in and out tangents). Before the first key: its value; from the last on: the last
 * value; else a binary search for the segment. Keys are read through GQR5 (psq_l) for the
 * interpolation and as plain s16 for the comparisons, like the SDK assembly. */
ALWAYS_INLINE float i16f(Ctx *c, uint8_t *M, uint32_t a) { return (float)(int16_t)ld16(c, M, a); }
ALWAYS_INLINE float msub(float a, float b, float c) { return (float)((double)a * b - c); }
static uint64_t i_J3DGetKeyFrameInterpolation_s___FfP18J3DAnmKeyTableBasePs(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t tbl = (uint32_t)a0, k = (uint32_t)a1;
    float frame = (float)ps_d(a2);
    if (frame < i16f(c, M, k)) return dbits(i16f(c, M, k + 2));
    uint32_t stride = ld16(c, M, tbl + 4) == 0 ? 6 : 8;
    uint32_t n = ld16(c, M, tbl);
    uint32_t last = (n - 1) * stride;
    if (i16f(c, M, k + last) <= frame) return dbits(i16f(c, M, k + last + 2));
    while (n > 1) {
        uint32_t half = n >> 1;
        if (frame >= i16f(c, M, k + half * stride)) { k += half * stride; n -= half; }
        else n = half;
    }
    uint32_t g = (uint32_t)RD(c, REG_GQR0 + 4 * 5, 4);
    double q[6], one;
#define Q(i, off) ps_load(c, M, k + (off), g, 1, &q[i], &one)
    Q(0, 0); Q(1, stride); Q(2, 2); Q(3, stride + 2); Q(4, stride + 4); Q(5, stride == 6 ? 4 : 6);
#undef Q
    float f0f = (float)q[0], f1f = (float)q[1], v0 = (float)q[2], v1 = (float)q[3], t1 = (float)q[4], t0 = (float)q[5];
    float d = f1f - f0f;
    float t = (frame - f0f) / d;
    float dv = v1 - v0;
    float a = madd(t1, d, v0);
    float b = -msub(d, t0, dv);
    float t2 = t * t;
    a = a - v1;
    a = a - b;
    a = t2 * a;
    float r = madd(d, t0, a);
    r = madd(r, t, v0);
    r = madd(b, t2, r);
    return dbits(r - a);
}

/* J3DMtxProjConcat(Mtx a, Mtx44 b, Mtx out): out (3x4) = a (3x4, as rows of 4) * b (4x4), two
 * columns at a time like the paired-single assembly (b is re-read for every pair). */
static uint64_t i_J3DMtxProjConcat__FPA4_fPA4_fPA4_f(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t a = (uint32_t)a0, b = (uint32_t)a1, o = (uint32_t)a2;
    for (int r = 0; r < 3; r++) {
        float ar[4], col[4][2], res[2];
        ldfn(c, M, a + 0x10 * r, ar, 4);
        for (int k = 0; k < 4; k += 2) {
            for (int i = 0; i < 4; i++) ldfn(c, M, b + 0x10 * i + 4 * k, col[i], 2);
#ifdef HAVE_D2
            D2 v = d2_rs(d2_mul(d2_dup(ar[0]), d2_ld(col[0])));
            for (int i = 1; i < 4; i++) v = d2_rs(d2_add(d2_mul(d2_dup(ar[i]), d2_ld(col[i])), v));
            d2_st(res, v);
#else
            for (int h = 0; h < 2; h++) {
                float v = mul(ar[0], col[0][h]);
                v = madd(ar[1], col[1][h], v);
                v = madd(ar[2], col[2][h], v);
                res[h] = madd(ar[3], col[3][h], v);
            }
#endif
            stfn(c, M, o + 0x10 * r + 4 * k, res, 2);
        }
    }
    return 0;
}

/* FUN_80026a1c: out = a * s + b for 3-vectors (paired singles; s is f1 as a double). */
static uint64_t i_FUN_80026a1c(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    double sc = ps_d(a3);
    float a[3], b[3], o[3];
    ldfn(c, M, (uint32_t)a0, a, 3);
    ldfn(c, M, (uint32_t)a1, b, 3);
    for (int i = 0; i < 3; i++) o[i] = (float)((double)a[i] * sc + b[i]);
    stfn(c, M, (uint32_t)a2, o, 3);
    return 0;
}
/* FUN_80026a74: out = in * diag(sx, sy, sz, 1) for a 3x4 matrix (f1..f3 as doubles; the 1 is a
 * constant at r2-0x7cf8). Row pairs are loaded, scaled and stored in the assembly's order. */
static uint64_t i_FUN_80026a74(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t in = (uint32_t)a0, out = (uint32_t)a1;
    double s[4] = {ps_d(a2), ps_d(a3), ps_d(a4), (double)ldf(c, M, (uint32_t)RD(c, REG_R2, 4) - 0x7cf8)};
    float m[12];
    ldfn(c, M, in, m, 12); /* all loads of a column pair happen before its stores; rows don't overlap */
    float o[12];
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 4; k++) o[4 * r + k] = (float)((double)m[4 * r + k] * s[k]);
    /* store order: (0,1) of rows 0-2, then (2,3) of rows 0-2 */
    for (int r = 0; r < 3; r++) stfn(c, M, out + 0x10 * r, o + 4 * r, 2);
    for (int r = 0; r < 3; r++) stfn(c, M, out + 0x10 * r + 8, o + 4 * r + 2, 2);
    return 0;
}

/* FUN_80026880(s16 x, s16 y, s16 z, Quaternion *out): Euler angles to a quaternion with the
 * JMath sin/cos table (0x803A4F00: {sin, cos} per 1/2048 turn), half angles truncated to zero. */
ALWAYS_INLINE uint32_t half_idx(uint32_t v) {
    int32_t h = (int16_t)v / 2; /* srawi 1 + addze: rounds toward zero */
    return ((uint32_t)(int16_t)h & 0xFFFF) >> 5;
}
static uint64_t i_FUN_80026880(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    const uint32_t tbl = 0x803A4F00u;
    uint32_t ix = half_idx((uint32_t)a0) * 8, iy = half_idx((uint32_t)a1) * 8, iz = half_idx((uint32_t)a2) * 8;
    float sx = ldf(c, M, tbl + ix), cx = ldf(c, M, tbl + 4 + ix);
    float sy = ldf(c, M, tbl + iy), cy = ldf(c, M, tbl + 4 + iy);
    float sz = ldf(c, M, tbl + iz), cz = ldf(c, M, tbl + 4 + iz);
    float sxsy = sx * sy, sxcy = sx * cy, cxsy = cx * sy, sysz = sy * sz;
    float szsxcy = sz * sxcy, cycz = cy * cz, cxcy = cx * cy;
    float w = madd(cx, cycz, sx * sysz);
    float x = msub(sx, cycz, cx * sysz);
    float y = madd(cz, cxsy, szsxcy);
    float z = msub(sz, cxcy, cz * sxsy);
    uint32_t o = (uint32_t)a3;
    stf(c, M, o + 0xc, w);
    stf(c, M, o, x);
    stf(c, M, o + 4, y);
    stf(c, M, o + 8, z);
    return 0;
}
/* FUN_80026944(Quaternion *a, Quaternion *b, Quaternion *out, f32 t): out = a + t (b - a), or
 * a - t (a + b) when a.b < 0 (the shorter way round). out may be a. */
static uint64_t i_FUN_80026944(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    float a[4], b[4], o[4];
    ldfn(c, M, (uint32_t)a0, a, 4);
    ldfn(c, M, (uint32_t)a1, b, 4);
    float d0 = madd(a[2], b[2], a[0] * b[0]), d1 = madd(a[3], b[3], a[1] * b[1]);
    float dot = d0 + d1;
    double zero = ps_d(mld(c, M, (uint32_t)RD(c, REG_R2, 4) - 0x7d00, 8));
    double nt = -ps_d(a3);
    if (dot < zero) for (int i = 0; i < 4; i++) o[i] = (float)(nt * (a[i] + b[i]) + a[i]);
    else for (int i = 0; i < 4; i++) o[i] = (float)(nt * (a[i] - b[i]) + a[i]);
    stfn(c, M, (uint32_t)a2, o, 4);
    return 0;
}

/* FUN_801912c4(obj, u32 bit): set (bit set in obj->u16 @8) or clear flag 0x10 of obj->@4->@0x10. */
static uint64_t i_FUN_801912c4(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t o = (uint32_t)a0, sh = (uint32_t)a1 & 63, bit = sh < 32 ? 1u << sh : 0;
    uint32_t t = ld32(c, M, o + 4);
    uint32_t v = ld32(c, M, t + 0x10);
    mst(c, M, t + 0x10, 4, ld16(c, M, o + 8) & bit ? v | 0x10 : v & ~0x10u);
    return 0;
}
/* FUN_80182fa4(mgr, u32 bit): FUN_801912c4(..., bit) on every part of every object. */
static uint64_t i_FUN_80182fa4(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t mgr = (uint32_t)a0;
    for (uint32_t i = 0; i < ld16(c, M, mgr + 4); i++) {
        uint32_t off = (i << 2) & 0x3FFFC;
        uint32_t obj = ld32(c, M, ld32(c, M, mgr + 0xc) + off);
        if (!obj) continue;
        uint32_t n = ld16(c, M, ld32(c, M, ld32(c, M, mgr + 8) + off) + 0x7c);
        for (uint32_t j = 0; j < n; j++)
            i_FUN_801912c4(c, M, ld32(c, M, ld32(c, M, obj + 0xc4) + j * 0x3c + 0xc), a1, 0, 0, 0, 0, 0, 0);
    }
    return 0;
}

/* ---- GX: immediate matrix loads, written to the gather pipe in one go ---- */
static void fifo_from_ram(Ctx *c, uint8_t *M, uint32_t a, unsigned words) {
    if (LIKELY(IS_RAM(a) && IS_RAM(a + 4 * words - 1))) { hw_gather_copy(M + (a & 0x01FFFFFFu), 4 * words); return; }
    for (unsigned i = 0; i < words; i++) hw_gather_write(4, mld(c, M, a + 4 * i, 4));
}
/* GXLoadPosMtxImm(Mtx m, u32 id): XF load of 12 words at id * 4. */
static uint64_t i_GXLoadPosMtxImm(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    hw_gather_write(1, 0x10);
    hw_gather_write(4, (((uint32_t)a1 << 2) & 0xFFFFFFFCu) | 0x000B0000u);
    fifo_from_ram(c, M, (uint32_t)a0, 12);
    return 0;
}
/* GXLoadTexMtxImm(Mtx m, u32 id, GXTexMtxType type): post-transform matrices (id >= 0x40) live
 * at 0x500; type 1 (2x4) loads 8 words, anything else 12 (the data written follows type == 0). */
static uint64_t i_GXLoadTexMtxImm(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t id = (uint32_t)a1, type = (uint32_t)a2;
    uint32_t addr = id >= 0x40 ? (((id - 0x40) << 2) & 0xFFFFFFFCu) + 0x500 : (id << 2) & 0xFFFFFFFCu;
    uint32_t n = type == 1 ? 8 : 12;
    hw_gather_write(1, 0x10);
    hw_gather_write(4, addr | (((n - 1) << 16) & 0xFFFF0000u));
    fifo_from_ram(c, M, (uint32_t)a0, type == 0 ? 12 : 8);
    return 0;
}

/* GXSetArray(GXAttr attr, void *base, u8 stride): CP 0xA0+n (base) and 0xB0+n (stride),
 * n = attr - 9 (GX_VA_NBT counts as GX_VA_NRM); texcoord arrays (n 12..15) are also kept in
 * __GXData (+0x88, +0x98). */
static uint64_t i_GXSetArray(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t attr = (uint32_t)a0 == 0x19 ? 0xa : (uint32_t)a0;
    uint32_t n = attr - 9, base = (uint32_t)a1 & 0x3FFFFFFFu, stride = (uint32_t)a2 & 0xFF;
    int32_t t = (int32_t)(n - 0xc);
    uint32_t gx = ld32(c, M, (uint32_t)RD(c, REG_R2, 4) - 0x73f8);
    uint8_t cmd[6] = {0x08, (uint8_t)(n | 0xa0)};
    put_be(cmd + 2, 4, base);
    hw_gather_copy(cmd, 6);
    if (t >= 0 && t < 4) mst(c, M, gx + 0x88 + 4 * (uint32_t)t, 4, base);
    cmd[1] = (uint8_t)(n | 0xb0);
    put_be(cmd + 2, 4, stride);
    hw_gather_copy(cmd, 6);
    if (t >= 0 && t < 4) mst(c, M, gx + 0x98 + 4 * (uint32_t)t, 4, stride);
    return 0;
}
/* GXSetTevColor(GXTevRegID id, GXColor *c): BP 0xE0+2id (red/alpha) once, 0xE1+2id
 * (blue/green) three times; then __GXData->bpSentNot (u16 @2) = 0. */
static uint64_t i_GXSetTevColor(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t col = (uint32_t)a1, r7 = ((uint32_t)a0 << 1) & 0xFFFFFFFEu;
    uint32_t r = mld(c, M, col, 1), g = mld(c, M, col + 1, 1), b = mld(c, M, col + 2, 1), al = mld(c, M, col + 3, 1);
    uint32_t ra = (r & 0x7FF) | ((al << 12) & 0x007FF000u) | ((r7 + 0xe0) << 24);
    uint32_t bg = (b & 0x7FF) | ((g << 12) & 0x007FF000u) | ((r7 + 0xe1) << 24);
    uint8_t cmd[20] = {0x61};
    put_be(cmd + 1, 4, ra);
    for (int i = 0; i < 3; i++) { cmd[5 + 5 * i] = 0x61; put_be(cmd + 6 + 5 * i, 4, bg); }
    hw_gather_copy(cmd, 20);
    mst(c, M, ld32(c, M, (uint32_t)RD(c, REG_R2, 4) - 0x73f8) + 2, 2, 0);
    return 0;
}
/* GXLoadLightObjImm(GXLightObj *l, GXLightID id): XF 0x600 + 0x10 * light (16 words: 3 zero,
 * colour, 12 floats); then __GXData->bpSentNot = 1. */
static uint64_t i_GXLoadLightObjImm(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t l = (uint32_t)a0, id = (uint32_t)a1;
    uint32_t n = 31 - (uint32_t)(id ? __builtin_clz(id) : 32);
    uint8_t head[21] = {0x10};
    put_be(head + 1, 4, ((((n << 4) & 0x70) + 0x600) | 0x000F0000u));
    put_be(head + 17, 4, ld32(c, M, l + 0xc));
    hw_gather_copy(head, 21);
    fifo_from_ram(c, M, l + 0x10, 12);
    mst(c, M, ld32(c, M, (uint32_t)RD(c, REG_R2, 4) - 0x73f8) + 2, 2, 1);
    return 0;
}

/* ---- calling guest code from a native (flag 1: the caller has stored its registers) ---- */
static void guest2(Ctx *c, uint32_t fn, uint32_t r3, uint32_t r4) {
    ctx_set_gpr(c, 3, r3);
    ctx_set_gpr(c, 4, r4);
    ctx_call(c, fn);
}

static uint64_t i_FUN_8000a1b4(Ctx *c, uint8_t *M, ARGS);
/* J3DMtxCalc::calc (vtable +0x28): the animated one (FUN_8000a1b4) natively. */
static void mtxcalc_calc(Ctx *c, uint8_t *M, uint32_t calc) {
    uint32_t fn = ld32(c, M, ld32(c, M, calc) + 0x28);
    if (fn == 0x8000A1B4u) i_FUN_8000a1b4(c, M, calc, 0, 0, 0, 0, 0, 0, 0);
    else guest2(c, fn, calc, 0);
}

/* J3DJoint::recursiveCalc(): j3dSys's current matrix (0x803AD134, 3x4) and the two scale
 * vectors after it (0x803AD164, 0x803AD170) are saved around the children and restored before
 * the siblings; the joint's J3DMtxCalc (or the inherited one in the SDA slot r13-0x6670, joint
 * in r13-0x6674) runs its calc (vtable +0x28); the joint callback runs before (0) and after (1)
 * the children. */
static uint64_t i_recursiveCalc__8J3DJointFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    const uint32_t CUR = 0x803AD134u, S0 = 0x803AD164u, S1 = 0x803AD170u;
    uint32_t r13 = (uint32_t)RD(c, REG_R13, 4);
    /* the original's 0xb0-byte frame: callees run at the same stack addresses */
    uint32_t old_sp = ctx_gpr(c, 1), sp = old_sp - 0xb0;
    mst(c, M, sp, 4, old_sp);
    ctx_set_gpr(c, 1, sp);
    uint32_t j = (uint32_t)a0;
    {
        uint8_t saved[48];
        uint32_t sv[6];
        for (int i = 0; i < 12; i++) put_be(saved + 4 * i, 4, ld32(c, M, CUR + 4 * i));
        uint32_t prev = 0, calc = ld32(c, M, j + 0x54);
        for (int i = 0; i < 3; i++) { sv[i] = ld32(c, M, S0 + 4 * i); sv[3 + i] = ld32(c, M, S1 + 4 * i); }
        if (calc) {
            prev = ld32(c, M, r13 - 0x6670);
            mst(c, M, r13 - 0x6674, 4, j);
            mst(c, M, r13 - 0x6670, 4, calc);
            mtxcalc_calc(c, M, calc);
        } else {
            uint32_t m = ld32(c, M, r13 - 0x6670);
            if (m) {
                mst(c, M, r13 - 0x6674, 4, j);
                mtxcalc_calc(c, M, m);
            }
        }
        uint32_t cb = ld32(c, M, j + 4);
        if (cb) guest2(c, cb, j, 0);
        uint32_t child = ld32(c, M, j + 0xc);
        if (child) i_recursiveCalc__8J3DJointFv(c, M, child, 0, 0, 0, 0, 0, 0, 0);
        for (int i = 0; i < 12; i++) mst(c, M, CUR + 4 * i, 4, get_be(saved + 4 * i, 4));
        for (int i = 0; i < 3; i++) { mst(c, M, S0 + 4 * i, 4, sv[i]); mst(c, M, S1 + 4 * i, 4, sv[3 + i]); }
        if (prev) mst(c, M, r13 - 0x6670, 4, prev);
        if (cb) guest2(c, cb, j, 1);
        uint32_t sib = ld32(c, M, j + 0x10);
        if (sib) i_recursiveCalc__8J3DJointFv(c, M, sib, 0, 0, 0, 0, 0, 0, 0);
    }
    ctx_set_gpr(c, 1, old_sp);
    return 0;
}

/* FUN_800089e4(Vec *scale, Vec *trans, J3DAnmTransform *anm[4], f32 w[4]): blends up to four
 * animations for the current joint (SDA r13-0x6674; its matrix in r13-0x6678's buffer). One
 * active animation: its rotation straight from the sin/cos table; several: scale and translation
 * weighted, rotation by successive quaternion blends (FUN_80026944), then PSMTXQuat. Its stack
 * frame (0xc0) holds the transform info and quaternions the callees write, as in the original. */
static uint32_t s_gt_full; /* J3DAnmTransformFull::getTransform: called natively */
static void anm_get(Ctx *c, uint8_t *M, uint32_t anm, uint32_t joint, uint32_t info) {
    uint32_t fn = ld32(c, M, ld32(c, M, anm) + 0x10);
    if (fn == s_gt_full) { i_getTransform__19J3DAnmTransformFullCFUsP16J3DTransformInfo(c, M, anm, joint, info, 0, 0, 0, 0, 0); return; }
    ctx_set_gpr(c, 3, anm);
    ctx_set_gpr(c, 4, joint);
    ctx_set_gpr(c, 5, info);
    ctx_call(c, fn);
}
static uint64_t i_FUN_800089e4(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t sca = (uint32_t)a0, tra = (uint32_t)a1, anms = (uint32_t)a2, ws = (uint32_t)a3;
    uint32_t r2 = (uint32_t)RD(c, REG_R2, 4), r13 = (uint32_t)RD(c, REG_R13, 4);
    uint32_t old_sp = ctx_gpr(c, 1), sp = old_sp - 0xc0;
    mst(c, M, sp, 4, old_sp);
    ctx_set_gpr(c, 1, sp);
    uint32_t joint = ld16(c, M, ld32(c, M, r13 - 0x6674) + 0x14);
    uint32_t mtx = ld32(c, M, ld32(c, M, r13 - 0x6678) + 0xc) + joint * 0x30;
    float sum = ldf(c, M, r2 - 0x7f9c), thr = ldf(c, M, r2 - 0x7f98);
    uint32_t n = 0, last = 0;
    for (uint32_t i = 0; i < 4; i++) {
        if (!ld32(c, M, anms + 4 * i)) continue;
        float w = ldf(c, M, ws + 4 * i);
        if (w <= thr) continue; /* ble: NaN weights count */
        sum = sum + w;
        last = i;
        n++;
    }
    if (n == 1) {
        uint32_t info = sp + 0x48;
        anm_get(c, M, ld32(c, M, anms + 4 * last), joint, info);
        for (int k = 0; k < 3; k++) stf(c, M, sca + 4 * k, ldf(c, M, info + 4 * k));
        for (int k = 0; k < 3; k++) stf(c, M, tra + 4 * k, ldf(c, M, info + 0x14 + 4 * k));
        const uint32_t tbl = 0x803A4F00u;
        uint32_t ix = (ld16(c, M, info + 0xc) >> 5) * 8, iy = (ld16(c, M, info + 0xe) >> 5) * 8, iz = (ld16(c, M, info + 0x10) >> 5) * 8;
        float sx = ldf(c, M, tbl + ix), cx = ldf(c, M, tbl + 4 + ix);
        float sy = ldf(c, M, tbl + iy), cy = ldf(c, M, tbl + 4 + iy);
        float sz = ldf(c, M, tbl + iz), cz = ldf(c, M, tbl + 4 + iz);
        float czcy = cz * cy, cysx = cy * sx, sxcz = sx * cz, szcy = sz * cy, cxsz = cx * sz;
        float cycx = cy * cx, sxsz = sx * sz, cxcz = cx * cz;
        stf(c, M, mtx + 0x20, -sy);
        stf(c, M, mtx + 0x00, czcy);
        stf(c, M, mtx + 0x10, szcy);
        stf(c, M, mtx + 0x24, cysx);
        stf(c, M, mtx + 0x28, cycx);
        stf(c, M, mtx + 0x04, msub(sxcz, sy, cxsz));
        stf(c, M, mtx + 0x18, msub(cxsz, sy, sxcz));
        stf(c, M, mtx + 0x08, madd(cxcz, sy, sxsz));
        stf(c, M, mtx + 0x14, madd(sxsz, sy, cxcz));
    } else {
        float zero = ldf(c, M, r2 - 0x7f9c);
        float inv = (float)(1.0 / sum); /* fres */
        float acc_w = zero;
        for (int k = 2; k >= 0; k--) stf(c, M, sca + 4 * k, zero);
        for (int k = 2; k >= 0; k--) stf(c, M, tra + 4 * k, zero);
        for (int k = 3; k >= 0; k--) stf(c, M, sp + 0x18 + 4 * k, zero);
        for (uint32_t i = 0; (int32_t)i <= (int32_t)last; i++) {
            uint32_t anm = ld32(c, M, anms + 4 * i);
            if (!anm) continue;
            float w = ldf(c, M, ws + 4 * i);
            if (w <= ldf(c, M, r2 - 0x7f98)) continue;
            float wn = w * inv;
            uint32_t info = sp + 0x28;
            anm_get(c, M, anm, joint, info);
            uint64_t wb = dbits(wn);
            i_FUN_80026a1c(c, M, info, sca, sca, wb, 0, 0, 0, 0);
            i_FUN_80026a1c(c, M, sp + 0x3c, tra, tra, wb, 0, 0, 0, 0);
            i_FUN_80026880(c, M, (uint32_t)(int32_t)(int16_t)ld16(c, M, info + 0xc), (uint32_t)(int32_t)(int16_t)ld16(c, M, info + 0xe),
                           (uint32_t)(int32_t)(int16_t)ld16(c, M, info + 0x10), sp + 8, 0, 0, 0, 0);
            acc_w = acc_w + wn;
            float t = wn * (float)(1.0 / acc_w); /* fres */
            i_FUN_80026944(c, M, sp + 0x18, sp + 8, sp + 0x18, dbits(t), 0, 0, 0, 0);
        }
        i_PSMTXQuat(c, M, mtx, sp + 0x18, 0, 0, 0, 0, 0, 0);
    }
    ctx_set_gpr(c, 1, old_sp);
    return 0;
}

/* ---- CrsGround (course collision) ---- */
/* Moving ground (obj->@0x4dc->@4->u8 @4 set): vectors go through the ground's matrix. */
ALWAYS_INLINE bool crs_moving(Ctx *c, uint8_t *M, uint32_t obj) { return mld(c, M, ld32(c, M, ld32(c, M, obj + 0x4dc) + 4) + 4, 1) != 0; }

/* FUN_8017b8bc(obj, Vec *out, Vec *in): out = obj translation (0x1ec) + in.x * col (0x1bc)
 * + in.y * col (0x1b0) + in.z * col (0x1a4) on a moving ground, else out = in. */
static uint64_t i_FUN_8017b8bc(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t obj = (uint32_t)a0, out = (uint32_t)a1, in = (uint32_t)a2;
    if (crs_moving(c, M, obj)) {
        float t0 = ldf(c, M, obj + 0x1ec);
        float x = ldf(c, M, in);
        stf(c, M, out, t0);
        stf(c, M, out + 4, ldf(c, M, obj + 0x1f0));
        stf(c, M, out + 8, ldf(c, M, obj + 0x1f4));
        i_FUN_80026a1c(c, M, obj + 0x1bc, out, out, dbits(x), 0, 0, 0, 0);
        i_FUN_80026a1c(c, M, obj + 0x1b0, out, out, dbits(ldf(c, M, in + 4)), 0, 0, 0, 0);
        i_FUN_80026a1c(c, M, obj + 0x1a4, out, out, dbits(ldf(c, M, in + 8)), 0, 0, 0, 0);
    } else {
        float x = ldf(c, M, in), y = ldf(c, M, in + 4);
        stf(c, M, out, x);
        float z = ldf(c, M, in + 8);
        stf(c, M, out + 4, y);
        stf(c, M, out + 8, z);
    }
    return 0;
}

/* FUN_8017b96c(obj, f32 plane[4], Vec *p, f64 nx, ny, nz, d): on a moving ground the normal
 * goes through the ground's rotation and is renormalised (when its squared length is above
 * eps), and d = -(n . p); otherwise plane = (nx, ny, nz, d). */
static uint64_t i_FUN_8017b96c(Ctx *c, uint8_t *M, ARGS) {
    uint32_t obj = (uint32_t)a0, out = (uint32_t)a1, pt = (uint32_t)a2;
    double nx = ps_d(a3), ny = ps_d(a4), nz = ps_d(a5);
    if (!crs_moving(c, M, obj)) {
        stf(c, M, out, (float)nx);
        stf(c, M, out + 4, (float)ny);
        stf(c, M, out + 8, (float)nz);
        stf(c, M, out + 0xc, (float)ps_d(a6));
        return 0;
    }
    uint32_t r2 = (uint32_t)RD(c, REG_R2, 4);
    double zero = ldf(c, M, r2 - 0x5e90), one = ldf(c, M, r2 - 0x5e8c);
    P n01 = {(float)nx, (float)ny};
    P f3 = psub((P){(float)nz, 1.0}, (P){zero, 1.0});
    P f4 = {ldf(c, M, obj + 0x1b8), 1.0};
    P f5 = padd(f4, f3);
    P d = psub(n01, (P){zero, one});
    double eps = rs((double)ldf(c, M, r2 - 0x5e74) * ldf(c, M, 0x803C9A1Cu));
    P f6 = pl(c, M, obj + 0x1b0);
    double z = (float)f5.a;
    P v = padd(f6, d);
    P sq = pmul(v, v);
    P t = pmadd((P){z, z}, (P){z, z}, sq);
    double l2 = rs(t.a + sq.b);
    double vx = v.a, vy = v.b;
    if (!(l2 <= eps)) {
        double k;
        if (l2 <= zero) k = l2;
        else {
            double e = rs(1.0 / sqrt(l2)); /* frsqrte + frsp */
            double e2 = rs(e * e), eh = rs((double)ldf(c, M, r2 - 0x5e70) * e);
            k = rs(eh * rs(-(l2 * e2 - (double)ldf(c, M, r2 - 0x5e6c))));
        }
        vx = rs(vx * k);
        vy = rs(vy * k);
        z = rs(z * k);
    }
    stf(c, M, out, (float)vx);
    stf(c, M, out + 4, (float)vy);
    float py = ldf(c, M, pt + 4);
    stf(c, M, out + 8, (float)z);
    float px = ldf(c, M, pt), o1 = ldf(c, M, out + 4), o0 = ldf(c, M, out), o2 = ldf(c, M, out + 8), pz = ldf(c, M, pt + 8);
    float acc = madd(o0, px, o1 * py);
    stf(c, M, out + 0xc, (float)-((double)o2 * pz + acc));
    return 0;
}

/* Edge/offset vector normalisation used by FUN_8017ddd4: unchanged when the squared length is
 * at most eps, scaled by the length squared itself when that is at most c84 (as the assembly
 * does), else by a Newton-refined 1/sqrt. */
static void crs_norm(Ctx *c, uint8_t *M, uint32_t r2, double eps, double *x, double *y, double *z) {
    P v = {*x, *y};
    P sq = pmul(v, v);
    P t = pmadd((P){*z, *z}, (P){*z, *z}, sq);
    double l2 = rs(t.a + sq.b);
    if (l2 <= eps) return;
    double k;
    if (l2 <= (double)ldf(c, M, r2 - 0x5d84)) k = l2;
    else {
        double e = rs(1.0 / sqrt(l2));
        double e2 = rs(e * e), eh = rs((double)ldf(c, M, r2 - 0x5d74) * e);
        k = rs(eh * rs(-(l2 * e2 - (double)ldf(c, M, r2 - 0x5d70))));
    }
    *x = rs(*x * k);
    *y = rs(*y * k);
    *z = rs(*z * k);
}

/* FUN_8017b8bc / FUN_8017b96c on host values (no aliasing between input and output). */
static void crs_xform_h(Ctx *c, uint8_t *M, uint32_t obj, uint32_t in, float o[3]) {
    if (!crs_moving(c, M, obj)) { for (int k = 0; k < 3; k++) o[k] = ldf(c, M, in + 4 * k); return; }
    for (int k = 0; k < 3; k++) o[k] = ldf(c, M, obj + 0x1ec + 4 * k);
    static const uint32_t col[3] = {0x1bc, 0x1b0, 0x1a4};
    for (int a = 0; a < 3; a++) {
        double s = ldf(c, M, in + 4 * a);
        for (int k = 0; k < 3; k++) o[k] = (float)((double)ldf(c, M, obj + col[a] + 4 * k) * s + o[k]);
    }
}
static void crs_plane_h(Ctx *c, uint8_t *M, uint32_t obj, const float pt[3], double nx, double ny, double nz, double d, float o[4]) {
    if (!crs_moving(c, M, obj)) { o[0] = (float)nx; o[1] = (float)ny; o[2] = (float)nz; o[3] = (float)d; return; }
    uint32_t r2 = (uint32_t)RD(c, REG_R2, 4);
    double zero = ldf(c, M, r2 - 0x5e90), one = ldf(c, M, r2 - 0x5e8c);
    P f3 = psub((P){(float)nz, 1.0}, (P){zero, 1.0});
    P f5 = padd((P){ldf(c, M, obj + 0x1b8), 1.0}, f3);
    P dd = psub((P){(float)nx, (float)ny}, (P){zero, one});
    double eps = rs((double)ldf(c, M, r2 - 0x5e74) * ldf(c, M, 0x803C9A1Cu));
    double z = (float)f5.a;
    P v = padd(pl(c, M, obj + 0x1b0), dd);
    P sq = pmul(v, v);
    P t = pmadd((P){z, z}, (P){z, z}, sq);
    double l2 = rs(t.a + sq.b), vx = v.a, vy = v.b;
    if (!(l2 <= eps)) {
        double k;
        if (l2 <= zero) k = l2;
        else {
            double e = rs(1.0 / sqrt(l2));
            double e2 = rs(e * e), eh = rs((double)ldf(c, M, r2 - 0x5e70) * e);
            k = rs(eh * rs(-(l2 * e2 - (double)ldf(c, M, r2 - 0x5e6c))));
        }
        vx = rs(vx * k); vy = rs(vy * k); z = rs(z * k);
    }
    o[0] = (float)vx; o[1] = (float)vy; o[2] = (float)z;
    float acc = madd(o[0], pt[0], o[1] * pt[1]);
    o[3] = (float)-((double)o[2] * pt[2] + acc);
}

/* FUN_8017ddd4(tri, f32 plane[4] *out, Vec *pos, -, Vec *verts): is pos over course triangle tri?
 * Vertices through the (moving) ground; x/z bounds from the triangle's extreme-vertex indices;
 * below the plane by more than the tolerance (base + 4-bit step) fails; then each edge's cross
 * product y with the vertex-to-pos vector must pass the type's test (types 2 and 0x12: > c84,
 * else >= c60). On success the plane is copied to out. Returns 1/0. The original keeps the
 * vertices and plane in its stack frame (dead after the call); here they are host values. */
static uint64_t i_FUN_8017ddd4(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t tri = (uint32_t)a0, out = (uint32_t)a1, pos = (uint32_t)a2, verts = (uint32_t)a4;
    uint32_t r2 = (uint32_t)RD(c, REG_R2, 4), r13 = (uint32_t)RD(c, REG_R13, 4);
    uint32_t obj = ld32(c, M, ld32(c, M, r13 - 0x5c38) + 0x44);
    uint32_t step = (mld(c, M, tri + 0x20, 1) >> 4) & 0xF;
    float tol = ldf(c, M, r13 - 0x79b0);
    if (step) tol = madd(ldf(c, M, r2 - 0x5d6c), (float)rs((double)step), tol);
    uint32_t b = (uint32_t)mld(c, M, tri + 0x18, 1);
    uint32_t ix0 = b & 3, iz0 = (b >> 2) & 3, ix1 = (b >> 4) & 3, iz1 = (b >> 6) & 3;
    float V[3][3], pl4[4];
    for (uint32_t i = 0; i < 3; i++) crs_xform_h(c, M, obj, verts + ld32(c, M, tri + 4 * i) * 12, V[i]);
    float px = ldf(c, M, pos);
    if (px < V[ix0][0]) return 0;
    if (!(px <= V[ix1][0])) return 0;
    float pz = ldf(c, M, pos + 8);
    if (pz < V[iz0][2]) return 0;
    if (!(pz <= V[iz1][2])) return 0;
    double ns = ldf(c, M, r2 - 0x5d68);
    double nx = rs(ns * (float)(int16_t)ld16(c, M, tri + 0x10));
    double ny = rs(ns * (float)(int16_t)ld16(c, M, tri + 0x12));
    double nz = rs(ns * (float)(int16_t)ld16(c, M, tri + 0x14));
    crs_plane_h(c, M, obj, V[0], nx, ny, nz, ldf(c, M, tri + 0xc), pl4);
    float py = ldf(c, M, pos + 4);
    px = ldf(c, M, pos);
    pz = ldf(c, M, pos + 8);
    float ax = pl4[0] * px, cz = pl4[2] * pz;
    float dist = pl4[3] + (cz + madd(pl4[1], py, ax));
    if (dist < ldf(c, M, r2 - 0x5d64)) {
        float h = ((float)-(pl4[3] + (ax + cz)) / pl4[1]) - py;
        if (!(h <= tol)) return 0;
    }
    double eps = rs((double)ldf(c, M, r2 - 0x5d78) * ldf(c, M, 0x803C9A1Cu));
    uint32_t type = (uint32_t)mld(c, M, tri + 0x16, 1);
    double qx = ldf(c, M, pos), qy = ldf(c, M, pos + 4), qz = ldf(c, M, pos + 8);
    uint32_t ok = 0;
    for (uint32_t i = 0; i < 3; i++) {
        const float *vi = V[i], *vj = V[(i + 1) % 3];
        double ex = rs((double)vj[0] - vi[0]), ey = rs((double)vj[1] - vi[1]), ez = rs((double)vj[2] - vi[2]);
        crs_norm(c, M, r2, eps, &ex, &ey, &ez);
        double wx = rs(qx - vi[0]), wy = rs(qy - vi[1]), wz = rs(qz - vi[2]);
        crs_norm(c, M, r2, eps, &wx, &wy, &wz);
        double cy = rs(ez * wx - rs(ex * wz));
        bool pass;
        if (type == 0x12 || type == 2) pass = !(cy <= (double)ldf(c, M, r2 - 0x5d84));
        else pass = !(cy < (double)ldf(c, M, r2 - 0x5d60));
        if (!pass) break;
        if (i == 2) { ok = 1; break; }
    }
    if (ok) for (int k = 0; k < 4; k++) stf(c, M, out + 4 * k, pl4[k]);
    return ok;
}

/* ---- batch 2: more J3D / vector math (paired-single and scalar SDK code, operation for
 * operation as recompiled: every single-precision result rounded through double) ---- */
#define SINCOS_TBL 0x803A4F00u /* JMath sin/cos table: {sin, cos} per 1/2048 turn */

/* PSVECNormalize(src, dst) */
static uint64_t i_PSVECNormalize(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t s = (uint32_t)a0, d = (uint32_t)a1, r2 = (uint32_t)RD(c, REG_R2, 4);
    double half = ldf(c, M, r2 - 0x7408), three = ldf(c, M, r2 - 0x7404);
    P f2 = pl(c, M, s);
    P f5 = pmul(f2, f2);
    P f3 = pl1(c, M, s + 8);
    P f4 = pmadd(f3, f3, f5);
    f4 = psum0(f4, f5, f3); /* ps_sum0 f4,f4,f3,f5: f4.a + f5.b */
    double e = 1.0 / sqrt(f4.a); /* frsqrte as recompiled */
    double e2 = rs(e * e), eh = rs(e * half);
    double k = rs(-(e2 * f4.a - three));
    double s5 = rs(k * eh);
    P K = {s5, s5};
    ps_st(c, M, d, pmuls0(f2, K));
    ps_st1(c, M, d + 8, pmuls0(f3, K));
    return 0;
}

/* J3DPSCalcInverseTranspose(Mtx src, Mtx33 dst): 0 if singular */
static uint64_t i_J3DPSCalcInverseTranspose__FPA4_fPA3_f(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t s = (uint32_t)a0, o = (uint32_t)a1;
    P f0 = pl1(c, M, s), f1 = pl(c, M, s + 4), f2 = pl1(c, M, s + 0x10);
    P f6 = m10(f1, f0);
    P f3 = pl(c, M, s + 0x14), f4 = pl1(c, M, s + 0x20);
    P f7 = m10(f3, f2);
    P f5 = pl(c, M, s + 0x24);
    P f11 = pmul(f3, f6);
    P f8 = m10(f5, f4);
    P f13 = pmul(f5, f7);
    f11 = pmsub(f1, f7, f11);
    P f12 = pmul(f1, f8);
    f13 = pmsub(f3, f8, f13);
    f12 = pmsub(f5, f6, f12);
    P f10 = pmul(f3, f4), f9 = pmul(f0, f5);
    f8 = pmul(f1, f2);
    f10 = pmsub(f2, f5, f10);
    f9 = pmsub(f1, f4, f9);
    f8 = pmsub(f0, f3, f8);
    f7 = pmul(f0, f13);
    double zero = rs(f1.a - f1.a);
    f7 = pmadd(f2, f12, f7);
    f7 = pmadd(f4, f11, f7);
    if (f7.a == zero) return 0;
    double r = rs(1.0 / f7.a); /* fres */
    for (int it = 0; it < 2; it++) {
        double r2 = rs(r + r), rr = rs(r * r);
        r = rs(-(f7.a * rr - r2));
    }
    P k = {r, r};
    ps_st(c, M, o, pmuls0(f13, k));
    ps_st(c, M, o + 0xc, pmuls0(f12, k));
    ps_st(c, M, o + 0x18, pmuls0(f11, k));
    ps_st1(c, M, o + 0x8, pmuls0(f10, k));
    ps_st1(c, M, o + 0x14, pmuls0(f9, k));
    ps_st1(c, M, o + 0x20, pmuls0(f8, k));
    return 1;
}

ALWAYS_INLINE uint32_t sc_idx(Ctx *c, uint8_t *M, uint32_t a) { return SINCOS_TBL + ((ld16(c, M, a) & 0xFFFFu) >> 5) * 8; }

/* J3DGetTranslateRotateMtx(const J3DTransformInfo &, Mtx): rotation from the s16 angles (x, y,
 * z at +0xc) via the sin/cos table, translation from +0x14 */
static uint64_t i_J3DGetTranslateRotateMtx__FRC16J3DTransformInfoPA4_f(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t t = (uint32_t)a0, o = (uint32_t)a1;
    uint32_t ix = sc_idx(c, M, t + 0xc), iy = sc_idx(c, M, t + 0xe), iz = sc_idx(c, M, t + 0x10);
    double sy = ldf(c, M, iy), cy = ldf(c, M, iy + 4), cz = ldf(c, M, iz + 4), sx = ldf(c, M, ix), cx = ldf(c, M, ix + 4);
    double sz = ldf(c, M, iz);
    double f3 = rs(cz * cy), f4 = rs(cy * sx), f12 = rs(sx * cz);
    double tx = ldf(c, M, t + 0x14);
    stf(c, M, o + 0x20, (float)-sy);
    double f11 = rs(cx * sz);
    double ty = ldf(c, M, t + 0x18);
    double f0 = rs(sz * cy);
    stf(c, M, o + 0x0, (float)f3);
    f3 = rs(cy * cx);
    double f6 = rs(f12 * sy - f11);
    stf(c, M, o + 0x10, (float)f0);
    double f9 = rs(sx * sz), f7 = rs(cx * cz);
    double tz = ldf(c, M, t + 0x1c);
    stf(c, M, o + 0x24, (float)f4);
    double f5 = rs(f11 * sy - f12);
    stf(c, M, o + 0x28, (float)f3);
    double g4 = rs(f7 * sy + f9), g3 = rs(f9 * sy + f7);
    stf(c, M, o + 0x4, (float)f6);
    stf(c, M, o + 0x18, (float)f5);
    stf(c, M, o + 0x8, (float)g4);
    stf(c, M, o + 0x14, (float)g3);
    stf(c, M, o + 0xc, (float)tx);
    stf(c, M, o + 0x1c, (float)ty);
    stf(c, M, o + 0x2c, (float)tz);
    return 0;
}

/* J3DGetTextureMtx / J3DGetTextureMtxOld(const J3DTextureSRTInfo &, const Vec &center, Mtx):
 * scale (+0, +4), s16 rotation (+8), translation (+0xc, +0x10). The two differ only in where
 * the translation column goes (old: column 3, new: column 2). */
static void tex_mtx(Ctx *c, uint8_t *M, uint32_t srt, uint32_t ctr, uint32_t o, bool old) {
    uint32_t r2 = (uint32_t)RD(c, REG_R2, 4);
    uint32_t i = sc_idx(c, M, srt + 8);
    double sx = ldf(c, M, srt), cy_ = ldf(c, M, ctr + 4), sy = ldf(c, M, srt + 4), cx_ = ldf(c, M, ctr);
    double sn = ldf(c, M, i), cs = ldf(c, M, i + 4);
    double f10 = rs(sx * sn), tx = ldf(c, M, srt + 0xc), f2 = rs(sx * cs), ty = ldf(c, M, srt + 0x10);
    double f11 = rs(sy * sn);
    double zero = ldf(c, M, r2 - 0x7a90), one = ldf(c, M, r2 - 0x7a8c);
    stf(c, M, o, (float)f2);
    double p = rs(f10 * cy_);
    double f12 = rs(sy * cs);
    double f5 = rs(-f2 * cx_ + p);
    stf(c, M, o + 4, (float)-f10);
    double q = rs(f12 * cy_);
    f5 = rs(cx_ + f5);
    q = rs(-f11 * cx_ - q);
    double f3 = rs(tx + f5);
    q = rs(cy_ + q);
    stf(c, M, o + (old ? 0xc : 0x8), (float)f3);
    stf(c, M, o + 0x10, (float)f11);
    q = rs(ty + q);
    stf(c, M, o + 0x14, (float)f12);
    stf(c, M, o + (old ? 0x1c : 0x18), (float)q);
    stf(c, M, o + 0x2c, (float)zero);
    stf(c, M, o + 0x24, (float)zero);
    stf(c, M, o + 0x20, (float)zero);
    stf(c, M, o + (old ? 0x18 : 0x1c), (float)zero);
    stf(c, M, o + (old ? 0x8 : 0xc), (float)zero);
    stf(c, M, o + 0x28, (float)one);
}
static uint64_t i_J3DGetTextureMtx__FRC17J3DTextureSRTInfoRC3VecPA4_f(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    tex_mtx(c, M, (uint32_t)a0, (uint32_t)a1, (uint32_t)a2, false);
    return 0;
}
static uint64_t i_J3DGetTextureMtxOld__FRC17J3DTextureSRTInfoRC3VecPA4_f(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    tex_mtx(c, M, (uint32_t)a0, (uint32_t)a1, (uint32_t)a2, true);
    return 0;
}

/* J3DGetKeyFrameInterpolation<f>(frame, J3DAnmKeyTableBase *, f32 *keys): keys of 3 floats
 * (time, value, tangent) or 4 (time, value, in, out); Hermite between the bracketing keys. */
static uint64_t i_J3DGetKeyFrameInterpolation_f___FfP18J3DAnmKeyTableBasePf(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t tbl = (uint32_t)a0, k = (uint32_t)a1;
    double frame = ps_d(a2);
    if (!(frame >= (double)ldf(c, M, k))) return dbits(ldf(c, M, k + 4));
    uint32_t stride = ld16(c, M, tbl + 4) == 0 ? 0xc : 0x10;
    uint32_t n = ld16(c, M, tbl);
    uint32_t last = (n - 1) * stride;
    double lt = ldf(c, M, k + last);
    if (lt < frame || lt == frame) return dbits(ldf(c, M, k + last + 4));
    while (n > 1) {
        uint32_t half = n >> 1;
        if (frame >= (double)ldf(c, M, k + half * stride)) { k += half * stride; n -= half; }
        else n = half;
    }
    double t0 = ldf(c, M, k), t1 = ldf(c, M, k + stride);
    double f5 = rs(frame - t0);
    double v0 = ldf(c, M, k + 4);
    double f4 = rs(t1 - t0);
    double v1 = ldf(c, M, k + stride + 4);
    double f7 = ldf(c, M, k + stride - 4), f8 = ldf(c, M, k + stride + 8);
    double t = rs(f5 / f4);
    double t2 = rs(t * t);
    double dv = rs(v0 - v1);
    double tt = rs(t + t);
    double u = rs(t2 - t);
    double h = rs(tt * u - t2);
    double a = rs(f7 * u + f7);
    h = rs(h * dv + v0);
    a = rs(f8 * u + a);
    a = rs(t * f7 - a);
    return dbits(rs(-(f5 * a - h)));
}

/* J3DMaterial::calcCurrentMtx(): packs the 8 tex coords' matrix ids (byte 2, or the low byte
 * of the halfword at 4 when j3dSys flag 0x40000000 is set) into +0x40 / +0x44. The tex-gen
 * block's getTexCoord (vtable +0x48) is a trivial accessor: run natively when it is one of the
 * two known ones. */
static uint32_t texcoord(Ctx *c, uint8_t *M, uint32_t mat, uint32_t i) {
    uint32_t blk = ld32(c, M, mat + 0x28);
    uint32_t fn = ld32(c, M, ld32(c, M, blk) + 0x48);
    if (fn == 0x800740D4u) return blk + 8 + i * 6; /* J3DTexGenBlockPatched */
    if (fn == 0x80053714u) return 0;              /* J3DTexGenBlock (base) */
    guest2(c, fn, blk, i);
    return ctx_gpr(c, 3);
}
static uint64_t i_calcCurrentMtx__11J3DMaterialFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t mat = (uint32_t)a0;
    bool alt = ld32(c, M, 0x803ACF90u + 0x34) & 0x40000000u;
    uint32_t v[8];
    for (int i = 7; i >= 0; i--) {
        uint32_t a = texcoord(c, M, mat, (uint32_t)i);
        v[i] = alt ? (ld16(c, M, a + 4) & 0xFF) : (uint32_t)mld(c, M, a + 2, 1);
    }
    mst(c, M, mat + 0x40, 4, (v[0] << 6) | (v[1] << 12) | (v[2] << 18) | (v[3] << 24));
    mst(c, M, mat + 0x44, 4, v[4] | (v[5] << 6) | (v[6] << 12) | (v[7] << 18));
    return 0;
}

/* GD display-list writes: the SDK's GDWrite* store one byte at a time through the current list
 * (r13-0x6238, write pointer at +8); the bytes are big-endian guest memory either way. */
static void gd_put(Ctx *c, uint8_t *M, const uint8_t *b, uint32_t n) {
    uint32_t gd = ld32(c, M, (uint32_t)RD(c, REG_R13, 4) - 0x6238), p = ld32(c, M, gd + 8);
    if (LIKELY(IS_RAM(p) && IS_RAM(p + n - 1))) memcpy(M + (p & 0x01FFFFFFu), b, n);
    else for (uint32_t i = 0; i < n; i++) mst(c, M, p + i, 1, b[i]);
    mst(c, M, gd + 8, 4, p + n);
}
/* FUN_800546a4(Mtx m, u32 id): XF load of a 3x4 matrix (12 words) at XF 0x500 + (id - 0x40) * 4
 * (the post-transform texture matrices) into the GD list. */
static uint64_t i_FUN_800546a4(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t m = (uint32_t)a0, addr = (((uint32_t)a1 - 0x40) * 4 + 0x500) & 0xFFFF;
    uint8_t b[53] = {0x10, 0x00, 0x0b, (uint8_t)(addr >> 8), (uint8_t)addr};
    for (int i = 0; i < 12; i++) put_be(b + 5 + 4 * i, 4, (uint32_t)mld(c, M, m + 4 * i, 4));
    gd_put(c, M, b, sizeof b);
    return 0;
}

/* ---- collision planes ---- */
/* stMakePlaneParam's work: normalises n (guest memory) in place when its squared length is
 * above a threshold; pl = {n, -n.p}. False (pl untouched) when |n| is 0. */
static bool plane_param(Ctx *c, uint8_t *M, uint32_t n, uint32_t p, float out[4]) {
    uint32_t r2 = (uint32_t)RD(c, REG_R2, 4);
    double k1 = ldf(c, M, r2 - 0x4680), zero = ldf(c, M, r2 - 0x467c);
    P f4 = pl(c, M, n);
    double g = ldf(c, M, 0x803C9A1Cu);
    f4 = pmul(f4, f4);
    double z = ldf(c, M, n + 8);
    double lim = rs(k1 * g);
    double m2 = psum0(pmadd((P){z, z}, (P){z, z}, f4), f4, f4).a;
    if (!(m2 < lim || m2 == lim)) {
        double s;
        if (m2 < zero || m2 == zero) s = m2;
        else {
            double e = rs(1.0 / sqrt(m2)); /* frsqrte + frsp */
            double half = ldf(c, M, r2 - 0x4678), three = ldf(c, M, r2 - 0x4674);
            s = rs(rs(half * e) * rs(-(m2 * rs(e * e) - three)));
        }
        P S = {s, s};
        P v01 = pl(c, M, n), v2 = pl1(c, M, n + 8);
        ps_st(c, M, n, pmuls0(v01, S));
        ps_st1(c, M, n + 8, pmuls0(v2, S));
    }
    double mag = ps_d(i_PSVECMag(c, M, n, 0, 0, 0, 0, 0, 0, 0));
    if (zero == mag) return false;
    P q12 = pl(c, M, p + 4), q01 = pl(c, M, p);
    out[0] = ldf(c, M, n);
    out[1] = ldf(c, M, n + 4);
    out[2] = ldf(c, M, n + 8);
    P n12 = pl(c, M, n + 4), n01 = pl(c, M, n);
    P f3 = pmul(n12, q12);
    out[3] = (float)-psum0(pmadd(n01, q01, f3), f3, f3).a;
    return true;
}
static uint64_t i_stMakePlaneParam__FR12stPlaneParamRQ29JGeometry8TVec3_f_RCQ29JGeometry8TVec3_f_(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    float v[4];
    if (!plane_param(c, M, (uint32_t)a1, (uint32_t)a2, v)) return 0;
    stfn(c, M, (uint32_t)a0, v, 4);
    return 1;
}

/* FUN_8017b7ac(obj, TVec3f *out, const TVec3f *in): with the object's plane mode on
 * (*(*(obj+0x4dc)+4)+4), projects in onto the plane through obj+0x1ec with normal obj+0x1b0
 * (normalised in place) along y, then expresses it in the 2D frame (obj+0x1bc.., obj+0x1a4..)
 * around obj+0x1ec; else copies in. */
/* On host values (in is read before out is written, which the original's order also gives when
 * they alias). */
static void crs_proj(Ctx *c, uint8_t *M, uint32_t o, const float in[3], float out[3]) {
    uint32_t r2 = (uint32_t)RD(c, REG_R2, 4);
    if (!mld(c, M, ld32(c, M, ld32(c, M, o + 0x4dc) + 4) + 4, 1)) {
        out[0] = in[0];
        out[1] = in[1];
        out[2] = in[2];
        return;
    }
    /* the plane is in the original's stack frame; a degenerate normal leaves it unwritten there
     * (stale stack; never seen with course planes): zero here */
    float plv[4] = {0, 0, 0, 0};
    plane_param(c, M, o + 0x1b0, o + 0x1ec, plv);
    double px = plv[0], py = plv[1], pz = plv[2], pd = plv[3];
    double ix = in[0], iz = in[2];
    double t = rs(px * ix + rs(pz * iz));
    t = rs(pd + t);
    double y = rs(-t / py);
    P o01 = pl(c, M, o + 0x1ec), o2 = pl1(c, M, o + 0x1f4);
    P a = pl(c, M, o + 0x1bc), b = pl(c, M, o + 0x1c0);
    double k = ldf(c, M, r2 - 0x5e90);
    P d2 = psub((P){iz, 1.0}, o2);           /* (iz - oz, 0) */
    P d01 = psub((P){ix, y}, o01);           /* (dx, dy) */
    P d12 = {d01.b, (double)(float)d2.a};    /* psq_l from the stack: (dy, dz) */
    P f2 = pmul(d12, b);
    P f5 = pmadd(d01, a, f2);
    out[0] = (float)psum0(f5, f2, f2).a;
    out[1] = (float)k;
    P c1 = pl(c, M, o + 0x1a8), c0 = pl(c, M, o + 0x1a4);
    P f4 = pmul(d12, c1);
    P g2 = pmadd(d01, c0, f4);
    out[2] = (float)psum0(g2, f4, f4).a;
}
static uint64_t i_FUN_8017b7ac(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t out = (uint32_t)a1, in = (uint32_t)a2;
    float v[3] = {ldf(c, M, in), ldf(c, M, in + 4), ldf(c, M, in + 8)}, o[3];
    crs_proj(c, M, (uint32_t)a0, v, o);
    stf(c, M, out, o[0]);
    stf(c, M, out + 4, o[1]);
    stf(c, M, out + 8, o[2]);
    return 0;
}

/* FUN_80054080(Mtx m, u32 id, u32 type): XF load of a texture matrix at XF id * 4 into the GD
 * list; the header counts 8 words for type 1 (2x4) else 12, but only type 0 writes all 12 */
static uint64_t i_FUN_80054080(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t m = (uint32_t)a0, addr = ((uint32_t)a1 * 4) & 0xFFFF, type = (uint32_t)a2;
    uint32_t cnt = ((type == 1 ? 8u : 12u) - 1) & 0xFFFF, nw = type == 0 ? 12 : 8;
    uint8_t b[5 + 48] = {0x10, (uint8_t)(cnt >> 8), (uint8_t)cnt, (uint8_t)(addr >> 8), (uint8_t)addr};
    for (uint32_t i = 0; i < nw; i++) put_be(b + 5 + 4 * i, 4, (uint32_t)mld(c, M, m + 4 * i, 4));
    gd_put(c, M, b, 5 + 4 * nw);
    return 0;
}

/* GDPadCurr32(): zero-fill the GD list to the next 32-byte boundary */
static uint64_t i_GDPadCurr32(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t gd = ld32(c, M, (uint32_t)RD(c, REG_R13, 4) - 0x6238);
    uint32_t n = ld32(c, M, gd + 8) & 31;
    if (!n) return 0;
    static const uint8_t z[32];
    gd_put(c, M, z, 32 - n);
    return 0;
}

/* BP writes of a TEV colour register pair (0xE0 + 2 id: low, 0xE1 + 2 id: high) into the GD
 * list, after GDOverflowCheck (guest GDOverflowed when the list would overrun its end). */
static void gd_reserve(Ctx *c, uint8_t *M, uint32_t n) {
    uint32_t gd = ld32(c, M, (uint32_t)RD(c, REG_R13, 4) - 0x6238);
    if (ld32(c, M, gd + 8) + n > ld32(c, M, gd + 0xc)) ctx_call(c, 0x800C5ABCu); /* GDOverflowed */
}
static void bp_bytes(uint8_t *b, uint32_t v) { b[0] = 0x61; put_be(b + 1, 4, v); }
/* FUN_800671c8(u32 id, const GXColor *col): konst colour (bit 23 set), 8-bit channels */
static uint64_t i_FUN_800671c8(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t r = (uint32_t)a0 * 2, col = (uint32_t)a1;
    uint32_t c0 = (uint32_t)mld(c, M, col, 1), c1 = (uint32_t)mld(c, M, col + 1, 1);
    uint32_t c2 = (uint32_t)mld(c, M, col + 2, 1), c3 = (uint32_t)mld(c, M, col + 3, 1);
    gd_reserve(c, M, 10);
    uint8_t b[10];
    bp_bytes(b, (c0 | c3 << 12 | 0x800000u) | (r + 0xe0) << 24);
    bp_bytes(b + 5, (c2 | c1 << 12 | 0x800000u) | (r + 0xe1) << 24);
    gd_put(c, M, b, 10);
    return 0;
}
/* FUN_80067338(u32 id, const GXColorS10 *col): 11-bit channels; the high register is written
 * three times (the hardware needs the repeats) */
static uint64_t i_FUN_80067338(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t r = (uint32_t)a0 * 2, col = (uint32_t)a1;
    uint32_t c3 = ld16(c, M, col + 6) & 0x7ff, c1 = ld16(c, M, col + 2) & 0x7ff;
    uint32_t c0 = ld16(c, M, col) & 0x7ff, c2 = ld16(c, M, col + 4) & 0x7ff;
    gd_reserve(c, M, 20);
    uint8_t b[20];
    uint32_t hi = (c2 | c1 << 12) | (r + 0xe1) << 24;
    bp_bytes(b, (c0 | c3 << 12) | (r + 0xe0) << 24);
    bp_bytes(b + 5, hi);
    bp_bytes(b + 10, hi);
    bp_bytes(b + 15, hi);
    gd_put(c, M, b, 20);
    return 0;
}

/* FUN_80067a64(Mtx m, u32 id): GXLoadPosMtxImm through byte/halfword pipe writes (same bytes) */
static uint64_t i_FUN_80067a64(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint8_t h[5] = {0x10, 0x00, 0x0b, (uint8_t)(((uint32_t)a1 * 4) >> 8), (uint8_t)((uint32_t)a1 * 4)};
    hw_gather_copy(h, 5);
    fifo_from_ram(c, M, (uint32_t)a0, 12);
    return 0;
}
/* FUN_80067ae8(Mtx m, u32 id): normal matrix (the 3x3 part of m) at XF 0x400 + id * 3 */
static uint64_t i_FUN_80067ae8(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t m = (uint32_t)a0, addr = ((uint32_t)a1 * 3 + 0x400) & 0xFFFF;
    uint8_t b[5 + 36] = {0x10, 0x00, 0x08, (uint8_t)(addr >> 8), (uint8_t)addr};
    static const uint8_t off[9] = {0x0, 0x4, 0x8, 0x10, 0x14, 0x18, 0x20, 0x24, 0x28};
    for (int i = 0; i < 9; i++) put_be(b + 5 + 4 * i, 4, (uint32_t)mld(c, M, m + off[i], 4));
    hw_gather_copy(b, sizeof b);
    return 0;
}

/* ---- view-frustum culling of course / object models ---- */
/* FUN_8000a3c8 (J3DUClipper::clip(Mtx view, Vec *pos, f32 r)): 1 if the sphere is outside the
 * near/far range or any of the 4 side planes (clipper +4, 12 bytes each). */
static uint32_t uclip(Ctx *c, uint8_t *M, uint32_t cl, uint32_t m, const float v[3], double r) {
    P v01 = {v[0], v[1]}, v2 = {v[2], 1.0};
    double p[3];
    for (int i = 0; i < 3; i++) { /* PSMTXMultVec */
        P t = pmadd(pl(c, M, m + 0x10 * i + 8), v2, pmul(pl(c, M, m + 0x10 * i), v01));
        p[i] = (float)psum0(t, t, t).a;
    }
    double nz = -p[2];
    if (!(nz >= rs((double)ldf(c, M, cl + 0x54) - r))) return 1;
    if (!(nz <= rs((double)ldf(c, M, cl + 0x58) + r))) return 1;
    for (int k = 0; k < 4; k++) {
        uint32_t q = cl + 4 + 0xc * k;
        double d = rs(p[1] * ldf(c, M, q + 4));
        d = rs(p[0] * ldf(c, M, q) + d);
        d = rs(p[2] * ldf(c, M, q + 8) + d);
        if (k < 3 ? !(d <= r) : d > r) return 1;
    }
    return 0;
}
static uint64_t i_FUN_8000a3c8(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t v = (uint32_t)a2;
    float p[3] = {ldf(c, M, v), ldf(c, M, v + 4), ldf(c, M, v + 8)};
    return uclip(c, M, (uint32_t)a0, (uint32_t)a1, p, ps_d(a3));
}

/* FUN_80183afc(obj, u32 bit, u16 idx, bool show): sets (show = 0) or clears the hide bit in the
 * shape flags (+8 of each draw packet's shape, via the material's packet list) of material idx
 * in every model of obj's list, or in obj+0x1a for empty slots. Returns early when the
 * material is flagged (+0x16 & 0xf). */
static void vis_bits(Ctx *c, uint8_t *M, uint32_t obj, uint32_t bit, uint32_t idx, uint32_t show) {
    if (!ld32(c, M, ld32(c, M, obj + 8))) return;
    uint32_t mask = bit & 0x20 ? 0 : 1u << (bit & 31), off = (idx & 0xFFFF) * 4;
    for (uint32_t i = 0; (i & 0xFFFF) < ld16(c, M, obj + 4); i++) {
        uint32_t mdl = ld32(c, M, ld32(c, M, obj + 0xc) + (i & 0xFFFF) * 4);
        if (!mdl) {
            uint32_t h = ld16(c, M, obj + 0x1a);
            mst(c, M, obj + 0x1a, 2, (show & 0xFF) ? h & ~mask : h | mask);
            continue;
        }
        uint32_t mat = ld32(c, M, ld32(c, M, ld32(c, M, mdl + 4) + 0x28) + off);
        if (mld(c, M, mat + 0x16, 1) & 0xF) return;
        for (uint32_t n = ld32(c, M, mat + 0x58); n; n = ld32(c, M, n + 4)) {
            uint32_t sh = ld32(c, M, ld32(c, M, mdl + 0xc4) + ld16(c, M, ld32(c, M, n + 8) + 8) * 0x3c + 0xc);
            uint32_t h = ld16(c, M, sh + 8);
            mst(c, M, sh + 8, 2, (show & 0xFF) ? h & ~mask : h | mask);
        }
    }
}
static uint64_t i_FUN_80183afc(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    vis_bits(c, M, (uint32_t)a0, (uint32_t)a1, (uint32_t)a2, (uint32_t)a3);
    return 0;
}

/* FUN_80183934(obj, u32 bit, clipper, Mtx view, f32 scale): clip each material's bounding
 * sphere (centre from the model's joint matrices, radius * scale) when culling is on
 * (r13-0x7988) and update the hide bits; with no models, the object's own sphere and +0x1a. */
static uint64_t i_FUN_80183934(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t obj = (uint32_t)a0, bit = (uint32_t)a1, cl = (uint32_t)a2, m = (uint32_t)a3;
    double s = ps_d(a4);
    uint32_t r13 = (uint32_t)RD(c, REG_R13, 4);
    uint32_t d = ld32(c, M, ld32(c, M, obj + 8));
    if (!d) return 0;
    if (ld32(c, M, ld32(c, M, obj + 0xc))) {
        for (uint32_t i = 0; (i & 0xFFFF) < ld16(c, M, d + 0x2c); i++) {
            uint32_t hidden = 0;
            if (mld(c, M, r13 - 0x7988, 1)) {
                uint32_t mat = ld32(c, M, ld32(c, M, d + 0x28) + (i & 0xFFFF) * 4);
                if (mld(c, M, mat + 0x16, 1) & 0xF) continue;
                uint32_t j = ld32(c, M, ld32(c, M, ld32(c, M, ld32(c, M, obj + 0xc)) + 0x84) + 0xc) + (i & 0xFFFF) * 0x30;
                double r = rs(s * ldf(c, M, mat + 0x38));
                float p[3] = {ldf(c, M, j + 0xc), ldf(c, M, j + 0x1c), ldf(c, M, j + 0x2c)};
                hidden = uclip(c, M, cl, m, p, r);
            }
            vis_bits(c, M, obj, bit, i, hidden == 0);
        }
    } else {
        uint32_t hidden = 0;
        if (mld(c, M, r13 - 0x7988, 1)) {
            float p[3] = {ldf(c, M, obj + 0x38), ldf(c, M, obj + 0x48), ldf(c, M, obj + 0x58)};
            double r = rs(s * ldf(c, M, ld32(c, M, ld32(c, M, d + 0x28)) + 0x38));
            hidden = uclip(c, M, cl, m, p, r);
        }
        uint32_t mask = bit & 0x20 ? 0 : 1u << (bit & 31), h = ld16(c, M, obj + 0x1a);
        mst(c, M, obj + 0x1a, 2, hidden ? h | mask : h & ~mask);
    }
    return 0;
}

/* ---- course areas (shadow / camera / sound zones): local coordinates and weights ---- */
/* FUN_8017cea8(area, f32 out[3], const Vec *pos): pos in the area's frame (origin +4, axes
 * +0x10.. +0x28..); inside test: type (+0x24 of its data) 1 = cylinder (radius +0x34), else box
 * (half extents +0x34 / +0x3c); height 0..+0x38. */
static uint32_t area_local(Ctx *c, uint8_t *M, uint32_t a, uint32_t pos, float out[3]) {
    uint32_t r2 = (uint32_t)RD(c, REG_R2, 4);
    P d01 = psub(pl(c, M, pos), pl(c, M, a + 4));
    P d2 = psub(pl1(c, M, pos + 8), pl1(c, M, a + 0xc));
    P d12 = {d01.b, d2.a};
    static const uint32_t ax[3][2] = {{0x28, 0x2c}, {0x1c, 0x20}, {0x10, 0x14}};
    for (int k = 0; k < 3; k++) {
        P q = pmul(d12, pl(c, M, a + ax[k][1]));
        out[k] = (float)psum0(pmadd(d01, pl(c, M, a + ax[k][0]), q), q, q).a;
    }
    double x = out[0], y = out[1], z = out[2], lo = ldf(c, M, r2 - 0x5e44);
    if (mld(c, M, ld32(c, M, a) + 0x24, 1) == 1) {
        double R = ldf(c, M, a + 0x34);
        if (!(rs(rs(x * x) + rs(z * z)) <= rs(R * R))) return 0;
        if (y < lo) return 0;
        return y <= (double)ldf(c, M, a + 0x38);
    }
    double R = ldf(c, M, a + 0x34);
    if (x < -R || !(x <= R)) return 0;
    if (y < lo || !(y <= (double)ldf(c, M, a + 0x38))) return 0;
    double Z = ldf(c, M, a + 0x3c);
    if (z < -Z) return 0;
    return z <= Z;
}
static uint64_t i_FUN_8017cea8(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    float o[3];
    uint32_t r = area_local(c, M, (uint32_t)a0, (uint32_t)a2, o);
    stfn(c, M, (uint32_t)a1, o, 3);
    return r;
}
/* FUN_8017cffc(area, const f32 local[3]) -> f32: weight 1 inside, fading out over the edge
 * band (+0x40, and +0x44 for boxes along z) */
static double area_weight(Ctx *c, uint8_t *M, uint32_t a, const float v[3]) {
    uint32_t r2 = (uint32_t)RD(c, REG_R2, 4);
    double zero = ldf(c, M, r2 - 0x5e90), one = ldf(c, M, r2 - 0x5e8c);
    if (mld(c, M, ld32(c, M, a) + 0x24, 1) == 1) {
        double R = ldf(c, M, a + 0x34);
        double d2 = rs(rs((double)v[0] * v[0]) + rs((double)v[2] * v[2])), R2 = rs(R * R);
        if (zero == R2) return zero;
        double d = d2 > zero ? rs(1.0 / sqrt(d2) * d2) : d2;
        double r = R2 > zero ? rs(1.0 / sqrt(R2) * R2) : R2;
        double t = rs(d / r), w = ldf(c, M, a + 0x40);
        if (t < rs(one - w)) return one;
        if (t < one || t == one) return rs(rs(one - t) / w);
        return zero;
    }
    double Z = ldf(c, M, a + 0x3c), z = v[2];
    if (!(z > -Z || z == -Z)) return zero;
    if (!(z < Z || z == Z)) return zero;
    double t = rs(rs(z + Z) / rs((double)ldf(c, M, r2 - 0x5e40) * Z)), w = ldf(c, M, a + 0x40);
    if (!(t >= w)) return rs(t / w);
    double q = ldf(c, M, a + 0x44);
    if (t <= q) return one;
    return rs(rs(one - t) / rs(one - q));
}
static uint64_t i_FUN_8017cffc(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    float v[3];
    ldfn(c, M, (uint32_t)a1, v, 3);
    return dbits(area_weight(c, M, (uint32_t)a0, v));
}
/* FUN_8018af28(Result *res, u8 kind, const Vec *pos): res = {pos, area 0, weight default};
 * then the areas of that kind containing pos: the first one (kind != 0) or the one with the
 * largest weight. */
static uint64_t i_FUN_8018af28(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t res = (uint32_t)a0, kind = (uint32_t)a1, pos = (uint32_t)a2;
    uint32_t r2 = (uint32_t)RD(c, REG_R2, 4), r13 = (uint32_t)RD(c, REG_R13, 4);
    float p0 = ldf(c, M, pos), p1 = ldf(c, M, pos + 4);
    stf(c, M, res, p0);
    float p2 = ldf(c, M, pos + 8);
    stf(c, M, res + 4, p1);
    float def = ldf(c, M, r2 - 0x5b70);
    stf(c, M, res + 8, p2);
    mst(c, M, res + 0xc, 4, 0);
    stf(c, M, res + 0x10, def);
    uint32_t mgr = ld32(c, M, r13 - 0x5c38);
    if (!mgr || !(mgr = ld32(c, M, mgr + 0x44))) return 0;
    double best = def;
    int32_t n = (int32_t)ld32(c, M, mgr + 0x524);
    for (int32_t i = 0; i < n; i++) {
        uint32_t area = ld32(c, M, ld32(c, M, ld32(c, M, r13 - 0x5c38) + 0x44) + 0x528) + (uint32_t)i * 0x4c;
        if (mld(c, M, ld32(c, M, area) + 0x25, 1) != (kind & 0xFF)) continue;
        float loc[3];
        if (!area_local(c, M, area, pos, loc)) continue;
        double w = area_weight(c, M, area, loc);
        if (kind & 0xFF) {
            mst(c, M, res + 0xc, 4, area);
            stf(c, M, res + 0x10, (float)w);
            return 0;
        }
        if (!(w <= best)) {
            best = w;
            mst(c, M, res + 0xc, 4, area);
            stf(c, M, res + 0x10, (float)w);
        }
    }
    return 0;
}

/* FUN_8005826c (J3DAnmTransformKey::calcTransform(f32 frame, u16 joint, J3DTransformInfo *)):
 * per axis, scale / rotation / translation from a key table (0x12 bytes per axis: count,
 * offset, tangent mode for each of S, R, T): no keys = default, one key = constant, else the
 * key-frame interpolation. Rotations are s16 shifted left by anm+0x20. */
static uint64_t i_FUN_8005826c(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t anm = (uint32_t)a0, out = (uint32_t)a2, r2 = (uint32_t)RD(c, REG_R2, 4);
    uint64_t fr = a3;
    uint32_t idx = ((uint32_t)a1 * 3) & 0xFFFF, base = ld32(c, M, anm + 0x24);
    uint32_t T[3] = {base + idx * 0x12, base + (idx + 1) * 0x12, base + (idx + 2) * 0x12};
    for (int k = 0; k < 3; k++) {
        uint32_t t = T[k], n = ld16(c, M, t), arr = ld32(c, M, anm + 0xc) + ld16(c, M, t + 2) * 4;
        float v = n == 0 ? ldf(c, M, r2 - 0x7a10) : n == 1 ? ldf(c, M, arr)
                         : (float)ps_d(i_J3DGetKeyFrameInterpolation_f___FfP18J3DAnmKeyTableBasePf(c, M, t, arr, fr, 0, 0, 0, 0, 0));
        stf(c, M, out + 4 * k, v);
    }
    for (int k = 0; k < 3; k++) {
        uint32_t t = T[k] + 6, n = ld16(c, M, t), arr = ld32(c, M, anm + 0x10) + ld16(c, M, t + 2) * 2;
        uint32_t sh = ld32(c, M, anm + 0x20) & 0x3F, v;
        if (n == 0) v = 0;
        else {
            int32_t i = n == 1 ? (int16_t)ld16(c, M, arr)
                               : sat_i32(ps_d(i_J3DGetKeyFrameInterpolation_s___FfP18J3DAnmKeyTableBasePs(c, M, t, arr, fr, 0, 0, 0, 0, 0)));
            v = sh >= 32 ? 0 : (uint32_t)i << sh;
        }
        mst(c, M, out + 0xc + 2 * k, 2, v);
    }
    for (int k = 0; k < 3; k++) {
        uint32_t t = T[k] + 0xc, n = ld16(c, M, t), arr = ld32(c, M, anm + 0x14) + ld16(c, M, t + 2) * 4;
        float v = n == 0 ? ldf(c, M, r2 - 0x7a0c) : n == 1 ? ldf(c, M, arr)
                         : (float)ps_d(i_J3DGetKeyFrameInterpolation_f___FfP18J3DAnmKeyTableBasePf(c, M, t, arr, fr, 0, 0, 0, 0, 0));
        stf(c, M, out + 0x14 + 4 * k, v);
    }
    return 0;
}

/* ---- data cache maintenance: the dcbi / dcbf / dcbst loops are no-ops here (one coherent
 * memory); the flushing ones end with sc (counted). ---- */
static uint64_t i_DCInvalidateRange(Ctx *c, uint8_t *M, ARGS) { (void)c; (void)M; UNUSED_ARGS; gx_mem_dirty((uint32_t)a0, (uint32_t)a1); return 0; }
/* (the flushes also tell the vertex cache which memory the CPU wrote: gx_mem_dirty) */
static uint64_t i_DCFlushRange(Ctx *c, uint8_t *M, ARGS) { (void)c; (void)M; UNUSED_ARGS; if ((uint32_t)a1) g_syscalls++; gx_mem_dirty((uint32_t)a0, (uint32_t)a1); return 0; }
static uint64_t i_DCStoreRange(Ctx *c, uint8_t *M, ARGS) { (void)c; (void)M; UNUSED_ARGS; if ((uint32_t)a1) g_syscalls++; gx_mem_dirty((uint32_t)a0, (uint32_t)a1); return 0; }
static uint64_t i_DCFlushRangeNoSync(Ctx *c, uint8_t *M, ARGS) { (void)c; (void)M; UNUSED_ARGS; gx_mem_dirty((uint32_t)a0, (uint32_t)a1); return 0; }
static uint64_t i_DCStoreRangeNoSync(Ctx *c, uint8_t *M, ARGS) { (void)c; (void)M; UNUSED_ARGS; gx_mem_dirty((uint32_t)a0, (uint32_t)a1); return 0; }
/* GDFlushCurrToMem(): DCFlushRange(list start, length) */
static uint64_t i_GDFlushCurrToMem(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t gd = ld32(c, M, (uint32_t)RD(c, REG_R13, 4) - 0x6238);
    if (ld32(c, M, gd + 4)) g_syscalls++;
    gx_mem_dirty(ld32(c, M, gd), ld32(c, M, gd + 4));
    return 0;
}

/* ---- calling guest code with up to three arguments (flag 1/4 natives); returns r3 ---- */
static uint32_t guest3(Ctx *c, uint32_t fn, uint32_t r3, uint32_t r4, uint32_t r5) {
    ctx_set_gpr(c, 3, r3);
    ctx_set_gpr(c, 4, r4);
    ctx_set_gpr(c, 5, r5);
    ctx_call(c, fn);
    return ctx_gpr(c, 3);
}
static uint32_t guest1(Ctx *c, uint32_t fn, uint32_t r3) {
    ctx_set_gpr(c, 3, r3);
    ctx_call(c, fn);
    return ctx_gpr(c, 3);
}
ALWAYS_INLINE void cp32(Ctx *c, uint8_t *M, uint32_t dst, uint32_t src, int n) {
    for (int i = 0; i < n; i++) mst(c, M, dst + 4 * i, 4, ld32(c, M, src + 4 * i));
}
ALWAYS_INLINE double fres_(double x) { return (double)(float)(1.0 / x); } /* as recompiled */
ALWAYS_INLINE int32_t fctiwz_(double x) { return (int32_t)(uint32_t)trunc_(dbits(x), 8, 4); }

/* ---- J3DMtxCalc per-joint matrices (Basic FUN_80009398, Maya FUN_8005c08c, Softimage
 * FUN_8005be04). The current joint (SDA r13-0x6674, index at +0x14) owns a 3x4 matrix in the
 * matrix buffer (r13-0x6678: +0xc matrices, +4 one "unit scale" byte per joint). j3dSys holds
 * the current matrix (0x803AD134), the accumulated scale (0x803AD164) and the parent's scale
 * (0x803AD170). ---- */
#define J3D_CUR 0x803AD134u
#define J3D_S0 0x803AD164u
#define J3D_S1 0x803AD170u
static uint32_t jnt_mtx(Ctx *c, uint8_t *M, uint32_t *joint, uint32_t *buf, uint32_t *idx) {
    uint32_t r13 = (uint32_t)RD(c, REG_R13, 4);
    *joint = ld32(c, M, r13 - 0x6674);
    *buf = ld32(c, M, r13 - 0x6678);
    *idx = ld16(c, M, *joint + 0x14);
    return ld32(c, M, *buf + 0xc) + *idx * 0x30;
}
/* Unit flag from cmp (x, y, z == one); otherwise mtx's columns scaled by s (FUN_80026a74). */
static void jnt_scale(Ctx *c, uint8_t *M, uint32_t buf, uint32_t idx, uint32_t mtx, uint32_t s, uint32_t cmp, double one) {
    bool unit = ldf(c, M, cmp) == one && ldf(c, M, cmp + 4) == one && ldf(c, M, cmp + 8) == one;
    mst(c, M, ld32(c, M, buf + 4) + idx, 1, unit);
    if (!unit)
        i_FUN_80026a74(c, M, mtx, mtx, dbits(ldf(c, M, s)), dbits(ldf(c, M, s + 4)), dbits(ldf(c, M, s + 8)), 0, 0, 0);
}
/* Maya scale compensation (joint +0x17 == 1): rows divided by the parent's scale (fres). */
static void jnt_maya(Ctx *c, uint8_t *M, uint32_t joint, uint32_t mtx) {
    if (mld(c, M, joint + 0x17, 1) != 1) return;
    double r[3] = {fres_(ldf(c, M, J3D_S1)), fres_(ldf(c, M, J3D_S1 + 4)), fres_(ldf(c, M, J3D_S1 + 8))};
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < 3; i++) {
            uint32_t a = mtx + 0x10 * k + 4 * i;
            stf(c, M, a, (float)((double)ldf(c, M, a) * r[k]));
        }
}
static void jnt_concat(Ctx *c, uint8_t *M, uint32_t mtx) {
    i_PSMTXConcat(c, M, J3D_CUR, mtx, J3D_CUR, 0, 0, 0, 0, 0);
    i_PSMTXCopy(c, M, J3D_CUR, mtx, 0, 0, 0, 0, 0, 0);
}
/* FUN_80009398(Vec *scale, Vec *trans): the animated joint's matrix (rotation already there). */
static uint64_t i_FUN_80009398(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t s = (uint32_t)a0, t = (uint32_t)a1, joint, buf, idx;
    uint32_t mtx = jnt_mtx(c, M, &joint, &buf, &idx);
    double one = ldf(c, M, (uint32_t)RD(c, REG_R2, 4) - 0x7fa0);
    float tx = ldf(c, M, t), ty = ldf(c, M, t + 4);
    stf(c, M, mtx + 0xc, tx);
    float tz = ldf(c, M, t + 8);
    stf(c, M, mtx + 0x1c, ty);
    stf(c, M, mtx + 0x2c, tz);
    jnt_scale(c, M, buf, idx, mtx, s, s, one);
    jnt_maya(c, M, joint, mtx);
    jnt_concat(c, M, mtx);
    float x = ldf(c, M, s), y = ldf(c, M, s + 4), z = ldf(c, M, s + 8);
    stf(c, M, J3D_S1, x);
    stf(c, M, J3D_S1 + 4, y);
    stf(c, M, J3D_S1 + 8, z);
    return 0;
}
/* FUN_8005c08c(J3DTransformInfo *): the same from a transform (scale @0, rotation, translation). */
static uint64_t i_FUN_8005c08c(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t info = (uint32_t)a0, joint, buf, idx;
    uint32_t mtx = jnt_mtx(c, M, &joint, &buf, &idx);
    i_J3DGetTranslateRotateMtx__FRC16J3DTransformInfoPA4_f(c, M, info, mtx, 0, 0, 0, 0, 0, 0);
    jnt_scale(c, M, buf, idx, mtx, info, info, ldf(c, M, (uint32_t)RD(c, REG_R2, 4) - 0x79a8));
    jnt_maya(c, M, joint, mtx);
    jnt_concat(c, M, mtx);
    float x = ldf(c, M, info), y = ldf(c, M, info + 4), z = ldf(c, M, info + 8);
    stf(c, M, J3D_S1, x);
    stf(c, M, J3D_S1 + 4, y);
    stf(c, M, J3D_S1 + 8, z);
    return 0;
}
/* FUN_8005be04(J3DTransformInfo *): Softimage: the accumulated scale times the joint's decides
 * the unit flag; the matrix is scaled by the joint's own scale. */
static uint64_t i_FUN_8005be04(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t info = (uint32_t)a0, joint, buf, idx;
    uint32_t mtx = jnt_mtx(c, M, &joint, &buf, &idx);
    float s0 = mul(ldf(c, M, J3D_S0), ldf(c, M, info));
    float s1 = mul(ldf(c, M, J3D_S0 + 4), ldf(c, M, info + 4));
    float s2 = mul(ldf(c, M, J3D_S0 + 8), ldf(c, M, info + 8));
    stf(c, M, J3D_S0, s0);
    stf(c, M, J3D_S0 + 4, s1);
    stf(c, M, J3D_S0 + 8, s2);
    i_J3DGetTranslateRotateMtx__FRC16J3DTransformInfoPA4_f(c, M, info, mtx, 0, 0, 0, 0, 0, 0);
    jnt_scale(c, M, buf, idx, mtx, info, J3D_S0, ldf(c, M, (uint32_t)RD(c, REG_R2, 4) - 0x79a8));
    jnt_concat(c, M, mtx);
    return 0;
}
/* FUN_8000a1b4(J3DMtxCalcAnimation *): j3dSys+0x30 = this; blend the animations (FUN_800089e4)
 * into scale/translation in its 0x20-byte frame, then FUN_80009398. */
static uint64_t i_FUN_8000a1b4(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t self = (uint32_t)a0, old_sp = ctx_gpr(c, 1), sp = old_sp - 0x20;
    mst(c, M, sp, 4, old_sp);
    ctx_set_gpr(c, 1, sp);
    mst(c, M, 0x803ACF90u + 0x30, 4, self);
    i_FUN_800089e4(c, M, sp + 8, sp + 0x14, self + 4, self + 0x14, 0, 0, 0, 0);
    i_FUN_80009398(c, M, sp + 8, sp + 0x14, 0, 0, 0, 0, 0, 0);
    ctx_set_gpr(c, 1, old_sp);
    return 0;
}

/* ---- CrsGround::search core (FUN_8017fd0c) ---- */
/* FUN_80181370 on host values: the cell (i, j) of a grid (origin x0/z0, cells w x h, nx x nz
 * of them) that pos (projected by FUN_8017b7ac) falls in, or -1/-1 when outside. */
static void crs_cell(Ctx *c, uint8_t *M, uint32_t obj, uint32_t pos, double x0, double z0, double w, double h,
                     int32_t nx, int32_t nz, int32_t *oi, int32_t *oj) {
    uint32_t r2 = (uint32_t)RD(c, REG_R2, 4);
    float in[3] = {ldf(c, M, pos), ldf(c, M, pos + 4), ldf(c, M, pos + 8)}, o[3];
    crs_proj(c, M, obj, in, o);
    *oi = *oj = -1;
    double lo = ldf(c, M, r2 - 0x5cf8), hi = ldf(c, M, r2 - 0x5cf4);
    double dx = rs((double)o[0] - x0);
    double xmax = rs(w * rs((double)nx) + x0), zmax = rs(h * rs((double)nz) + z0);
    if (dx < lo || rs((double)o[0] - xmax) > hi) return;
    double dz = rs((double)o[2] - z0);
    if (dz < lo || rs((double)o[2] - zmax) > hi) return;
    *oi = fctiwz_(rs(dx / w));
    *oj = fctiwz_(rs(dz / h));
}
/* FUN_8017da80: the leaf's first triangle (index list from node+4, count u8 at node) that
 * FUN_8017ddd4 accepts; kind (u8 +0x16) 10 skipped when skip10, mode 1 skips kind 2, mode 2
 * takes only kind 2. */
static uint32_t crs_leaf(Ctx *c, uint8_t *M, uint32_t node, uint32_t plane, uint32_t p, uint32_t q, uint32_t idxs,
                         uint32_t tris, uint32_t extra, int32_t mode, uint32_t skip10) {
    uint32_t a = idxs + (ld32(c, M, node + 4) << 1);
    for (int32_t i = 0; i < (int32_t)mld(c, M, node, 1); i++, a += 2) {
        uint32_t t = tris + ld16(c, M, a) * 0x24, kind = (uint32_t)mld(c, M, t + 0x16, 1);
        if (skip10 && kind == 0xa) continue;
        if (mode == 2 ? kind != 2 : mode == 1 && kind == 2) continue;
        if ((uint8_t)i_FUN_8017ddd4(c, M, t, plane, p, q, extra, 0, 0, 0)) return t;
    }
    return 0;
}
/* FUN_8017fd0c(CrsGround *this, filter, Vec *pos, Vec *vel, mode): a geography object's bound
 * (search_boundSp / search_boundEx, their virtual hit test) or the course's quadtree: descend
 * the cells containing pos (halving the cell size), test the leaf's triangles; then fill the
 * result (plane, attributes, height) as the original. Its 0x100-byte frame holds the plane
 * (sp+0x60) the triangle test writes and the vectors passed to guest callees. */
static uint64_t i_FUN_8017fd0c(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t self = (uint32_t)a0, filt = (uint32_t)a1, pos = (uint32_t)a2, vel = (uint32_t)a3;
    int32_t mode = (int32_t)a4;
    uint32_t r2 = (uint32_t)RD(c, REG_R2, 4), r13 = (uint32_t)RD(c, REG_R13, 4);
    uint32_t old_sp = ctx_gpr(c, 1), sp = old_sp - 0x100, pl = sp + 0x60;
    mst(c, M, sp, 4, old_sp);
    ctx_set_gpr(c, 1, sp);
    mst(c, M, self + 0x1d, 1, 0);
    uint32_t k0 = ld32(c, M, r2 - 0x5d38), k1 = ld32(c, M, r2 - 0x5d34);
    uint32_t grid = ld32(c, M, ld32(c, M, self) + 0x4dc);
    mst(c, M, pl, 4, k0);
    mst(c, M, pl + 4, 4, k1);
    mst(c, M, pl + 8, 4, k0);
    mst(c, M, pl + 0xc, 4, k0);
    if (!ld32(c, M, grid)) goto out;
    uint32_t mgr = ld32(c, M, r13 - 0x5560);
    uint32_t g = guest3(c, 0x801F5414u, mgr, pos, filt); /* search_boundSp */
    if (g) {
        guest3(c, ld32(c, M, ld32(c, M, g) + 0x88), g, vel, pos);
    } else if ((g = guest3(c, 0x801F5378u, mgr, pos, filt))) { /* search_boundEx */
        uint32_t o = ld32(c, M, g + 0x14c);
        cp32(c, M, sp + 0x48, vel, 3);
        cp32(c, M, sp + 0x3c, pos, 3);
        guest3(c, ld32(c, M, ld32(c, M, o) + 0x1c), o, sp + 0x48, sp + 0x3c);
    }
    if (g) {
        cp32(c, M, self + 4, pos, 3);
        cp32(c, M, self + 0x10, vel, 3);
        mst(c, M, self + 0x20, 4, 0);
        mst(c, M, self + 0x24, 4, g);
        cp32(c, M, self + 0x28, ld32(c, M, g + 0x14c) + 0x84, 3);
        cp32(c, M, self + 0x34, ld32(c, M, g + 0x14c) + 0xa8, 3);
        cp32(c, M, self + 0x40, ld32(c, M, g + 0x14c) + 0x90, 3);
        static const uint16_t from[5] = {0xbc, 0xd0, 0xc0, 0xb4, 0xc8};
        for (int i = 0; i < 5; i++) mst(c, M, self + 0x4c + 4 * i, 4, ld32(c, M, ld32(c, M, g + 0x14c) + from[i]));
        goto out;
    }
    uint32_t hit = 0;
    if (!crs_moving(c, M, ld32(c, M, self))) {
        uint32_t prev = ld32(c, M, self + 0x20);
        if (prev && ldf(c, M, pos) == ldf(c, M, self + 4) && ldf(c, M, pos + 4) == ldf(c, M, self + 8) &&
            ldf(c, M, pos + 8) == ldf(c, M, self + 0xc))
            hit = prev;
    }
    if (!hit) {
        uint32_t gd = ld32(c, M, grid);
        double x0 = rs((double)(int32_t)ld32(c, M, gd + 8)), z0 = rs((double)(int32_t)ld32(c, M, gd + 0xc));
        double w = rs((double)ld32(c, M, gd + 0x10)), h = rs((double)ld32(c, M, gd + 0x14));
        int32_t nx = (int32_t)ld16(c, M, gd + 4), nz = (int32_t)ld16(c, M, gd + 6), base = 0;
        double half = ldf(c, M, r2 - 0x5d28);
        uint32_t leaf;
        for (;;) {
            int32_t i, j;
            crs_cell(c, M, ld32(c, M, self), pos, x0, z0, w, h, nx, nz, &i, &j);
            uint32_t node = 0, kids;
            if (i >= 0 && j >= 0 && i < nx && j < nz) node = ld32(c, M, grid) + ((uint32_t)(j * nx + base + i) << 3) + 0x2c;
            if (node && (kids = ld16(c, M, node + 2)) != 0) {
                x0 = rs(w * rs((double)i) + x0);
                w = rs(w * half);
                z0 = rs(h * rs((double)j) + z0);
                h = rs(h * half);
                base = (int32_t)kids;
                nx = nz = 2;
                continue;
            }
            leaf = node;
            break;
        }
        if (leaf) {
            uint32_t gt = ld32(c, M, grid);
            hit = crs_leaf(c, M, leaf, pl, pos, vel, gt + ld32(c, M, gt + 0x1c), gt + ld32(c, M, gt + 0x20),
                           gt + ld32(c, M, gt + 0x24), mode, (uint32_t)mld(c, M, self + 0x1c, 1));
        }
    }
    cp32(c, M, self + 4, pos, 3);
    cp32(c, M, self + 0x10, vel, 3);
    mst(c, M, self + 0x20, 4, hit);
    mst(c, M, self + 0x24, 4, 0);
    if (hit) {
        if (!crs_moving(c, M, ld32(c, M, self))) {
            /* plane from the triangle's normal (s16 x 1/32767-ish) and d */
            double k = ldf(c, M, r2 - 0x5d24);
            for (int i = 0; i < 3; i++) stf(c, M, pl + 4 * i, (float)(k * (double)(int16_t)ld16(c, M, hit + 0x10 + 2 * i)));
            mst(c, M, pl + 0xc, 4, ld32(c, M, hit + 0xc));
        }
        cp32(c, M, self + 0x28, pl, 3);
        mst(c, M, self + 0x4c, 4, (uint32_t)mld(c, M, ld32(c, M, self + 0x20) + 0x16, 1));
        mst(c, M, self + 0x50, 4, (uint32_t)mld(c, M, ld32(c, M, self + 0x20) + 0x17, 1));
        uint32_t t = ld32(c, M, self + 0x20);
        uint32_t attr = guest3(c, 0x8017D780u, grid, (uint32_t)mld(c, M, t + 0x16, 1), (uint32_t)mld(c, M, t + 0x17, 1));
        mst(c, M, self + 0x54, 4, attr & 0xFFFF);
        double v = rs((double)ldf(c, M, pl + 8) * ldf(c, M, self + 0xc));
        v = rs((double)ldf(c, M, pl) * ldf(c, M, self + 4) + v);
        v = rs((double)ldf(c, M, pl + 0xc) + v);
        stf(c, M, self + 0x58, (float)rs(-v / (double)ldf(c, M, pl + 4)));
        mst(c, M, sp + 0x54, 4, ld32(c, M, self + 4));
        mst(c, M, sp + 0x58, 4, ld32(c, M, self + 0x58));
        mst(c, M, sp + 0x5c, 4, ld32(c, M, self + 0xc));
        guest3(c, 0x8017BAD4u, ld32(c, M, self), self + 0x34, sp + 0x54);
        guest1(c, 0x80181F1Cu, self);
    } else {
        mst(c, M, self + 0x28, 4, k0);
        mst(c, M, self + 0x2c, 4, k1);
        mst(c, M, self + 0x30, 4, k0);
        mst(c, M, self + 0x4c, 4, 0xff);
        mst(c, M, self + 0x50, 4, 0);
        mst(c, M, self + 0x54, 4, 0xff);
        mst(c, M, self + 0x58, 4, k0);
        ctx_set_gpr(c, 4, pos);
        if (guest1(c, 0x8017D354u, ld32(c, M, self)) & 0xFF) {
            mst(c, M, self + 0x4c, 4, ld32(c, M, ld32(c, M, self) + 0x530));
            mst(c, M, self + 0x50, 4, ld32(c, M, ld32(c, M, self) + 0x534));
            mst(c, M, self + 0x58, 4, ld32(c, M, ld32(c, M, self) + 0x538));
        }
        uint32_t z = ld32(c, M, r2 - 0x5d38);
        mst(c, M, self + 0x3c, 4, z);
        mst(c, M, self + 0x38, 4, z);
        mst(c, M, self + 0x34, 4, z);
    }
    {
        uint32_t f2 = ld32(c, M, r2 - 0x5d38), f1 = ld32(c, M, r2 - 0x5d34), f0 = ld32(c, M, r2 - 0x5d2c);
        mst(c, M, self + 0x40, 4, f2);
        mst(c, M, self + 0x44, 4, f1);
        mst(c, M, self + 0x48, 4, f2);
        mst(c, M, self + 0x5c, 4, f0);
    }
out:
    ctx_set_gpr(c, 1, old_sp);
    return 0;
}

/* ---- GX ---- */
/* FUN_80067bc8(Mtx m, u32 id): XF load of m's 3x3 part with a zero (r2-0x797c) translation
 * column at XF id * 4. */
static uint64_t i_FUN_80067bc8(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t m = (uint32_t)a0, addr = ((uint32_t)a1 << 2) & 0xFFFF, z = ld32(c, M, (uint32_t)RD(c, REG_R2, 4) - 0x797c);
    uint8_t b[5 + 48] = {0x10, 0x00, 0x0b, (uint8_t)(addr >> 8), (uint8_t)addr};
    for (int r = 0; r < 3; r++) {
        for (int k = 0; k < 3; k++) put_be(b + 5 + 16 * r + 4 * k, 4, ld32(c, M, m + 0x10 * r + 4 * k));
        put_be(b + 5 + 16 * r + 12, 4, z);
    }
    hw_gather_copy(b, sizeof b);
    return 0;
}
/* GXLoadTexObjPreLoaded(GXTexObj *, GXTexRegion *, GXTexMapID id): the register ids of map id
 * (u8 tables in the SDA) go into the top byte of the object's and region's BP words, which are
 * then sent; a texture with a TLUT (flags +0x1f bit 1 clear) also sends the TLUT region's word
 * from __GXData's region callback (+0x4cc). __GXData (r2-0x73f8): the map's two words kept at
 * +0x514/+0x534, dirty bit 0 of +0x5ac, +2 = 0. */
static uint64_t i_GXLoadTexObjPreLoaded(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t obj = (uint32_t)a0, reg = (uint32_t)a1, id = (uint32_t)a2;
    uint32_t r2 = (uint32_t)RD(c, REG_R2, 4), r13 = (uint32_t)RD(c, REG_R13, 4);
    static const uint16_t tbl[6] = {0x7e18, 0x7e10, 0x7e08, 0x7e00, 0x7df8, 0x7df0};
    const uint32_t word[6] = {obj, obj + 4, obj + 8, reg, reg + 4, obj + 0xc};
    for (int i = 0; i < 6; i++) {
        uint32_t v = (uint32_t)mld(c, M, r13 - tbl[i] + id, 1);
        mst(c, M, word[i], 4, (ld32(c, M, word[i]) & 0x00FFFFFFu) | (v << 24));
    }
    uint8_t b[30];
    for (int i = 0; i < 6; i++) {
        b[5 * i] = 0x61;
        put_be(b + 5 * i + 1, 4, ld32(c, M, word[i]));
    }
    hw_gather_copy(b, sizeof b);
    if (!(mld(c, M, obj + 0x1f, 1) & 2)) {
        uint32_t old_sp = ctx_gpr(c, 1), sp = old_sp - 0x40;
        mst(c, M, sp, 4, old_sp);
        ctx_set_gpr(c, 1, sp);
        uint32_t t = guest1(c, ld32(c, M, ld32(c, M, r2 - 0x73f8) + 0x4cc), ld32(c, M, obj + 0x18));
        ctx_set_gpr(c, 1, old_sp);
        uint32_t v = (uint32_t)mld(c, M, r13 - 0x7de8 + id, 1);
        mst(c, M, t + 4, 4, (ld32(c, M, t + 4) & 0x00FFFFFFu) | (v << 24));
        uint8_t tb[5] = {0x61};
        put_be(tb + 1, 4, ld32(c, M, t + 4));
        hw_gather_copy(tb, sizeof tb);
    }
    uint32_t gx = ld32(c, M, r2 - 0x73f8);
    mst(c, M, gx + 0x514 + 4 * id, 4, ld32(c, M, obj + 8));
    mst(c, M, gx + 0x534 + 4 * id, 4, ld32(c, M, obj));
    mst(c, M, gx + 0x5ac, 4, ld32(c, M, gx + 0x5ac) | 1);
    mst(c, M, gx + 2, 2, 0);
    return 0;
}

/* ---- J3D model / material loops ---- */
/* J3DTexGenBlock::getTexMtx (vtable +0x50): the two known implementations natively. */
static uint32_t texgen_mtx(Ctx *c, uint8_t *M, uint32_t tgb, uint32_t i) {
    uint32_t fn = ld32(c, M, ld32(c, M, tgb) + 0x50);
    if (fn == 0x800740F8u) return ld32(c, M, tgb + 4 * i + 0x38); /* J3DTexGenBlockPatched */
    if (fn == 0x80053C18u) return 0;                               /* J3DTexGenBlock */
    return guest3(c, fn, tgb, i, 0);
}
/* J3DModel::calcDiffTexMtx(): j3dSys+0x38 = model; per shape (j3dSys+0x3c = its packet) the
 * shape's virtual +0xc with its joint's matrix; per material the 8 texture matrices (+0x64)
 * copied into the material packet's (+0x24 -> @0) slots. */
static uint64_t i_calcDiffTexMtx__8J3DModelFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    const uint32_t SYS = 0x803ACF90u;
    uint32_t model = (uint32_t)a0;
    mst(c, M, SYS + 0x38, 4, model);
    uint32_t n = ld16(c, M, ld32(c, M, model + 4) + 0x5c);
    for (uint32_t i = 0; i < n; i++) {
        mst(c, M, SYS + 0x3c, 4, ld32(c, M, model + 0xc0) + (i << 6));
        uint32_t shape = ld32(c, M, ld32(c, M, ld32(c, M, model + 4) + 0x60) + 4 * i);
        uint32_t mtx = ld32(c, M, ld32(c, M, model + 0x84) + 0xc) + ld16(c, M, ld32(c, M, shape + 0xc) + 0x14) * 0x30;
        guest3(c, ld32(c, M, ld32(c, M, shape) + 0xc), shape, mtx, 0);
    }
    n = ld16(c, M, ld32(c, M, model + 4) + 0x7c);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t mat = ld32(c, M, ld32(c, M, ld32(c, M, model + 4) + 0x80) + 4 * i);
        uint32_t pkt = ld32(c, M, model + 0xc4) + i * 0x3c;
        uint32_t tgb = ld32(c, M, ld32(c, M, mat + 4) + 0x28);
        for (uint32_t k = 0; k < 8; k++) {
            uint32_t tm = texgen_mtx(c, M, tgb, k), dst = ld32(c, M, pkt + 0x24);
            if (tm && dst) i_PSMTXCopy(c, M, tm + 0x64, ld32(c, M, dst) + k * 0x30, 0, 0, 0, 0, 0, 0);
        }
    }
    return 0;
}
/* FUN_801826d4(obj, a, const u32 v[2]): for each material (obj+8 -> @0: count u16 +0x5c, list
 * +0x60), its +0x2c block's virtual +0x64(a, &copy of v) (the copy in the 0x30-byte frame). */
static uint64_t i_FUN_801826d4(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t a = (uint32_t)a1, set = ld32(c, M, ld32(c, M, (uint32_t)a0 + 8));
    if (!set) return 0;
    uint32_t v0 = ld32(c, M, (uint32_t)a2), v1 = ld32(c, M, (uint32_t)a2 + 4);
    uint32_t old_sp = ctx_gpr(c, 1), sp = old_sp - 0x30;
    mst(c, M, sp, 4, old_sp);
    ctx_set_gpr(c, 1, sp);
    for (uint32_t i = 0; i < ld16(c, M, set + 0x5c); i++) {
        uint32_t blk = ld32(c, M, ld32(c, M, ld32(c, M, set + 0x60) + 4 * i) + 0x2c);
        mst(c, M, sp + 8, 4, v0);
        mst(c, M, sp + 0xc, 4, v1);
        guest3(c, ld32(c, M, ld32(c, M, blk) + 0x64), blk, a, sp + 8);
    }
    ctx_set_gpr(c, 1, old_sp);
    return 0;
}
/* FUN_80183c08(obj, Mtx m): m copied to every entry's matrix (+0x24), or to obj+0x2c for empty
 * entries (count u16 +4, list +0xc); nothing when obj+8 -> @0 is null. */
static uint64_t i_FUN_80183c08(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t obj = (uint32_t)a0, m = (uint32_t)a1;
    if (!ld32(c, M, ld32(c, M, obj + 8))) return 0;
    for (uint32_t i = 0; i < ld16(c, M, obj + 4); i++) {
        uint32_t e = ld32(c, M, ld32(c, M, obj + 0xc) + 4 * i);
        i_PSMTXCopy(c, M, m, e ? e + 0x24 : obj + 0x2c, 0, 0, 0, 0, 0, 0);
    }
    return 0;
}
/* FUN_80183d2c(obj, Mtx m, u32 k): for every entry's materials whose texture matrix k has
 * projection type 5 or 11 (u8 +1 & 0x3f), the packet's matrix k = m with row 3 = (0, 0, 0, 1)
 * (r2-0x5cd8 / -0x5ce4). */
static uint64_t i_FUN_80183d2c(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t obj = (uint32_t)a0, m = (uint32_t)a1, k = (uint32_t)a2, r2 = (uint32_t)RD(c, REG_R2, 4);
    uint32_t off = (k << 6) & 0x003FFFC0u;
    for (uint32_t i = 0; i < ld16(c, M, obj + 4); i++) {
        uint32_t e = ld32(c, M, ld32(c, M, obj + 0xc) + 4 * i);
        if (!e) continue;
        uint32_t n = ld16(c, M, ld32(c, M, e + 4) + 0x7c);
        for (uint32_t j = 0; j < n; j++) {
            uint32_t pkt = ld32(c, M, e + 0xc4) + j * 0x3c;
            uint32_t tm = texgen_mtx(c, M, ld32(c, M, ld32(c, M, ld32(c, M, pkt + 0x28) + 4) + 0x28), k);
            if (!tm) continue;
            uint32_t type = (uint32_t)mld(c, M, tm + 1, 1) & 0x3f;
            if (type != 0xb && type != 5) continue;
            uint32_t d = ld32(c, M, pkt + 0x24);
            uint32_t dst = ld32(c, M, d + 4) + off;
            if (IS_RAM(dst) && IS_RAM(dst + 0x2f) && IS_RAM(m) && IS_RAM(m + 0x2f))
                memmove(M + (dst & 0x01FFFFFFu), M + (m & 0x01FFFFFFu), 0x30);
            else
                for (uint32_t b = 0; b < 0x30; b++) mst(c, M, dst + b, 1, mld(c, M, m + b, 1));
            uint32_t one = ld32(c, M, r2 - 0x5cd8), zero = ld32(c, M, r2 - 0x5ce4);
            mst(c, M, off + ld32(c, M, d + 4) + 0x38, 4, one);
            mst(c, M, off + ld32(c, M, d + 4) + 0x34, 4, one);
            mst(c, M, off + ld32(c, M, d + 4) + 0x30, 4, one);
            mst(c, M, off + ld32(c, M, d + 4) + 0x3c, 4, zero);
        }
    }
    return 0;
}

/* Virtual call obj->vtable[off / 4](obj, r4); returns r3. */
static uint32_t vcall(Ctx *c, uint8_t *M, uint32_t obj, uint32_t off, uint32_t r4) {
    ctx_set_gpr(c, 3, obj);
    ctx_set_gpr(c, 4, r4);
    ctx_call(c, ld32(c, M, ld32(c, M, obj) + off));
    return ctx_gpr(c, 3);
}
/* J3DMaterial::diff(u32 flags): within the material packet's diff list (beginDiff/endDiff;
 * only when the packet has one), the blocks' diff virtuals; with flag 0x02000000 also the
 * channel controls (FUN_80064ff0) and the XF channel count (0x103f) written to the GD list. */
static uint64_t i_diff__11J3DMaterialFUl(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    const uint32_t SYS = 0x803ACF90u;
    uint32_t mat = (uint32_t)a0, flags = (uint32_t)a1;
    uint32_t pkt = ld32(c, M, SYS + 0x3c), dl = ld32(c, M, pkt + 0x28);
    if (!ld32(c, M, dl + 0x20)) return 0;
    uint32_t old_sp = ctx_gpr(c, 1), sp = old_sp - 0x20;
    mst(c, M, sp, 4, old_sp);
    ctx_set_gpr(c, 1, sp);
    ctx_set_gpr(c, 4, dl);
    guest1(c, 0x8004FF8Cu, pkt); /* beginDiff */
    vcall(c, M, ld32(c, M, mat + 0x2c), 0x10, flags);
    vcall(c, M, ld32(c, M, mat + 0x30), 0xc, flags);
    vcall(c, M, ld32(c, M, mat + 0x34), 0x14, flags);
    if (flags & 0x02000000u) {
        uint32_t r27 = guest1(c, ld32(c, M, ld32(c, M, ld32(c, M, mat + 0x30)) + 0x20), ld32(c, M, mat + 0x30));
        uint32_t r28 = guest1(c, ld32(c, M, ld32(c, M, ld32(c, M, mat + 0x2c)) + 0x98), ld32(c, M, mat + 0x2c));
        uint32_t n = guest1(c, ld32(c, M, ld32(c, M, ld32(c, M, mat + 0x28)) + 0x40), ld32(c, M, mat + 0x28));
        guest3(c, 0x80064FF0u, n & 0xFF, r28, r27);
        n = guest1(c, ld32(c, M, ld32(c, M, ld32(c, M, mat + 0x28)) + 0x40), ld32(c, M, mat + 0x28));
        const uint8_t b[9] = {0x10, 0, 0, 0x10, 0x3f, 0, 0, 0, (uint8_t)n};
        gd_put(c, M, b, sizeof b);
    }
    vcall(c, M, ld32(c, M, mat + 0x28), 0x24, flags);
    vcall(c, M, ld32(c, M, mat + 0x24), 0x1c, flags);
    guest1(c, 0x8004FFF0u, ld32(c, M, SYS + 0x3c)); /* endDiff */
    ctx_set_gpr(c, 1, old_sp);
    return 0;
}
/* J3DMaterialAnm::calc(J3DMaterial *): the material's animated values from its animations:
 * material colours (2), texture numbers (8), TEV colours (3) and konst colours (4), texture
 * matrices (8; FUN_8005875c). An entry is active when its u16 count is non-zero. */
static uint64_t i_calc__14J3DMaterialAnmCFP11J3DMaterial(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t self = (uint32_t)a0, mat = (uint32_t)a1;
    uint32_t old_sp = ctx_gpr(c, 1), sp = old_sp - 0x30;
    mst(c, M, sp, 4, old_sp);
    ctx_set_gpr(c, 1, sp);
    for (uint32_t i = 0; i < 2; i++) {
        uint32_t e = self + 8 * i;
        if (!ld16(c, M, e + 6)) continue;
        uint32_t col = vcall(c, M, ld32(c, M, mat + 0x24), 0x38, i);
        ctx_set_gpr(c, 5, col);
        vcall(c, M, ld32(c, M, e + 8), 0x10, ld16(c, M, e + 4));
    }
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t e = self + 12 * i;
        if (!ld16(c, M, e + 0x5a)) continue;
        vcall(c, M, e + 0x54, 0x8, sp + 8);
        ctx_set_gpr(c, 5, ld16(c, M, sp + 8));
        vcall(c, M, ld32(c, M, mat + 0x2c), 0x4c, i);
    }
    for (uint32_t i = 0; i < 3; i++) {
        uint32_t e = self + 8 * i;
        if (!ld16(c, M, e + 0xb6)) continue;
        uint32_t p = vcall(c, M, ld32(c, M, mat + 0x2c), 0x68, i);
        guest3(c, 0x80059B50u, ld32(c, M, e + 0xb8), ld16(c, M, e + 0xb4), p); /* getTevColorReg */
    }
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t e = self + 8 * i;
        if (!ld16(c, M, e + 0xd6)) continue;
        uint32_t p = vcall(c, M, ld32(c, M, mat + 0x2c), 0x74, i);
        guest3(c, 0x80059E0Cu, ld32(c, M, e + 0xd8), ld16(c, M, e + 0xd4), p); /* getTevKonstReg */
    }
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t e = self + 8 * i;
        if (!ld16(c, M, e + 0x16)) continue;
        uint32_t tm = texgen_mtx(c, M, ld32(c, M, mat + 0x28), i), anm = ld32(c, M, e + 0x18);
        uint64_t f = dbits(ldf(c, M, anm + 8));
        WR(c, 0x4008, 8, f);
        WR(c, 0x5008, 8, f);
        guest3(c, 0x8005875Cu, anm, ld16(c, M, e + 0x14), tm + 0x10);
    }
    ctx_set_gpr(c, 1, old_sp);
    return 0;
}

/* ---- audio ---- */
/* JAISoundHandles::setPos(const Vec &): the position into every live handle's sound (+8 -> +4). */
static uint64_t i_setPos__15JAISoundHandlesFRCQ29JGeometry8TVec3_f_(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t self = (uint32_t)a0, v = (uint32_t)a1;
    uint32_t x = ld32(c, M, v), y = ld32(c, M, v + 4), z = ld32(c, M, v + 8);
    for (int32_t i = 0; i < (int32_t)ld32(c, M, self + 4); i++) {
        uint32_t h = ld32(c, M, ld32(c, M, self) + 4 * i);
        if (!h) continue;
        uint32_t s = ld32(c, M, h + 8);
        if (!s) continue;
        mst(c, M, s + 4, 4, x);
        mst(c, M, s + 8, 4, y);
        mst(c, M, s + 0xc, 4, z);
    }
    return 0;
}

/* ---- widescreen: the game's camera aspect (System::getPerspAspect*, read by every kart camera
 * and the lens flare) and C_MTXPerspective (menus, character select, replays) times
 * g_aspect_scale. Exact originals at 1.0. ---- */
float g_aspect_scale = 1.0f;
static uint32_t s_addr[NATIVE_COUNT];
static uint64_t aspect_sda(Ctx *c, uint8_t *M, uint32_t off) {
    double v = ldf(c, M, (uint32_t)RD(c, REG_R13, 4) - off);
    return dbits(g_aspect_scale == 1.0f ? v : rs(v * g_aspect_scale));
}
static uint64_t i_getPerspAspectDiv2__6SystemFv(Ctx *c, uint8_t *M, ARGS) { UNUSED_ARGS; return aspect_sda(c, M, 0x5618); }
static uint64_t i_getPerspAspectDiv4__6SystemFv(Ctx *c, uint8_t *M, ARGS) { UNUSED_ARGS; return aspect_sda(c, M, 0x5614); }
static uint64_t i_getPerspAspectSub__6SystemFv(Ctx *c, uint8_t *M, ARGS) { UNUSED_ARGS; return aspect_sda(c, M, 0x5618); }
static uint64_t i_C_MTXPerspective(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    if (g_aspect_scale != 1.0f) {
        uint64_t a = dbits(rs(ps_d(a2) * g_aspect_scale));
        WR(c, 0x4010, 8, a);
        WR(c, 0x5010, 8, a);
    }
    ctx_call_orig(c, s_addr[NAT_C_MTXPerspective]);
    return 0;
}
/* FUN_8029e0a8(KartCam *, ...): sets up a race camera and stores its aspect (from the getters
 * above) at +0x168 once, so a display mode change mid-race only stretched the picture. The
 * cameras are remembered here and aspect_refresh rescales them on the next race frame; a slot is
 * only rewritten while it still holds the value last stored there (cameras of an old race are
 * gone). */
static struct { uint32_t cam, bits; float base; } s_cams[8];
static unsigned s_cam_next;
static float s_cam_scale = 1.0f;
static uint64_t i_FUN_8029e0a8(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    float scale = g_aspect_scale;
    ctx_call_orig(c, s_addr[NAT_FUN_8029e0a8]);
    uint32_t cam = (uint32_t)a0;
    unsigned k = 0;
    while (k < 8 && s_cams[k].cam != cam) k++;
    if (k == 8) k = s_cam_next++ & 7;
    s_cams[k].cam = cam;
    s_cams[k].bits = ld32(c, M, cam + 0x168);
    s_cams[k].base = ldf(c, M, cam + 0x168) / scale;
    return 0;
}
static void aspect_refresh(Ctx *c, uint8_t *M) {
    float scale = g_aspect_scale;
    if (scale == s_cam_scale) return;
    s_cam_scale = scale;
    for (unsigned k = 0; k < 8; k++) {
        if (!s_cams[k].cam || ld32(c, M, s_cams[k].cam + 0x168) != s_cams[k].bits) { s_cams[k].cam = 0; continue; }
        stf(c, M, s_cams[k].cam + 0x168, (float)rs(s_cams[k].base * scale));
        s_cams[k].bits = ld32(c, M, s_cams[k].cam + 0x168);
    }
}

/* ---- field of view (the - menu's FIELD OF VIEW): the race cameras' projection (FUN_801a77e8, from
 * GrafPort::setPort: matrix, fovy, aspect, near, far) and their view-frustum culling (FUN_8000a260,
 * J3DUClipper::calcViewFrustum: fovy in degrees at +0x4c) both see tan(fovy / 2) times
 * g_fov_scale, so the culling matches the wider picture. Exact originals at 1.0. ---- */
float g_fov_scale = 1.0f;
static double fov_widen(double deg) {
    double r = atan(tan(deg * (3.14159265358979323846 / 360.0)) * g_fov_scale) * (360.0 / 3.14159265358979323846);
    return r < 170.0 ? r : 170.0;
}
static uint64_t i_FUN_801a77e8(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    if (g_fov_scale != 1.0f) {
        uint64_t f = dbits(rs(fov_widen(ps_d(a1))));
        WR(c, 0x4008, 8, f);
        WR(c, 0x5008, 8, f);
    }
    ctx_call_orig(c, s_addr[NAT_FUN_801a77e8]);
    return 0;
}
static uint64_t i_FUN_8000a260(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t clip = (uint32_t)a0;
    float fovy = ldf(c, M, clip + 0x4c);
    bool widen = g_fov_scale != 1.0f;
    if (widen) stf(c, M, clip + 0x4c, (float)rs(fov_widen(fovy)));
    ctx_call_orig(c, s_addr[NAT_FUN_8000a260]);
    if (widen) stf(c, M, clip + 0x4c, fovy);
    return 0;
}

/* ==== batch 4 (animation, J3D drawing, HUD) ==== */

/* FUN_80026ae0(f32 y, f32 x, f32 *tbl): atan2 from a table (JMath), by octant: the table holds
 * atan(t) for t = i / N, indexed with (int)(N * small / large + 0.5); the octant adds or
 * subtracts the result from 0, pi/2, pi, -pi/2, -pi. Constants from the SDA2 (r2). */
static uint64_t i_FUN_80026ae0(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t r2 = (uint32_t)RD(c, REG_R2, 4), tbl = (uint32_t)a0;
    double y = ps_d(a1), x = ps_d(a2);
    double zero = ldf(c, M, r2 - 0x7cf0), n = ldf(c, M, r2 - 0x7ce4), half = ldf(c, M, r2 - 0x7ce8);
#define AT(num, den) (double)ldf(c, M, tbl + ((uint32_t)fctiwz_(rs(half + rs(rs(n * (num)) / (den)))) << 2))
    double r;
    if (y >= zero) {
        if (x >= zero) {
            if (x >= y) r = zero == x ? zero : AT(y, x);
            else {
                double k = ldf(c, M, r2 - 0x7cec);
                r = rs(k - (zero == y ? zero : AT(x, y)));
            }
        } else {
            double nx = -x;
            if (nx < y) {
                double k = ldf(c, M, r2 - 0x7cec);
                r = rs(k + (zero == y ? zero : AT(nx, y)));
            } else {
                double k = ldf(c, M, r2 - 0x7cdc);
                r = rs(k - (zero == nx ? zero : AT(y, nx)));
            }
        }
    } else {
        double ny = -y;
        if (x < zero) {
            double nx = -x;
            if (nx >= ny) {
                double k = ldf(c, M, r2 - 0x7cd8);
                r = rs(k + (zero == nx ? zero : AT(ny, nx)));
            } else {
                double k = ldf(c, M, r2 - 0x7ce0);
                r = rs(k - (zero == ny ? zero : AT(nx, ny)));
            }
        } else if (x < ny) {
            double k = ldf(c, M, r2 - 0x7ce0);
            r = rs(k + (zero == ny ? zero : AT(x, ny)));
        } else {
            r = -(zero == x ? zero : AT(ny, x));
        }
    }
#undef AT
    return dbits(r);
}

/* J3DFrameCtrl::update(): +4 mode, +5 state (bit 0 stopped at an end, bit 1 wrapped), +6 start,
 * +8 end, +0xa loop start (s16), +0xc rate, +0x10 frame. frame += rate, then by mode: 0 clamp
 * (end - 1), 1 clamp back to start, 2 repeat (loop..end), 3 mirror once, 4 mirror repeat. The
 * s16 fields convert to float exactly (the original's 0x4330 double trick); r2 holds 0 and 1. */
static uint64_t i_update__12J3DFrameCtrlFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t r2 = (uint32_t)RD(c, REG_R2, 4), t = (uint32_t)a0;
    uint32_t st = 0;
    double frame = rs((double)ldf(c, M, t + 0x10) + ldf(c, M, t + 0xc));
    stf(c, M, t + 0x10, (float)frame);
    unsigned mode = (unsigned)mld(c, M, t + 4, 1);
    double s = (int16_t)ld16(c, M, t + 6), e = (int16_t)ld16(c, M, t + 8), lp = (int16_t)ld16(c, M, t + 0xa);
    double zero = ldf(c, M, r2 - 0x7a0c);
#define STOP(v) do { frame = (v); stf(c, M, t + 0x10, (float)frame); stf(c, M, t + 0xc, (float)zero); st |= 1; } while (0)
#define FLIP(v) do { frame = (v); stf(c, M, t + 0x10, (float)frame); stf(c, M, t + 0xc, -ldf(c, M, t + 0xc)); } while (0)
    switch (mode) {
    case 0:
        if (frame < s) STOP(s);
        if (frame >= e) STOP(rs(e - ldf(c, M, r2 - 0x7a08)));
        break;
    case 1:
        if (frame < s) STOP(s);
        if (frame >= e) STOP(s);
        break;
    case 2: {
        while (frame < s) {
            st |= 2;
            double d = (double)((int32_t)lp - (int32_t)s);
            if (d <= zero) break;
            frame = rs(frame + d);
            stf(c, M, t + 0x10, (float)frame);
        }
        while (frame >= e) {
            st |= 2;
            double d = (double)((int32_t)e - (int32_t)lp);
            if (d <= zero) break;
            frame = rs(frame - d);
            stf(c, M, t + 0x10, (float)frame);
        }
        break;
    }
    case 3:
        if (frame >= e) FLIP(rs(e - rs(frame - e)));
        if (frame < s) STOP(rs(s - rs(frame - s)));
        break;
    case 4: {
        double e1 = rs(e - ldf(c, M, r2 - 0x7a10));
        if (frame >= e1) FLIP(rs(e1 - rs(frame - e1)));
        if (frame < s) { FLIP(rs(s - rs(frame - s))); st |= 2; }
        break;
    }
    default: break;
    }
#undef STOP
#undef FLIP
    mst(c, M, t + 5, 1, st);
    return 0;
}

/* PSMTXScale(Mtx m, f32 x, f32 y, f32 z) */
static void mtx_scale(Ctx *c, uint8_t *M, uint32_t m, double x, double y, double z) {
    float zero = ldf(c, M, (uint32_t)RD(c, REG_R2, 4) - 0x743c);
    float v[12] = {(float)x, zero, zero, zero, zero, (float)y, zero, zero, zero, zero, (float)z, zero};
    stfn(c, M, m, v, 12);
}
static uint64_t i_PSMTXScale(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    mtx_scale(c, M, (uint32_t)a0, ps_d(a1), ps_d(a2), ps_d(a3));
    return 0;
}

/* |column| of a 3x4 matrix with the game's square-root idiom: sum of squares (row 1 first),
 * then sum * (k1 * e) * -(sum * e * e - k2) with e = frsqrte(sum) rounded to single; a sum <= 0
 * is returned as is. r2 offsets of 0, k1 (0.5) and k2 (3.0) differ per function. */
static double col_len(Ctx *c, uint8_t *M, uint32_t m, int col, uint32_t r2, int32_t o0, int32_t ok1, int32_t ok2) {
    double a = ldf(c, M, m + 4 * col), b = ldf(c, M, m + 0x10 + 4 * col), d = ldf(c, M, m + 0x20 + 4 * col);
    double s = rs(b * b);
    s = rs(a * a + s);
    s = rs(d * d + s);
    if (s <= (double)ldf(c, M, r2 + o0)) return s;
    double e = rs(1.0 / sqrt(s));
    double e2 = rs(e * e), k = rs((double)ldf(c, M, r2 + ok1) * e);
    double t = -rs(s * e2 - (double)ldf(c, M, r2 + ok2));
    return rs(s * rs(k * t));
}

/* IKCtrl::calcJointScaleMtx(Mtx dst, const Mtx src): dst = scale(|src col 0|, |col 1|, |col 2|)
 * (the original copies src to dst first; PSMTXScale then overwrites all of it). */
static uint64_t i_calcJointScaleMtx__6IKCtrlFPQ29JGeometry64TPosition3_Q29JGeometry38TMatrix34_Q29JGeometry13SMatrix34C_f___PA4_f(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t r2 = (uint32_t)RD(c, REG_R2, 4), dst = (uint32_t)a0, src = (uint32_t)a1;
    float v[12];
    ldfn(c, M, src, v, 12);
    stfn(c, M, dst, v, 12);
    double x = col_len(c, M, dst, 0, r2, -0x3fd8, -0x3fd4, -0x3fd0);
    double y = col_len(c, M, dst, 1, r2, -0x3fd8, -0x3fd4, -0x3fd0);
    double z = col_len(c, M, dst, 2, r2, -0x3fd8, -0x3fd4, -0x3fd0);
    mtx_scale(c, M, dst, x, y, z);
    return 0;
}

/* LumbarCtrl::calcScaleMtx(const Mtx src): this+0x78 = src, column lengths to this+0xa8..0xb0,
 * then this+0x78 = scale(lengths). */
static uint64_t i_calcScaleMtx__10LumbarCtrlFPA4_f(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t r2 = (uint32_t)RD(c, REG_R2, 4), t = (uint32_t)a0, src = (uint32_t)a1;
    float v[12];
    ldfn(c, M, src, v, 12);
    stfn(c, M, t + 0x78, v, 12);
    float l[3];
    for (int i = 0; i < 3; i++) {
        l[i] = (float)col_len(c, M, t + 0x78, i, r2, -0x3ef0, -0x3f00, -0x3ee4);
        stf(c, M, t + 0xa8 + 4 * i, l[i]);
    }
    mtx_scale(c, M, t + 0x78, l[0], l[1], l[2]);
    return 0;
}

/* stVecNormalize(TVec3 &v): false (untouched) when |v|^2 <= eps (r2 constant times a global);
 * otherwise v *= rsqrt(|v|^2) (|v|^2 itself when <= a second threshold) and true. */
static uint64_t i_stVecNormalize__FRQ29JGeometry8TVec3_f_(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t r2 = (uint32_t)RD(c, REG_R2, 4), v = (uint32_t)a0;
    P xy = pl(c, M, v);
    double z = ldf(c, M, v + 8);
    P sq = pmul(xy, xy);
    double s = rs(rs(z * z + sq.a) + sq.b);
    double eps = rs((double)ldf(c, M, r2 - 0x4680) * ldf(c, M, 0x803c9a1cu));
    if (s <= eps) return 0;
    double f;
    if (s <= (double)ldf(c, M, r2 - 0x467c)) f = s;
    else {
        double e = rs(1.0 / sqrt(s));
        double e2 = rs(e * e), k = rs((double)ldf(c, M, r2 - 0x4678) * e);
        f = rs(k * -rs(s * e2 - (double)ldf(c, M, r2 - 0x4674)));
    }
    stf(c, M, v, (float)rs(xy.a * f));
    stf(c, M, v + 4, (float)rs(xy.b * f));
    stf(c, M, v + 8, (float)rs(z * f));
    return 1;
}

/* DriverModel::calcMPos(TVec3 &root, TVec3 &target, TVec3 &up, f32 l1, f32 l2, TVec3 *out): the
 * middle joint of a two-segment limb. If root and target coincide (within eps per axis), out =
 * root. Otherwise a frame u = norm(target - root), w = norm(up x u), v = w x u at root; the
 * target in that frame gives the reach x; with x < l1 + l2 the joint sits at
 * (X, sqrt(l1^2 - X^2), 0), X = (x^2 - l2^2 + l1^2) / (2x), else at (l1, 0, 0); out = frame * that.
 * Temporaries live in the original's stack frame addresses (dead after the return). */
static uint64_t i_PSVECMag(Ctx *c, uint8_t *M, ARGS);
static uint64_t i_PSVECNormalize(Ctx *c, uint8_t *M, ARGS);
static uint64_t i_PSVECCrossProduct(Ctx *c, uint8_t *M, ARGS);
static uint64_t i_PSMTXInverse(Ctx *c, uint8_t *M, ARGS);
static uint64_t i_PSMTXMultVec(Ctx *c, uint8_t *M, ARGS);
#define CALLN(fn, x, y, z) fn(c, M, (x), (y), (z), 0, 0, 0, 0, 0)
static uint64_t i_calcMPos__11DriverModelFRQ29JGeometry8TVec3_f_RQ29JGeometry8TVec3_f_RQ29JGeometry8TVec3_f_ffPQ29JGeometry8TVec3_f_(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t r2 = (uint32_t)RD(c, REG_R2, 4);
    uint32_t pa = (uint32_t)a1, pb = (uint32_t)a2, pu = (uint32_t)a3, out = (uint32_t)a4;
    double l1 = ps_d(a5), l2 = ps_d(a6);
    uint32_t sp = ctx_gpr(c, 1) - 0xe0;
    double eps = rs((double)ldf(c, M, r2 - 0x534c) * ldf(c, M, 0x803c9a1cu)), neps = -eps;
    bool near = true;
    for (int i = 0; i < 3 && near; i++) {
        double d = rs((double)ldf(c, M, pa + 4 * i) - ldf(c, M, pb + 4 * i));
        near = neps <= d && d <= eps;
    }
    if (near) {
        float v[3];
        ldfn(c, M, pa, v, 3);
        stfn(c, M, out, v, 3);
        return 0;
    }
    double zero = ldf(c, M, r2 - 0x5350);
    if (zero == ps_d(CALLN(i_PSVECMag, pu, 0, 0))) stf(c, M, pu + 8, ldf(c, M, r2 - 0x5340));
    for (int i = 0; i < 3; i++) stf(c, M, sp + 8 + 4 * i, (float)rs((double)ldf(c, M, pb + 4 * i) - ldf(c, M, pa + 4 * i)));
    if (zero == ps_d(CALLN(i_PSVECMag, sp + 8, 0, 0))) stf(c, M, sp + 0xc, ldf(c, M, r2 - 0x5344));
    CALLN(i_PSVECNormalize, sp + 8, sp + 0x14, 0);
    CALLN(i_PSVECCrossProduct, pu, sp + 0x14, sp + 0x2c);
    CALLN(i_PSVECNormalize, sp + 0x2c, sp + 0x2c, 0);
    CALLN(i_PSVECCrossProduct, sp + 0x2c, sp + 0x14, sp + 0x20);
    float m[12];
    for (int r = 0; r < 3; r++) {
        m[4 * r] = ldf(c, M, sp + 0x14 + 4 * r);
        m[4 * r + 1] = ldf(c, M, sp + 0x20 + 4 * r);
        m[4 * r + 2] = ldf(c, M, sp + 0x2c + 4 * r);
        m[4 * r + 3] = ldf(c, M, pa + 4 * r);
    }
    stfn(c, M, sp + 0x80, m, 12);
    CALLN(i_PSMTXInverse, sp + 0x80, sp + 0x50, 0);
    CALLN(i_PSMTXMultVec, sp + 0x50, pb, sp + 0x44);
    double px = ldf(c, M, sp + 0x44);
    float j[3];
    if (px < rs(l1 + l2)) {
        double l1s = rs(l1 * l1);
        double den = rs((double)ldf(c, M, r2 - 0x533c) * px);
        double num = rs(px * px + -rs(l2 * l2 - l1s));
        double x = rs(num / den);
        double h = rs(l1s - rs(x * x));
        double y;
        if (h < zero) y = zero;
        else if (h > zero) y = rs((1.0 / sqrt(h)) * h);
        else y = h;
        j[0] = (float)x; j[1] = (float)y; j[2] = (float)zero;
    } else {
        j[0] = (float)l1; j[1] = (float)zero; j[2] = (float)zero;
    }
    stfn(c, M, sp + 0x38, j, 3);
    CALLN(i_PSMTXMultVec, sp + 0x80, sp + 0x38, out);
    return 0;
}

/* ---- J3D draw chain: J3DDrawBuffer::drawHead -> J3DMatPacket::draw -> shape packet
 * (FUN_800507a0) -> shape (FUN_80050f70) -> matrix loads + J3DShapeDraw -> GXCallDisplayList ---- */
#define J3DSYS 0x803acf90u
/* A guest call from a native (flag 1/4 natives: the caller stored its registers), in a frame of
 * its own below the caller's. */
static uint32_t gcall(Ctx *c, uint8_t *M, uint32_t fn, uint32_t r3, uint32_t r4, uint32_t r5) {
    uint32_t old = ctx_gpr(c, 1), sp = old - 0x20;
    mst(c, M, sp, 4, old);
    ctx_set_gpr(c, 1, sp);
    ctx_set_gpr(c, 3, r3);
    ctx_set_gpr(c, 4, r4);
    ctx_set_gpr(c, 5, r5);
    ctx_call(c, fn);
    ctx_set_gpr(c, 1, old);
    return ctx_gpr(c, 3);
}
ALWAYS_INLINE void fifo8(uint8_t **p, uint32_t v) { *(*p)++ = (uint8_t)v; }
ALWAYS_INLINE void fifo16(uint8_t **p, uint32_t v) { put_be(*p, 2, v); *p += 2; }
ALWAYS_INLINE void fifo32(uint8_t **p, uint32_t v) { put_be(*p, 4, v); *p += 4; }

/* GXCallDisplayList(list, size): pending dirty GX state first (__GXSetDirtyState), the
 * flush primitive when the last command was not a draw (__GXSendFlushPrim: 0x98, the vertex
 * count, then that many zero words... rounded per vertex size), then 0x40 list size. */
static void dirty_state(Ctx *c, uint8_t *M);
/* __GXSendFlushPrim: 0x98 with the last draw's vertex count (+4) and that many zero vertices of
 * its size (+6), in words; +2 = 1. */
static void send_flush_prim(Ctx *c, uint8_t *M) {
    uint32_t gx = ld32(c, M, (uint32_t)RD(c, REG_R2, 4) - 0x73f8);
    uint32_t n = ld16(c, M, gx + 4), sz = ld16(c, M, gx + 6), bytes = n * sz;
    uint8_t h[3], *p = h;
    fifo8(&p, 0x98);
    fifo16(&p, n);
    hw_gather_copy(h, 3);
    if (bytes) {
        static const uint8_t z[64];
        for (uint32_t w = (bytes + 3) >> 2; w; ) {
            uint32_t k = w > 16 ? 16 : w;
            hw_gather_copy(z, 4 * k);
            w -= k;
        }
    }
    mst(c, M, gx + 2, 2, 1);
}
static void gx_call_dl(Ctx *c, uint8_t *M, uint32_t list, uint32_t size) {
    uint32_t r2 = (uint32_t)RD(c, REG_R2, 4);
    uint32_t gx = ld32(c, M, r2 - 0x73f8);
    if (ld32(c, M, gx + 0x5ac)) dirty_state(c, M);
    gx = ld32(c, M, r2 - 0x73f8);
    if (!ld32(c, M, gx)) send_flush_prim(c, M);
    uint8_t b[9], *p = b;
    fifo8(&p, 0x40);
    fifo32(&p, list);
    fifo32(&p, size);
    hw_gather_copy(b, 9);
}
static uint64_t i_GXCallDisplayList(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    gx_call_dl(c, M, (uint32_t)a0, (uint32_t)a1);
    return 0;
}

/* The shape packet's texture-matrix / CP block: XF 0x30 = a, 0x40 = b, then an XF write of the
 * two at 0x1018 (two words). */
static void shp_texblock(Ctx *c, uint8_t *M, uint32_t p) {
    uint32_t a = ld32(c, M, p + 0x40), bb = ld32(c, M, p + 0x44);
    uint8_t b[25], *q = b;
    fifo8(&q, 8); fifo8(&q, 0x30); fifo32(&q, a);
    fifo8(&q, 8); fifo8(&q, 0x40); fifo32(&q, bb);
    fifo8(&q, 0x10); fifo16(&q, 1); fifo16(&q, 0x1018); fifo32(&q, a); fifo32(&q, bb);
    hw_gather_copy(b, 25);
}
/* The material display list (shape +0x2c) again unless it was the last one (SDA r13-0x6688). */
static void shp_matdl(Ctx *c, uint8_t *M, uint32_t p, uint32_t r13) {
    uint32_t dl = ld32(c, M, p + 0x2c);
    if (ld32(c, M, r13 - 0x6688) != dl) {
        gx_call_dl(c, M, dl, 0xc0);
        mst(c, M, r13 - 0x6688, 4, ld32(c, M, p + 0x2c));
    }
}
static uint64_t i_FUN_80050ed0(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t p = (uint32_t)a0;
    shp_matdl(c, M, p, (uint32_t)RD(c, REG_R13, 4));
    shp_texblock(c, M, p);
    return 0;
}

static uint64_t i_GXSetArray(Ctx *c, uint8_t *M, ARGS);
static uint64_t i_FUN_80067a64(Ctx *c, uint8_t *M, ARGS);
static uint64_t i_FUN_80067ae8(Ctx *c, uint8_t *M, ARGS);
static void shape_mtx_load(Ctx *c, uint8_t *M, uint32_t m);
/* J3DShape draw (vtable +0xc of the shapes MKDD uses): CP array bases for the position and
 * normal matrices, the shape's flags into j3dSys / the SDA, then per matrix group its J3DShapeMtx
 * load (vtable +0x18, guest) and J3DShapeDraw (GXCallDisplayList of its list). Flag 0x200: one
 * matrix (the current view) for all groups. */
static uint64_t i_FUN_80050f70(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t p = (uint32_t)a0, r13 = (uint32_t)RD(c, REG_R13, 4);
    shp_matdl(c, M, p, r13);
    if (mld(c, M, r13 - 0x6684, 1) && !mld(c, M, p + 0x48, 1)) shp_texblock(c, M, p);
    uint32_t fl = ld32(c, M, p + 0xc);
    mst(c, M, r13 - 0x6658, 4, (fl & 0x1c) >> 2);
    {
        uint8_t b[18], *q = b;
        fifo8(&q, 8); fifo8(&q, 0xa0); fifo32(&q, ld32(c, M, J3DSYS + 0x10c) & 0x7fffffffu);
        hw_gather_copy(b, 6);
        if (!mld(c, M, p + 0x34, 1)) {
            q = b;
            fifo8(&q, 8); fifo8(&q, 0xa1); fifo32(&q, ld32(c, M, J3DSYS + 0x110) & 0x7fffffffu);
            hw_gather_copy(b, 6);
        }
        q = b;
        fifo8(&q, 8); fifo8(&q, 0xa2); fifo32(&q, ld32(c, M, J3DSYS + 0x114) & 0x7fffffffu);
        hw_gather_copy(b, 6);
    }
    uint32_t cur = ld32(c, M, ld32(c, M, p + 0x60)) << 2;
    uint32_t pos = ld32(c, M, ld32(c, M, p + 0x58) + cur);
    mst(c, M, J3DSYS + 0x104, 4, pos);
    i_GXSetArray(c, M, 0x15, pos, 0x30, 0, 0, 0, 0, 0);
    cur = ld32(c, M, ld32(c, M, p + 0x60)) << 2;
    uint32_t nrm = ld32(c, M, ld32(c, M, p + 0x5c) + cur);
    mst(c, M, J3DSYS + 0x108, 4, nrm);
    i_GXSetArray(c, M, 0x16, nrm, 0x24, 0, 0, 0, 0, 0);
    fl = ld32(c, M, p + 0xc);
    mst(c, M, r13 - 0x6654, 4, ld32(c, M, p + 0x54));
    mst(c, M, r13 - 0x6650, 1, mld(c, M, p + 0x34, 1));
    mst(c, M, r13 - 0x6684, 1, mld(c, M, p + 0x48, 1));
    mst(c, M, r13 - 0x664c, 4, fl & 0xf000);
    uint32_t n = ld16(c, M, p + 0xa);
    if (!(fl & 0x200)) {
        if (mld(c, M, r13 - 0x664f, 1))
            for (int i = 0; i < 10; i++) mst(c, M, 0x803afe54u + 2 * i, 2, 0xffff); /* resetMtxLoadCache */
        uint32_t mtx = ld32(c, M, p + 0x38), drw = ld32(c, M, p + 0x3c);
        for (uint32_t i = 0; (i & 0xffff) < n; i++) {
            uint32_t o = 4 * (i & 0xffff);
            uint32_t m = ld32(c, M, mtx + o);
            if (m) shape_mtx_load(c, M, m);
            uint32_t d = ld32(c, M, drw + o);
            if (d) gx_call_dl(c, M, ld32(c, M, d + 8), ld32(c, M, d + 4));
        }
    } else {
        uint32_t view = ld32(c, M, ld32(c, M, J3DSYS + 0x40) + 0x30);
        i_FUN_80067a64(c, M, view, 0, 0, 0, 0, 0, 0, 0);
        i_FUN_80067ae8(c, M, ld32(c, M, ld32(c, M, J3DSYS + 0x40) + 0x30), 0, 0, 0, 0, 0, 0, 0);
        uint32_t drw = ld32(c, M, p + 0x3c);
        for (uint32_t i = 0; (i & 0xffff) < n; i++) {
            uint32_t d = ld32(c, M, drw + 4 * (i & 0xffff));
            if (d) gx_call_dl(c, M, ld32(c, M, d + 8), ld32(c, M, d + 4));
        }
    }
    return 0;
}

/* Shape packet draw (FUN_800507a0): unless hidden (+0x10 bit 4) or shapeless, publishes the
 * vertex arrays (J3DVertexBuffer::setArray), model data and packet in j3dSys, mirrors model flags
 * into the shape (+0xc bits 2 and 3), points the shape at the packet's matrices, syncs j3dSys
 * flag 0x40000000 (J3DModelData::syncJ3DSysFlags), sets the SDA texture-matrix slots and calls
 * the shape's draw (vtable +0xc; FUN_80050f70 natively). */
static uint64_t i_FUN_800507a0(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t p = (uint32_t)a0, r13 = (uint32_t)RD(c, REG_R13, 4);
    if (ld32(c, M, p + 0x10) & 0x10) return 0;
    if (!ld32(c, M, p + 0x28)) return 0;
    uint32_t vb = ld32(c, M, p + 0x38) + 0x88;
    uint32_t v0 = ld32(c, M, vb + 0x2c), v1 = ld32(c, M, vb + 0x30), v2 = ld32(c, M, vb + 0x34);
    mst(c, M, J3DSYS + 0x10c, 4, v0);
    mst(c, M, J3DSYS + 0x110, 4, v1);
    mst(c, M, J3DSYS + 0x114, 4, v2);
    uint32_t md = ld32(c, M, p + 0x38);
    mst(c, M, J3DSYS + 0x38, 4, md);
    mst(c, M, J3DSYS + 0x40, 4, p);
    mst(c, M, r13 - 0x664f, 1, (ld32(c, M, md + 8) & 0x10) != 0);
    uint32_t s = ld32(c, M, p + 0x28);
    uint32_t sf = ld32(c, M, s + 0xc);
    sf = (ld32(c, M, ld32(c, M, p + 0x38) + 8) & 4) ? sf | 4 : sf & ~4u;
    mst(c, M, s + 0xc, 4, sf);
    if ((ld32(c, M, ld32(c, M, p + 0x38) + 8) & 8) && !(ld32(c, M, s + 0xc) & 0x100))
        mst(c, M, s + 0xc, 4, ld32(c, M, s + 0xc) | 8);
    else
        mst(c, M, s + 0xc, 4, ld32(c, M, s + 0xc) & ~8u);
    uint32_t mp = ld32(c, M, p + 0x2c);
    mst(c, M, s + 0x60, 4, mp + 0x30);
    mst(c, M, s + 0x54, 4, ld32(c, M, ld32(c, M, p + 0x2c) + 4));
    mst(c, M, s + 0x58, 4, ld32(c, M, ld32(c, M, p + 0x2c) + 0x18));
    if (!mld(c, M, s + 0x34, 1)) mst(c, M, s + 0x5c, 4, ld32(c, M, ld32(c, M, p + 0x2c) + 0x20));
    else mst(c, M, s + 0x5c, 4, ld32(c, M, ld32(c, M, ld32(c, M, p + 0x2c) + 0x28) + (ld32(c, M, s + 0x64) << 2)));
    uint32_t mdd = ld32(c, M, ld32(c, M, p + 0x38) + 4);
    uint32_t sysf = ld32(c, M, J3DSYS + 0x34);
    mst(c, M, J3DSYS + 0x34, 4, (ld32(c, M, mdd + 8) & 0x20) ? sysf | 0x40000000u : sysf & ~0x40000000u);
    if (ld32(c, M, p + 0x24)) {
        mst(c, M, r13 - 0x6640, 4, ld32(c, M, ld32(c, M, ld32(c, M, p + 0x28) + 4) + 0x28));
        mst(c, M, r13 - 0x663c, 4, ld32(c, M, p + 0x24));
    } else {
        mst(c, M, r13 - 0x6640, 4, 0);
    }
    s = ld32(c, M, p + 0x28);
    uint32_t fn = ld32(c, M, ld32(c, M, s) + 0xc);
    if (fn == 0x80050F70u) i_FUN_80050f70(c, M, s, 0, 0, 0, 0, 0, 0, 0);
    else gcall(c, M, fn, s, 0, 0);
    return 0;
}

/* J3DMatPacket::draw(): material load (vtable +0x18 of +0x30, guest), the material display list
 * (+0x20: list, size), the first shape packet's texture block (FUN_80050ed0 on its +0x28), then
 * every shape packet in the chain (+0x2c, linked by +4) with its own list first; clears the
 * last-list SDA slot. */
static uint64_t i_draw__12J3DMatPacketFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t t = (uint32_t)a0, r13 = (uint32_t)RD(c, REG_R13, 4);
    uint32_t mat = ld32(c, M, t + 0x30);
    gcall(c, M, ld32(c, M, ld32(c, M, mat) + 0x18), mat, 0, 0);
    uint32_t dl = ld32(c, M, t + 0x20);
    gx_call_dl(c, M, ld32(c, M, dl), ld32(c, M, dl + 8));
    uint32_t sp = ld32(c, M, t + 0x2c);
    i_FUN_80050ed0(c, M, ld32(c, M, sp + 0x28), 0, 0, 0, 0, 0, 0, 0);
    for (; sp; sp = ld32(c, M, sp + 4)) {
        uint32_t l = ld32(c, M, sp + 0x20);
        if (l) gx_call_dl(c, M, ld32(c, M, l), ld32(c, M, l + 8));
        i_FUN_800507a0(c, M, sp, 0, 0, 0, 0, 0, 0, 0);
    }
    mst(c, M, r13 - 0x6688, 4, 0);
    return 0;
}

/* J3DDrawBuffer::drawHead(): every packet of every bucket (+0: bucket array, +4: count; packets
 * linked by +4), packet->draw() (vtable +0xc; J3DMatPacket::draw natively). */
static uint64_t i_drawHead__13J3DDrawBufferCFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t b = (uint32_t)a0;
    uint32_t n = ld32(c, M, b + 4), arr = ld32(c, M, b);
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t pk = ld32(c, M, arr + 4 * i); pk; pk = ld32(c, M, pk + 4)) {
            uint32_t fn = ld32(c, M, ld32(c, M, pk) + 0xc);
            if (fn == 0x80050078u) i_draw__12J3DMatPacketFv(c, M, pk, 0, 0, 0, 0, 0, 0, 0);
            else gcall(c, M, fn, pk, 0, 0);
        }
    return 0;
}


/* ---- J3DShapeMtx matrix loads (the shape's vtable +0x18 from FUN_80050f70) ---- */
/* FUN_80067b58(Mtx33 m, u32 id): 3x3 normal matrix at XF 0x400 + id * 3 */
static void xf_nrm33(Ctx *c, uint8_t *M, uint32_t m, uint32_t id) {
    uint32_t addr = (id * 3 + 0x400) & 0xFFFF;
    uint8_t b[5 + 36] = {0x10, 0x00, 0x08, (uint8_t)(addr >> 8), (uint8_t)addr};
    for (int i = 0; i < 9; i++) put_be(b + 5 + 4 * i, 4, ld32(c, M, m + 4 * i));
    hw_gather_copy(b, sizeof b);
}
static uint64_t i_FUN_80067b58(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    xf_nrm33(c, M, (uint32_t)a0, (uint32_t)a1);
    return 0;
}
/* FUN_80067c64(Mtx33 m, u32 id): a 3x3 matrix as a 3x4 (fourth column the r2 constant, 0) at
 * XF id * 4 (texture matrix from a normal matrix) */
static uint64_t i_FUN_80067c64(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t m = (uint32_t)a0, addr = ((uint32_t)a1 * 4) & 0xFFFF;
    float z = ldf(c, M, (uint32_t)RD(c, REG_R2, 4) - 0x797c);
    uint32_t zb;
    memcpy(&zb, &z, 4);
    uint8_t b[5 + 48] = {0x10, 0x00, 0x0b, (uint8_t)(addr >> 8), (uint8_t)addr};
    for (int r = 0; r < 3; r++) {
        for (int k = 0; k < 3; k++) put_be(b + 5 + 16 * r + 4 * k, 4, ld32(c, M, m + 12 * r + 4 * k));
        put_be(b + 5 + 16 * r + 12, 4, zb);
    }
    hw_gather_copy(b, sizeof b);
    return 0;
}
/* J3DScaleNrmMtx33(Mtx33 m, const Vec *s): column j *= s[j] */
static void scale_nrm33(Ctx *c, uint8_t *M, uint32_t m, uint32_t s) {
    double sv[3] = {ldf(c, M, s), ldf(c, M, s + 4), ldf(c, M, s + 8)};
    float v[9];
    ldfn(c, M, m, v, 9);
    for (int i = 0; i < 9; i++) v[i] = (float)rs((double)v[i] * sv[i % 3]);
    stfn(c, M, m, v, 9);
}
static uint64_t i_J3DScaleNrmMtx33__FPA3_fRC3Vec(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    scale_nrm33(c, M, (uint32_t)a0, (uint32_t)a1);
    return 0;
}

static uint64_t i_FUN_80067bc8(Ctx *c, uint8_t *M, ARGS);
static uint64_t i_J3DPSCalcInverseTranspose__FPA4_fPA3_f(Ctx *c, uint8_t *M, ARGS);
/* loadNrmMtx of J3DShapeMtxConcatView (multi = false: XF slots 0x1e / 0) and
 * J3DShapeMtxMultiConcatView (slots 0x1e + 3 * slot / 3 * slot) for the position matrix m of
 * matrix index idx: an index whose kind byte (SDA r13-0x6654) is 1 uses m's 3x3 as is, others
 * its inverse transpose; with texture-matrix mode 0x2000 that also goes to the texture slot;
 * scaled by j3dSys +0x118 when the SDA flag r13-0x6650 is set. Temporaries in the original's
 * (dead) stack frame. */
static void load_nrm(Ctx *c, uint8_t *M, bool multi, uint32_t slot, uint32_t idx, uint32_t m) {
    uint32_t r13 = (uint32_t)RD(c, REG_R13, 4), sp = ctx_gpr(c, 1) - 0x60;
    uint32_t idt = multi ? slot * 3 + 0x1e : 0x1e, idn = multi ? slot * 3 : 0;
    if (mld(c, M, ld32(c, M, r13 - 0x6654) + (idx & 0xffff), 1) == 1) {
        if (ld32(c, M, r13 - 0x664c) == 0x2000) i_FUN_80067bc8(c, M, m, idt, 0, 0, 0, 0, 0, 0);
        if (!mld(c, M, r13 - 0x6650, 1)) { i_FUN_80067ae8(c, M, m, idn, 0, 0, 0, 0, 0, 0); return; }
        uint32_t t = sp + 0x2c;
        float v[9];
        for (int r = 0; r < 3; r++)
            for (int k = 0; k < 3; k++) v[3 * r + k] = ldf(c, M, m + 16 * r + 4 * k);
        stfn(c, M, t, v, 9);
        scale_nrm33(c, M, t, ld32(c, M, J3DSYS + 0x118));
        xf_nrm33(c, M, t, idn);
        return;
    }
    uint32_t t = sp + 8;
    i_J3DPSCalcInverseTranspose__FPA4_fPA3_f(c, M, m, t, 0, 0, 0, 0, 0, 0);
    if (ld32(c, M, r13 - 0x664c) == 0x2000) i_FUN_80067c64(c, M, t, idt, 0, 0, 0, 0, 0, 0);
    if (mld(c, M, r13 - 0x6650, 1)) scale_nrm33(c, M, t, ld32(c, M, J3DSYS + 0x118));
    xf_nrm33(c, M, t, idn);
}
static uint64_t i_loadNrmMtx__21J3DShapeMtxConcatViewCFiUsPA4_f(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    load_nrm(c, M, false, (uint32_t)a1, (uint32_t)a2, (uint32_t)a3);
    return 0;
}
static uint64_t i_loadNrmMtx__26J3DShapeMtxMultiConcatViewCFiUsPA4_f(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    load_nrm(c, M, true, (uint32_t)a1, (uint32_t)a2, (uint32_t)a3);
    return 0;
}

static uint64_t i_PSMTXConcat(Ctx *c, uint8_t *M, ARGS);
/* J3DShapeMtxConcatView::loadMtxConcatView_PNGP(int slot, u16 idx): view * the model's matrix
 * idx (j3dSys +0x104 array) into the frame at sp+8, the differed texture matrices if any (SDA
 * r13-0x6640; J3DDifferedTexMtx::loadExecute, guest) from it, the position matrix to XF slot*3,
 * then the normal matrix (vtable +0x24: the two loadNrmMtx natively). */
static uint64_t i_loadMtxConcatView_PNGP__21J3DShapeMtxConcatViewCFiUs(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t t = (uint32_t)a0, slot = (uint32_t)a1, idx = (uint32_t)a2 & 0xffff, idx_raw = (uint32_t)a2;
    uint32_t r13 = (uint32_t)RD(c, REG_R13, 4);
    uint32_t old = ctx_gpr(c, 1), sp = old - 0x50, m = sp + 8;
    mst(c, M, sp, 4, old);
    ctx_set_gpr(c, 1, sp);
    i_PSMTXConcat(c, M, ld32(c, M, ld32(c, M, J3DSYS + 0x40) + 0x30), ld32(c, M, J3DSYS + 0x104) + idx * 0x30, m, 0, 0, 0, 0, 0);
    if (ld32(c, M, r13 - 0x6640)) {
        ctx_set_gpr(c, 3, m);
        ctx_call(c, 0x80076EB8u);
    }
    i_FUN_80067a64(c, M, m, slot * 3, 0, 0, 0, 0, 0, 0);
    uint32_t fn = ld32(c, M, ld32(c, M, t) + 0x24);
    if (fn == 0x8007799Cu) load_nrm(c, M, false, slot, idx_raw, m);
    else if (fn == 0x80077DF8u) load_nrm(c, M, true, slot, idx_raw, m);
    else {
        ctx_set_gpr(c, 3, t);
        ctx_set_gpr(c, 4, slot);
        ctx_set_gpr(c, 5, idx_raw);
        ctx_set_gpr(c, 6, m);
        ctx_call(c, fn);
    }
    ctx_set_gpr(c, 1, old);
    return 0;
}

/* __ptmf_scall(this, r4, r5) through the pointer-to-member at ptmf: this += delta; a
 * non-negative vtable offset means virtual. loadMtxConcatView_PNGP natively. */
static void ptmf_call(Ctx *c, uint8_t *M, uint32_t ptmf, uint32_t t, uint32_t r4, uint32_t r5) {
    uint32_t delta = ld32(c, M, ptmf), vo = ld32(c, M, ptmf + 4), fn = ld32(c, M, ptmf + 8);
    t += delta;
    if ((int32_t)vo >= 0) fn = ld32(c, M, ld32(c, M, t + fn) + vo);
    if (fn == 0x800774CCu) { i_loadMtxConcatView_PNGP__21J3DShapeMtxConcatViewCFiUs(c, M, t, r4, r5, 0, 0, 0, 0, 0); return; }
    ctx_set_gpr(c, 3, t);
    ctx_set_gpr(c, 4, r4);
    ctx_set_gpr(c, 5, r5);
    ctx_set_gpr(c, 12, fn);
    ctx_call(c, fn);
}
/* Position matrix array for matrix index idx (j3dSys +0x104): the model's world or view
 * matrices (SDA r13-0x6648 / -0x6644, chosen by the model data's kind byte), CP array 0x15. */
static void set_pos_array(Ctx *c, uint8_t *M, uint32_t idx, uint32_t r13) {
    uint32_t md = ld32(c, M, J3DSYS + 0x38);
    uint32_t kind = mld(c, M, ld32(c, M, ld32(c, M, md + 4) + 0x48) + idx, 1);
    uint32_t arr = ld32(c, M, r13 - 0x6648 + (kind << 2));
    mst(c, M, J3DSYS + 0x104, 4, arr);
    i_GXSetArray(c, M, 0x15, arr, 0x30, 0, 0, 0, 0, 0);
}
/* The model's matrix arrays into the SDA (r13-0x6648 / -0x6644 from model +0x84 +0x34 / +0x10). */
static void publish_mtx_arrays(Ctx *c, uint8_t *M, uint32_t r13) {
    uint32_t mdl = ld32(c, M, ld32(c, M, J3DSYS + 0x38) + 0x84);
    mst(c, M, r13 - 0x6648, 4, ld32(c, M, mdl + 0x34));
    mst(c, M, r13 - 0x6644, 4, ld32(c, M, mdl + 0x10));
}
/* J3DShapeMtxConcatView::load(): the arrays for the shape's matrix index (+4), loaded into slot
 * 0 by the loader of the current texture mode (ptmf table 0x80340e38, entry SDA r13-0x6658). */
static uint64_t i_load__21J3DShapeMtxConcatViewCFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t t = (uint32_t)a0, r13 = (uint32_t)RD(c, REG_R13, 4);
    publish_mtx_arrays(c, M, r13);
    uint32_t ptmf = 0x80340E38u + ld32(c, M, r13 - 0x6658) * 0xc;
    uint32_t idx = ld16(c, M, t + 4);
    set_pos_array(c, M, idx, r13);
    uint32_t md = ld32(c, M, J3DSYS + 0x38);
    uint32_t mi = ld16(c, M, ld32(c, M, ld32(c, M, md + 4) + 0x4c) + (idx << 1));
    uint32_t old = ctx_gpr(c, 1);
    mst(c, M, old - 0x20, 4, old);
    ctx_set_gpr(c, 1, old - 0x20);
    ptmf_call(c, M, ptmf, t, 0, mi);
    ctx_set_gpr(c, 1, old);
    return 0;
}

/* J3DShapeMtxMultiConcatView::load(): matrix indices +0xc (count +8; 0xffff = unused), loaded
 * into slots 0.. Without the SDA flag r13-0x664f: per index its array and the mode's loader
 * (table 0x80340e38). With it: one array (the world matrices), the matrix load cache at
 * 0x803afe54 skips a slot that already holds the index, and the loader comes from table
 * 0x80340e98 (0x80340e38 for kind 0). */
static uint64_t i_load__26J3DShapeMtxMultiConcatViewCFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t t = (uint32_t)a0, r13 = (uint32_t)RD(c, REG_R13, 4);
    uint32_t old = ctx_gpr(c, 1);
    mst(c, M, old - 0x50, 4, old);
    ctx_set_gpr(c, 1, old - 0x50);
    bool cached = mld(c, M, r13 - 0x664f, 1) != 0;
    uint32_t mdl = ld32(c, M, ld32(c, M, J3DSYS + 0x38) + 0x84);
    uint32_t world = ld32(c, M, mdl + 0x34);
    mst(c, M, r13 - 0x6648, 4, world);
    mst(c, M, r13 - 0x6644, 4, ld32(c, M, mdl + 0x10));
    int32_t n = (int32_t)ld16(c, M, t + 8);
    uint32_t arr = ld32(c, M, t + 0xc);
    if (!cached) {
        uint32_t ptmf = 0x80340E38u + ld32(c, M, r13 - 0x6658) * 0xc;
        for (int32_t i = 0; i < n; i++) {
            uint32_t k = ld16(c, M, arr + 2 * (uint32_t)i);
            if (k == 0xffff) continue;
            uint32_t md4 = ld32(c, M, ld32(c, M, J3DSYS + 0x38) + 4);
            uint32_t kind = mld(c, M, ld32(c, M, md4 + 0x48) + k, 1);
            uint32_t mi = ld16(c, M, ld32(c, M, md4 + 0x4c) + (k << 1));
            uint32_t pa = ld32(c, M, r13 - 0x6648 + (kind << 2));
            mst(c, M, J3DSYS + 0x104, 4, pa);
            i_GXSetArray(c, M, 0x15, pa, 0x30, 0, 0, 0, 0, 0);
            ptmf_call(c, M, ptmf, t, (uint32_t)i, mi);
        }
    } else {
        uint32_t mis = ld32(c, M, ld32(c, M, ld32(c, M, J3DSYS + 0x38) + 4) + 0x40);
        mst(c, M, J3DSYS + 0x104, 4, world);
        i_GXSetArray(c, M, 0x15, world, 0x30, 0, 0, 0, 0, 0);
        n = (int32_t)ld16(c, M, t + 8);
        for (int32_t i = 0; i < n; i++) {
            uint32_t e = ld32(c, M, r13 - 0x6658) * 0xc;
            uint32_t k = ld16(c, M, arr + 2 * (uint32_t)i);
            if (k == 0xffff) continue;
            uint32_t mi = ld16(c, M, mis + (k << 1));
            if (mi == ld16(c, M, 0x803AFE54u + 2 * (uint32_t)i)) continue;
            uint32_t kind = mld(c, M, ld32(c, M, ld32(c, M, ld32(c, M, J3DSYS + 0x38) + 4) + 0x48) + k, 1);
            ptmf_call(c, M, (kind ? 0x80340E98u : 0x80340E38u) + e, t, (uint32_t)i, mi);
            mst(c, M, 0x803AFE54u + 2 * (uint32_t)i, 2, mi);
        }
    }
    ctx_set_gpr(c, 1, old);
    return 0;
}

/* The J3DShapeMtx load (vtable +0x18) from the shape draw: the two concat-view kinds natively. */
static void shape_mtx_load(Ctx *c, uint8_t *M, uint32_t m) {
    uint32_t fn = ld32(c, M, ld32(c, M, m) + 0x18);
    if (fn == 0x800778D0u) i_load__21J3DShapeMtxConcatViewCFv(c, M, m, 0, 0, 0, 0, 0, 0, 0);
    else if (fn == 0x80077C08u) i_load__26J3DShapeMtxMultiConcatViewCFv(c, M, m, 0, 0, 0, 0, 0, 0, 0);
    else gcall(c, M, fn, m, 0, 0);
}


/* ---- 2D (HUD) ---- */
/* K2DPicture::getNewColor(TColor out[4]): the picture's four corner colours (+0x150), alphas
 * scaled by the picture's alpha (+0xb3) and then the global one (SDA byte r13-0x79f0), each
 * a * k / 255 when k != 255. */
static uint64_t i_getNewColor__10K2DPictureFPQ28JUtility6TColor(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t t = (uint32_t)a0, out = (uint32_t)a1, r13 = (uint32_t)RD(c, REG_R13, 4);
    uint8_t col[16];
    for (int i = 0; i < 16; i++) col[i] = (uint8_t)mld(c, M, t + 0x150 + i, 1);
    uint32_t k = (uint32_t)mld(c, M, t + 0xb3, 1);
    if (k != 0xff)
        for (int i = 3; i < 16; i += 4) col[i] = (uint8_t)((col[i] * k) / 0xff);
    k = (uint32_t)mld(c, M, r13 - 0x79f0, 1);
    if (k != 0xff)
        for (int i = 3; i < 16; i += 4) col[i] = (uint8_t)((col[i] * k) / 0xff);
    for (int i = 0; i < 16; i++) mst(c, M, out + i, 1, col[i]);
    return 0;
}

/* ---- MSL fdlibm sin / cos (double; the recompiled code rounds every multiply and add, as
 * plain C with -ffp-contract=off does). Constants from the SDA2 (r2). ---- */
ALWAYS_INLINE int32_t hi_word(double x) { return (int32_t)(dbits(x) >> 32); }
ALWAYS_INLINE double kd(Ctx *c, uint8_t *M, uint32_t r2, int32_t o) { return ps_d(mld(c, M, r2 + (uint32_t)o, 8)); }
static double k_cos(Ctx *c, uint8_t *M, uint32_t r2, double x, double y) {
    int32_t ix = hi_word(x) & 0x7fffffff;
    if (ix < 0x3e400000 && fctiwz_(x) == 0) return kd(c, M, r2, -0x6f58);
    double z = x * x;
    double r = kd(c, M, r2, -0x6f28) * z + kd(c, M, r2, -0x6f30);
    r = z * r + kd(c, M, r2, -0x6f38);
    r = z * r + kd(c, M, r2, -0x6f40);
    r = z * r + kd(c, M, r2, -0x6f48);
    r = z * r + kd(c, M, r2, -0x6f50);
    r = z * r;
    double half = kd(c, M, r2, -0x6f20), one = kd(c, M, r2, -0x6f58);
    if (ix < 0x3fd33333) return one - (half * z - (z * r - x * y));
    double qx;
    if (ix > 0x3fe90000) qx = kd(c, M, r2, -0x6f18);
    else qx = ps_d((uint64_t)(uint32_t)(ix - 0x00200000) << 32);
    double hz = half * z - qx, a = one - qx;
    return a - (hz - (z * r - x * y));
}
static double k_sin(Ctx *c, uint8_t *M, uint32_t r2, double x, double y, int iy) {
    int32_t ix = hi_word(x) & 0x7fffffff;
    if (ix < 0x3e400000 && fctiwz_(x) == 0) return x;
    double z = x * x, v = z * x;
    double r = kd(c, M, r2, -0x6eb0) * z + kd(c, M, r2, -0x6eb8);
    r = z * r + kd(c, M, r2, -0x6ec0);
    r = z * r + kd(c, M, r2, -0x6ec8);
    r = z * r + kd(c, M, r2, -0x6ed0);
    double s1 = kd(c, M, r2, -0x6ea8);
    if (!iy) return v * (z * r + s1) + x;
    double t = kd(c, M, r2, -0x6ea0) * y - v * r;
    t = z * t - y;
    t = -(s1 * v - t);
    return x - t;
}
/* __ieee754_rem_pio2(x, y): n with x = n*pi/2 + y[0] + y[1]. Huge arguments (|x| > 2^19*pi/2):
 * false, the caller runs the original. */
static bool rem_pio2(Ctx *c, uint8_t *M, uint32_t r2, double x, double y[2], int32_t *n_out) {
    int32_t hx = hi_word(x), ix = hx & 0x7fffffff;
    double p1 = kd(c, M, r2, -0x6fa8), p1t = kd(c, M, r2, -0x6fa0), p2 = kd(c, M, r2, -0x6f98), p2t = kd(c, M, r2, -0x6f90);
    if (ix <= 0x3fe921fb) { y[0] = x; y[1] = kd(c, M, r2, -0x6fb0); *n_out = 0; return true; }
    if (ix < 0x4002d97c) {
        bool exact = (uint32_t)(ix - 0x3ff90000) == 0x21fb;
        if (hx > 0) {
            double z = x - p1;
            if (!exact) { y[0] = z - p1t; y[1] = (z - y[0]) - p1t; }
            else { z = z - p2; y[0] = z - p2t; y[1] = (z - y[0]) - p2t; }
            *n_out = 1;
        } else {
            double z = p1 + x;
            if (!exact) { y[0] = p1t + z; y[1] = p1t + (z - y[0]); }
            else { z = z + p2; y[0] = p2t + z; y[1] = p2t + (z - y[0]); }
            *n_out = -1;
        }
        return true;
    }
    if (ix > 0x413921fb) {
        if (ix >= 0x7ff00000) { y[0] = y[1] = x - x; *n_out = 0; return true; }
        return false;
    }
    double t = fabs(x);
    int32_t n = fctiwz_(kd(c, M, r2, -0x6f80) * t + kd(c, M, r2, -0x6f88));
    double fn = (double)n;
    double r = -(p1 * fn - t), w = p1t * fn;
    if (n < 32 && ix != (int32_t)ld32(c, M, 0x80329F90u + 4u * (uint32_t)n - 4u)) {
        y[0] = r - w;
    } else {
        int32_t j = ix >> 20;
        y[0] = r - w;
        if (j - ((hi_word(y[0]) >> 20) & 0x7ff) > 16) {
            double tt = r;
            w = p2 * fn;
            r = tt - w;
            w = p2t * fn - ((tt - r) - w);
            y[0] = r - w;
            if (j - ((hi_word(y[0]) >> 20) & 0x7ff) > 49) {
                tt = r;
                w = kd(c, M, r2, -0x6f78) * fn;
                r = tt - w;
                w = kd(c, M, r2, -0x6f70) * fn - ((tt - r) - w);
                y[0] = r - w;
            }
        }
    }
    y[1] = (r - y[0]) - w;
    if (hx < 0) { y[0] = -y[0]; y[1] = -y[1]; n = -n; }
    *n_out = n;
    return true;
}
/* sin / cos: |x| <= pi/4 straight to the kernel, Inf/NaN -> x - x, else by quadrant. */
static uint64_t sincos_n(Ctx *c, uint8_t *M, double x, bool is_cos, uint32_t orig) {
    uint32_t r2 = (uint32_t)RD(c, REG_R2, 4);
    int32_t ix = hi_word(x) & 0x7fffffff;
    if (ix <= 0x3fe921fb)
        return dbits(is_cos ? k_cos(c, M, r2, x, kd(c, M, r2, -0x6e28)) : k_sin(c, M, r2, x, kd(c, M, r2, -0x6de0), 0));
    if (ix >= 0x7ff00000) return dbits(x - x);
    double y[2];
    int32_t n;
    if (!rem_pio2(c, M, r2, x, y, &n)) {
        WR(c, 0x4008, 8, dbits(x));
        ctx_call_orig(c, orig);
        return RD(c, 0x4008, 8);
    }
    switch ((n & 3) + (is_cos ? 4 : 0)) {
    case 0: return dbits(k_sin(c, M, r2, y[0], y[1], 1));
    case 1: return dbits(k_cos(c, M, r2, y[0], y[1]));
    case 2: return dbits(-k_sin(c, M, r2, y[0], y[1], 1));
    case 3: return dbits(-k_cos(c, M, r2, y[0], y[1]));
    case 4: return dbits(k_cos(c, M, r2, y[0], y[1]));
    case 5: return dbits(-k_sin(c, M, r2, y[0], y[1], 1));
    case 6: return dbits(-k_cos(c, M, r2, y[0], y[1]));
    default: return dbits(k_sin(c, M, r2, y[0], y[1], 1));
    }
}
static uint64_t i_sin(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    return sincos_n(c, M, ps_d(a0), false, 0x8010BF24u);
}
static uint64_t i_cos(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    return sincos_n(c, M, ps_d(a0), true, 0x8010B9BCu);
}


/* ==== batch 5 ==== */
/* J3DAnmTexPattern::getTexNo(u16 idx, u16 *out): table entry idx (+0x10, 8 bytes: count, first)
 * picks out of the texture-number list (+0xc): frame (+8) < 0 the first, >= count the last,
 * else first + (int)frame. The count converts through the 0x4330 double trick (r2-0x79f0). */
static uint64_t i_getTexNo__16J3DAnmTexPatternCFUsPUs(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t t = (uint32_t)a0, off = ((uint32_t)a1 & 0xffff) << 3, out = (uint32_t)a2;
    uint32_t r2 = (uint32_t)RD(c, REG_R2, 4);
    double frame = ldf(c, M, t + 8);
    uint32_t tab = ld32(c, M, t + 0x10), n = ld16(c, M, tab + off);
    uint32_t list = ld32(c, M, t + 0xc), first = ld16(c, M, tab + off + 2);
    uint32_t i;
    if (frame < (double)ldf(c, M, r2 - 0x7a0c)) i = first;
    else {
        double fn = rs(ps_d(0x4330000000000000ull | n) - ps_d(mld(c, M, r2 - 0x79f0, 8)));
        if (frame >= fn) i = n + first - 1;
        else i = (uint32_t)fctiwz_(frame) + first;
    }
    mst(c, M, out, 2, ld16(c, M, list + ((i << 1) & ~1u)));
    return 0;
}

/* FUN_80183820(obj): for every model slot (count +4; models +0xc, data +8) with a model: each
 * of its material entries (data +0x7c count; model +0xc4, 0x3c bytes) gets its +0xc object's
 * +8 = 0xffff and flag 0x10 at +0x10; without one, obj +0x1a = 0xffff. */
static uint64_t i_FUN_80183820(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t t = (uint32_t)a0;
    for (uint32_t i = 0; (i & 0xffff) < ld16(c, M, t + 4); i++) {
        uint32_t o = (i << 2) & 0x3fffc;
        if (!ld32(c, M, ld32(c, M, t + 0xc) + o)) { mst(c, M, t + 0x1a, 2, 0xffff); continue; }
        uint32_t n = ld16(c, M, ld32(c, M, ld32(c, M, t + 8) + o) + 0x7c);
        for (uint32_t j = 0; (j & 0xffff) < n; j++) {
            uint32_t e = ld32(c, M, ld32(c, M, o + ld32(c, M, t + 0xc)) + 0xc4) + (j & 0xffff) * 0x3c;
            mst(c, M, ld32(c, M, e + 0xc) + 8, 2, 0xffff);
            mst(c, M, e + 0x10, 4, ld32(c, M, e + 0x10) | 0x10);
        }
    }
    return 0;
}

/* rlwimi: rotate s left by sh, insert under the mask (LSB-numbered value m) */
ALWAYS_INLINE uint32_t rlwimi_(uint32_t a, uint32_t s, unsigned sh, uint32_t m) {
    uint32_t r = sh ? (s << sh) | (s >> (32 - sh)) : s;
    return (a & ~m) | (r & m);
}
/* GXSetTevOrder(stage, coord, map, color): the stage pair's TEV order register (gx +0x100 +
 * stage/2*4; odd stages in the high half), map saved at +0x554, texture-coordinate enable bit in
 * +0x598, then BP 0x61 with the register, dirty bit 1. Colour channel through the table at
 * 0x80346a40 (0xff = 7). */
static uint64_t i_GXSetTevOrder(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    int32_t stage = (int32_t)a0, coord = (int32_t)a1;
    uint32_t map = (uint32_t)a2, color = (uint32_t)a3;
    uint32_t r2 = (uint32_t)RD(c, REG_R2, 4), gx = ld32(c, M, r2 - 0x73f8);
    uint32_t p = gx + 0x100 + (uint32_t)(stage / 2) * 4;
    mst(c, M, gx + 0x554 + (uint32_t)stage * 4, 4, map);
    uint32_t tm = map & ~0x100u;
    if (tm >= 8) tm = 0;
    uint32_t bit = ((uint32_t)stage & 63) < 32 ? 1u << ((uint32_t)stage & 31) : 0;
    uint32_t en598 = ld32(c, M, gx + 0x598);
    if (coord >= 8) { en598 &= ~bit; coord = 0; }
    else en598 |= bit;
    mst(c, M, gx + 0x598, 4, en598);
    uint32_t ras = color == 0xff ? 7 : ld32(c, M, 0x80346A40u + (color << 2));
    uint32_t en = ((int32_t)map != 0xff && !(map & 0x100)) ? 1 : 0;
    uint32_t v = ld32(c, M, p);
    if (stage & 1) {
        v = rlwimi_(v, tm, 12, 0x7000);
        v = rlwimi_(v, (uint32_t)coord, 15, 0x38000);
        v = rlwimi_(v, ras, 19, 0x380000);
        v = rlwimi_(v, en, 18, 0x40000);
    } else {
        v = rlwimi_(v, tm, 0, 0x7);
        v = rlwimi_(v, (uint32_t)coord, 3, 0x38);
        v = rlwimi_(v, ras, 7, 0x380);
        v = rlwimi_(v, en, 6, 0x40);
    }
    mst(c, M, p, 4, v);
    uint8_t b[5] = {0x61};
    put_be(b + 1, 4, v);
    hw_gather_copy(b, 5);
    gx = ld32(c, M, r2 - 0x73f8);
    mst(c, M, gx + 2, 2, 0);
    mst(c, M, gx + 0x5ac, 4, ld32(c, M, gx + 0x5ac) | 1);
    return 0;
}

static uint64_t i_J3DGetKeyFrameInterpolation_s___FfP18J3DAnmKeyTableBasePs(Ctx *c, uint8_t *M, ARGS);
/* J3DAnmTevRegKey::getTevColorReg(u16 idx, GXColorS10 *out): per component (r, g, b, a: key
 * tables at entry idx (+0x48, 0x1c bytes) +0/+6/+0xc/+0x12, values +0x50..+0x5c): no keys 0, one
 * key its value, else the s16 Hermite interpolation at the frame (+8) clamped to -0x400..0x3ff
 * through the original's GQR5 quantised store (here into the dead stack slot it used). */
static uint64_t i_getTevColorReg__15J3DAnmTevRegKeyCFUsP11_GXColorS10(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t t = (uint32_t)a0, out = (uint32_t)a2, r2 = (uint32_t)RD(c, REG_R2, 4);
    uint32_t e = ld32(c, M, t + 0x48) + ((uint32_t)a1 & 0xffff) * 0x1c;
    uint32_t sp = ctx_gpr(c, 1) - 0x40, gqr5 = (uint32_t)RD(c, REG_GQR0 + 5 * 4, 4);
    for (uint32_t k = 0; k < 4; k++) {
        uint32_t tb = e + 6 * k, vals = ld32(c, M, t + 0x50 + 4 * k);
        uint32_t cnt = ld16(c, M, tb), first = ld16(c, M, tb + 2);
        uint32_t v;
        if ((int32_t)cnt == 0) v = 0;
        else if ((int32_t)cnt == 1) v = ld16(c, M, vals + ((first << 1) & ~1u));
        else {
            double f = ps_d(i_J3DGetKeyFrameInterpolation_s___FfP18J3DAnmKeyTableBasePs(c, M, tb, vals + ((first << 1) & ~1u),
                                                                                           dbits(ldf(c, M, t + 8)),
                                                                                           0, 0, 0, 0, 0));
            if (f < (double)ldf(c, M, r2 - 0x79e4)) v = 0xfc00;
            else if (f > (double)ldf(c, M, r2 - 0x79e0)) v = 0x3ff;
            else {
                uint32_t slot = sp + 0x14 - 4 * k;
                ps_store(c, M, slot, gqr5, 1, f, 1.0);
                v = ld16(c, M, slot);
            }
        }
        mst(c, M, out + 2 * k, 2, v);
    }
    return 0;
}

/* __cvt_fp2unsigned(f64): 0 below the lower limit (0x80329bf0), 0xffffffff at or above the upper
 * (+8; also NaN), values from 2^31 (+0x10) truncated after subtracting it, plus 0x80000000. */
static uint64_t i___cvt_fp2unsigned(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    double x = ps_d(a0);
    double lo = ps_d(mld(c, M, 0x80329BF0u, 8)), hi = ps_d(mld(c, M, 0x80329BF8u, 8)), half = ps_d(mld(c, M, 0x80329C00u, 8));
    if (x < lo) return 0;
    if (!(x < hi)) return 0xffffffffu;
    if (x < half) return (uint32_t)fctiwz_(x);
    return (uint32_t)fctiwz_(x - half) - 0x80000000u;
}

static uint64_t i_diff__11J3DMaterialFUl(Ctx *c, uint8_t *M, ARGS);
/* J3DModel::diff(): per material (model data +0x5c count, +0x60 list) its packet into j3dSys
 * +0x3c (model +0xc0, 64 bytes each), then material->diff(model +0xc) (vtable +0x24;
 * J3DMaterial::diff natively). */
static uint64_t i_diff__8J3DModelFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t m = (uint32_t)a0;
    uint32_t n = ld16(c, M, ld32(c, M, m + 4) + 0x5c);
    uint32_t old = ctx_gpr(c, 1);
    mst(c, M, old - 0x20, 4, old);
    ctx_set_gpr(c, 1, old - 0x20);
    for (uint32_t i = 0; (i & 0xffff) < n; i++) {
        uint32_t k = i & 0xffff;
        mst(c, M, J3DSYS + 0x3c, 4, ld32(c, M, m + 0xc0) + (k << 6));
        uint32_t mat = ld32(c, M, ld32(c, M, ld32(c, M, m + 4) + 0x60) + (k << 2));
        uint32_t arg = ld32(c, M, m + 0xc);
        uint32_t fn = ld32(c, M, ld32(c, M, mat) + 0x24);
        if (fn == 0x80053178u) i_diff__11J3DMaterialFUl(c, M, mat, arg, 0, 0, 0, 0, 0, 0);
        else {
            ctx_set_gpr(c, 3, mat);
            ctx_set_gpr(c, 4, arg);
            ctx_call(c, fn);
        }
    }
    ctx_set_gpr(c, 1, old);
    return 0;
}

/* Race2D::draw (the race HUD) and Race2D::drawCourse (the map), single player only: bracketed by a
 * marker in the FIFO (a CP write to register 0x01, which the hardware does not have) so the
 * decoder can move their draws to the screen edges in the widescreen HUD (gx.c XV_HUD_LEFT). The
 * marker is 1 (place each draw by its position) or 2 (drawCourse: the map and the top-4 rank column;
 * nothing there stays in the middle). The
 * console count is RaceMgr (r13 - 0x5c38) -> +0x38 -> s16 at +0x20 (getConsoleNumber). */
static void hud_zone(uint32_t on) {
    uint8_t b[6], *p = b;
    fifo8(&p, 0x08);
    fifo8(&p, 0x01);
    fifo32(&p, on);
    hw_gather_copy(b, 6);
}
static void race_hud_call(Ctx *c, uint8_t *M, uint32_t fn, uint32_t self, uint32_t zone) {
    uint32_t rm = ld32(c, M, ctx_gpr(c, 13) - 0x5c38), info = rm ? ld32(c, M, rm + 0x38) : 0;
    bool one = info && (int16_t)ld16(c, M, info + 0x20) == 1;
    if (one) hud_zone(zone);
    uint32_t old = ctx_gpr(c, 1), sp = old - 0x20;
    mst(c, M, sp, 4, old);
    ctx_set_gpr(c, 1, sp);
    ctx_set_gpr(c, 3, self);
    ctx_call_orig(c, fn); /* the recompiled function's entry would come back here */
    ctx_set_gpr(c, 1, old);
    if (one) hud_zone(0);
}
volatile uint32_t g_race_frames;
static uint64_t i_draw__6Race2DFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    g_race_frames++;
    aspect_refresh(c, M); /* once per race frame */
    race_hud_call(c, M, 0x80145CB0u, (uint32_t)a0, 1);
    return 0;
}
static uint64_t i_drawCourse__6Race2DFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    race_hud_call(c, M, 0x801483B4u, (uint32_t)a0, 2); /* map, its kart icons and the top-4 rank column */
    return 0;
}
/* WipeManager::draw: the fades and wipes over each kart's 3D view (the black screen of a respawn,
 * race start and end). They fill the 3D viewport, which widescreen widens, so they stay full
 * width: marker 3, any number of players (J2DManager::draw calls it after Race2D::draw). */
static uint64_t i_draw__11WipeManagerFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    hud_zone(3);
    uint32_t old = ctx_gpr(c, 1), sp = old - 0x20;
    mst(c, M, sp, 4, old);
    ctx_set_gpr(c, 1, sp);
    ctx_set_gpr(c, 3, (uint32_t)a0);
    ctx_call_orig(c, 0x8012A358u);
    ctx_set_gpr(c, 1, old);
    hud_zone(0);
    return 0;
}

/* ---- GX SDK: dirty state, texture loads (per particle in JParticle) ---- */
#define GXDATA(c, M) ld32(c, M, (uint32_t)RD(c, REG_R2, 4) - 0x73f8)
/* __SetSURegs(map, coord): texture size of map (tImage0 +0x514: width-1 bits 0-9, height-1
 * 10-19) into the coordinate's su_ts0/su_ts1 (+0xb8 / +0xd8, low 16 bits), bias bit 16 set for
 * a repeat wrap (mode0 +0x534 bits 0-1 / 2-3 == 1); both sent as BP writes; +2 = 0. */
static void su_regs(Ctx *c, uint8_t *M, uint32_t gx, uint32_t map, uint32_t coord) {
    uint32_t im = ld32(c, M, gx + 0x514 + 4 * map), mode = ld32(c, M, gx + 0x534 + 4 * map);
    uint32_t s0 = ld32(c, M, gx + 0xb8 + 4 * coord), s1 = ld32(c, M, gx + 0xd8 + 4 * coord);
    s0 = (s0 & 0xFFFF0000u) | (im & 0x3FF);
    s1 = (s1 & 0xFFFF0000u) | ((im >> 10) & 0x3FF);
    s0 = (s0 & ~0x10000u) | ((mode & 3) == 1 ? 0x10000u : 0);
    s1 = (s1 & ~0x10000u) | (((mode >> 2) & 3) == 1 ? 0x10000u : 0);
    mst(c, M, gx + 0xb8 + 4 * coord, 4, s0);
    mst(c, M, gx + 0xd8 + 4 * coord, 4, s1);
    uint8_t b[10] = {0x61};
    put_be(b + 1, 4, s0);
    b[5] = 0x61;
    put_be(b + 6, 4, s1);
    hw_gather_copy(b, 10);
    mst(c, M, gx + 2, 2, 0);
}
/* __GXSetSUTexRegs: unless every coordinate is in manual scale mode (+0x594 == 0xff), the
 * indirect stages' map/coord pairs (iref +0x120, 6 bits each; count = genMode +0x204 bits 16-18)
 * and the TEV stages' (texmap +0x554, coord from tref +0x100 + 4 * (i / 2), bits 3-5 / 15-17;
 * count = genMode bits 10-13 + 1; only stages enabled in +0x598) whose coordinate is not manual. */
static void su_tex_regs(Ctx *c, uint8_t *M, uint32_t gx) {
    uint32_t man = ld32(c, M, gx + 0x594);
    if (man == 0xff) return;
    uint32_t gen = ld32(c, M, gx + 0x204), nst = ((gen >> 10) & 0xF) + 1, nind = (gen >> 16) & 7;
    for (uint32_t i = 0; i < nind; i++) {
        uint32_t j = i < 4 ? i : 3; /* (stages past 4 repeat the 4th's pair, as the original) */
        uint32_t iref = ld32(c, M, gx + 0x120), map = (iref >> (6 * j)) & 7, coord = (iref >> (6 * j + 3)) & 7;
        if (!(ld32(c, M, gx + 0x594) & (1u << coord))) su_regs(c, M, gx, map, coord);
    }
    for (uint32_t i = 0; i < nst; i++) {
        uint32_t map = ld32(c, M, gx + 0x554 + 4 * i) & ~0x100u;
        uint32_t tref = ld32(c, M, gx + 0x100 + 4 * (i >> 1));
        uint32_t coord = (i & 1) ? (tref >> 15) & 7 : (tref >> 3) & 7;
        if (map != 0xff && !(ld32(c, M, gx + 0x594) & (1u << coord)) && (ld32(c, M, gx + 0x598) & (1u << i)))
            su_regs(c, M, gx, map, coord);
    }
}
/* __GXSetDirtyState: the dirty bits (+0x5ac) read once: 1 texture scales (natively), 2 BP mask,
 * 4 gen mode, 8 VCD, 0x10 VAT, 8|0x10 vertex size limit (guest), then cleared. */
static void dirty_state(Ctx *c, uint8_t *M) {
    uint32_t d = ld32(c, M, GXDATA(c, M) + 0x5ac);
    if (d & 1) su_tex_regs(c, M, GXDATA(c, M));
    if (d & 2) gcall(c, M, 0x800C0600u, 0, 0, 0);
    if (d & 4) gcall(c, M, 0x800BE15Cu, 0, 0, 0);
    if (d & 8) gcall(c, M, 0x800BC5F4u, 0, 0, 0);
    if (d & 0x10) gcall(c, M, 0x800BCF2Cu, 0, 0, 0);
    if (d & 0x18) gcall(c, M, 0x800BC6B0u, 0, 0, 0);
    mst(c, M, GXDATA(c, M) + 0x5ac, 4, 0);
}
static uint64_t i___GXSetDirtyState(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    dirty_state(c, M);
    return 0;
}
/* GXLoadTexObj(obj, id): the region from __GXData's callback (+0x4c8; the default one natively:
 * by format and mipmap flag, 16-byte regions at +0x208 / +0x288 / +0x308 indexed by id % 8),
 * then GXLoadTexObjPreLoaded. */
static uint64_t i_GXLoadTexObj(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t obj = (uint32_t)a0, id = (uint32_t)a1, gx = GXDATA(c, M), cb = ld32(c, M, gx + 0x4c8), reg;
    if (cb == 0x800BA4FCu) {
        int32_t fmt = (int32_t)ld32(c, M, obj + 0x14);
        bool mip = mld(c, M, obj + 0x1f, 1) & 1;
        uint32_t k = (uint32_t)((int32_t)id % 8), base;
        if (fmt == 6) base = mip ? 0x308 : 0x288;
        else if (fmt >= 8 && fmt <= 10) base = 0x208;
        else base = mip ? 0x288 : 0x208;
        reg = gx + base + 16 * k;
    } else {
        reg = gcall(c, M, cb, obj, id, 0);
    }
    return i_GXLoadTexObjPreLoaded(c, M, obj, reg, id, 0, 0, 0, 0, 0);
}
/* JUTTexture::load(tex, id): the palette (+0x2c) first (JUTPalette::load, guest), then the
 * texture object at +0. */
static uint64_t i_FUN_80023828(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t t = (uint32_t)a0;
    if (ld32(c, M, t + 0x2c)) gcall(c, M, 0x8001FB94u, ld32(c, M, t + 0x2c), 0, 0);
    return i_GXLoadTexObj(c, M, t, a1, 0, 0, 0, 0, 0, 0);
}

/* ---- JParticle per-particle draw procs (JPABaseShape.cpp): one quad each, a position matrix
 * and a 32-byte display list. Work data w (JPAEmitterWorkData): +0x4 resource, +0x8 resource
 * manager, +0x120 emitter direction, +0x144/+0x148 global particle scale, +0x184 position/camera
 * matrix, +0x1b4 projection matrix, +0x200 direction type, +0x204 rotation type, +0x208 plane
 * type, +0x20c display-list type, +0x210 projection type. Particle p: +0 position, +0xc local
 * position, +0x24 velocity, +0x54 base axis, +0x60/+0x64 scale, +0x7c status, +0x88 rotation,
 * +0x94 texture index. The type tables hold function pointers (data at 0x803411e0..); only the
 * known ones are done natively, anything else runs the original. ---- */
#define JPA_DL 0x803411A0u
static bool jpa_flags_ok(Ctx *c, uint8_t *M, uint32_t w, bool dir, bool rot) {
    uint32_t prj = ld32(c, M, 0x803411E0u + 4 * ld32(c, M, w + 0x210));
    if (prj != 0x8007D114u && prj != 0x8007D118u) return false;
    if (dir) {
        uint32_t d = ld32(c, M, 0x803411ECu + 4 * ld32(c, M, w + 0x200));
        if (d != 0x8007D748u && d != 0x8007D764u && d != 0x8007D780u && d != 0x8007D7B4u) return false;
        uint32_t pl = ld32(c, M, 0x80341214u + 4 * ld32(c, M, w + 0x208));
        if (pl != 0x8007D984u && pl != 0x8007D9D0u && pl != 0x8007DA1Cu) return false;
    }
    if (rot) {
        uint32_t r = ld32(c, M, 0x80341200u + 4 * ld32(c, M, w + 0x204));
        if (r != 0x8007D868u && r != 0x8007D8A8u && r != 0x8007D8E8u && r != 0x8007D928u) return false;
    }
    return true;
}
/* the original, for types without a native path (the caller stored its registers: flag 4) */
static void jpa_orig(Ctx *c, uint32_t fn, uint32_t w, uint32_t p) {
    ctx_set_gpr(c, 3, w);
    ctx_set_gpr(c, 4, p);
    ctx_call_orig(c, fn);
}
/* GXLoadPosMtxImm / GXLoadTexMtxImm (3x4, id < 0x40) of a host matrix */
static void gx_mtx_h(const float m[12], uint32_t addr) {
    uint8_t b[53] = {0x10};
    put_be(b + 1, 4, addr | 0x000B0000u);
    for (int i = 0; i < 12; i++) { uint32_t u; memcpy(&u, &m[i], 4); put_be(b + 5 + 4 * i, 4, u); }
    hw_gather_copy(b, sizeof b);
}
/* p_prj: 0 nothing, 1 loadPrj (texture matrix 0x1e = projection (+0x1b4) x m) */
static void jpa_prj(Ctx *c, uint8_t *M, uint32_t w, const float m[12]) {
    if (ld32(c, M, 0x803411E0u + 4 * ld32(c, M, w + 0x210)) != 0x8007D118u) return;
    float p[12], t[12];
    ldfn(c, M, w + 0x1b4, p, 12);
    mtx_concat_h(p, m, t);
    gx_mtx_h(t, 0x1e << 2);
}
/* PSMTXMultVec of a host matrix and vector */
static void mtx_vec_h(const float m[12], const float v[3], float o[3]) {
    P v01 = {v[0], v[1]}, v2 = {v[2], 1.0};
    for (int i = 0; i < 3; i++) {
        P t = pmadd((P){m[4 * i + 2], m[4 * i + 3]}, v2, pmul((P){m[4 * i], m[4 * i + 1]}, v01));
        o[i] = (float)psum0(t, t, t).a;
    }
}
/* PSVECCrossProduct a x b */
static void cross_h(const float a[3], const float b[3], float o[3]) {
    P b01 = {b[0], b[1]}, a01 = {a[0], a[1]};
    double az = a[2], bz = b[2];
    P f6 = m10(b01, b01);
    P f4 = pmul(b01, (P){az, az});
    P f7 = pmuls0(b01, a01);
    P f5 = pmsub(a01, (P){bz, bz}, f4);
    P f8 = pmsub(a01, f6, f7);
    o[0] = (float)f5.b;
    o[1] = (float)-f5.a;
    o[2] = (float)-f8.b;
}
/* |v|^2 as the inline TVec3::isZero / normalize code: ((x*x, y*y) + z*z).x + y*y */
static double jpa_len2(const float v[3]) {
    P f3 = pmul((P){v[0], v[1]}, (P){v[0], v[1]});
    double z = v[2];
    P f2 = pmadd((P){z, z}, (P){z, z}, f3);
    return psum0(f2, f3, f3).a;
}
/* TVec3::normalize after isZero: unless l2 <= eps, v *= one Newton step of frsqrte (0.5, 3 from
 * the SDA2) */
static void jpa_norm(Ctx *c, uint8_t *M, uint32_t r2, float v[3], double l2, double eps) {
    if (l2 <= eps) return;
    double s = l2;
    if (!(l2 <= (double)ldf(c, M, r2 - 0x78b8))) {
        double e = rs(1.0 / sqrt(l2));
        double e2 = rs(e * e), eh = rs((double)ldf(c, M, r2 - 0x78c0) * e);
        double k = rs(-(l2 * e2 - (double)ldf(c, M, r2 - 0x789c)));
        s = rs(eh * k);
    }
    for (int i = 0; i < 3; i++) v[i] = (float)((double)v[i] * s);
}
/* p_direction (known types) */
static void jpa_dir(Ctx *c, uint8_t *M, uint32_t w, uint32_t p, float d[3]) {
    uint32_t fn = ld32(c, M, 0x803411ECu + 4 * ld32(c, M, w + 0x200));
    if (fn == 0x8007D748u) ldfn(c, M, p + 0x24, d, 3);
    else if (fn == 0x8007D7B4u) ldfn(c, M, w + 0x120, d, 3);
    else {
        ldfn(c, M, p + 0xc, d, 3);
        if (fn == 0x8007D780u) for (int i = 0; i < 3; i++) d[i] = -d[i];
    }
}
/* p_rot (known types) from the rotation's sin s and cos k */
static void jpa_rot(Ctx *c, uint8_t *M, uint32_t w, uint32_t r2, double s, double k, float m[12]) {
    uint32_t fn = ld32(c, M, 0x80341200u + 4 * ld32(c, M, w + 0x204));
    float z = ldf(c, M, r2 - 0x78b8), one = ldf(c, M, r2 - 0x78bc), S = (float)s, K = (float)k;
    if (fn == 0x8007D928u) {
        double t = rs((double)ldf(c, M, r2 - 0x78a8) * rs((double)one - k)), o = rs((double)ldf(c, M, r2 - 0x78a4) * s);
        float a = (float)(t + o), b = (float)(t - o), dg = (float)(t + k);
        const float v[12] = {dg, b, a, z, a, dg, b, z, b, a, dg, z};
        memcpy(m, v, sizeof v);
    } else if (fn == 0x8007D8A8u) {
        const float v[12] = {one, z, z, z, z, K, -S, z, z, S, K, z};
        memcpy(m, v, sizeof v);
    } else if (fn == 0x8007D8E8u) {
        const float v[12] = {K, -S, z, z, S, K, z, z, z, z, one, z};
        memcpy(m, v, sizeof v);
    } else {
        const float v[12] = {K, z, -S, z, z, one, z, z, S, z, K, z};
        memcpy(m, v, sizeof v);
    }
}
/* p_plane: scale columns 0 / 1 (XY), 0 / 2 (XZ), 0 and 2 / 1 (X) */
static void jpa_plane(Ctx *c, uint8_t *M, uint32_t w, float m[12], double x, double y) {
    uint32_t fn = ld32(c, M, 0x80341214u + 4 * ld32(c, M, w + 0x208));
    double col[3] = {x, fn == 0x8007D9D0u ? 1.0 : y, fn == 0x8007D9D0u ? y : fn == 0x8007DA1Cu ? x : 1.0};
    bool use[3] = {true, fn != 0x8007D9D0u, fn != 0x8007D984u};
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++)
            if (use[k]) m[4 * r + k] = (float)((double)m[4 * r + k] * col[k]);
}

/* JPADrawRotDirection: direction d (p_direction), side e = axis x d, axis = d x e (stored back),
 * all normalised (d or e zero: nothing drawn); rotation (p_rot) scaled by p_plane, then
 * [axis d e pos] x rotation, x the camera matrix. */
static uint64_t i_FUN_8007de24(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t w = (uint32_t)a0, p = (uint32_t)a1;
    if (ld32(c, M, p + 0x7c) & 8) return 0;
    if (!jpa_flags_ok(c, M, w, true, true)) { jpa_orig(c, 0x8007DE24u, w, p); return 0; }
    uint32_t r2 = (uint32_t)RD(c, REG_R2, 4), sc = sc_idx(c, M, p + 0x88);
    double sn = ldf(c, M, sc), cs = ldf(c, M, sc + 4);
    double eps = rs((double)ldf(c, M, r2 - 0x78a0) * ldf(c, M, 0x803C9A1Cu));
    float d[3], e[3], ax[3], pos[3];
    jpa_dir(c, M, w, p, d);
    double l2 = jpa_len2(d);
    if (l2 <= eps) return 0;
    jpa_norm(c, M, r2, d, l2, eps);
    ldfn(c, M, p + 0x54, ax, 3);
    cross_h(ax, d, e);
    l2 = jpa_len2(e);
    if (l2 <= eps) return 0;
    jpa_norm(c, M, r2, e, l2, eps);
    cross_h(d, e, ax);
    jpa_norm(c, M, r2, ax, jpa_len2(ax), eps);
    stfn(c, M, p + 0x54, ax, 3);
    double sx = rs((double)ldf(c, M, w + 0x144) * ldf(c, M, p + 0x60));
    double sy = rs((double)ldf(c, M, w + 0x148) * ldf(c, M, p + 0x64));
    float r[12], v[12], cam[12];
    jpa_rot(c, M, w, r2, sn, cs, r);
    jpa_plane(c, M, w, r, sx, sy);
    ldfn(c, M, p, pos, 3);
    const float t[12] = {ax[0], d[0], e[0], pos[0], ax[1], d[1], e[1], pos[1], ax[2], d[2], e[2], pos[2]};
    mtx_concat_h(t, r, r);
    ldfn(c, M, w + 0x184, cam, 12);
    mtx_concat_h(cam, r, v);
    gx_mtx_h(v, 0);
    jpa_prj(c, M, w, v);
    gx_call_dl(c, M, ld32(c, M, ctx_gpr(c, 13) - 0x7ef0 + 4 * ld32(c, M, w + 0x20c)), 0x20);
    return 0;
}
/* JPADrawBillboard / JPADrawRotBillboard: camera-space position, scale (and rotation) in the
 * screen plane. */
static void jpa_billboard(Ctx *c, uint8_t *M, uint32_t w, uint32_t p, bool rot) {
    uint32_t r2 = (uint32_t)RD(c, REG_R2, 4);
    float cam[12], pp[3], pos[3];
    ldfn(c, M, w + 0x184, cam, 12);
    ldfn(c, M, p, pp, 3);
    mtx_vec_h(cam, pp, pos);
    double sx = rs((double)ldf(c, M, w + 0x144) * ldf(c, M, p + 0x60));
    double sy = rs((double)ldf(c, M, w + 0x148) * ldf(c, M, p + 0x64));
    float z = ldf(c, M, r2 - 0x78b8), one = ldf(c, M, r2 - 0x78bc), m[12];
    if (rot) {
        uint32_t sc = sc_idx(c, M, p + 0x88);
        double s = ldf(c, M, sc), k = ldf(c, M, sc + 4);
        const float v[12] = {(float)(k * sx), (float)(-s * sy), z, pos[0], (float)(s * sx), (float)(k * sy), z, pos[1], z, z, one, pos[2]};
        memcpy(m, v, sizeof v);
    } else {
        const float v[12] = {(float)sx, z, z, pos[0], z, (float)sy, z, pos[1], z, z, one, pos[2]};
        memcpy(m, v, sizeof v);
    }
    gx_mtx_h(m, 0);
    jpa_prj(c, M, w, m);
    gx_call_dl(c, M, JPA_DL, 0x20);
}
static uint64_t i_FUN_8007d304(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t w = (uint32_t)a0, p = (uint32_t)a1;
    if (ld32(c, M, p + 0x7c) & 8) return 0;
    if (!jpa_flags_ok(c, M, w, false, false)) { jpa_orig(c, 0x8007D304u, w, p); return 0; }
    jpa_billboard(c, M, w, p, false);
    return 0;
}
static uint64_t i_FUN_8007d3e8(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t w = (uint32_t)a0, p = (uint32_t)a1;
    if (ld32(c, M, p + 0x7c) & 8) return 0;
    if (!jpa_flags_ok(c, M, w, false, false)) { jpa_orig(c, 0x8007D3E8u, w, p); return 0; }
    jpa_billboard(c, M, w, p, true);
    return 0;
}
/* JPALoadTexAnm(w, p): texture index p+0x94 -> resource's table (+0x38, u16) -> the resource
 * manager's texture list (+0x8) -> JUTTexture at +4, loaded as map 0. */
static uint64_t i_FUN_8007ce10(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t w = (uint32_t)a0, p = (uint32_t)a1;
    uint32_t idx = ld16(c, M, ld32(c, M, ld32(c, M, w + 4) + 0x38) + 2 * (uint32_t)mld(c, M, p + 0x94, 1));
    uint32_t tex = ld32(c, M, ld32(c, M, ld32(c, M, w + 8) + 8) + 4 * idx) + 4;
    return i_FUN_80023828(c, M, tex, 0, 0, 0, 0, 0, 0, 0);
}
/* JPARegistAlpha (prm = emitter +0x108) / JPARegistPrmAlpha (prm = particle +0x8c): each channel
 * times (emitter global prm +0xb8.. + 1) >> 8, alpha also times (p+0x96 + 1) >> 8; TEV colour 1. */
static void jpa_regist(Ctx *c, uint8_t *M, uint32_t w, uint32_t p, uint32_t src) {
    uint32_t em = ld32(c, M, w), col = ld32(c, M, src), out = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t v = (col >> (24 - 8 * i)) & 0xFF;
        v = ((v * ((uint32_t)mld(c, M, em + 0xb8 + (uint32_t)i, 1) + 1)) >> 8) & 0xFF;
        if (i == 3) v = ((v * ((uint32_t)mld(c, M, p + 0x96, 1) + 1)) >> 8) & 0xFF;
        out |= v << (24 - 8 * i);
    }
    uint32_t r1 = ctx_gpr(c, 1), tmp = r1 - 0x10 + 8;
    mst(c, M, tmp, 4, out);
    i_GXSetTevColor(c, M, 1, tmp, 0, 0, 0, 0, 0, 0);
}
static uint64_t i_FUN_8007c198(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    jpa_regist(c, M, (uint32_t)a0, (uint32_t)a1, ld32(c, M, (uint32_t)a0) + 0x108);
    return 0;
}
static uint64_t i_FUN_8007c260(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    jpa_regist(c, M, (uint32_t)a0, (uint32_t)a1, (uint32_t)a1 + 0x8c);
    return 0;
}

/* ---- GX SDK state setters (J2D HUD, J3D materials, particles): PowerPC bit-field code
 * transliterated with rlwinm / rlwimi; __GXData (r2-0x73f8) +2 = bpSentNot. ---- */
ALWAYS_INLINE uint32_t ppc_mask(unsigned mb, unsigned me) {
    uint32_t a = 0xFFFFFFFFu >> mb, b = 0xFFFFFFFFu << (31 - me);
    return mb <= me ? a & b : a | b;
}
ALWAYS_INLINE uint32_t rotl32(uint32_t x, unsigned s) { return s ? (x << s) | (x >> (32 - s)) : x; }
ALWAYS_INLINE uint32_t rlwinm(uint32_t rs, unsigned sh, unsigned mb, unsigned me) { return rotl32(rs, sh) & ppc_mask(mb, me); }
ALWAYS_INLINE uint32_t rlwimi(uint32_t ra, uint32_t rs, unsigned sh, unsigned mb, unsigned me) {
    uint32_t m = ppc_mask(mb, me);
    return (ra & ~m) | (rotl32(rs, sh) & m);
}
static void gx_bp(uint32_t v) {
    uint8_t b[5] = {0x61};
    put_be(b + 1, 4, v);
    hw_gather_copy(b, 5);
}
static void gx_xf1(uint32_t addr, uint32_t v) {
    uint8_t b[9] = {0x10};
    put_be(b + 1, 4, addr);
    put_be(b + 5, 4, v);
    hw_gather_copy(b, 9);
}
/* GXSetTevDirect(stage): GXSetTevIndirect with everything 0: BP 0x10 + stage = 0 */
static uint64_t i_GXSetTevDirect(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    gx_bp(((uint32_t)a0 + 0x10) << 24);
    mst(c, M, GXDATA(c, M) + 2, 2, 0);
    return 0;
}
/* GXSetTevColorOp / GXSetTevAlphaOp(stage, op, bias, scale, clamp, out): the stage's kept
 * combiner word (+0x130 / +0x170 + 4 * stage) updated and sent; ops > 1 (compare) put the op's
 * bits 1-2 in the scale field and bias 3. */
static void tev_op(Ctx *c, uint8_t *M, uint32_t base, const uint64_t *a) {
    uint32_t gx = GXDATA(c, M), stage = (uint32_t)a[0], op = (uint32_t)a[1];
    uint32_t v = rlwimi(ld32(c, M, gx + base + 4 * stage), op, 0x12, 0xd, 0xd);
    if ((int32_t)op <= 1) {
        v = rlwimi(v, (uint32_t)a[3], 0x14, 0xa, 0xb);
        v = rlwimi(v, (uint32_t)a[2], 0x10, 0xe, 0xf);
    } else {
        v = rlwimi(v, op, 0x13, 0xa, 0xb);
        v = rlwimi(v, 3, 0x10, 0xe, 0xf);
    }
    v = rlwimi(v, (uint32_t)a[4], 0x13, 0xc, 0xc);
    v = rlwimi(v, (uint32_t)a[5], 0x16, 0x8, 0x9);
    gx_bp(v);
    mst(c, M, gx + base + 4 * stage, 4, v);
    mst(c, M, gx + 2, 2, 0);
}
static uint64_t i_GXSetTevColorOp(Ctx *c, uint8_t *M, ARGS) {
    const uint64_t a[6] = {a0, a1, a2, a3, a4, a5};
    (void)a6, (void)a7;
    tev_op(c, M, 0x130, a);
    return 0;
}
static uint64_t i_GXSetTevAlphaOp(Ctx *c, uint8_t *M, ARGS) {
    const uint64_t a[6] = {a0, a1, a2, a3, a4, a5};
    (void)a6, (void)a7;
    tev_op(c, M, 0x170, a);
    return 0;
}
/* __GXSetMatrixIndex(i): matrix index word A (+0x80, i < 5) or B (+0x84) to CP 0x30 / 0x40 and
 * XF 0x1018 / 0x1019; +2 = 1. */
static void mtx_index(Ctx *c, uint8_t *M, uint32_t i) {
    uint32_t gx = GXDATA(c, M);
    bool lo = (int32_t)i < 5;
    uint32_t v = ld32(c, M, gx + (lo ? 0x80 : 0x84));
    uint8_t b[15] = {0x08, (uint8_t)(lo ? 0x30 : 0x40)};
    put_be(b + 2, 4, v);
    b[6] = 0x10;
    put_be(b + 7, 4, lo ? 0x1018 : 0x1019);
    put_be(b + 11, 4, v);
    hw_gather_copy(b, 15);
    mst(c, M, gx + 2, 2, 1);
}
static uint64_t i___GXSetMatrixIndex(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    mtx_index(c, M, (uint32_t)a0);
    return 0;
}
/* GXSetTexCoordGen2(dst, func, src, mtx, normalize, pt_mtx): XF 0x1040 + dst (texgen: projection
 * bit 1 for 3x4, source row / form from src, bump / SRTG kinds), XF 0x1050 + dst (post matrix,
 * normalize bit 8); the matrix index kept in +0x80 / +0x84 (6 bits per coordinate) and sent. */
static uint64_t i_GXSetTexCoordGen2(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t dst = (uint32_t)a0, func = (uint32_t)a1, src = (uint32_t)a2, mtx = (uint32_t)a3;
    static const int8_t row[21] = {0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 5, 5, 5, 5, 5, 5, 5, 2, 2};
    static const int8_t form[21] = {1, 1, 1, 1};
    uint32_t r11 = src <= 0x14 ? (uint32_t)row[src] : 5, r12 = src <= 0x14 ? (uint32_t)form[src] : 0, r10 = 0;
    int32_t fn = (int32_t)func;
    if (fn == 1 || fn == 0) {
        r10 = rlwimi(r10, fn == 0 ? 1 : 0, 1, 0x1e, 0x1e);
        r10 = rlwimi(r10, r12, 2, 0x1d, 0x1d);
        r10 = rlwimi(r10, 0, 4, 0x19, 0x1b);
        r10 = rlwimi(r10, r11, 7, 0x14, 0x18);
    } else if (fn >= 2 && fn < 10) {
        r10 = rlwimi(r10, 0, 1, 0x1e, 0x1e);
        r10 = rlwimi(r10, r12, 2, 0x1d, 0x1d);
        r10 = rlwimi(r10, 1, 4, 0x19, 0x1b);
        r10 = rlwimi(r10, r11, 7, 0x14, 0x18);
        r10 = rlwimi(r10, src - 0xc, 0xc, 0x11, 0x13);
        r10 = rlwimi(r10, func - 2, 0xf, 0xe, 0x10);
    } else if (fn == 10) {
        r10 = rlwimi(r10, 0, 1, 0x1e, 0x1e);
        r10 = rlwimi(r10, r12, 2, 0x1d, 0x1d);
        r10 = rlwimi(r10, (int32_t)src == 0x13 ? 2 : 3, 4, 0x19, 0x1b);
        r10 = rlwimi(r10, 2, 7, 0x14, 0x18);
    }
    uint32_t r8 = rlwimi(0, (uint32_t)a5 - 0x40, 0, 0x1a, 0x1f);
    r8 = rlwimi(r8, (uint32_t)a4, 8, 0x17, 0x17);
    uint8_t b[18] = {0x10};
    put_be(b + 1, 4, dst + 0x1040);
    put_be(b + 5, 4, r10);
    b[9] = 0x10;
    put_be(b + 10, 4, dst + 0x1050);
    put_be(b + 14, 4, r8);
    hw_gather_copy(b, 18);
    uint32_t gx = GXDATA(c, M);
    static const uint8_t sh[8] = {6, 0xc, 0x12, 0x18, 0, 6, 0xc, 0x12}, mb[8] = {0x14, 0xe, 0x8, 0x2, 0x1a, 0x14, 0xe, 0x8};
    uint32_t k = dst <= 6 ? dst : 7, w = gx + (k < 4 ? 0x80 : 0x84);
    mst(c, M, w, 4, rlwimi(ld32(c, M, w), mtx, sh[k], mb[k], mb[k] + 5));
    mtx_index(c, M, dst + 1);
    return 0;
}
/* GXSetChanCtrl(chan, enable, amb_src, mat_src, light_mask, diff_fn, attn_fn): XF 0x100e +
 * (chan & 3), and for GX_COLOR0A0 / GX_COLOR1A1 (4 / 5) also 0x1010 / 0x1011; +2 = 1. */
static uint64_t i_GXSetChanCtrl(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t chan = (uint32_t)a0, lm = (uint32_t)a4, diff = (uint32_t)a5, attn = (uint32_t)a6;
    uint32_t v = rlwimi(0, (uint32_t)a1 & 0xFF, 1, 0x1e, 0x1e);
    v = rlwimi(v, (uint32_t)a3, 0, 0x1f, 0x1f);
    if (attn == 0) diff = 0;
    v = rlwimi(v, (uint32_t)a2, 6, 0x19, 0x19);
    v = rlwimi(v, diff, 7, 0x17, 0x18);
    v = rlwimi(v, attn != 2, 9, 0x16, 0x16);
    v = rlwimi(v, attn != 0, 0xa, 0x15, 0x15);
    v = rlwinm(v, 0, 0x1e, 0x19);
    v = rlwimi(v, lm, 2, 0x1a, 0x1d);
    v = rlwinm(v, 0, 0x15, 0x10);
    v = rlwimi(v, lm, 7, 0x11, 0x14);
    gx_xf1((chan & 3) + 0x100e, v);
    if (chan == 4) gx_xf1(0x1010, v);
    else if (chan == 5) gx_xf1(0x1011, v);
    mst(c, M, GXDATA(c, M) + 2, 2, 1);
    return 0;
}
/* GXSetVtxDesc(attr, type): the VCD words kept at +0x14 (matrix indices, position, colours,
 * normal type in bits 11-12 from +0x4d0 when a normal or NBT is on: flags +0x4d4 / +0x4d5) and
 * +0x18 (texcoords, 2 bits each); VCD dirty (+0x5ac bit 3). */
static uint64_t i_GXSetVtxDesc(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t attr = (uint32_t)a0, type = (uint32_t)a1, gx = GXDATA(c, M);
    if (attr <= 8) {
        mst(c, M, gx + 0x14, 4, rlwimi(ld32(c, M, gx + 0x14), type, attr, 31 - attr, 31 - attr));
    } else if (attr == 9 || attr == 11 || attr == 12) {
        unsigned sh = attr == 9 ? 9 : attr == 11 ? 0xd : 0xf;
        mst(c, M, gx + 0x14, 4, rlwimi(ld32(c, M, gx + 0x14), type, sh, 31 - sh - 1, 31 - sh));
    } else if (attr == 10 || attr == 0x19) {
        uint32_t on = attr == 10 ? 0x4d4 : 0x4d5, off = attr == 10 ? 0x4d5 : 0x4d4;
        if ((int32_t)type != 0) {
            mst(c, M, gx + on, 1, 1);
            mst(c, M, gx + off, 1, 0);
            mst(c, M, gx + 0x4d0, 4, type);
        } else {
            mst(c, M, gx + on, 1, 0);
        }
    } else if (attr >= 13 && attr <= 20) {
        unsigned sh = 2 * (attr - 13);
        mst(c, M, gx + 0x18, 4, rlwimi(ld32(c, M, gx + 0x18), type, sh, 30 - sh, 31 - sh));
    }
    gx = GXDATA(c, M);
    uint32_t nt = mld(c, M, gx + 0x4d4, 1) || mld(c, M, gx + 0x4d5, 1) ? ld32(c, M, gx + 0x4d0) : 0;
    mst(c, M, gx + 0x14, 4, rlwimi(ld32(c, M, gx + 0x14), nt, 0xb, 0x13, 0x14));
    mst(c, M, gx + 0x5ac, 4, ld32(c, M, gx + 0x5ac) | 8);
    return 0;
}
/* GXBegin(type, fmt, n): pending dirty state, the flush primitive after a non-draw command,
 * then the draw command byte and vertex count. */
static void send_flush_prim(Ctx *c, uint8_t *M);
static uint64_t i_GXBegin(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    if (ld32(c, M, GXDATA(c, M) + 0x5ac)) dirty_state(c, M);
    if (!ld32(c, M, GXDATA(c, M))) send_flush_prim(c, M);
    uint8_t b[3] = {(uint8_t)((uint32_t)a1 | (uint32_t)a0)};
    put_be(b + 1, 2, (uint32_t)a2);
    hw_gather_copy(b, 3);
    return 0;
}

/* ---- batch 1 (race 4 profile): object collision ---- */
/* Single-precision add / subtract as the recompiled code does them: in double, rounded once. */
ALWAYS_INLINE float fadd(float a, float b) { return (float)((double)a + b); }
ALWAYS_INLINE float fsub(float a, float b) { return (float)((double)a - b); }
/* frsqrte as the recompiled code models it (exact 1/sqrt in double), times x, rounded: fsqrt. */
ALWAYS_INLINE float rsqrt_mul(float x) { return (float)((1.0 / sqrt((double)x)) * x); }

/* ObjColSphere::IsHitSphere(TVec3f posOther, TVec3f posThis, f32 otherRadius): this+0x14 gets
 * posThis - posOther, and on a hit the depth (sum of radii - distance) at +0x10 and the
 * normalised direction. Radius at +4, scale at +0xc; 0.0 at r2-0x45b0. */
#define OBJCOLSPHERE_ISHITSPHERE 0x801f8bf4u
static uint64_t i_IsHitSphere__12ObjColSphereFQ29JGeometry8TVec3_f_Q29JGeometry8TVec3_f_f(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t self = (uint32_t)a0, other = (uint32_t)a1, me = (uint32_t)a2;
    double fr;
    memcpy(&fr, &a3, 8);
    float other_r = (float)fr;
    float dx = fsub(ldf(c, M, me), ldf(c, M, other)), dy = fsub(ldf(c, M, me + 4), ldf(c, M, other + 4));
    float dz = fsub(ldf(c, M, me + 8), ldf(c, M, other + 8));
    float zero = ldf(c, M, (uint32_t)RD(c, REG_R2, 4) - 0x45b0);
    stf(c, M, self + 0x14, dx);
    stf(c, M, self + 0x18, dy);
    stf(c, M, self + 0x1c, dz);
    float sum = madd(ldf(c, M, self + 4), ldf(c, M, self + 0xc), other_r);
    float xx = mul(dx, dx), yy = mul(dy, dy);
    float zz_xx = madd(dz, dz, xx);
    float sum2 = mul(sum, sum);
    float dist2 = fadd(zz_xx, yy);
    float d = fsub(sum2, dist2);
    if (!(d > zero)) return 0; /* ble: NaN too */
    float rsum = sum2 > zero ? rsqrt_mul(sum2) : sum2;
    float dist = dist2 > zero ? rsqrt_mul(dist2) : dist2;
    stf(c, M, self + 0x10, fsub(rsum, dist));
    n_stVecNormalize__FRQ29JGeometry8TVec3_f_(c, M, self + 0x14, 0, 0, 0, 0, 0, 0, 0);
    return 1;
}

/* GeographyObj::IsHitKart(const TVec3f &spherePos, f32 radius, TVec3f *posAdjust): every bound
 * (mBounds +0x134, count u8 +0x140, global positions +0x13c, 12 bytes each) tested with its
 * virtual IsHitSphere (native for ObjColSphere); on a hit with posAdjust and _58 != 2, adds the
 * bound's direction (+0x14) times its depth (+0x10). The positions are passed by value: copies
 * in this function's stack frame (0x90), as the original. */
static uint64_t i_IsHitKart__12GeographyObjFRCQ29JGeometry8TVec3_f_fPQ29JGeometry8TVec3_f_(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t self = (uint32_t)a0, pos = (uint32_t)a1, adj = (uint32_t)a3;
    uint64_t radius = a2;
    uint32_t old_sp = ctx_gpr(c, 1), sp = old_sp - 0x90;
    mst(c, M, sp, 4, old_sp);
    ctx_set_gpr(c, 1, sp);
    float px = ldf(c, M, pos), py = ldf(c, M, pos + 4), pz = ldf(c, M, pos + 8);
    uint32_t bounds = ld32(c, M, self + 0x134), hit = 0;
    for (uint32_t i = 0; i < mld(c, M, self + 0x140, 1); i = (i + 1) & 0xFF, bounds += 4) {
        uint32_t gl = ld32(c, M, self + 0x13c) + i * 0xc;
        for (int k = 0; k < 3; k++) mst(c, M, sp + 0x14 + 4 * k, 4, ld32(c, M, gl + 4 * k));
        stf(c, M, sp + 0x8, px);
        stf(c, M, sp + 0xc, py);
        stf(c, M, sp + 0x10, pz);
        uint32_t b = ld32(c, M, bounds), fn = ld32(c, M, ld32(c, M, b) + 8), r;
        if (fn == OBJCOLSPHERE_ISHITSPHERE) {
            r = (uint32_t)i_IsHitSphere__12ObjColSphereFQ29JGeometry8TVec3_f_Q29JGeometry8TVec3_f_f(c, M, b, sp + 0x14, sp + 0x8, radius, 0, 0, 0, 0);
        } else {
            ctx_set_gpr(c, 3, b);
            ctx_set_gpr(c, 4, sp + 0x14);
            ctx_set_gpr(c, 5, sp + 0x8);
            WR(c, 0x4008, 8, radius);
            ctx_call(c, fn);
            r = ctx_gpr(c, 3);
        }
        if (!(r & 0xFF)) continue;
        if (adj && ld32(c, M, self + 0x58) != 2) {
            b = ld32(c, M, bounds);
            float depth = ldf(c, M, b + 0x10);
            float sx = mul(ldf(c, M, b + 0x14), depth), sy = mul(ldf(c, M, b + 0x18), depth), sz = mul(ldf(c, M, b + 0x1c), depth);
            stf(c, M, adj, fadd(ldf(c, M, adj), sx));
            stf(c, M, adj + 4, fadd(ldf(c, M, adj + 4), sy));
            stf(c, M, adj + 8, fadd(ldf(c, M, adj + 8), sz));
        }
        hit = 1;
    }
    ctx_set_gpr(c, 1, old_sp);
    return hit;
}

/* ---- batch 1: list walks that call a virtual function per element ---- */
/* A stack frame like the original's, for natives that call guest code (callees save LR in it). */
ALWAYS_INLINE uint32_t frame_push(Ctx *c, uint8_t *M, uint32_t size) {
    uint32_t old_sp = ctx_gpr(c, 1), sp = old_sp - size;
    mst(c, M, sp, 4, old_sp);
    ctx_set_gpr(c, 1, sp);
    return old_sp;
}
ALWAYS_INLINE void frame_pop(Ctx *c, uint32_t old_sp) { ctx_set_gpr(c, 1, old_sp); }
/* Virtual call fn(r3[, r4]). */
ALWAYS_INLINE void vcall1(Ctx *c, uint32_t fn, uint32_t r3) { ctx_set_gpr(c, 3, r3); ctx_call(c, fn); }
ALWAYS_INLINE void vcall2(Ctx *c, uint32_t fn, uint32_t r3, uint32_t r4) { ctx_set_gpr(c, 3, r3); ctx_set_gpr(c, 4, r4); ctx_call(c, fn); }

/* ShadowManager::calc(): for each of the four shadow lists (+0x18, +0x28, +0x38, +0x48: JSUList,
 * first link at +8, the list itself the end), each model with any of the low 4 bits of +0x8c set
 * gets its virtual calc (vtable +0x10) and +0x95 = 1, the others +0x95 = 0. Then +0x5f becomes 1
 * if it was 0. */
static uint64_t i_calc__13ShadowManagerFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t self = (uint32_t)a0, old_sp = frame_push(c, M, 0x30);
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t list = self + 0x18 + 0x10 * i, end = list + 8;
        for (uint32_t link = ld32(c, M, list + 8); link != end; link = ld32(c, M, link)) {
            uint32_t m = ld32(c, M, link + 8);
            if (mld(c, M, m + 0x8c, 1) & 0xf) {
                vcall1(c, ld32(c, M, ld32(c, M, m) + 0x10), m);
                mst(c, M, ld32(c, M, link + 8) + 0x95, 1, 1);
            } else {
                mst(c, M, m + 0x95, 1, 0);
            }
        }
    }
    if (!mld(c, M, self + 0x5f, 1)) mst(c, M, self + 0x5f, 1, 1);
    frame_pop(c, old_sp);
    return 0;
}

/* GameAudio::SoundMgr<ObjectSoundMgr>::frameWorkAll(): every manager in the list (head at
 * r13-0x5de8, next at +0x58) gets its virtual frameWork (vtable at +8, slot +0x34). */
static uint64_t i_frameWorkAll__Q29GameAudio38SoundMgr_Q29GameAudio14ObjectSoundMgr_Fv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t old_sp = frame_push(c, M, 0x10);
    for (uint32_t m = ld32(c, M, (uint32_t)RD(c, REG_R13, 4) - 0x5de8); m; m = ld32(c, M, m + 0x58))
        vcall1(c, ld32(c, M, ld32(c, M, m + 8) + 0x34), m);
    frame_pop(c, old_sp);
    return 0;
}

/* GeographyObjManager::setCurrentViewNo(u32 view): every object in the two lists (first links
 * at +0x350 and +0x380; object at link +0, next at +0xc) gets setCurrentViewNo (vtable +0x3c). */
static uint64_t i_setCurrentViewNo__19GeographyObjManagerFUl(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t self = (uint32_t)a0, view = (uint32_t)a1, old_sp = frame_push(c, M, 0x20);
    for (int k = 0; k < 2; k++)
        for (uint32_t link = ld32(c, M, self + (k ? 0x380 : 0x350)); link; link = ld32(c, M, link + 0xc)) {
            uint32_t o = ld32(c, M, link);
            vcall2(c, ld32(c, M, ld32(c, M, o) + 0x3c), o, view);
        }
    frame_pop(c, old_sp);
    return 0;
}

/* ---- batch 1: J3D model / material per-frame work, course object view setup ---- */
#define J3DMATERIAL_CALC 0x80053384u /* J3DMaterial::calc, native below */

/* J3DMaterial::calc(const Mtx m): its texture generators' calc (object at +0x28: vtable +0x14
 * with j3dSys flag 0x40000000, else +0xc; m passed on in r4), calcCurrentMtx, then
 * setCurrentMtx (the material's +0x40 / +0x44 into its shape at +8). */
static uint64_t i_calc__11J3DMaterialFPA4_Cf(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t self = (uint32_t)a0, mtx = (uint32_t)a1, old_sp = frame_push(c, M, 0x10);
    uint32_t o = ld32(c, M, self + 0x28);
    vcall2(c, ld32(c, M, ld32(c, M, o) + ((ld32(c, M, J3DSYS + 0x34) & 0x40000000u) ? 0x14 : 0xc)), o, mtx);
    n_calcCurrentMtx__11J3DMaterialFv(c, M, self, 0, 0, 0, 0, 0, 0, 0);
    uint32_t shape = ld32(c, M, self + 8);
    mst(c, M, shape + 0x40, 4, ld32(c, M, self + 0x40));
    mst(c, M, shape + 0x44, 4, ld32(c, M, self + 0x44));
    frame_pop(c, old_sp);
    return 0;
}

/* J3DModel::calcMaterial(): j3dSys model = this, its flags 4 / 8 from the model's (+8), the model
 * data's sync flag (syncJ3DSysFlags: 0x40000000 from data +8 bit 0x20) and texture info
 * (+0x6c -> j3dSys +0x58); then per material (data +0x60, count u16 +0x5c): j3dSys +0x3c = this
 * +0xc0's 0x40-byte entry, the material animation (+0x3c, a pointer below 0xC0000000) gets
 * calc(material) (vtable +0xc), and the material calc(joint matrix) (vtable +8; the joint at
 * material +0xc, index u16 +0x14, matrices at this +0x84 -> +0xc). */
static uint64_t i_calcMaterial__8J3DModelFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t self = (uint32_t)a0, old_sp = frame_push(c, M, 0x20);
    mst(c, M, J3DSYS + 0x38, 4, self);
    uint32_t f = ld32(c, M, self + 8), s = ld32(c, M, J3DSYS + 0x34);
    s = f & 4 ? s | 4 : s & ~4u;
    s = f & 8 ? s | 8 : s & ~8u;
    s = ld32(c, M, ld32(c, M, self + 4) + 8) & 0x20 ? s | 0x40000000u : s & ~0x40000000u;
    mst(c, M, J3DSYS + 0x34, 4, s);
    uint32_t data = ld32(c, M, self + 4);
    mst(c, M, J3DSYS + 0x58, 4, ld32(c, M, data + 0x6c));
    uint32_t n = ld16(c, M, data + 0x5c);
    for (uint32_t i = 0; i < n; i++) {
        mst(c, M, J3DSYS + 0x3c, 4, ld32(c, M, self + 0xc0) + (i << 6));
        uint32_t mat = ld32(c, M, ld32(c, M, ld32(c, M, self + 4) + 0x60) + 4 * i);
        uint32_t anm = ld32(c, M, mat + 0x3c);
        if (anm && anm < 0xC0000000u) vcall2(c, ld32(c, M, ld32(c, M, anm) + 0xc), anm, mat);
        uint32_t mtx = ld32(c, M, ld32(c, M, self + 0x84) + 0xc) + ld16(c, M, ld32(c, M, mat + 0xc) + 0x14) * 0x30;
        uint32_t fn = ld32(c, M, ld32(c, M, mat) + 8);
        if (fn == J3DMATERIAL_CALC) i_calc__11J3DMaterialFPA4_Cf(c, M, mat, mtx, 0, 0, 0, 0, 0, 0);
        else vcall2(c, fn, mat, mtx);
    }
    frame_pop(c, old_sp);
    return 0;
}

/* J3DMatPacket::beginDiff(): its display list object's beginDL (packet +0x28 -> +0x20): swap the
 * two buffers, interrupts off (old state at r13-0x6698), GDInitGDLObj(&sGDLObj 0x803AD1BC,
 * buffer, size +0xc), __GDCurrentDL (r13-0x6238) = &sGDLObj. */
static uint64_t i_beginDiff__12J3DMatPacketFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t dl = ld32(c, M, ld32(c, M, (uint32_t)a0 + 0x28) + 0x20);
    uint32_t b0 = ld32(c, M, dl), b1 = ld32(c, M, dl + 4);
    mst(c, M, dl, 4, b1);
    mst(c, M, dl + 4, 4, b0);
    uint32_t r13 = (uint32_t)RD(c, REG_R13, 4);
    mst(c, M, r13 - 0x6698, 4, (uint32_t)n_OSDisableInterrupts(c, M, 0, 0, 0, 0, 0, 0, 0, 0));
    const uint32_t gd = 0x803AD1BCu;
    uint32_t start = ld32(c, M, dl), size = ld32(c, M, dl + 0xc);
    mst(c, M, gd, 4, start);
    mst(c, M, gd + 8, 4, start);
    mst(c, M, gd + 0xc, 4, start + size);
    mst(c, M, gd + 4, 4, size);
    mst(c, M, r13 - 0x6238, 4, gd);
    return 0;
}

/* GeographyObjManager::viewCalc(u32 view): for every object in both lists (first links at
 * +0x350 / +0x380; object at link +0, next +0xc): with flag 0x20 at +0xec FUN_80183820(+0x5c),
 * else unless u16 +0x11c has 0x200, FUN_80183934(+0x5c, view, camera +8, &j3dSys,
 * getAxisMaxScale()) (camera: r13-0x4e98 -> +0x200 + 4 * view); then its viewCalc (vtable +0x34). */
static uint64_t i_viewCalc__19GeographyObjManagerFUl(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t self = (uint32_t)a0, view = (uint32_t)a1, old_sp = frame_push(c, M, 0x30);
    uint32_t cam = ld32(c, M, ld32(c, M, (uint32_t)RD(c, REG_R13, 4) - 0x4e98) + ((view << 2) & ~3u) + 0x200) + 8;
    for (int k = 0; k < 2; k++)
        for (uint32_t link = ld32(c, M, self + (k ? 0x380 : 0x350)); link; link = ld32(c, M, link + 0xc)) {
            uint32_t o = ld32(c, M, link);
            if (ld32(c, M, o + 0xec) & 0x20) {
                n_FUN_80183820(c, M, o + 0x5c, 0, 0, 0, 0, 0, 0, 0);
            } else if (!(ld16(c, M, o + 0x11c) & 0x200)) {
                float m = ldf(c, M, o + 0x40), t = ldf(c, M, o + 0x44); /* getAxisMaxScale */
                if (t > m) m = t;
                t = ldf(c, M, o + 0x48);
                if (t > m) m = t;
                n_FUN_80183934(c, M, o + 0x5c, view, cam, J3DSYS, dbits((double)m), 0, 0, 0);
            }
            o = ld32(c, M, link);
            vcall2(c, ld32(c, M, ld32(c, M, o) + 0x34), o, view);
        }
    frame_pop(c, old_sp);
    return 0;
}

/* ---- batch 1: J3DModel::calc, course object vs kart collision ---- */
ALWAYS_INLINE uint32_t slw1(uint32_t n) { n &= 0x3f; return n < 32 ? 1u << n : 0; } /* slw 1, n */

/* J3DModel::calc(): j3dSys model = this with its flags (as calcMaterial); the base scale
 * (+0x8c/+0x94/+0x9c -> +0xb4..); the joint animation (+0xd8, vtable +0xc with the model data),
 * deform (+0xc8), vertex colours (+0xd0), skin (+0xd4, vtable +8), the joint tree's calc (model
 * data +0x10, vtable +8; base scale / matrix from fixed defaults with model flag 2, else +0x18 /
 * +0x24), the bump matrices when needed, skin deform (+0xcc) and the callback (+0x10, (this, 0)).
 * createBumpMtxArray gets r4 / r5 as the joint tree's calc left them, like the original. */
static uint64_t i_calc__8J3DModelFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t self = (uint32_t)a0, old_sp = frame_push(c, M, 0x10), p;
    mst(c, M, J3DSYS + 0x38, 4, self);
    uint32_t f = ld32(c, M, self + 8), s = ld32(c, M, J3DSYS + 0x34);
    s = f & 4 ? s | 4 : s & ~4u;
    s = f & 8 ? s | 8 : s & ~8u;
    s = ld32(c, M, ld32(c, M, self + 4) + 8) & 0x20 ? s | 0x40000000u : s & ~0x40000000u;
    mst(c, M, J3DSYS + 0x34, 4, s);
    mst(c, M, self + 0xb4, 4, ld32(c, M, self + 0x8c));
    mst(c, M, self + 0xb8, 4, ld32(c, M, self + 0x94));
    mst(c, M, self + 0xbc, 4, ld32(c, M, self + 0x9c));
    if ((p = ld32(c, M, self + 0xd8))) vcall2(c, ld32(c, M, ld32(c, M, p) + 0xc), p, ld32(c, M, self + 4));
    if ((p = ld32(c, M, self + 0xc8))) vcall2(c, 0x8005b340u, p, self);
    if ((p = ld32(c, M, self + 0xd0))) vcall2(c, 0x80079248u, p, self);
    if ((p = ld32(c, M, self + 0xd4))) vcall2(c, ld32(c, M, ld32(c, M, p) + 8), p, self);
    mst(c, M, J3DSYS + 0x38, 4, self);
    uint32_t tree = ld32(c, M, self + 4) + 0x10;
    bool fixed = ld32(c, M, self + 8) & 2;
    ctx_set_gpr(c, 3, tree);
    ctx_set_gpr(c, 4, ld32(c, M, self + 0x84));
    ctx_set_gpr(c, 5, fixed ? 0x80328720u : self + 0x18);
    ctx_set_gpr(c, 6, fixed ? 0x8032872Cu : self + 0x24);
    ctx_call(c, ld32(c, M, ld32(c, M, tree) + 8));
    uint32_t data = ld32(c, M, self + 4);
    if (ld16(c, M, data + 0x2e) && !(ld32(c, M, self + 8) & 0x10) && !(ld32(c, M, data + 8) & 0x100))
        vcall1(c, 0x800799d0u, ld32(c, M, self + 0x84));
    if ((p = ld32(c, M, self + 0xcc))) vcall2(c, 0x800791b8u, p, self);
    if ((p = ld32(c, M, self + 0x10))) vcall2(c, p, self, 0);
    frame_pop(c, old_sp);
    return 0;
}

/* GeographyObjManager::Collide_norm(const JSUListIterator<GeographyObj> &it, const int &kart,
 * const TVec3f &pos, const f32 &radius): for an object that collides with this kart (bit in
 * +0xf0) and isn't switched off (+0x11c & 0x80): IsHitKart with this kart's push vector
 * (+0x2c8 + 12 * kart); on a hit setIsHitKartFlg, the object's hit callback (vtable +0x74), the
 * object recorded at +0x28 + 0x50 * kart + 4 * its kind (+0x58) and the push length at +0x2a8 +
 * 4 * kart; otherwise clrIsHitKartSoundFlg. */
static uint64_t i_Collide_norm__19GeographyObjManagerFRC31JSUListIterator_12GeographyObj_RCiRCQ29JGeometry8TVec3_f_RCf(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t self = (uint32_t)a0, it = (uint32_t)a1, kp = (uint32_t)a2, pos = (uint32_t)a3, radp = (uint32_t)a4;
    uint32_t old_sp = frame_push(c, M, 0x20);
    uint32_t k = ld32(c, M, kp), obj = ld32(c, M, ld32(c, M, it));
    if ((ld32(c, M, obj + 0xf0) & slw1(k)) && !(ld16(c, M, obj + 0x11c) & 0x80)) {
        uint32_t adj = self + k * 0xc + 0x2c8;
        uint32_t r = (uint32_t)i_IsHitKart__12GeographyObjFRCQ29JGeometry8TVec3_f_fPQ29JGeometry8TVec3_f_(
            c, M, obj, pos, dbits((double)ldf(c, M, radp)), adj, 0, 0, 0, 0);
        obj = ld32(c, M, ld32(c, M, it));
        k = ld32(c, M, kp);
        uint32_t bit = slw1(k);
        if (r & 0xFF) {
            uint32_t fa = ld16(c, M, obj + 0xfa); /* setIsHitKartFlg */
            if (!(fa & bit)) {
                uint32_t f8 = ld16(c, M, obj + 0xf8);
                if (!(f8 & bit)) {
                    mst(c, M, obj + 0xf8, 2, (f8 | bit) & 0xFFFF);
                } else {
                    mst(c, M, obj + 0xfa, 2, (fa | bit) & 0xFFFF);
                    mst(c, M, obj + 0xf8, 2, ld16(c, M, obj + 0xf8) & ~bit & 0xFFFF);
                }
            }
            mst(c, M, obj + 0xf4, 4, ld32(c, M, obj + 0xf4) | bit);
            obj = ld32(c, M, ld32(c, M, it));
            vcall2(c, ld32(c, M, ld32(c, M, obj) + 0x74), obj, ld32(c, M, kp));
            obj = ld32(c, M, ld32(c, M, it));
            k = ld32(c, M, kp);
            mst(c, M, self + k * 0x50 + ((ld32(c, M, obj + 0x58) << 2) & ~3u) + 0x28, 4, obj);
            uint64_t mag = n_PSVECMag(c, M, self + k * 0xc + 0x2c8, 0, 0, 0, 0, 0, 0, 0);
            double d;
            memcpy(&d, &mag, 8);
            stf(c, M, self + ((k << 2) & ~3u) + 0x2a8, (float)d);
        } else {
            uint32_t m = ~bit; /* clrIsHitKartSoundFlg */
            mst(c, M, obj + 0xf8, 2, ld16(c, M, obj + 0xf8) & m & 0xFFFF);
            mst(c, M, obj + 0xfa, 2, ld16(c, M, obj + 0xfa) & m & 0xFFFF);
        }
    }
    frame_pop(c, old_sp);
    return 0;
}

/* ---- batch 1: particles ---- */
ALWAYS_INLINE float fdiv(float a, float b) { return (float)((double)a / b); }
/* Fade factor of a particle at frame t of life n: from v_in up to 1 while t < n * f_in, from 1
 * down to v_out after n * f_out (as stParticleDraw computes it, single precision throughout);
 * 1 (no change) in between, returned as NAN so the caller keeps its value. */
static float particle_fade(float t, float n, uint32_t age, uint32_t frame, float f_in, float f_out, float v_in, float v_out, float one, bool *set) {
    float in_end = mul(f_in, n), out_start = mul(f_out, n);
    *set = true;
    if (t < in_end) return madd(fsub(one, v_in), fdiv(t, in_end), v_in);
    if (t > out_start) return madd(fsub(one, v_out), fdiv((float)(int32_t)(age - frame), fsub(n, out_start)), v_out);
    *set = false;
    return 0;
}

/* stParticleDraw::drawEachParticle(JSUListIterator<stParticle> &it): for the current particle
 * (it -> link -> particle; life u16 +0x32, frame u16 +0x30) with the emitter's parameters (this
 * +0x20 -> +0x18): x scale (+0x34, fades +0x6c..+0x78) and y scale (+0x38, the same fades, or
 * +0xc8..+0xd4 with flag 0x8000 at +0xb6), both times +0x3c, into r13-0x5504 / -0x5500, the alpha
 * (255 * +0x7c, fades +0x80..+0x8c) into r13-0x54fc; then each draw pass (count u8 r13-0x54fb)
 * through its member function pointer (table 0x803A2A90, 12 bytes each, __ptmf_scall). */
static uint64_t i_drawEachParticle__14stParticleDrawFR29JSUListIterator_10stParticle_(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t self = (uint32_t)a0, it = (uint32_t)a1, old_sp = frame_push(c, M, 0x70);
    uint32_t r2 = (uint32_t)RD(c, REG_R2, 4), r13 = (uint32_t)RD(c, REG_R13, 4);
    float one = ldf(c, M, r2 - 0x44f8), c255 = ldf(c, M, r2 - 0x44b0);
    for (uint32_t i = 0; (i & 0xFF) < mld(c, M, r13 - 0x54fb, 1); i++) {
        bool set;
        uint32_t p = ld32(c, M, ld32(c, M, it)), e = ld32(c, M, ld32(c, M, self + 0x20) + 0x18);
        uint32_t age = ld16(c, M, p + 0x32), frame = ld16(c, M, p + 0x30);
        float n = (float)age, t = (float)frame;
        float s = ldf(c, M, p + 0x34);
        float f = particle_fade(t, n, age, frame, ldf(c, M, e + 0x74), ldf(c, M, e + 0x78), ldf(c, M, e + 0x6c), ldf(c, M, e + 0x70), one, &set);
        if (set) s = mul(s, f);
        stf(c, M, r13 - 0x5504, mul(s, ldf(c, M, p + 0x3c)));
        bool own_y = ld16(c, M, e + 0xb6) & 0x8000;
        s = ldf(c, M, p + 0x38);
        f = own_y ? particle_fade(t, n, age, frame, ldf(c, M, e + 0xd0), ldf(c, M, e + 0xd4), ldf(c, M, e + 0xc8), ldf(c, M, e + 0xcc), one, &set)
                  : particle_fade(t, n, age, frame, ldf(c, M, e + 0x74), ldf(c, M, e + 0x78), ldf(c, M, e + 0x6c), ldf(c, M, e + 0x70), one, &set);
        if (set) s = mul(s, f);
        stf(c, M, r13 - 0x5500, mul(s, ldf(c, M, p + 0x3c)));
        uint32_t alpha = (uint32_t)sat_i32((double)mul(c255, ldf(c, M, e + 0x7c)));
        f = particle_fade(t, n, age, frame, ldf(c, M, e + 0x88), ldf(c, M, e + 0x8c), ldf(c, M, e + 0x80), ldf(c, M, e + 0x84), one, &set);
        if (set) alpha = (uint32_t)sat_i32((double)mul((float)(alpha & 0xFF), f));
        mst(c, M, r13 - 0x54fc, 1, alpha & 0xFF);
        ctx_set_gpr(c, 3, self);
        ctx_set_gpr(c, 4, it);
        ctx_set_gpr(c, 12, 0x803A2A90u + (i & 0xFF) * 0xc);
        ctx_call(c, 0x800ff4d4u); /* __ptmf_scall */
    }
    frame_pop(c, old_sp);
    return 0;
}

/* ---- batch 1: texture coordinate matrices ---- */
#define J3D_IDENTITY_MTX 0x8032872Cu
/* FUN_800724d0(this, Mtx model): for each of the 8 texture generators (+0x38 + 4i, info byte +1
 * low 6 bits = matrix type): +0xc + 6i (u16) = the byte at +0xa + 6i, then FUN_80054c6c(gen,
 * matrix) with the matrix the type needs: types 1/6/7 view * model (j3dSys flag 8: the view
 * alone) without translation, 2/8 model (flag 4: identity), 3/9 view * model (flag 4: the view),
 * 5/10/11 model without translation (flag 8: identity), others identity. Built in the original's
 * stack frame (+8), which the callee reads. */
static uint64_t i_FUN_800724d0(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t self = (uint32_t)a0, mtx = (uint32_t)a1, old_sp = frame_push(c, M, 0x60), tmp = old_sp - 0x60 + 8;
    float zero = ldf(c, M, (uint32_t)RD(c, REG_R2, 4) - 0x7944);
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t gen = ld32(c, M, self + 0x38 + 4 * i);
        if (!gen) continue;
        uint32_t type = mld(c, M, gen + 1, 1) & 0x3f;
        mst(c, M, self + 6 * i + 0xc, 2, mld(c, M, self + 6 * i + 0xa, 1));
        uint32_t flags = ld32(c, M, J3DSYS + 0x34), m = J3D_IDENTITY_MTX;
        bool no_trans = false;
        switch (type) {
        case 1: case 6: case 7:
            if (!(flags & 8)) n_PSMTXConcat(c, M, J3DSYS, mtx, tmp, 0, 0, 0, 0, 0);
            else n_PSMTXCopy(c, M, J3DSYS, tmp, 0, 0, 0, 0, 0, 0);
            m = tmp;
            no_trans = true;
            break;
        case 2: case 8:
            m = flags & 4 ? J3D_IDENTITY_MTX : mtx;
            break;
        case 3: case 9:
            if (!(flags & 4)) { n_PSMTXConcat(c, M, J3DSYS, mtx, tmp, 0, 0, 0, 0, 0); m = tmp; }
            else m = J3DSYS;
            break;
        case 5: case 10: case 11:
            if (!(flags & 8)) { n_PSMTXCopy(c, M, mtx, tmp, 0, 0, 0, 0, 0, 0); m = tmp; no_trans = true; }
            break;
        default:
            break;
        }
        if (no_trans) {
            stf(c, M, tmp + 0xc, zero);
            stf(c, M, tmp + 0x1c, zero);
            stf(c, M, tmp + 0x2c, zero);
        }
        vcall2(c, 0x80054c6cu, gen, m);
    }
    frame_pop(c, old_sp);
    return 0;
}

/* ---- batch 1: 2D pictures (race HUD) ---- */
/* K2DPicture::drawK2D(f32 x, f32 y, f32 w, f32 h, bool tev): when visible (+0xb0): per texture
 * (count u8 +0x110) its load (vtable +0x100), GXSetNumTexGens(count), alpha +0xb3 = +0xb2, the
 * corner colours (getNewColor into four white words), the TEV setup (setTevModeK2D when tev,
 * else the konst colours +0x160 / +0x164 for several textures and the colours +0x14c / +0x148
 * unless those are black / white), makeMatrix(x, y), its matrix (+0x50) as position matrix 0
 * and one quad (w x h, texcoords 0 / 0x80 as u8) written to the FIFO. */
static uint64_t i_drawK2D__10K2DPictureFffffb(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t self = (uint32_t)a0;
    if (!mld(c, M, self + 0xb0, 1)) return 0;
    uint32_t old_sp = frame_push(c, M, 0x80), sp = old_sp - 0x80;
    for (uint32_t i = 0; (i & 0xFF) < mld(c, M, self + 0x110, 1); i++) vcall2(c, ld32(c, M, ld32(c, M, self) + 0x100), self, i);
    vcall1(c, 0x800bd5d8u, mld(c, M, self + 0x110, 1)); /* GXSetNumTexGens */
    mst(c, M, self + 0xb3, 1, mld(c, M, self + 0xb2, 1));
    for (int k = 0; k < 4; k++) mst(c, M, sp + 0x18 + 4 * k, 4, 0xFFFFFFFFu);
    n_getNewColor__10K2DPictureFPQ28JUtility6TColor(c, M, self, sp + 0x18, 0, 0, 0, 0, 0, 0);
    if ((uint32_t)a5 & 0xFF) {
        vcall1(c, 0x8012cae4u, self); /* setTevModeK2D */
    } else {
        if (mld(c, M, self + 0x110, 1) != 1) {
            mst(c, M, sp + 0x14, 4, ld32(c, M, self + 0x160));
            vcall2(c, 0x800c0934u, 0, sp + 0x14); /* GXSetTevKColor */
            mst(c, M, sp + 0x10, 4, ld32(c, M, self + 0x164));
            vcall2(c, 0x800c0934u, 2, sp + 0x10);
        }
        if (ld32(c, M, self + 0x14c) || ld32(c, M, self + 0x148) + 0x10000u != 0xFFFFu) {
            mst(c, M, sp + 0xc, 4, ld32(c, M, self + 0x14c));
            n_GXSetTevColor(c, M, 1, sp + 0xc, 0, 0, 0, 0, 0, 0);
            mst(c, M, sp + 8, 4, ld32(c, M, self + 0x148));
            n_GXSetTevColor(c, M, 2, sp + 8, 0, 0, 0, 0, 0, 0);
        }
    }
    ctx_set_gpr(c, 3, self);
    WR(c, 0x4008, 8, a1);
    WR(c, 0x4010, 8, a2);
    ctx_call(c, 0x8012ceacu); /* makeMatrix(x, y) */
    n_GXLoadPosMtxImm(c, M, self + 0x50, 0, 0, 0, 0, 0, 0, 0);
    n_GXBegin(c, M, 0x80, 0, 4, 0, 0, 0, 0, 0);
    float zero = ldf(c, M, (uint32_t)RD(c, REG_R2, 4) - 0x69a8);
    double dw, dh;
    memcpy(&dw, &a3, 8);
    memcpy(&dh, &a4, 8);
    float w = (float)dw, h = (float)dh;
    const float xs[4] = {zero, w, w, zero}, ys[4] = {zero, zero, h, h};
    const uint32_t col[4] = {0x18, 0x1c, 0x24, 0x20};
    const uint8_t ts[4] = {0, 0x80, 0x80, 0}, tt[4] = {0, 0, 0x80, 0x80};
    uint8_t q[4 * 14];
    for (int v = 0; v < 4; v++) {
        uint8_t *o = q + 14 * v;
        uint32_t u;
        memcpy(&u, &xs[v], 4);
        put_be(o, 4, u);
        memcpy(&u, &ys[v], 4);
        put_be(o + 4, 4, u);
        put_be(o + 8, 4, ld32(c, M, sp + col[v]));
        o[12] = ts[v];
        o[13] = tt[v];
    }
    hw_gather_copy(q, sizeof q);
    frame_pop(c, old_sp);
    return 0;
}

/* ---- batch 1: audio channel mix, shape packet setup ---- */
ALWAYS_INLINE float clampf_ppc(float v, float lo, float hi) { return v <= lo ? lo : v >= hi ? hi : v; } /* NaN passes */
ALWAYS_INLINE void set_fpr(Ctx *c, int n, float v) { WR(c, 0x4000 + 8 * n, 8, dbits((double)v)); }

/* FUN_80093294(channel, a, b, params): a JASChannel's output mix. Pan / fx / dolby sums from the
 * channel (+0xa8.., +0xb8.., +0xc0..) and params (+8.., +0x10), arranged by the driver's output
 * mode (getOutputMode: 1 mono-ish, 2 surround); the volume from the envelope (+0xca / const)
 * squared times +0xb4 * params[0] * +0x9c * (1 + params +0x18 * LFO), at least 0; pan, fx and
 * dolby clamped to [0, 1]; then FUN_80094118(channel, b, ...) when +0x8c is not 0xffff, else
 * FUN_80094008(channel, a, ...), with f1..f4 = volume, pan, fx, dolby. */
static uint64_t i_FUN_80093294(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t ch = (uint32_t)a0, pa = (uint32_t)a1, pb = (uint32_t)a2, prm = (uint32_t)a3;
    uint32_t old_sp = frame_push(c, M, 0xd0), r2 = (uint32_t)RD(c, REG_R2, 4);
    float c0 = ldf(c, M, ch + 0xc0), a8 = ldf(c, M, ch + 0xa8), p8 = ldf(c, M, prm + 8);
    float b8 = ldf(c, M, ch + 0xb8), ac = ldf(c, M, ch + 0xac), pc = ldf(c, M, prm + 0xc);
    float c4 = ldf(c, M, ch + 0xc4), b0 = ldf(c, M, ch + 0xb0), p10 = ldf(c, M, prm + 0x10);
    float half = ldf(c, M, r2 - 0x7678), lo = ldf(c, M, r2 - 0x767c), hi = ldf(c, M, r2 - 0x7680);
    float pan = half, dolby = lo;
    ctx_call(c, 0x8009472cu); /* getOutputMode(): r3..r6 as this function received them */
    uint32_t mode = ctx_gpr(c, 3);
    if (mode == 1 || mode == 2) {
        if (mode == 2) dolby = fadd(b0, fadd(c4, p10));
        float t = fadd(half, fsub(c0, half));
        pan = fadd(fsub(a8, half), fadd(t, fsub(p8, half)));
    }
    float fx = fadd(ac, fadd(b8, pc));
    float env = fdiv((float)ld16(c, M, ch + 0xca), ldf(c, M, r2 - 0x7660));
    env = mul(env, env);
    ctx_set_gpr(c, 3, ch + 0x74);
    ctx_call(c, 0x80095ad0u); /* JASLfo::getValue */
    double lfo;
    { uint64_t u = RD(c, 0x4008, 8); memcpy(&lfo, &u, 8); }
    float k = (float)((double)ldf(c, M, prm + 0x18) * lfo + hi); /* fmadds with the LFO value as returned */
    float v = mul(mul(ldf(c, M, ch + 0x9c), mul(ldf(c, M, ch + 0xb4), ldf(c, M, prm))), k);
    v = mul(env, v);
    if (v < lo) v = lo;
    set_fpr(c, 1, v);
    set_fpr(c, 2, clampf_ppc(pan, lo, hi));
    set_fpr(c, 3, clampf_ppc(fx, lo, hi));
    set_fpr(c, 4, clampf_ppc(dolby, lo, hi));
    bool sub = ld16(c, M, ch + 0x8c) != 0xFFFF;
    ctx_set_gpr(c, 3, ch);
    ctx_set_gpr(c, 4, sub ? pb : pa);
    ctx_call(c, sub ? 0x80094118u : 0x80094008u);
    frame_pop(c, old_sp);
    return 0;
}

/* FUN_800505bc(packet): a shape packet's draw setup unless hidden (+0x10 & 0x10) or without a
 * shape (+0x28): the vertex arrays (model +0x38 -> +0x88: setArray), j3dSys model / packet, the
 * model's flags into the shape (+0xc: 4 and 8, 8 unless the shape has 0x100) and r13-0x664f,
 * the shape's matrices / draw matrices from +0x2c, syncJ3DSysFlags, the skin display list (+0x24
 * -> r13-0x663c / -0x6640), the packet's display list (+0x20) and the shape's draw (vtable +8). */
static uint64_t i_FUN_800505bc(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t self = (uint32_t)a0;
    if ((ld32(c, M, self + 0x10) & 0x10) || !ld32(c, M, self + 0x28)) return 0;
    uint32_t old_sp = frame_push(c, M, 0x10), r13 = (uint32_t)RD(c, REG_R13, 4);
    vcall1(c, 0x8004ecfcu, ld32(c, M, self + 0x38) + 0x88); /* J3DVertexBuffer::setArray */
    uint32_t mdl = ld32(c, M, self + 0x38);
    mst(c, M, J3DSYS + 0x38, 4, mdl);
    mst(c, M, J3DSYS + 0x40, 4, self);
    mst(c, M, r13 - 0x664f, 1, (ld32(c, M, mdl + 8) & 0x10) ? 1 : 0);
    uint32_t mf = ld32(c, M, ld32(c, M, self + 0x38) + 8), shp = ld32(c, M, self + 0x28);
    uint32_t sf = ld32(c, M, shp + 0xc);
    sf = mf & 4 ? sf | 4 : sf & ~4u;
    sf = (mf & 8) && !(sf & 0x100) ? sf | 8 : sf & ~8u;
    mst(c, M, shp + 0xc, 4, sf);
    uint32_t dm = ld32(c, M, self + 0x2c);
    mst(c, M, shp + 0x60, 4, dm + 0x30);
    mst(c, M, shp + 0x54, 4, ld32(c, M, dm + 4));
    mst(c, M, shp + 0x58, 4, ld32(c, M, dm + 0x18));
    if (!mld(c, M, shp + 0x34, 1)) mst(c, M, shp + 0x5c, 4, ld32(c, M, dm + 0x20));
    else mst(c, M, shp + 0x5c, 4, ld32(c, M, ld32(c, M, dm + 0x28) + ((ld32(c, M, shp + 0x64) << 2) & ~3u)));
    uint32_t s = ld32(c, M, J3DSYS + 0x34); /* syncJ3DSysFlags */
    s = ld32(c, M, ld32(c, M, ld32(c, M, self + 0x38) + 4) + 8) & 0x20 ? s | 0x40000000u : s & ~0x40000000u;
    mst(c, M, J3DSYS + 0x34, 4, s);
    if (ld32(c, M, self + 0x24)) {
        mst(c, M, r13 - 0x6640, 4, ld32(c, M, ld32(c, M, ld32(c, M, self + 0x28) + 4) + 0x28));
        mst(c, M, r13 - 0x663c, 4, ld32(c, M, self + 0x24));
    } else {
        mst(c, M, r13 - 0x6640, 4, 0);
    }
    uint32_t dl = ld32(c, M, self + 0x20);
    if (dl) n_GXCallDisplayList(c, M, ld32(c, M, dl), ld32(c, M, dl + 8), 0, 0, 0, 0, 0, 0);
    shp = ld32(c, M, self + 0x28);
    vcall1(c, ld32(c, M, ld32(c, M, shp) + 8), shp);
    frame_pop(c, old_sp);
    return 0;
}

/* ---- batch 1: course/kart model sets ---- */
/* FUN_80182d74(set, u32 view): the model of view `view` in a per-view model set (count u16 +4,
 * models +0xc). View 0 gets its own calc (vtable +0x10); the others copy view 0's joint and
 * weight matrices (data counts u16 +0x2c / +0x2e, matrix buffer +0x84: +0xc / +0x10) and their
 * materials get the shared texture info (set +8 -> .. -> material +0x2c, vtable +0x68 / +0x64).
 * Then, as for every view: viewCalc (+0x14) and diff when the model has a matrix calc (+0xc),
 * entryAll unless j3dSys draws to the shadow buffer (+0x48 = 0x803B1438), prepareShapePackets
 * and, when the first model's first shape has a display list, its update (+0x18). For view 0
 * (also when the model is missing) the first model's base matrix (+0x24) goes to set +0x2c. */
static uint64_t i_FUN_80182d74(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t set = (uint32_t)a0, view = (uint32_t)a1, i = view & 0xFFFF;
    uint32_t old_sp = frame_push(c, M, 0x30), sp = old_sp - 0x30;
    uint32_t mdl = ld32(c, M, ld32(c, M, set + 0xc) + ((view << 2) & 0x3FFFC));
    if (i < ld16(c, M, set + 4) && mdl) {
        if (!i) {
            vcall1(c, ld32(c, M, ld32(c, M, mdl) + 0x10), mdl);
        } else {
            uint32_t first = ld32(c, M, ld32(c, M, set + 0xc));
            for (int k = 0; k < (int)ld16(c, M, ld32(c, M, mdl + 4) + 0x2c); k++)
                n_PSMTXCopy(c, M, ld32(c, M, ld32(c, M, first + 0x84) + 0xc) + 0x30 * k, ld32(c, M, ld32(c, M, mdl + 0x84) + 0xc) + 0x30 * k, 0, 0, 0, 0, 0, 0);
            for (int k = 0; k < (int)ld16(c, M, ld32(c, M, mdl + 4) + 0x2e); k++)
                n_PSMTXCopy(c, M, ld32(c, M, ld32(c, M, first + 0x84) + 0x10) + 0x30 * k, ld32(c, M, ld32(c, M, mdl + 0x84) + 0x10) + 0x30 * k, 0, 0, 0, 0, 0, 0);
            uint32_t src = ld32(c, M, ld32(c, M, ld32(c, M, ld32(c, M, ld32(c, M, set + 8)) + 0x60)) + 0x2c);
            vcall2(c, ld32(c, M, ld32(c, M, src) + 0x68), src, 0);
            uint32_t tex = ctx_gpr(c, 3), data;
            if (tex && (data = ld32(c, M, ld32(c, M, set + 8) + 4))) {
                for (uint32_t k = 0; (k & 0xFFFF) < ld16(c, M, data + 0x5c); k++) {
                    uint32_t mat = ld32(c, M, ld32(c, M, data + 0x60) + ((k << 2) & 0x3FFFC));
                    mst(c, M, sp + 8, 4, ld32(c, M, tex));
                    mst(c, M, sp + 0xc, 4, ld32(c, M, tex + 4));
                    uint32_t t = ld32(c, M, mat + 0x2c);
                    ctx_set_gpr(c, 3, t);
                    ctx_set_gpr(c, 4, 0);
                    ctx_set_gpr(c, 5, sp + 8);
                    ctx_call(c, ld32(c, M, ld32(c, M, t) + 0x64));
                }
            }
        }
        if (ld32(c, M, mdl + 0xc)) {
            vcall1(c, ld32(c, M, ld32(c, M, mdl) + 0x14), mdl);
            n_diff__8J3DModelFv(c, M, mdl, 0, 0, 0, 0, 0, 0, 0);
        }
        if (ld32(c, M, J3DSYS + 0x48) != 0x803B1438u) vcall1(c, ld32(c, M, ld32(c, M, mdl) + 0xc), mdl);
        vcall1(c, 0x8005706cu, mdl); /* prepareShapePackets */
        if (ld32(c, M, ld32(c, M, ld32(c, M, ld32(c, M, set + 0xc)) + 0xc4) + 0x24)) /* FUN_80183d08 */
            vcall1(c, ld32(c, M, ld32(c, M, mdl) + 0x18), mdl);
    }
    if (!(view & 0xFFFF)) {
        uint32_t first = ld32(c, M, ld32(c, M, set + 0xc));
        if (first) n_PSMTXCopy(c, M, first + 0x24, set + 0x2c, 0, 0, 0, 0, 0, 0);
    }
    frame_pop(c, old_sp);
    return 0;
}

/* stParticleDraw::drawParentBillBoardCommon(it, Mtx out): with colour animation on (r13-0x5508),
 * the particle's prim / env colours (+0x50.. / +0x53..) blend from the emitter's start to end
 * colours (+0xa8.. -> +0xab.., +0xae.. -> +0xb1..) by frame / life (or a constant without a life),
 * into r13-0x5510 / -0x550c and TEV colour 2; then TEV colour 1 = prim with the alpha from
 * r13-0x54fc, and out = view (r13-0x54f8) * translation(particle +0x18). */
static uint64_t i_drawParentBillBoardCommon__14stParticleDrawFR29JSUListIterator_10stParticle_PA4_f(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t self = (uint32_t)a0, it = (uint32_t)a1, out = (uint32_t)a2;
    uint32_t old_sp = frame_push(c, M, 0xc0), sp = old_sp - 0xc0;
    uint32_t r13 = (uint32_t)RD(c, REG_R13, 4), r2 = (uint32_t)RD(c, REG_R2, 4);
    if (mld(c, M, r13 - 0x5508, 1)) {
        uint32_t p = ld32(c, M, ld32(c, M, it)), e = ld32(c, M, ld32(c, M, self + 0x20) + 0x18);
        float k = ldf(c, M, r2 - 0x44d4);
        uint32_t n = ld16(c, M, p + 0x32);
        if (n) k = fdiv((float)ld16(c, M, p + 0x30), (float)n);
        static const uint8_t from[6] = {0xa8, 0xa9, 0xaa, 0xae, 0xaf, 0xb0};
        for (int j = 0; j < 6; j++) {
            uint32_t s = mld(c, M, e + from[j], 1), t = mld(c, M, e + from[j] + 3, 1);
            uint32_t v = s + (uint32_t)sat_i32((double)mul((float)(int32_t)(t - s), k));
            mst(c, M, p + 0x50 + j, 1, v & 0xFF);
        }
        for (int j = 0; j < 3; j++) {
            mst(c, M, r13 - 0x5510 + j, 1, mld(c, M, p + 0x50 + j, 1));
            mst(c, M, r13 - 0x550c + j, 1, mld(c, M, p + 0x53 + j, 1));
        }
        mst(c, M, sp + 0xc, 4, ld32(c, M, r13 - 0x550c));
        n_GXSetTevColor(c, M, 2, sp + 0xc, 0, 0, 0, 0, 0, 0);
    }
    mst(c, M, r13 - 0x5510 + 3, 1, mld(c, M, r13 - 0x54fc, 1));
    mst(c, M, sp + 8, 4, ld32(c, M, r13 - 0x5510));
    n_GXSetTevColor(c, M, 1, sp + 8, 0, 0, 0, 0, 0, 0);
    uint32_t p = ld32(c, M, ld32(c, M, it));
    ctx_set_gpr(c, 3, sp + 0x10);
    for (int j = 0; j < 3; j++) WR(c, 0x4008 + 8 * j, 8, dbits((double)ldf(c, M, p + 0x18 + 4 * j)));
    ctx_call(c, 0x800a9bc8u); /* PSMTXTrans */
    n_PSMTXConcat(c, M, ld32(c, M, r13 - 0x54f8), sp + 0x10, out, 0, 0, 0, 0, 0);
    frame_pop(c, old_sp);
    return 0;
}

/* ---- batch 1: shadows ---- */
/* Guest call fn(r3..r3+n-1). */
static void gcalln(Ctx *c, uint32_t fn, int n, const uint32_t *r) {
    for (int i = 0; i < n; i++) ctx_set_gpr(c, 3 + i, r[i]);
    ctx_call(c, fn);
}
#define GCALL(c, fn, ...) do { const uint32_t _a[] = {__VA_ARGS__}; gcalln(c, fn, (int)(sizeof _a / sizeof *_a), _a); } while (0)

/* ShadowManager::draw(u32 view, TList<ShadowModel*> &list): when shadows are on (+0x5f), the GX
 * setup for shadow volumes, then per model in the list (link +8 = model): its view setup (vtable
 * +0x20); visible ones (+0x8c low bits, +0x95) draw their shapes twice, first with blend 3 and
 * one cull mode, then blend 1 and the other (+0x5e / +0x60 pick which), with the material
 * colour's alpha = depth (+0x5c + [+0x5e]) * model alpha (+0x94) / 255 (also times the first
 * material's alpha when +0x5e is 1). Finally colour writes back on, setDstAlpha, z mode back. */
static uint64_t i_draw__13ShadowManagerFUlRQ27JGadget60TList_P11ShadowModel_Q27JGadget26TAllocator_P11ShadowModel__(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t self = (uint32_t)a0, view = (uint32_t)a1, list = (uint32_t)a2;
    if (!mld(c, M, self + 0x5f, 1)) return 0;
    uint32_t old_sp = frame_push(c, M, 0x40), sp = old_sp - 0x40;
    GCALL(c, 0x800c11b0u, 1, 6, 0);           /* GXSetZMode */
    GCALL(c, 0x800bd5d8u, 0);                 /* GXSetNumTexGens */
    GCALL(c, 0x800c0d94u, 1);                 /* GXSetNumTevStages */
    n_GXSetTevOrder(c, M, 0, 0xff, 0xff, 4, 0, 0, 0, 0);
    GCALL(c, 0x800c0658u, 0, 4);              /* GXSetTevOp */
    GCALL(c, 0x800bf040u, 1);                 /* GXSetNumChans */
    n_GXSetChanCtrl(c, M, 2, 0, 0, 0, 0, 0, 2, 0);
    GCALL(c, 0x800c0b28u, 7, 0, 0, 7, 0);     /* GXSetAlphaCompare */
    GCALL(c, 0x800c11e4u, 1);                 /* GXSetZCompLoc */
    GCALL(c, 0x800c1158u, 0);                 /* GXSetColorUpdate */
    GCALL(c, 0x800c1184u, 1);                 /* GXSetAlphaUpdate */
    GCALL(c, 0x800c1318u, 0, 0);              /* GXSetDstAlpha */
    uint32_t end = list + 8;
    for (uint32_t link = ld32(c, M, list + 8); link != end; link = ld32(c, M, link)) {
        uint32_t m = ld32(c, M, link + 8);
        GCALL(c, ld32(c, M, ld32(c, M, m) + 0x20), m, view);
        m = ld32(c, M, link + 8);
        if (!(mld(c, M, m + 0x8c, 1) & 0xf) || !mld(c, M, m + 0x95, 1)) continue;
        uint32_t kind = mld(c, M, self + 0x5e, 1);
        uint32_t data = ld32(c, M, ld32(c, M, m + 8)), mdl = ld32(c, M, ld32(c, M, m + 0xc));
        uint32_t nshapes = ld16(c, M, data + 0x7c);
        uint32_t alpha = (uint32_t)((int32_t)(mld(c, M, self + kind + 0x5c, 1) * mld(c, M, m + 0x94, 1)) / 0xff) & 0xFF;
        if (kind == 1) {
            uint32_t mat = ld32(c, M, ld32(c, M, ld32(c, M, data + 0x60)) + 0x24);
            GCALL(c, ld32(c, M, ld32(c, M, mat) + 0x38), mat, 0);
            alpha = (uint32_t)((int32_t)(alpha * mld(c, M, ctx_gpr(c, 3) + 3, 1)) / 0xff) & 0xFC;
        }
        mst(c, M, sp + 0x14, 4, alpha); /* (0, 0, 0, alpha) */
        mst(c, M, sp + 0x10, 4, ld32(c, M, sp + 0x14));
        GCALL(c, 0x800bef50u, 4, sp + 0x10);  /* GXSetChanMatColor */
        for (int pass = 0; pass < 2; pass++) {
            GCALL(c, 0x800c1104u, pass ? 1 : 3, 1, 1, 5); /* GXSetBlendMode */
            kind = mld(c, M, self + 0x5e, 1);
            uint32_t flip = mld(c, M, self + 0x60, 1);
            bool a_side = (kind == 1 && !flip) || (kind == 0 && flip);
            GCALL(c, 0x800be0e4u, a_side == !pass ? 2 : 1); /* GXSetCullMode */
            for (uint32_t k = 0; (k & 0xFFFF) < nshapes; k++) {
                uint32_t s = ld32(c, M, mdl + 0xc4) + (k & 0xFFFF) * 0x3c;
                GCALL(c, ld32(c, M, ld32(c, M, s) + 0xc), s);
            }
        }
    }
    GCALL(c, 0x800c1158u, 1);                 /* GXSetColorUpdate */
    GCALL(c, 0x801dabacu, self);              /* setDstAlpha */
    GCALL(c, 0x800c11b0u, 1, 3, 1);           /* GXSetZMode */
    frame_pop(c, old_sp);
    return 0;
}

/* FUN_80085f48(particle, emitter): one frame of a JPA particle. Age +0x80 (s16) counts up to its
 * life +0x82 (returns 1 when done); +0x84 = age / life; flag 0x20 (+0x7c) follows the emitter's
 * position (+0x138.. -> +0x18..); the per-frame force (+0x48..) restarts at 0 and is filled by
 * the field block (unless flag 0x40) plus the base acceleration (+0x3c..); the drag (+0x30..)
 * scales by the emitter's air resistance (resource +0x48); velocity +0x24.. = (drag + force) *
 * +0x70 * +0x74; the emitter callback (resource +0xf0, vtable +0xc); unless flag 2, the
 * rotation and child emission (+0x88 += +0x8a, FUN_80086440 -> FUN_80081130 per child); the
 * local position +0xc.. moves by the velocity and the global +0.. = local * emitter scale
 * (+0x12c..) + base. Returns 0. */
static uint64_t i_FUN_80085f48(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t p = (uint32_t)a0, em = (uint32_t)a1, r2 = (uint32_t)RD(c, REG_R2, 4);
    int16_t age = (int16_t)(ld16(c, M, p + 0x80) + 1);
    mst(c, M, p + 0x80, 2, (uint16_t)age);
    int16_t life = (int16_t)ld16(c, M, p + 0x82);
    if (age >= life) return 1;
    uint32_t old_sp = frame_push(c, M, 0x40);
    stf(c, M, p + 0x84, fdiv((float)age, (float)life));
    if (ld32(c, M, p + 0x7c) & 0x20)
        for (int k = 0; k < 3; k++) mst(c, M, p + 0x18 + 4 * k, 4, ld32(c, M, em + 0x138 + 4 * k));
    float zero = ldf(c, M, r2 - 0x77dc);
    stf(c, M, p + 0x50, zero);
    stf(c, M, p + 0x4c, zero);
    stf(c, M, p + 0x48, zero);
    if (!(ld32(c, M, p + 0x7c) & 0x40)) GCALL(c, 0x80088be0u, ld32(c, M, em + 4), em, p);
    for (int k = 0; k < 3; k++) stf(c, M, p + 0x48 + 4 * k, fadd(ldf(c, M, p + 0x48 + 4 * k), ldf(c, M, p + 0x3c + 4 * k)));
    float air = ldf(c, M, ld32(c, M, em) + 0x48);
    for (int k = 0; k < 3; k++) stf(c, M, p + 0x30 + 4 * k, mul(ldf(c, M, p + 0x30 + 4 * k), air));
    float s = mul(ldf(c, M, p + 0x70), ldf(c, M, p + 0x74));
    for (int k = 0; k < 3; k++) stf(c, M, p + 0x24 + 4 * k, mul(s, fadd(ldf(c, M, p + 0x30 + 4 * k), ldf(c, M, p + 0x48 + 4 * k))));
    uint32_t res = ld32(c, M, em), cb = ld32(c, M, res + 0xf0);
    if (cb) GCALL(c, ld32(c, M, ld32(c, M, cb) + 0xc), cb, res, p);
    uint64_t ret = 1;
    if (!(ld32(c, M, p + 0x7c) & 2)) {
        GCALL(c, 0x80088af8u, ld32(c, M, em + 4), em, p);
        mst(c, M, p + 0x88, 2, (ld16(c, M, p + 0x88) + (uint32_t)(int32_t)(int16_t)ld16(c, M, p + 0x8a)) & 0xFFFF);
        if (ld32(c, M, ld32(c, M, em + 4) + 0x24)) {
            GCALL(c, 0x80086440u, p, em);
            if (ctx_gpr(c, 3) & 0xFF)
                for (int32_t n = (int16_t)ld16(c, M, ld32(c, M, ld32(c, M, ld32(c, M, em + 4) + 0x24)) + 0x42); n > 0; n--)
                    GCALL(c, 0x80081130u, ld32(c, M, em), p);
        }
        for (int k = 0; k < 3; k++) stf(c, M, p + 0xc + 4 * k, fadd(ldf(c, M, p + 0xc + 4 * k), ldf(c, M, p + 0x24 + 4 * k)));
        for (int k = 0; k < 3; k++) stf(c, M, p + 4 * k, madd(ldf(c, M, p + 0xc + 4 * k), ldf(c, M, em + 0x12c + 4 * k), ldf(c, M, p + 0x18 + 4 * k)));
        ret = 0;
    }
    frame_pop(c, old_sp);
    return ret;
}

/* ---- batch 1: small per-frame helpers ---- */
/* GeographyObjManager::Calc_norm(it): the object's calc_norm (its calc, vtable +0x2c, then
 * FUN_801f7684); with flag 0x10 at +0xec, FUN_80183820(+0x5c), flags 1 and 2 cleared, +0xf0 = 0. */
static uint64_t i_Calc_norm__19GeographyObjManagerFRC31JSUListIterator_12GeographyObj_(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t it = (uint32_t)a1, old_sp = frame_push(c, M, 0x10);
    uint32_t o = ld32(c, M, ld32(c, M, it));
    { uint32_t sp2 = frame_push(c, M, 0x10); /* calc_norm's own frame */
      GCALL(c, ld32(c, M, ld32(c, M, o) + 0x2c), o);
      GCALL(c, 0x801f7684u, o);
      frame_pop(c, sp2); }
    o = ld32(c, M, ld32(c, M, it));
    if (ld32(c, M, o + 0xec) & 0x10) {
        n_FUN_80183820(c, M, o + 0x5c, 0, 0, 0, 0, 0, 0, 0);
        o = ld32(c, M, ld32(c, M, it));
        mst(c, M, o + 0xec, 4, ld32(c, M, o + 0xec) & ~3u);
        mst(c, M, o + 0xf0, 4, 0);
    }
    frame_pop(c, old_sp);
    return 0;
}

/* ShadowManager::viewCalc(u32 view): each visible model (+0x8c low bits) in the four lists gets
 * its viewCalc (vtable +0x1c). */
static uint64_t i_viewCalc__13ShadowManagerFUl(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t self = (uint32_t)a0, view = (uint32_t)a1, old_sp = frame_push(c, M, 0x30);
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t list = self + 0x18 + 0x10 * i, end = list + 8;
        for (uint32_t link = ld32(c, M, list + 8); link != end; link = ld32(c, M, link)) {
            uint32_t m = ld32(c, M, link + 8);
            if (mld(c, M, m + 0x8c, 1) & 0xf) GCALL(c, ld32(c, M, ld32(c, M, m) + 0x1c), m, view);
        }
    }
    frame_pop(c, old_sp);
    return 0;
}

/* J3DModel::prepareShapePackets(): each shape packet (+0xc4, 0x3c bytes, count data +0x7c) gets
 * the matrix buffer (+0x84) and its draw matrices: this +0x54 for model flag type 2, else j3dSys. */
static uint64_t i_prepareShapePackets__8J3DModelFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t self = (uint32_t)a0, n = ld16(c, M, ld32(c, M, self + 4) + 0x7c);
    for (uint32_t k = 0; k < n; k++) {
        uint32_t pkt = ld32(c, M, self + 0xc4) + k * 0x3c;
        mst(c, M, pkt + 0x2c, 4, ld32(c, M, self + 0x84));
        mst(c, M, pkt + 0x30, 4, (ld32(c, M, self + 8) & 3) == 2 ? self + 0x54 : J3DSYS);
    }
    return 0;
}

/* J3DJointTree::calc(J3DMtxBuffer *buf, const Vec &scale, const Mtx m): the matrix calc's init
 * (+0x14, vtable +0x24 with scale and m), the current buffer (r13-0x6678) and, with a root joint
 * (+0x10), the calc (r13-0x6670) and recursiveCalc from the root. */
static uint64_t i_calc__12J3DJointTreeFP12J3DMtxBufferRC3VecRA3_A4_Cf(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t self = (uint32_t)a0, buf = (uint32_t)a1, old_sp = frame_push(c, M, 0x10);
    uint32_t r13 = (uint32_t)RD(c, REG_R13, 4), mc = ld32(c, M, self + 0x14);
    GCALL(c, ld32(c, M, ld32(c, M, mc) + 0x24), mc, (uint32_t)a2, (uint32_t)a3);
    mst(c, M, r13 - 0x6678, 4, buf);
    uint32_t root = ld32(c, M, self + 0x10);
    if (root) {
        mst(c, M, r13 - 0x6670, 4, ld32(c, M, self + 0x14));
        n_recursiveCalc__8J3DJointFv(c, M, root, 0, 0, 0, 0, 0, 0, 0);
    }
    frame_pop(c, old_sp);
    return 0;
}

/* ---- verifier ---- */
int g_native_verify;
#define NATIVE_INFO(name, args, ret, guest) {#name, args, ret, guest},
static const struct { const char *name, *args; char ret; int flag; } s_info[] = {NATIVE_LIST(NATIVE_INFO)};
#undef NATIVE_INFO
static uint64_t s_calls[NATIVE_COUNT], s_checked[NATIVE_COUNT], s_close[NATIVE_COUNT], s_bad[NATIVE_COUNT];
static unsigned s_logged[NATIVE_COUNT];
static bool s_in_verify;
static uint64_t s_next_ns[NATIVE_COUNT], s_gap_ns = 50000000;
static bool s_only[NATIVE_COUNT];
static uint64_t s_verify_from_ns;
static uint8_t *s_snap, *s_after, *s_regs;

void native_init(void) {
    /* NATIVE_LIST names are the guest names with every non-identifier character as '_' */
    for (unsigned k = 0; k < NAME_COUNT; k++) {
        char id[256];
        size_t n = 0;
        for (const char *p = NAMES[k].name; *p && n < sizeof id - 1; p++)
            id[n++] = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_' ? *p : '_';
        id[n] = 0;
        for (int i = 0; i < NATIVE_COUNT; i++)
            if (!s_addr[i] && !strcmp(id, s_info[i].name)) s_addr[i] = NAMES[k].addr;
    }
    for (int i = 0; i < NATIVE_COUNT; i++)
        if (!s_addr[i]) rt_log("native: no guest function %s", s_info[i].name);
    s_gt_full = s_addr[NAT_getTransform__19J3DAnmTransformFullCFUsP16J3DTransformInfo];
    const char *e = plat_getenv("MKDD_NATIVE_VERIFY");
    g_native_verify = e ? atoi(e) : 0;
    if (g_native_verify <= 0) { g_native_verify = 0; return; }
    const char *ms = plat_getenv("MKDD_NATIVE_VERIFY_MS");
    if (ms) s_gap_ns = (uint64_t)atoi(ms) * 1000000u;
    const char *only = plat_getenv("MKDD_NATIVE_VERIFY_ONLY"), *after = plat_getenv("MKDD_NATIVE_VERIFY_AFTER");
    for (int i = 0; i < NATIVE_COUNT; i++) {
        s_only[i] = !only;
        for (const char *p = only; p && *p; ) {
            const char *q = strchr(p, ',');
            size_t n = q ? (size_t)(q - p) : strlen(p);
            char key[128];
            snprintf(key, sizeof key, "%.*s", (int)n, p);
            if (n && strstr(s_info[i].name, key)) s_only[i] = true;
            p = q ? q + 1 : p + n;
        }
    }
    if (after) s_verify_from_ns = plat_now_ns() + (uint64_t)atoi(after) * 1000000000ull;
    s_snap = malloc(RAM_SIZE);
    s_after = malloc(RAM_SIZE);
    s_regs = malloc(REG_BYTES);
    if (!s_snap || !s_after || !s_regs) { g_native_verify = 0; return; }
    rt_log("native: verifying every %d. call of %d functions", g_native_verify, NATIVE_COUNT);
}

void native_report(void) {
    if (!g_native_verify) return;
    for (int i = 0; i < NATIVE_COUNT; i++)
        if (s_checked[i])
            rt_log("native %s: %llu calls, %llu checked, %llu float-close, %llu wrong", s_info[i].name,
                   (unsigned long long)s_calls[i], (unsigned long long)s_checked[i],
                   (unsigned long long)s_close[i], (unsigned long long)s_bad[i]);
}

static bool float_close(uint32_t x, uint32_t y) {
    float a, b;
    memcpy(&a, &x, 4);
    memcpy(&b, &y, 4);
    if (!isfinite(a) || !isfinite(b)) return false;
    float m = fmaxf(fabsf(a), fabsf(b));
    return fabsf(a - b) <= 1e-5f * m || m < 1e-30f;
}

/* Sampling: each function's Nth call, but at most one check per function every
 * MKDD_NATIVE_VERIFY_MS (default 50) ms of host time, so the game still reaches the race. */
/* MKDD_NATIVE_VERIFY_ONLY=a,b: only natives whose name contains one of these;
 * MKDD_NATIVE_VERIFY_AFTER=s: no checks during the first s seconds (menus at full speed). */
static bool verify_due(int id) {
    if (++s_calls[id] % (uint64_t)g_native_verify) return false;
    if (!s_only[id]) return false;
    uint64_t now = plat_now_ns();
    if (now < s_verify_from_ns) return false;
    if (now < s_next_ns[id]) return false;
    s_next_ns[id] = now + s_gap_ns;
    return true;
}

typedef uint64_t (*Impl)(Ctx *, uint8_t *, ARGS);
/* Run the recompiled original and the native version from the same RAM and register state and
 * compare RAM (minus the stack below r1, dead after the call) and the result. */
static uint64_t verify(int id, Ctx *c, uint8_t *M, const uint64_t *a, Impl impl) {
    s_in_verify = true;
    bool irq = c->in_irq;
    c->in_irq = true; /* no interrupt delivery in between: it would be undone */
    memcpy(s_regs, c->regs, REG_BYTES);
    memcpy(s_snap, M, RAM_SIZE);
    unsigned g = 3, f = 1;
    for (const char *p = s_info[id].args; *p; p++) {
        uint64_t v = a[p - s_info[id].args];
        if (*p == 'r') ctx_set_gpr(c, g++, (uint32_t)v);
        else WR(c, 0x4000 + 8 * f++, 8, v);
    }
    uint32_t r1 = ctx_gpr(c, 1);
    bool gx = s_info[id].flag >= 3;
    static uint8_t cap[2][1u << 20]; /* whole draws: shadow volumes are tens of KB */
    size_t ncap[2] = {0, 0};
    if (gx) hw_gather_capture(cap[0], sizeof cap[0]);
    ctx_call_orig(c, s_addr[id]);
    if (gx) { ncap[0] = hw_gather_captured(); hw_gather_capture(cap[1], sizeof cap[1]); }
    uint64_t want = s_info[id].ret == 'r' ? ctx_gpr(c, 3) : s_info[id].ret == 'f' ? RD(c, 0x4008, 8) : 0;
    memcpy(s_after, M, RAM_SIZE);
    memcpy(M, s_snap, RAM_SIZE);
    memcpy(c->regs, s_regs, REG_BYTES);
    uint64_t got = impl(c, M, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
    if (gx) {
        ncap[1] = hw_gather_captured();
        hw_gather_capture(NULL, 0);
        if (ncap[0] != ncap[1] || memcmp(cap[0], cap[1], ncap[0] < sizeof cap[0] ? ncap[0] : sizeof cap[0])) {
            s_bad[id]++;
            if (s_logged[id]++ < 40) rt_log("native %s: FIFO original %u bytes, native %u bytes, differ", s_info[id].name, (unsigned)ncap[0], (unsigned)ncap[1]);
        }
        hw_gather_copy(cap[1], ncap[1] < sizeof cap[1] ? ncap[1] : sizeof cap[1]);
    }
    c->in_irq = irq;
    s_in_verify = false;

    uint32_t lo = (r1 & 0x01FFFFFFu) > 0x10000 ? (r1 & 0x01FFFFFFu) - 0x10000 : 0, hi = (r1 & 0x01FFFFFFu) + 8;
    unsigned bad = 0, close = 0;
    for (uint32_t o = 0; o < RAM_SIZE; o += 8) {
        if (!memcmp(M + o, s_after + o, 8)) continue;
        for (uint32_t w = o; w < o + 8; w += 4) {
            if (w >= lo && w < hi) continue;
            uint32_t x = (uint32_t)get_be(s_after + w, 4), y = (uint32_t)get_be(M + w, 4);
            if (x == y) continue;
            if (float_close(x, y)) { close++; continue; }
            if (bad++ < 4 && s_logged[id] < 40) {
                s_logged[id]++;
                rt_log("native %s(%#x,%#x,%#x): %#010x original %#010x native %#010x", s_info[id].name,
                       (uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2], 0x80000000u | w, x, y);
            }
        }
    }
    if (want != got) {
        double dw, dg;
        memcpy(&dw, &want, 8);
        memcpy(&dg, &got, 8);
        if (s_info[id].ret == 'f' && fabs(dw - dg) <= 1e-5 * fmax(fabs(dw), fabs(dg))) close++;
        else {
            bad++;
            if (s_logged[id]++ < 40) rt_log("native %s: result original %#llx native %#llx", s_info[id].name, (unsigned long long)want, (unsigned long long)got);
        }
    }
    s_checked[id]++;
    if (close) s_close[id]++;
    if (bad) s_bad[id]++;
    static uint64_t last_report;
    uint64_t now = plat_now_ns();
    if (now - last_report > 20000000000ull) { last_report = now; native_report(); }
    return got;
}

#define NATIVE_DEF(name, args, ret, guest)                                                                 \
    uint64_t n_##name(Ctx *c, uint8_t *M, ARGS) {                                                          \
        if (UNLIKELY(g_native_verify) && !s_in_verify && verify_due(NAT_##name)) {                                    \
            const uint64_t a[8] = {a0, a1, a2, a3, a4, a5, a6, a7};                                                \
            return verify(NAT_##name, c, M, a, i_##name);                                                  \
        }                                                                                                  \
        return i_##name(c, M, a0, a1, a2, a3, a4, a5, a6, a7);                                                     \
    }
NATIVE_LIST(NATIVE_DEF)
#undef NATIVE_DEF
