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
/* The threads started here (NULL once they returned), paused at exit (plat_pause_threads). */
static Thread *s_threads[128];
static int s_nthreads;
/* plat_pause_threads: 1 while threads started here end themselves at their next wait (t_slot:
 * their s_threads entry, -1 on threads not started here, which carry on) */
static volatile int s_quitting;
static __thread int t_slot = -1;
/* Every started thread's stack mapping, kept after it returns: a detached thread that finished
 * (the updater's worker) can leave its mirror mapped too. */
static struct { void *mirror, *mem; size_t size; } s_stacks[128];
/* Pause them, then unmap their stacks. libnx maps every thread stack (heap memory, here) at a
 * mirror address with svcMapMemory, which locks the heap pages until threadClose. The loader
 * (hbl) gets the heap back at exit and reads the next .nro into it: a paused thread's stack
 * still mapped inside that range made hbl abort (2001-0106, invalid memory state) when the
 * updater's RESTART NOW asked it to start the new 50 MB .nro. */
/* what the last plat_pause_threads did with the stacks (logged by the exit report) */
int g_plat_stacks, g_plat_unmap_ok, g_plat_unmap_fail, g_plat_unmap_gone;
unsigned g_plat_unmap_rc;
uint64_t g_plat_unmap_addr[3];
int g_plat_quit_count; /* threads that ended themselves at the last plat_pause_threads */
void plat_pause_threads(void) {
    /* threads waiting on a condition end themselves (pcond_wait_ns); then the rest are paused */
    int before = 0, after = 0, n = __atomic_load_n(&s_nthreads, __ATOMIC_ACQUIRE);
    if (n > 128) n = 128;
    for (int i = 0; i < n; i++) before += __atomic_load_n(&s_threads[i], __ATOMIC_ACQUIRE) != NULL;
    s_quitting = 1;
    svcSleepThread(250000000);
    s_quitting = 2; /* from here a thread that wakes just carries on until it's paused */
    svcSleepThread(1000000);
    for (int i = 0; i < n; i++) after += __atomic_load_n(&s_threads[i], __ATOMIC_ACQUIRE) != NULL;
    g_plat_quit_count = before - after;
    n = __atomic_load_n(&s_nthreads, __ATOMIC_ACQUIRE);
    if (n > 128) n = 128;
    for (int i = 0; i < n; i++) {
        Thread *t = __atomic_load_n(&s_threads[i], __ATOMIC_ACQUIRE);
        if (t) svcSetThreadActivity(t->handle, ThreadActivity_Paused);
    }
    svcSleepThread(20000000); /* a thread in the middle of a system call stops when it returns */
    /* every stack still mapped (a mapping already gone just fails: harmless) */
    int unmapped = 0;
    for (int i = 0; i < n; i++) {
        __atomic_store_n(&s_threads[i], NULL, __ATOMIC_RELEASE);
        if (!s_stacks[i].mirror || !s_stacks[i].mem || !s_stacks[i].size) continue;
        MemoryInfo mi;
        u32 pi;
        g_plat_stacks++;
        if (R_FAILED(svcQueryMemory(&mi, &pi, (u64)(uintptr_t)s_stacks[i].mirror)) || mi.type == MemType_Unmapped) { g_plat_unmap_gone++; continue; }
        /* the mapping covers whole pages: libnx keeps the requested size (0xf950 failed with 0xca01,
         * invalid size), so round it up */
        size_t sz = (s_stacks[i].size + 0xFFF) & ~(size_t)0xFFF;
        Result rc = svcUnmapMemory(s_stacks[i].mirror, s_stacks[i].mem, sz);
        if (R_SUCCEEDED(rc)) unmapped++;
        else if (!g_plat_unmap_fail++) {
            g_plat_unmap_rc = rc;
            g_plat_unmap_addr[0] = (uint64_t)(uintptr_t)s_stacks[i].mirror;
            g_plat_unmap_addr[1] = (uint64_t)(uintptr_t)s_stacks[i].mem;
            g_plat_unmap_addr[2] = s_stacks[i].size;
        }
        s_stacks[i].mirror = NULL;
    }
    g_plat_unmap_ok = unmapped;
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
#ifdef __SWITCH__
    /* A paused thread still counts against the process's thread limit after we hand back to the
     * loader, and hbmenu then couldn't start its own (2001-0132, LimitReached; more threads since
     * the LAN mode's). Ending it gives its thread back. */
    if (__builtin_expect(s_quitting == 1, 0) && t_slot >= 0) {
        if (t_slot < 128) __atomic_store_n(&s_threads[t_slot], NULL, __ATOMIC_RELEASE);
        pthread_mutex_unlock(m);
        pthread_exit(NULL);
    }
#endif
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
    int slot = __atomic_fetch_add(&s_nthreads, 1, __ATOMIC_ACQ_REL);
    t_slot = slot;
    if (slot < 128) {
        Thread *self = threadGetSelf();
        s_stacks[slot].mem = self->stack_mem;
        s_stacks[slot].size = self->stack_sz;
        s_stacks[slot].mirror = self->stack_mirror;
        __atomic_store_n(&s_threads[slot], self, __ATOMIC_RELEASE);
    }
    if (s.core >= 0) svcSetThreadCoreMask(CUR_THREAD_HANDLE, s.core, 1u << s.core);
    if (s.prio_bias) {
        s32 prio = 0x2C;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        svcSetThreadPriority(CUR_THREAD_HANDLE, (u32)(prio + s.prio_bias));
    }
#endif
    s.fn(s.arg);
#ifdef __SWITCH__
    if (slot < 128) __atomic_store_n(&s_threads[slot], NULL, __ATOMIC_RELEASE); /* libnx closes it now */
#endif
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
#ifdef __SWITCH__
    /* A boot once hung on the loading screen when a game thread failed to start (fine on the next
     * launch): the system can refuse a thread for a moment (its thread resources still held by the
     * last app). Try again for a second before giving up. */
    for (int tries = 0; r && tries < 20; tries++) {
        svcSleepThread(50000000);
        r = pthread_create(&t, &attr, thread_main, s);
    }
    if (r) fprintf(stderr, "thread %s: pthread_create failed (%d)\n", name, r);
#endif
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

/* The Switch's sockets, shared by whoever needs them (the updater, the LAN adapter, eth.c): the
 * first plat_net_up starts them, the last plat_net_down stops them (they hold transfer memory). */
#ifdef __SWITCH__
static PMutex s_net_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_net_users;

bool plat_net_up(void) {
    pmutex_lock(&s_net_lock);
    Result rc = s_net_users ? 0 : socketInitializeDefault();
    if (R_SUCCEEDED(rc)) s_net_users++;
    pmutex_unlock(&s_net_lock);
    if (R_FAILED(rc)) fprintf(stderr, "net: socketInitializeDefault %#x\n", rc);
    return R_SUCCEEDED(rc);
}

void plat_net_down(void) {
    pmutex_lock(&s_net_lock);
    if (s_net_users > 0 && --s_net_users == 0) socketExit();
    pmutex_unlock(&s_net_lock);
}

/* At exit, after plat_pause_threads: the sockets' transfer memory locks ~2 MB of heap, which the
 * loader takes back for the next .nro; RESTART NOW's 50 MB one ran into it (2001-0106). Without
 * the lock: a paused user (the updater's worker) may hold it. */
void plat_net_exit(void) {
    if (s_net_users > 0) socketExit();
    s_net_users = 0;
}
#else
bool plat_net_up(void) { return true; }
void plat_net_down(void) {}
void plat_net_exit(void) {}
#endif
