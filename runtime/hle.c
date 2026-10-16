/* Function-level high-level emulation (C port of gc-runtime's hle.rs).
 *
 * Every generated function checks core_hooked() on entry; functions listed in sheets/hle.csv
 * are routed to rt_hle(). Handlers are bound once at boot by the function's name, can run the
 * original recompiled body with ctx_call_orig, and set r3 as the return value. Returning false
 * falls through to the original code.
 *
 * The Rust version's stream / ARQ / heap diagnostics (they only log, then run the original)
 * are left out: their hooks fall through to the original code here. */
#include <stdlib.h>
#include <string.h>
#include "gx.h"
#include "rt.h"
#include "native.h"
#include "table.h"

uint64_t g_dvd_reads;

enum {
    H_NONE,
    H_DISABLE, H_ENABLE, H_RESTORE, H_MTMSR,
    H_MTX_COPY, H_MTX_CONCAT, H_MTX_MULTVEC, H_VEC_CROSS, H_VEC_MAG, H_MTX_INVERSE,
    H_CREATE_THREAD, H_RESUME, H_SUSPEND, H_SLEEP, H_WAKEUP, H_YIELD, H_RESCHEDULE, H_EXIT, H_CANCEL,
    H_SELECT_THREAD,
    H_SET_ALARM, H_SET_PERIODIC_ALARM, H_CANCEL_ALARM,
    H_DVD_INIT, H_DVD_READ_ABS, H_DVD_READ_DISK_ID, H_DVD_INQUIRY, H_DVD_CANCEL_ASYNC, H_DVD_ZERO, H_DVD_CHECK,
    H_DVD_BLOCK_STATUS,
    H_PAD_ONE, H_PAD_NOP, H_PAD_ZERO, H_PAD_READ, H_SI_PROBE,
    H_GX_FIFO_LINK,
    H_AUDIO_NOP, H_DSP_BOOT, H_DSP_SEND, H_DSP_RUNNING_CHECK,
    H_HID2,
    H_THP_DECODE, H_OS_REPORT, H_OS_PANIC, H_RESET_IGNORED, H_PPC_HALT,
    H_SYS_FETCH, H_SYS_STORE, H_CARD, H_GX_DRAW_DONE, H_SET_PRIORITY,
};

static const struct { const char *name; int id; } BIND[] = {
    {"OSDisableInterrupts", H_DISABLE}, {"OSEnableInterrupts", H_ENABLE}, {"OSRestoreInterrupts", H_RESTORE},
    {"PPCMtmsr", H_MTMSR},
    {"PSMTXCopy", H_MTX_COPY}, {"PSMTXConcat", H_MTX_CONCAT}, {"PSMTXMultVec", H_MTX_MULTVEC},
    {"PSVECCrossProduct", H_VEC_CROSS}, {"PSVECMag", H_VEC_MAG}, {"PSMTXInverse", H_MTX_INVERSE},
    {"OSCreateThread", H_CREATE_THREAD}, {"OSResumeThread", H_RESUME}, {"OSSuspendThread", H_SUSPEND},
    {"OSSleepThread", H_SLEEP}, {"OSWakeupThread", H_WAKEUP}, {"OSYieldThread", H_YIELD},
    {"__OSReschedule", H_RESCHEDULE}, {"OSExitThread", H_EXIT}, {"OSCancelThread", H_CANCEL},
    {"SetEffectivePriority", H_SET_PRIORITY},
    {"SelectThread", H_SELECT_THREAD}, {"OSLoadContext", H_SELECT_THREAD},
    {"OSSetAlarm", H_SET_ALARM}, {"OSSetPeriodicAlarm", H_SET_PERIODIC_ALARM}, {"OSCancelAlarm", H_CANCEL_ALARM},
    {"DVDInit", H_DVD_INIT}, {"DVDReadAbsAsyncPrio", H_DVD_READ_ABS}, {"DVDReadDiskID", H_DVD_READ_DISK_ID},
    {"DVDInquiryAsync", H_DVD_INQUIRY}, {"DVDCancelAsync", H_DVD_CANCEL_ASYNC},
    {"DVDCancel", H_DVD_ZERO}, {"DVDGetDriveStatus", H_DVD_ZERO}, {"DVDReset", H_DVD_ZERO}, {"DVDResume", H_DVD_ZERO},
    {"DVDCheckDisk", H_DVD_CHECK}, {"DVDGetCommandBlockStatus", H_DVD_BLOCK_STATUS},
    {"PADInit", H_PAD_ONE}, {"PADReset", H_PAD_ONE}, {"PADRecalibrate", H_PAD_ONE},
    {"PADControlMotor", H_PAD_NOP}, {"PADSetSamplingCallback", H_PAD_ZERO}, {"PADRead", H_PAD_READ},
    {"SIProbe", H_SI_PROBE}, {"SIGetType", H_SI_PROBE},
    {"__GXFifoLink", H_GX_FIFO_LINK},
    {"__AI_SRC_INIT", H_AUDIO_NOP}, {"__OSInitAudioSystem", H_AUDIO_NOP}, {"__OSStopAudioSystem", H_AUDIO_NOP},
    {"DspBoot__FPFPv_v", H_DSP_BOOT}, {"DSPSendCommands2__FPUlUlPFUs_v", H_DSP_SEND},
    {"Dsp_Running_Check__Fv", H_DSP_RUNNING_CHECK},
    {"PPCMfhid2", H_HID2}, {"PPCMthid2", H_HID2}, {"__OSFPRInit", H_HID2}, {"__LCEnable", H_HID2},
    {"LCDisable", H_HID2}, {"LCQueueWait", H_HID2}, {"__OSLoadFPUContext", H_HID2}, {"__OSSaveFPUContext", H_HID2},
    {"OSFillFPUContext", H_HID2}, {"FUN_80110a50", H_HID2}, {"FUN_80110c08", H_HID2},
    {"THPVideoDecode", H_THP_DECODE}, {"OSReport", H_OS_REPORT}, {"OSPanic", H_OS_PANIC},
    {"OSResetSystem", H_RESET_IGNORED}, {"__OSDoHotReset", H_RESET_IGNORED}, {"PPCHalt", H_PPC_HALT},
    {"fetch__10SystemFileFv", H_SYS_FETCH}, {"store__10SystemFileFv", H_SYS_STORE},
    {"GXDrawDone", H_GX_DRAW_DONE},
};

typedef struct { uint32_t addr; int id; const char *name; } Hook;
static Hook *s_hooks;
static unsigned s_nhooks;
extern volatile uint32_t g_sleep_lr;
static volatile uint64_t s_hid2; /* the CPU's HID2 (bit 32 set once some thread has published it) */

static int cmp_hook(const void *a, const void *b) {
    uint32_t x = ((const Hook *)a)->addr, y = ((const Hook *)b)->addr;
    return x < y ? -1 : x > y;
}

void hle_init(void) {
    s_hooks = calloc(HLE_COUNT, sizeof *s_hooks);
    for (unsigned i = 0; i < HLE_COUNT; i++) {
        Hook *h = &s_hooks[s_nhooks++];
        h->addr = HLE[i].addr;
        h->name = HLE[i].name;
        h->id = H_NONE;
        for (size_t k = 0; k < sizeof BIND / sizeof *BIND; k++)
            if (!strcmp(BIND[k].name, HLE[i].name)) { h->id = BIND[k].id; break; }
        if (h->id == H_NONE && !strncmp(HLE[i].name, "CARD", 4)) h->id = H_CARD;
    }
    /* Handlers added here that hle.csv does not list (ids from H_GX_DRAW_DONE on): hook by name. */
    size_t nbind = sizeof BIND / sizeof *BIND;
    s_hooks = realloc(s_hooks, (HLE_COUNT + nbind) * sizeof *s_hooks);
    for (size_t k = 0; k < nbind; k++) {
        bool listed = false;
        for (unsigned i = 0; i < HLE_COUNT && !listed; i++) listed = !strcmp(HLE[i].name, BIND[k].name);
        if (listed || BIND[k].id < H_GX_DRAW_DONE) continue; /* only the handlers added for the port */
        uint32_t a = m_addr_of(BIND[k].name);
        if (!a) continue;
        s_hooks[s_nhooks++] = (Hook){a, BIND[k].id, BIND[k].name};
        core_install_hooks(&a, 1);
    }
    qsort(s_hooks, s_nhooks, sizeof *s_hooks, cmp_hook);
}

static const Hook *find_hook(uint32_t addr) {
    unsigned lo = 0, hi = s_nhooks;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (s_hooks[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    return lo < s_nhooks && s_hooks[lo].addr == addr ? &s_hooks[lo] : NULL;
}

/* 64-bit argument in a register pair (PPC EABI: rN = high word, rN+1 = low word). */
static uint64_t arg64(Ctx *c, unsigned n) { return (uint64_t)ctx_gpr(c, n) << 32 | ctx_gpr(c, n + 1); }

static void dvd_read(uint64_t off, uint32_t dst, uint32_t len) {
    int64_t o = ram_off(dst);
    if (o < 0) return;
    if (len > RAM_SIZE - (uint64_t)o) len = (uint32_t)(RAM_SIZE - (uint64_t)o);
    disc_read(off, g_ram + o, len);
}

static void read_cstr(uint32_t addr, char *out, size_t n) {
    size_t i = 0;
    for (; i + 1 < n && i < 1024; i++) {
        char ch = (char)m_read_be(addr + (uint32_t)i, 1);
        if (!ch) break;
        out[i] = ch;
    }
    out[i] = 0;
}

/* Minimal printf for OSReport/OSPanic: integer, char and string conversions. */
static void format_guest(Ctx *c, uint32_t fmt_addr, unsigned arg, char *out, size_t n) {
    char fmt[1024];
    read_cstr(fmt_addr, fmt, sizeof fmt);
    size_t k = 0;
#define NEXT() (arg <= 10 ? ctx_gpr(c, arg++) : (arg++, 0u))
    for (const char *p = fmt; *p && k + 1 < n; p++) {
        if (*p != '%') { out[k++] = *p; continue; }
        char spec[16];
        size_t s = 0;
        while (p[1] && s < sizeof spec - 1) {
            char ch = *++p;
            spec[s++] = ch;
            if ((((ch | 32) >= 'a' && (ch | 32) <= 'z') && ch != 'l' && ch != 'h') || ch == '%') break;
        }
        spec[s] = 0;
        char last = s ? spec[s - 1] : 0, tmp[1100];
        switch (last) {
        case 'd': case 'i': snprintf(tmp, sizeof tmp, "%d", (int32_t)NEXT()); break;
        case 'u': snprintf(tmp, sizeof tmp, "%u", NEXT()); break;
        case 'x': case 'p': snprintf(tmp, sizeof tmp, "%x", NEXT()); break;
        case 'X': snprintf(tmp, sizeof tmp, "%X", NEXT()); break;
        case 'c': snprintf(tmp, sizeof tmp, "%c", (char)NEXT()); break;
        case 's': read_cstr(NEXT(), tmp, sizeof tmp); break;
        case '%': snprintf(tmp, sizeof tmp, "%%"); break;
        default: snprintf(tmp, sizeof tmp, "%%%s", spec);
        }
        for (const char *t = tmp; *t && k + 1 < n; t++) out[k++] = *t;
    }
#undef NEXT
    while (k && (out[k - 1] == '\n' || out[k - 1] == ' ')) k--;
    out[k] = 0;
}

static void call_cb(Ctx *c, uint32_t cb, uint32_t a, uint32_t b) {
    uint32_t args[2] = {a, b};
    ctx_call_guest(c, cb, args, 2);
}

bool rt_hle(Ctx *c, uint32_t addr) {
    const Hook *h = find_hook(addr);
    if (!h) { hw_gx_flush(); return false; }
    /* Pure hooks (MSR.EE, matrix maths) run thousands of times a frame and never wait on the GPU;
     * everything else may block, switch threads or read GX state, so the decoder must first have
     * every burst written so far. */
    switch (h->id) {
    case H_DISABLE: case H_ENABLE: case H_RESTORE: case H_MTMSR:
    case H_MTX_COPY: case H_MTX_CONCAT: case H_MTX_MULTVEC: case H_VEC_CROSS: case H_VEC_MAG: case H_MTX_INVERSE:
        break;
    default:
        hw_gx_flush();
    }
    switch (h->id) {
    /* ---- MSR.EE: Ghidra's mtmsr p-code keeps the old EE bit, so the recompiled versions of these
     * never change the interrupt-enable state. ---- */
    case H_DISABLE: ctx_set_gpr(c, 3, (uint32_t)n_OSDisableInterrupts(c, c->ram, 0, 0, 0, 0, 0, 0, 0, 0)); break;
    case H_ENABLE: ctx_set_gpr(c, 3, (uint32_t)n_OSEnableInterrupts(c, c->ram, 0, 0, 0, 0, 0, 0, 0, 0)); break;
    case H_RESTORE: ctx_set_gpr(c, 3, (uint32_t)n_OSRestoreInterrupts(c, c->ram, ctx_gpr(c, 3), 0, 0, 0, 0, 0, 0, 0)); break;
    case H_MTMSR: WR(c, REG_MSR, 4, ctx_gpr(c, 3)); break;
    case H_MTX_COPY: psmtx_call(c, MTX_COPY); break;
    case H_MTX_CONCAT: psmtx_call(c, MTX_CONCAT); break;
    case H_MTX_MULTVEC: psmtx_call(c, MTX_MULTVEC); break;
    case H_VEC_CROSS: psmtx_call(c, VEC_CROSS); break;
    case H_VEC_MAG: psmtx_call(c, VEC_MAG); break;
    case H_MTX_INVERSE: psmtx_call(c, MTX_INVERSE); break;
    case H_GX_DRAW_DONE: {
        /* GXDrawDone = GXSetDrawDone + wait for the GPU to get there. Here the "GPU" includes
         * the CPU-side vertex decoder, so waiting serialises it with the game; like the real
         * GP, let it trail behind: enqueue the marker, don't wait (MKDD_SYNC_DRAWDONE=1 waits). */
        static int sync = -1;
        if (sync < 0) { const char *e = plat_getenv("MKDD_SYNC_DRAWDONE"); sync = e && *e == '1'; }
        if (sync) return false;
        static uint32_t set_addr;
        if (!set_addr) set_addr = m_addr_of("GXSetDrawDone");
        if (!set_addr) return false;
        ctx_call_guest(c, set_addr, NULL, 0);
        break;
    }

    /* ---- threads ---- */
    case H_CREATE_THREAD: {
        uint32_t t = ctx_gpr(c, 3);
        ctx_call_orig(c, addr); /* fills the OSThread/OSContext; returns TRUE in r3 */
        sched_create(c, t);
        break;
    }
    case H_RESUME:
        rt_log("OSResumeThread %#x from %#x", ctx_gpr(c, 3), c->thread);
        ctx_set_gpr(c, 3, (uint32_t)sched_resume(c, ctx_gpr(c, 3)));
        break;
    case H_SUSPEND:
        rt_log("OSSuspendThread %#x from %#x", ctx_gpr(c, 3), c->thread);
        ctx_set_gpr(c, 3, (uint32_t)sched_suspend(c, ctx_gpr(c, 3)));
        break;
    case H_SLEEP: g_sleep_lr = (uint32_t)RD(c, REG_LR, 4); sched_sleep(c, ctx_gpr(c, 3)); break;
    case H_WAKEUP: sched_wakeup(c, ctx_gpr(c, 3)); break;
    case H_YIELD: sched_yield_now(c); break;
    case H_RESCHEDULE: sched_reschedule(c); break;
    case H_SET_PRIORITY: ctx_set_gpr(c, 3, sched_set_priority(ctx_gpr(c, 3), (int)ctx_gpr(c, 4))); break;
    case H_EXIT:
        m_write_be(c->thread + 0x2D8, 4, ctx_gpr(c, 3));
        sched_exit(c);
        plat_park_forever();
    case H_CANCEL:
        if (ctx_gpr(c, 3) == c->thread) { sched_exit(c); plat_park_forever(); }
        sched_cancel(c, ctx_gpr(c, 3));
        break;
    case H_SELECT_THREAD: rt_log("%s reached (unexpected with HLE scheduler)", h->name); break;

    /* ---- alarms ---- */
    case H_SET_ALARM: {
        uint32_t alarm = ctx_gpr(c, 3), handler = ctx_gpr(c, 7);
        uint64_t fire = sched_timebase() + arg64(c, 5);
        m_write_be(alarm, 4, handler);
        m_write_be(alarm + 0x08, 8, fire);
        m_write_be(alarm + 0x18, 8, 0);
        sched_set_alarm(alarm, handler, fire, 0);
        break;
    }
    case H_SET_PERIODIC_ALARM: {
        uint32_t alarm = ctx_gpr(c, 3), handler = ctx_gpr(c, 9);
        uint64_t start = arg64(c, 5), period = arg64(c, 7);
        if (!period) period = 1;
        uint64_t now = sched_timebase();
        uint64_t fire = start > now ? start : start + ((now - start) / period + 1) * period;
        m_write_be(alarm, 4, handler);
        m_write_be(alarm + 0x08, 8, fire);
        m_write_be(alarm + 0x18, 8, period);
        m_write_be(alarm + 0x20, 8, start);
        sched_set_alarm(alarm, handler, fire, period);
        break;
    }
    case H_CANCEL_ALARM:
        sched_cancel_alarm(ctx_gpr(c, 3));
        m_write_be(ctx_gpr(c, 3), 4, 0);
        break;

    /* ---- DVD: reads go straight to the disc image ---- */
    case H_DVD_INIT: {
        uint32_t a = m_addr_of("__DVDFSInit");
        if (a) ctx_call(c, a);
        break;
    }
    case H_DVD_READ_ABS: {
        uint32_t block = ctx_gpr(c, 3), dst = ctx_gpr(c, 4), len = ctx_gpr(c, 5), off = ctx_gpr(c, 6), cb = ctx_gpr(c, 7);
        g_dvd_reads++;
        dvd_read(off, dst, len);
        const uint32_t fields[8][2] = {{0x08, 1}, {0x0C, 0}, {0x10, off}, {0x14, len}, {0x18, dst}, {0x1C, len}, {0x20, len}, {0x28, cb}};
        for (int i = 0; i < 8; i++) m_write_be(block + fields[i][0], 4, fields[i][1]);
        if (cb) call_cb(c, cb, len, block);
        ctx_set_gpr(c, 3, 1);
        break;
    }
    case H_DVD_READ_DISK_ID: {
        uint32_t block = ctx_gpr(c, 3), dst = ctx_gpr(c, 4), cb = ctx_gpr(c, 5);
        dvd_read(0, dst, 0x20);
        m_write_be(block + 0x0C, 4, 0);
        if (cb) call_cb(c, cb, 0x20, block);
        ctx_set_gpr(c, 3, 1);
        break;
    }
    case H_DVD_INQUIRY: {
        uint32_t block = ctx_gpr(c, 3), info = ctx_gpr(c, 4), cb = ctx_gpr(c, 5);
        for (uint32_t i = 0; i < 8; i++) m_write_be(info + 4 * i, 4, 0);
        m_write_be(block + 0x0C, 4, 0);
        if (cb) call_cb(c, cb, 0x20, block);
        ctx_set_gpr(c, 3, 1);
        break;
    }
    case H_DVD_CANCEL_ASYNC: {
        uint32_t block = ctx_gpr(c, 3), cb = ctx_gpr(c, 4);
        m_write_be(block + 0x0C, 4, 0);
        if (cb) call_cb(c, cb, 0, block);
        ctx_set_gpr(c, 3, 1);
        break;
    }
    case H_DVD_ZERO: ctx_set_gpr(c, 3, 0); break;
    case H_DVD_CHECK: ctx_set_gpr(c, 3, 1); break;
    case H_DVD_BLOCK_STATUS: ctx_set_gpr(c, 3, (uint32_t)m_read_be(ctx_gpr(c, 3) + 0x0C, 4)); break;

    /* ---- controllers ---- */
    case H_PAD_ONE: ctx_set_gpr(c, 3, 1); break;
    case H_PAD_NOP: break;
    case H_PAD_ZERO: ctx_set_gpr(c, 3, 0); break;
    case H_PAD_READ: {
        uint32_t dst = ctx_gpr(c, 3);
        PadStatus p[4];
        pad_get_all(p);
        for (uint32_t i = 0; i < 4; i++) {
            uint32_t o = dst + 12 * i;
            m_write_be(o, 2, p[i].button);
            m_write_be(o + 2, 1, (uint8_t)p[i].stick_x);
            m_write_be(o + 3, 1, (uint8_t)p[i].stick_y);
            m_write_be(o + 4, 1, (uint8_t)p[i].substick_x);
            m_write_be(o + 5, 1, (uint8_t)p[i].substick_y);
            m_write_be(o + 6, 1, p[i].trigger_l);
            m_write_be(o + 7, 1, p[i].trigger_r);
            m_write_be(o + 8, 1, p[i].analog_a);
            m_write_be(o + 9, 1, p[i].analog_b);
            m_write_be(o + 10, 1, p[i].connected ? 0 : 0xFF); /* PAD_ERR_NONE / NO_CONTROLLER */
            m_write_be(o + 11, 1, 0);
        }
        cheats_tick();
        ctx_set_gpr(c, 3, 0);
        break;
    }
    case H_SI_PROBE: {
        /* SI_GC_CONTROLLER for connected ports, SI_ERROR_NO_RESPONSE otherwise. */
        PadStatus p[4];
        pad_get_all(p);
        ctx_set_gpr(c, 3, p[ctx_gpr(c, 3) & 3].connected ? 0x09000000u : 0x08u);
        break;
    }

    /* ---- GX ---- */
    case H_GX_FIFO_LINK:
        g_gx_link = (ctx_gpr(c, 3) & 0xFF) != 0;
        ctx_call_orig(c, addr);
        break;

    /* ---- audio: skip boot handshakes; the DSP microcode is HLE'd in dsp.c ---- */
    case H_AUDIO_NOP: break;
    case H_DSP_BOOT: {
        g_dsp_callback = ctx_gpr(c, 3);
        rt_log("DSP: JAudio callback %#010x", g_dsp_callback);
        /* The boot handshake would end in Dsp_Running_Start(); without it DsyncFrame2 only
         * queues frames and never sends them. */
        uint32_t a = m_addr_of("Dsp_Running_Start__Fv");
        if (a) ctx_call(c, a);
        break;
    }
    case H_DSP_SEND: {
        uint32_t cmds = ctx_gpr(c, 3), count = ctx_gpr(c, 4), cb = ctx_gpr(c, 5);
        bool frame = dsp_command(cmds, count);
        if (cb) { uint32_t arg = 0; ctx_call_guest(c, cb, &arg, 1); }
        if (frame) sched_raise(IRQ_DSP_DSP);
        ctx_set_gpr(c, 3, 1);
        break;
    }
    /* JAudio busy-waits for the DSP task to report it's running. */
    case H_DSP_RUNNING_CHECK: ctx_set_gpr(c, 3, 1); break;

    /* ---- HID2 belongs to the CPU, not a thread, but every host thread has its own register
     * file: the functions that touch it see the shared copy on entry and publish it on exit. ---- */
    case H_HID2: {
        const unsigned REG_HID2 = 0x1E60;
        uint64_t g = __atomic_load_n(&s_hid2, __ATOMIC_SEQ_CST);
        if (g >> 32) WR(c, REG_HID2, 4, g & 0xFFFFFFFFu);
        ctx_call_orig(c, addr);
        __atomic_store_n(&s_hid2, 1ull << 32 | RD(c, REG_HID2, 4), __ATOMIC_SEQ_CST);
        break;
    }

    case H_THP_DECODE: {
        /* THPVideoDecode(file, tileY, tileU, tileV, work): decode natively; fall back to the
         * recompiled SDK decoder if the stream is unusual. */
        uint32_t file = ctx_gpr(c, 3), dst[3] = {ctx_gpr(c, 4), ctx_gpr(c, 5), ctx_gpr(c, 6)};
        if (file && thp_decode(file, dst)) { ctx_set_gpr(c, 3, 0); return true; }
        rt_log("THP: native decode declined frame at %#x; using SDK decoder", file);
        ctx_call_orig(c, addr);
        break;
    }
    case H_OS_REPORT: {
        char msg[1024];
        format_guest(c, ctx_gpr(c, 3), 4, msg, sizeof msg);
        rt_log("OSReport: %s", msg);
        break;
    }
    case H_OS_PANIC: {
        char file[256], msg[1024];
        read_cstr(ctx_gpr(c, 3), file, sizeof file);
        format_guest(c, ctx_gpr(c, 5), 6, msg, sizeof msg);
        rt_log("OSPanic %s:%u: %s", file, ctx_gpr(c, 4), msg);
        diag_flush();
        plat_park_forever();
    }
    case H_RESET_IGNORED: rt_log("%s called (ignored)", h->name); break;
    case H_PPC_HALT: rt_log("PPCHalt"); diag_flush(); plat_park_forever();

    /* ---- "Unlock all" stays out of the memory card ---- */
    case H_SYS_FETCH:
        cheats_before_save();
        ctx_call_orig(c, addr);
        cheats_after_save();
        break;
    case H_SYS_STORE:
        ctx_call_orig(c, addr);
        cheats_after_load();
        break;
    case H_CARD: {
        int32_t r;
        if (!memcard_call(c, h->name, &r)) return false;
        rt_log("%s -> %d", h->name, r);
        ctx_set_gpr(c, 3, (uint32_t)r);
        break;
    }
    default: return false;
    }
    return true;
}
