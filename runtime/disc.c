/* Read-only access to the game disc image (.iso/.gcm, .ciso or .rvz). C port of gc-runtime's disc.rs. */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "rt.h"

#define CISO_HEADER 0x8000

static FILE *s_file;
static PMutex s_lock;
static uint64_t s_bs;          /* CISO block size, 0 for plain ISO */
static int64_t *s_map;         /* CISO block -> stored slot, -1 = missing */
static size_t s_nblocks;
static bool s_rvz;             /* RVZ image: reads go through rvz.c */
static void ahead_start(void);
/* Reads land here first, never straight in guest RAM: on Switch a large fread becomes an fs
 * IPC receive buffer, and the kernel unmaps its whole pages from the process until the read
 * returns, so the GX decoder thread faulted reading a display list in those pages. */
#define BOUNCE (256u << 10)
static uint8_t s_bounce[BOUNCE];
static struct { uint64_t off; uint8_t b; } s_patch[1024];
static unsigned s_npatch;

void disc_patch(uint64_t off, const void *bytes, size_t n) {
    for (size_t i = 0; i < n && s_npatch < 1024; i++) {
        s_patch[s_npatch].off = off + i;
        s_patch[s_npatch++].b = ((const uint8_t *)bytes)[i];
    }
}

bool disc_open(const char *path, char *err, size_t errlen) {
    if (s_file) return true; /* opened early for the loading screen's font */
    s_file = fopen(path, "rb");
    if (!s_file) { snprintf(err, errlen, "could not open disc image %s", path); return false; }
    setvbuf(s_file, NULL, _IOFBF, 64 << 10);
    pmutex_init(&s_lock);
    uint8_t *hdr = malloc(CISO_HEADER);
    if (fread(hdr, 1, 8, s_file) == 8 && !memcmp(hdr, "CISO", 4)) {
        if (fread(hdr + 8, 1, CISO_HEADER - 8, s_file) != CISO_HEADER - 8) {
            free(hdr);
            snprintf(err, errlen, "%s: truncated CISO header", path);
            return false;
        }
        s_bs = (uint64_t)hdr[4] | (uint64_t)hdr[5] << 8 | (uint64_t)hdr[6] << 16 | (uint64_t)hdr[7] << 24;
        s_nblocks = CISO_HEADER - 8;
        s_map = malloc(s_nblocks * sizeof *s_map);
        int64_t n = 0;
        for (size_t i = 0; i < s_nblocks; i++) s_map[i] = hdr[8 + i] ? n++ : -1;
    } else if (!memcmp(hdr, "RVZ", 4)) {
        uint64_t size;
        char why[160];
        if (!rvz_open(s_file, &size, why, sizeof why)) {
            free(hdr);
            fclose(s_file);
            s_file = NULL;
            snprintf(err, errlen, "%s: %s", path, why);
            return false;
        }
        s_rvz = true;
    }
    free(hdr);
    ahead_start();
    return true;
}

bool disc_present(void) { return s_file != NULL; }

/* Read n bytes at disc offset off into dst, lock held. Missing CISO blocks and reads past the
 * end are zero. */
static void raw_read(uint64_t off, uint8_t *dst, size_t n) {
    if (s_rvz) { rvz_read(off, dst, n); return; } /* decompresses into its own cache, never straight into guest RAM */
    size_t done = 0;
    while (done < n) {
        int64_t pos;
        size_t take;
        if (!s_bs) {
            pos = (int64_t)off;
            take = n - done;
        } else {
            uint64_t blk = off / s_bs, inner = off % s_bs;
            take = (size_t)(s_bs - inner);
            if (take > n - done) take = n - done;
            pos = blk < s_nblocks && s_map[blk] >= 0 ? CISO_HEADER + s_map[blk] * (int64_t)s_bs + (int64_t)inner : -1;
        }
        if (take > BOUNCE) take = BOUNCE;
        if (pos < 0 || fseeko(s_file, pos, SEEK_SET) != 0 || fread(s_bounce, 1, take, s_file) != take)
            memset(dst + done, 0, take);
        else
            memcpy(dst + done, s_bounce, take);
        done += take;
        off += take;
    }
}

/* ---- read-ahead ----
 * In a race the music streams from the disc in small reads at consecutive offsets (20 KB at a
 * time). After a small read, a worker on another core fetches the next AHEAD bytes (decompressing
 * them, for an RVZ) into a window, so the game thread's next reads are copies instead of SD card
 * reads. The worker reads in AHEAD_CHUNK pieces and drops the lock between them, so a game read
 * elsewhere waits for one piece at most. Large reads (scene loads) don't start it. */
#define AHEAD (512u << 10)
#define AHEAD_CHUNK (64u << 10)
#define NO_REQ UINT64_MAX
static struct { uint64_t off; size_t len; uint8_t *data; } s_win; /* ready data */
static uint8_t *s_stage;
static uint64_t s_want = NO_REQ, s_fetching = NO_REQ; /* requested start; start being fetched */
static PCond s_ahead_cv;
static bool s_ahead;
uint64_t g_disc_ahead_hits, g_disc_reads_direct;

static void ahead_worker(void *arg) {
    (void)arg;
    pmutex_lock(&s_lock);
    for (;;) {
        while (s_want == NO_REQ) pcond_wait(&s_ahead_cv, &s_lock);
        uint64_t start = s_want;
        s_want = NO_REQ;
        s_fetching = start;
        size_t got = 0;
        if (s_win.len && start >= s_win.off && start < s_win.off + s_win.len) { /* keep what the window already has */
            got = (size_t)(s_win.off + s_win.len - start);
            memcpy(s_stage, s_win.data + (start - s_win.off), got);
        }
        while (got < AHEAD) {
            size_t k = AHEAD - got < AHEAD_CHUNK ? AHEAD - got : AHEAD_CHUNK;
            raw_read(start + got, s_stage + got, k);
            got += k;
            pmutex_unlock(&s_lock); /* let a waiting game read in */
            pmutex_lock(&s_lock);
            if (s_want != NO_REQ && (s_want < start || s_want > start + got)) break; /* the stream moved elsewhere */
        }
        uint8_t *t = s_win.data;
        s_win.data = s_stage;
        s_stage = t;
        s_win.off = start;
        s_win.len = got;
        s_fetching = NO_REQ;
    }
}

static void ahead_start(void) {
    if (plat_getenv("MKDD_NO_AHEAD")) return; /* A/B timing */
    s_win.data = malloc(AHEAD);
    s_stage = malloc(AHEAD);
    pcond_init(&s_ahead_cv);
    s_ahead = s_win.data && s_stage && pthread_start("mkdd-disc", ahead_worker, NULL, 64 << 10, 0, 0);
}

/* Read n bytes at disc offset off. Missing CISO blocks and reads past the end are zero. */
void disc_read(uint64_t off, void *out, size_t n) {
    uint8_t *dst = out;
    if (!s_file) { memset(out, 0, n); return; }
    pmutex_lock(&s_lock);
    size_t done = 0;
    if (s_win.len && off >= s_win.off && off < s_win.off + s_win.len) {
        done = (size_t)(s_win.off + s_win.len - off);
        if (done > n) done = n;
        memcpy(dst, s_win.data + (off - s_win.off), done);
        g_disc_ahead_hits++;
    }
    if (done < n) {
        raw_read(off + done, dst + done, n - done);
        g_disc_reads_direct++;
    }
    uint64_t end = off + n;
    if (s_ahead && n <= AHEAD / 4) {
        bool in_win = s_win.len && end >= s_win.off && end <= s_win.off + s_win.len;
        uint64_t want = in_win ? s_win.off + s_win.len : end;
        bool enough = in_win && s_win.off + s_win.len - end >= AHEAD / 2;
        if (!enough && want != s_fetching && want != s_want) {
            s_want = want;
            pcond_signal(&s_ahead_cv);
        }
    }
    for (unsigned i = 0; i < s_npatch; i++)
        if (s_patch[i].off >= off && s_patch[i].off - off < n) dst[s_patch[i].off - off] = s_patch[i].b;
    pmutex_unlock(&s_lock);
}

uint32_t disc_read_u32(uint64_t off) {
    uint8_t b[4];
    disc_read(off, b, 4);
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

/* Offset and size of a file in the disc's file system table, by its path ("MRAM.arc",
 * "SceneData/back.arc"; case-insensitive). */
bool disc_find(const char *path, uint64_t *off, uint32_t *size) {
    if (!s_file) return false;
    uint32_t fst_off = disc_read_u32(0x424), fst_size = disc_read_u32(0x428);
    if (fst_size < 12 || fst_size > (16u << 20)) return false;
    uint8_t *fst = malloc(fst_size);
    disc_read(fst_off, fst, fst_size);
#define U32(p) ((uint32_t)(p)[0] << 24 | (uint32_t)(p)[1] << 16 | (uint32_t)(p)[2] << 8 | (p)[3])
    uint32_t n = U32(fst + 8);
    bool found = false;
    if ((uint64_t)n * 12 <= fst_size) {
        const char *names = (const char *)fst + n * 12;
        size_t names_len = fst_size - n * 12;
        uint32_t i = 1, end = n; /* the directory being searched: entries i..end-1 */
        const char *p = path;
        while (*p == '/') p++;
        while (*p && i < end) {
            const char *slash = strchr(p, '/');
            size_t len = slash ? (size_t)(slash - p) : strlen(p);
            bool hit = false;
            while (i < end) {
                const uint8_t *e = fst + i * 12;
                uint32_t no = U32(e) & 0xFFFFFF;
                bool dir = e[0] != 0;
                const char *nm = no < names_len ? names + no : "";
                if (strlen(nm) == len && !strncasecmp(nm, p, len)) {
                    if (slash && dir) { end = U32(e + 8); i++; hit = true; break; }
                    if (!slash && !dir) { *off = U32(e + 4); *size = U32(e + 8); found = true; hit = true; break; }
                }
                i = dir ? U32(e + 8) : i + 1; /* skip a whole sub-directory */
            }
            if (!hit || found) break;
            p = slash + 1;
        }
    }
#undef U32
    free(fst);
    return found;
}
