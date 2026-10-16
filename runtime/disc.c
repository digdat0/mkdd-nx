/* Read-only access to the game disc image (.iso/.gcm or .ciso). C port of gc-runtime's disc.rs. */
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#define CISO_HEADER 0x8000

static FILE *s_file;
static PMutex s_lock;
static uint64_t s_bs;          /* CISO block size, 0 for plain ISO */
static int64_t *s_map;         /* CISO block -> stored slot, -1 = missing */
static size_t s_nblocks;
/* Reads land here first, never straight in guest RAM: on Switch a large fread becomes an fs
 * IPC receive buffer, and the kernel unmaps its whole pages from the process until the read
 * returns, so the GX decoder thread faulted reading a display list in those pages. */
#define BOUNCE (256u << 10)
static uint8_t s_bounce[BOUNCE];

bool disc_open(const char *path, char *err, size_t errlen) {
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
    pmutex_unlock(&s_lock);
}

uint32_t disc_read_u32(uint64_t off) {
    uint8_t b[4];
    disc_read(off, b, 4);
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}
