/* Read-only access to the game disc image (.iso/.gcm, .ciso or .rvz). C port of gc-runtime's disc.rs. */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "rt.h"

#define CISO_HEADER 0x8000

static FILE *s_file;
static PMutex s_lock;
static uint64_t s_bs;          /* CISO block size, 0 for plain ISO */
static int64_t *s_map;         /* CISO block -> stored slot, -1 = missing */
static size_t s_nblocks;
static bool s_rvz;             /* RVZ image: reads go through rvz.c */
static void ahead_start(void);
static void raw_read(uint64_t off, uint8_t *dst, size_t n);
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

/* Read n bytes at image offset off into dst, lock held. Missing CISO blocks and reads past the
 * end are zero. */
static void img_read(uint64_t off, uint8_t *dst, size_t n) {
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
static unsigned s_layout; /* bumped when the overlay goes live: a fetch started before it is stale */
uint64_t g_disc_ahead_hits, g_disc_reads_direct;

static void ahead_worker(void *arg) {
    (void)arg;
    pmutex_lock(&s_lock);
    for (;;) {
        while (s_want == NO_REQ) pcond_wait(&s_ahead_cv, &s_lock);
        uint64_t start = s_want;
        s_want = NO_REQ;
        s_fetching = start;
        unsigned layout = s_layout;
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
        s_win.len = layout == s_layout ? got : 0;
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

/* A file's offset and size in a file system table (fst_size bytes), by path (case-insensitive). */
static bool find_in(const uint8_t *fst, uint32_t fst_size, const char *path, uint64_t *off, uint32_t *size) {
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
    return found;
}

/* ---- mod overlay ----
 * A mod extracted to a folder (<mod>/sys/main.dol, <mod>/sys/fst.bin, <mod>/files/...) is played
 * as a disc made up on the fly: the image's header with the mod's DOL and file table, every file
 * the mod's table lists taken from <mod>/files when it is there and from the image otherwise. The
 * game reads the table and its files by disc offset, so all of it is laid out at made-up offsets
 * (extents, sorted); nothing is copied or patched on the SD card. */
enum { X_IMAGE, X_FILE, X_MEM };
typedef struct { uint64_t voff, soff; uint32_t size; uint8_t kind; uint16_t file; const uint8_t *mem; } Extent;
static Extent *s_ext, *s_build; /* the live overlay; the one disc_overlay is laying out */
static unsigned s_next, s_nbuild;
static char **s_files;       /* paths of the mod's files, by Extent.file */
static uint8_t *s_seen;      /* files already read once (logged) */
static unsigned s_nfiles;
static uint8_t *s_ov_header, *s_ov_fst;
static char s_mod_name[128];
/* the last few mod files read, kept open (a stream reads one file in small pieces) */
#define OPEN_FILES 4
static struct { FILE *f; int file; unsigned used; } s_open[OPEN_FILES];
static unsigned s_open_tick;

static FILE *mod_file(int file) {
    unsigned lru = 0;
    for (unsigned i = 0; i < OPEN_FILES; i++) {
        if (s_open[i].f && s_open[i].file == file) { s_open[i].used = ++s_open_tick; return s_open[i].f; }
        if (s_open[i].used < s_open[lru].used) lru = i;
    }
    if (s_open[lru].f) fclose(s_open[lru].f);
    s_open[lru].f = fopen(s_files[file], "rb");
    s_open[lru].file = file;
    s_open[lru].used = ++s_open_tick;
    if (!s_open[lru].f) rt_log("mod: could not open %s", s_files[file]);
    else if (s_seen && !s_seen[file]) { /* once per file: which of the mod's files a session used */
        s_seen[file] = 1;
        rt_log("mod: reading %s", s_files[file]);
    }
    return s_open[lru].f;
}

static void raw_read(uint64_t off, uint8_t *dst, size_t n) {
    if (!s_ext) { img_read(off, dst, n); return; }
    /* the last extent starting at or before off */
    unsigned lo = 0, hi = s_next;
    while (hi - lo > 1) {
        unsigned mid = (lo + hi) / 2;
        if (s_ext[mid].voff <= off) lo = mid;
        else hi = mid;
    }
    while (n) {
        const Extent *e = lo < s_next ? &s_ext[lo] : NULL;
        size_t take;
        if (!e || off < e->voff) { /* before the first extent or in a gap: zero up to the next */
            take = e ? (size_t)(e->voff - off) : n;
            if (take > n) take = n;
            memset(dst, 0, take);
        } else if (off >= e->voff + e->size) {
            uint64_t next = lo + 1 < s_next ? s_ext[lo + 1].voff : UINT64_MAX;
            take = next - off < n ? (size_t)(next - off) : n;
            memset(dst, 0, take);
            lo++;
        } else {
            uint64_t in = off - e->voff;
            take = e->size - in < n ? (size_t)(e->size - in) : n;
            if (e->kind == X_IMAGE) img_read(e->soff + in, dst, take);
            else if (e->kind == X_MEM) memcpy(dst, e->mem + in, take);
            else {
                FILE *f = mod_file(e->file);
                for (size_t done = 0; done < take;) { /* through the bounce buffer, like img_read */
                    size_t k = take - done < BOUNCE ? take - done : BOUNCE;
                    if (!f || fseeko(f, (off_t)(e->soff + in + done), SEEK_SET) != 0 || fread(s_bounce, 1, k, f) != k)
                        memset(dst + done, 0, k);
                    else
                        memcpy(dst + done, s_bounce, k);
                    done += k;
                }
            }
            if (in + take == e->size) lo++;
        }
        dst += take;
        off += take;
        n -= take;
    }
}

#define BE32(p) ((uint32_t)(p)[0] << 24 | (uint32_t)(p)[1] << 16 | (uint32_t)(p)[2] << 8 | (p)[3])
static void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static long file_size(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) ? (long)st.st_size : -1;
}
static bool add_ext(unsigned *cap, Extent e) {
    if (s_nbuild == *cap) {
        Extent *n = realloc(s_build, (*cap = *cap ? *cap * 2 : 256) * sizeof *n);
        if (!n) return false;
        s_build = n;
    }
    s_build[s_nbuild++] = e;
    return true;
}
static void overlay_free(void) {
    for (unsigned i = 0; i < s_nfiles; i++) free(s_files[i]);
    free(s_files);
    free(s_seen);
    s_seen = NULL;
    free(s_build);
    free(s_ov_header);
    free(s_ov_fst);
    s_files = NULL;
    s_build = NULL;
    s_ov_header = s_ov_fst = NULL;
    s_nfiles = s_nbuild = 0;
}

#define OV_ALIGN 0x8000u
#define OV_DOL 0x20000u   /* past the image's boot header, BI2 and apploader */
bool disc_overlay(const char *mod_dir, char *err, size_t errlen) {
    if (!s_file) { snprintf(err, errlen, "no disc image open"); return false; }
    if (s_ext) return true;
    char path[512];
    snprintf(path, sizeof path, "%s/sys/main.dol", mod_dir);
    long dol_size = file_size(path);
    if (dol_size <= 0x100) { snprintf(err, errlen, "%s: no sys/main.dol", mod_dir); return false; }
    snprintf(path, sizeof path, "%s/sys/fst.bin", mod_dir);
    long fst_size = file_size(path);
    FILE *f = fst_size >= 12 && fst_size < (4l << 20) ? fopen(path, "rb") : NULL;
    if (!f) { snprintf(err, errlen, "%s: no sys/fst.bin", mod_dir); return false; }
    uint8_t *fst = malloc((size_t)fst_size);
    bool ok = fst && fread(fst, 1, (size_t)fst_size, f) == (size_t)fst_size;
    fclose(f);
    uint32_t n = ok ? BE32(fst + 8) : 0;
    if (!ok || (uint64_t)n * 12 > (uint64_t)fst_size) {
        free(fst);
        snprintf(err, errlen, "%s: sys/fst.bin is damaged", mod_dir);
        return false;
    }
    s_ov_fst = fst;
    uint8_t *image_fst = NULL;
    uint32_t image_fst_size = 0;
    s_files = calloc(n + 1, sizeof *s_files);
    s_seen = calloc(n + 1, 1);
    unsigned cap = 0;
    uint8_t *hdr = s_ov_header = malloc(0x440);
    if (!s_files || !hdr) goto oom;
    pmutex_lock(&s_lock); /* the read-ahead worker reads the image too */
    img_read(0, hdr, 0x440);
    image_fst_size = BE32(hdr + 0x428);
    if (image_fst_size >= 12 && image_fst_size < (16u << 20) && (image_fst = malloc(image_fst_size)))
        img_read(BE32(hdr + 0x424), image_fst, image_fst_size);
    pmutex_unlock(&s_lock);
    if (!image_fst) { snprintf(err, errlen, "the disc image's file table is damaged"); goto fail; }
    uint32_t fst_voff = (OV_DOL + (uint32_t)dol_size + OV_ALIGN - 1) & ~(OV_ALIGN - 1);
    put_be32(hdr + 0x420, OV_DOL);
    put_be32(hdr + 0x424, fst_voff);
    put_be32(hdr + 0x428, (uint32_t)fst_size);
    put_be32(hdr + 0x42C, (uint32_t)fst_size);
    if (!add_ext(&cap, (Extent){0, 0, 0x440, X_MEM, 0, hdr}) ||
        !add_ext(&cap, (Extent){0x440, 0x440, OV_DOL - 0x440, X_IMAGE, 0, NULL})) goto oom;
    snprintf(path, sizeof path, "%s/sys/main.dol", mod_dir);
    if (!(s_files[s_nfiles] = strdup(path)) ||
        !add_ext(&cap, (Extent){OV_DOL, 0, (uint32_t)dol_size, X_FILE, (uint16_t)s_nfiles++, NULL}) ||
        !add_ext(&cap, (Extent){fst_voff, 0, (uint32_t)fst_size, X_MEM, 0, fst})) goto oom;

    /* every file entry: the mod's copy, else the image's; laid out in table order */
    const char *names = (const char *)fst + n * 12;
    size_t names_len = (size_t)fst_size - n * 12;
    uint64_t voff = fst_voff + (uint64_t)fst_size;
    unsigned from_mod = 0, from_image = 0;
    uint32_t dir_end[64], dir_name[64];
    int depth = 0;
    for (uint32_t i = 1; i < n; i++) {
        uint8_t *e = fst + i * 12;
        while (depth > 0 && i >= dir_end[depth - 1]) depth--;
        uint32_t no = BE32(e) & 0xFFFFFF;
        if (no >= names_len) { snprintf(err, errlen, "%s: sys/fst.bin is damaged", mod_dir); goto fail; }
        if (e[0]) { /* directory: its entries run to BE32(e + 8) */
            if (depth == 64 || BE32(e + 8) <= i || BE32(e + 8) > n) { snprintf(err, errlen, "%s: sys/fst.bin is damaged", mod_dir); goto fail; }
            dir_name[depth] = no;
            dir_end[depth++] = BE32(e + 8);
            continue;
        }
        char rel[384];
        size_t k = 0;
        for (int d = 0; d < depth; d++) k += (size_t)snprintf(rel + k, sizeof rel - k, "%s/", names + dir_name[d]);
        snprintf(rel + (k < sizeof rel ? k : sizeof rel - 1), sizeof rel - (k < sizeof rel ? k : sizeof rel - 1), "%s", names + no);
        voff = (voff + OV_ALIGN - 1) & ~(uint64_t)(OV_ALIGN - 1);
        snprintf(path, sizeof path, "%s/files/%s", mod_dir, rel);
        long sz = file_size(path);
        Extent x = {voff, 0, 0, X_FILE, 0, NULL};
        if (sz >= 0) {
            /* the file's own size: a mod can update a file without its table (MKDD Plus 3.3's MRAM.arc) */
            if (s_nfiles >= 0xFFFF || !(s_files[s_nfiles] = strdup(path))) goto oom;
            x.size = (uint32_t)sz;
            x.file = (uint16_t)s_nfiles++;
            from_mod++;
        } else {
            uint64_t ioff;
            uint32_t isz;
            if (!find_in(image_fst, image_fst_size, rel, &ioff, &isz)) {
                snprintf(err, errlen, "%s: %s is in the mod's file table but neither in files/ nor on the disc", mod_dir, rel);
                goto fail;
            }
            x.kind = X_IMAGE;
            x.soff = ioff;
            x.size = isz;
            from_image++;
        }
        put_be32(e + 4, (uint32_t)voff);
        put_be32(e + 8, x.size);
        if (x.size && !add_ext(&cap, x)) goto oom;
        voff += x.size;
        if (voff > 0xFFFFFFFFull) { snprintf(err, errlen, "%s: the mod's files are over 4 GB", mod_dir); goto fail; }
    }
    const char *slash = strrchr(mod_dir, '/');
    snprintf(s_mod_name, sizeof s_mod_name, "%s", slash ? slash + 1 : mod_dir);
    free(image_fst);
    pmutex_lock(&s_lock); /* live from the next read on */
    s_ext = s_build;
    s_next = s_nbuild;
    s_win.len = 0; /* the read-ahead window holds image offsets */
    s_want = NO_REQ;
    s_layout++;
    pmutex_unlock(&s_lock);
    rt_log("mod: %s: %u files from the mod, %u from the disc image, %.0f MB disc", s_mod_name, from_mod, from_image,
           (double)voff / 1048576);
    return true;
oom:
    snprintf(err, errlen, "out of memory");
fail:
    free(image_fst);
    overlay_free();
    return false;
}

bool disc_overlay_active(void) { return s_ext != NULL; }
const char *disc_overlay_name(void) { return s_ext ? s_mod_name : NULL; }

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
    if (!fst) return false;
    disc_read(fst_off, fst, fst_size);
    bool found = find_in(fst, fst_size, path, off, size);
    free(fst);
    return found;
}
