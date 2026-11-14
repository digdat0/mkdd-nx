/* Diagnostics: the log (each distinct message printed once, repeats counted) and a watchdog that
 * appends a snapshot to the trace file every 5 s. C port of gc-runtime's diag.rs.
 *
 * rt_log never writes a file on the calling thread (an SD card write stalls for up to ~260 ms,
 * and a guest thread waiting there held up the game): lines go into a ring that the watchdog
 * thread drains to stderr and mkdd.log (woken at once during boot or when the ring fills up, else
 * every 200 ms; files flushed at most once a second, at once during boot). Before the watchdog
 * runs (early boot) rt_log still writes through itself. diag_flush drains on the calling thread
 * (exit, panic, settings changes); diag_flush_crash does so without taking any lock. */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "gx.h"
#include "rt.h"

#define MAX_SEEN 4096u   /* distinct messages remembered (later new ones are always printed) */
#define SEEN_SLOTS 8192u /* open-addressed hash table, at most half full */
#define RING (256u << 10)
static PMutex s_lock;  /* the seen table, the ring's head and drops, the intrinsic counts */
static PMutex s_dlock = PTHREAD_MUTEX_INITIALIZER; /* the drain side: s_tail, the files */
static PCond s_cv = PTHREAD_COND_INITIALIZER;     /* wakes the watchdog to drain */
static bool s_init;
static uint64_t s_seen[SEEN_SLOTS]; /* hash | 1 (0 = empty) */
static unsigned s_nseen, s_lines;
static FILE *s_log;
volatile uint32_t g_log_mask = LOG_GAME | LOG_AUDIO | LOG_BATTLE;
volatile bool g_log_boot = true;
static char s_trace_path[512];
static struct { const char *name; uint64_t n; } s_intr[64];
static unsigned s_nintr;
/* Ring of records (2-byte length, then "[t] message\n"). s_head and s_tail count bytes ever
 * written and drained; s_tail only moves under both locks. */
static char s_ring[RING];
static size_t s_head, s_tail;
static unsigned s_dropped;
static volatile bool s_drainer; /* the watchdog runs */
static char s_out[RING + 64];  /* the drain's copy (one drain at a time: s_dlock) */

static void lock(void) {
    if (!s_init) { pmutex_init(&s_lock); s_init = true; }
    pmutex_lock(&s_lock);
}

static void ring_put(const void *p, size_t n) {
    const char *b = p;
    size_t at = s_head % RING, first = RING - at < n ? RING - at : n;
    memcpy(s_ring + at, b, first);
    memcpy(s_ring, b + first, n - first);
    s_head += n;
}

/* Copy the ring out (s_lock held unless crashing); returns the bytes. */
static size_t ring_take(unsigned *dropped) {
    size_t n = s_head - s_tail, at = s_tail % RING;
    if (n > RING) n = 0; /* (torn read while crashing) */
    size_t first = RING - at < n ? RING - at : n;
    memcpy(s_out, s_ring + at, first);
    memcpy(s_out + first, s_ring, n - first);
    s_tail += n;
    *dropped = s_dropped;
    s_dropped = 0;
    return n;
}

/* Write a copied-out run of records to the files (s_dlock held unless crashing). */
static void write_out(size_t n, unsigned dropped) {
    for (size_t i = 0; i + 2 <= n;) {
        size_t len = (size_t)(uint8_t)s_out[i] | (size_t)(uint8_t)s_out[i + 1] << 8;
        if (i + 2 + len > n || len < 2) break;
        const char *line = s_out + i + 2; /* "[t] message\n" */
        fprintf(stderr, "[mkdd %.*s", (int)len - 1, line + 1);
        if (s_log) fwrite(line, 1, len, s_log);
        i += 2 + len;
    }
    if (dropped) {
        fprintf(stderr, "[mkdd] log: %u lines dropped (the log ring was full)\n", dropped);
        if (s_log) fprintf(s_log, "[%.3f] log: %u lines dropped (the log ring was full)\n", (double)plat_now_ns() / 1e9, dropped);
    }
}

/* Drain the ring to the files on this thread; flush them too if asked. Returns bytes drained. */
static size_t drain(bool flush) {
    pmutex_lock(&s_dlock);
    lock();
    unsigned dropped;
    size_t n = ring_take(&dropped);
    pmutex_unlock(&s_lock);
    write_out(n, dropped);
    if (flush) {
        if (s_log) fflush(s_log);
        fflush(stderr);
    }
    pmutex_unlock(&s_dlock);
    return n;
}

#define LOG_LINES 50000u /* mkdd.log's lines per run (about 6 MB) */
void diag_set_log_file(const char *path) {
    pmutex_lock(&s_dlock);
    plat_mkdirs_for(path);
    s_log = fopen(path, "w");
    if (s_log) setvbuf(s_log, NULL, _IOFBF, 64 << 10);
    pmutex_unlock(&s_dlock);
}

/* from the crash handler: the locks may be the crashed thread's own */
void diag_flush_crash(void) {
    unsigned dropped;
    size_t n = ring_take(&dropped);
    write_out(n, dropped);
    if (s_log) fflush(s_log);
    fflush(stderr);
}

void diag_flush(void) { drain(true); }

void rt_log(const char *fmt, ...) {
    uint32_t mask = g_log_mask | (g_log_boot ? LOG_GAME : 0);
    if (!mask) return; /* Logging off: don't even format the line */
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (!strncmp(msg, "unknown call target", 19) || !strncmp(msg, "unmapped", 8)) {
        size_t n = strlen(msg);
        snprintf(msg + n, sizeof msg - n, " in %#010x %s", g_last_fn, m_name(g_last_fn));
    }
    bool audio = !strncmp(msg, "DSP", 3) || !strncmp(msg, "audio", 5) || !strncmp(msg, "AI ", 3) || !strncmp(msg, "AX", 2);
    if (!(mask & (audio ? LOG_AUDIO : LOG_GAME))) return;
    uint64_t h = fnv_bytes(FNV_INIT, msg, strlen(msg)) | 1;
    char line[1100];
    int len = 0;
    lock();
    unsigned slot = (unsigned)(h ^ h >> 29) & (SEEN_SLOTS - 1);
    while (s_seen[slot] && s_seen[slot] != h) slot = (slot + 1) & (SEEN_SLOTS - 1);
    if (s_seen[slot]) { pmutex_unlock(&s_lock); return; } /* printed before */
    if (s_nseen < MAX_SEEN) { s_seen[slot] = h; s_nseen++; }
    if (s_lines == LOG_LINES) {
        s_lines++;
        len = snprintf(line + 2, sizeof line - 2, "[%.3f] log: %u lines, the rest of this run not logged\n", (double)plat_now_ns() / 1e9, LOG_LINES);
    } else if (s_lines < LOG_LINES) {
        s_lines++;
        len = snprintf(line + 2, sizeof line - 2, "[%.3f] %s\n", (double)plat_now_ns() / 1e9, msg);
    }
    if (len >= (int)sizeof line - 2) len = (int)sizeof line - 3, line[2 + len - 1] = '\n';
    bool wake = false;
    if (len > 0) {
        if (s_head - s_tail + 2 + (size_t)len > RING) s_dropped++;
        else {
            line[0] = (char)(len & 0xFF), line[1] = (char)(len >> 8);
            ring_put(line, 2 + (size_t)len);
            wake = g_log_boot || s_head - s_tail > RING / 4;
        }
    }
    bool direct = len > 0 && !s_drainer;
    if (wake && s_drainer) pcond_broadcast(&s_cv);
    pmutex_unlock(&s_lock);
    if (direct) drain(g_log_boot); /* early boot: no watchdog yet */
}

void diag_intrinsic(const char *name) {
    lock();
    unsigned i;
    for (i = 0; i < s_nintr && strcmp(s_intr[i].name, name); i++) {}
    if (i == s_nintr && s_nintr < 64) s_intr[s_nintr++].name = name;
    if (i < 64) s_intr[i].n++;
    pmutex_unlock(&s_lock);
}

static void watchdog(void *arg) {
    (void)arg;
    uint64_t start = plat_now_ns();
    uint64_t prev[5] = {0}, prev_busy = 0, prev_draws = 0, prev_stale = 0, prev_lost = 0;
    FILE *f = fopen(s_trace_path, "w");
    char buf[8192];
    uint64_t next = start + 5000000000ull, flushed = 0;
    bool dirty = false;
    s_drainer = true;
    for (;;) {
        /* drain the log ring (rt_log wakes this during boot and when the ring fills up) */
        lock();
        if (s_head == s_tail) pcond_wait_ns(&s_cv, &s_lock, 200000000ull);
        pmutex_unlock(&s_lock);
        uint64_t t = plat_now_ns();
        if (drain(false)) dirty = true;
        if (dirty && (g_log_boot || t - flushed >= 1000000000ull || t >= next)) { /* SD writes here, not in rt_log */
            pmutex_lock(&s_dlock);
            if (s_log) fflush(s_log);
            fflush(stderr);
            pmutex_unlock(&s_dlock);
            dirty = false;
            flushed = t;
        }
        if (t < next) continue;
        next = t - next >= 5000000000ull ? t + 5000000000ull : next + 5000000000ull;
        uint64_t now[5] = {gx_frame_count(), g_ai_buffers, g_audio_underruns, g_dvd_reads, g_dsp_subframes};
        char threads[4096];
        sched_describe(threads, sizeof threads);
        int n = snprintf(buf, sizeof buf,
                         "=== t=%.0fs entries=%llu calls=%llu unresolved=%llu block_misses=%llu frames=%llu gx_bytes=%llu\n"
                         "pacing: %llu frames/5s, %llu AI buffers/5s (%llu stale, %llu merged, max latency %llu us), audio queue %zu, underruns %llu, DVD reads %llu, DSP subframes %llu, voices %llu\n"
                         "last fn: %#010x %s\nthreads: %s\n",
                         (double)(plat_now_ns() - start) / 1e9, (unsigned long long)g_fn_entries, (unsigned long long)g_calls,
                         (unsigned long long)g_unknown_targets, (unsigned long long)g_block_misses, (unsigned long long)now[0],
                         (unsigned long long)g_gx_bytes, (unsigned long long)(now[0] - prev[0]), (unsigned long long)(now[1] - prev[1]),
                         (unsigned long long)(g_ai_stale - prev_stale), (unsigned long long)(g_ai_lost - prev_lost), (unsigned long long)g_ai_lat_max_us, audio_queued(), (unsigned long long)(now[2] - prev[2]), (unsigned long long)(now[3] - prev[3]),
                         (unsigned long long)(now[4] - prev[4]), (unsigned long long)g_dsp_voices_last, g_last_fn, m_name(g_last_fn), threads);
        uint64_t busy = g_gx_busy_ns, draws = g_gx_draws;
        static uint64_t prev_sw, prev_dl, prev_bl;
        n += snprintf(buf + n, sizeof buf - (size_t)n, "gx decoder: %.0f%% busy, %llu draws/5s; %llu thread switches/5s, %llu deliveries/5s, %llu blocking waits/5s\n",
                      (double)(busy - prev_busy) / 5e7, (unsigned long long)(draws - prev_draws),
                      (unsigned long long)(g_sched_switches - prev_sw), (unsigned long long)(g_sched_deliveries - prev_dl),
                      (unsigned long long)(g_sched_blocks - prev_bl));
        prev_bl = g_sched_blocks;
        static uint64_t prev_drains, prev_drain_ns, prev_skips;
        static uint64_t prev_idle, prev_idle_gpu, prev_qw, prev_setups, prev_cpuv;
        n += snprintf(buf + n, sizeof buf - (size_t)n,
                      "game core: %.0f%% busy, idle %.0f%% (%.0f%% while the decoder was busy), queue-full waits %.0f ms/5s, DMA drains %llu (%.1f ms, %llu skipped)/5s; "
                      "gx setups %llu/5s, CPU-transformed verts %llu/5s\n",
                      100.0 - (double)(g_guest_idle_ns - prev_idle) / 5e7, (double)(g_guest_idle_ns - prev_idle) / 5e7,
                      (double)(g_guest_idle_gpu_ns - prev_idle_gpu) / 5e7, (double)(g_gx_qwait_ns - prev_qw) / 1e6,
                      (unsigned long long)(g_gx_drains - prev_drains), (double)(g_gx_drain_ns - prev_drain_ns) / 1e6, (unsigned long long)(g_gx_drain_skips - prev_skips),
                      (unsigned long long)(g_gx_setups - prev_setups), (unsigned long long)(g_gx_cpu_verts - prev_cpuv));
        prev_idle = g_guest_idle_ns; prev_idle_gpu = g_guest_idle_gpu_ns; prev_qw = g_gx_qwait_ns;
        prev_drains = g_gx_drains; prev_drain_ns = g_gx_drain_ns; prev_skips = g_gx_drain_skips;
        prev_setups = g_gx_setups; prev_cpuv = g_gx_cpu_verts;
        prev_sw = g_sched_switches;
        prev_dl = g_sched_deliveries;
        prev_busy = busy;
        prev_draws = draws;
        prev_stale = g_ai_stale;
        prev_lost = g_ai_lost;
        g_ai_lat_max_us = 0;
        memcpy(prev, now, sizeof prev);
        lock();
        for (unsigned i = 0; i < s_nintr && n < (int)sizeof buf - 64; i++)
            n += snprintf(buf + n, sizeof buf - (size_t)n, "intrinsic %s x%llu\n", s_intr[i].name, (unsigned long long)s_intr[i].n);
        if (g_syscalls && n < (int)sizeof buf - 64)
            n += snprintf(buf + n, sizeof buf - (size_t)n, "intrinsic syscall x%llu (inline)\n", (unsigned long long)g_syscalls);
        extern uint64_t g_disc_ahead_hits, g_disc_reads_direct;
        if (n < (int)sizeof buf - 96)
            n += snprintf(buf + n, sizeof buf - (size_t)n, "disc: %llu reads from the read-ahead window, %llu direct\n",
                          (unsigned long long)g_disc_ahead_hits, (unsigned long long)g_disc_reads_direct);
        pmutex_unlock(&s_lock);
        if (f && g_log_mask) { fputs(buf, f); fflush(f); }
    }
}

void diag_start_watchdog(const char *trace_path) {
    snprintf(s_trace_path, sizeof s_trace_path, "%s", trace_path);
    plat_mkdirs_for(trace_path);
    pthread_start("mkdd-watchdog", watchdog, NULL, 256 << 10, -1, 0);
}
