/* The ABI between generated code (gen/part_NN.c, from tools/gen_c.py) and the console runtime.
 *
 * C port of MKDD-Recomp's gc-core crate (rust/crates/gc-core): the CPU context, p-code operator
 * semantics, the inline MEM1 fast path and the entry hook every generated function calls.
 * Semantics follow the Rust version exactly, including Rust's saturating float->int casts
 * (C leaves out-of-range and NaN conversions undefined, so they are spelled out here).
 * Build generated code with -ffp-contract=off: fused multiply-add would change results. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
/* Switch (PIE, one statically linked image): runtime symbols are local to the image, so code
 * addresses them PC-relative instead of loading every global's address from the GOT. */
#if defined(__aarch64__) && !defined(_WIN32)
#pragma GCC visibility push(hidden)
#define GC_CORE_VIS_PUSHED
#endif

#define GAME_RAM_SIZE 0x01800000u /* 24 MiB of MEM1: what the game is told it has */
/* MEM1 as the port reads it: the game's 24 MiB plus 8 MiB above (0x81800000..0x81FFFFFF) that the
 * game never uses, where the port keeps data the game is pointed at (charpack.c: character packs) */
#define RAM_SIZE 0x02000000u
#define RAM_ALLOC 0x02000000u /* allocation behind it (see mld_s) */
#define REG_BYTES 0x10010    /* Ghidra's Gekko register space, incl. PS1 */
#define REG_R2 0x08
#define REG_R13 0x34
#define REG_LR 0x1020
#define REG_MSR 0x700
#define REG_GQR0 0x1E40
#define REG_DMAU 0x1E68
#define REG_DMAL 0x1E6C
#define MSR_EE 0x8000ull
#define TEXT_BASE 0x80000000u
#define TEXT_WORDS (0x00400000u / 4)

#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
/* The mod whose code is running (machine.c): 0 = none, n = MODS[n - 1]; its functions hand over to
 * their fp<n>_ versions (tools/gen_mod.py). */
extern unsigned g_mod;
#define ALWAYS_INLINE static inline __attribute__((always_inline))

/* One guest CPU thread. Guest code runs on exactly one host thread at a time (the runtime's
 * scheduler token), so nothing here is locked. */
typedef struct Ctx {
    uint8_t regs[REG_BYTES];
    uint8_t *ram;      /* MEM1, owned by the runtime */
    uint32_t thread;   /* guest OSThread pointer (or a scheduler marker below 0x80000000) */
    bool in_irq;
    uint32_t depth;
    uint32_t bypass;   /* set by ctx_call_orig: the next entry to this address skips its HLE handler */
} Ctx;

/* Runtime services (implemented in runtime/). */
uint64_t rt_mmio_read(Ctx *c, uint32_t addr, unsigned size);
void rt_mmio_write(Ctx *c, uint32_t addr, unsigned size, uint64_t v);
/* Generated code reaches those through register-preserving trampolines (mmio_tramp.c): a plain
 * call on every access's slow path made the compiler keep the guest registers in stack slots. */
ALWAYS_INLINE uint64_t mmio_rd(Ctx *c, uint32_t a, unsigned size) {
#if defined(__aarch64__) && !defined(MKDD_NO_TRAMP)
    register uint64_t x0 __asm__("x0") = (uint64_t)(uintptr_t)c;
    register uint64_t x1 __asm__("x1") = a;
    register uint64_t x2 __asm__("x2") = size;
    __asm__ volatile("bl rt_mmio_read_tramp" : "+r"(x0) : "r"(x1), "r"(x2) : "x16", "x17", "x30", "cc", "memory");
    return x0;
#elif defined(__x86_64__) && defined(_WIN32) && !defined(MKDD_NO_TRAMP)
    uint64_t r;
    register uint64_t r8 __asm__("r8") = size;
    __asm__ volatile("call rt_mmio_read_tramp" : "=a"(r) : "c"(c), "d"((uint64_t)a), "r"(r8) : "cc", "memory");
    return r;
#else
    return rt_mmio_read(c, a, size);
#endif
}
ALWAYS_INLINE void mmio_wr(Ctx *c, uint32_t a, unsigned size, uint64_t v) {
#if defined(__aarch64__) && !defined(MKDD_NO_TRAMP)
    register uint64_t x0 __asm__("x0") = (uint64_t)(uintptr_t)c;
    register uint64_t x1 __asm__("x1") = a;
    register uint64_t x2 __asm__("x2") = size;
    register uint64_t x3 __asm__("x3") = v;
    __asm__ volatile("bl rt_mmio_write_tramp" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "x16", "x17", "x30", "cc", "memory");
#elif defined(__x86_64__) && defined(_WIN32) && !defined(MKDD_NO_TRAMP)
    uint64_t r;
    register uint64_t r8 __asm__("r8") = size;
    register uint64_t r9 __asm__("r9") = v;
    __asm__ volatile("call rt_mmio_write_tramp" : "=a"(r) : "c"(c), "d"((uint64_t)a), "r"(r8), "r"(r9) : "cc", "memory");
    (void)r;
#else
    rt_mmio_write(c, a, size, v);
#endif
}
/* Gather pipe (hw.c): stage_len = completed bytes in g_gx_stage, gather_len = bytes of the burst
 * being assembled behind them, pending = completed bursts whose PI write-pointer advance is owed.
 * mmio_tramp.c depends on this layout. */
typedef struct { size_t stage_len; unsigned gather_len, pending; } GxGatherPipe;
#if !defined(_WIN32)
#define RT_HIDDEN __attribute__((visibility("hidden")))
#else
#define RT_HIDDEN
#endif
extern RT_HIDDEN GxGatherPipe g_gx_gp;
extern RT_HIDDEN uint8_t g_gx_stage[];
extern RT_HIDDEN uint8_t *g_gx_cap;
void rt_deliver(Ctx *c);
bool rt_hle(Ctx *c, uint32_t addr);
bool rt_dispatch(Ctx *c, uint32_t addr);
void rt_intrinsic(Ctx *c, const char *name, const uint64_t *args, unsigned n);
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
uint64_t rt_timebase(void);

/* Statistics, bumped without locking (one guest thread runs at a time). */
extern uint64_t g_fn_entries, g_calls, g_block_misses, g_unknown_targets, g_syscalls;
extern uint32_t g_last_fn;
/* Set by the runtime (any host thread) when an interrupt or alarm waits to be delivered. */
extern volatile bool g_attention;
extern uint64_t g_hooked[TEXT_WORDS / 64];
void core_install_hooks(const uint32_t *addrs, unsigned n);

ALWAYS_INLINE uint64_t mask(uint64_t v, unsigned s) { return s >= 8 ? v : v & ((1ull << (8 * s)) - 1); }
ALWAYS_INLINE int64_t sext(uint64_t v, unsigned s) {
    if (s >= 8) return (int64_t)v;
    unsigned sh = 64 - 8 * s;
    return (int64_t)(v << sh) >> sh;
}

ALWAYS_INLINE uint64_t get_be(const uint8_t *p, unsigned size) {
    switch (size) {
    case 1: return p[0];
    case 2: { uint16_t v; memcpy(&v, p, 2); return __builtin_bswap16(v); }
    case 4: { uint32_t v; memcpy(&v, p, 4); return __builtin_bswap32(v); }
    case 8: { uint64_t v; memcpy(&v, p, 8); return __builtin_bswap64(v); }
    default: { uint64_t v = 0; for (unsigned i = 0; i < size; i++) v = (v << 8) | p[i]; return v; }
    }
}
ALWAYS_INLINE void put_be(uint8_t *p, unsigned size, uint64_t v) {
    switch (size) {
    case 1: p[0] = (uint8_t)v; break;
    case 2: { uint16_t x = __builtin_bswap16((uint16_t)v); memcpy(p, &x, 2); break; }
    case 4: { uint32_t x = __builtin_bswap32((uint32_t)v); memcpy(p, &x, 4); break; }
    case 8: { uint64_t x = __builtin_bswap64(v); memcpy(p, &x, 8); break; }
    default: for (unsigned i = 0; i < size; i++) p[i] = (uint8_t)(v >> (8 * (size - 1 - i)));
    }
}

/* GameCube effective/physical address -> MEM1 offset, or -1. */
ALWAYS_INLINE int64_t ram_off(uint32_t addr) {
    uint32_t phys = addr & 0x3FFFFFFFu;
    return phys < RAM_SIZE ? (int64_t)phys : -1;
}

/* Register space. */
/* The register file is host-endian: every register is always accessed at its own offset and size
 * (generated code and runtime alike), so no byte swaps are needed. */
ALWAYS_INLINE uint64_t RD(const Ctx *c, unsigned off, unsigned s) {
    const uint8_t *p = c->regs + off;
    switch (s) {
    case 1: return p[0];
    case 2: { uint16_t v; memcpy(&v, p, 2); return v; }
    case 4: { uint32_t v; memcpy(&v, p, 4); return v; }
    case 8: { uint64_t v; memcpy(&v, p, 8); return v; }
    default: return get_be(p, s);
    }
}
ALWAYS_INLINE void WR(Ctx *c, unsigned off, unsigned s, uint64_t v) {
    uint8_t *p = c->regs + off;
    switch (s) {
    case 1: p[0] = (uint8_t)v; break;
    case 2: { uint16_t x = (uint16_t)v; memcpy(p, &x, 2); break; }
    case 4: { uint32_t x = (uint32_t)v; memcpy(p, &x, 4); break; }
    case 8: memcpy(p, &v, 8); break;
    default: put_be(p, s, v);
    }
}
ALWAYS_INLINE uint32_t ctx_gpr(const Ctx *c, unsigned n) { return (uint32_t)RD(c, 4 * n, 4); }
ALWAYS_INLINE void ctx_set_gpr(Ctx *c, unsigned n, uint32_t v) { WR(c, 4 * n, 4, v); }

/* mftb: guest time base backed by the host clock. */
ALWAYS_INLINE uint64_t ctx_tb_lo(Ctx *c) { (void)c; return rt_timebase() & 0xFFFFFFFFull; }
ALWAYS_INLINE uint64_t ctx_tb_hi(Ctx *c) { (void)c; return rt_timebase() >> 32; }

ALWAYS_INLINE uint64_t ctx_ld(Ctx *c, uint64_t addr, unsigned size) {
    uint32_t a = (uint32_t)addr, phys = a & 0x3FFFFFFFu;
    if (LIKELY(phys + size <= RAM_SIZE)) return get_be(c->ram + phys, size);
    return mmio_rd(c, a, size);
}
ALWAYS_INLINE void ctx_st(Ctx *c, uint64_t addr, unsigned size, uint64_t v) {
    uint32_t a = (uint32_t)addr, phys = a & 0x3FFFFFFFu;
    if (LIKELY(phys + size <= RAM_SIZE)) put_be(c->ram + phys, size, v);
    else mmio_wr(c, a, size, v);
}

/* Same, with MEM1 passed in: generated functions keep c->ram in a local (M) so guest stores
 * through it don't force the compiler to reload the pointer. */
/* RAM test: physical address below 32 MiB (MEM1 plus the allocation's unused tail), a single
 * bit test. Hardware registers (0x0C......), the locked cache (0x20......) etc. take the slow
 * path. The allocation has slack past RAM_ALLOC for accesses straddling its end. */
#define IS_RAM(a) (((a) & 0x3E000000u) == 0)
ALWAYS_INLINE uint64_t mld(Ctx *c, uint8_t *M, uint64_t addr, unsigned size) {
    uint32_t a = (uint32_t)addr;
    if (LIKELY(IS_RAM(a))) return get_be(M + (a & 0x01FFFFFFu), size);
    return mmio_rd(c, a, size);
}
/* A store the compiler can see goes to the gather pipe (lis rX,0xCC01 / stw -0x8000(rX)): append
 * in line while the burst stays incomplete; the store that completes it (1 in 8 for words) takes
 * the trampoline, which writes it and commits the burst. */
ALWAYS_INLINE void gp_wr(Ctx *c, unsigned size, uint64_t v) {
    size_t s = g_gx_gp.stage_len;
    unsigned g = g_gx_gp.gather_len;
    if (LIKELY(!g_gx_cap && g + size < 32)) {
        put_be(g_gx_stage + s + g, size, v);
        g_gx_gp.gather_len = g + size;
        return;
    }
    mmio_wr(c, 0xCC008000u, size, v);
}
ALWAYS_INLINE void mst(Ctx *c, uint8_t *M, uint64_t addr, unsigned size, uint64_t v) {
    uint32_t a = (uint32_t)addr;
    if (__builtin_constant_p(a) && (a & 0xFFFFFFF8u) == 0xCC008000u) { gp_wr(c, size, v); return; }
    if (LIKELY(IS_RAM(a))) put_be(M + (a & 0x01FFFFFFu), size, v);
    else mmio_wr(c, a, size, v);
}

/* Accesses based on r1 / r2 / r13 (stack, small-data areas): always MEM1, so no MMIO check;
 * the 25-bit mask keeps them inside the 32 MiB allocation whatever the value. */
ALWAYS_INLINE uint64_t mld_s(uint8_t *M, uint64_t addr, unsigned size) { return get_be(M + ((uint32_t)addr & 0x01FFFFFFu), size); }
ALWAYS_INLINE void mst_s(uint8_t *M, uint64_t addr, unsigned size, uint64_t v) { put_be(M + ((uint32_t)addr & 0x01FFFFFFu), size, v); }

ALWAYS_INLINE bool core_hooked(uint32_t addr) {
    uint32_t i = (addr - TEXT_BASE) / 4;
    return i < TEXT_WORDS && (g_hooked[i / 64] >> (i % 64)) & 1;
}

/* Top of every generated function: bookkeeping, interrupt delivery and the HLE check.
 * True if an HLE handler fully handled the call. */
bool ctx_enter_hooked(Ctx *c, uint32_t addr, uint32_t entry);
ALWAYS_INLINE bool ctx_enter(Ctx *c, uint32_t addr, uint32_t entry) {
    g_last_fn = addr;
    /* HLE'd functions (and their bypass) take the out-of-line path. */
    if (UNLIKELY(core_hooked(addr))) return ctx_enter_hooked(c, addr, entry);
    if (UNLIKELY(g_attention) && !c->in_irq && (RD(c, REG_MSR, 4) & MSR_EE)) rt_deliver(c);
    return false;
}

/* Same for functions that are never hooked (the generator knows them). */
ALWAYS_INLINE bool ctx_enter_nh(Ctx *c, uint32_t addr, uint32_t entry) {
    (void)entry;
    g_last_fn = addr;
    if (UNLIKELY(g_attention) && !c->in_irq && (RD(c, REG_MSR, 4) & MSR_EE)) rt_deliver(c);
    return false;
}

void ctx_block_miss(Ctx *c, uint32_t func, uint32_t pc);
void ctx_call(Ctx *c, uint64_t target);
void ctx_call_orig(Ctx *c, uint32_t addr);
uint32_t ctx_call_guest(Ctx *c, uint32_t addr, const uint32_t *args, unsigned n);
uint64_t ctx_intrinsic(Ctx *c, const char *name, const uint64_t *args, unsigned n);
void ctx_lc_dma(Ctx *c);
void ctx_ps(Ctx *c, uint32_t w); /* paired singles, runtime/ps.c */
/* gen_c.py --check-lazy: callee-saved registers (r14-r31, f14-f31) must come back unchanged. */
ALWAYS_INLINE void lazy_snap(const Ctx *c, uint64_t *sv) {
    for (int i = 0; i < 18; i++) { sv[i] = RD(c, 4 * (14 + i), 4); sv[18 + i] = RD(c, 0x4000 + 8 * (14 + i), 8); }
}
void lazy_report(uint32_t callee, uint32_t site, int reg);
void mod_report(uint32_t callee, uint32_t site, uint32_t off);
/* gen_c.py --abi-check: returns write this into the volatile registers they skip storing; after a
 * call, abi_after() reports any the caller reads (mask: ABI_CAND bits) still holding it, and
 * abi_ind() reports indirect calls into functions that read more than EABI arguments. */
#define ABI_POISON(s) (0x7FF4DEAD7FF4DEADull & ((s) >= 8 ? ~0ull : (1ull << (8 * (s))) - 1))
void abi_after(Ctx *c, uint32_t site, uint64_t mask);
void abi_ind(Ctx *c, uint32_t target, uint32_t site);
ALWAYS_INLINE void lazy_check(const Ctx *c, const uint64_t *sv, uint32_t callee, uint32_t site) {
    for (int i = 0; i < 18; i++) {
        if (sv[i] != RD(c, 4 * (14 + i), 4)) lazy_report(callee, site, 14 + i);
        if (sv[18 + i] != RD(c, 0x4000 + 8 * (14 + i), 8)) lazy_report(callee, site, 100 + 14 + i);
    }
}

/* ---- p-code operators: values in uint64_t, s = operand size in bytes ---- */
#define B(x) ((uint64_t)!!(x))
ALWAYS_INLINE uint64_t int_add(uint64_t a, uint64_t b, unsigned s) { return mask(a + b, s); }
ALWAYS_INLINE uint64_t int_sub(uint64_t a, uint64_t b, unsigned s) { return mask(a - b, s); }
ALWAYS_INLINE uint64_t int_mult(uint64_t a, uint64_t b, unsigned s) { return mask(a * b, s); }
ALWAYS_INLINE uint64_t int_div(uint64_t a, uint64_t b, unsigned s) { return b == 0 ? 0 : mask(mask(a, s) / mask(b, s), s); }
ALWAYS_INLINE uint64_t int_rem(uint64_t a, uint64_t b, unsigned s) { return b == 0 ? 0 : mask(mask(a, s) % mask(b, s), s); }
ALWAYS_INLINE int64_t wrap_div(int64_t a, int64_t d) { return (a == INT64_MIN && d == -1) ? a : a / d; }
ALWAYS_INLINE int64_t wrap_rem(int64_t a, int64_t d) { return (a == INT64_MIN && d == -1) ? 0 : a % d; }
ALWAYS_INLINE uint64_t int_sdiv(uint64_t a, uint64_t b, unsigned s) { int64_t d = sext(b, s); return d == 0 ? 0 : mask((uint64_t)wrap_div(sext(a, s), d), s); }
ALWAYS_INLINE uint64_t int_srem(uint64_t a, uint64_t b, unsigned s) { int64_t d = sext(b, s); return d == 0 ? 0 : mask((uint64_t)wrap_rem(sext(a, s), d), s); }
ALWAYS_INLINE uint64_t int_and(uint64_t a, uint64_t b, unsigned s) { (void)s; return a & b; }
ALWAYS_INLINE uint64_t int_or(uint64_t a, uint64_t b, unsigned s) { (void)s; return a | b; }
ALWAYS_INLINE uint64_t int_xor(uint64_t a, uint64_t b, unsigned s) { (void)s; return a ^ b; }
ALWAYS_INLINE uint64_t int_negate(uint64_t a, unsigned s) { return mask(~a, s); }
ALWAYS_INLINE uint64_t int_2comp(uint64_t a, unsigned s) { return mask(0 - a, s); }
ALWAYS_INLINE uint64_t int_left(uint64_t a, uint64_t b, unsigned s) { return b >= 64 ? 0 : mask(a << b, s); }
ALWAYS_INLINE uint64_t int_right(uint64_t a, uint64_t b, unsigned s) { return b >= 64 ? 0 : mask(a, s) >> b; }
ALWAYS_INLINE uint64_t int_sright(uint64_t a, uint64_t b, unsigned s) { return mask((uint64_t)(sext(a, s) >> (b > 63 ? 63 : b)), s); }
ALWAYS_INLINE uint64_t int_equal(uint64_t a, uint64_t b, unsigned s) { (void)s; return B(a == b); }
ALWAYS_INLINE uint64_t int_notequal(uint64_t a, uint64_t b, unsigned s) { (void)s; return B(a != b); }
ALWAYS_INLINE uint64_t int_less(uint64_t a, uint64_t b, unsigned s) { return B(mask(a, s) < mask(b, s)); }
ALWAYS_INLINE uint64_t int_lessequal(uint64_t a, uint64_t b, unsigned s) { return B(mask(a, s) <= mask(b, s)); }
ALWAYS_INLINE uint64_t int_sless(uint64_t a, uint64_t b, unsigned s) { return B(sext(a, s) < sext(b, s)); }
ALWAYS_INLINE uint64_t int_slessequal(uint64_t a, uint64_t b, unsigned s) { return B(sext(a, s) <= sext(b, s)); }
ALWAYS_INLINE uint64_t int_carry(uint64_t a, uint64_t b, unsigned s) { return B(mask(a, s) > mask(~b, s)); }
ALWAYS_INLINE uint64_t int_scarry(uint64_t a, uint64_t b, unsigned s) { uint64_t r = int_add(a, b, s); return ((a ^ r) & (b ^ r)) >> (8 * s - 1) & 1; }
ALWAYS_INLINE uint64_t int_sborrow(uint64_t a, uint64_t b, unsigned s) { uint64_t r = int_sub(a, b, s); return ((a ^ b) & (a ^ r)) >> (8 * s - 1) & 1; }
ALWAYS_INLINE uint64_t int_zext(uint64_t a, unsigned s) { return mask(a, s); }
ALWAYS_INLINE uint64_t int_sext(uint64_t a, unsigned s) { return (uint64_t)sext(a, s); }
ALWAYS_INLINE uint64_t bool_negate(uint64_t a, unsigned s) { (void)s; return B((a & 1) == 0); }
ALWAYS_INLINE uint64_t bool_and(uint64_t a, uint64_t b, unsigned s) { (void)s; return a & b & 1; }
ALWAYS_INLINE uint64_t bool_or(uint64_t a, uint64_t b, unsigned s) { (void)s; return (a | b) & 1; }
ALWAYS_INLINE uint64_t bool_xor(uint64_t a, uint64_t b, unsigned s) { (void)s; return (a ^ b) & 1; }
ALWAYS_INLINE uint64_t popcount(uint64_t a, unsigned s) { return (uint64_t)__builtin_popcountll(mask(a, s)); }
ALWAYS_INLINE uint64_t lzcount(uint64_t a, unsigned s) {
    uint64_t m = mask(a, s);
    uint64_t lz = m ? (uint64_t)__builtin_clzll(m) : 64;
    uint64_t pad = 64 - 8 * (uint64_t)s;
    return lz > pad ? lz - pad : 0;
}
/* SUBPIECE: drop c low bytes, truncate to the output size. */
ALWAYS_INLINE uint64_t subpiece(uint64_t a, uint64_t c, unsigned out) { return mask(c >= 8 ? 0 : a >> (8 * c), out); }
/* PIECE: hi concatenated above lo (lo has size ls). */
ALWAYS_INLINE uint64_t piece(uint64_t hi, uint64_t lo, unsigned ls) { return ls >= 8 ? lo : (hi << (8 * ls)) | mask(lo, ls); }

/* Floating point: size 4 => f32 bits, size 8 => f64 bits; arithmetic in f64 like the Rust code. */
ALWAYS_INLINE double tof(uint64_t a, unsigned s) {
    if (s == 4) { uint32_t u = (uint32_t)a; float f; memcpy(&f, &u, 4); return (double)f; }
    double d; memcpy(&d, &a, 8); return d;
}
ALWAYS_INLINE uint64_t fromf(double v, unsigned s) {
    if (s == 4) { float f = (float)v; uint32_t u; memcpy(&u, &f, 4); return u; }
    uint64_t u; memcpy(&u, &v, 8); return u;
}
/* Rust `as` casts: saturating, NaN -> 0. */
ALWAYS_INLINE int64_t sat_i64(double v) { return v != v ? 0 : v >= 9223372036854775807.0 ? INT64_MAX : v <= -9223372036854775808.0 ? INT64_MIN : (int64_t)v; }
ALWAYS_INLINE int32_t sat_i32(double v) { return v != v ? 0 : v >= 2147483647.0 ? INT32_MAX : v <= -2147483648.0 ? INT32_MIN : (int32_t)v; }
ALWAYS_INLINE uint64_t sat_u64(double v) { return v != v || v <= 0 ? 0 : v >= 18446744073709551615.0 ? UINT64_MAX : (uint64_t)v; }

ALWAYS_INLINE uint64_t float_add(uint64_t a, uint64_t b, unsigned s) { return fromf(tof(a, s) + tof(b, s), s); }
ALWAYS_INLINE uint64_t float_sub(uint64_t a, uint64_t b, unsigned s) { return fromf(tof(a, s) - tof(b, s), s); }
ALWAYS_INLINE uint64_t float_mult(uint64_t a, uint64_t b, unsigned s) { return fromf(tof(a, s) * tof(b, s), s); }
ALWAYS_INLINE uint64_t float_div(uint64_t a, uint64_t b, unsigned s) { return fromf(tof(a, s) / tof(b, s), s); }
ALWAYS_INLINE uint64_t float_neg(uint64_t a, unsigned s) { return fromf(-tof(a, s), s); }
ALWAYS_INLINE uint64_t float_abs(uint64_t a, unsigned s) { return fromf(__builtin_fabs(tof(a, s)), s); }
ALWAYS_INLINE uint64_t float_sqrt(uint64_t a, unsigned s) { return fromf(__builtin_sqrt(tof(a, s)), s); }
ALWAYS_INLINE uint64_t float_ceil(uint64_t a, unsigned s) { return fromf(__builtin_ceil(tof(a, s)), s); }
ALWAYS_INLINE uint64_t float_floor(uint64_t a, unsigned s) { return fromf(__builtin_floor(tof(a, s)), s); }
ALWAYS_INLINE uint64_t float_round(uint64_t a, unsigned s) { return fromf(__builtin_round(tof(a, s)), s); }
ALWAYS_INLINE uint64_t float_nan(uint64_t a, unsigned s) { double v = tof(a, s); return B(v != v); }
ALWAYS_INLINE uint64_t float_equal(uint64_t a, uint64_t b, unsigned s) { return B(tof(a, s) == tof(b, s)); }
ALWAYS_INLINE uint64_t float_notequal(uint64_t a, uint64_t b, unsigned s) { return B(tof(a, s) != tof(b, s)); }
ALWAYS_INLINE uint64_t float_less(uint64_t a, uint64_t b, unsigned s) { return B(tof(a, s) < tof(b, s)); }
ALWAYS_INLINE uint64_t float_lessequal(uint64_t a, uint64_t b, unsigned s) { return B(tof(a, s) <= tof(b, s)); }
/* FLOAT2FLOAT: input size s -> output size out. */
ALWAYS_INLINE uint64_t float2float(uint64_t a, unsigned s, unsigned out) { return fromf(tof(a, s), out); }
ALWAYS_INLINE uint64_t int2float(uint64_t a, unsigned s, unsigned out) { return fromf((double)sext(a, s), out); }
ALWAYS_INLINE uint64_t trunc_(uint64_t a, unsigned s, unsigned out) {
    double v = tof(a, s);
    int64_t i = out <= 4 ? (int64_t)sat_i32(v) : sat_i64(v);
    return mask((uint64_t)i, out);
}
/* GameCubeLoader ldexpf intrinsic: the paired-single dequantisation scale 2^n. */
ALWAYS_INLINE uint64_t ldexp_pow2(uint64_t a, unsigned s, unsigned out) {
    return fromf(__builtin_powi(2.0, (int)sext(a, s)), out);
}

/* ---- paired singles compiled inline by gen_c.py (same semantics as runtime/ps.c) ---- */
ALWAYS_INLINE double ps_d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
ALWAYS_INLINE uint64_t ps_u(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
ALWAYS_INLINE uint64_t ps_cmp(double a, double b) {
    if (a != a || b != b) return 1;
    return a < b ? 8 : a > b ? 4 : 2;
}
/* 2^n exactly, n in -32..31 */
ALWAYS_INLINE double ps_pow2(int n) { return ps_d((uint64_t)(1023 + n) << 52); }
ALWAYS_INLINE int ps_sext6(uint32_t v) { return (int32_t)(v << 26) >> 26; }
ALWAYS_INLINE double ps_clampd(double x, double lo, double hi) { return x != x ? x : x < lo ? lo : x > hi ? hi : x; }

ALWAYS_INLINE double ps_ldq(Ctx *c, uint8_t *M, uint32_t ea, uint32_t ty, int scale, uint32_t *size) {
    switch (ty) {
    case 4: *size = 1; return (double)(uint8_t)mld(c, M, ea, 1) * ps_pow2(-scale);
    case 5: *size = 2; return (double)(uint16_t)mld(c, M, ea, 2) * ps_pow2(-scale);
    case 6: *size = 1; return (double)(int8_t)(uint8_t)mld(c, M, ea, 1) * ps_pow2(-scale);
    case 7: *size = 2; return (double)(int16_t)(uint16_t)mld(c, M, ea, 2) * ps_pow2(-scale);
    default: { *size = 4; uint32_t u = (uint32_t)mld(c, M, ea, 4); float f; memcpy(&f, &u, 4); return (double)f; }
    }
}
ALWAYS_INLINE uint32_t ps_stq(Ctx *c, uint8_t *M, uint32_t ea, double v, uint32_t ty, int scale) {
    double x = v * ps_pow2(scale);
    switch (ty) {
    case 4: mst(c, M, ea, 1, sat_u64(ps_clampd(x, 0.0, 255.0))); return 1;
    case 5: mst(c, M, ea, 2, sat_u64(ps_clampd(x, 0.0, 65535.0))); return 2;
    case 6: mst(c, M, ea, 1, (uint64_t)sat_i64(ps_clampd(x, -128.0, 127.0)) & 0xFF); return 1;
    case 7: mst(c, M, ea, 2, (uint64_t)sat_i64(ps_clampd(x, -32768.0, 32767.0)) & 0xFFFF); return 2;
    default: { float f = (float)v; uint32_t u; memcpy(&u, &f, 4); mst(c, M, ea, 4, u); return 4; }
    }
}
/* psq_l: GQR load type (bits 16-18) and scale (24-29); w = one value, ps1 = 1.0 */
ALWAYS_INLINE double ps_f32(uint32_t u) { float f; memcpy(&f, &u, 4); return (double)f; }
ALWAYS_INLINE uint32_t ps_u32(double v) { float f = (float)v; uint32_t u; memcpy(&u, &f, 4); return u; }
ALWAYS_INLINE void ps_load(Ctx *c, uint8_t *M, uint32_t ea, uint32_t g, int w, double *p0, double *p1) {
    /* Fast path: float pairs (GQR type 0, what GQR0 always holds) in RAM, one 8-byte access. */
    if (LIKELY((g & 0x00070000u) == 0 && IS_RAM(ea))) {
        uint8_t *q = M + (ea & 0x01FFFFFFu);
        if (w) { *p0 = ps_f32((uint32_t)get_be(q, 4)); *p1 = 1.0; return; }
        uint64_t v = get_be(q, 8);
        *p0 = ps_f32((uint32_t)(v >> 32));
        *p1 = ps_f32((uint32_t)v);
        return;
    }
    uint32_t ty = (g >> 16) & 7, size, s2;
    int scale = ps_sext6((g >> 24) & 0x3F);
    *p0 = ps_ldq(c, M, ea, ty, scale, &size);
    *p1 = w ? 1.0 : ps_ldq(c, M, ea + size, ty, scale, &s2);
}
/* psq_st: GQR store type (bits 0-2) and scale (8-13) */
ALWAYS_INLINE void ps_store(Ctx *c, uint8_t *M, uint32_t ea, uint32_t g, int w, double a, double b) {
    if (LIKELY((g & 7u) == 0 && IS_RAM(ea))) {
        uint8_t *q = M + (ea & 0x01FFFFFFu);
        if (w) put_be(q, 4, ps_u32(a));
        else put_be(q, 8, (uint64_t)ps_u32(a) << 32 | ps_u32(b));
        return;
    }
    uint32_t ty = g & 7;
    int scale = ps_sext6((g >> 8) & 0x3F);
    uint32_t size = ps_stq(c, M, ea, a, ty, scale);
    if (!w) ps_stq(c, M, ea + size, b, ty, scale);
}

#ifdef GC_CORE_VIS_PUSHED
#pragma GCC visibility pop
#endif
