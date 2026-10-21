/* Mod install: turn a mod as its makers ship it into the mods/<key> folder disc_overlay plays.
 *
 * A mod comes as an xdelta3 patch (VCDIFF, RFC 3284, with xdelta3's LZMA secondary compression and
 * per-window Adler-32) of the clean disc, or as a disc image someone already patched. Either way
 * the patched disc streams through once, front to back, and is split as it goes: sys/ (boot.bin,
 * bi2.bin, apploader.img, main.dol, fst.bin) and files/, leaving out every file that is the same
 * as the clean disc's (disc_overlay takes those from the image). The source of a patch is the open
 * disc image through disc_read, so a clean .rvz works as well as an .iso.
 *
 * Which mod it is comes from the patched main.dol's SHA-1, matched against the mods this build
 * carries code for (table.h MODS): mod_identify decodes just far enough to see it. */
#include <dirent.h>
#include <lzma.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "rt.h"
#include "table.h"

#define BE32(p) ((uint32_t)(p)[0] << 24 | (uint32_t)(p)[1] << 16 | (uint32_t)(p)[2] << 8 | (uint32_t)(p)[3])
#define PRE_MAX (64u << 20)  /* the disc's start, up to the end of fst.bin and main.dol */
#define WIN_MAX (256u << 20) /* a window's target, source segment or delta */
#define CHUNK (1u << 20)

/* ---- splitter: the patched disc, in order, into sys/ and files/ ---- */
typedef struct { uint64_t off, base_off; uint32_t size; bool base; char *rel; } OutFile;
typedef struct {
    const char *dir; /* output folder; NULL: identify only (stops once main.dol is known) */
    uint8_t *pre;    /* the disc's start, until its layout is known */
    size_t pre_len;
    bool laid_out;
    unsigned mod;    /* MODS[mod - 1] has this main.dol; 0: none of them */
    OutFile *f;
    unsigned nf, cur;
    uint64_t pos;    /* disc bytes routed to files */
    FILE *out;       /* the current file, once it differs from the clean disc's */
    uint8_t *cmp;
    unsigned written, kept;
    char *err;
    size_t errlen;
} Split;

static bool fail(Split *s, const char *fmt, const char *arg) {
    if (!s->err[0]) snprintf(s->err, s->errlen, fmt, arg);
    return false;
}

static FILE *create(Split *s, const char *rel) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", s->dir, rel);
    plat_mkdirs_for(p);
    FILE *f = fopen(p, "wb");
    if (!f) fail(s, "could not create %s", p);
    return f;
}

static bool put_file(Split *s, const char *rel, const uint8_t *data, size_t n) {
    FILE *f = create(s, rel);
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    if (fclose(f) != 0) ok = false;
    return ok || fail(s, "could not write %s (is the SD card full?)", rel);
}

static int by_off(const void *a, const void *b) {
    const OutFile *x = a, *y = b;
    return x->off < y->off ? -1 : x->off > y->off;
}

/* How much of the disc's start the layout needs (more once main.dol's header is in). */
static size_t pre_need(const Split *s) {
    const uint8_t *p = s->pre;
    if (s->pre_len < 0x2460) return 0x2460;
    uint32_t dol_off = BE32(p + 0x420);
    uint64_t fst_end = (uint64_t)BE32(p + 0x424) + BE32(p + 0x428);
    uint64_t need = fst_end > dol_off + 0x100ull ? fst_end : dol_off + 0x100ull;
    if (s->pre_len >= (uint64_t)dol_off + 0x100) {
        uint32_t dol_size = 0x100;
        for (int i = 0; i < 18; i++) {
            uint32_t o = BE32(p + dol_off + 4 * i), n = BE32(p + dol_off + 0x90 + 4 * i);
            if (n && o + n > dol_size) dol_size = o + n;
        }
        if ((uint64_t)dol_off + dol_size > need) need = (uint64_t)dol_off + dol_size;
    }
    return need > PRE_MAX ? PRE_MAX + 1 : (size_t)need;
}

/* The disc's start is in s->pre: which mod it is; write sys/, list the files in disc order. */
static bool lay_out(Split *s) {
    const uint8_t *p = s->pre;
    if (BE32(p + 0x1C) != 0xC2339F3Du) return fail(s, "%s", "the patched disc is not a GameCube disc image");
    uint32_t dol_off = BE32(p + 0x420), fst_off = BE32(p + 0x424), fst_size = BE32(p + 0x428), dol_size = 0x100;
    for (int i = 0; i < 18; i++) {
        uint32_t o = BE32(p + dol_off + 4 * i), n = BE32(p + dol_off + 0x90 + 4 * i);
        if (n && o + n > dol_size) dol_size = o + n;
    }
    char sha[41];
    sha1_hex(p + dol_off, dol_size, sha);
    for (unsigned i = 0; i < MOD_COUNT && !s->mod; i++)
        if (!strcmp(sha, MODS[i].dol_sha1)) s->mod = i + 1;
    rt_log("install: patched main.dol SHA-1 %s: %s", sha, s->mod ? MODS[s->mod - 1].name : "no mod this build supports");
    s->laid_out = true;
    if (!s->dir) return true;
    if (!s->mod) return fail(s, "%s", "this mod (or this version of it) is not supported");
    uint32_t app = 0x2440 + 0x20 + BE32(p + 0x2440 + 0x14) + BE32(p + 0x2440 + 0x18);
    if (app > s->pre_len || fst_size < 12) return fail(s, "%s", "the patched disc is damaged");
    const uint8_t *fst = p + fst_off;
    uint32_t n = BE32(fst + 8);
    if ((uint64_t)n * 12 > fst_size) return fail(s, "%s", "the patched disc's file table is damaged");
    if (!put_file(s, "sys/boot.bin", p, 0x440) || !put_file(s, "sys/bi2.bin", p + 0x440, 0x2000) ||
        !put_file(s, "sys/apploader.img", p + 0x2440, app - 0x2440) || !put_file(s, "sys/main.dol", p + dol_off, dol_size) ||
        !put_file(s, "sys/fst.bin", fst, fst_size))
        return false;
    if (!(s->f = calloc(n, sizeof *s->f)) || !(s->cmp = malloc(CHUNK))) return fail(s, "%s", "out of memory");
    const char *names = (const char *)fst + n * 12;
    size_t names_len = fst_size - n * 12;
    uint32_t dir_end[64], dir_name[64];
    int depth = 0;
    for (uint32_t i = 1; i < n; i++) {
        const uint8_t *e = fst + i * 12;
        while (depth > 0 && i >= dir_end[depth - 1]) depth--;
        uint32_t no = BE32(e) & 0xFFFFFF;
        if (no >= names_len || !memchr(names + no, 0, names_len - no)) return fail(s, "%s", "the patched disc's file table is damaged");
        if (e[0]) { /* directory: its entries run to BE32(e + 8) */
            if (depth == 64 || BE32(e + 8) <= i || BE32(e + 8) > n) return fail(s, "%s", "the patched disc's file table is damaged");
            dir_name[depth] = no, dir_end[depth++] = BE32(e + 8);
            continue;
        }
        char rel[384];
        size_t k = 0;
        for (int d = 0; d < depth && k < sizeof rel; d++) k += (size_t)snprintf(rel + k, sizeof rel - k, "%s/", names + dir_name[d]);
        if (k >= sizeof rel) return fail(s, "%s", "a file name on the patched disc is too long");
        snprintf(rel + k, sizeof rel - k, "%s", names + no);
        OutFile *o = &s->f[s->nf++];
        o->off = BE32(e + 4), o->size = BE32(e + 8);
        uint32_t bsize;
        o->base = disc_find(rel, &o->base_off, &bsize) && bsize == o->size; /* same name and size: maybe the same file */
        char path[400];
        snprintf(path, sizeof path, "files/%s", rel);
        if (!(o->rel = strdup(path))) return fail(s, "%s", "out of memory");
        if (!o->size && !o->base) {
            if (!put_file(s, o->rel, NULL, 0)) return false;
            s->written++;
        }
    }
    qsort(s->f, s->nf, sizeof *s->f, by_off);
    for (unsigned i = 1; i < s->nf; i++)
        if (s->f[i].off < s->f[i - 1].off + s->f[i - 1].size) return fail(s, "%s", "the patched disc's files overlap");
    return true;
}

/* Bytes [in, + n) of file o: written once they differ from the clean disc's copy. */
static bool file_part(Split *s, OutFile *o, uint64_t in, const uint8_t *data, size_t n) {
    if (!s->out && o->base) {
        size_t same = 0;
        while (same < n) {
            size_t k = n - same < CHUNK ? n - same : CHUNK;
            disc_read(o->base_off + in + same, s->cmp, k);
            if (memcmp(s->cmp, data + same, k)) break;
            same += k;
        }
        if (same == n) return true;
        /* differs: the file from its start, the clean copy's bytes first */
        if (!(s->out = create(s, o->rel))) return false;
        for (uint64_t k = 0; k < in;) {
            size_t c = in - k < CHUNK ? (size_t)(in - k) : CHUNK;
            disc_read(o->base_off + k, s->cmp, c);
            if (fwrite(s->cmp, 1, c, s->out) != c) return fail(s, "could not write %s (is the SD card full?)", o->rel);
            k += c;
        }
    } else if (!s->out && !(s->out = create(s, o->rel))) return false;
    return fwrite(data, 1, n, s->out) == n || fail(s, "could not write %s (is the SD card full?)", o->rel);
}

static bool file_done(Split *s, OutFile *o) {
    if (!s->out) return s->kept++, true; /* the same as the clean disc's: read from there */
    bool ok = fclose(s->out) == 0;
    s->out = NULL;
    s->written++;
    return ok || fail(s, "could not write %s (is the SD card full?)", o->rel);
}

/* Disc bytes [s->pos, + n) into the files they belong to. */
static bool route(Split *s, const uint8_t *data, size_t n) {
    while (n) {
        while (s->cur < s->nf && s->f[s->cur].off + s->f[s->cur].size <= s->pos) s->cur++; /* empty files */
        if (s->cur == s->nf) { s->pos += n; return true; }
        OutFile *o = &s->f[s->cur];
        if (s->pos < o->off) { /* padding between files */
            size_t k = o->off - s->pos < n ? (size_t)(o->off - s->pos) : n;
            s->pos += k, data += k, n -= k;
            continue;
        }
        uint64_t in = s->pos - o->off;
        size_t k = o->size - in < n ? (size_t)(o->size - in) : n;
        if (!file_part(s, o, in, data, k)) return false;
        s->pos += k, data += k, n -= k;
        if (in + k == o->size) {
            if (!file_done(s, o)) return false;
            s->cur++;
        }
    }
    return true;
}

/* The patched disc's next n bytes. */
static bool split(Split *s, const uint8_t *data, size_t n) {
    while (!s->laid_out) {
        size_t need = pre_need(s);
        if (need > PRE_MAX) return fail(s, "%s", "the patched disc's layout is not supported");
        if (s->pre_len >= need) {
            if (!lay_out(s)) return false;
            if (!s->dir) return true;
            if (!route(s, s->pre, s->pre_len)) return false; /* a file may start before fst.bin ends */
            free(s->pre), s->pre = NULL;
            break;
        }
        if (!n) return true;
        size_t take = need - s->pre_len < n ? need - s->pre_len : n;
        uint8_t *g = realloc(s->pre, s->pre_len + take);
        if (!g) return fail(s, "%s", "out of memory");
        memcpy((s->pre = g) + s->pre_len, data, take);
        s->pre_len += take, data += take, n -= take;
    }
    return !n || !s->dir || route(s, data, n);
}

static void split_free(Split *s) {
    if (s->out) fclose(s->out);
    for (unsigned i = 0; i < s->nf; i++) free(s->f[i].rel);
    free(s->f), free(s->pre), free(s->cmp);
}

/* ---- VCDIFF (xdelta3) ---- */
static bool var_f(FILE *f, uint64_t *v) {
    uint64_t x = 0;
    for (int i = 0; i < 10; i++) {
        int c = fgetc(f);
        if (c < 0) return false;
        x = x << 7 | (c & 0x7F);
        if (!(c & 0x80)) return *v = x, true;
    }
    return false;
}

typedef struct { const uint8_t *p, *end; } Rd;
static bool var_m(Rd *r, uint64_t *v) {
    uint64_t x = 0;
    for (int i = 0; i < 10 && r->p < r->end; i++) {
        uint8_t c = *r->p++;
        x = x << 7 | (c & 0x7F);
        if (!(c & 0x80)) return *v = x, true;
    }
    return false;
}

/* The default code table (RFC 3284 5.6): per code, two (type, size, mode); type 0 none, 1 ADD, 2 RUN, 3 COPY. */
static uint8_t s_code[256][2][3];
static void code_table(void) {
    if (s_code[0][0][0]) return;
    int i = 0;
    s_code[i++][0][0] = 2; /* RUN 0 */
    for (int s = 0; s < 18; s++, i++) s_code[i][0][0] = 1, s_code[i][0][1] = (uint8_t)s;
    for (int m = 0; m < 9; m++) {
        s_code[i][0][0] = 3, s_code[i][0][2] = (uint8_t)m, i++;
        for (int s = 4; s < 19; s++, i++) s_code[i][0][0] = 3, s_code[i][0][1] = (uint8_t)s, s_code[i][0][2] = (uint8_t)m;
    }
    for (int m = 0; m < 6; m++)
        for (int a = 1; a < 5; a++)
            for (int c = 4; c < 7; c++, i++)
                s_code[i][0][0] = 1, s_code[i][0][1] = (uint8_t)a, s_code[i][1][0] = 3, s_code[i][1][1] = (uint8_t)c, s_code[i][1][2] = (uint8_t)m;
    for (int m = 6; m < 9; m++)
        for (int a = 1; a < 5; a++, i++)
            s_code[i][0][0] = 1, s_code[i][0][1] = (uint8_t)a, s_code[i][1][0] = 3, s_code[i][1][1] = 4, s_code[i][1][2] = (uint8_t)m;
    for (int m = 0; m < 9; m++, i++)
        s_code[i][0][0] = 3, s_code[i][0][1] = 4, s_code[i][0][2] = (uint8_t)m, s_code[i][1][0] = 1, s_code[i][1][1] = 1;
}

/* xdelta3's secondary LZMA: one xz stream per section kind (data, instructions, addresses) runs
 * through the whole patch, each window's section a flushed piece of it (only the first carries
 * the xz header). Input the decoder hasn't taken yet waits for the next piece. */
typedef struct { lzma_stream z; bool on; uint8_t *carry; size_t carry_n; } Lz;

/* A compressed section: its size, then the stream's next piece. */
static bool unsec(Lz *lz, const uint8_t *p, size_t n, uint8_t **out, size_t *out_n) {
    Rd r = {p, p + n};
    uint64_t size;
    if (!var_m(&r, &size) || size > WIN_MAX) return false;
    if (!lz->on) {
        lz->z = (lzma_stream)LZMA_STREAM_INIT;
        if (lzma_stream_decoder(&lz->z, UINT64_MAX, 0) != LZMA_OK) return false;
        lz->on = true;
    }
    size_t in_n = lz->carry_n + (size_t)(r.end - r.p);
    uint8_t *in = malloc(in_n ? in_n : 1), *o = malloc(size ? size : 1);
    bool ok = in && o;
    if (ok) {
        if (lz->carry_n) memcpy(in, lz->carry, lz->carry_n);
        memcpy(in + lz->carry_n, r.p, (size_t)(r.end - r.p));
        lz->z.next_in = in, lz->z.avail_in = in_n;
        lz->z.next_out = o, lz->z.avail_out = size;
        while (lz->z.avail_out) {
            size_t before = lz->z.avail_in + lz->z.avail_out;
            lzma_ret ret = lzma_code(&lz->z, LZMA_RUN);
            if ((ret != LZMA_OK && ret != LZMA_STREAM_END) || ret == LZMA_STREAM_END || lz->z.avail_in + lz->z.avail_out == before) break;
        }
        ok = !lz->z.avail_out;
    }
    if (ok) { /* what it didn't take */
        free(lz->carry);
        lz->carry_n = lz->z.avail_in;
        lz->carry = lz->carry_n ? malloc(lz->carry_n) : NULL;
        if (lz->carry_n && !lz->carry) ok = false;
        else if (lz->carry_n) memcpy(lz->carry, lz->z.next_in, lz->carry_n);
    }
    free(in);
    if (!ok) return free(o), false;
    *out = o, *out_n = size;
    return true;
}

static void lz_end(Lz *lz) {
    if (lz->on) lzma_end(&lz->z);
    free(lz->carry);
}

static uint32_t adler32(const uint8_t *p, size_t n) {
    uint32_t a = 1, b = 0;
    while (n) {
        size_t k = n < 5552 ? n : 5552;
        n -= k;
        while (k--) a += *p++, b += a;
        a %= 65521u, b %= 65521u;
    }
    return b << 16 | a;
}

/* Decode the patch in f (after its magic) against the open disc image into s. */
static bool vcdiff(FILE *f, uint64_t fsize, Split *s, ModProgress cb, void *ud) {
    code_table();
    int ind = fgetc(f);
    uint64_t len;
    if (ind < 0 || (ind & 2)) return fail(s, "%s", "the patch uses a format this version can't read (code table)");
    if ((ind & 1) && fgetc(f) != 2) return fail(s, "%s", "the patch uses a compression this version can't read");
    if (ind & 4) {
        if (!var_f(f, &len) || fseeko(f, (off_t)len, SEEK_CUR)) return fail(s, "%s", "the patch is damaged");
    }
    uint8_t *src = NULL, *tgt = NULL, *delta = NULL;
    uint64_t src_pos = 0, src_len = 0;
    Lz lz[3];
    memset(lz, 0, sizeof lz);
    bool ok = true;
    for (unsigned win = 0; ok; win++) {
        int wi = fgetc(f);
        if (wi < 0) break; /* the end */
        uint64_t seg_len = 0, seg_pos = 0, dlen, tlen, ldata, linst, laddr;
        if (wi & 2) { ok = fail(s, "%s", "the patch uses a format this version can't read (target windows)"); break; }
        if ((wi & 1) && (!var_f(f, &seg_len) || !var_f(f, &seg_pos) || seg_len > WIN_MAX)) { ok = fail(s, "%s", "the patch is damaged"); break; }
        if (!var_f(f, &dlen) || dlen > WIN_MAX) { ok = fail(s, "%s", "the patch is damaged"); break; }
        free(delta);
        if (!(delta = malloc(dlen ? dlen : 1)) || fread(delta, 1, dlen, f) != dlen) { ok = fail(s, "%s", "the patch is damaged or cut short"); break; }
        Rd r = {delta, delta + dlen};
        if (!var_m(&r, &tlen) || tlen > WIN_MAX || r.p >= r.end) { ok = fail(s, "%s", "the patch is damaged"); break; }
        uint8_t dind = *r.p++;
        if (!var_m(&r, &ldata) || !var_m(&r, &linst) || !var_m(&r, &laddr)) { ok = fail(s, "%s", "the patch is damaged"); break; }
        uint32_t want_adler = 0;
        if (wi & 4) {
            if (r.end - r.p < 4) { ok = fail(s, "%s", "the patch is damaged"); break; }
            want_adler = BE32(r.p), r.p += 4;
        }
        if ((uint64_t)(r.end - r.p) < ldata + linst + laddr) { ok = fail(s, "%s", "the patch is damaged"); break; }
        const uint8_t *sec[3] = {r.p, r.p + ldata, r.p + ldata + linst};
        size_t sec_n[3] = {ldata, linst, laddr};
        uint8_t *dec[3] = {NULL, NULL, NULL};
        for (int k = 0; k < 3 && ok; k++)
            if (dind & (1 << k)) {
                size_t n2;
                if (!unsec(&lz[k], sec[k], sec_n[k], &dec[k], &n2)) ok = fail(s, "%s", "the patch is damaged (LZMA)");
                else sec[k] = dec[k], sec_n[k] = n2;
            }
        if (ok && (wi & 1) && (seg_pos != src_pos || seg_len != src_len || !src)) {
            free(src);
            if (!(src = malloc(seg_len ? seg_len : 1))) ok = fail(s, "%s", "out of memory");
            else disc_read(seg_pos, src, seg_len), src_pos = seg_pos, src_len = seg_len;
        }
        if (ok) {
            free(tgt);
            if (!(tgt = malloc(tlen ? tlen : 1))) ok = fail(s, "%s", "out of memory");
        }
        /* the instructions */
        Rd d = {sec[0], sec[0] + sec_n[0]}, in = {sec[1], sec[1] + sec_n[1]}, ad = {sec[2], sec[2] + sec_n[2]};
        uint64_t near[4] = {0}, same[768] = {0}, t = 0;
        unsigned next = 0;
        uint64_t sl = (wi & 1) ? seg_len : 0;
        while (ok && in.p < in.end) {
            uint8_t code = *in.p++;
            for (int h = 0; h < 2 && ok; h++) {
                const uint8_t *op = s_code[code][h];
                if (!op[0]) continue;
                uint64_t size = op[1];
                if (!size && !var_m(&in, &size)) { ok = false; break; }
                if (size > tlen - t) { ok = false; break; }
                if (op[0] == 1) { /* ADD */
                    if ((uint64_t)(d.end - d.p) < size) { ok = false; break; }
                    memcpy(tgt + t, d.p, size), d.p += size;
                } else if (op[0] == 2) { /* RUN */
                    if (d.p >= d.end) { ok = false; break; }
                    memset(tgt + t, *d.p++, size);
                } else { /* COPY */
                    uint64_t here = sl + t, a = 0, v;
                    unsigned m = op[2];
                    if (m < 6) {
                        if (!var_m(&ad, &v)) { ok = false; break; }
                        a = m == 0 ? v : m == 1 ? here - v : near[m - 2] + v;
                    } else {
                        if (ad.p >= ad.end) { ok = false; break; }
                        a = same[(m - 6) * 256 + *ad.p++];
                    }
                    near[next] = a, next = (next + 1) & 3, same[a % 768] = a;
                    if (a >= here) { ok = false; break; }
                    if (a + size <= sl) memcpy(tgt + t, src + a, size);
                    else
                        for (uint64_t x = 0; x < size; x++) { /* may overlap what it writes */
                            uint64_t q = a + x;
                            tgt[t + x] = q < sl ? src[q] : tgt[q - sl];
                        }
                }
                t += size;
            }
        }
        for (int k = 0; k < 3; k++) free(dec[k]);
        if (!ok) { fail(s, "%s", "the patch is damaged (instructions)"); break; }
        if (t != tlen) { ok = fail(s, "%s", "the patch is damaged (window size)"); break; }
        if ((wi & 4) && adler32(tgt, tlen) != want_adler) {
            rt_log("install: window %u checksum %08x, the patch says %08x", win, adler32(tgt, tlen), want_adler);
            ok = fail(s, "%s", "the patch does not fit this disc image: it needs a clean USA (GM4E01) disc");
            break;
        }
        if (!split(s, tgt, tlen)) { ok = false; break; }
        if (s->laid_out && !s->dir) break; /* identify only */
        if (cb && !cb(ud, (uint64_t)ftello(f), fsize)) { ok = fail(s, "%s", "cancelled"); break; }
    }
    free(src), free(tgt), free(delta);
    for (int k = 0; k < 3; k++) lz_end(&lz[k]);
    return ok;
}

/* ---- whole files ---- */
static bool feed(const char *path, Split *s, ModProgress cb, void *ud) {
    FILE *f = fopen(path, "rb");
    if (!f) return fail(s, "could not open %s", path);
    fseeko(f, 0, SEEK_END);
    uint64_t size = (uint64_t)ftello(f);
    fseeko(f, 0, SEEK_SET);
    uint8_t magic[4] = {0};
    bool ok = fread(magic, 1, 4, f) == 4;
    if (ok && magic[0] == 0xD6 && magic[1] == 0xC3 && magic[2] == 0xC4 && magic[3] == 0)
        ok = vcdiff(f, size, s, cb, ud);
    else if (ok) { /* a patched disc image (.iso / .gcm) */
        uint8_t *buf = malloc(4 * CHUNK);
        fseeko(f, 0, SEEK_SET);
        ok = buf != NULL || fail(s, "%s", "out of memory");
        uint64_t done = 0;
        while (ok) {
            size_t n = fread(buf, 1, 4 * CHUNK, f);
            if (!n) break;
            ok = split(s, buf, n);
            done += n;
            if (s->laid_out && !s->dir) break;
            if (ok && cb && !cb(ud, done, size)) ok = fail(s, "%s", "cancelled");
        }
        free(buf);
    } else fail(s, "%s is empty", path);
    fclose(f);
    if (ok && !s->laid_out) ok = fail(s, "%s", "the patched disc is incomplete");
    while (s->dir && s->cur < s->nf && !s->f[s->cur].size) s->cur++;
    if (ok && s->dir && s->cur < s->nf) ok = fail(s, "%s", "the patched disc is cut short");
    return ok;
}

unsigned mod_identify(const char *path, char *err, size_t errlen) {
    Split s = {.err = err, .errlen = errlen};
    err[0] = 0;
    bool ok = feed(path, &s, NULL, NULL);
    split_free(&s);
    if (ok && !s.mod) snprintf(err, errlen, "not a mod (or a version of it) this build supports");
    return ok ? s.mod : 0;
}

static void remove_tree(const char *path) {
    DIR *d = opendir(path);
    if (!d) { remove(path); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[512];
        snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(p);
        else remove(p);
    }
    closedir(d);
    rmdir(path);
}

unsigned mod_install(const char *path, const char *out_dir, ModProgress cb, void *ud, char *err, size_t errlen) {
    char tmp[512], old[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", out_dir);
    snprintf(old, sizeof old, "%s.old", out_dir);
    remove_tree(tmp); /* an install that didn't finish */
    char probe[600];
    snprintf(probe, sizeof probe, "%s/sys", tmp);
    plat_mkdirs_for(probe);
    Split s = {.dir = tmp, .err = err, .errlen = errlen};
    err[0] = 0;
    uint64_t t0 = plat_now_ns();
    bool ok = feed(path, &s, cb, ud);
    unsigned written = s.written, kept = s.kept, mod = s.mod;
    split_free(&s);
    if (ok) { /* where it came from: the front end doesn't install it again */
        char p[600];
        snprintf(p, sizeof p, "%s/source.txt", tmp);
        FILE *f = fopen(p, "w");
        const char *base = strrchr(path, '/');
        struct stat st;
        ok = f && stat(path, &st) == 0 && fprintf(f, "%s\n%lld\n", base ? base + 1 : path, (long long)st.st_size) > 0;
        if (f && fclose(f) != 0) ok = false;
        if (!ok) fail(&s, "%s", "could not write source.txt (is the SD card full?)");
    }
    if (ok) {
        remove_tree(old);
        struct stat st;
        bool had = stat(out_dir, &st) == 0;
        ok = (!had || rename(out_dir, old) == 0) && rename(tmp, out_dir) == 0;
        if (ok) remove_tree(old);
        else fail(&s, "could not rename %s", tmp);
    }
    if (!ok) {
        rt_log("install: %s: %s", path, err);
        remove_tree(tmp);
        return 0;
    }
    rt_log("install: %s -> %s (%s): %u files written, %u the same as the disc's, %.0f s", path, out_dir,
           MODS[mod - 1].name, written, kept, (double)(plat_now_ns() - t0) / 1e9);
    return mod;
}
