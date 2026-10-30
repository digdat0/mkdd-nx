/* Host platform layer: threads, locks, time, files. pthreads + clock_gettime, which both the
 * PC test build (MSYS2 gcc) and the Switch (libnx newlib) provide. */
#pragma once
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef pthread_mutex_t PMutex;
typedef pthread_cond_t PCond;

void pmutex_init(PMutex *m);
static inline void pmutex_lock(PMutex *m) { pthread_mutex_lock(m); }
static inline void pmutex_unlock(PMutex *m) { pthread_mutex_unlock(m); }
void pcond_init(PCond *c);
/* Wait at most ns nanoseconds. */
void pcond_wait_ns(PCond *c, PMutex *m, uint64_t ns);
#ifdef __SWITCH__
/* In slices, so that at exit (plat_pause_threads) a waiting thread ends itself: callers loop on
 * their condition anyway. */
static inline void pcond_wait(PCond *c, PMutex *m) { pcond_wait_ns(c, m, 100000000ull); }
#else
static inline void pcond_wait(PCond *c, PMutex *m) { pthread_cond_wait(c, m); }
#endif
static inline void pcond_broadcast(PCond *c) { pthread_cond_broadcast(c); }
static inline void pcond_signal(PCond *c) { pthread_cond_signal(c); }

typedef void (*ThreadFn)(void *arg);
/* Start a detached host thread. core: preferred CPU core (-1 = any); prio_bias: 0 normal,
 * negative = more important (Switch only). */
bool pthread_start(const char *name, ThreadFn fn, void *arg, size_t stack, int core, int prio_bias);

uint64_t plat_now_ns(void);          /* monotonic */
uint64_t plat_unix_seconds(void);    /* wall clock */
void plat_sleep_ns(uint64_t ns);
void plat_yield(void);
uint32_t plat_thread_handle(void); /* Switch: kernel handle of the calling thread (0 elsewhere) */
__attribute__((noreturn)) void plat_park_forever(void);
#ifdef __SWITCH__
void plat_pause_threads(void); /* stop every thread pthread_start made (exit) */
#endif
/* Sockets (Switch: socketInitializeDefault, counted: the last plat_net_down stops them; elsewhere
 * nothing to do). false: no network. */
bool plat_net_up(void);
void plat_net_down(void);
void plat_net_exit(void); /* at exit, the other threads paused: sockets closed whoever holds them */

/* Whole-file helpers; plat_read_file returns malloc'd data (NULL on failure). */
void *plat_read_file(const char *path, size_t *size);
bool plat_write_file_atomic(const char *path, const void *data, size_t size);
void plat_mkdirs_for(const char *path);
const char *plat_getenv(const char *name);
void *plat_aligned_alloc(size_t align, size_t size); /* never freed */

/* Growable byte/element arrays. */
#define VEC(T) struct { T *data; size_t len, cap; }
#define vec_reserve(v, n) do { \
    size_t _need = (v)->len + (n); \
    if (_need > (v)->cap) { \
        size_t _c = (v)->cap ? (v)->cap * 2 : 64; \
        while (_c < _need) _c *= 2; \
        (v)->data = realloc((v)->data, _c * sizeof *(v)->data); \
        (v)->cap = _c; \
    } } while (0)
#define vec_push(v, x) do { vec_reserve(v, 1); (v)->data[(v)->len++] = (x); } while (0)
#define vec_free(v) do { free((v)->data); (v)->data = NULL; (v)->len = (v)->cap = 0; } while (0)

/* Open-addressing u64 -> u64 hash map (key 0 is allowed; uses a separate "used" flag). */
typedef struct { uint64_t *keys, *vals; uint8_t *used; size_t cap, len; } U64Map;
bool u64map_get(const U64Map *m, uint64_t key, uint64_t *val);
void u64map_put(U64Map *m, uint64_t key, uint64_t val);
bool u64map_del(U64Map *m, uint64_t key);
void u64map_clear(U64Map *m);

static inline uint64_t fnv_bytes(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}
#define FNV_INIT 0xCBF29CE484222325ull
