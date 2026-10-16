#include "plat.h"
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <malloc.h>
#include <windows.h>
/* PC profiler (pc/prof_win.c) samples these. */
void *g_plat_thread_handles[128];
const char *g_plat_thread_names[128];
volatile int g_plat_nthreads;
#endif
#ifdef __SWITCH__
#include <switch.h>
/* Handles of the threads started here, paused at exit (plat_pause_threads). */
static Handle s_handles[128];
static int s_nhandles;
void plat_pause_threads(void) {
    int n = __atomic_load_n(&s_nhandles, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n && i < 128; i++) {
        Handle h = __atomic_load_n(&s_handles[i], __ATOMIC_ACQUIRE);
        if (h) svcSetThreadActivity(h, ThreadActivity_Paused);
    }
}
#endif

void pmutex_init(PMutex *m) { pthread_mutex_init(m, NULL); }
void pcond_init(PCond *c) { pthread_cond_init(c, NULL); }

void pcond_wait_ns(PCond *c, PMutex *m, uint64_t ns) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t t = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec + ns;
    ts.tv_sec = (time_t)(t / 1000000000ull);
    ts.tv_nsec = (long)(t % 1000000000ull);
    pthread_cond_timedwait(c, m, &ts);
}

typedef struct { ThreadFn fn; void *arg; int core; int prio_bias; const char *name; } Start;

static void *thread_main(void *p) {
    Start s = *(Start *)p;
    free(p);
#ifdef _WIN32
    HANDLE h;
    if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        int i = __atomic_fetch_add(&g_plat_nthreads, 1, __ATOMIC_ACQ_REL);
        if (i < 128) { g_plat_thread_names[i] = s.name; __atomic_store_n(&g_plat_thread_handles[i], h, __ATOMIC_RELEASE); }
    }
#endif
#ifdef __SWITCH__
    {
        int i = __atomic_fetch_add(&s_nhandles, 1, __ATOMIC_ACQ_REL);
        if (i < 128) __atomic_store_n(&s_handles[i], threadGetCurHandle(), __ATOMIC_RELEASE);
    }
    if (s.core >= 0) svcSetThreadCoreMask(CUR_THREAD_HANDLE, s.core, 1u << s.core);
    if (s.prio_bias) {
        s32 prio = 0x2C;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        svcSetThreadPriority(CUR_THREAD_HANDLE, (u32)(prio + s.prio_bias));
    }
#endif
    s.fn(s.arg);
    return NULL;
}

bool pthread_start(const char *name, ThreadFn fn, void *arg, size_t stack, int core, int prio_bias) {
    Start *s = malloc(sizeof *s);
    *s = (Start){fn, arg, core, prio_bias, name};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
#ifdef __SWITCH__
    /* Our own page-exact stack: given none, libnx allocates stack + TLS with an unaligned size
     * but maps (svcMapMemory) whole pages, so the start of the next heap block stays unmapped
     * for the thread's life and whatever malloc puts there later faults. */
    stack = (stack ? stack : 128u << 10) + 0xFFF;
    stack &= ~(size_t)0xFFF;
    void *mem = aligned_alloc(0x1000, stack);
    if (!mem) { free(s); pthread_attr_destroy(&attr); return false; }
    pthread_attr_setstack(&attr, mem, stack);
#else
    if (stack) pthread_attr_setstacksize(&attr, stack);
#endif
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    int r = pthread_create(&t, &attr, thread_main, s);
    pthread_attr_destroy(&attr);
#ifdef __SWITCH__
    if (r) { free(mem); free(s); return false; }
#else
    if (r) { free(s); return false; }
#endif
    return true;
}

uint64_t plat_now_ns(void) {
#ifdef __SWITCH__
    return armTicksToNs(armGetSystemTick());
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

uint64_t plat_unix_seconds(void) { return (uint64_t)time(NULL); }

void plat_sleep_ns(uint64_t ns) {
#ifdef __SWITCH__
    svcSleepThread((s64)ns);
#else
    struct timespec ts = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
#endif
}

uint32_t plat_thread_handle(void) {
#ifdef __SWITCH__
    return threadGetCurHandle();
#else
    return 0;
#endif
}

void plat_yield(void) {
#ifdef __SWITCH__
    svcSleepThread(0);
#else
    sched_yield();
#endif
}

void plat_park_forever(void) {
    for (;;) plat_sleep_ns(3600ull * 1000000000ull);
}

void *plat_read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *d = malloc(n > 0 ? (size_t)n : 1);
    if (n > 0 && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); fclose(f); return NULL; }
    fclose(f);
    if (size) *size = (size_t)n;
    return d;
}

void plat_mkdirs_for(const char *path) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (strcmp(buf, "sdmc:") != 0) {
#ifdef _WIN32
                mkdir(buf);
#else
                mkdir(buf, 0777);
#endif
            }
            *p = '/';
        }
    }
}

bool plat_write_file_atomic(const char *path, const void *data, size_t size) {
    char tmp[520];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    plat_mkdirs_for(path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, size, f) == size;
    ok = fclose(f) == 0 && ok;
    if (!ok) return false;
    remove(path); /* newlib/Windows rename doesn't replace */
    return rename(tmp, path) == 0;
}

const char *plat_getenv(const char *name) { return getenv(name); }

void *plat_aligned_alloc(size_t align, size_t size) {
#ifdef _WIN32
    return _aligned_malloc(size, align);
#else
    return aligned_alloc(align, size);
#endif
}

/* ---- U64Map ---- */
static size_t slot_of(uint64_t k, size_t cap) {
    k ^= k >> 33; k *= 0xff51afd7ed558ccdull; k ^= k >> 33;
    return (size_t)k & (cap - 1);
}

static void grow(U64Map *m) {
    U64Map n = {0};
    n.cap = m->cap ? m->cap * 2 : 64;
    n.keys = calloc(n.cap, 8); n.vals = calloc(n.cap, 8); n.used = calloc(n.cap, 1);
    for (size_t i = 0; i < m->cap; i++)
        if (m->used[i] == 1) u64map_put(&n, m->keys[i], m->vals[i]);
    free(m->keys); free(m->vals); free(m->used);
    *m = n;
}

bool u64map_get(const U64Map *m, uint64_t key, uint64_t *val) {
    if (!m->cap) return false;
    for (size_t i = slot_of(key, m->cap);; i = (i + 1) & (m->cap - 1)) {
        if (m->used[i] == 0) return false;
        if (m->used[i] == 1 && m->keys[i] == key) { if (val) *val = m->vals[i]; return true; }
    }
}

void u64map_put(U64Map *m, uint64_t key, uint64_t val) {
    if ((m->len + 1) * 4 >= m->cap * 3) grow(m);
    size_t tomb = (size_t)-1;
    for (size_t i = slot_of(key, m->cap);; i = (i + 1) & (m->cap - 1)) {
        if (m->used[i] == 1 && m->keys[i] == key) { m->vals[i] = val; return; }
        if (m->used[i] == 2 && tomb == (size_t)-1) tomb = i;
        if (m->used[i] == 0) {
            size_t s = tomb != (size_t)-1 ? tomb : i;
            if (tomb == (size_t)-1) m->len++;
            m->used[s] = 1; m->keys[s] = key; m->vals[s] = val;
            return;
        }
    }
}

bool u64map_del(U64Map *m, uint64_t key) {
    if (!m->cap) return false;
    for (size_t i = slot_of(key, m->cap);; i = (i + 1) & (m->cap - 1)) {
        if (m->used[i] == 0) return false;
        if (m->used[i] == 1 && m->keys[i] == key) { m->used[i] = 2; return true; }
    }
}

void u64map_clear(U64Map *m) {
    if (m->cap) memset(m->used, 0, m->cap);
    m->len = 0;
}
