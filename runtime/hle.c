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
    H_PAD_ONE, H_PAD_NOP, H_PAD_ZERO, H_PAD_READ, H_PAD_MOTOR, H_SI_PROBE,
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
    {"PADControlMotor", H_PAD_MOTOR}, {"PADSetSamplingCallback", H_PAD_ZERO}, {"PADRead", H_PAD_READ},
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
    hw_gx_flush();
    gx_drain();
    disc_read(off, g_ram + o, len);
    gx_mem_dirty((uint32_t)o, len);
    if (len >= (256u << 10)) gx_mem_dirty_all(); /* a scene load: see gx_mem_dirty_all */
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

/* Game-flow watcher (GM4E01, small-data base r13 = 0x803D1420), checked once a frame from
 * PADRead: logs scene requests, soft resets, pause-menu choices and race phases when they
 * change, to explain where the game went when a race transition stalls. */
#define W_SCENE 0x803C9CE0u      /* SequenceApp::msScene, msNextScene at +4 */
#define W_RESET 0x803CBE80u      /* ResetAgent::msState; msForceMenu +8, msInvalid +9 */
#define W_PAUSE 0x803CB6E4u      /* PauseManager::mPauseChoice */
#define W_RACEMGR 0x803CB7E8u    /* RaceMgr::sRaceManager; +0x18 RaceDirector: phase +0xc, next +0x10 */
#define W_SEQAPP 0x803CBE20u     /* SequenceApp::mspSequenceApp: requested scenes +0x38, loaded +0x3c, state +0x44 */
static void watch_flow(uint16_t buttons) {
    static uint32_t last[11];
    static bool init;
    uint32_t v[11] = {0};
    v[0] = (uint32_t)m_read_be(W_SCENE, 4);
    v[1] = (uint32_t)m_read_be(W_SCENE + 4, 4);
    v[2] = (uint32_t)m_read_be(W_RESET, 4);
    v[3] = (uint32_t)m_read_be(W_RESET + 8, 1) << 8 | (uint32_t)m_read_be(W_RESET + 9, 1);
    v[4] = (uint32_t)m_read_be(W_PAUSE, 4);
    uint32_t mgr = (uint32_t)m_read_be(W_RACEMGR, 4);
    uint32_t dir = (mgr & 0xFE000000u) == 0x80000000u ? (uint32_t)m_read_be(mgr + 0x18, 4) : 0;
    if ((dir & 0xFE000000u) == 0x80000000u) {
        v[5] = (uint32_t)m_read_be(dir + 0xc, 4);
        v[6] = (uint32_t)m_read_be(dir + 0x10, 4);
    }
    uint32_t seq = (uint32_t)m_read_be(W_SEQAPP, 4);
    if ((seq & 0xFE000000u) == 0x80000000u) {
        v[8] = (uint32_t)m_read_be(seq + 0x44, 4);
        v[9] = (uint32_t)m_read_be(seq + 0x38, 4);
        v[10] = (uint32_t)m_read_be(seq + 0x3c, 4);
    }
    v[7] = (buttons & 0x1600) == 0x1600; /* START + X + B: the game's soft-reset combo */
    if (init && !memcmp(v, last, sizeof v)) return;
    init = true;
    memcpy(last, v, sizeof v);
    rt_log("flow: scene %d next %d, reset state %u (menu/invalid %#x), pause choice %d, race phase %d next %d, "
           "sequence state %u (scenes requested %#x, loaded %#x), buttons %#06x%s", (int)v[0], (int)v[1], v[2], v[3],
           (int)v[4], (int)v[5], (int)v[6], v[8], v[9], v[10], buttons,
           v[7] ? " (reset combo held)" : "");
}

/* Scene load watchdog: recovers instead of hanging. SequenceApp::ready marks a scene as loading
 * (+0x38) and queues loadTask on the low-priority task (System::mspLoTask); loadTask clears the
 * mark and sets the scene's bit in +0x3c when done. If the request is lost, the mark stays, ready()
 * never requests the load again and the game waits on a black screen forever (seen going to the
 * next Grand Prix course; also possible for scenes the menus preload). Any scene marked loading
 * for 5 s whose load is neither queued nor running gets its mark cleared, so the game's next
 * ready() requests it again; a message waiting in the task's queue with nobody receiving it (a
 * lost wake-up) gets the task woken. Runs whatever the logging setting. */
bool sched_waiting_on(uint32_t t, uint32_t queue); /* sched.c */
#define W_LOTASK 0x803CBD8Cu   /* System::mspLoTask (JKRTask: thread +0x2c, queue +0x30, requests +0x8c / +0x90) */
#define W_LOADTASK 0x801D985Cu /* SequenceApp::loadTask */
#define SCENE_STUCK_FRAMES 300
static void watch_scene_load(Ctx *c) {
    static unsigned stuck[10];
    static uint32_t last_seq;
    uint32_t seq = (uint32_t)m_read_be(W_SEQAPP, 4);
    if ((seq & 0xFE000000u) != 0x80000000u || seq != last_seq) {
        memset(stuck, 0, sizeof stuck);
        last_seq = seq;
        return;
    }
    uint32_t loading = (uint32_t)m_read_be(seq + 0x38, 4), loaded = (uint32_t)m_read_be(seq + 0x3c, 4);
    static int test = -1; /* MKDD_TEST_LOST_LOAD=1 (PC): on the title screen, mark the course select
                           * scene (3) as loading without a request: a lost request, as in the hang */
    if (test < 0) test = plat_getenv("MKDD_TEST_LOST_LOAD") != NULL;
    if (test && (int32_t)m_read_be(W_SCENE, 4) == 0 && !(loading & 8) && !(loaded & 8)) {
        test = 0;
        m_write_be(seq + 0x38, 4, loading | 8);
        rt_log("scene watchdog test: scene 3 marked loading without a request");
        return;
    }
    for (int s = 0; s < 10; s++) {
        uint32_t bit = 1u << s;
        if (!(loading & bit) || (loaded & bit)) { stuck[s] = 0; continue; }
        if (++stuck[s] < SCENE_STUCK_FRAMES) continue;
        stuck[s] = 0;
        uint32_t task = (uint32_t)m_read_be(W_LOTASK, 4);
        if ((task & 0xFE000000u) != 0x80000000u) continue;
        uint32_t q = task + 0x30, used = (uint32_t)m_read_be(q + 0x1c, 4), cap = (uint32_t)m_read_be(q + 0x14, 4);
        uint32_t reqs = (uint32_t)m_read_be(task + 0x8c, 4), nreq = (uint32_t)m_read_be(task + 0x90, 4), busy = 0, slot = 0;
        bool pending = false;
        for (uint32_t i = 0; i < nreq && i < 256; i++) {
            uint32_t cb = (uint32_t)m_read_be(reqs + 12 * i, 4);
            if (!cb) continue;
            busy++;
            if (cb == W_LOADTASK && (uint32_t)m_read_be(reqs + 12 * i + 4, 4) == (uint32_t)s) { pending = true; slot = reqs + 12 * i; }
        }
        /* The task thread idle in its queue (receiving) means nothing is running: a load still
         * marked pending was lost. A busy thread (a long load, another task) is left alone. */
        uint32_t th = (uint32_t)m_read_be(task + 0x2c, 4);
        bool idle = sched_waiting_on(th, q + 8);
        const char *act = !idle ? "task busy, waiting" : used ? "waking the task" : "requesting the load again";
        rt_log("scene watchdog: scene %d still loading after 5 s: task %#x thread %#x %s, queue %u/%u messages, %u requests in use%s -> %s",
               s, task, th, idle ? "idle" : "busy", used, cap, busy, pending ? " (its load among them)" : "", act);
        if (!idle) continue;
        if (used) {
            sched_wakeup(c, q + 8); /* OSMessageQueue.queueReceive: a lost wake-up */
        } else {
            if (pending) m_write_be(slot, 4, 0); /* a request slot nothing will run */
            loading &= ~bit;
            m_write_be(seq + 0x38, 4, loading);
        }
    }
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
    case H_PAD_MOTOR: pad_motor_set((int)ctx_gpr(c, 3), ctx_gpr(c, 4) == 1); break; /* PAD_MOTOR_RUMBLE */
    case H_PAD_ZERO: ctx_set_gpr(c, 3, 0); break;
    case H_PAD_READ: {
        while (g_pad_hold) plat_sleep_ns(4000000); /* paused by the front end */
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
        watch_scene_load(c);
        if (g_log_mask & LOG_GAME) watch_flow(p[0].button);
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
