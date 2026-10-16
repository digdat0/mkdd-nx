/* Out-of-line parts of the gc_core.h ABI (C port of gc-core's lib.rs) and the dispatch map
 * (C port of mkdd-recomp's lib.rs). */
#include <stdlib.h>
#include "gc_core.h"
#include "table.h"
#include "rt.h"

uint64_t g_fn_entries, g_calls, g_block_misses, g_unknown_targets, g_syscalls;
uint32_t g_last_fn;
volatile uint32_t g_sleep_lr; /* caller of the last OSSleepThread (profiling) */
volatile bool g_attention;
bool g_count_hits;
uint64_t g_hooked[TEXT_WORDS / 64];
static uint32_t *s_hits;

void core_count_hit(uint32_t addr) {
    if (!s_hits) s_hits = calloc(TEXT_WORDS, sizeof *s_hits);
    uint32_t i = (addr - TEXT_BASE) / 4;
    if (s_hits && i < TEXT_WORDS) s_hits[i]++;
}
const uint32_t *core_hit_counters(void) { return s_hits; }

void core_install_hooks(const uint32_t *addrs, unsigned n) {
    for (unsigned k = 0; k < n; k++) {
        if (HOOKABLE_COUNT) {
            /* the generator only compiled the HLE check into HOOKABLE functions */
            unsigned lo = 0, hi = HOOKABLE_COUNT;
            while (lo < hi) { unsigned m = (lo + hi) / 2; if (HOOKABLE[m] < addrs[k]) lo = m + 1; else hi = m; }
            if (lo == HOOKABLE_COUNT || HOOKABLE[lo] != addrs[k]) rt_log("hook at %#010x: function compiled without the HLE check", addrs[k]);
        }
        uint32_t i = (addrs[k] - TEXT_BASE) / 4;
        if (i < TEXT_WORDS) g_hooked[i / 64] |= 1ull << (i % 64);
    }
}

/* A function entered at an address that isn't one of its block leaders (a computed jump the
 * generator didn't know about). Logged; the call returns without running code. */
void ctx_block_miss(Ctx *c, uint32_t func, uint32_t pc) {
    (void)c;
    g_block_misses++;
    rt_log("mid-block entry %#010x in function %#010x (not a block leader)", pc, func);
}

/* Call through the dispatch map (indirect calls, tail jumps, calls into unknown code). */
/* ctx_enter for a function with an HLE handler (the original order: bypass first). */
bool ctx_enter_hooked(Ctx *c, uint32_t addr, uint32_t entry) {
    g_fn_entries++;
    if (UNLIKELY(g_count_hits)) core_count_hit(addr);
    /* Consume the bypass before interrupt delivery can run other guest code. */
    bool bypass = c->bypass == addr;
    if (bypass) c->bypass = 0;
    if (UNLIKELY(g_attention) && !c->in_irq && (RD(c, REG_MSR, 4) & MSR_EE)) { hw_gx_flush(); rt_deliver(c); }
    if (bypass || entry != addr) { hw_gx_flush(); return false; }
    return rt_hle(c, addr); /* flushes the gather pipe itself, except for the pure hooks */
}

/* Recently dispatched targets (virtual calls hit a few hundred addresses per frame): one cache
 * line instead of the 1.5 MB slot table plus FUNCS. Only the token holder dispatches, so the
 * guest threads sharing it never race. ctx_call checks it before going through rt_dispatch. */
#define DCACHE 2048
static struct { uint32_t addr; GuestFn fn; } s_dcache[DCACHE];

void ctx_call(Ctx *c, uint64_t target) {
    g_calls++;
    unsigned h = ((uint32_t)target >> 2) & (DCACHE - 1);
    if (LIKELY(s_dcache[h].addr == (uint32_t)target && s_dcache[h].fn && c->depth <= 20000)) {
        c->depth++;
        s_dcache[h].fn(c, (uint32_t)target);
        c->depth--;
        return;
    }
    if (c->depth > 20000) {
        rt_log("call depth limit hit at %#010x", (uint32_t)target);
        return;
    }
    c->depth++;
    bool found = rt_dispatch(c, (uint32_t)target);
    c->depth--;
    if (!found) {
        g_unknown_targets++;
        rt_log("unknown call target %#010x", (uint32_t)target);
    }
}

/* From an HLE handler: run the original recompiled function at addr. */
void ctx_call_orig(Ctx *c, uint32_t addr) {
    c->bypass = addr;
    ctx_call(c, addr);
    c->bypass = 0;
}

/* Call a guest function with up to 8 integer arguments; returns r3. */
uint32_t ctx_call_guest(Ctx *c, uint32_t addr, const uint32_t *args, unsigned n) {
    for (unsigned i = 0; i < n; i++) ctx_set_gpr(c, 3 + i, args[i]);
    ctx_call(c, addr);
    return ctx_gpr(c, 3);
}

/* CALLOTHER intrinsics. Cache-block zeroing is real; the rest go to the runtime. */
uint64_t ctx_intrinsic(Ctx *c, const char *name, const uint64_t *args, unsigned n) {
    if (!strcmp(name, "dataCacheBlockClearToZero") || !strcmp(name, "dataCacheBlockSetToZeroLocked")) {
        uint64_t a = (n ? args[0] : 0) & ~31ull;
        for (int i = 0; i < 4; i++) ctx_st(c, a + 8 * i, 8, 0);
    } else {
        rt_intrinsic(c, name, args, n);
    }
    return 0;
}

/* mtspr DMAL with the trigger bit: copy between RAM and the locked cache. */
void ctx_lc_dma(Ctx *c) {
    uint32_t u = (uint32_t)RD(c, REG_DMAU, 4), l = (uint32_t)RD(c, REG_DMAL, 4);
    if (!(l & 2)) return;
    uint32_t blocks = ((u & 0x1F) << 2) | ((l >> 2) & 3);
    if (!blocks) blocks = 128;
    uint64_t mem = u & ~0x1Fu, lc = l & ~0x1Fu;
    for (uint64_t i = 0; i < (uint64_t)blocks * 4; i++) {
        if (l & 0x10) ctx_st(c, lc + 8 * i, 8, ctx_ld(c, mem + 8 * i, 8));
        else ctx_st(c, mem + 8 * i, 8, ctx_ld(c, lc + 8 * i, 8));
    }
    WR(c, REG_DMAL, 4, l & ~2u);
}

/* ---- dispatch map: for every code word, 1 + index of the innermost function containing it ---- */
static uint16_t *s_slots;
static uint32_t s_base, s_nslots;

void core_build_dispatch(void) {
    s_base = FUNCS[0].start & ~3u;
    uint32_t end = s_base;
    for (unsigned i = 0; i < FUNC_COUNT; i++)
        if (FUNCS[i].end > end) end = FUNCS[i].end;
    s_nslots = (end - s_base) / 4 + 1;
    s_slots = calloc(s_nslots, sizeof *s_slots);
    /* Ranges can nest (small orphan pseudo-functions inside a function around switch tables).
     * Filling in start order lets an inner range override its enclosing function only where
     * it lies, so a computed jump past such an orphan still resolves to the outer function. */
    for (unsigned i = 0; i < FUNC_COUNT; i++)
        for (uint32_t a = FUNCS[i].start; a < FUNCS[i].end; a += 4) s_slots[(a - s_base) / 4] = (uint16_t)(i + 1);
}

bool core_dispatch(Ctx *c, uint32_t addr) {
    unsigned h = (addr >> 2) & (DCACHE - 1);
    if (LIKELY(s_dcache[h].addr == addr && s_dcache[h].fn)) { s_dcache[h].fn(c, addr); return true; }
    uint32_t slot = (addr - s_base) / 4;
    if (slot >= s_nslots || (addr & 3)) return false;
    uint16_t i = s_slots[slot];
    if (!i) return false;
    GuestFn fn = FUNCS[i - 1].fn;
    s_dcache[h].addr = addr;
    s_dcache[h].fn = fn;
    fn(c, addr);
    return true;
}

const char *core_function_name(uint32_t addr) {
    unsigned lo = 0, hi = NAME_COUNT;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (NAMES[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    return lo < NAME_COUNT && NAMES[lo].addr == addr ? NAMES[lo].name : NULL;
}

uint32_t core_function_addr(const char *name) {
    for (unsigned i = 0; i < NAME_COUNT; i++)
        if (!strcmp(NAMES[i].name, name)) return NAMES[i].addr;
    return 0;
}

static bool abi_seen(uint64_t key) {
    static uint64_t seen[4096];
    unsigned h = (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 52);
    for (unsigned i = 0; i < 4096; i++, h = (h + 1) & 4095) {
        if (seen[h] == key + 1) return true;
        if (!seen[h]) { seen[h] = key + 1; return false; }
    }
    return true;
}

void abi_after(Ctx *c, uint32_t site, uint64_t mask) {
    for (unsigned i = 0; mask && i < ABI_CAND_COUNT; i++, mask >>= 1) {
        if (!(mask & 1)) continue;
        unsigned s = ABI_CAND_SIZE[i], o = ABI_CAND_OFF[i];
        if (RD(c, o, s) != ABI_POISON(s) || abi_seen((uint64_t)site << 8 | i)) continue;
        rt_log("abi-check: call at %#010x in %s: register %#x read after the call was left unstored by a callee",
               site, m_name(site), o);
    }
}

void abi_ind(Ctx *c, uint32_t target, uint32_t site) {
    (void)c;
    unsigned lo = 0, hi = ABI_SUSPECT_COUNT;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (ABI_SUSPECT[mid] < target) lo = mid + 1;
        else hi = mid;
    }
    if (lo < ABI_SUSPECT_COUNT && ABI_SUSPECT[lo] == target && !abi_seen(1ull << 40 | target))
        rt_log("abi-check: indirect call at %#010x in %s to %#010x %s, which reads non-argument registers at entry",
               site, m_name(site), target, m_name(target));
}

/* gen_c.py --check-mod: a callee changed a register outside its computed write set (MOD). */
void mod_report(uint32_t callee, uint32_t site, uint32_t off) {
    static uint32_t seen[256];
    static unsigned n;
    for (unsigned i = 0; i < n; i++) if (seen[i] == callee) return;
    if (n < 256) seen[n++] = callee;
    rt_log("mod: callee %#010x %s changed register offset %#x (call at %#010x in %s)", callee, m_name(callee),
           off, site, m_name(site));
}
void lazy_report(uint32_t callee, uint32_t site, int reg) {
    static uint32_t seen[256];
    static unsigned n;
    for (unsigned i = 0; i < n; i++) if (seen[i] == callee) return;
    if (n < 256) seen[n++] = callee;
    rt_log("lazy: callee %#010x %s changed %s%d (call at %#010x in %s)", callee, m_name(callee),
           reg >= 100 ? "f" : "r", reg % 100, site, m_name(site));
}
