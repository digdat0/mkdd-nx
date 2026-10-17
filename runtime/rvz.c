/* RVZ disc images (Dolphin's compressed format), read in place. Written from Dolphin's format
 * description (docs/WiaAndRvz.md); tools/rvz.py is the same reader in Python.
 *
 * A GameCube RVZ holds the disc as "raw data" split into groups of chunk_size bytes, each stored
 * compressed (Zstandard by default; bzip2, LZMA, LZMA2 or none). A group may also be "packed":
 * runs of the disc's pseudo-random filler are stored as the 17-word seed of the lagged Fibonacci
 * generator that made them. Decompressed groups are kept in a small cache, since the game reads
 * files in order. Called by disc.c with its lock held. */
#include <stdlib.h>
#include <string.h>
#include <zstd.h>
#ifdef MKDD_RVZ_XZ
#include <bzlib.h>
#include <lzma.h>
#endif
#include "rt.h"

enum { M_NONE, M_PURGE, M_BZIP2, M_LZMA, M_LZMA2, M_ZSTD };
#define BLOCK 0x8000u   /* raw data starts are aligned down to this; filler restarts every block */
#define LFG_K 521
#define LFG_J 32
#define SEED_WORDS 17
#define SLOTS 8         /* decompressed groups kept */

typedef struct { uint64_t off, size; uint32_t group, ngroups; } RawEntry;
typedef struct { uint64_t file_off; uint32_t size; bool compressed; uint32_t packed; } Group;

static FILE *s_f;
static uint32_t s_method, s_chunk;
static uint8_t s_dhead[0x80], s_props[7], s_nprops;
static RawEntry *s_raw;
static uint32_t s_nraw;
static Group *s_groups;
static uint32_t s_ngroups;
static uint8_t *s_in, *s_unpack; /* stored group bytes; decompressed packed stream */
static size_t s_in_cap, s_unpack_cap;
static struct { uint32_t group; uint64_t used; uint8_t *data; } s_slot[SLOTS];
static uint64_t s_clock;
static ZSTD_DCtx *s_zstd;
static unsigned s_errors;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

static bool file_read(uint64_t off, void *out, size_t n) {
    return fseeko(s_f, (off_t)off, SEEK_SET) == 0 && fread(out, 1, n, s_f) == n;
}

/* Decompress in (n bytes) into exactly out_n bytes of out. */
static bool decompress(const uint8_t *in, size_t n, uint8_t *out, size_t out_n) {
    switch (s_method) {
    case M_NONE:
        if (n < out_n) return false;
        memcpy(out, in, out_n);
        return true;
    case M_ZSTD: {
        size_t r = ZSTD_decompressDCtx(s_zstd, out, out_n, in, n);
        return !ZSTD_isError(r) && r == out_n;
    }
#ifdef MKDD_RVZ_XZ
    case M_BZIP2: {
        unsigned int len = (unsigned int)out_n;
        return BZ2_bzBuffToBuffDecompress((char *)out, &len, (char *)in, (unsigned int)n, 0, 0) == BZ_OK && len == out_n;
    }
    case M_LZMA:
    case M_LZMA2: {
        lzma_filter f[2] = {{.id = s_method == M_LZMA ? LZMA_FILTER_LZMA1 : LZMA_FILTER_LZMA2}, {.id = LZMA_VLI_UNKNOWN}};
        if (lzma_properties_decode(&f[0], NULL, s_props, s_nprops) != LZMA_OK) return false;
        lzma_stream z = LZMA_STREAM_INIT;
        bool ok = lzma_raw_decoder(&z, f) == LZMA_OK;
        free(f[0].options);
        if (ok) {
            z.next_in = in; z.avail_in = n; z.next_out = out; z.avail_out = out_n;
            lzma_ret r;
            do r = lzma_code(&z, LZMA_RUN); while (r == LZMA_OK && z.avail_out && z.avail_in);
            ok = (r == LZMA_OK || r == LZMA_STREAM_END) && z.avail_out == 0;
        }
        lzma_end(&z);
        return ok;
    }
#endif
    default:
        return false;
    }
}

/* ---- the disc filler generator ---- */
typedef struct { uint32_t w[LFG_K]; size_t pos; } Lfg; /* w: big-endian output words, pos: byte in them */

static void lfg_step(Lfg *g) {
    for (int i = 0; i < LFG_J; i++) g->w[i] ^= g->w[i + LFG_K - LFG_J];
    for (int i = LFG_J; i < LFG_K; i++) g->w[i] ^= g->w[i - LFG_J];
}

static void lfg_seed(Lfg *g, const uint8_t *seed) {
    for (int i = 0; i < SEED_WORDS; i++) g->w[i] = be32(seed + i * 4);
    for (int i = SEED_WORDS; i < LFG_K; i++) g->w[i] = (g->w[i - 17] << 23) ^ (g->w[i - 16] >> 9) ^ g->w[i - 1];
    for (int i = 0; i < LFG_K; i++) g->w[i] = (g->w[i] & 0xFF00FFFFu) | ((g->w[i] >> 2) & 0x00FF0000u);
    for (int i = 0; i < 4; i++) lfg_step(g);
    g->pos = 0;
}

static void lfg_skip(Lfg *g, size_t n) {
    g->pos += n;
    while (g->pos >= LFG_K * 4) { lfg_step(g); g->pos -= LFG_K * 4; }
}

static void lfg_take(Lfg *g, uint8_t *out, size_t n) {
    while (n) {
        size_t k = LFG_K * 4 - g->pos;
        if (k > n) k = n;
        for (size_t i = 0; i < k; i++) out[i] = (uint8_t)(g->w[(g->pos + i) / 4] >> (24 - 8 * ((g->pos + i) % 4)));
        g->pos += k; out += k; n -= k;
        if (g->pos == LFG_K * 4) { lfg_step(g); g->pos = 0; }
    }
}

/* Expand a packed group (in, n bytes) holding disc bytes [pos, pos + size) into out. */
static bool unpack(const uint8_t *in, size_t n, uint64_t pos, uint8_t *out, size_t size) {
    static Lfg g;
    size_t i = 0, o = 0;
    while (o < size) {
        if (i + 4 > n) return false;
        uint32_t len = be32(in + i);
        i += 4;
        bool junk = len & 0x80000000u;
        len &= 0x7FFFFFFFu;
        if (len > size - o) return false;
        if (junk) {
            if (i + SEED_WORDS * 4 > n) return false;
            lfg_seed(&g, in + i);
            i += SEED_WORDS * 4;
            lfg_skip(&g, (size_t)(pos % BLOCK));
            lfg_take(&g, out + o, len);
        } else {
            if (i + len > n) return false;
            memcpy(out + o, in + i, len);
            i += len;
        }
        o += len;
        pos += len;
    }
    return true;
}

/* Group gi, holding disc bytes [start, start + size): a cache slot's data. */
static const uint8_t *group(uint32_t gi, uint64_t start, size_t size) {
    int victim = 0;
    for (int s = 0; s < SLOTS; s++) {
        if (s_slot[s].data && s_slot[s].group == gi) { s_slot[s].used = ++s_clock; return s_slot[s].data; }
        if (s_slot[s].used < s_slot[victim].used) victim = s;
    }
    uint8_t *out = s_slot[victim].data;
    if (!out) out = s_slot[victim].data = malloc(s_chunk);
    s_slot[victim].group = gi;
    s_slot[victim].used = ++s_clock;
    const Group *g = &s_groups[gi];
    bool ok = true;
    if (!g->size) memset(out, 0, size);
    else {
        size_t want = g->packed ? g->packed : size; /* bytes after decompression */
        if (g->size > s_in_cap) { s_in = realloc(s_in, g->size); s_in_cap = g->size; }
        uint8_t *dst = out;
        if (g->packed) {
            if (want > s_unpack_cap) { s_unpack = realloc(s_unpack, want); s_unpack_cap = want; }
            dst = s_unpack;
        }
        ok = file_read(g->file_off, s_in, g->size);
        if (ok) {
            if (g->compressed) ok = decompress(s_in, g->size, dst, want);
            else if (g->size >= want) memcpy(dst, s_in, want);
            else ok = false;
        }
        if (ok && g->packed) ok = unpack(s_unpack, want, start, out, size);
    }
    if (!ok) {
        memset(out, 0, size);
        if (s_errors++ < 8) rt_log("rvz: group %u (disc %#llx) is damaged, read as zeros", gi, (unsigned long long)start);
    }
    return out;
}

/* Read a table stored compressed at off (n bytes) into a new out_n-byte buffer. */
static uint8_t *table(uint64_t off, uint32_t n, size_t out_n) {
    uint8_t *in = malloc(n ? n : 1), *out = malloc(out_n ? out_n : 1);
    if (!file_read(off, in, n) || !decompress(in, n, out, out_n)) { free(out); out = NULL; }
    free(in);
    return out;
}

bool rvz_open(FILE *f, uint64_t *disc_size, char *err, size_t errlen) {
    uint8_t h[0x48 + 0xDC];
    s_f = f;
    if (!file_read(0, h, sizeof h) || memcmp(h, "RVZ\x01", 4)) { snprintf(err, errlen, "not an RVZ file"); return false; }
    const uint8_t *d = h + 0x48;
    *disc_size = be64(h + 0x24);
    s_method = be32(d + 4);
    s_chunk = be32(d + 0xC);
    memcpy(s_dhead, d + 0x10, 0x80);
    s_nraw = be32(d + 0xB4);
    uint64_t raw_off = be64(d + 0xB8);
    uint32_t raw_size = be32(d + 0xC0);
    s_ngroups = be32(d + 0xC4);
    uint64_t group_off = be64(d + 0xC8);
    uint32_t group_size = be32(d + 0xD0);
    s_nprops = d[0xD4] <= 7 ? d[0xD4] : 7;
    memcpy(s_props, d + 0xD5, s_nprops);
    if (be32(d) != 1) { snprintf(err, errlen, "this RVZ is not a GameCube disc"); return false; }
    bool known = s_method == M_NONE || s_method == M_ZSTD;
#ifdef MKDD_RVZ_XZ
    known |= s_method == M_BZIP2 || s_method == M_LZMA || s_method == M_LZMA2;
#endif
    if (!known) { snprintf(err, errlen, "RVZ compression %u is not supported: convert it again in Dolphin with Zstandard", s_method); return false; }
    if (s_chunk < BLOCK || s_chunk > (2u << 20) || s_nraw == 0 || s_nraw > 1024 || s_ngroups == 0 || s_ngroups > (1u << 20)) {
        snprintf(err, errlen, "RVZ header is damaged");
        return false;
    }
    s_zstd = ZSTD_createDCtx();
    uint8_t *raw = table(raw_off, raw_size, (size_t)s_nraw * 24), *grp = table(group_off, group_size, (size_t)s_ngroups * 12);
    if (!raw || !grp) { free(raw); free(grp); snprintf(err, errlen, "RVZ tables are damaged"); return false; }
    s_raw = malloc(s_nraw * sizeof *s_raw);
    for (uint32_t i = 0; i < s_nraw; i++) {
        const uint8_t *e = raw + i * 24;
        s_raw[i] = (RawEntry){be64(e), be64(e + 8), be32(e + 16), be32(e + 20)};
        if ((uint64_t)s_raw[i].group + s_raw[i].ngroups > s_ngroups) { snprintf(err, errlen, "RVZ tables are damaged"); return false; }
    }
    s_groups = malloc(s_ngroups * sizeof *s_groups);
    for (uint32_t i = 0; i < s_ngroups; i++) {
        const uint8_t *e = grp + i * 12;
        uint32_t sz = be32(e + 4);
        s_groups[i] = (Group){(uint64_t)be32(e) << 2, sz & 0x7FFFFFFFu, (sz & 0x80000000u) != 0, be32(e + 8)};
    }
    free(raw);
    free(grp);
    rt_log("disc: RVZ, %u groups of %u KB, compression %u", s_ngroups, s_chunk >> 10, s_method);
    return true;
}

void rvz_read(uint64_t off, uint8_t *out, size_t n) {
    memset(out, 0, n);
    uint64_t end = off + n;
    if (off < 0x80) memcpy(out, s_dhead + off, (size_t)((end < 0x80 ? end : 0x80) - off)); /* kept in the header */
    for (uint32_t r = 0; r < s_nraw; r++) {
        const RawEntry *e = &s_raw[r];
        uint64_t base = e->off - e->off % BLOCK, r_end = e->off + e->size;
        uint64_t lo = off > e->off ? off : e->off, hi = end < r_end ? end : r_end;
        if (lo >= hi) continue;
        for (uint64_t gi = (lo - base) / s_chunk; gi <= (hi - 1 - base) / s_chunk && gi < e->ngroups; gi++) {
            uint64_t gs = base + gi * s_chunk, gsize = r_end - gs < s_chunk ? r_end - gs : s_chunk;
            const uint8_t *data = group(e->group + (uint32_t)gi, gs, (size_t)gsize);
            uint64_t a = lo > gs ? lo : gs, b = hi < gs + gsize ? hi : gs + gsize;
            memcpy(out + (a - off), data + (a - gs), (size_t)(b - a));
        }
    }
}
