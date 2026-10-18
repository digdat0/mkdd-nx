/* Guest thread scheduler, interrupts, alarms and time (C port of gc-runtime's sched.rs).
 *
 * The SDK's own scheduler ends in OSLoadContext + rfi, which a static recompilation can't
 * follow. Instead every OSThread runs on its own host thread and a single token (running)
 * decides which one executes guest code. hle.c maps OSSleepThread / OSWakeupThread /
 * OSResumeThread / ... onto this, using the SDK's priority rules (lower value = higher prio).
 *
 * Interrupts are raised as pending bits and delivered at the next function entry of the running
 * thread (when MSR.EE is set), or by the idle thread when nothing is runnable. Handlers come from
 * the OS interrupt table at 0x80003040, so the game's own handlers run. */
#include <stdlib.h>
#include <string.h>
#include "gx.h"
#include "rt.h"

#define INTERRUPT_TABLE 0x80003040u
/* OSThread field offsets / states. */
#define T_STATE 0x2C8
#define T_SUSPEND 0x2CC
#define T_PRIORITY 0x2D0
#define T_QUEUE_JOIN 0x2E8
#define T_MUTEX 0x2F0
#define GS_READY 1
#define GS_RUNNING 2
#define GS_WAITING 4
#define GS_MORIBUND 8
#define VI_PERIOD (TB_HZ * 1001 / 60000) /* 59.94 Hz */
#define GUEST_STACK (32u << 20)
/* Switch: guest threads (only one runs at a time) share core 1; core 0 renders, core 2 decodes GX. */
#ifdef __SWITCH__
#define GUEST_CORE 1
#else
#define GUEST_CORE -1
#endif

enum { TS_READY, TS_RUNNING, TS_WAITING, TS_SUSPENDED, TS_DEAD };

typedef struct {
    bool used;
    uint32_t addr;
    int prio;
    int state;
    uint32_t wait_queue;
    uint32_t wait_lr, wait_sp; /* where it went to sleep (for the trace's thread dump) */
    uint64_t seq;
    int suspend;
    bool started;
    /* OSThread structs get reused (static THP threads, heap-allocated loaders), so a host
     * thread only owns running == addr while its incarnation matches. */
    uint64_t incarnation;
} Th;

typedef struct { uint32_t ptr, handler; uint64_t fire, period; } Alarm;

#define MAX_THREADS 64
#define MAX_ALARMS 64

static PMutex s_lock;
/* s_cv wakes the idle thread; each guest thread waits on its own slot's condvar, so a handoff
 * wakes only the thread that gets the token (not every guest thread on the shared core). */
static PCond s_cv;
static PCond s_tcv[MAX_THREADS];
static uint64_t s_next_incarnation = 1;
static uint32_t s_running = SCHED_BOOT;
static Th s_threads[MAX_THREADS];
static uint64_t s_seq = 1;
static Alarm s_alarms[MAX_ALARMS];
static int s_nalarms;
static uint64_t s_next_vi = VI_PERIOD;
/* Latest small-data-area bases (r2 << 32 | r13) seen on a guest thread, for the idle context. */
static volatile uint64_t s_sda;

static volatile uint32_t s_pending_irq;
static volatile uint64_t s_ai_period, s_ai_next, s_ai_raised;
uint64_t g_ai_lat_max_us;
static volatile uint32_t s_idle_sp;
static volatile bool s_alarm_due;
volatile uint32_t g_vi_count;
static volatile uint64_t s_last_vi_tb;
static uint64_t s_start_ns;

static _Thread_local uint64_t t_incarnation; /* of the guest thread this host thread runs (0 = boot) */

/* PC benchmark: MKDD_TIMESCALE=n runs guest time n times faster, so the game runs as fast as
 * the CPU allows instead of at 60 fps (game frames per second then measure throughput). */
static uint64_t s_timescale = 1;
uint64_t sched_timebase(void) {
    uint64_t ns = (plat_now_ns() - s_start_ns) * s_timescale;
    return ns * 81 / 2000;
}

void sched_set_ai_period(uint64_t ticks) {
    uint64_t was = __atomic_exchange_n(&s_ai_period, ticks, __ATOMIC_RELAXED);
    if (ticks > 0 && was == 0) __atomic_store_n(&s_ai_next, sched_timebase() + ticks, __ATOMIC_RELAXED);
}
void sched_set_idle_stack(uint32_t sp) { s_idle_sp = sp; }

static bool pending(void) { return __atomic_load_n(&s_pending_irq, __ATOMIC_RELAXED) != 0 || s_alarm_due; }

/* The idle thread shares the game's core and has work only while no guest thread holds the
 * token: a running thread delivers interrupts itself (g_attention). Waking it otherwise took the
 * game's core and the scheduler lock hundreds of times a second (timer ticks, every interrupt).
 * Pairs with give_token's store: an event raised before the token is dropped is seen by the idle
 * thread, which the drop wakes (kick_token). */
static void wake_idle(void) {
    if (__atomic_load_n(&s_running, __ATOMIC_SEQ_CST) == SCHED_NONE) pcond_broadcast(&s_cv);
}

void sched_raise(int irq) {
    __atomic_fetch_or(&s_pending_irq, 1u << irq, __ATOMIC_SEQ_CST);
    g_attention = true;
    /* If every guest thread is asleep (e.g. waiting for this very interrupt), only the idle
     * thread can deliver it: wake it now instead of at its next timed tick. */
    wake_idle();
}

/* VI beam position as the hardware reports it: (vertical count 1..525 over a full interlaced
 * frame = two retraces, horizontal count). */
void sched_vi_beam(uint32_t *v, uint32_t *h) {
    uint32_t field = g_vi_count & 1;
    uint64_t now = sched_timebase(), last = s_last_vi_tb;
    uint64_t since = now > last ? now - last : 0;
    uint64_t within = since * 262 / VI_PERIOD;
    if (within > 261) within = 261;
    *v = field * 263 + (uint32_t)within + 1;
    *h = 1;
}

static Th *find(uint32_t t) {
    for (int i = 0; i < MAX_THREADS; i++)
        if (s_threads[i].used && s_threads[i].addr == t) return &s_threads[i];
    return NULL;
}
static Th *alloc_th(uint32_t t) {
    Th *th = find(t);
    if (th) return th;
    for (int i = 0; i < MAX_THREADS; i++)
        if (!s_threads[i].used) { memset(&s_threads[i], 0, sizeof s_threads[i]); s_threads[i].used = true; s_threads[i].addr = t; return &s_threads[i]; }
    rt_log("sched: thread table full");
    return NULL;
}

/* True if this host thread's guest thread was cancelled/exited or its OSThread re-created: it
 * must never run guest code again. */
static bool stale(uint32_t me) {
    Th *th = find(me);
    return th && (th->state == TS_DEAD || th->incarnation != t_incarnation);
}

static void set_gstate(uint32_t t, uint64_t s) {
    if (t >= 0x80000000u) m_write_be(t + T_STATE, 2, s);
}

void sched_init(void) {
    s_start_ns = plat_now_ns();
    const char *ts = plat_getenv("MKDD_TIMESCALE");
    if (ts && atoi(ts) > 1) s_timescale = (uint64_t)atoi(ts);
    pmutex_init(&s_lock);
    pcond_init(&s_cv);
    for (int i = 0; i < MAX_THREADS; i++) pcond_init(&s_tcv[i]);
    Th *b = alloc_th(SCHED_BOOT);
    b->prio = 16;
    b->state = TS_RUNNING;
    b->started = true;
}

/* Re-key the boot context to the OS default thread once OSInit has created it, and record the
 * SDA bases for the idle context. Called with the lock NOT held. */
static void adopt(Ctx *c) {
    uint64_t sda = (RD(c, REG_R2, 4) << 32) | RD(c, REG_R13, 4);
    if (sda) s_sda = sda;
    if (c->thread == SCHED_BOOT) {
        uint32_t p = (uint32_t)m_read_be(OS_CURRENT_THREAD, 4);
        if (p >= 0x80000000u) {
            pmutex_lock(&s_lock);
            Th *th = find(SCHED_BOOT);
            if (th) {
                th->addr = p;
                th->prio = (int)(int32_t)m_read_be(p + T_PRIORITY, 4);
            }
            if (s_running == SCHED_BOOT) s_running = p;
            pmutex_unlock(&s_lock);
            c->thread = p;
        }
    }
}

/* The thread's current priority, from its OSThread. The SDK also changes it without
 * SetEffectivePriority: OSUnlockMutex ends priority inheritance by writing thread->priority
 * itself. Going by a cached copy kept the low-priority loader task (18) at an inherited 4 for
 * the whole session, so every queued load preempted the main thread (16) at once: after the
 * title demo, loadTask then ran inside SequenceApp's constructor, before mspSequenceApp was
 * set, read its heap through a null pointer (0x30310000, the "01" of the disc ID) and left the
 * title scene without its background. Lock held. */
static int prio_of(Th *t) {
    if (t->addr >= 0x80000000u) t->prio = (int)(int32_t)m_read_be(t->addr + T_PRIORITY, 4);
    return t->prio;
}

static Th *pick(void) {
    Th *best = NULL;
    int bp = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        Th *t = &s_threads[i];
        if (!t->used || t->state != TS_READY) continue;
        int p = prio_of(t);
        if (!best || p < bp || (p == bp && t->seq < best->seq)) { best = t; bp = p; }
    }
    return best;
}

static void make_ready(uint32_t t) {
    s_seq++;
    Th *th = find(t);
    if (th) { th->state = TS_READY; th->seq = s_seq; }
    set_gstate(t, GS_READY);
}

/* Wake whoever now holds the token (or the idle thread if nobody does). Lock held. */
/* Host thread that last took the token (for the Switch sampling profiler). */
volatile uint32_t g_sched_host;
uint32_t sched_running_now(void) { return s_running; }
void sched_note_host(void) { g_sched_host = plat_thread_handle(); }

static void kick_token(void) {
    Th *n = s_running >= 0x80000000u ? find(s_running) : NULL;
    if (n) pcond_broadcast(&s_tcv[n - s_threads]);
    else pcond_broadcast(&s_cv);
}

/* Block until something changes for guest thread me. Lock held. */
static void wait_mine(uint32_t me) {
    Th *th = find(me);
    pcond_wait(th ? &s_tcv[th - s_threads] : &s_cv, &s_lock);
}

static void give_token(void) {
    Th *n = pick();
    if (n) {
        s_running = n->addr;
        n->state = TS_RUNNING;
        if (n->addr >= 0x80000000u) {
            m_write_be(OS_CURRENT_THREAD, 4, n->addr);
            set_gstate(n->addr, GS_RUNNING);
        }
    } else {
        __atomic_store_n(&s_running, SCHED_NONE, __ATOMIC_SEQ_CST);
    }
}

/* Hand the token to the best ready thread and block until it comes back to me. Lock held. */
uint64_t g_sched_switches, g_sched_deliveries;

static void switch_to_next(uint32_t me) {
    g_sched_switches++;
    give_token();
    kick_token();
    Th *th = find(me);
    if (!th || th->state == TS_DEAD) return;
    while (s_running != me || stale(me)) {
        if (stale(me)) { pmutex_unlock(&s_lock); plat_park_forever(); }
        wait_mine(me);
    }
    sched_note_host();
}

/* If a higher-priority thread is ready, yield to it (never inside an interrupt handler). Lock held. */
static void maybe_preempt(Ctx *c) {
    if (c->in_irq || c->thread < 0x80000000u) return;
    Th *me = find(c->thread);
    int my = me ? prio_of(me) : 0x7FFFFFFF;
    Th *best = pick();
    if (best && best->prio < my) {
        make_ready(c->thread);
        switch_to_next(c->thread);
    }
}

static void wait_turn(uint32_t me, uint64_t incarnation) {
    t_incarnation = incarnation;
    pmutex_lock(&s_lock);
    while (s_running != me || stale(me)) {
        if (stale(me)) { pmutex_unlock(&s_lock); plat_park_forever(); }
        wait_mine(me);
    }
    sched_note_host();
    pmutex_unlock(&s_lock);
}

/* ---- OS thread API (called from hle.c) ---- */

void sched_create(Ctx *c, uint32_t t) {
    adopt(c);
    int prio = (int)(int32_t)m_read_be(t + T_PRIORITY, 4);
    pmutex_lock(&s_lock);
    uint64_t inc = ++s_next_incarnation;
    Th *th = alloc_th(t);
    if (th) {
        th->prio = prio; th->state = TS_SUSPENDED; th->seq = 0; th->suspend = 1;
        th->started = false; th->incarnation = inc; th->wait_queue = 0;
    }
    pmutex_unlock(&s_lock);
}

typedef struct { Ctx c; uint32_t t, entry; uint64_t incarnation; } Spawn;

static void guest_thread_main(void *p) {
    Spawn *s = p;
    wait_turn(s->t, s->incarnation);
    rt_log("thread %#010x starts at %#010x %s", s->t, s->entry, m_name(s->entry));
    ctx_call(&s->c, s->entry);
    sched_exit(&s->c);
    plat_park_forever();
}

static void spawn_guest_thread(Ctx *c, uint32_t t, uint64_t incarnation) {
    Spawn *s = malloc(sizeof *s);
    ctx_spawn_like(&s->c, c, t);
    /* The SDK's OSCreateThread already filled the thread's OSContext (gpr[1]=sp, gpr[3]=arg,
     * srr0=entry); start from there. */
    ctx_set_gpr(&s->c, 1, (uint32_t)m_read_be(t + 0x04, 4));
    ctx_set_gpr(&s->c, 3, (uint32_t)m_read_be(t + 0x0C, 4));
    s->entry = (uint32_t)m_read_be(t + 0x198, 4);
    WR(&s->c, REG_MSR, 4, RD(&s->c, REG_MSR, 4) | MSR_EE);
    s->t = t;
    s->incarnation = incarnation;
    if (!pthread_start("guest", guest_thread_main, s, GUEST_STACK, GUEST_CORE, 0)) rt_log("spawn guest thread %#x failed", t);
}

/* OSResumeThread: returns the previous suspend count. */
int sched_resume(Ctx *c, uint32_t t) {
    adopt(c);
    pmutex_lock(&s_lock);
    Th *th = find(t);
    if (!th) {
        pmutex_unlock(&s_lock);
        rt_log("OSResumeThread on unknown thread %#010x", t);
        return 0;
    }
    int prev = th->suspend;
    th->suspend = th->suspend - 1 > 0 ? th->suspend - 1 : 0;
    m_write_be(t + T_SUSPEND, 4, (uint32_t)th->suspend);
    if (th->suspend == 0 && th->state == TS_SUSPENDED) {
        bool spawn = !th->started;
        uint64_t inc = th->incarnation;
        th->started = true;
        make_ready(t);
        if (spawn) spawn_guest_thread(c, t, inc);
    }
    maybe_preempt(c);
    pmutex_unlock(&s_lock);
    return prev;
}

/* OSSuspendThread: returns the previous suspend count. */
int sched_suspend(Ctx *c, uint32_t t) {
    adopt(c);
    pmutex_lock(&s_lock);
    Th *th = find(t);
    if (!th) { pmutex_unlock(&s_lock); return 0; }
    int prev = th->suspend++;
    m_write_be(t + T_SUSPEND, 4, (uint32_t)th->suspend);
    if (th->state == TS_READY || th->state == TS_RUNNING) {
        th->state = TS_SUSPENDED;
        if (t == c->thread && !c->in_irq) switch_to_next(t);
    }
    pmutex_unlock(&s_lock);
    return prev;
}

void sched_sleep(Ctx *c, uint32_t queue) {
    adopt(c);
    uint32_t me = c->thread;
    if (c->in_irq || me < 0x80000000u) return; /* sleeping outside a real thread: just continue */
    pmutex_lock(&s_lock);
    Th *th = find(me);
    if (th) {
        th->state = TS_WAITING;
        th->wait_queue = queue;
        th->wait_lr = (uint32_t)RD(c, REG_LR, 4);
        th->wait_sp = ctx_gpr(c, 1);
    }
    set_gstate(me, GS_WAITING);
    switch_to_next(me);
    pmutex_unlock(&s_lock);
}

static void wake_queue(uint32_t queue) {
    for (int i = 0; i < MAX_THREADS; i++) {
        Th *t = &s_threads[i];
        if (t->used && t->state == TS_WAITING && t->wait_queue == queue) make_ready(t->addr);
    }
}

void sched_wakeup(Ctx *c, uint32_t queue) {
    adopt(c);
    pmutex_lock(&s_lock);
    wake_queue(queue);
    maybe_preempt(c);
    pmutex_unlock(&s_lock);
}

void sched_yield_now(Ctx *c) {
    adopt(c);
    if (c->in_irq || c->thread < 0x80000000u) return;
    pmutex_lock(&s_lock);
    make_ready(c->thread);
    switch_to_next(c->thread);
    pmutex_unlock(&s_lock);
}

void sched_reschedule(Ctx *c) {
    adopt(c);
    pmutex_lock(&s_lock);
    maybe_preempt(c);
    pmutex_unlock(&s_lock);
}

/* OSExitThread / thread function returned: wake joiners and hand the token on. */
/* SetEffectivePriority (OSSetThreadPriority and mutex priority inheritance). The SDK's version
 * moves a ready thread between the run queues, which this scheduler doesn't keep: the thread's
 * queue pointer is null there, so it wrote zeros over the disc ID at 0x80000000 (saves were then
 * created under a blank game code, or not found). Returns the owner of the mutex a waiting thread
 * waits for (the next thread to promote), as the SDK does. */
uint32_t sched_set_priority(uint32_t t, int prio) {
    m_write_be(t + T_PRIORITY, 4, (uint32_t)prio);
    pmutex_lock(&s_lock);
    Th *th = find(t);
    if (th) th->prio = prio;
    pmutex_unlock(&s_lock);
    static int logged;
    if (logged < 32) { logged++; rt_log("thread %#010x priority %d", t, prio); }
    uint32_t mutex = (uint32_t)m_read_be(t + T_MUTEX, 4);
    if (m_read_be(t + T_STATE, 2) == GS_WAITING && mutex >= 0x80000000u) return (uint32_t)m_read_be(mutex + 8, 4);
    return 0;
}

void sched_exit(Ctx *c) {
    uint32_t me = c->thread;
    pmutex_lock(&s_lock);
    Th *th = find(me);
    if (th) th->state = TS_DEAD;
    set_gstate(me, GS_MORIBUND);
    wake_queue(me + T_QUEUE_JOIN);
    switch_to_next(me);
    pmutex_unlock(&s_lock);
}

void sched_cancel(Ctx *c, uint32_t t) {
    if (t == c->thread) { sched_exit(c); return; }
    pmutex_lock(&s_lock);
    Th *th = find(t);
    if (th) { th->state = TS_DEAD; pcond_broadcast(&s_tcv[th - s_threads]); }
    set_gstate(t, GS_MORIBUND);
    pmutex_unlock(&s_lock);
}

/* ---- alarms ---- */

void sched_set_alarm(uint32_t ptr, uint32_t handler, uint64_t fire, uint64_t period) {
    pmutex_lock(&s_lock);
    int i;
    for (i = 0; i < s_nalarms && s_alarms[i].ptr != ptr; i++) {}
    if (i == s_nalarms) {
        if (s_nalarms == MAX_ALARMS) { pmutex_unlock(&s_lock); rt_log("sched: alarm table full"); return; }
        s_nalarms++;
    }
    s_alarms[i] = (Alarm){ptr, handler, fire, period};
    wake_idle();
    pmutex_unlock(&s_lock);
}

void sched_cancel_alarm(uint32_t ptr) {
    pmutex_lock(&s_lock);
    for (int i = 0; i < s_nalarms; i++)
        if (s_alarms[i].ptr == ptr) { s_alarms[i] = s_alarms[--s_nalarms]; break; }
    pmutex_unlock(&s_lock);
}

/* Due alarms, in table order; periodic ones are rescheduled. */
static int take_due_alarms(uint32_t out[][2], int max) {
    uint64_t now = sched_timebase();
    int n = 0;
    pmutex_lock(&s_lock);
    for (int i = 0; i < s_nalarms;) {
        Alarm *a = &s_alarms[i];
        if (a->fire > now) { i++; continue; }
        if (n < max) { out[n][0] = a->ptr; out[n][1] = a->handler; n++; }
        if (a->period > 0) {
            while (a->fire <= now) a->fire += a->period;
            i++;
        } else {
            *a = s_alarms[--s_nalarms];
        }
    }
    pmutex_unlock(&s_lock);
    return n;
}

/* Generate time-driven interrupts; returns ns until the next event. Lock held. */
static uint64_t tick(void) {
    uint64_t now = sched_timebase();
    if (now >= s_next_vi) {
        sched_raise(IRQ_VI);
        __atomic_fetch_add(&g_vi_count, 1, __ATOMIC_RELAXED);
        s_last_vi_tb = now;
        gx_scanout();
        /* Keep a fixed cadence; if we fell more than a period behind, skip whole periods. */
        s_next_vi += VI_PERIOD;
        if (s_next_vi <= now) s_next_vi += (now - s_next_vi) / VI_PERIOD * VI_PERIOD + VI_PERIOD;
    }
    uint64_t next = s_next_vi;
    uint64_t ai = s_ai_period;
    if (ai > 0) {
        uint64_t due = s_ai_next;
        if (now >= due) {
            if (__atomic_load_n(&s_pending_irq, __ATOMIC_RELAXED) & (1u << IRQ_DSP_AI)) {
                g_ai_lost++;
                if (g_ai_lost < 40) rt_log("AI merged: running %#x last fn %s", s_running, m_name(g_last_fn));
            } else s_ai_raised = now;
            sched_raise(IRQ_DSP_AI);
            s_ai_next = now - due > 4 * ai ? now + ai : due + ai;
        }
        if (s_ai_next < next) next = s_ai_next;
    }
    for (int i = 0; i < s_nalarms; i++) {
        if (s_alarms[i].fire <= now) { s_alarm_due = true; g_attention = true; }
        if (s_alarms[i].fire < next) next = s_alarms[i].fire;
    }
    uint64_t ns = next > now ? (next - now) * 2000 / 81 : 0;
    if (ns < 200000) ns = 200000;
    if (ns > 16000000) ns = 16000000;
    return ns;
}

/* Parts of the register space the PowerPC model uses (Ghidra's ppc_32_4xx_be/Gekko layout:
 * GPRs, XER, CR fields, SPRs, FPRs, paired-single halves, ...). The rest of the 64 KiB is never
 * touched (0x7000.. are the 4xx DCRs, which Gekko code never uses), so an interrupt saves ~5 KiB
 * instead of the whole space. */
static const struct { uint32_t off, len; } REG_RANGES[] = {
    {0x0, 0x80}, {0x400, 0x8}, {0x500, 0x20}, {0x700, 0x8}, {0x720, 0x18}, {0x780, 0x8}, {0x800, 0x40},
    {0x900, 0x8}, {0x980, 0x10}, {0x1000, 0x1000}, {0x4000, 0x100}, {0x5000, 0x100}, {0x6000, 0x8},
    {0x10000, 0x8},
};
#define REG_SAVE_BYTES (0x80 + 0x8 + 0x20 + 0x8 + 0x18 + 0x8 + 0x40 + 0x8 + 0x10 + 0x1000 + 0x100 + 0x100 + 0x8 + 0x8)

static void regs_save(const Ctx *c, uint8_t *buf) {
    for (size_t i = 0, o = 0; i < sizeof REG_RANGES / sizeof *REG_RANGES; o += REG_RANGES[i++].len)
        memcpy(buf + o, c->regs + REG_RANGES[i].off, REG_RANGES[i].len);
}
static void regs_restore(Ctx *c, const uint8_t *buf) {
    for (size_t i = 0, o = 0; i < sizeof REG_RANGES / sizeof *REG_RANGES; o += REG_RANGES[i++].len)
        memcpy(c->regs + REG_RANGES[i].off, buf + o, REG_RANGES[i].len);
}

/* Run pending interrupt handlers and due alarms on c (which holds the token). */
void rt_deliver(Ctx *c) {
    g_sched_deliveries++;
    uint64_t t_deliver0 = plat_now_ns();
    g_attention = false; /* anything raised while handlers run sets it again */
    _Static_assert(REG_BYTES >= 0x10008, "register ranges past the register space");
    uint8_t mine[REG_SAVE_BYTES]; /* handlers can nest through other guest calls */
    regs_save(c, mine);
    c->in_irq = true;
    WR(c, REG_MSR, 4, RD(c, REG_MSR, 4) & ~MSR_EE);
    uint32_t ctx_ptr = (uint32_t)m_read_be(OS_CURRENT_THREAD, 4);
    uint32_t bits = __atomic_exchange_n(&s_pending_irq, 0, __ATOMIC_ACQ_REL);
    for (int irq = 0; irq < 32; irq++) {
        if (!(bits & (1u << irq))) continue;
        if (irq == IRQ_DSP_AI) {
            uint64_t us = (sched_timebase() - s_ai_raised) * 2000 / 81 / 1000;
            if (us > g_ai_lat_max_us) g_ai_lat_max_us = us;
            static int logged; /* AI interrupts taken more than half a period late (first 80) */
            if (us > 8000 && logged++ < 80) rt_log("AI late %llu us: delivered by %#x, last fn %s", (unsigned long long)us, c->thread, m_name(g_last_fn));
        }
        hw_before_interrupt(c, irq);
        uint32_t handler = (uint32_t)m_read_be(INTERRUPT_TABLE + 4 * irq, 4);
        if (irq == IRQ_DSP_DSP && handler == 0) {
            /* JAudio never runs the SDK DSP driver here (DspBoot is HLE'd): talk to it directly. */
            dsp_interrupt(c);
        } else if (handler) {
            uint32_t args[2] = {(uint32_t)irq, ctx_ptr};
            ctx_call_guest(c, handler, args, 2);
        }
    }
    if (__atomic_exchange_n(&s_alarm_due, false, __ATOMIC_ACQ_REL)) {
        uint32_t due[MAX_ALARMS][2];
        int n = take_due_alarms(due, MAX_ALARMS);
        for (int i = 0; i < n; i++) {
            if (!due[i][1]) continue;
            uint32_t args[2] = {due[i][0], ctx_ptr};
            ctx_call_guest(c, due[i][1], args, 2);
        }
    }
    { /* slow interrupt handlers (first 60) */
        static int logged;
        uint64_t d = plat_now_ns() - t_deliver0;
        if (d > 4000000 && logged++ < 60) rt_log("deliver took %.1f ms, irqs %#x, thread %#x", d / 1e6, bits, c->thread);
    }
    regs_restore(c, mine);
    c->in_irq = false;
    if (c->thread >= 0x80000000u) {
        pmutex_lock(&s_lock);
        maybe_preempt(c);
        pmutex_unlock(&s_lock);
    }
}

/* Timer thread: raises VI / AI / alarm events at their times. Kept separate from the idle
 * thread, whose interrupt-handler work would otherwise delay retraces. */
static void timer_loop(void *arg) {
    (void)arg;
    for (;;) {
        pmutex_lock(&s_lock);
        uint64_t wait = tick();
        pmutex_unlock(&s_lock);
        wake_idle(); /* due alarms (tick sets s_alarm_due) */
        uint64_t deadline = plat_now_ns() + wait;
#ifdef __SWITCH__
        /* Horizon's sleeps are accurate; never spin (a spinning thread starves its core). */
        plat_sleep_ns(wait);
#else
        if (wait > 1500000) plat_sleep_ns(wait - 1200000);
        while (plat_now_ns() < deadline) plat_yield();
#endif
        (void)deadline;
    }
}

static Ctx s_idle_ctx;
uint64_t g_guest_idle_ns, g_guest_idle_gpu_ns;

static void idle_loop(void *arg) {
    (void)arg;
    Ctx *c = &s_idle_ctx;
    ctx_init(c);
    c->thread = SCHED_IDLE;
    for (;;) {
        pmutex_lock(&s_lock);
        if (s_running == SCHED_NONE && pending()) {
            s_running = SCHED_IDLE;
            sched_note_host();
            pmutex_unlock(&s_lock);
            uint64_t sda = s_sda;
            WR(c, REG_R2, 4, sda >> 32);
            WR(c, REG_R13, 4, sda & 0xFFFFFFFFu);
            ctx_set_gpr(c, 1, s_idle_sp);
            rt_deliver(c);
            pmutex_lock(&s_lock);
            if (s_running == SCHED_IDLE) {
                give_token();
                kick_token();
            }
            pmutex_unlock(&s_lock);
            continue;
        }
        /* statistics: no guest thread runnable, and whether the decoder still had work */
        bool idle = s_running == SCHED_NONE, gpu = idle && gx_gpu_busy_hint();
        uint64_t t0 = idle ? plat_now_ns() : 0;
        pcond_wait_ns(&s_cv, &s_lock, idle ? 2000000 : 20000000); /* while the game runs, only a backstop */
        if (idle) {
            uint64_t d = plat_now_ns() - t0;
            g_guest_idle_ns += d;
            if (gpu) g_guest_idle_gpu_ns += d;
        }
        pmutex_unlock(&s_lock);
    }
}

void sched_start_idle(void) {
    /* The timer runs at a higher priority than the GX decoder sharing its core. */
    pthread_start("mkdd-idle", idle_loop, NULL, GUEST_STACK, GUEST_CORE, 0);
    pthread_start("mkdd-timer", timer_loop, NULL, 64 << 10, 2, -2);
}

bool sched_waiting_on(uint32_t t, uint32_t queue) {
    pmutex_lock(&s_lock);
    Th *th = find(t);
    bool w = th && th->state == TS_WAITING && th->wait_queue == queue;
    pmutex_unlock(&s_lock);
    return w;
}

void sched_describe(char *out, size_t n) {
    pmutex_lock(&s_lock);
    size_t k = (size_t)snprintf(out, n, "running=%#x ", s_running);
    static const char *names[] = {"Ready", "Running", "Waiting", "Suspended", "Dead"};
    for (int i = 0; i < MAX_THREADS && k < n; i++) {
        Th *t = &s_threads[i];
        if (!t->used) continue;
        k += (size_t)snprintf(out + k, n - k, "[%#010x p%d %s s%d", t->addr, t->prio, names[t->state], t->suspend);
        if (t->state == TS_WAITING && k < n) {
            /* what it waits on: the queue, the caller of the sleep and the stack's saved-LR chain */
            k += (size_t)snprintf(out + k, n - k, " q%#x lr%#x", t->wait_queue, t->wait_lr);
            uint32_t fp = t->wait_sp;
            for (int d = 0; d < 6 && k < n; d++) {
                uint32_t next = (uint32_t)m_read_be(fp, 4);
                if (next < 0x80000000u || next >= 0x81800000u || next <= fp) break;
                k += (size_t)snprintf(out + k, n - k, "<%#x", (uint32_t)m_read_be(next + 4, 4));
                fp = next;
            }
        }
        if (k < n) k += (size_t)snprintf(out + k, n - k, "] ");
    }
    pmutex_unlock(&s_lock);
}
