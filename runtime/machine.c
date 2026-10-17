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

/* ---- main.dol ---- */
/* SHA-1 (FIPS 180-1), for checking the disc's main.dol. */
static uint32_t rol32(uint32_t x, int n) { return x << n | x >> (32 - n); }
static void sha1_block(uint32_t h[5], const uint8_t *p) {
    uint32_t w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                   k = 0xCA62C1D6u; }
        uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol32(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}
static void sha1_hex(const uint8_t *data, size_t n, char out[41]) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    size_t i = 0;
    for (; i + 64 <= n; i += 64) sha1_block(h, data + i);
    uint8_t tail[128] = {0};
    size_t r = n - i, tl = r < 56 ? 64 : 128;
    memcpy(tail, data + i, r);
    tail[r] = 0x80;
    uint64_t bits = (uint64_t)n * 8;
    for (int j = 0; j < 8; j++) tail[tl - 1 - j] = (uint8_t)(bits >> (8 * j));
    for (size_t j = 0; j < tl; j += 64) sha1_block(h, tail + j);
    for (int j = 0; j < 5; j++) snprintf(out + 8 * j, 9, "%08x", (unsigned)h[j]);
}

/* Load the game's code and data from main.dol on the disc (it is not part of the build): find it
 * through the disc header, check it is the exact main.dol the code was generated from, and copy
 * every section the generator listed to its address. */
static bool load_dol(const char *disc_path, char *err, size_t errlen) {
    uint32_t dol_off = disc_read_u32(0x420);
    uint8_t hdr[0x100];
    disc_read(dol_off, hdr, sizeof hdr);
    uint32_t off[18], addr[18], size[18], dol_size = sizeof hdr;
    for (int i = 0; i < 18; i++) {
        off[i]  = (uint32_t)hdr[4 * i] << 24 | hdr[4 * i + 1] << 16 | hdr[4 * i + 2] << 8 | hdr[4 * i + 3];
        addr[i] = (uint32_t)hdr[0x48 + 4 * i] << 24 | hdr[0x49 + 4 * i] << 16 | hdr[0x4A + 4 * i] << 8 | hdr[0x4B + 4 * i];
        size[i] = (uint32_t)hdr[0x90 + 4 * i] << 24 | hdr[0x91 + 4 * i] << 16 | hdr[0x92 + 4 * i] << 8 | hdr[0x93 + 4 * i];
        if (size[i] && off[i] + size[i] > dol_size) dol_size = off[i] + size[i];
    }
    if (dol_size > (16u << 20)) {
        snprintf(err, errlen, "%s: main.dol header is damaged", disc_path);
        return false;
    }
    uint8_t *dol = malloc(dol_size);
    if (!dol) { snprintf(err, errlen, "out of memory"); return false; }
    disc_read(dol_off, dol, dol_size);
    char sha[41];
    sha1_hex(dol, dol_size, sha);
    if (strcmp(sha, DOL_SHA1) != 0) {
        free(dol);
        rt_log("boot: main.dol SHA-1 %s, expected %s", sha, DOL_SHA1);
        snprintf(err, errlen, "%s: main.dol is not the supported version (USA GM4E01, SHA-1 %.8s, found %.8s). "
                 "Use a clean dump of the disc", disc_path, DOL_SHA1, sha);
        return false;
    }
    for (unsigned s = 0; s < SECTION_COUNT; s++) {
        int i = 0;
        while (i < 18 && !(size[i] && addr[i] == SECTIONS[s].addr && size[i] >= SECTIONS[s].size)) i++;
        if (i == 18) { /* cannot happen with a matching SHA-1 unless the generator's sheets are off */
            free(dol);
            snprintf(err, errlen, "main.dol has no section at %#010x (%u bytes): regenerate the C", SECTIONS[s].addr,
                     SECTIONS[s].size);
            return false;
        }
        m_load(SECTIONS[s].addr, dol + off[i], SECTIONS[s].size);
    }
    free(dol);
    rt_log("boot: main.dol loaded from the disc (%u bytes, %u sections, SHA-1 ok)", dol_size, SECTION_COUNT);
    return true;
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
    if (!load_dol(disc_path, err, errlen)) return false;

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
