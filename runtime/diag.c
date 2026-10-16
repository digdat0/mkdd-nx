/* Diagnostics: the log (each distinct message printed once, repeats counted) and a watchdog that
 * appends a snapshot to the trace file every 5 s. C port of gc-runtime's diag.rs. */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "gx.h"
#include "rt.h"

#define MAX_SEEN 4096
static PMutex s_lock;
static bool s_init;
static uint64_t s_seen_hash[MAX_SEEN];
static uint32_t s_seen_count[MAX_SEEN];
static unsigned s_nseen, s_lines;
static FILE *s_log;
volatile uint32_t g_log_mask = LOG_GAME | LOG_AUDIO;
static char s_trace_path[512];
static struct { const char *name; uint64_t n; } s_intr[64];
static unsigned s_nintr;

static void lock(void) {
    if (!s_init) { pmutex_init(&s_lock); s_init = true; }
    pmutex_lock(&s_lock);
}

void diag_set_log_file(const char *path) {
    lock();
    plat_mkdirs_for(path);
    s_log = fopen(path, "w");
    if (s_log) setvbuf(s_log, NULL, _IOFBF, 64 << 10);
    pmutex_unlock(&s_lock);
}

void diag_flush(void) {
    lock();
    if (s_log) fflush(s_log);
    fflush(stderr);
    pmutex_unlock(&s_lock);
}

void rt_log(const char *fmt, ...) {
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
    if (!(g_log_mask & (audio ? LOG_AUDIO : LOG_GAME))) return;
    uint64_t h = fnv_bytes(FNV_INIT, msg, strlen(msg));
    lock();
    for (unsigned i = 0; i < s_nseen; i++)
        if (s_seen_hash[i] == h) { s_seen_count[i]++; pmutex_unlock(&s_lock); return; }
    if (s_nseen < MAX_SEEN) { s_seen_hash[s_nseen] = h; s_seen_count[s_nseen++] = 1; }
    if (s_lines++ < 20000) {
        double t = (double)plat_now_ns() / 1e9;
        fprintf(stderr, "[mkdd %.3f] %s\n", t, msg);
        if (s_log) fprintf(s_log, "[%.3f] %s\n", t, msg);
    }
    pmutex_unlock(&s_lock);
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
    char buf[4096];
    for (;;) {
        plat_sleep_ns(5000000000ull);
        uint64_t now[5] = {gx_frame_count(), g_ai_buffers, g_audio_underruns, g_dvd_reads, g_dsp_subframes};
        char threads[2048];
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
        static uint64_t prev_sw, prev_dl;
        n += snprintf(buf + n, sizeof buf - (size_t)n, "gx decoder: %.0f%% busy, %llu draws/5s; %llu thread switches/5s, %llu deliveries/5s\n",
                      (double)(busy - prev_busy) / 5e7, (unsigned long long)(draws - prev_draws),
                      (unsigned long long)(g_sched_switches - prev_sw), (unsigned long long)(g_sched_deliveries - prev_dl));
        static uint64_t prev_idle, prev_idle_gpu, prev_qw, prev_setups, prev_cpuv;
        n += snprintf(buf + n, sizeof buf - (size_t)n,
                      "game core: %.0f%% busy, idle %.0f%% (%.0f%% while the decoder was busy), queue-full waits %.0f ms/5s; "
                      "gx setups %llu/5s, CPU-transformed verts %llu/5s\n",
                      100.0 - (double)(g_guest_idle_ns - prev_idle) / 5e7, (double)(g_guest_idle_ns - prev_idle) / 5e7,
                      (double)(g_guest_idle_gpu_ns - prev_idle_gpu) / 5e7, (double)(g_gx_qwait_ns - prev_qw) / 1e6,
                      (unsigned long long)(g_gx_setups - prev_setups), (unsigned long long)(g_gx_cpu_verts - prev_cpuv));
        prev_idle = g_guest_idle_ns; prev_idle_gpu = g_guest_idle_gpu_ns; prev_qw = g_gx_qwait_ns;
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
        if (s_log) fflush(s_log);
        pmutex_unlock(&s_lock);
        if (f && g_log_mask) { fputs(buf, f); fflush(f); }
    }
}

void diag_start_watchdog(const char *trace_path) {
    snprintf(s_trace_path, sizeof s_trace_path, "%s", trace_path);
    plat_mkdirs_for(trace_path);
    pthread_start("mkdd-watchdog", watchdog, NULL, 256 << 10, -1, 0);
}
