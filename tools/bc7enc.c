/* BC7 encoder used by tools/hdtex_pack.py (built as a DLL / shared library, called through ctypes).
 *
 * Modes 6 (one subset, RGBA), 1 and 3 (two subsets, opaque), 7 (two subsets, RGBA) and 5 (one
 * subset, separate alpha indices, channel rotation). Each subset is fitted along its principal axis,
 * then refined by least squares over the chosen indices; two-subset modes try the partitions whose
 * subsets lie closest to a line. The block with the smallest RGBA squared error wins.
 *
 *   gcc -O2 -shared -o bc7enc.dll bc7enc.c
 */
#include <stdint.h>
#include <string.h>
#include <math.h>

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

/* Two-subset partitions (bit i = subset of pixel i) and the anchor pixel of subset 1. */
static const uint16_t PART2[64] = {
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8, 0xFF00, 0xFFF0, 0xF000,
    0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C,
    0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696, 0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660,
    0x0272, 0x04E4, 0x4E40, 0x2720, 0xC936, 0x936C, 0x39C6, 0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22};
static const uint8_t ANCHOR2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
    15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6,
    6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15};

static const int W2[4] = {0, 21, 43, 64};
static const int W3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
static const int W4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
static const int *weights(int ib) { return ib == 2 ? W2 : ib == 3 ? W3 : W4; }

typedef struct { uint8_t b[16]; int pos; } Bits;
static void put(Bits *w, uint32_t v, int n) {
    for (int i = 0; i < n; i++, w->pos++)
        if (v >> i & 1) w->b[w->pos >> 3] |= (uint8_t)(1 << (w->pos & 7));
}

/* n-bit value to 8 bits, as the decoder does */
static int expand(int v, int n) { v <<= 8 - n; return v | v >> n; }

/* Endpoint fit of one subset. nch channels (3: RGB, 4: RGBA) quantised to cb bits, plus a p-bit per
 * endpoint (pmode 1), one shared by both endpoints (pmode 2) or none (0); ib-bit indices. opaque:
 * p-bits fixed at 1, so alpha 127 decodes to exactly 255. */
typedef struct { int nch, cb, pmode, ib, opaque; } Fit;
typedef struct { int q[2][4], p[2], err; uint8_t idx[16]; } Ends;

static int quant(float v, int cb, int p) {
    int top = (1 << cb) - 1, q;
    if (p < 0) q = (int)lrintf(v * top / 255.0f);
    else q = (int)lrintf(((v * ((1 << (cb + 1)) - 1) / 255.0f) - p) * 0.5f);
    return q < 0 ? 0 : q > top ? top : q;
}
static int unquant(int q, int cb, int p) { return p < 0 ? expand(q, cb) : expand(q << 1 | p, cb + 1); }

static int assign(const Fit *f, const uint8_t (*px)[4], int n, const int pal[16][4], uint8_t *idx) {
    int err = 0, np = 1 << f->ib;
    for (int i = 0; i < n; i++) {
        int best = 1 << 30, bi = 0;
        for (int k = 0; k < np; k++) {
            int e = 0;
            for (int c = 0; c < f->nch; c++) { int d = px[i][c] - pal[k][c]; e += d * d; }
            if (e < best) { best = e; bi = k; }
        }
        idx[i] = (uint8_t)bi;
        err += best;
    }
    return err;
}

static void palette(const Fit *f, const int q[2][4], const int p[2], int pal[16][4]) {
    const int *w = weights(f->ib);
    for (int c = 0; c < f->nch; c++) {
        int a = unquant(q[0][c], f->cb, f->pmode ? p[0] : -1), b = unquant(q[1][c], f->cb, f->pmode ? p[1] : -1);
        for (int k = 0; k < 1 << f->ib; k++) pal[k][c] = ((64 - w[k]) * a + w[k] * b + 32) >> 6;
    }
}

static void fit_subset(const Fit *f, const uint8_t (*px)[4], int n, Ends *out) {
    float mean[4] = {0}, cov[4][4] = {{0}}, axis[4] = {1, 1, 1, 1};
    const int nc = f->nch;
    for (int i = 0; i < n; i++)
        for (int c = 0; c < nc; c++) mean[c] += px[i][c];
    for (int c = 0; c < nc; c++) mean[c] /= (float)n;
    for (int i = 0; i < n; i++)
        for (int a = 0; a < nc; a++)
            for (int b = 0; b < nc; b++) cov[a][b] += (px[i][a] - mean[a]) * (px[i][b] - mean[b]);
    for (int it = 0; it < 8; it++) {
        float v[4] = {0}, m = 0;
        for (int a = 0; a < nc; a++) {
            for (int b = 0; b < nc; b++) v[a] += cov[a][b] * axis[b];
            if (fabsf(v[a]) > m) m = fabsf(v[a]);
        }
        if (m < 1e-6f) break;
        for (int a = 0; a < nc; a++) axis[a] = v[a] / m;
    }
    float len = 0;
    for (int c = 0; c < nc; c++) len += axis[c] * axis[c];
    len = sqrtf(len);
    for (int c = 0; c < nc; c++) axis[c] = len > 0 ? axis[c] / len : 0;
    float tmin = 1e9f, tmax = -1e9f;
    for (int i = 0; i < n; i++) {
        float t = 0;
        for (int c = 0; c < nc; c++) t += (px[i][c] - mean[c]) * axis[c];
        if (t < tmin) tmin = t;
        if (t > tmax) tmax = t;
    }
    float e[2][4];
    for (int c = 0; c < nc; c++) { e[0][c] = mean[c] + axis[c] * tmin; e[1][c] = mean[c] + axis[c] * tmax; }

    const int *w = weights(f->ib), top = (1 << f->ib) - 1;
    out->err = 1 << 30;
    for (int it = 0; it < 3; it++) {
        /* best p-bits for these endpoints */
        int ncomb = f->pmode == 1 ? 4 : f->pmode == 2 ? 2 : 1;
        for (int pc = f->opaque ? ncomb - 1 : 0; pc < ncomb; pc++) {
            Ends t;
            memset(&t, 0, sizeof t);
            t.p[0] = f->pmode == 1 ? pc & 1 : f->pmode == 2 ? pc : 0;
            t.p[1] = f->pmode == 1 ? pc >> 1 : t.p[0];
            for (int k = 0; k < 2; k++)
                for (int c = 0; c < nc; c++) {
                    float v = e[k][c] < 0 ? 0 : e[k][c] > 255 ? 255 : e[k][c];
                    t.q[k][c] = quant(v, f->cb, f->pmode ? t.p[k] : -1);
                }
            int pal[16][4];
            palette(f, t.q, t.p, pal);
            t.err = assign(f, px, n, pal, t.idx);
            if (t.err < out->err) *out = t;
        }
        if (out->err == 0) break;
        /* least squares endpoints for the chosen indices */
        float aa = 0, ab = 0, bb = 0, ax[4] = {0}, bx[4] = {0};
        for (int i = 0; i < n; i++) {
            float b = w[out->idx[i]] / 64.0f, a = 1 - b;
            aa += a * a; ab += a * b; bb += b * b;
            for (int c = 0; c < nc; c++) { ax[c] += a * px[i][c]; bx[c] += b * px[i][c]; }
        }
        float det = aa * bb - ab * ab;
        if (fabsf(det) < 1e-6f) break;
        for (int c = 0; c < nc; c++) {
            e[0][c] = (ax[c] * bb - bx[c] * ab) / det;
            e[1][c] = (bx[c] * aa - ax[c] * ab) / det;
        }
        (void)top;
    }
}

/* Residual of a subset off its best-fitting line (sum of squares minus the largest eigenvalue). */
static float line_residual(const uint8_t (*px)[4], const uint8_t *sel, int which, int nc) {
    float mean[4] = {0}, cov[4][4] = {{0}}, axis[4] = {1, 1, 1, 1};
    int n = 0;
    for (int i = 0; i < 16; i++)
        if (sel[i] == which) { n++; for (int c = 0; c < nc; c++) mean[c] += px[i][c]; }
    if (n < 2) return 0;
    for (int c = 0; c < nc; c++) mean[c] /= (float)n;
    float tr = 0;
    for (int i = 0; i < 16; i++)
        if (sel[i] == which)
            for (int a = 0; a < nc; a++)
                for (int b = 0; b < nc; b++) cov[a][b] += (px[i][a] - mean[a]) * (px[i][b] - mean[b]);
    for (int c = 0; c < nc; c++) tr += cov[c][c];
    float lam = 0;
    for (int it = 0; it < 6; it++) {
        float v[4] = {0}, m = 0;
        for (int a = 0; a < nc; a++) {
            for (int b = 0; b < nc; b++) v[a] += cov[a][b] * axis[b];
            if (fabsf(v[a]) > m) m = fabsf(v[a]);
        }
        if (m < 1e-6f) return tr;
        lam = m;
        for (int a = 0; a < nc; a++) axis[a] = v[a] / m;
    }
    /* Rayleigh quotient for a better eigenvalue */
    float num = 0, den = 0;
    for (int a = 0; a < nc; a++) {
        float v = 0;
        for (int b = 0; b < nc; b++) v += cov[a][b] * axis[b];
        num += axis[a] * v; den += axis[a] * axis[a];
    }
    if (den > 0) lam = num / den;
    return tr - lam;
}

/* Full RGBA error of a candidate block, against the source pixels. */
static int block_err(const uint8_t (*src)[4], const uint8_t (*dec)[4]) {
    int e = 0;
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < 4; c++) { int d = src[i][c] - dec[i][c]; e += d * d; }
    return e;
}

/* ---- mode 6 / 1 / 3 / 7 (shared indices for all channels) ---- */
typedef struct { uint8_t blk[16]; int err; } Cand;

static void emit_shared(int mode, int part, const Fit *f, Ends *s, int nsub, const uint8_t *sel, const uint8_t (*src)[4], Cand *best) {
    int anchor[2] = {0, nsub > 1 ? ANCHOR2[part] : 0};
    uint8_t idx[16];
    int half = 1 << (f->ib - 1), top = (1 << f->ib) - 1;
    /* per-subset index lists back to pixel order; flip subsets whose anchor index has the top bit */
    int cnt[2] = {0, 0};
    for (int i = 0; i < 16; i++) { int k = sel[i]; idx[i] = s[k].idx[cnt[k]++]; }
    for (int k = 0; k < nsub; k++) {
        if (idx[anchor[k]] < half) continue;
        for (int c = 0; c < 4; c++) { int t = s[k].q[0][c]; s[k].q[0][c] = s[k].q[1][c]; s[k].q[1][c] = t; }
        int t = s[k].p[0]; s[k].p[0] = s[k].p[1]; s[k].p[1] = t;
        for (int i = 0; i < 16; i++) if (sel[i] == k) idx[i] = (uint8_t)(top - idx[i]);
    }
    Bits w = {{0}, 0};
    put(&w, 1u << mode, mode + 1);
    if (nsub > 1) put(&w, (uint32_t)part, 6);
    for (int c = 0; c < f->nch; c++)
        for (int k = 0; k < nsub; k++) { put(&w, (uint32_t)s[k].q[0][c], f->cb); put(&w, (uint32_t)s[k].q[1][c], f->cb); }
    if (f->pmode == 1)
        for (int k = 0; k < nsub; k++) { put(&w, (uint32_t)s[k].p[0], 1); put(&w, (uint32_t)s[k].p[1], 1); }
    else if (f->pmode == 2)
        for (int k = 0; k < nsub; k++) put(&w, (uint32_t)s[k].p[0], 1);
    for (int i = 0; i < 16; i++) put(&w, idx[i], f->ib - (i == anchor[0] || (nsub > 1 && i == anchor[1])));

    /* decode for the true error (alpha 255 in RGB-only modes) */
    uint8_t dec[16][4];
    for (int k = 0; k < nsub; k++) {
        int pal[16][4];
        palette(f, (const int (*)[4])s[k].q, s[k].p, pal);
        for (int i = 0; i < 16; i++) {
            if (sel[i] != k) continue;
            for (int c = 0; c < 4; c++) dec[i][c] = (uint8_t)(c < f->nch ? pal[idx[i]][c] : 255);
        }
    }
    int e = block_err(src, dec);
    if (e < best->err) { memcpy(best->blk, w.b, 16); best->err = e; }
}

static void try_one_subset(int mode, const Fit *f, const uint8_t (*px)[4], Cand *best) {
    Ends s;
    uint8_t sel[16] = {0};
    fit_subset(f, px, 16, &s);
    emit_shared(mode, 0, f, &s, 1, sel, px, best);
}

static void try_two_subsets(int mode, const Fit *f, const uint8_t (*px)[4], const int *parts, int nparts, Cand *best) {
    for (int pi = 0; pi < nparts; pi++) {
        int part = parts[pi];
        uint8_t sel[16], sub[2][16][4];
        int n[2] = {0, 0};
        for (int i = 0; i < 16; i++) {
            sel[i] = (uint8_t)(PART2[part] >> i & 1);
            memcpy(sub[sel[i]][n[sel[i]]++], px[i], 4);
        }
        Ends s[2];
        fit_subset(f, (const uint8_t (*)[4])sub[0], n[0], &s[0]);
        fit_subset(f, (const uint8_t (*)[4])sub[1], n[1], &s[1]);
        emit_shared(mode, part, f, s, 2, sel, px, best);
    }
}

/* Best partitions for two-subset modes, by line residual. */
static int pick_parts(const uint8_t (*px)[4], int nc, int *out, int want) {
    float score[64];
    for (int p = 0; p < 64; p++) {
        uint8_t sel[16];
        for (int i = 0; i < 16; i++) sel[i] = (uint8_t)(PART2[p] >> i & 1);
        score[p] = line_residual(px, sel, 0, nc) + line_residual(px, sel, 1, nc);
    }
    for (int k = 0; k < want; k++) {
        int b = -1;
        for (int p = 0; p < 64; p++) {
            int used = 0;
            for (int j = 0; j < k; j++) used |= out[j] == p;
            if (!used && (b < 0 || score[p] < score[b])) b = p;
        }
        out[k] = b;
    }
    return want;
}

/* ---- mode 5: RGB endpoints and a separate scalar channel (after rotation) ---- */
static void try_mode5(const uint8_t (*src)[4], Cand *best) {
    for (int rot = 0; rot < 4; rot++) {
        uint8_t px[16][4];
        for (int i = 0; i < 16; i++) {
            memcpy(px[i], src[i], 4);
            if (rot) { uint8_t t = px[i][3]; px[i][3] = px[i][rot - 1]; px[i][rot - 1] = t; }
        }
        Fit f = {3, 7, 0, 2, 0};
        Ends s;
        fit_subset(&f, (const uint8_t (*)[4])px, 16, &s);
        /* scalar: endpoints min/max, then least squares */
        float a0 = 255, a1 = 0;
        for (int i = 0; i < 16; i++) { if (px[i][3] < a0) a0 = px[i][3]; if (px[i][3] > a1) a1 = px[i][3]; }
        int qa[2] = {0, 0};
        uint8_t ai[16];
        int aerr = 1 << 30;
        for (int it = 0; it < 3; it++) {
            int q0 = (int)lrintf(a0 < 0 ? 0 : a0 > 255 ? 255 : a0), q1 = (int)lrintf(a1 < 0 ? 0 : a1 > 255 ? 255 : a1);
            int e = 0;
            uint8_t ti[16];
            for (int i = 0; i < 16; i++) {
                int bb = 1 << 30, bi = 0;
                for (int k = 0; k < 4; k++) {
                    int v = ((64 - W2[k]) * q0 + W2[k] * q1 + 32) >> 6, d = px[i][3] - v;
                    if (d * d < bb) { bb = d * d; bi = k; }
                }
                ti[i] = (uint8_t)bi; e += bb;
            }
            if (e < aerr) { aerr = e; qa[0] = q0; qa[1] = q1; memcpy(ai, ti, 16); }
            float aa = 0, ab = 0, bb2 = 0, ax = 0, bx = 0;
            for (int i = 0; i < 16; i++) {
                float b = W2[ai[i]] / 64.0f, a = 1 - b;
                aa += a * a; ab += a * b; bb2 += b * b; ax += a * px[i][3]; bx += b * px[i][3];
            }
            float det = aa * bb2 - ab * ab;
            if (fabsf(det) < 1e-6f || aerr == 0) break;
            a0 = (ax * bb2 - bx * ab) / det;
            a1 = (bx * aa - ax * ab) / det;
        }
        uint8_t ci[16];
        memcpy(ci, s.idx, 16);
        if (ci[0] >= 2) {
            for (int c = 0; c < 3; c++) { int t = s.q[0][c]; s.q[0][c] = s.q[1][c]; s.q[1][c] = t; }
            for (int i = 0; i < 16; i++) ci[i] = (uint8_t)(3 - ci[i]);
        }
        if (ai[0] >= 2) {
            int t = qa[0]; qa[0] = qa[1]; qa[1] = t;
            for (int i = 0; i < 16; i++) ai[i] = (uint8_t)(3 - ai[i]);
        }
        Bits w = {{0}, 0};
        put(&w, 1u << 5, 6);
        put(&w, (uint32_t)rot, 2);
        for (int c = 0; c < 3; c++) { put(&w, (uint32_t)s.q[0][c], 7); put(&w, (uint32_t)s.q[1][c], 7); }
        put(&w, (uint32_t)qa[0], 8); put(&w, (uint32_t)qa[1], 8);
        for (int i = 0; i < 16; i++) put(&w, ci[i], i ? 2 : 1);
        for (int i = 0; i < 16; i++) put(&w, ai[i], i ? 2 : 1);
        uint8_t dec[16][4];
        int pal[16][4];
        int p0[2] = {0, 0};
        palette(&f, (const int (*)[4])s.q, p0, pal);
        for (int i = 0; i < 16; i++) {
            for (int c = 0; c < 3; c++) dec[i][c] = (uint8_t)pal[ci[i]][c];
            dec[i][3] = (uint8_t)(((64 - W2[ai[i]]) * qa[0] + W2[ai[i]] * qa[1] + 32) >> 6);
            if (rot) { uint8_t t = dec[i][3]; dec[i][3] = dec[i][rot - 1]; dec[i][rot - 1] = t; }
        }
        int e = block_err(src, dec);
        if (e < best->err) { memcpy(best->blk, w.b, 16); best->err = e; }
    }
}

static void encode_block(const uint8_t (*px)[4], uint8_t *out) {
    Cand best;
    best.err = 1 << 30;
    int opaque = 1;
    for (int i = 0; i < 16; i++) opaque &= px[i][3] == 255;
    Fit m6 = {4, 7, 1, 4, opaque};
    try_one_subset(6, &m6, px, &best);
    if (best.err > 3 * 16) { /* already within ~1 level per channel: two-subset modes rarely beat it */
        int parts[4];
        if (opaque) {
            pick_parts(px, 3, parts, 4);
            Fit m1 = {3, 6, 2, 3, 0}, m3 = {3, 7, 1, 2, 0};
            try_two_subsets(1, &m1, px, parts, 4, &best);
            try_two_subsets(3, &m3, px, parts, 4, &best);
        } else {
            try_mode5(px, &best);
            pick_parts(px, 4, parts, 4);
            Fit m7 = {4, 5, 1, 2, 0};
            try_two_subsets(7, &m7, px, parts, 4, &best);
        }
    }
    memcpy(out, best.blk, 16);
}

/* RGBA8 image (w x h, rows packed) to BC7 blocks, row-major; edges replicate the last pixel. */
EXPORT void bc7_encode(const uint8_t *rgba, int w, int h, uint8_t *out) {
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
            uint8_t px[16][4];
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++) {
                    int sx = bx * 4 + x, sy = by * 4 + y;
                    if (sx >= w) sx = w - 1;
                    if (sy >= h) sy = h - 1;
                    memcpy(px[y * 4 + x], rgba + ((size_t)sy * w + sx) * 4, 4);
                }
            encode_block((const uint8_t (*)[4])px, out + ((size_t)by * bw + bx) * 16);
        }
}
