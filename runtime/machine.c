/* The console: MEM1/ARAM, the runtime hooks gc_core.h calls, and boot (C port of gc-runtime's
 * lib.rs and mkdd-recomp's lib.rs). */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "gx.h"
#include "rt.h"
#include "native.h"
#include "table.h"

uint8_t *g_ram, *g_aram;

void core_build_dispatch(void);
bool core_dispatch(Ctx *c, uint32_t addr);
const char *core_function_name(uint32_t addr);
uint32_t core_function_addr(const char *name);

void m_load(uint32_t addr, const void *bytes, size_t n) {
    int64_t o = ram_off(addr);
    if (o < 0) return;
    if (n > RAM_SIZE - (size_t)o) n = RAM_SIZE - (size_t)o;
    memcpy(g_ram + o, bytes, n);
}
const char *m_name(uint32_t addr) {
    const char *n = core_function_name(addr);
    return n ? n : "?";
}
uint32_t m_addr_of(const char *name) { return core_function_addr(name); }

void ctx_init(Ctx *c) {
    memset(c, 0, sizeof *c);
    c->ram = g_ram;
}

/* A fresh context for a new guest thread, inheriting SDA bases, MSR and GQRs. */
void ctx_spawn_like(Ctx *c, const Ctx *from, uint32_t thread) {
    ctx_init(c);
    WR(c, REG_R2, 4, RD(from, REG_R2, 4));
    WR(c, REG_R13, 4, RD(from, REG_R13, 4));
    WR(c, REG_MSR, 4, RD(from, REG_MSR, 4));
    memcpy(c->regs + REG_GQR0, from->regs + REG_GQR0, 32);
    c->thread = thread;
}

/* ---- gc_core.h runtime hooks ---- */
uint64_t rt_timebase(void) { return sched_timebase(); }

bool rt_dispatch(Ctx *c, uint32_t addr) {
    bool found = core_dispatch(c, addr);
    if (!found) {
        /* Context for bad indirect calls: caller (LR), stack, thread, this-pointer, and the
         * stack's saved-LR chain. */
        uint32_t lr = (uint32_t)RD(c, REG_LR, 4), sp = ctx_gpr(c, 1), fp = sp;
        char chain[160] = "";
        size_t n = 0;
        for (int i = 0; i < 6; i++) {
            uint32_t next = (uint32_t)m_read_be(fp, 4);
            if (next < 0x80000000u || next >= 0x81800000u || next <= fp) break;
            n += (size_t)snprintf(chain + n, sizeof chain - n, " <- %#010x", (uint32_t)m_read_be(next + 4, 4));
            if (n >= sizeof chain) break;
            fp = next;
        }
        rt_log("bad call %#010x: lr %#010x sp %#010x thread %#x r3 %#010x [r3]=%#010x chain%s", addr, lr, sp,
               c->thread, ctx_gpr(c, 3), (uint32_t)m_read_be(ctx_gpr(c, 3), 4), chain);
    }
    return found;
}

void rt_intrinsic(Ctx *c, const char *name, const uint64_t *args, unsigned n) {
    (void)c; (void)args; (void)n;
    diag_intrinsic(name);
}

/* ---- boot ---- */
bool machine_boot(const char *disc_path, Ctx *boot, char *err, size_t errlen) {
    /* 32 MiB: generated code masks stack / small-data addresses to 25 bits without a bounds
     * check (mld_s / mst_s), so the 8 MiB past MEM1 must exist. */
    g_ram = plat_aligned_alloc(4096, RAM_ALLOC + 4096); /* slack: see IS_RAM */
    memset(g_ram, 0, RAM_ALLOC + 4096);
    g_aram = plat_aligned_alloc(4096, ARAM_SIZE);
    if (!g_ram || !g_aram) { snprintf(err, errlen, "out of memory"); return false; }
    memset(g_ram, 0, RAM_SIZE);
    memset(g_aram, 0, ARAM_SIZE);
    if (!disc_open(disc_path, err, errlen)) return false;
    char id[7] = {0};
    disc_read(0, id, 6);
    if (memcmp(id, GAME_ID, 6) != 0) {
        snprintf(err, errlen, "%s is game %s, not %s (Mario Kart: Double Dash!! USA)", disc_path, id, GAME_ID);
        return false;
    }
    core_build_dispatch();
    for (unsigned i = 0; i < SECTION_COUNT; i++) m_load(SECTIONS[i].addr, SECTIONS[i].data, SECTIONS[i].size);

    uint8_t header[0x20];
    disc_read(0, header, sizeof header);
    m_load(0x80000000u, header, sizeof header);
    uint32_t fst_off = disc_read_u32(0x424), fst_size = disc_read_u32(0x428), fst_max = disc_read_u32(0x42C);
    uint32_t fst_addr = (0x81800000u - (fst_max > fst_size ? fst_max : fst_size)) & ~0x1Fu;
    uint8_t *fst = malloc(fst_size);
    disc_read(fst_off, fst, fst_size);
    m_load(fst_addr, fst, fst_size);
    free(fst);
    m_write_be(0x80000038u, 4, fst_addr);
    m_write_be(0x8000003Cu, 4, fst_max);
    uint32_t bi2_addr = (fst_addr - 0x2000) & ~0x1Fu;
    uint8_t bi2[0x2000];
    disc_read(0x440, bi2, sizeof bi2);
    m_load(bi2_addr, bi2, sizeof bi2);
    m_write_be(0x800000F4u, 4, bi2_addr);
    /* 64 KiB below BI2 is the stack for interrupt handlers run while every thread sleeps. */
    uint32_t arena_hi = bi2_addr - 0x10000;
    sched_set_idle_stack(bi2_addr - 0x20);
    static const struct { uint32_t addr, v; } low[] = {
        {0x80000020u, 0x0D15EA5Eu},   /* normal boot magic */
        {0x80000024u, 1},             /* boot version */
        {0x80000028u, RAM_SIZE},      /* physical memory size */
        {0x8000002Cu, 1},             /* console type: retail */
        {0x80000034u, 0},             /* ArenaHi (set below) */
        {0x800000CCu, 0},             /* video mode: NTSC */
        {0x800000D0u, ARAM_SIZE},
        {0x800000F0u, RAM_SIZE},      /* simulated memory size */
        {0x800000F8u, 162000000u},    /* bus clock */
        {0x800000FCu, 486000000u},    /* cpu clock */
    };
    for (size_t i = 0; i < sizeof low / sizeof *low; i++) m_write_be(low[i].addr, 4, low[i].v);
    m_write_be(0x80000034u, 4, arena_hi);

    uint32_t *hle = malloc(HLE_COUNT * sizeof *hle);
    for (unsigned i = 0; i < HLE_COUNT; i++) hle[i] = HLE[i].addr;
    core_install_hooks(hle, HLE_COUNT);
    free(hle);
    hle_init();
    native_init();
    hw_init();
    sched_init();
    gx_start_gpu();
    sched_start_idle();
    ctx_init(boot);
    boot->thread = SCHED_BOOT;
    WR(boot, REG_MSR, 4, 0x2032); /* FP, IR/DR on; external interrupts off until OSInit */
    return true;
}

void machine_run(Ctx *boot) {
    ctx_call(boot, ENTRY_POINT);
    rt_log("entry point returned");
}
