/* The Trick Style files the app carries (assets/trick.pak, tools/trickpak.py): on the first start of a new
 * pack (mods/trick/.pack holds the last one's CRC and the disc's game ID) they are written into the app
 * folder (mods/trick/..., mods/coins/...). Parts of the tracks and of the mode page's menu.arc are the game's own
 * data: the pack has none of those bytes, only copies from the player's disc (Yaz0 files decompressed), each
 * source checked by CRC; a file whose source differs on this disc is left out. Files are written uncompressed. */
#include "rt.h"
#include "plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

extern const uint8_t trick_pak[], trick_pak_end[];

#define MARK "mods/trick/.pack"

static uint32_t crc32_of(const uint8_t *p, size_t n) {
    static uint32_t t[256];
    if (!t[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
    uint32_t c = 0xFFFFFFFFu;
    while (n--) c = t[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* a disc file, Yaz0-decompressed; NULL when missing or damaged */
static uint8_t *disc_file(const char *path, uint32_t *size) {
    uint64_t off;
    uint32_t n;
    if (!disc_find(path, &off, &n) || n < 16 || n > (64u << 20)) return NULL;
    uint8_t *b = malloc(n);
    if (!b) return NULL;
    disc_read(off, b, n);
    if (memcmp(b, "Yaz0", 4)) return *size = n, b;
    uint32_t out_n = be32(b + 4);
    uint8_t *o = out_n && out_n <= (64u << 20) ? malloc(out_n) : NULL;
    uint32_t i = 16, w = 0;
    while (o && w < out_n) {
        if (i >= n) break;
        uint8_t code = b[i++];
        for (int k = 0; k < 8 && w < out_n; k++, code <<= 1) {
            if (code & 0x80) {
                if (i >= n) goto bad;
                o[w++] = b[i++];
            } else {
                if (i + 1 >= n) goto bad;
                uint32_t d = ((b[i] & 15u) << 8 | b[i + 1]) + 1, len = b[i] >> 4;
                i += 2;
                if (!len) {
                    if (i >= n) goto bad;
                    len = b[i++] + 18u;
                } else len += 2;
                if (d > w || w + len > out_n) goto bad;
                for (uint32_t j = 0; j < len; j++, w++) o[w] = o[w - d];
            }
        }
    }
    if (o && w == out_n) return free(b), *size = out_n, o;
bad:
    free(b), free(o);
    return NULL;
}

static bool same_file(const char *path, uint32_t size, uint32_t crc) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t *b = malloc(size + 1);
    size_t n = b ? fread(b, 1, size + 1, f) : 0;
    fclose(f);
    bool same = b && n == size && crc32_of(b, size) == crc;
    free(b);
    return same;
}

/* ---- the TRICK STYLE title, lettered here from the player's disc ----
 * The mode page's row title (menu.arc timg/entry_trick.bti) and the stage page's name (timg/mozi_trickbattle.bti,
 * trick.c puts it over mozi_battle1.bti) are IA4 256 x 32. The pack carries them blank; they are lettered with the
 * game's MarioFont (race2d.arc mariofont_*.bti, IA4 32 x 32, inside MRAM.arc: the letters ui_font.c draws the
 * menus with), so no picture of the game's is in the pack. Each letter is cut to its own width, set 1 apart (a
 * space 9), and the word is scaled to fit 248 x 32 and centred. */
#define TT_W 256
#define TT_H 32
#define TT_CELL 32
#define TT_CANVAS 640

/* a file anywhere in an uncompressed RARC in memory, by name (case-insensitive); NULL if not there */
static const uint8_t *rarc_get(const uint8_t *d, uint32_t n, const char *name, uint32_t *size) {
    if (n < 0x40 || memcmp(d, "RARC", 4)) return NULL;
    uint32_t data = 0x20 + be32(d + 0x0C), ne = be32(d + 0x28), eo = 0x20 + be32(d + 0x2C), ss = be32(d + 0x30),
             so = 0x20 + be32(d + 0x34);
    if (eo > n || ne > (n - eo) / 20 || so > n || ss > n - so) return NULL;
    for (uint32_t i = 0; i < ne; i++) {
        const uint8_t *e = d + eo + 20 * i;
        uint32_t no = (uint32_t)e[6] << 8 | e[7], off = be32(e + 8), sz = be32(e + 12);
        if (!(e[4] & 1) || no >= ss || !memchr(d + so + no, 0, ss - no)) continue;
        const char *s = (const char *)d + so + no;
        size_t k = 0;
        while (s[k] && name[k] && (s[k] | 0x20) == (name[k] | 0x20)) k++;
        if (s[k] || name[k]) continue;
        if (data > n || off > n - data || sz > n - data - off) return NULL;
        *size = sz;
        return d + data + off;
    }
    return NULL;
}

/* the IA4 picture's texels when bti is format 2, w x h; NULL otherwise */
static uint8_t *ia4_texels(uint8_t *bti, uint32_t n, unsigned w, unsigned h) {
    if (n < 0x20 || bti[0] != 2 || (unsigned)(bti[2] << 8 | bti[3]) != w || (unsigned)(bti[4] << 8 | bti[5]) != h) return NULL;
    uint32_t off = be32(bti + 0x1C);
    return off >= 0x20 && off <= n && n - off >= w * h ? bti + off : NULL; /* IA4: a byte a texel */
}

/* IA4 texels (8 x 4 blocks, a byte each: high nibble alpha, low intensity) <-> intensity and alpha planes (0..255) */
static void ia4_dec(const uint8_t *t, unsigned w, unsigned h, uint8_t *I, uint8_t *A) {
    for (unsigned by = 0, k = 0; by < h; by += 4)
        for (unsigned bx = 0; bx < w; bx += 8)
            for (unsigned y = 0; y < 4; y++)
                for (unsigned x = 0; x < 8; x++, k++) {
                    size_t p = (size_t)(by + y) * w + bx + x;
                    I[p] = (uint8_t)((t[k] & 15) * 17), A[p] = (uint8_t)((t[k] >> 4) * 17);
                }
}
static void ia4_enc(uint8_t *t, unsigned w, unsigned h, const float *I, const float *A) {
    for (unsigned by = 0, k = 0; by < h; by += 4)
        for (unsigned bx = 0; bx < w; bx += 8)
            for (unsigned y = 0; y < 4; y++)
                for (unsigned x = 0; x < 8; x++, k++) {
                    size_t p = (size_t)(by + y) * w + bx + x;
                    int i = (int)(I[p] * 15.0f / 255.0f + 0.5f), a = (int)(A[p] * 15.0f / 255.0f + 0.5f);
                    i = i < 0 ? 0 : i > 15 ? 15 : i, a = a < 0 ? 0 : a > 15 ? 15 : a;
                    t[k] = (uint8_t)(a << 4 | i);
                }
}

/* one axis of an area-average resize: n source samples (stride ss) -> m (stride ds), m <= n */
static void area_1d(const float *s, size_t ss, unsigned n, float *d, size_t ds, unsigned m) {
    float step = (float)n / (float)m;
    for (unsigned j = 0; j < m; j++) {
        float a = j * step, b = a + step, sum = 0;
        for (unsigned i = (unsigned)a; i < n && (float)i < b; i++) {
            float lo = (float)i > a ? (float)i : a, hi = (float)(i + 1) < b ? (float)(i + 1) : b;
            sum += s[i * ss] * (hi - lo);
        }
        d[j * ds] = sum / step;
    }
}

/* word lettered into IA4 TT_W x TT_H texels; false when the disc's letters can't be read */
static bool trick_title(const char *word, uint8_t *tex) {
    uint32_t mn, rn, gn;
    uint8_t *mram = disc_file("MRAM.arc", &mn);
    const uint8_t *r2 = mram ? rarc_get(mram, mn, "race2d.arc", &rn) : NULL;
    static float cI[TT_H][TT_CANVAS], cA[TT_H][TT_CANVAS];
    memset(cI, 0, sizeof cI), memset(cA, 0, sizeof cA);
    unsigned pen = 0;
    bool ok = r2 != NULL;
    for (const char *c = word; ok && *c; c++) {
        if (*c == ' ') { pen += 9; continue; }
        char name[32];
        snprintf(name, sizeof name, "mariofont_%c.bti", *c | 0x20);
        uint8_t gI[TT_CELL * TT_CELL], gA[TT_CELL * TT_CELL];
        const uint8_t *g = rarc_get(r2, rn, name, &gn);
        if (!g || gn > 0x10000u) { ok = false; break; }
        uint8_t *gb = malloc(gn);
        if (!gb) { ok = false; break; }
        memcpy(gb, g, gn);
        const uint8_t *t = ia4_texels(gb, gn, TT_CELL, TT_CELL);
        if (t) ia4_dec(t, TT_CELL, TT_CELL, gI, gA);
        free(gb);
        if (!t) { ok = false; break; }
        int x0 = TT_CELL, x1 = 0; /* the letter's own width (alpha over 40) */
        for (int y = 0; y < TT_CELL; y++)
            for (int x = 0; x < TT_CELL; x++)
                if (gA[y * TT_CELL + x] > 40) { if (x < x0) x0 = x; if (x + 1 > x1) x1 = x + 1; }
        if (x1 <= x0 || pen + (unsigned)(x1 - x0) > TT_CANVAS) { ok = false; break; }
        for (int y = 0; y < TT_CELL; y++)
            for (int x = x0; x < x1; x++) { /* overlapping edges: the lighter of the two */
                float i = gI[y * TT_CELL + x], a = gA[y * TT_CELL + x];
                float *pi = &cI[y][pen + (unsigned)(x - x0)], *pa = &cA[y][pen + (unsigned)(x - x0)];
                if (i > *pi) *pi = i;
                if (a > *pa) *pa = a;
            }
        pen += (unsigned)(x1 - x0) + 1;
    }
    free(mram);
    if (!ok) return false;
    unsigned bx0 = TT_CANVAS, bx1 = 0, by0 = TT_H, by1 = 0; /* the word's box (any alpha) */
    for (unsigned y = 0; y < TT_H; y++)
        for (unsigned x = 0; x < TT_CANVAS; x++)
            if (cA[y][x] > 0) {
                if (x < bx0) bx0 = x;
                if (x + 1 > bx1) bx1 = x + 1;
                if (y < by0) by0 = y;
                if (y + 1 > by1) by1 = y + 1;
            }
    if (bx1 <= bx0 || by1 <= by0) return false;
    unsigned bw = bx1 - bx0, bh = by1 - by0;
    float k = 248.0f / bw < 32.0f / bh ? 248.0f / bw : 32.0f / bh;
    if (k > 1) k = 1; /* (only ever smaller) */
    unsigned ow = (unsigned)(bw * k), oh = (unsigned)(bh * k);
    if (!ow || !oh || ow > TT_W || oh > TT_H) return false;
    static float hI[TT_H][TT_W], hA[TT_H][TT_W], oI[TT_W * TT_H], oA[TT_W * TT_H];
    for (unsigned y = 0; y < bh; y++) {
        area_1d(&cI[by0 + y][bx0], 1, bw, hI[y], 1, ow);
        area_1d(&cA[by0 + y][bx0], 1, bw, hA[y], 1, ow);
    }
    memset(oI, 0, sizeof oI), memset(oA, 0, sizeof oA);
    unsigned ox = (TT_W - ow) / 2, oy = (TT_H - oh) / 2;
    for (unsigned x = 0; x < ow; x++) {
        area_1d(&hI[0][x], TT_W, bh, &oI[oy * TT_W + ox + x], TT_W, oh);
        area_1d(&hA[0][x], TT_W, bh, &oA[oy * TT_W + ox + x], TT_W, oh);
    }
    ia4_enc(tex, TT_W, TT_H, oI, oA);
    return true;
}

/* menu.arc as rebuilt: its two blank titles lettered; true when both were */
static bool menu_titles(uint8_t *arc, uint32_t n) {
    static uint8_t tex[TT_W * TT_H];
    static int have; /* 0 not yet, 1 lettered, -1 the disc's letters can't be read */
    if (!have) have = trick_title("TRICK STYLE", tex) ? 1 : -1;
    if (have < 0) { rt_log("trickpak: MarioFont not found on the disc: the TRICK STYLE title stays blank"); return false; }
    static const char *const FILES[2] = {"entry_trick.bti", "mozi_trickbattle.bti"};
    int done = 0;
    for (int i = 0; i < 2; i++) {
        uint32_t fn;
        uint8_t *b = (uint8_t *)rarc_get(arc, n, FILES[i], &fn), *t = b ? ia4_texels(b, fn, TT_W, TT_H) : NULL;
        if (t) memcpy(t, tex, sizeof tex), done++;
        else rt_log("trickpak: menu.arc has no %ux%u IA4 %s", TT_W, TT_H, FILES[i]);
    }
    return done == 2;
}

void trickpak_install(void) {
    size_t pak_n = (size_t)(trick_pak_end - trick_pak);
    if (pak_n < 20 || memcmp(trick_pak, "MKTP", 4) || le32(trick_pak + 4) != 1 || !disc_present()) return;
    uint32_t raw_n = le32(trick_pak + 8), comp_n = le32(trick_pak + 12), pak_crc = le32(trick_pak + 16);
    char id[7] = {0}, want[32], have[32] = {0};
    disc_read(0, id, 6);
    snprintf(want, sizeof want, "%08x %s", (unsigned)pak_crc, id);
    FILE *m = fopen(MARK, "r");
    if (m) {
        if (!fgets(have, sizeof have, m)) have[0] = 0;
        fclose(m);
        have[strcspn(have, "\r\n")] = 0;
        if (!strcmp(have, want)) return;
    }
    uint8_t *raw = comp_n <= pak_n - 20 ? malloc(raw_n) : NULL;
    if (!raw || ZSTD_decompress(raw, raw_n, trick_pak + 20, comp_n) != raw_n || crc32_of(raw, raw_n) != pak_crc) {
        rt_log("trickpak: the built-in pack is damaged");
        free(raw);
        return;
    }
    const uint8_t *p = raw, *end = raw + raw_n;
#define NEED(k) \
    if ((size_t)(end - p) < (size_t)(k)) goto damaged
    NEED(4);
    uint32_t nfiles = le32(p), wrote = 0, kept = 0, skipped = 0, failed = 0;
    p += 4;
    for (uint32_t fi = 0; fi < nfiles; fi++) {
        char path[256], sp[256];
        uint8_t *src[8] = {0}, *out = NULL;
        uint32_t src_n[8] = {0};
        bool ok = true;
        NEED(2);
        unsigned pl = p[0] | p[1] << 8;
        p += 2;
        NEED(pl + 2);
        if (pl >= sizeof path) goto damaged;
        memcpy(path, p, pl), path[pl] = 0, p += pl;
        unsigned ns = p[0] | p[1] << 8;
        p += 2;
        if (ns > 8) goto damaged;
        for (unsigned s = 0; s < ns; s++) {
            NEED(2);
            unsigned l = p[0] | p[1] << 8;
            p += 2;
            NEED(l + 8);
            if (l >= sizeof sp) goto damaged;
            memcpy(sp, p, l), sp[l] = 0, p += l;
            uint32_t want_n = le32(p), want_crc = le32(p + 4);
            p += 8;
            if (!ok) continue;
            src[s] = disc_file(sp, &src_n[s]);
            if (!src[s] || src_n[s] != want_n || crc32_of(src[s], want_n) != want_crc) {
                rt_log("trickpak: %s left out: this disc's %s differs", path, sp);
                ok = false;
            }
        }
        NEED(12);
        uint32_t out_n = le32(p), out_crc = le32(p + 4), nops = le32(p + 8);
        p += 12;
        if (out_n > (64u << 20)) goto damaged_f;
        out = ok ? malloc(out_n ? out_n : 1) : NULL;
        if (ok && !out) ok = false;
        uint32_t w = 0;
        for (uint32_t k = 0; k < nops; k++) {
            NEED(1);
            uint8_t kind = *p++;
            if (!kind) {
                NEED(4);
                uint32_t l = le32(p);
                p += 4;
                NEED(l);
                if (ok) {
                    if (l > out_n - w) goto damaged_f;
                    memcpy(out + w, p, l), w += l;
                }
                p += l;
            } else {
                NEED(8);
                uint32_t s = kind - 1u, o = le32(p), l = le32(p + 4);
                p += 8;
                if (s >= ns) goto damaged_f;
                if (ok) {
                    if (o > src_n[s] || l > src_n[s] - o || l > out_n - w) goto damaged_f;
                    memcpy(out + w, src[s] + o, l), w += l;
                }
            }
        }
        if (ok && (w != out_n || crc32_of(out, out_n) != out_crc)) {
            rt_log("trickpak: %s did not rebuild (CRC)", path);
            ok = false;
        }
        uint32_t disk_crc = out_crc;
        if (ok && strlen(path) >= 26 && !strcmp(path + strlen(path) - 26, "SceneData/English/menu.arc") && menu_titles(out, out_n))
            disk_crc = crc32_of(out, out_n); /* the titles lettered (blank in the pack) */
        if (ok) {
            if (same_file(path, out_n, disk_crc)) kept++;
            else {
                char tmp[272];
                snprintf(tmp, sizeof tmp, "%s.tmp", path);
                plat_mkdirs_for(path);
                FILE *f = fopen(tmp, "wb");
                bool good = f && fwrite(out, 1, out_n, f) == out_n;
                if (f && fclose(f)) good = false;
                remove(path);
                if (good && !rename(tmp, path)) wrote++;
                else {
                    remove(tmp);
                    rt_log("trickpak: %s could not be written", path);
                    failed++;
                }
            }
        } else skipped++;
        for (unsigned s = 0; s < ns; s++) free(src[s]);
        free(out);
        continue;
    damaged_f:
        for (unsigned s = 0; s < ns; s++) free(src[s]);
        free(out);
        goto damaged;
    }
    rt_log("trickpak: %u files written, %u already there, %u left out (disc %s), %u could not be written", (unsigned)wrote,
           (unsigned)kept, (unsigned)skipped, id, (unsigned)failed);
    if (!failed) { /* done for this pack (a file this disc can't make counts as done; a failed write is tried again next start) */
        plat_mkdirs_for(MARK);
        if ((m = fopen(MARK, "w"))) fprintf(m, "%s\n", want), fclose(m);
    }
    free(raw);
    return;
damaged:
    rt_log("trickpak: the built-in pack is damaged");
    free(raw);
#undef NEED
}
