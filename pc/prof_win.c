/* PC-only sampling profiler: every millisecond, suspends each runtime thread, records its
 * instruction pointer (as an offset into mkdd_pc.exe; outside = waiting in the OS) and resumes it.
 * prof_dump writes "thread offset count" lines; pc/prof_report.py maps them to functions. */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <psapi.h>
#include <string.h>
#include "../runtime/rt.h"

extern void *g_plat_thread_handles[128];
extern const char *g_plat_thread_names[128];
extern volatile int g_plat_nthreads;
extern volatile uint32_t g_sleep_lr;

typedef struct { const char *name; uint64_t rva; uint64_t n; } Sample;
#define CAP (1u << 20)
static Sample *s_tab;
static volatile bool s_on;
static uint64_t s_base, s_cyc0[128], s_t0;

static void cycles(uint64_t *out) {
    for (int i = 0; i < 128 && i < g_plat_nthreads; i++) {
        ULONG64 c = 0;
        if (g_plat_thread_handles[i]) QueryThreadCycleTime(g_plat_thread_handles[i], &c);
        out[i] = c;
    }
}

/* interned strings (pointer identity is the hash key) */
static const char *strdup_cached(const char *k) {
    static char *tab[256];
    for (int i = 0; i < 256; i++) {
        if (!tab[i]) { tab[i] = strdup(k); return tab[i]; }
        if (!strcmp(tab[i], k)) return tab[i];
    }
    return "overflow";
}
static void record(const char *name, uint64_t rva) {
    uint64_t h = (rva * 0x9E3779B97F4A7C15ull) ^ (uint64_t)(uintptr_t)name;
    for (uint32_t i = (uint32_t)(h >> 40) & (CAP - 1);; i = (i + 1) & (CAP - 1)) {
        Sample *s = &s_tab[i];
        if (!s->n) { s->name = name; s->rva = rva; s->n = 1; return; }
        if (s->rva == rva && s->name == name) { s->n++; return; }
    }
}

static void sampler(void *arg) {
    (void)arg;
    timeBeginPeriod(1);
    for (;;) {
        Sleep(1);
        if (!s_on) continue;
        int n = g_plat_nthreads < 128 ? g_plat_nthreads : 128;
        for (int i = 0; i < n; i++) {
            HANDLE h = __atomic_load_n(&g_plat_thread_handles[i], __ATOMIC_ACQUIRE);
            const char *name = g_plat_thread_names[i];
            if (!h || !name || !strcmp(name, "prof")) continue;
            if (SuspendThread(h) == (DWORD)-1) continue;
            CONTEXT ctx;
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(h, &ctx)) {
                uint64_t rip = ctx.Rip;
                if (rip >= s_base && rip < s_base + 0x40000000ull) record(name, rip - s_base);
                else {
                    /* outside the exe: attribute to the DLL (+ its export-less offset) */
                    HMODULE m = NULL;
                    char mod[64] = "?";
                    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)rip, &m))
                        GetModuleBaseNameA(GetCurrentProcess(), m, mod, sizeof mod);
                    char key[96];
                    snprintf(key, sizeof key, "%s@%s", name, mod);
                    /* game thread blocked: which guest function it waits in */
                    if (!strcmp(name, "game")) record("game-wait", (uint64_t)(g_last_fn == 0x800ec0a0u ? g_sleep_lr : g_last_fn) | 0x4000000000000000ull);
                    record(strdup_cached(key), (rip - (uint64_t)(uintptr_t)m) | 0x8000000000000000ull);
                }
            }
            ResumeThread(h);
        }
    }
}

void prof_start(void) {
    s_tab = calloc(CAP, sizeof *s_tab);
    s_base = (uint64_t)(uintptr_t)GetModuleHandleA(NULL);
    pthread_start("prof", sampler, NULL, 64 << 10, -1, 0);
}
void prof_enable(bool on) { if (on) { cycles(s_cyc0); s_t0 = plat_now_ns(); } s_on = on; }
void prof_dump(const char *path) {
    s_on = false;
    Sleep(5);
    FILE *f = fopen(path, "w");
    if (!f) return;
    uint64_t c1[128] = {0};
    cycles(c1);
    double secs = (double)(plat_now_ns() - s_t0) / 1e9;
    for (int i = 0; i < 128 && i < g_plat_nthreads; i++)
        if (c1[i] > s_cyc0[i]) fprintf(stderr, "cpu: %-14s %7.1f Mcycles/s\n", g_plat_thread_names[i], (double)(c1[i] - s_cyc0[i]) / secs / 1e6);
    for (uint32_t i = 0; i < CAP; i++)
        if (s_tab[i].n) fprintf(f, "%s %llx %llu\n", s_tab[i].name, (unsigned long long)s_tab[i].rva, (unsigned long long)s_tab[i].n);
    fclose(f);
}
