/* Sampling profiler (Switch). About 500 times a second it pauses the host thread that holds the
 * guest token (every 4th tick also the GX decoder, and the render thread two ticks later), reads its
 * registers and walks the frame-pointer
 * chain, and appends PC, LR and the return addresses as image offsets to mkdd_prof.bin.
 * tools/prof_report.py turns that into self / inclusive tables with the build's .elf.
 * While the target is paused nothing here takes a lock or allocates (it may hold either). */
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "../runtime/rt.h"
#include "prof_switch.h"
#include "menu.h"

#define MAX_FRAMES 32
#define PERIOD_NS 2000000
enum { K_GAME = 1, K_IDLE = 2, K_GPU = 3, K_FAIL = 4, K_RENDER = 5 };

static FILE *s_f;
static u64 s_base, s_end, s_t0;
static u32 s_buf[48 << 10];
static size_t s_n;
static volatile bool s_stop;
static PMutex s_flock;
static u64 s_samples[6];
static Handle s_render; /* the main thread (GL rendering, present) */

static void flush_locked(void) {
    if (s_f && s_n) {
        fwrite(s_buf, 4, s_n, s_f);
        fflush(s_f);
    }
    s_n = 0;
}

static void put(const u32 *w, size_t n) {
    pmutex_lock(&s_flock);
    if (s_n + n > sizeof s_buf / 4) flush_locked();
    memcpy(s_buf + s_n, w, n * 4);
    s_n += n;
    pmutex_unlock(&s_flock);
}

static u32 off(u64 a) { return a >= s_base && a < s_end ? (u32)(a - s_base) : 0; }

/* PC, LR, then the frame chain. -1 if the thread could not be paused / read. */
static int grab(Handle h, u32 *fr) {
    if (!h || R_FAILED(svcSetThreadActivity(h, ThreadActivity_Paused))) return -1;
    ThreadContext tc;
    int n = -1;
    if (R_SUCCEEDED(svcGetThreadContext3(&tc, h))) {
        n = 0;
        fr[n++] = off(tc.pc.x);
        fr[n++] = off(tc.lr);
        MemoryInfo mi;
        u32 pi;
        if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, tc.sp)) && (mi.perm & Perm_R)) {
            u64 lo = tc.sp, hi = mi.addr + mi.size, fp = tc.fp;
            while (n < MAX_FRAMES && fp >= lo && fp + 16 <= hi && !(fp & 7)) {
                const u64 *p = (const u64 *)fp;
                fr[n++] = off(p[1]);
                if (p[0] <= fp) break;
                fp = p[0];
            }
        }
    }
    svcSetThreadActivity(h, ThreadActivity_Runnable);
    return n;
}

static void record(int kind, const u32 *fr, int n) {
    u32 w[3 + MAX_FRAMES];
    if (n < 0) { kind = K_FAIL; n = 0; }
    w[0] = (u32)kind | (u32)n << 8;
    w[1] = (u32)((armTicksToNs(armGetSystemTick()) - s_t0) / 1000000u);
    w[2] = g_last_fn;
    memcpy(w + 3, fr, (size_t)n * 4);
    put(w, 3 + (size_t)n);
    s_samples[kind]++;
}

static void sampler(void *arg) {
    (void)arg;
    u32 fr[MAX_FRAMES];
    u64 tick = 0, last_log = armGetSystemTick();
    while (!s_stop) {
        svcSleepThread(PERIOD_NS);
        if (!(g_log_mask & LOG_GAME)) { svcSleepThread(100000000); continue; } /* LOGGING off: no samples */
        u32 run = sched_running_now();
        if (run == SCHED_NONE) record(K_IDLE, fr, 0);
        else record(K_GAME, fr, grab(g_sched_host, fr));
        if ((++tick & 3) == 0 && g_gx_gpu_host) record(K_GPU, fr, grab(g_gx_gpu_host, fr));
        if ((tick & 3) == 2 && s_render) record(K_RENDER, fr, grab(s_render, fr));
        if (armTicksToNs(armGetSystemTick() - last_log) > 60000000000ull) {
            last_log = armGetSystemTick();
            rt_log("prof: %llu game, %llu idle, %llu decoder, %llu render samples, %llu failed",
                   (unsigned long long)s_samples[K_GAME], (unsigned long long)s_samples[K_IDLE],
                   (unsigned long long)s_samples[K_GPU], (unsigned long long)s_samples[K_RENDER],
                   (unsigned long long)s_samples[K_FAIL]);
        }
    }
}

void prof_start(const char *path, u64 image_base, u64 image_end) {
    s_f = fopen(path, "wb");
    if (!s_f) { rt_log("prof: cannot write %s", path); return; }
    pmutex_init(&s_flock);
    s_base = image_base;
    s_end = image_end;
    s_t0 = armTicksToNs(armGetSystemTick());
    s_render = envGetMainThreadHandle(); /* prof_start runs on the main thread */
    /* header: magic, version, image base / end, build stamp */
    u32 hdr[6] = {0x31504B4Du /* "MKP1" */, 1, (u32)image_base, (u32)(image_base >> 32), (u32)image_end, (u32)(image_end >> 32)};
    fwrite(hdr, 4, 6, s_f);
    char stamp[32];
    snprintf(stamp, sizeof stamp, "%s", g_build_stamp); /* main_switch.c's: rebuilt every build */
    fwrite(stamp, 1, sizeof stamp, s_f);
    fflush(s_f);
    /* core 2 (shared with the decoder), above it in priority so the ticks stay regular */
    pthread_start("mkdd-prof", sampler, NULL, 64 << 10, 2, -3);
    rt_log("prof: sampling the game thread every %d ms to %s", PERIOD_NS / 1000000, path);
}

void prof_stop(void) {
    if (!s_f) return;
    s_stop = true;
    svcSleepThread(3 * PERIOD_NS);
    pmutex_lock(&s_flock);
    flush_locked();
    fclose(s_f);
    s_f = NULL;
    pmutex_unlock(&s_flock);
}
