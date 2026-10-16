/* Native THP video frame decoder (HLE of the SDK's THPVideoDecode). C port of gc-runtime's
 * thp.rs.
 *
 * THP video frames are baseline JPEG (Huffman, 8-bit, usually 4:2:0) whose entropy-coded segment
 * is not byte-stuffed. The SDK decoder writes the Y, U and V planes as GX I8 textures (8x4-texel
 * tiles, 32 bytes each) with the usual +128 level shift. The recompiled SDK decoder works but
 * runs below real time, which starves the movie's audio; this does the same job natively. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

static const uint8_t ZIGZAG[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21,
    28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61,
    54, 47, 55, 62, 63,
};

/* Huffman table: codes up to LOOK bits through one table lookup (length << 8 | value, 0 = longer
 * code), longer ones through the canonical maxcode/valptr/mincode walk. */
#define LOOK 9
typedef struct { bool ok; int32_t maxcode[18], valptr[17], mincode[17]; uint8_t vals[256]; int nvals; uint16_t fast[1 << LOOK]; } Huff;

static void huff_build(Huff *h, const uint8_t bits[16], const uint8_t *vals, int total) {
    memset(h, 0, sizeof *h);
    for (int i = 0; i < 18; i++) h->maxcode[i] = -1;
    h->nvals = total > 256 ? 256 : total;
    memcpy(h->vals, vals, (size_t)h->nvals);
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        int n = bits[l - 1];
        if (n > 0) {
            h->valptr[l] = k;
            h->mincode[l] = code;
            for (int i = 0; i < n && l <= LOOK; i++) {
                int idx = k + i;
                if (idx >= h->nvals) break;
                int first = (code + i) << (LOOK - l);
                for (int j = 0; j < 1 << (LOOK - l); j++) h->fast[first + j] = (uint16_t)(l << 8 | h->vals[idx]);
            }
            code += n;
            k += n;
            h->maxcode[l] = code - 1;
        }
        code <<= 1;
    }
    h->maxcode[17] = INT32_MAX;
    h->ok = true;
}

/* MSB-first bit buffer: the next bit is acc's top bit, n valid bits. */
typedef struct { const uint8_t *d; size_t len, pos; uint64_t acc; int n; } Bits;

static inline void refill(Bits *b) {
    while (b->n <= 56) {
        uint64_t byte = b->pos < b->len ? b->d[b->pos] : 0;
        b->pos++;
        b->acc |= byte << (56 - b->n);
        b->n += 8;
    }
}
static inline int getbits(Bits *b, unsigned s) {
    if (b->n < (int)s) refill(b);
    int v = (int)(b->acc >> (64 - s));
    b->acc <<= s;
    b->n -= (int)s;
    return v;
}
static inline int decode(Bits *b, const Huff *h) {
    if (b->n < 16) refill(b);
    unsigned f = h->fast[b->acc >> (64 - LOOK)];
    if (f) {
        b->acc <<= f >> 8;
        b->n -= (int)(f >> 8);
        return (int)(f & 0xFF);
    }
    for (int l = LOOK + 1; l <= 16; l++) {
        int code = (int)(b->acc >> (64 - l));
        if (code <= h->maxcode[l]) {
            b->acc <<= l;
            b->n -= l;
            int i = h->valptr[l] + code - h->mincode[l];
            return i >= 0 && i < h->nvals ? h->vals[i] : -1;
        }
    }
    return -1;
}
/* Restart marker: drop the rest of the current byte and the bytes buffered ahead. */
static void byte_align(Bits *b) {
    b->pos -= (size_t)(b->n / 8);
    b->acc = 0;
    b->n = 0;
}
/* JPEG "receive + extend" of an s-bit magnitude. */
static inline int extend(Bits *b, unsigned s) {
    if (!s) return 0;
    int v = getbits(b, s);
    return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v;
}

/* Integer 8x8 IDCT (the IJG "islow" algorithm, 12-bit fixed-point constants, as in stb_image). */
#define F2F(x) ((int)((x) * 4096 + 0.5))
#define IDCT_1D(s0, s1, s2, s3, s4, s5, s6, s7)                                                   \
    int t0, t1, t2, t3, p1, p2, p3, p4, p5, x0, x1, x2, x3;                                       \
    p2 = s2;                                                                                      \
    p3 = s6;                                                                                      \
    p1 = (p2 + p3) * F2F(0.5411961f);                                                             \
    t2 = p1 + p3 * F2F(-1.847759065f);                                                            \
    t3 = p1 + p2 * F2F(0.765366865f);                                                             \
    p2 = s0;                                                                                      \
    p3 = s4;                                                                                      \
    t0 = (p2 + p3) * 4096;                                                                        \
    t1 = (p2 - p3) * 4096;                                                                        \
    x0 = t0 + t3;                                                                                 \
    x3 = t0 - t3;                                                                                 \
    x1 = t1 + t2;                                                                                 \
    x2 = t1 - t2;                                                                                 \
    t0 = s7;                                                                                      \
    t1 = s5;                                                                                      \
    t2 = s3;                                                                                      \
    t3 = s1;                                                                                      \
    p3 = t0 + t2;                                                                                 \
    p4 = t1 + t3;                                                                                 \
    p1 = t0 + t3;                                                                                 \
    p2 = t1 + t2;                                                                                 \
    p5 = (p3 + p4) * F2F(1.175875602f);                                                           \
    t0 = t0 * F2F(0.298631336f);                                                                  \
    t1 = t1 * F2F(2.053119869f);                                                                  \
    t2 = t2 * F2F(3.072711026f);                                                                  \
    t3 = t3 * F2F(1.501321110f);                                                                  \
    p1 = p5 + p1 * F2F(-0.899976223f);                                                            \
    p2 = p5 + p2 * F2F(-2.562915447f);                                                            \
    p3 = p3 * F2F(-1.961570560f);                                                                 \
    p4 = p4 * F2F(-0.390180644f);                                                                 \
    t3 += p1 + p4;                                                                                \
    t2 += p2 + p3;                                                                                \
    t1 += p2 + p4;                                                                                \
    t0 += p1 + p3;
static inline uint8_t clamp8(int x) { return (unsigned)x > 255 ? (x < 0 ? 0 : 255) : (uint8_t)x; }
/* blk: dequantised coefficients in natural order; out: 8x8 pixels with the +128 level shift. */
static void idct(const int blk[64], uint8_t out[64]) {
    int val[64];
    for (int i = 0; i < 8; i++) {
        const int *d = blk + i;
        int *v = val + i;
        if (!d[8] && !d[16] && !d[24] && !d[32] && !d[40] && !d[48] && !d[56]) {
            int dc = d[0] * 4;
            v[0] = v[8] = v[16] = v[24] = v[32] = v[40] = v[48] = v[56] = dc;
            continue;
        }
        IDCT_1D(d[0], d[8], d[16], d[24], d[32], d[40], d[48], d[56])
        x0 += 512; x1 += 512; x2 += 512; x3 += 512;
        v[0] = (x0 + t3) >> 10;
        v[56] = (x0 - t3) >> 10;
        v[8] = (x1 + t2) >> 10;
        v[48] = (x1 - t2) >> 10;
        v[16] = (x2 + t1) >> 10;
        v[40] = (x2 - t1) >> 10;
        v[24] = (x3 + t0) >> 10;
        v[32] = (x3 - t0) >> 10;
    }
    for (int i = 0; i < 8; i++) {
        const int *v = val + 8 * i;
        uint8_t *o = out + 8 * i;
        IDCT_1D(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7])
        x0 += 65536 + (128 << 17); x1 += 65536 + (128 << 17); x2 += 65536 + (128 << 17); x3 += 65536 + (128 << 17);
        o[0] = clamp8((x0 + t3) >> 17);
        o[7] = clamp8((x0 - t3) >> 17);
        o[1] = clamp8((x1 + t2) >> 17);
        o[6] = clamp8((x1 - t2) >> 17);
        o[2] = clamp8((x2 + t1) >> 17);
        o[5] = clamp8((x2 - t1) >> 17);
        o[3] = clamp8((x3 + t0) >> 17);
        o[4] = clamp8((x3 - t0) >> 17);
    }
}

typedef struct { int h, v, tq, td, ta, pred; } Comp;

volatile uint64_t g_thp_frames;
bool thp_decode(uint32_t file, const uint32_t dst[3]) {
    g_thp_frames++;
    int64_t start = ram_off(file);
    if (start < 0) return false;
    const uint8_t *d = g_ram + start;
    size_t len = RAM_SIZE - (size_t)start;
#define AT(p) ((p) < len ? d[p] : 0)
#define BE16(p) ((size_t)AT(p) << 8 | AT((p) + 1))
    static int qt[4][64];
    static Huff dc[4], ac[4];
    for (int i = 0; i < 4; i++) { dc[i].ok = false; ac[i].ok = false; }
    Comp comps[4];
    int ncomps = 0;
    size_t width = 0, height = 0, restart = 0, p = 0, scan_start = 0;
    for (;;) {
        if (p >= len || d[p] != 0xFF) return false;
        while (p < len && d[p] == 0xFF) p++;
        if (p >= len) return false;
        uint8_t marker = d[p++];
        if (marker == 0xD8) continue;
        if (marker == 0xC0) {
            size_t l = BE16(p);
            height = BE16(p + 3);
            width = BE16(p + 5);
            int n = AT(p + 7);
            if (n > 4) return false;
            ncomps = n;
            for (int i = 0; i < n; i++) {
                size_t c = p + 8 + 3 * (size_t)i;
                int hv = AT(c + 1);
                comps[i] = (Comp){hv >> 4, hv & 15, AT(c + 2) & 3, 0, 0, 0};
            }
            p += l;
        } else if (marker == 0xC4) {
            size_t l = BE16(p), end = p + l, q = p + 2;
            while (q < end) {
                uint8_t tc_th = AT(q);
                uint8_t bits[16];
                int total = 0;
                for (int i = 0; i < 16; i++) { bits[i] = AT(q + 1 + i); total += bits[i]; }
                if (q + 17 + (size_t)total > len) return false;
                Huff *h = (tc_th >> 4) == 0 ? &dc[tc_th & 3] : &ac[tc_th & 3];
                huff_build(h, bits, d + q + 17, total);
                q += 17 + (size_t)total;
            }
            p = end;
        } else if (marker == 0xDB) {
            size_t l = BE16(p), end = p + l, q = p + 2;
            while (q < end) {
                uint8_t pq_tq = AT(q);
                int id = pq_tq & 3;
                q++;
                for (int k = 0; k < 64; k++) qt[id][ZIGZAG[k]] = (pq_tq >> 4) == 0 ? (int)AT(q + k) : (int)BE16(q + 2 * k);
                q += (pq_tq >> 4) == 0 ? 64 : 128;
            }
            p = end;
        } else if (marker == 0xDD) {
            restart = BE16(p + 2);
            p += BE16(p);
        } else if (marker == 0xDA) {
            size_t l = BE16(p);
            int ns = AT(p + 2);
            if (ns != ncomps) return false; /* THP always scans all components in order */
            for (int i = 0; i < ns; i++) {
                uint8_t t = AT(p + 4 + 2 * (size_t)i);
                comps[i].td = (t >> 4) & 3;
                comps[i].ta = t & 3;
            }
            scan_start = p + l;
            break;
        } else if ((marker >= 0xE0 && marker <= 0xEF) || marker == 0xFE) {
            p += BE16(p);
        } else {
            return false;
        }
    }
    if (!width || !height || ncomps != 3 || width % 8) return false;
    int hmax = 0, vmax = 0;
    for (int i = 0; i < 3; i++) {
        if (comps[i].h > hmax) hmax = comps[i].h;
        if (comps[i].v > vmax) vmax = comps[i].v;
    }
    if (!hmax || !vmax) return false;
    size_t mcus_x = (width + 8 * (size_t)hmax - 1) / (8 * (size_t)hmax);
    size_t mcus_y = (height + 8 * (size_t)vmax - 1) / (8 * (size_t)vmax);
    size_t poff[3], pw[3], ph[3];
    for (int i = 0; i < 3; i++) {
        pw[i] = width * (size_t)comps[i].h / (size_t)hmax;
        ph[i] = height * (size_t)comps[i].v / (size_t)vmax;
        int64_t o = ram_off(dst[i]);
        if (o < 0 || pw[i] % 8 || (size_t)o + pw[i] * ph[i] > RAM_SIZE) return false;
        poff[i] = (size_t)o;
        if (!dc[comps[i].td].ok || !ac[comps[i].ta].ok) return false;
    }
    /* Decode straight into the I8 tiles. The scan reads the frame in RAM and writes the planes,
     * which never overlap the frame data in practice, but decode fully before writing to stay
     * safe: blocks go to a scratch list first, as in the Rust version. */
    size_t nblocks = 0;
    for (int i = 0; i < 3; i++) nblocks += (size_t)comps[i].h * (size_t)comps[i].v;
    nblocks *= mcus_x * mcus_y;
    typedef struct { uint8_t ci; uint16_t bx, by; uint8_t px[64]; } Blk;
    Blk *out = malloc(nblocks * sizeof *out);
    if (!out) return false;
    size_t nout = 0, mcu = 0;
    Bits bits = {d, len, scan_start, 0, 0};
    int blk[64];
    bool ok = true;
    for (size_t my = 0; my < mcus_y && ok; my++)
        for (size_t mx = 0; mx < mcus_x && ok; mx++) {
            if (restart && mcu && mcu % restart == 0) {
                byte_align(&bits);
                if (AT(bits.pos) == 0xFF && AT(bits.pos + 1) >= 0xD0 && AT(bits.pos + 1) <= 0xD7) bits.pos += 2;
                for (int i = 0; i < 3; i++) comps[i].pred = 0;
            }
            mcu++;
            for (int ci = 0; ci < 3 && ok; ci++) {
                Comp *c = &comps[ci];
                const Huff *dct = &dc[c->td], *act = &ac[c->ta];
                const int *q = qt[c->tq];
                for (int v = 0; v < c->v && ok; v++)
                    for (int h = 0; h < c->h && ok; h++) {
                        memset(blk, 0, sizeof blk);
                        int s = decode(&bits, dct);
                        if (s < 0) { ok = false; break; }
                        c->pred += extend(&bits, (unsigned)s);
                        blk[0] = c->pred * q[0];
                        for (int k = 1; k < 64;) {
                            int rs = decode(&bits, act);
                            if (rs < 0) { ok = false; break; }
                            int r = rs >> 4, sz = rs & 15;
                            if (sz == 0) {
                                if (r == 15) { k += 16; continue; }
                                break;
                            }
                            k += r;
                            if (k > 63) break;
                            int z = ZIGZAG[k];
                            blk[z] = extend(&bits, (unsigned)sz) * q[z];
                            k++;
                        }
                        if (!ok) break;
                        Blk *o = &out[nout++];
                        o->ci = (uint8_t)ci;
                        o->bx = (uint16_t)((mx * (size_t)c->h + (size_t)h) * 8);
                        o->by = (uint16_t)((my * (size_t)c->v + (size_t)v) * 8);
                        idct(blk, o->px);
                    }
            }
        }
    if (ok) {
        /* Write the planes as I8 textures: 8x4 tiles in row-major tile order, 32 bytes per tile. */
        for (size_t i = 0; i < nout; i++) {
            Blk *b = &out[i];
            size_t off = poff[b->ci], w = pw[b->ci], hgt = ph[b->ci], tiles_per_row = w / 8;
            if (b->bx >= w) continue;
            for (size_t y = 0; y < 8; y++) {
                size_t yy = b->by + y;
                if (yy >= hgt) break;
                size_t tile = (yy / 4) * tiles_per_row + b->bx / 8;
                memcpy(g_ram + off + tile * 32 + (yy % 4) * 8, b->px + y * 8, 8);
            }
        }
    }
    free(out);
    return ok;
#undef AT
#undef BE16
}
