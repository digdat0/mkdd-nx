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
    return true;
}

bool disc_present(void) { return s_file != NULL; }

/* Read n bytes at disc offset off. Missing CISO blocks and reads past the end are zero. */
void disc_read(uint64_t off, void *out, size_t n) {
    uint8_t *dst = out;
    if (!s_file) { memset(out, 0, n); return; }
    pmutex_lock(&s_lock);
    const uint64_t start = off;
    size_t done = s_rvz ? n : 0;
    if (s_rvz) rvz_read(off, dst, n); /* decompresses into its own cache, never straight into guest RAM */
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
    for (unsigned i = 0; i < s_npatch; i++)
        if (s_patch[i].off >= start && s_patch[i].off - start < n) dst[s_patch[i].off - start] = s_patch[i].b;
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
