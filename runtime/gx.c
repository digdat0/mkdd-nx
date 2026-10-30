/* GX command-stream decoder (C port of gc-runtime's gx.rs).
 *
 * Parses the FIFO the game writes to 0xCC008000 (and display lists it calls): CP / XF / BP
 * register loads and draw commands. For each draw it decodes vertices (direct and indexed, all
 * component/colour formats), transforms positions by the position matrix and XF projection to
 * clip space, computes the colour channels with the game's XF lighting, generates texture
 * coordinates through the texgen matrices, snapshots the TEV/blend state for the per-pixel
 * shader and references the bound textures (decoded later by the renderer). A display copy
 * (BP 0x52 with the copy-to-XFB bit) ends the frame; the next VI retrace presents it.
 * Not handled: indirect textures, lines/points. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gx.h"
#include "rt.h"

#define MAX_FRAME_VERTS 2000000u
#define MAX_BATCHES 8192u
#define TMEM_SIZE 0x100000u
#define TEX_BUDGET (160u << 20)
/* FIFO bytes the game may be ahead of the decoder: about 1.5 frames (s_qlimit), so a slow decoder
 * slows the game instead of building a backlog. A fixed 4 MB allowed 20-35 race frames
 * (115-220 KB each): when the decoder fell behind, the picture lagged the controls by up to half a
 * second (race 1 "laggy controls", the countdown). */
#define GPU_QUEUE_MIN (128u << 10)
#define GPU_QUEUE_MAX (4u << 20)

static inline uint32_t be16(const uint8_t *d) { return (uint32_t)d[0] << 8 | d[1]; }
static inline uint32_t be32(const uint8_t *d) { return (uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3]; }
static inline uint32_t bits(uint32_t v, uint32_t lo, uint32_t n) { return (v >> lo) & ((1u << n) - 1); }
static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void rgba_u32(uint32_t v, float o[4]) {
    o[0] = (float)((v >> 24) & 0xFF) / 255.0f; o[1] = (float)((v >> 16) & 0xFF) / 255.0f;
    o[2] = (float)((v >> 8) & 0xFF) / 255.0f; o[3] = (float)(v & 0xFF) / 255.0f;
}
static inline float sext11(uint32_t v) { return (float)((int32_t)((v & 0x7FF) << 21) >> 21) / 255.0f; }
static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

/* Bytes per component for GX component formats u8, s8, u16, s16, f32. */
static inline unsigned comp_size(uint32_t fmt) { return fmt <= 1 ? 1 : fmt <= 3 ? 2 : 4; }
static inline unsigned color_size(uint32_t fmt) { static const unsigned s[6] = {2, 3, 4, 2, 3, 4}; return fmt < 6 ? s[fmt] : 4; }

typedef struct { uint32_t mode, fmt; unsigned count; float scale; unsigned inl; } AttrDesc; /* mode 0 none 1 direct 2 idx8 3 idx16 */
typedef struct { bool valid, pnmtx, texmtx[8]; AttrDesc pos, nrm, col[2], tex[8]; unsigned size; } VtxFormat;
typedef struct { bool stq; uint32_t ty; unsigned row; bool dual, normalize; float post[12]; } TexGen;
typedef struct { float col[4], cos[3], dist[3], pos[3], dir[3], k[3]; } LightParams;
typedef struct { bool lighting, mat_vtx, amb_vtx; float mat[4], amb[4]; uint32_t diff_fn, attn_fn; LightParams lights[8]; unsigned count; } LitPart;
typedef struct { unsigned chans; LitPart parts[2][2]; } LightSetup;

/* ---- decoder state (owned by the GPU thread) ---- */
static struct {
    uint8_t *buf;
    size_t len, cap, pos;
    uint32_t cp[256];
    uint32_t xf_mem[0x1000];
    uint32_t xf_reg[0x100];
    uint32_t bp[256];
    uint32_t bp_mask;
    float tev_reg[4][4], konst[4][4];
    uint8_t *tmem;
    U64Map tex_cache_hash, tex_cache_key; /* (addr, fmt, w, h, tlut addr, tlut fmt) -> content hash / key */
    U64Map tex_memo;                      /* image regs + mode + palette regs -> key (0 = none), per frame */
    uint64_t state_gen, xf_gen, cp_gen;
    bool mat_valid;
    uint64_t mat_gen, mat_key;
    TevState mat_tev;
    uint8_t mat_uvs;
    VtxFormat fmt_cache[8];
    uint64_t light_gen, light_cache_gen;
    bool light_valid;
    LightSetup light;
    U64Map copy_targets;   /* EFB copy destination -> signature of its RAM at the copy (copy_sig) */
    U64Map copy_len;       /* ... -> bytes the signature covers */
    Frame *frame;
    float efb[2];
    unsigned unknown_logged;
    float mtx_cache[64][21];
    uint64_t mtx_valid;
    /* draw-setup dependencies: XF matrix rows (groups of 4 words), normal rows (groups of 3),
     * post rows read by the current setup; a write there sets xf_dirty */
    uint64_t dep_mtx, dep_post, dep_nrm;
    bool xf_dirty;
    uint64_t vat_gen;
    /* GPU transform: per-frame matrix slots and light block in frame->xf */
    uint64_t slot_valid;
    Frame *slot_frame;
    uint32_t slot_off[64];
    Frame *lblock_frame;
    uint64_t lblock_gen;
    uint32_t lblock_off;
    unsigned movie;        /* frames left in which a movie counts as playing (end_frame) */
    uint32_t hud_zone;     /* CP 0x01 marker (native.c): 1 inside the race HUD's draw, 2 its map, 3 a wipe,
                            * 4-6 a MODERN HUD picture kept in the middle, at the left, at the right edge
                            * (bits 8 and up: its group, MG_*) */
} G;

/* ---- frames shared with the renderer ---- */
uint64_t g_gx_busy_ns, g_gx_draws;
static PMutex s_flock;
static Frame *s_pending, *s_latest;
static volatile uint64_t s_frame_count;

/* Released frames keep their buffers for reuse: a race frame is ~8 MB of vertices, and fresh
 * allocations of that size every frame cost page faults and copies while the vectors grow. */
static uint64_t s_frames_decoded; /* end_frame count (decoder thread) */
/* Per-frame caches compare frame pointers, and frames are recycled from the pool: end_frame
 * forgets them all so a reused Frame never matches an old cache entry. */
struct Frame;
static struct Frame *s_vb_frame, *s_rec_frame, *s_tmi_frame;
static uint32_t s_vb_layout, s_vb_next;
static PMutex s_pool_lock;
static Frame *s_pool[4];
static int s_npool;

static Frame *frame_new(void) {
    Frame *f = NULL;
    pmutex_lock(&s_pool_lock);
    if (s_npool) f = s_pool[--s_npool];
    pmutex_unlock(&s_pool_lock);
    if (f) {
        f->nverts = 0;
        f->batches.len = f->vb.len = f->indices.len = f->copies.len = f->xf.len = 0;
        memset(f->clear, 0, sizeof f->clear);
        memset(f->efb, 0, sizeof f->efb);
        f->draws = f->vseam = 0;
    } else f = calloc(1, sizeof *f);
    f->refs = 1;
    return f;
}
void gx_frame_release(Frame *f) {
    if (!f || __atomic_sub_fetch(&f->refs, 1, __ATOMIC_ACQ_REL) > 0) return;
    pmutex_lock(&s_pool_lock);
    if (s_npool < 4) { s_pool[s_npool++] = f; f = NULL; }
    pmutex_unlock(&s_pool_lock);
    if (!f) return;
    vec_free(&f->batches); vec_free(&f->vb); vec_free(&f->indices); vec_free(&f->copies); vec_free(&f->xf);
    free(f);
}
Frame *gx_frame_acquire(void) {
    pmutex_lock(&s_flock);
    Frame *f = s_latest;
    if (f) __atomic_add_fetch(&f->refs, 1, __ATOMIC_ACQ_REL);
    pmutex_unlock(&s_flock);
    return f;
}
uint64_t gx_frame_count(void) { return s_frame_count; }

/* Display copy finished: like the real VI, the new XFB is shown at the next retrace. */
static void publish(Frame *f) {
    pmutex_lock(&s_flock);
    Frame *old = s_pending;
    s_pending = f;
    pmutex_unlock(&s_flock);
    gx_frame_release(old);
}
void gx_scanout(void) {
    pmutex_lock(&s_flock);
    Frame *f = s_pending, *old = NULL;
    if (f) { s_pending = NULL; old = s_latest; s_latest = f; s_frame_count++; }
    pmutex_unlock(&s_flock);
    gx_frame_release(old);
}

/* ---- texture store shared with the renderer: budgeted, oldest-first eviction ---- */
static PMutex s_tlock;
static U64Map s_textures;  /* key -> GxTexture* */
static struct { uint64_t key; size_t bytes; } *s_torder;
static size_t s_torder_head, s_torder_len, s_torder_cap, s_tbytes;

static void tex_free(GxTexture *t) { free(t->raw); free(t->tlut); free(t); }
void gx_texture_release(GxTexture *t) {
    if (t && __atomic_sub_fetch(&t->refs, 1, __ATOMIC_ACQ_REL) == 0) tex_free(t);
}
GxTexture *gx_texture_acquire(uint64_t key) {
    pmutex_lock(&s_tlock);
    uint64_t v;
    GxTexture *t = u64map_get(&s_textures, key, &v) ? (GxTexture *)(uintptr_t)v : NULL;
    if (t) __atomic_add_fetch(&t->refs, 1, __ATOMIC_ACQ_REL);
    pmutex_unlock(&s_tlock);
    return t;
}
static bool store_has(uint64_t key) {
    pmutex_lock(&s_tlock);
    bool has = u64map_get(&s_textures, key, NULL);
    pmutex_unlock(&s_tlock);
    return has;
}
static void add_texture(GxTexture *t) {
    size_t bytes = t->raw_len + (t->tlut ? 512 : 0);
    pmutex_lock(&s_tlock);
    if (s_torder_len == s_torder_cap) {
        size_t ncap = s_torder_cap ? s_torder_cap * 2 : 1024;
        void *n = malloc(ncap * sizeof *s_torder);
        for (size_t i = 0; i < s_torder_len; i++) memcpy((char *)n + i * sizeof *s_torder, &s_torder[(s_torder_head + i) % s_torder_cap], sizeof *s_torder);
        free(s_torder);
        s_torder = n;
        s_torder_head = 0;
        s_torder_cap = ncap;
    }
    size_t tail = (s_torder_head + s_torder_len) % s_torder_cap;
    s_torder[tail].key = t->key;
    s_torder[tail].bytes = bytes;
    s_torder_len++;
    s_tbytes += bytes;
    while (s_tbytes > TEX_BUDGET && s_torder_len > 1) {
        uint64_t k = s_torder[s_torder_head].key;
        s_tbytes -= s_torder[s_torder_head].bytes;
        s_torder_head = (s_torder_head + 1) % s_torder_cap;
        s_torder_len--;
        uint64_t v;
        if (u64map_get(&s_textures, k, &v)) {
            u64map_del(&s_textures, k);
            gx_texture_release((GxTexture *)(uintptr_t)v);
        }
    }
    uint64_t v;
    if (u64map_get(&s_textures, t->key, &v)) gx_texture_release((GxTexture *)(uintptr_t)v);
    t->refs = 1;
    u64map_put(&s_textures, t->key, (uint64_t)(uintptr_t)t);
    pmutex_unlock(&s_tlock);
}

/* ---- GPU queue: the CPU's gather pipe submits 32-byte bursts; the GPU thread decodes ---- */
static PMutex s_qlock;
static PCond s_qwork, s_qspace;
static uint8_t *s_qbuf;
static size_t s_qlen, s_qcap;
static size_t s_qheld;                   /* bytes the decoder took and has not finished */
static volatile size_t s_qlimit = GPU_QUEUE_MAX;
static bool s_qbusy;
static PMutex s_elock;
static GxEvent s_events[1024];
static int s_nevents;

void gx_submit(const uint8_t *bytes, size_t n) {
    pmutex_lock(&s_qlock);
    if (s_qlen + s_qheld > s_qlimit) {
        uint64_t t0 = plat_now_ns();
        while (s_qlen + s_qheld > s_qlimit) pcond_wait(&s_qspace, &s_qlock);
        g_gx_qwait_ns += plat_now_ns() - t0;
    }
    if (s_qlen + n > s_qcap) {
        s_qcap = s_qcap ? s_qcap * 2 : 1 << 20;
        while (s_qcap < s_qlen + n) s_qcap *= 2;
        s_qbuf = realloc(s_qbuf, s_qcap);
    }
    memcpy(s_qbuf + s_qlen, bytes, n);
    s_qlen += n;
    /* The gather pipe submits 32-byte bursts: only wake the GPU thread when it sleeps (it takes
     * the whole queue each round), not once per burst. */
    if (!s_qbusy) {
        s_qbusy = true;
        pcond_signal(&s_qwork);
    }
    pmutex_unlock(&s_qlock);
}
bool gx_gpu_busy_hint(void) { return s_qbusy || s_qlen; } /* unlocked, for statistics */
bool gx_gpu_idle(void) {
    pmutex_lock(&s_qlock);
    bool idle = s_qlen == 0 && !s_qbusy;
    pmutex_unlock(&s_qlock);
    return idle;
}
int gx_take_events(GxEvent *out, int max) {
    pmutex_lock(&s_elock);
    int n = s_nevents < max ? s_nevents : max;
    memcpy(out, s_events, (size_t)n * sizeof *out);
    memmove(s_events, s_events + n, (size_t)(s_nevents - n) * sizeof *out);
    s_nevents -= n;
    pmutex_unlock(&s_elock);
    return n;
}
/* Events produced during one gx_push, published afterwards. */
static GxEvent s_local_ev[1024];
static int s_nlocal;
static void push_event(GxEventKind k, uint16_t tok) {
    if (s_nlocal < 1024) s_local_ev[s_nlocal++] = (GxEvent){k, tok};
}

/* ---- decoding ---- */
static VtxFormat decode_format(uint32_t vat) {
    uint32_t lo = G.cp[0x50], hi = G.cp[0x60], a = G.cp[0x70 + vat], b = G.cp[0x80 + vat], c = G.cp[0x90 + vat];
    VtxFormat f = {0};
    f.valid = true;
#define MK(mode_, fmt_, count_, shift_, elem_) ((AttrDesc){(mode_), (fmt_), (count_), 1.0f / (float)(1u << (shift_)), \
        (mode_) == 1 ? (elem_) : (mode_) == 2 ? 1u : (mode_) == 3 ? 2u : 0u})
    f.pnmtx = lo & 1;
    for (int i = 0; i < 8; i++) f.texmtx[i] = (lo >> (1 + i)) & 1;
    unsigned pos_cnt = a & 1 ? 3 : 2;
    uint32_t pos_fmt = bits(a, 1, 3);
    f.pos = MK(bits(lo, 9, 2), pos_fmt, pos_cnt, bits(a, 4, 5), pos_cnt * comp_size(pos_fmt));
    bool nbt = bits(a, 9, 1);
    uint32_t nrm_fmt = bits(a, 10, 3);
    unsigned nrm_cnt = nbt ? 9 : 3;
    uint32_t nrm_shift = nrm_fmt == 1 ? 6 : nrm_fmt == 3 ? 14 : 0;
    f.nrm = MK(bits(lo, 11, 2), nrm_fmt, nrm_cnt, nrm_shift, nrm_cnt * comp_size(nrm_fmt));
    /* NBT with three separate indices: the normal is the first of them. */
    if (f.nrm.mode >= 2 && nbt && (a >> 31) & 1) f.nrm.inl *= 3;
    f.col[0] = MK(bits(lo, 13, 2), bits(a, 14, 3), 4, 0, color_size(bits(a, 14, 3)));
    f.col[1] = MK(bits(lo, 15, 2), bits(a, 18, 3), 4, 0, color_size(bits(a, 18, 3)));
    /* (count bit, fmt, shift) per texcoord, spread across VAT A/B/C. */
    const uint32_t tf[8][3] = {
        {bits(a, 21, 1), bits(a, 22, 3), bits(a, 25, 5)}, {bits(b, 0, 1), bits(b, 1, 3), bits(b, 4, 5)},
        {bits(b, 9, 1), bits(b, 10, 3), bits(b, 13, 5)}, {bits(b, 18, 1), bits(b, 19, 3), bits(b, 22, 5)},
        {bits(b, 27, 1), bits(b, 28, 3), bits(c, 0, 5)}, {bits(c, 5, 1), bits(c, 6, 3), bits(c, 9, 5)},
        {bits(c, 14, 1), bits(c, 15, 3), bits(c, 18, 5)}, {bits(c, 23, 1), bits(c, 24, 3), bits(c, 27, 5)},
    };
    for (int i = 0; i < 8; i++) {
        unsigned n = tf[i][0] ? 2 : 1;
        f.tex[i] = MK(bits(hi, 2 * (uint32_t)i, 2), tf[i][1], n, tf[i][2], n * comp_size(tf[i][1]));
    }
#undef MK
    f.size = f.pnmtx;
    for (int i = 0; i < 8; i++) f.size += f.texmtx[i];
    f.size += f.pos.inl + f.nrm.inl + f.col[0].inl + f.col[1].inl;
    for (int i = 0; i < 8; i++) f.size += f.tex[i].inl;
    return f;
}

static const VtxFormat *format(uint32_t vat) {
    VtxFormat *f = &G.fmt_cache[vat & 7];
    if (!f->valid) *f = decode_format(vat & 7);
    return f;
}

/* Indexed attribute element (NULL past the end of RAM); r is a draw()'s Reader. */
#define attr_idx(r, idx) ((uint64_t)(idx) * (r)->stride + (r)->elem <= (r)->limit ? (r)->base + (uint64_t)(idx) * (r)->stride : NULL)

/* n components of format fmt (u8, s8, u16, s16, f32) */
static inline void read_comps(const uint8_t *b, unsigned n, uint32_t fmt, float scale, float *o) {
    switch (fmt) {
    case 0: for (unsigned i = 0; i < n; i++) o[i] = (float)b[i] * scale; break;
    case 1: for (unsigned i = 0; i < n; i++) o[i] = (float)(int8_t)b[i] * scale; break;
    case 2: for (unsigned i = 0; i < n; i++) o[i] = (float)be16(b + 2 * i) * scale; break;
    case 3: for (unsigned i = 0; i < n; i++) o[i] = (float)(int16_t)be16(b + 2 * i) * scale; break;
    default: for (unsigned i = 0; i < n; i++) o[i] = u2f(be32(b + 4 * i));
    }
}
/* Vertex colour as RGBA8 (same rounding as read_color then * 255 + 0.5). */
static inline void read_color8(const uint8_t *b, uint32_t fmt, uint8_t o[4]) {
#define E(v, n) ((uint8_t)(((v) * 510u + (1u << (n)) - 1) / (2u * ((1u << (n)) - 1))))
    uint32_t v;
    switch (fmt) {
    case 0: v = be16(b); o[0] = E(v >> 11, 5); o[1] = E((v >> 5) & 63, 6); o[2] = E(v & 31, 5); o[3] = 255; break;
    case 1: case 2: o[0] = b[0]; o[1] = b[1]; o[2] = b[2]; o[3] = 255; break;
    case 3: v = be16(b); o[0] = (uint8_t)((v >> 12) * 17); o[1] = (uint8_t)(((v >> 8) & 15) * 17); o[2] = (uint8_t)(((v >> 4) & 15) * 17); o[3] = (uint8_t)((v & 15) * 17); break;
    case 4: v = (uint32_t)b[0] << 16 | (uint32_t)b[1] << 8 | b[2];
        o[0] = E(v >> 18, 6); o[1] = E((v >> 12) & 63, 6); o[2] = E((v >> 6) & 63, 6); o[3] = E(v & 63, 6); break;
    default: memcpy(o, b, 4);
    }
#undef E
}

static void read_color(const uint8_t *b, uint32_t fmt, float o[4]) {
#define N(v, bits_) ((float)(v) / (float)((1u << (bits_)) - 1))
    uint32_t v;
    switch (fmt) {
    case 0: v = be16(b); o[0] = N(v >> 11, 5); o[1] = N((v >> 5) & 63, 6); o[2] = N(v & 31, 5); o[3] = 1; break;
    case 1: case 2: o[0] = N(b[0], 8); o[1] = N(b[1], 8); o[2] = N(b[2], 8); o[3] = 1; break;
    case 3: v = be16(b); o[0] = N(v >> 12, 4); o[1] = N((v >> 8) & 15, 4); o[2] = N((v >> 4) & 15, 4); o[3] = N(v & 15, 4); break;
    case 4: v = (uint32_t)b[0] << 16 | (uint32_t)b[1] << 8 | b[2];
        o[0] = N(v >> 18, 6); o[1] = N((v >> 12) & 63, 6); o[2] = N((v >> 6) & 63, 6); o[3] = N(v & 63, 6); break;
    default: o[0] = N(b[0], 8); o[1] = N(b[1], 8); o[2] = N(b[2], 8); o[3] = N(b[3], 8);
    }
#undef N
}

static inline float xf_f(unsigned addr) { return addr < 0x1000 ? u2f(G.xf_mem[addr]) : 0.0f; }
static void xf_block(unsigned base, float o[12]) { for (int k = 0; k < 12; k++) o[k] = xf_f(base + (unsigned)k); }

static void mtx_invalidate(uint64_t m) { G.mtx_valid &= ~m; G.slot_valid &= ~m; }
/* bits r-2..r (the matrices whose 3 groups include group r), clipped to 0..lim-1 */
static uint64_t mtx_mask(unsigned r, unsigned lim) {
    uint64_t m = 0;
    for (unsigned k = r >= 2 ? r - 2 : 0; k <= r && k < lim; k++) m |= 1ull << k;
    return m;
}
static void xf_write(uint32_t addr, uint32_t v) {
    G.xf_gen++;
    if (addr < 0x1000) {
        if (G.xf_mem[addr] == v) return; /* J3D reloads unchanged matrices all the time */
        G.xf_mem[addr] = v;
        if (addr < 0x100) {
            /* position matrix i covers words 4i..4i+11 */
            unsigned r = addr >> 2;
            mtx_invalidate(mtx_mask(r, 64));
            if (G.dep_mtx >> r & 1) G.xf_dirty = true;
        } else if (addr >= 0x400 && addr < 0x460) {
            /* normal matrix j (of indices j and j + 32) covers words 3j..3j+8 */
            unsigned r = (addr - 0x400) / 3;
            uint64_t m = mtx_mask(r, 32);
            mtx_invalidate(m | m << 32);
            if (G.dep_nrm >> r & 1) G.xf_dirty = true;
        } else if (addr >= 0x500 && addr < 0x600) {
            if (G.dep_post >> ((addr - 0x500) >> 2) & 1) G.xf_dirty = true;
        } else if (addr >= 0x600 && addr < 0x680) G.light_gen++;
    } else if (addr >= 0x1000 && addr < 0x1100) {
        if (G.xf_reg[addr - 0x1000] == v) return;
        G.xf_reg[addr - 0x1000] = v;
        if (addr >= 0x1009 && addr <= 0x1011) G.light_gen++;
        /* matrix index registers 0x1018/0x1019 are not read (CP 0x30/0x40 are) */
        if (addr != 0x1018 && addr != 0x1019) G.xf_dirty = true;
    }
}

/* Position (12) + normal (9) matrix for matrix index pmtx (cached until matrix memory changes). */
static const float *matrices(uint32_t pmtx) {
    unsigned i = pmtx & 63;
    if (!(G.mtx_valid & (1ull << i))) {
        float *m = G.mtx_cache[i];
        for (unsigned k = 0; k < 12; k++) m[k] = xf_f(i * 4 + k);
        unsigned nb = 0x400 + (i & 31) * 3;
        for (unsigned k = 0; k < 9; k++) m[12 + k] = xf_f(nb + k);
        G.mtx_valid |= 1ull << i;
    }
    return G.mtx_cache[i];
}

static void vc_frame(uint32_t frame_verts);
static void end_frame(void) {
    /* a movie is playing while frames were decoded in the last half second */
    static uint64_t thp_seen;
    if (g_thp_frames != thp_seen) { thp_seen = g_thp_frames; G.movie = 30; }
    else if (G.movie) G.movie--;
    u64map_clear(&G.tex_memo);
    G.state_gen++;
    uint32_t ar = G.bp[0x4F], gb = G.bp[0x50], size = G.bp[0x4A];
    float w = (float)(bits(size, 0, 10) + 1), h = (float)(bits(size, 10, 10) + 1);
    if (w >= 64 && h >= 64) { G.efb[0] = w; G.efb[1] = h; }
    Frame *f = G.frame;
    f->efb[0] = G.efb[0];
    f->efb[1] = G.efb[1];
    f->clear[0] = (float)(ar & 0xFF) / 255.0f;
    f->clear[1] = (float)((gb >> 8) & 0xFF) / 255.0f;
    f->clear[2] = (float)(gb & 0xFF) / 255.0f;
    f->clear[3] = (float)((ar >> 8) & 0xFF) / 255.0f;
    vc_frame((uint32_t)f->nverts);
    publish(f);
    s_vb_frame = s_rec_frame = s_tmi_frame = NULL;
    G.slot_frame = G.lblock_frame = NULL;
    G.frame = frame_new();
    s_frames_decoded++;
}

static uint64_t copy_sig(uint32_t addr, size_t len);
static void bp_write(uint32_t v) {
    G.state_gen++;
    uint32_t reg = v >> 24, val = v & 0xFFFFFF;
    if (reg == 0xFE) { G.bp_mask = val; return; }
    uint32_t nw = (G.bp[reg] & ~G.bp_mask) | (val & G.bp_mask);
    G.bp_mask = 0xFFFFFF;
    G.bp[reg] = nw;
    switch (reg) {
    case 0x45: push_event(GXEV_DRAW_DONE, 0); break;
    case 0x47: push_event(GXEV_TOKEN, (uint16_t)nw); break;
    case 0x48: push_event(GXEV_TOKEN_INT, (uint16_t)nw); break;
    case 0x52: {
        EfbCopy cp = {(uint32_t)G.frame->batches.len, nw, G.bp[0x49], G.bp[0x4A], G.bp[0x4B],
                      {G.bp[0x43], G.bp[0x4F], G.bp[0x50], G.bp[0x51], G.bp[0x40], G.bp[0x41], G.bp[0x53], G.bp[0x54]}};
        vec_push(&G.frame->copies, cp);
        if (nw & (1u << 14)) end_frame();
        else {
            uint32_t dst = gx_phys(G.bp[0x4B]), sz = G.bp[0x4A];
            size_t cw = bits(sz, 0, 10) + 1, ch = bits(sz, 10, 10) + 1;
            if ((nw >> 9) & 1) { cw = cw / 2 ? cw / 2 : 1; ch = ch / 2 ? ch / 2 : 1; }
            size_t len = cw * ch / 2; /* the smallest copy format (4 bits a pixel): inside the copy */
            if (len < 8) len = 8;
            if (dst < RAM_SIZE && len > RAM_SIZE - dst) len = RAM_SIZE - dst;
            if (dst < RAM_SIZE) {
                u64map_put(&G.copy_targets, dst, copy_sig(dst, len));
                u64map_put(&G.copy_len, dst, len);
            }
            u64map_clear(&G.tex_memo);
            G.state_gen++;
        }
        break;
    }
    case 0x65: {
        /* TX_LOADTLUT1: copy a palette from RAM into TMEM. */
        u64map_clear(&G.tex_memo);
        G.state_gen++;
        uint32_t src = gx_phys(G.bp[0x64]);
        uint32_t dst = (nw & 0x3FF) << 9;
        uint32_t len = ((nw >> 10) & 0x7FF) * 32;
        if (dst >= TMEM_SIZE) break;
        if (len > TMEM_SIZE - dst) len = TMEM_SIZE - dst;
        uint32_t avail = src < RAM_SIZE ? RAM_SIZE - src : 0;
        if (avail > len) avail = len;
        static unsigned logged;
        if (avail < len && logged++ < 20) rt_log("gx: palette load at %#x reads past the end of RAM", src);
        if (avail) memcpy(G.tmem + dst, g_ram + src, avail);
        memset(G.tmem + dst + avail, 0, len - avail); /* past the end of RAM reads as zero */
        break;
    }
    default:
        if (reg >= 0xE0 && reg <= 0xE7) {
            unsigned i = (reg - 0xE0) / 2;
            bool konst = (nw >> 23) & 1;
            float *d = konst ? G.konst[i] : G.tev_reg[i];
            if (!(reg & 1)) { d[0] = sext11(nw); d[3] = sext11(nw >> 12); }
            else { d[2] = sext11(nw); d[1] = sext11(nw >> 12); }
        }
    }
}

/* Sampled signature of RAM at an EFB copy's destination. Copies go to GL render targets, never
 * to RAM, so while the RAM there is unchanged a texture read from that address means the copy.
 * Once the CPU writes new data there (the memory reused after a scene change: character skins,
 * the title picture), it is an ordinary texture again: treating every address that ever received
 * a copy as a render target for good drew those textures as an old shadow buffer (flat white). */
static uint64_t copy_sig(uint32_t addr, size_t len) {
    const uint8_t *d = g_ram + addr;
    uint64_t h = 0xCBF29CE484222325ull ^ len;
    size_t words = len / 8, step = words / 64 ? words / 64 : 1;
    for (size_t i = 0; i < words; i += step) { uint64_t w; memcpy(&w, d + 8 * i, 8); h = (h ^ w) * 0x100000001B3ull; h ^= h >> 29; }
    return h | 1;
}
uint64_t g_gx_copy_targets_dropped;

/* (block width, block height, bytes per block) for a GX texture format. */
static bool block_info(uint32_t fmt, uint32_t *bw, uint32_t *bh, uint32_t *bytes) {
    switch (fmt) {
    case 0: case 8: *bw = 8; *bh = 8; *bytes = 32; return true;      /* I4, C4 */
    case 1: case 2: case 9: *bw = 8; *bh = 4; *bytes = 32; return true; /* I8, IA4, C8 */
    case 3: case 4: case 5: case 10: *bw = 4; *bh = 4; *bytes = 32; return true; /* IA8, RGB565, RGB5A3, C14X2 */
    case 6: *bw = 4; *bh = 4; *bytes = 64; return true;              /* RGBA8 */
    case 14: *bw = 8; *bh = 8; *bytes = 32; return true;             /* CMPR */
    default: return false;
    }
}
static size_t tex_size(uint32_t fmt, uint32_t w, uint32_t h) {
    uint32_t bw, bh, by;
    if (!block_info(fmt, &bw, &bh, &by)) return 0;
    return (size_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * by;
}
size_t gx_tex_size(uint32_t fmt, uint32_t w, uint32_t h) { return tex_size(fmt, w, h); }

static struct { uint32_t fmt, w, h; uint64_t hash; uint16_t id; } s_subst[GX_SUBST_MAX];
static unsigned s_nsubst;
void gx_subst_register(unsigned id, uint32_t fmt, uint32_t w, uint32_t h, uint64_t hash) {
    if (s_nsubst < GX_SUBST_MAX) s_subst[s_nsubst++] = (__typeof__(s_subst[0])){fmt, w, h, hash, (uint16_t)id};
}
/* New textures only (cache misses): the full hash runs only when format and size match. */
static volatile uint32_t s_movie[8][3]; /* recent movie plane sets (Y, U, V addresses) */
static volatile unsigned s_nmovie;
void gx_movie_planes(const uint32_t virt[3]) {
    const uint32_t planes[3] = {virt[0] & 0x01FFFFFFu, virt[1] & 0x01FFFFFFu, virt[2] & 0x01FFFFFFu}; /* as the texture registers hold them */
    for (unsigned i = 0; i < 8; i++)
        if (s_movie[i][0] == planes[0] && s_movie[i][1] == planes[1] && s_movie[i][2] == planes[2]) return;
    unsigned i = s_nmovie++ % 8;
    s_movie[i][0] = 0; /* never matched half-written */
    s_movie[i][2] = planes[2];
    s_movie[i][1] = planes[1];
    s_movie[i][0] = planes[0];
}
static uint8_t movie_plane(uint32_t addr) {
    for (unsigned i = 0; i < 8; i++)
        for (unsigned k = 0; k < 3; k++)
            if (s_movie[i][0] && s_movie[i][k] == addr) return (uint8_t)(k + 1);
    return 0;
}

/* textures drawn as a substitute by where they are (copies made for that, gx_subst_addr) */
static struct { uint32_t addr; uint16_t id; } s_subst_at[64];
static unsigned s_nsubst_at;
void gx_subst_addr(uint32_t addr, unsigned id) {
    addr &= 0x01FFFFFF;
    for (unsigned i = 0; i < s_nsubst_at; i++)
        if (s_subst_at[i].addr == addr) { s_subst_at[i].id = (uint16_t)id; return; }
    if (s_nsubst_at < sizeof s_subst_at / sizeof s_subst_at[0]) s_subst_at[s_nsubst_at++] = (__typeof__(s_subst_at[0])){addr, (uint16_t)id};
}
static uint16_t subst_id(uint32_t fmt, uint32_t w, uint32_t h, const uint8_t *data, size_t size);
/* native.c's hud_face asks every frame for each face on the HUD: answers are kept per picture and
 * redone only when 64 words spread over its bytes, or the substitute list, change. */
static struct { uint32_t addr, fmt, w, h; unsigned nsubst; uint64_t sample; uint16_t id; } s_at_memo[16];
static unsigned s_at_next;
static uint64_t sample_bytes(const uint8_t *d, size_t size) {
    uint64_t h = FNV_INIT;
    for (size_t i = 0; i < 64; i++) {
        uint64_t v;
        memcpy(&v, d + (size - 8) * i / 63, 8);
        h = (h ^ v) * 0x100000001b3ull;
    }
    return h;
}
unsigned gx_subst_at(uint32_t addr, uint32_t fmt, uint32_t w, uint32_t h) {
    addr &= 0x01FFFFFF;
    size_t size = tex_size(fmt, w, h);
    if (!size || addr + size > RAM_SIZE) return 0;
    const uint8_t *data = g_ram + addr;
    if (size < 1024) return subst_id(fmt, w, h, data, size);
    uint64_t sample = sample_bytes(data, size);
    unsigned n = sizeof s_at_memo / sizeof s_at_memo[0];
    for (unsigned i = 0; i < n; i++)
        if (s_at_memo[i].addr == addr && s_at_memo[i].fmt == fmt && s_at_memo[i].w == w && s_at_memo[i].h == h &&
            s_at_memo[i].nsubst == s_nsubst && s_at_memo[i].sample == sample)
            return s_at_memo[i].id;
    uint16_t id = subst_id(fmt, w, h, data, size);
    s_at_memo[s_at_next++ % n] = (__typeof__(s_at_memo[0])){addr, fmt, w, h, s_nsubst, sample, id};
    return id;
}
static uint16_t subst_id(uint32_t fmt, uint32_t w, uint32_t h, const uint8_t *data, size_t size) {
    uint64_t hash = 0;
    for (unsigned i = 0; i < s_nsubst; i++) {
        if (s_subst[i].fmt != fmt || s_subst[i].w != w || s_subst[i].h != h) continue;
        if (!hash) hash = fnv_bytes(FNV_INIT, data, size);
        if (hash == s_subst[i].hash) return s_subst[i].id;
    }
    return 0;
}

static uint64_t texture_inner(unsigned map) {
    uint32_t mode_r = map < 4 ? 0x80 + map : 0xA0 + map - 4, img0_r = map < 4 ? 0x88 + map : 0xA8 + map - 4;
    uint32_t img3_r = map < 4 ? 0x94 + map : 0xB4 + map - 4, tlut_r = map < 4 ? 0x98 + map : 0xB8 + map - 4;
    uint32_t img0 = G.bp[img0_r];
    uint32_t w = bits(img0, 0, 10) + 1, h = bits(img0, 10, 10) + 1, fmt = bits(img0, 20, 4);
    uint32_t addr = gx_phys(G.bp[img3_r]);
    if (!addr) return 0;
    uint64_t sig, clen;
    if (u64map_get(&G.copy_targets, addr, &sig)) { /* EFB copy target, while its RAM is untouched */
        if (u64map_get(&G.copy_len, addr, &clen) && copy_sig(addr, (size_t)clen) == sig) return rt_key(addr, G.bp[mode_r]);
        u64map_del(&G.copy_targets, addr);
        u64map_del(&G.copy_len, addr);
        if (g_gx_copy_targets_dropped++ < 20) rt_log("gx: %#x held an EFB copy, now new data: an ordinary texture again", addr);
    }
    size_t size = tex_size(fmt, w, h);
    if (!size || addr + size > RAM_SIZE) {
        static unsigned logged; /* a texture the draw can't show (it samples as black/transparent) */
        if (logged++ < 20) rt_log("gx: texture skipped: fmt %u %ux%u at %#x (%s)", fmt, w, h, addr, size ? "past the end of RAM" : "unknown format");
        return 0;
    }
    uint32_t tlut = G.bp[tlut_r], tlut_addr = (tlut & 0x3FF) << 9, tlut_fmt = bits(tlut, 10, 2);
    const uint8_t *data = g_ram + addr;
    uint32_t hdr[4] = {__builtin_bswap32(addr), __builtin_bswap32(fmt), __builtin_bswap32(w), __builtin_bswap32(h)};
    uint64_t h64 = fnv_bytes(FNV_INIT, hdr, sizeof hdr);
    /* 128 spread 8-byte samples (every word of small textures): each sample is a cache miss */
    size_t words = size / 8, step = words / 128 ? words / 128 : 1;
    for (size_t i = 0; i < words; i += step) { uint64_t w; memcpy(&w, data + 8 * i, 8); h64 = (h64 ^ w) * 0x100000001B3ull; h64 ^= h64 >> 29; }
    for (size_t i = words * 8; i < size; i++) h64 = (h64 ^ data[i]) * 0x100000001B3ull;
    bool paletted = fmt == 8 || fmt == 9 || fmt == 10;
    if (paletted) {
        size_t n = fmt == 8 ? 32 : 512;
        if (tlut_addr + n <= TMEM_SIZE) h64 ^= fnv_bytes(FNV_INIT, G.tmem + tlut_addr, n);
    }
    uint32_t ck_parts[6] = {addr, fmt, w, h, paletted ? tlut_addr : 0, tlut_fmt};
    uint64_t ck = fnv_bytes(FNV_INIT, ck_parts, sizeof ck_parts), cached;
    /* Seen before: reuse the key while the store still holds the texture. The store evicts the
     * oldest additions past its budget, also textures still in use; the renderer keeps its own
     * GL copy only while they are drawn, so after a few demos (characters and the title picture
     * unused for a while) it found nothing for the key and drew them white. Re-captured here. */
    if (u64map_get(&G.tex_cache_hash, ck, &cached) && cached == h64) {
        uint64_t key;
        if (u64map_get(&G.tex_cache_key, ck, &key) && store_has(key)) return key;
    }
    uint32_t mode = G.bp[mode_r];
    GxTexture *t = calloc(1, sizeof *t);
    t->width = w; t->height = h; t->fmt = fmt; t->tlut_fmt = tlut_fmt;
    t->raw = malloc(size);
    memcpy(t->raw, data, size);
    t->raw_len = size;
    t->subst = subst_id(fmt, w, h, data, size);
    for (unsigned i = 0; i < s_nsubst_at; i++)
        if (s_subst_at[i].addr == addr) t->subst = s_subst_at[i].id;
    t->movie = fmt == 1 ? movie_plane(addr) : 0; /* I8 */
    static bool movie_logged;
    if (t->movie && !movie_logged) { movie_logged = true; rt_log("gx: movie frame planes drawn as textures (first: %ux%u), scaled by the renderer", w, h); }
    if (paletted) {
        uint32_t t0 = tlut_addr < TMEM_SIZE - 512 ? tlut_addr : TMEM_SIZE - 512;
        t->tlut = malloc(512);
        memcpy(t->tlut, G.tmem + t0, 512);
    }
    t->wrap_s = (uint8_t)bits(mode, 0, 2);
    t->wrap_t = (uint8_t)bits(mode, 2, 2);
    t->linear = bits(mode, 4, 1);
    uint64_t key = h64 ^ ((uint64_t)mode << 40);
    if (!key) key = 1;
    if ((key & 0xFFFF000000000000ull) == RT_TAG) key ^= 1ull << 63; /* never mistaken for a render target */
    t->key = key;
    add_texture(t);
    u64map_put(&G.tex_cache_hash, ck, h64);
    u64map_put(&G.tex_cache_key, ck, key);
    return key;
}

/* Decode (or reuse) the texture bound to map; returns its cache key (0 = none). */
static uint64_t texture(unsigned map) {
    uint32_t mode_r = map < 4 ? 0x80 + map : 0xA0 + map - 4, img0_r = map < 4 ? 0x88 + map : 0xA8 + map - 4;
    uint32_t img3_r = map < 4 ? 0x94 + map : 0xB4 + map - 4, tlut_r = map < 4 ? 0x98 + map : 0xB8 + map - 4;
    uint32_t memo[4] = {G.bp[mode_r], G.bp[img0_r], G.bp[img3_r], G.bp[tlut_r]};
    uint64_t mk = fnv_bytes(FNV_INIT, memo, sizeof memo), key;
    if (u64map_get(&G.tex_memo, mk, &key)) return key;
    key = texture_inner(map);
    u64map_put(&G.tex_memo, mk, key);
    return key;
}

/* Snapshot the TEV / blend state of the current draw; returns the mask of texgens the stages read. */
static uint8_t material(TevState *t) {
    uint32_t genmode = G.bp[0x00];
    unsigned n = bits(genmode, 10, 4) + 1;
    memset(t, 0, sizeof *t);
    uint8_t maps = 0, uvs = 0;
    for (unsigned s = 0; s < n; s++) {
        uint32_t order = G.bp[0x28 + s / 2] >> (12 * (s & 1));
        uint32_t map = bits(order, 0, 3), tc = bits(order, 3, 3), en = bits(order, 6, 1), chan = bits(order, 7, 3);
        if (en) { maps |= (uint8_t)(1u << map); uvs |= (uint8_t)(1u << tc); }
        uint32_t ksel = G.bp[0xF6 + s / 2];
        uint32_t kc = s & 1 ? bits(ksel, 14, 5) : bits(ksel, 4, 5), ka = s & 1 ? bits(ksel, 19, 5) : bits(ksel, 9, 5);
        t->stages[s][0] = G.bp[0xC0 + 2 * s];
        t->stages[s][1] = G.bp[0xC1 + 2 * s];
        t->stages[s][2] = map | (tc << 4) | (en << 7) | (chan << 8);
        t->stages[s][3] = kc | (ka << 8);
    }
    for (unsigned map = 0; map < 8; map++)
        if (maps & (1u << map)) t->textures[map] = texture(map);
    uint32_t swap = 0;
    for (unsigned tbl = 0; tbl < 4; tbl++) {
        uint32_t rg = G.bp[0xF6 + 2 * tbl], ba = G.bp[0xF7 + 2 * tbl];
        uint32_t packed = bits(rg, 0, 2) | (bits(rg, 2, 2) << 2) | (bits(ba, 0, 2) << 4) | (bits(ba, 2, 2) << 6);
        swap |= packed << (8 * tbl);
    }
    memcpy(t->regs, G.tev_reg, sizeof t->regs);
    memcpy(t->konst, G.konst, sizeof t->konst);
    t->params[0] = n; t->params[1] = G.bp[0xF3]; t->params[2] = swap;
    /* Fog (BP 0xEE-0xF2): A and C are packed floats (11-bit mantissa, 8-bit exponent, sign). */
#define PACKED_FLOAT(v) u2f((bits(v, 19, 1) << 31) | (bits(v, 11, 8) << 23) | (bits(v, 0, 11) << 12))
    uint32_t p3 = G.bp[0xF1];
    t->fog[0] = PACKED_FLOAT(G.bp[0xEE]);
    t->fog[1] = PACKED_FLOAT(p3);
    t->fog[2] = (float)(G.bp[0xEF] & 0xFFFFFF);
    t->fog[3] = (float)bits(G.bp[0xF0], 0, 5);
#undef PACKED_FLOAT
    uint32_t fc = G.bp[0xF2];
    t->fog_color[0] = (float)bits(fc, 16, 8) / 255.0f;
    t->fog_color[1] = (float)bits(fc, 8, 8) / 255.0f;
    t->fog_color[2] = (float)bits(fc, 0, 8) / 255.0f;
    t->fog_color[3] = (float)(bits(p3, 21, 3) | (bits(p3, 20, 1) << 3));
    uint32_t zmode = G.bp[0x40], blend_reg = G.bp[0x41];
    t->pipe[0] = zmode;
    t->pipe[1] = blend_reg;
    t->pipe[2] = bits(G.bp[0x00], 14, 2); /* cull: 0 none, 1 back, 2 front (hardware encoding) */
    t->pipe[3] = bits(G.bp[0x43], 0, 3) | bits(G.bp[0x42], 0, 9) << 8; /* EFB pixel format, constant dst alpha */
    bool blending = (blend_reg & 1) || bits(blend_reg, 11, 1);
    t->blend = blending || !(zmode & 1) ? BLEND_SORTED : BLEND_OPAQUE;
    return uvs;
}

/* 64-bit word hash (the TEV snapshot is ~500 bytes and rehashed at every state change). */
static uint64_t hash_words(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (; n >= 8; n -= 8, b += 8) {
        uint64_t w;
        memcpy(&w, b, 8);
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    for (; n; n--, b++) h = (h ^ *b) * 0x100000001B3ull;
    return h;
}
static uint64_t tev_key(const TevState *t) {
    uint64_t h = hash_words(FNV_INIT, t->stages, sizeof t->stages);
    h = hash_words(h, t->regs, sizeof t->regs);
    h = hash_words(h, t->konst, sizeof t->konst);
    h = hash_words(h, t->params, sizeof t->params);
    h = hash_words(h, t->pipe, sizeof t->pipe);
    h = hash_words(h, t->fog, sizeof t->fog);
    h = hash_words(h, t->fog_color, sizeof t->fog_color);
    h = hash_words(h, t->textures, sizeof t->textures);
    return hash_words(h, &t->blend, sizeof t->blend);
}

/* Decode the XF lighting registers and enabled lights for both channels. */
static void light_setup(LightSetup *s) {
    memset(s, 0, sizeof *s);
    s->chans = bits(G.xf_reg[0x09], 0, 2);
    for (unsigned ch = 0; ch < 2; ch++)
        for (unsigned part = 0; part < 2; part++) {
            uint32_t ctrl = G.xf_reg[(part == 0 ? 0x0E : 0x10) + ch];
            LitPart *p = &s->parts[ch][part];
            p->mat_vtx = bits(ctrl, 0, 1);
            p->amb_vtx = bits(ctrl, 6, 1);
            p->lighting = bits(ctrl, 1, 1);
            rgba_u32(G.xf_reg[0x0C + ch], p->mat);
            rgba_u32(G.xf_reg[0x0A + ch], p->amb);
            p->diff_fn = bits(ctrl, 7, 2);
            p->attn_fn = bits(ctrl, 9, 2);
            uint32_t mask = bits(ctrl, 2, 4) | (bits(ctrl, 11, 4) << 4);
            for (unsigned l = 0; l < 8; l++) {
                if (!(mask & (1u << l))) continue;
                unsigned lb = 0x600 + l * 0x10;
                LightParams *lp = &p->lights[p->count++];
                rgba_u32(G.xf_mem[lb + 3], lp->col);
                for (int k = 0; k < 3; k++) {
                    lp->cos[k] = xf_f(lb + 4 + (unsigned)k);
                    lp->dist[k] = xf_f(lb + 7 + (unsigned)k);
                    lp->pos[k] = xf_f(lb + 10 + (unsigned)k);
                    lp->dir[k] = xf_f(lb + 13 + (unsigned)k);
                }
                /* specular attenuation uses the distance coefficients, normalised unless diff_fn 0 */
                if (p->diff_fn == 0) memcpy(lp->k, lp->dist, 12);
                else {
                    float l2 = lp->dist[0] * lp->dist[0] + lp->dist[1] * lp->dist[1] + lp->dist[2] * lp->dist[2];
                    float r = l2 > 0 ? 1.0f / sqrtf(l2) : 1.0f;
                    for (int k = 0; k < 3; k++) lp->k[k] = lp->dist[k] * r;
                }
            }
        }
}

static inline float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline void norm3(const float v[3], float o[3]) {
    float l2 = dot3(v, v);
    if (l2 > 0) { float r = 1.0f / sqrtf(l2); o[0] = v[0] * r; o[1] = v[1] * r; o[2] = v[2] * r; }
    else { o[0] = v[0]; o[1] = v[1]; o[2] = v[2]; }
}
/* roundf (half away from zero) without the libm call. */
static inline float round_fast(float x) { return (float)(int32_t)(x + (x >= 0 ? 0.5f : -0.5f)); }

/* XF colour channel ch (0/1): material/ambient sources, lighting with the enabled lights,
 * diffuse and attenuation functions (same formulas as Dolphin's LightingShaderGen). */
static inline void light_part(const LitPart *p, const float pos[3], const float n[3], const float *vcol, bool has_vcol,
                              float out[4]) {
    const float *a0 = p->amb_vtx && has_vcol ? vcol : p->amb;
    float acc[4] = {a0[0], a0[1], a0[2], a0[3]};
    for (unsigned li = 0; li < p->count; li++) {
        const LightParams *l = &p->lights[li];
        float to_light[3] = {l->pos[0] - pos[0], l->pos[1] - pos[1], l->pos[2] - pos[2]};
        float ldir[3], attn;
        if (p->attn_fn == 1) {
            /* Specular: the light's "direction" is the half-angle vector. */
            norm3(to_light, ldir);
            float a = dot3(n, ldir) >= 0 ? fmaxf(dot3(n, l->dir), 0) : 0;
            float num = fmaxf(l->cos[0] + l->cos[1] * a + l->cos[2] * a * a, 0);
            float den = l->k[0] + l->k[1] * a + l->k[2] * a * a;
            attn = den != 0 ? num / den : 0;
        } else if (p->attn_fn == 3) {
            /* Spot: angular (cos) and distance attenuation. */
            float dist2 = dot3(to_light, to_light), dist = sqrtf(dist2);
            if (dist > 0) { float r = 1.0f / dist; ldir[0] = to_light[0] * r; ldir[1] = to_light[1] * r; ldir[2] = to_light[2] * r; }
            else memcpy(ldir, to_light, 12);
            float cc = fmaxf(dot3(ldir, l->dir), 0);
            float num = fmaxf(l->cos[0] + l->cos[1] * cc + l->cos[2] * cc * cc, 0);
            float den = l->dist[0] + l->dist[1] * dist + l->dist[2] * dist2;
            attn = den != 0 ? num / den : 0;
        } else {
            /* None / directional: no attenuation. */
            norm3(to_light, ldir);
            if (dot3(ldir, ldir) == 0) memcpy(ldir, n, 12);
            attn = 1;
        }
        float ndl = dot3(ldir, n);
        float diff = p->diff_fn == 0 ? 1 : p->diff_fn == 1 ? ndl : fmaxf(ndl, 0);
        /* Each light's contribution is rounded to the 8-bit accumulator. */
        float s = attn * diff * 255.0f;
        for (int k = 0; k < 4; k++) acc[k] += round_fast(s * l->col[k]) * (1.0f / 255.0f);
    }
    for (int k = 0; k < 4; k++) out[k] = clampf(acc[k], 0, 1);
}

static inline void light_channel(const LightSetup *s, unsigned ch, const float pos[3], const float n[3], const float vcol[4],
                                 bool has_vcol, float out[4]) {
    if (ch >= (s->chans ? s->chans : 1)) { out[0] = out[1] = out[2] = out[3] = 0; return; }
    const LitPart *pc = &s->parts[ch][0], *pa = &s->parts[ch][1];
    const float *mc = pc->mat_vtx && has_vcol ? vcol : pc->mat, *ma = pa->mat_vtx && has_vcol ? vcol : pa->mat;
    float lit[4];
    if (pc->lighting) {
        light_part(pc, pos, n, vcol, has_vcol, lit);
        out[0] = lit[0] * mc[0]; out[1] = lit[1] * mc[1]; out[2] = lit[2] * mc[2];
    } else { out[0] = mc[0]; out[1] = mc[1]; out[2] = mc[2]; }
    if (pa->lighting) { light_part(pa, pos, n, vcol, has_vcol, lit); out[3] = lit[3] * ma[3]; }
    else out[3] = ma[3];
}
/* Texgen: matrix x source row (position, normal or texcoord), the optional dual-texture post
 * transform, projected if STQ. */
static inline void texgen(const TexGen *g, const float m[12], const float pos[3], const float nrm[3], float tc[8][2], float out[2]) {
    float in[4];
    if (g->row == 0) { in[0] = pos[0]; in[1] = pos[1]; in[2] = pos[2]; in[3] = 1; }
    else if (g->row == 1) { in[0] = nrm[0]; in[1] = nrm[1]; in[2] = nrm[2]; in[3] = 1; }
    else if (g->row >= 5 && g->row <= 12) { in[0] = tc[g->row - 5][0]; in[1] = tc[g->row - 5][1]; in[2] = 1; in[3] = 1; }
    else { in[0] = 0; in[1] = 0; in[2] = 1; in[3] = 1; }
    if (g->ty != 0) { out[0] = in[0]; out[1] = in[1]; return; } /* emboss / colour texgens: pass through */
#define ROW(k) (m[4 * (k)] * in[0] + m[4 * (k) + 1] * in[1] + m[4 * (k) + 2] * in[2] + m[4 * (k) + 3] * in[3])
    float v[3] = {ROW(0), ROW(1), g->stq ? ROW(2) : 1.0f};
#undef ROW
    if (g->dual) {
        if (g->normalize) {
            float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (len > 1e-12f) { v[0] /= len; v[1] /= len; v[2] /= len; }
        }
        const float *p = g->post;
        float q[3];
        for (int k = 0; k < 3; k++) q[k] = p[4 * k] * v[0] + p[4 * k + 1] * v[1] + p[4 * k + 2] * v[2] + p[4 * k + 3];
        memcpy(v, q, 12);
    }
    if (g->stq && fabsf(v[2]) > 1e-6f) { out[0] = v[0] / v[2]; out[1] = v[1] / v[2]; return; }
    out[0] = v[0];
    out[1] = v[1];
}

/* Z texture (GXSetZTexture): BP 0xF5 op 1 = add, 2 = replace, using the last TEV stage's
 * texture; the texel at (0,0) is used for the whole draw. Returns op (0 = none). */
static uint32_t ztexture(float *value) {
    uint32_t ztex2 = G.bp[0xF5], op = bits(ztex2, 2, 2);
    if (op == 0 || op == 3) return 0;
    uint32_t stage = bits(G.bp[0x00], 10, 4), order = G.bp[0x28 + stage / 2];
    uint32_t map = stage & 1 ? bits(order, 12, 3) : bits(order, 0, 3), on = stage & 1 ? bits(order, 18, 1) : bits(order, 6, 1);
    if (!on) return 0;
    uint32_t img0_r = map < 4 ? 0x88 + map : 0xA8 + map - 4, img3_r = map < 4 ? 0x94 + map : 0xB4 + map - 4;
    uint32_t fmt = bits(G.bp[img0_r], 20, 4), addr = gx_phys(G.bp[img3_r]);
    if (addr + 64 > RAM_SIZE) return 0;
    const uint8_t *d = g_ram + addr;
    uint32_t texel;
    switch (bits(ztex2, 0, 2)) {
    case 0: texel = d[0]; break;
    case 1: texel = (uint32_t)d[0] << 8 | d[1]; break;
    default: texel = fmt == 6 ? (uint32_t)d[1] << 16 | (uint32_t)d[32] << 8 | d[33] : (uint32_t)d[0] << 16 | (uint32_t)d[1] << 8 | d[2];
    }
    *value = (float)((texel + (G.bp[0xF4] & 0x00FFFFFFu)) & 0x00FFFFFFu);
    return op;
}

/* Screen-space offset subtracted from window coordinates (BP 0x59, units of 2 px, biased by 342). */
static void screen_offset(float *x, float *y) {
    uint32_t r = G.bp[0x59];
    if (!r) { *x = 342; *y = 342; return; }
    *x = (float)(bits(r, 0, 10) * 2);
    *y = (float)(bits(r, 10, 10) * 2);
}

/* Scissor box (BP 0x20/0x21) intersected with the viewport, in EFB pixels. */
/* classic places: race2d.arc p1_race.blo */
const HudMove g_hud_move[MG_COUNT] = {
    [MG_PLACE] = {23, 16, 10, 8, 0.6f, 5},      /* 23-119 x 16-112 */
    [MG_ITEMS] = {23, 16, 18, 8, 0.6f, 5},      /* 118-273 x 5-98: a gap after the place */
    [MG_LAP] = {27, 384, 204.5f, 6, 0.6f, 4},   /* 27-132 x 384-428: ends at 267.5 */
    [MG_TIME] = {422, 23, 283.5f, 7.5f, 0.75f, 4}, /* digits 422-582 x 25-49 ("TIME" not drawn): 283.5-403.5, top with the word LAP */
    [MG_MAP] = {602, 40.8f, 610, -12, 0.7f, 6},    /* drawn 457-602 x 41-331, the course's picture starts lower */
    [MG_MAP_BATTLE] = {602, 40.8f, 610, 128, 0.7f, 6}, /* 128-331: between the shine (to 124) and the speed (from 360) */
    [MG_ORDER] = {19, 114, 10, 263, 0.55f, 5},  /* rows 62.3 apart from 114; a 5th ends at 440 */
    [MG_SPEED] = {601, 446, 612, 455, 0.8f, 6}, /* 484-601 x 328-446 (its lowest block 436) */
    /* two players, p2_race.blo: player 1 at the top of the top screen, player 2 at the bottom of the
     * bottom one (place and items at the left, the lap count by the place: under it at the top, above
     * it at the bottom, the word LAP not drawn; the timer in the outer right corner), the map and the
     * race order between them at the edges */
    [MG2_PLACE1] = {525, 48, 10, 8, 0.75f, 5},     /* 525-583 x 48-106: 10-53.5 x 8-51.5 */
    [MG2_ITEMS1] = {81, 17, 58, 13, 0.75f, 5},     /* 78-196 x 1-84 */
    [MG2_LAP1] = {27, 44, 12, 56, 0.8f, 5},        /* the count 27-68 x 44-82: 12-45 x 56-86, under the place */
    [MG2_TIME1] = {593, 22, 600, 8, 0.8f, 6},      /* digits 434-593 x 22-44: 473-600 x 8-26 */
    [MG2_PLACE2] = {525, 347, 10, 396.5f, 0.75f, 5}, /* 10-53.5 x 396.5-440 */
    [MG2_ITEMS2] = {81, 383, 58, 401.4f, 0.75f, 5},
    [MG2_LAP2] = {28, 428, 12, 392.5f, 0.8f, 5},   /* the count 28-68 x 390-428: 12-44 x 362-392.5, above the place */
    [MG2_TIME2] = {593, 427, 600, 440, 0.8f, 6},   /* digits 434-593 x 405-427: 473-600 x 422-440 */
    [MG2_MAP] = {587, 124, 602, 141, 0.8f, 6},     /* 483-587 x 124-331 */
    [MG2_ORDER] = {19, 97, 8, 152, 0.6f, 5},
    /* four players, p4_race.blo: items in each quarter's outer top corner, the place in its outer
     * bottom corner with LAP beside it (native.c LAP4_*: the count right of the word), the map in
     * the middle; corners: the screen's edges, 6 in from the split at 224 */
    [MG4_PLACE1] = {22, 221, 8, 218, 0.7f, 5},     /* 22-67 x 176-221 */
    [MG4_ITEMS1] = {19, 16, 8, 6, 0.7f, 5},        /* 19-126 x 16-88 */
    [MG4_LAP1] = {22, 221, 8, 218, 0.7f, 5},       /* the place's corner: 68-145 x 184-212 beside it */
    [MG4_PLACE1 + 3] = {586, 222, 600, 218, 0.7f, 6},
    [MG4_ITEMS1 + 3] = {589, 16, 600, 6, 0.7f, 6},
    [MG4_LAP1 + 3] = {586, 222, 600, 218, 0.7f, 6}, /* 461-538, left of the place */
    [MG4_PLACE1 + 6] = {21, 428, 8, 442, 0.7f, 5},
    [MG4_ITEMS1 + 6] = {19, 227, 8, 229, 0.7f, 5},
    [MG4_LAP1 + 6] = {21, 428, 8, 442, 0.7f, 5},
    [MG4_PLACE1 + 9] = {587, 428, 600, 442, 0.7f, 6},
    [MG4_ITEMS1 + 9] = {589, 227, 600, 229, 0.7f, 6},
    [MG4_LAP1 + 9] = {587, 428, 600, 442, 0.7f, 6},
    [MG4_MAP] = {304, 225, 304, 225, 0.75f, 4},    /* 240-368 x 97-353, smaller about its middle */
    /* two players side by side: columns 0-303 and 305-608; each player's place in its column's top
     * left corner, the items beside it, the lap count under it, the timer in the top right corner;
     * the map at the bottom between the columns, the race order bottom left */
    [MGV_PLACE1] = {525, 48, 8, 8, 0.6f, 5},        /* 8-43 x 8-43 */
    [MGV_ITEMS1] = {81, 17, 51.8f, 17.6f, 0.6f, 5}, /* 50-121 x 8-58 */
    [MGV_LAP1] = {27, 44, 8, 52, 0.6f, 5},          /* 8-33 x 52-75 */
    [MGV_TIME1] = {593, 22, 296, 8, 0.6f, 4},       /* 200-296 x 8-21 */
    [MGV_PLACE2] = {525, 347, 313, 8, 0.6f, 4},
    [MGV_ITEMS2] = {81, 383, 356.8f, 17.6f, 0.6f, 4},
    [MGV_LAP2] = {28, 428, 313, 74.8f, 0.6f, 4},    /* the count's bottom left */
    [MGV_TIME2] = {593, 427, 600, 21.2f, 0.6f, 6},  /* the digits' bottom right */
    [MGV_MAP] = {587, 124, 335, 316, 0.6f, 4},      /* 273-335 x 316-440 */
    [MGV_ORDER] = {19, 97, 8, 300, 0.5f, 5},
};
static void pixel_rect(float xoff, float yoff, uint32_t out[4]) {
    uint32_t tl = G.bp[0x20], br = G.bp[0x21];
    float sc[4];
    if (!tl && !br) { sc[0] = 0; sc[1] = 0; sc[2] = 1024; sc[3] = 1024; }
    else {
        sc[0] = (float)bits(tl, 12, 11) - xoff; sc[1] = (float)bits(tl, 0, 11) - yoff;
        sc[2] = (float)bits(br, 12, 11) - xoff + 1; sc[3] = (float)bits(br, 0, 11) - yoff + 1;
    }
    float cx = u2f(G.xf_reg[0x1A + 3]) - xoff, cy = u2f(G.xf_reg[0x1A + 4]) - yoff;
    float hw = fabsf(u2f(G.xf_reg[0x1A])), hh = fabsf(u2f(G.xf_reg[0x1A + 1]));
    float vp[4] = {cx - hw, cy - hh, cx + hw, cy + hh};
    float ew = G.efb[0], eh = G.efb[1];
    uint32_t x0 = (uint32_t)roundf(clampf(fmaxf(sc[0], vp[0]), 0, ew)), y0 = (uint32_t)roundf(clampf(fmaxf(sc[1], vp[1]), 0, eh));
    uint32_t x1 = (uint32_t)roundf(clampf(fminf(sc[2], vp[2]), 0, ew)), y1 = (uint32_t)roundf(clampf(fminf(sc[3], vp[3]), 0, eh));
    out[0] = x0; out[1] = y0; out[2] = x1 > x0 ? x1 : x0; out[3] = y1 > y0 ? y1 : y0;
}

/* The batch to append to: the last one if its key and rect match (keeps draw order), else new. */
static Batch *batch_for(uint64_t key, const TevState *tev, const uint32_t rect[4], uint32_t hud, uint32_t layout, size_t start) {
    Frame *f = G.frame;
    if (f->nverts >= MAX_FRAME_VERTS) return NULL;
    /* Never append across an EFB copy: the copy must see exactly the draws before it. */
    bool after_copy = f->copies.len && f->copies.data[f->copies.len - 1].at_batch == f->batches.len;
    Batch *last = f->batches.len ? &f->batches.data[f->batches.len - 1] : NULL;
    if (!after_copy && last && last->key == key && last->hud == hud && last->layout == layout && !memcmp(last->rect, rect, 16)) return last;
    if (f->batches.len >= MAX_BATCHES) return NULL;
    vec_reserve(&f->batches, 1);
    Batch *b = &f->batches.data[f->batches.len++];
    memset(b, 0, sizeof *b);
    b->key = key;
    b->tev = *tev;
    memcpy(b->rect, rect, 16);
    b->hud = hud;
    b->layout = layout;
    b->vert_first = (uint32_t)(start / gx_layout_stride(layout));
    b->index_first = (uint32_t)f->indices.len;
    return b;
}

/* Everything a draw needs besides its vertex data. Rebuilt only when state it reads changes: BP
 * state, the XF registers and matrix rows it read (G.xf_dirty), the lights, the vertex format,
 * CP matrix indices or the frame; a CP array change only refreshes the attribute readers.
 * MKDD issues ~10k small draws per frame (J3D strips of a few vertices) and reloads position
 * matrices between most of them, which must not cost a setup each. */
typedef struct { const AttrDesc *d; const uint8_t *base; uint32_t stride, limit; unsigned elem; } Reader;
/* One attribute of the vertex stream (build_ops): what to read (mode 1 inline, 2/3 index into
 * reader ri), its format, the inline bytes it covers, and where it goes. */
enum { OP_SKIP, OP_PNMTX, OP_TMTX, OP_POS, OP_NRM, OP_COL, OP_TC };
typedef struct { uint8_t kind, mode, fmt, cnt, inl, ri, dst, uvmask; float scale; } VOp;
typedef struct {
    bool valid, gpu;
    uint64_t state_gen, cp_gen, light_gen, vat_gen;
    uint32_t vat, cp30, cp40;
    uint8_t uv_src;
    bool has_vcol[2], ortho, need_vn, need_nrm;
    TexGen gens[8];
    float tex_mtx[8][12];
    float pr[6];
    float zval, vp_zscale, vp_zoff, ax, bx, ay, by;
    uint32_t zop;
    float fixed_mt[21];
    unsigned nuv, uv_list[8];
    uint8_t tc_need;           /* texcoords read by the texgens (rows 5..12) */
    Reader rd[12];
    VOp ops[24];
    unsigned nops;
    uint32_t layout;           /* compact layout (texgen slots) or GX_LAYOUT_FULL */
    int8_t uv_from[8];         /* texgen slot -> texcoord it passes on (-1: none) */
    uint32_t rect[4];
    Frame *frame;
    uint32_t rec;              /* GPU path: texel offset of the XF record in frame->xf */
    uint64_t vsig;             /* display-list cache: what the conversion depends on (vc_sig) */
    bool vsig_dirty;
} DrawSetup;
static DrawSetup s_ds;

bool g_gx_cpu_xform, g_gx_cpu_xform_forced;
uint64_t g_gx_setups, g_gx_cpu_verts, g_gx_qwait_ns;
static bool s_xfcheck;         /* PC: MKDD_XFCHECK=1 runs both paths per vertex and compares */
static uint64_t s_xfcheck_n, s_xfcheck_bad;

/* Record which XF matrix words the setup read: a later write there rebuilds it. */
static void dep_block(unsigned base) {
    if (base < 0x100) { for (unsigned g = base >> 2; g < (base >> 2) + 3 && g < 64; g++) G.dep_mtx |= 1ull << g; }
    else if (base >= 0x500 && base < 0x600) { for (unsigned g = (base - 0x500) >> 2; g < ((base - 0x500) >> 2) + 3 && g < 64; g++) G.dep_post |= 1ull << g; }
}
static void xf_block_dep(unsigned base, float o[12]) { dep_block(base); xf_block(base, o); }

/* ---- GPU-transform data in frame->xf (RGBA32UI texels) ---- */
static uint32_t xf_alloc(unsigned texels, uint32_t **out) {
    Frame *f = G.frame;
    if (!f->xf.len) { vec_reserve(&f->xf, 4); memset(f->xf.data, 0, 16); f->xf.len = 4; } /* texel 0: unused */
    vec_reserve(&f->xf, (size_t)texels * 4);
    uint32_t off = (uint32_t)(f->xf.len / 4);
    *out = f->xf.data + f->xf.len;
    f->xf.len += (size_t)texels * 4;
    return off;
}
/* Position (3 texels) + normal matrix (3 texels, xyz) for matrix index i, once per frame and
 * matrix contents. */
static uint32_t mtx_slot(unsigned i) {
    i &= 63;
    if (!(G.slot_valid >> i & 1) || G.slot_frame != G.frame) {
        if (G.slot_frame != G.frame) { G.slot_valid = 0; G.slot_frame = G.frame; }
        const float *m = matrices(i);
        uint32_t *o;
        G.slot_off[i] = xf_alloc(6, &o);
        memcpy(o, m, 48);
        for (int r = 0; r < 3; r++) { memcpy(o + 12 + 4 * r, m + 12 + 3 * r, 12); o[15 + 4 * r] = 0; }
        G.slot_valid |= 1ull << i;
    }
    return G.slot_off[i];
}
/* The 8 XF lights (5 texels each: colour, cos, dist, pos, dir), once per frame and light state. */
static uint32_t light_block(void) {
    if (G.lblock_frame != G.frame || G.lblock_gen != G.light_gen) {
        uint32_t *o;
        G.lblock_off = xf_alloc(40, &o);
        for (unsigned l = 0; l < 8; l++) {
            unsigned lb = 0x600 + l * 0x10;
            uint32_t *t = o + 20 * l;
            t[0] = G.xf_mem[lb + 3]; t[1] = t[2] = t[3] = 0;
            for (unsigned k = 0; k < 3; k++) {
                t[4 + k] = G.xf_mem[lb + 4 + k];
                t[8 + k] = G.xf_mem[lb + 7 + k];
                t[12 + k] = G.xf_mem[lb + 10 + k];
                t[16 + k] = G.xf_mem[lb + 13 + k];
            }
            t[7] = t[11] = t[15] = t[19] = 0;
        }
        G.lblock_frame = G.frame;
        G.lblock_gen = G.light_gen;
    }
    return G.lblock_off;
}
/* XF record (layout shared with xf_vs.h):
 *   texel 0-1  projection pr[0..5], viewport z scale, z offset
 *   texel 2    clip mapping ax, bx, ay, by
 *   texel 3    flags (ortho | zop << 1 | chans << 3 | vertex colour 0/1 << 5/6), z texture value,
 *              light block texel offset, texgen mask
 *   texel 4-7  colour channel controls [ch0 colour, ch0 alpha, ch1 colour, ch1 alpha]:
 *              (XF control register, material RGBA8, ambient RGBA8, 0)
 *   texel 8+7i texgen i: (XF texgen info, post info, dual, 0), texture matrix (3), post matrix (3)
 * Consecutive setups that produce the same record share it. */
static uint32_t build_record(const DrawSetup *S) {
    static uint32_t w[32 + 28 * 8], last[32 + 28 * 8];
    static size_t last_len;
    static uint32_t last_off;
    memset(w, 0, sizeof w);
    for (int i = 0; i < 6; i++) w[i] = f2u(S->pr[i]);
    w[6] = f2u(S->vp_zscale); w[7] = f2u(S->vp_zoff);
    w[8] = f2u(S->ax); w[9] = f2u(S->bx); w[10] = f2u(S->ay); w[11] = f2u(S->by);
    uint32_t chans = bits(G.xf_reg[0x09], 0, 2);
    w[12] = (uint32_t)S->ortho | S->zop << 1 | chans << 3 | (uint32_t)S->has_vcol[0] << 5 | (uint32_t)S->has_vcol[1] << 6;
    w[13] = f2u(S->zval);
    w[15] = S->uv_src;
    bool lit = false;
    for (unsigned ch = 0; ch < 2; ch++)
        for (unsigned part = 0; part < 2; part++) {
            uint32_t *p = w + 16 + 4 * (2 * ch + part);
            p[0] = G.xf_reg[(part == 0 ? 0x0E : 0x10) + ch];
            p[1] = G.xf_reg[0x0C + ch];
            p[2] = G.xf_reg[0x0A + ch];
            lit |= (p[0] >> 1) & 1;
        }
    if (lit) w[14] = light_block();
    size_t len = 32;
    for (unsigned i = 0; i < 8; i++) {
        if (!(S->uv_src & (1u << i))) continue;
        uint32_t *t = w + 32 + 28 * i;
        t[0] = G.xf_reg[0x40 + i];
        t[1] = G.xf_reg[0x50 + i];
        t[2] = S->gens[i].dual;
        memcpy(t + 4, S->tex_mtx[i], 48);
        if (S->gens[i].dual) memcpy(t + 16, S->gens[i].post, 48);
        len = 32 + 28 * (i + 1);
    }
    if (s_rec_frame == G.frame && last_len == len && !memcmp(last, w, len * 4)) return last_off;
    uint32_t *o;
    uint32_t off = xf_alloc((unsigned)(len / 4), &o);
    memcpy(o, w, len * 4);
    memcpy(last, w, len * 4);
    last_len = len;
    last_off = off;
    s_rec_frame = G.frame;
    return off;
}

static void setup_readers(DrawSetup *S, const VtxFormat *f) {
    /* Attribute readers: inline offset or indexed array base / stride. */
    const AttrDesc *ads[12] = {&f->pos, &f->nrm, &f->col[0], &f->col[1], &f->tex[0], &f->tex[1], &f->tex[2], &f->tex[3],
                               &f->tex[4], &f->tex[5], &f->tex[6], &f->tex[7]};
    for (unsigned a = 0; a < 12; a++) {
        const AttrDesc *d = ads[a];
        Reader *r = &S->rd[a];
        r->d = d;
        if (d->mode < 2) continue; /* only indexed attributes read the arrays */
        r->elem = a == 0 ? d->count * comp_size(d->fmt) : a == 1 ? 3 * comp_size(d->fmt) : a < 4 ? color_size(d->fmt) : d->count * comp_size(d->fmt);
        uint32_t base = G.cp[0xA0 + a] & 0x03FFFFFFu;
        r->base = g_ram + (base < RAM_SIZE ? base : 0);
        r->stride = G.cp[0xB0 + a];
        r->limit = base < RAM_SIZE ? RAM_SIZE - base : 0;
    }
}

static void draw_setup(DrawSetup *S, const VtxFormat *f, uint8_t uv_src) {
    const LightSetup *lights = &G.light;
    G.dep_mtx = G.dep_post = G.dep_nrm = 0;
    G.xf_dirty = false;
    S->uv_src = uv_src;
    S->has_vcol[0] = f->col[0].mode != 0;
    S->has_vcol[1] = f->col[1].mode != 0;
    S->gpu = !g_gx_cpu_xform && !g_gx_cpu_xform_forced;
    bool dual = G.xf_reg[0x12] & 1;
    memset(S->gens, 0, sizeof S->gens);
    for (unsigned i = 0; i < 8; i++) {
        if (!(uv_src & (1u << i))) continue;
        uint32_t info = G.xf_reg[0x40 + i];
        TexGen *g = &S->gens[i];
        g->stq = bits(info, 1, 1);
        g->ty = bits(info, 4, 3);
        g->row = bits(info, 7, 5);
        /* Dual texture transform (XF 0x1012): optional normalisation, then the post-transform
         * matrix (XF memory 0x500 + 4 * index); J3D uses it for environment mapping. */
        g->dual = dual;
        if (dual) {
            uint32_t post = G.xf_reg[0x50 + i];
            g->normalize = bits(post, 8, 1);
            xf_block_dep(0x500 + bits(post, 0, 6) * 4, g->post);
        }
        uint32_t idx = i < 4 ? bits(G.cp[0x30], 6 + 6 * i, 6) : bits(G.cp[0x40], 6 * (i - 4), 6);
        xf_block_dep(idx * 4, S->tex_mtx[i]);
    }
    for (int i = 0; i < 6; i++) S->pr[i] = u2f(G.xf_reg[0x20 + i]);
    S->ortho = G.xf_reg[0x26] != 0;
    S->zval = 0;
    S->zop = ztexture(&S->zval);
    /* XF viewport: screen = offset + scale * ndc (x/y in EFB pixels + 342, z in 24-bit units). */
    float vp_xs = u2f(G.xf_reg[0x1A]), vp_ys = u2f(G.xf_reg[0x1B]);
    float vp_xo = u2f(G.xf_reg[0x1D]), vp_yo = u2f(G.xf_reg[0x1E]);
    S->vp_zscale = u2f(G.xf_reg[0x1C]);
    S->vp_zoff = u2f(G.xf_reg[0x1F]);
    float efb_w = G.efb[0], efb_h = G.efb[1], xoff, yoff;
    screen_offset(&xoff, &yoff);
    /* Window position inside the EFB, mapped to [-1, 1] over the whole EFB and kept homogeneous
     * so the GPU does the perspective divide and frustum clipping. */
    S->ax = vp_xs / efb_w * 2; S->bx = (vp_xo - xoff) / efb_w * 2 - 1;
    S->ay = -vp_ys / efb_h * 2; S->by = 1 - (vp_yo - yoff) / efb_h * 2;
    if (!f->pnmtx && !S->gpu) {
        unsigned i = G.cp[0x30] & 0x3F;
        memcpy(S->fixed_mt, matrices(i), sizeof S->fixed_mt);
        dep_block(i * 4);
        for (unsigned g = (i & 31); g < (i & 31) + 3; g++) G.dep_nrm |= 1ull << g;
    }
    unsigned active = lights->chans ? lights->chans : 1;
    S->need_vn = false;
    for (unsigned ch = 0; ch < active && ch < 2; ch++) S->need_vn |= lights->parts[ch][0].lighting || lights->parts[ch][1].lighting;
    S->need_nrm = S->need_vn;
    S->nuv = 0;
    S->tc_need = 0;
    for (unsigned i = 0; i < 8; i++) {
        if (!(uv_src & (1u << i))) continue;
        S->uv_list[S->nuv++] = i;
        if (S->gens[i].row == 1 && S->gens[i].ty == 0) S->need_nrm = true;
        if (S->gens[i].row >= 5 && S->gens[i].row <= 12) S->tc_need |= (uint8_t)(1u << (S->gens[i].row - 5));
    }
    setup_readers(S, f);
    pixel_rect(xoff, yoff, S->rect);
    S->frame = G.frame;
    if (S->gpu) S->rec = build_record(S);
}

/* CPU XF stage for one vertex: transform, lighting, texgen (the vertex shader's reference). */
static inline void xform_cpu(const DrawSetup *S, const float *mt, const uint8_t tmtx_vtx[8], const float pos[3], const float nrm[3],
                             float vcol[2][4], float tc[8][2], GpuVertex *v) {
    static const float zero12[12];
    const float *xff = (const float *)G.xf_mem;
    const float *pr = S->pr;
    float view[3] = {
        mt[0] * pos[0] + mt[1] * pos[1] + mt[2] * pos[2] + mt[3],
        mt[4] * pos[0] + mt[5] * pos[1] + mt[6] * pos[2] + mt[7],
        mt[8] * pos[0] + mt[9] * pos[1] + mt[10] * pos[2] + mt[11],
    };
    float vn[3] = {0, 0, 1};
    if (S->need_vn) {
        vn[0] = mt[12] * nrm[0] + mt[13] * nrm[1] + mt[14] * nrm[2];
        vn[1] = mt[15] * nrm[0] + mt[16] * nrm[1] + mt[17] * nrm[2];
        vn[2] = mt[18] * nrm[0] + mt[19] * nrm[1] + mt[20] * nrm[2];
        float l2 = vn[0] * vn[0] + vn[1] * vn[1] + vn[2] * vn[2];
        if (l2 > 1e-12f) { float r = 1.0f / sqrtf(l2); vn[0] *= r; vn[1] *= r; vn[2] *= r; }
    }
    float p[4];
    if (!S->ortho) { p[0] = pr[0] * view[0] + pr[1] * view[2]; p[1] = pr[2] * view[1] + pr[3] * view[2]; p[2] = pr[4] * view[2] + pr[5]; p[3] = -view[2]; }
    else { p[0] = pr[0] * view[0] + pr[1]; p[1] = pr[2] * view[1] + pr[3]; p[2] = pr[4] * view[2] + pr[5]; p[3] = 1; }
    float w = p[3];
    float zc = S->zop == 2 ? S->zval * w : S->zop ? S->vp_zoff * w + S->vp_zscale * p[2] + S->zval * w : S->vp_zoff * w + S->vp_zscale * p[2];
    v->pos[0] = S->ax * p[0] + S->bx * w;
    v->pos[1] = S->ay * p[1] + S->by * w;
    v->pos[2] = zc * (1.0f / 16777215.0f);
    v->pos[3] = w;
    v->rec = XV_PRE;
    float lc[2][4];
    light_channel(&G.light, 0, view, vn, vcol[0], S->has_vcol[0], lc[0]);
    light_channel(&G.light, 1, view, vn, vcol[1], S->has_vcol[1], lc[1]);
    /* channel values are already in [0, 1] (vertex / register colours, clamped lighting) */
    for (int k = 0; k < 4; k++) {
        v->color0[k] = (uint8_t)(lc[0][k] * 255.0f + 0.5f);
        v->color1[k] = (uint8_t)(lc[1][k] * 255.0f + 0.5f);
    }
    for (unsigned u = 0; u < S->nuv; u++) {
        unsigned i = S->uv_list[u];
        const float *tm = S->tex_mtx[i];
        if (tmtx_vtx[i] != 0xFF) { unsigned a = tmtx_vtx[i] * 4u; tm = a + 12 <= 0x1000 ? xff + a : zero12; }
        texgen(&S->gens[i], tm, pos, nrm, tc, v->uv[i]);
    }
}

static void xf_check(const GpuVertex *gv, const GpuVertex *ref, const DrawSetup *S) {
    GpuVertex e;
    gx_xf_eval(G.frame->xf.data, gv, &e);
    s_xfcheck_n++;
    float tol = 1e-4f * (1 + fabsf(ref->pos[3]));
    bool bad = false;
    for (int k = 0; k < 4; k++) bad |= fabsf(e.pos[k] - ref->pos[k]) > tol + 1e-4f * fabsf(ref->pos[k]);
    for (int k = 0; k < 4; k++) bad |= abs((int)e.color0[k] - ref->color0[k]) > 1 || abs((int)e.color1[k] - ref->color1[k]) > 1;
    for (unsigned u = 0; u < S->nuv; u++) {
        unsigned i = S->uv_list[u];
        for (int k = 0; k < 2; k++) bad |= fabsf(e.uv[i][k] - ref->uv[i][k]) > 1e-3f * (1 + fabsf(ref->uv[i][k]));
    }
    if (!bad) return;
    if (s_xfcheck_bad++ < 20)
        rt_log("xfcheck: pos %g %g %g %g vs %g %g %g %g, c0 %02x%02x%02x%02x vs %02x%02x%02x%02x, c1 %02x%02x%02x%02x vs %02x%02x%02x%02x, uv0 %g %g vs %g %g",
               e.pos[0], e.pos[1], e.pos[2], e.pos[3], ref->pos[0], ref->pos[1], ref->pos[2], ref->pos[3],
               e.color0[0], e.color0[1], e.color0[2], e.color0[3], ref->color0[0], ref->color0[1], ref->color0[2], ref->color0[3],
               e.color1[0], e.color1[1], e.color1[2], e.color1[3], ref->color1[0], ref->color1[1], ref->color1[2], ref->color1[3],
               e.uv[0][0], e.uv[0][1], ref->uv[0][0], ref->uv[0][1]);
}

/* Vertex attribute steps of the current setup, in stream order. */
static void build_ops(DrawSetup *S, const VtxFormat *f) {
    unsigned n = 0;
    VOp *op = S->ops;
#define ADD(k_, ri_, dst_, inl_) do { op[n] = (VOp){(k_), 0, 0, 0, (uint8_t)(inl_), (uint8_t)(ri_), (uint8_t)(dst_), 0, 1.0f}; \
        const AttrDesc *d_ = (ri_) < 12 ? S->rd[(ri_)].d : NULL; \
        if (d_) { op[n].mode = (uint8_t)d_->mode; op[n].fmt = (uint8_t)d_->fmt; op[n].cnt = (uint8_t)d_->count; op[n].scale = d_->scale; } n++; } while (0)
    if (f->pnmtx) ADD(OP_PNMTX, 12, 0, 1);
    for (int i = 0; i < 8; i++) if (f->texmtx[i]) ADD(OP_TMTX, 12, i, 1);
    if (f->pos.mode) ADD(OP_POS, 0, 0, f->pos.inl);
    if (f->nrm.mode) ADD(S->need_nrm ? OP_NRM : OP_SKIP, 1, 0, f->nrm.inl);
    for (int ch = 0; ch < 2; ch++) if (f->col[ch].mode) ADD(OP_COL, 2 + ch, ch, f->col[ch].inl);
    for (int i = 0; i < 8; i++) if (f->tex[i].mode) ADD((S->tc_need >> i & 1) ? OP_TC : OP_SKIP, 4 + i, i, f->tex[i].inl);
#undef ADD
    /* merge consecutive skips */
    unsigned m = 0;
    for (unsigned i = 0; i < n; i++) {
        if (op[i].kind == OP_SKIP && m && op[m - 1].kind == OP_SKIP) { op[m - 1].inl = (uint8_t)(op[m - 1].inl + op[i].inl); continue; }
        op[m++] = op[i];
    }
    S->nops = m;
    /* compact layout: texgen slots 0..k-1; slot u reads texcoord uv_from[u] (-1: none) */
    unsigned k = 0;
    for (unsigned i = 0; i < 8; i++) if (S->uv_src >> i & 1) k = i + 1;
    S->layout = S->gpu ? k : GX_LAYOUT_FULL;
    for (unsigned u = 0; u < 8; u++) {
        unsigned row = S->gens[u].row;
        S->uv_from[u] = (S->uv_src >> u & 1) && row >= 5 && row <= 12 ? (int8_t)(row - 5) : (int8_t)-1;
    }
    /* fast path (vtx_fast): the compact layout's uv slots each texcoord goes to */
    for (unsigned i = 0; i < m; i++)
        if (S->ops[i].kind == OP_TC)
            for (unsigned u = 0; u < k; u++)
                if (S->uv_from[u] == (int8_t)S->ops[i].dst) S->ops[i].uvmask |= (uint8_t)(1u << u);
}

static inline const uint8_t *op_src(const VOp *op, const Reader *rd, const uint8_t *c, size_t o) {
    if (op->mode == 1) return c + o;
    const Reader *r = &rd[op->ri];
    uint32_t idx = op->mode == 2 ? c[o] : be16(c + o);
    return attr_idx(r, idx);
}
static inline int16_t nrm_q(float x) {
    float v = x * GX_NRM_SCALE;
    v = v > 32767.0f ? 32767.0f : v < -32767.0f ? -32767.0f : v;
    return (int16_t)(v >= 0 ? v + 0.5f : v - 0.5f);
}

void gx_vertex_unpack(const Frame *f, const Batch *b, uint32_t idx, GpuVertex *out) {
    uint32_t stride = gx_layout_stride(b->layout);
    const uint8_t *p = f->vb.data + (size_t)idx * stride;
    if (b->layout == GX_LAYOUT_FULL) { memcpy(out, p, sizeof *out); return; }
    memset(out, 0, sizeof *out);
    memcpy(out->pos, p, 12);
    out->pos[3] = 1;
    memcpy(&out->rec, p + 12, 4);
    memcpy(&out->mtx, p + 16, 4);
    memcpy(out->color0, p + 20, 4);
    memcpy(out->color1, p + 24, 4);
    int16_t n[3];
    memcpy(n, p + 28, 6);
    for (int k = 0; k < 3; k++) out->nrm[k] = (float)n[k] / GX_NRM_SCALE;
    memcpy(out->uv, p + 36, 8 * b->layout);
}

/* Per-vertex texture matrices: the vertex's matrix field (bit 31 set) points to 3 texels [position
 * slot, texture matrix slot for uv 0..7 (0 = the record's)], shared by consecutive vertices with
 * the same matrices. */
static uint32_t texmtx_slot(uint32_t slot, const uint8_t tmtx_vtx[8]) {
    static uint32_t last_ind[12], last_off, last_slot;
    static uint64_t last_tm, last_gen;
    uint64_t tm;
    memcpy(&tm, tmtx_vtx, 8);
    /* same indices, same matrix slots (slot_valid unchanged since): reuse without rebuilding */
    if (s_tmi_frame != G.frame || tm != last_tm || slot != last_slot || last_gen != G.slot_valid) {
        uint32_t ind[12] = {slot};
        for (int i = 0; i < 8; i++) ind[1 + i] = tmtx_vtx[i] != 0xFF ? mtx_slot(tmtx_vtx[i]) : 0;
        if (s_tmi_frame != G.frame || memcmp(ind, last_ind, sizeof ind)) {
            uint32_t *w;
            last_off = xf_alloc(3, &w);
            memcpy(w, ind, sizeof ind);
            memcpy(last_ind, ind, sizeof ind);
            s_tmi_frame = G.frame;
        }
        last_tm = tm; last_slot = slot; last_gen = G.slot_valid;
    }
    return last_off | 0x80000000u;
}

/* GPU-path vertices: each vertex starts as a copy of the defaults (no position, white colours,
 * +z normal, zero uvs, the fixed matrix slot) and every attribute is written straight into its
 * place in the compact layout. texmtx: per-vertex texture matrices (texmtx_slot); ext: track the
 * screen x extent of HUD draws. Same bytes as the general loop in draw() (MKDD_VTXCHECK=1 on PC
 * compares them). Returns the stream offset after the last vertex. */
static inline __attribute__((always_inline)) size_t vtx_fast_t(const DrawSetup *S, const VtxFormat *f, const uint8_t *c, size_t o,
                                                               unsigned n, uint8_t *out, uint32_t stride, uint32_t rec,
                                                               uint32_t fixed_slot, bool texmtx, bool ext, float *xmn, float *xmx) {
    uint8_t tmpl[36 + 64];
    memset(tmpl, 0, sizeof tmpl);
    memcpy(tmpl + 12, &rec, 4);
    memcpy(tmpl + 16, &fixed_slot, 4);
    memset(tmpl + 20, 0xFF, 8);
    const int16_t nz[3] = {0, 0, (int16_t)GX_NRM_SCALE};
    memcpy(tmpl + 28, nz, 6);
    const Reader *rd = S->rd;
    const VOp *ops = S->ops;
    const unsigned nops = S->nops;
    const uint32_t midx0 = G.cp[0x30] & 0x3F;
    float xmin = *xmn, xmax = *xmx;
    for (unsigned vi = 0; vi < n; vi++) {
        uint8_t *ob = out + (size_t)vi * stride;
        memcpy(ob, tmpl, stride);
        uint32_t midx = midx0;
        uint8_t tm[8];
        if (texmtx) memset(tm, 0xFF, sizeof tm);
        for (unsigned k = 0; k < nops; k++) {
            const VOp *op = &ops[k];
            const uint8_t *b;
            switch (op->kind) {
            case OP_PNMTX:
                midx = c[o] & 0x3F;
                if (!texmtx) { uint32_t slot = mtx_slot(midx); memcpy(ob + 16, &slot, 4); }
                break;
            case OP_TMTX:
                if (texmtx) tm[op->dst] = c[o] & 0x3F;
                break;
            case OP_POS:
                if ((b = op_src(op, rd, c, o))) {
                    float pos[3] = {0, 0, 0};
                    read_comps(b, op->cnt < 3 ? op->cnt : 3, op->fmt, op->scale, pos);
                    memcpy(ob, pos, 12);
                }
                break;
            case OP_NRM:
                if ((b = op_src(op, rd, c, o))) {
                    int16_t nq[3];
                    if (op->fmt == 3 && op->scale == 1.0f / 16384.0f) {
                        for (int q = 0; q < 3; q++) nq[q] = (int16_t)be16(b + 2 * q);
                    } else {
                        float nrm[3];
                        read_comps(b, 3, op->fmt, op->scale, nrm);
                        for (int q = 0; q < 3; q++) nq[q] = nrm_q(nrm[q]);
                    }
                    memcpy(ob + 28, nq, 6);
                }
                break;
            case OP_COL:
                if ((b = op_src(op, rd, c, o))) read_color8(b, op->fmt, ob + 20 + 4 * op->dst);
                break;
            case OP_TC:
                if (op->uvmask && (b = op_src(op, rd, c, o))) {
                    float t[2] = {0, 0};
                    read_comps(b, op->cnt, op->fmt, op->scale, t);
                    for (unsigned u = op->uvmask, s = 0; u; u >>= 1, s++)
                        if (u & 1) memcpy(ob + 36 + 8 * s, t, 8);
                }
                break;
            default: break;
            }
            o += op->inl;
        }
        if (texmtx) {
            uint32_t slot = texmtx_slot(f->pnmtx ? mtx_slot(midx) : fixed_slot, tm);
            memcpy(ob + 16, &slot, 4);
        }
        if (ext) {
            float pos[3];
            memcpy(pos, ob, 12);
            const float *m = matrices(midx);
            float x = S->ax * (S->pr[0] * (m[0] * pos[0] + m[1] * pos[1] + m[2] * pos[2] + m[3]) + S->pr[1]) + S->bx;
            if (x < xmin) xmin = x;
            if (x > xmax) xmax = x;
        }
    }
    *xmn = xmin; *xmx = xmax;
    return o;
}
static size_t vtx_fast(const DrawSetup *S, const VtxFormat *f, const uint8_t *c, size_t o, unsigned n, uint8_t *out,
                       uint32_t stride, uint32_t rec, uint32_t fixed_slot, bool texmtx, bool ext, float *xmn, float *xmx) {
    if (texmtx) {
        if (ext) return vtx_fast_t(S, f, c, o, n, out, stride, rec, fixed_slot, true, true, xmn, xmx);
        return vtx_fast_t(S, f, c, o, n, out, stride, rec, fixed_slot, true, false, xmn, xmx);
    }
    if (ext) return vtx_fast_t(S, f, c, o, n, out, stride, rec, fixed_slot, false, true, xmn, xmx);
    return vtx_fast_t(S, f, c, o, n, out, stride, rec, fixed_slot, false, false, xmn, xmx);
}
/* MKDD_VTXCHECK: the same vertices? Texture-matrix records (matrix field bit 31) may sit at different
 * offsets when both loops ran; compare what they point to. */
static bool vtx_same(const uint8_t *a, const uint8_t *b, unsigned n, uint32_t stride, size_t *at) {
    for (unsigned v = 0; v < n; v++) {
        const uint8_t *x = a + (size_t)v * stride, *y = b + (size_t)v * stride;
        uint32_t sx, sy;
        memcpy(&sx, x + 16, 4);
        memcpy(&sy, y + 16, 4);
        bool slot_ok = sx == sy || ((sx & sy) >> 31 && !memcmp(G.frame->xf.data + 4 * (sx & 0x7FFFFFFFu),
                                                                G.frame->xf.data + 4 * (sy & 0x7FFFFFFFu), 48));
        if (!slot_ok || memcmp(x, y, 16) || memcmp(x + 20, y + 20, stride - 20)) { *at = (size_t)v * stride; return false; }
    }
    return true;
}
static bool s_vtxcheck;        /* PC: MKDD_VTXCHECK=1 runs vtx_fast and the general loop, compares */
static uint64_t s_vtxcheck_n, s_vtxcheck_bad;

/* Material, lights and the draw setup for a draw command (cmd: its VAT in the low bits). */
static DrawSetup *draw_prepare(uint32_t cmd, const VtxFormat **fo) {
    const VtxFormat *f = format(cmd & 7);
    *fo = f;
    if (!G.mat_valid || G.mat_gen != G.state_gen) {
        G.mat_uvs = material(&G.mat_tev);
        G.mat_key = tev_key(&G.mat_tev);
        G.mat_gen = G.state_gen;
        G.mat_valid = true;
    }
    if (!G.light_valid || G.light_cache_gen != G.light_gen) {
        light_setup(&G.light);
        G.light_cache_gen = G.light_gen;
        G.light_valid = true;
    }
    DrawSetup *S = &s_ds;
    if (!S->valid || S->state_gen != G.state_gen || G.xf_dirty || S->light_gen != G.light_gen || S->vat != (cmd & 7) ||
        S->vat_gen != G.vat_gen || S->uv_src != G.mat_uvs || S->cp30 != G.cp[0x30] || S->cp40 != G.cp[0x40] || S->frame != G.frame) {
        draw_setup(S, f, G.mat_uvs);
        build_ops(S, f);
        S->valid = true;
        S->state_gen = G.state_gen; S->cp_gen = G.cp_gen; S->light_gen = G.light_gen; S->vat_gen = G.vat_gen;
        S->vat = cmd & 7; S->cp30 = G.cp[0x30]; S->cp40 = G.cp[0x40];
        S->vsig_dirty = true;
        g_gx_setups++;
    } else if (S->cp_gen != G.cp_gen) {
        setup_readers(S, f);
        S->cp_gen = G.cp_gen;
        S->vsig_dirty = true;
    }
    return S;
}

static inline unsigned prim_tris(uint32_t prim, unsigned n) {
    switch (prim) {
    case 0x80: case 0x88: return n / 4 * 2;
    case 0x90: return n / 3;
    case 0x98: case 0xA0: return n >= 2 ? n - 2 : 0;
    default: return 0; /* lines and points are not drawn */
    }
}

/* ---- display-list cache ----
 * J3D draws each shape with a display list in guest RAM that holds nothing but draw commands
 * (about 30 strips of a few vertices each; matrices and material state are loaded outside it),
 * and calls the same lists with the same vertex arrays every frame: ~400 lists, ~13k draws a
 * frame. Such a list's whole output (converted vertices, indices relative to its first vertex) is
 * kept and appended in one go next time, instead of decoding every draw again. It is reused while
 * the conversion setup is the same (S->vsig: VAT, attribute readers, ops), the list's bytes hash the
 * same, and no memory it read (the list, the array ranges its indices reach) was written since.
 * Writes are seen the way the GPU must see them: DCFlushRange / DCStoreRange (the CPU's vertex
 * writes) and DVD / ARAM DMA into RAM; a large DVD read (a scene load, after which the CPU may
 * unpack data into reused memory) drops everything. The XF record and matrix slots (per frame) are
 * filled in on reuse. Lists with per-vertex texture matrices or 2D draws are not kept.
 * config/novcache (Switch) or MKDD_NOVCACHE (PC) turns it off; MKDD_VCACHECK=1 (PC) decodes every
 * reused list again and compares. */
bool g_gx_vcache = true;
#define VC_PAGE_SHIFT 12
#define VC_PAGES (RAM_SIZE >> VC_PAGE_SHIFT)
#define VC_SLOTS 8192u            /* hash slots (power of two) */
#define VC_MAX_ENTRIES 4096u
#define VC_MAX_BYTES (48u << 20)
#define VC_RANGES 13
static uint32_t s_page_gen[VC_PAGES];
static volatile uint32_t s_dirty_counter = 1, s_dirty_all;
typedef struct {
    uint64_t key, sig, ihash;
    uint32_t off, size, stamp, used;
    uint32_t nverts, stride, nidx, ndraws;
    uint32_t changes, skip_until; /* lists that change every frame are not kept for a while */
    uint32_t nranges, lo[VC_RANGES], hi[VC_RANGES];
    uint8_t *data;     /* nverts * stride converted bytes */
    uint8_t *midx;     /* per-vertex position matrix index (formats with one), or NULL */
    uint32_t *idx;     /* indices relative to the first vertex */
} VcEntry;
static VcEntry *s_vc;
static uint32_t s_vc_n, s_vc_bytes, s_vc_frame;
static int32_t s_vc_slot[VC_SLOTS]; /* entry index + 1, 0 = empty */
static bool s_vc_check;
static uint64_t s_vc_hit, s_vc_lists, s_vc_built, s_vc_changed, s_vc_verts, s_vc_bad;

void gx_mem_dirty(uint32_t addr, uint32_t len) {
    uint32_t off = addr & 0x01FFFFFFu;
    if (!len || off >= RAM_SIZE) return;
    if (len > RAM_SIZE - off) len = RAM_SIZE - off;
    uint32_t gen = __atomic_add_fetch(&s_dirty_counter, 1, __ATOMIC_ACQ_REL);
    for (uint32_t p = off >> VC_PAGE_SHIFT, e = (off + len - 1) >> VC_PAGE_SHIFT; p <= e; p++)
        __atomic_store_n(&s_page_gen[p], gen, __ATOMIC_RELEASE);
}
void gx_mem_dirty_all(void) {
    __atomic_store_n(&s_dirty_all, __atomic_add_fetch(&s_dirty_counter, 1, __ATOMIC_ACQ_REL), __ATOMIC_RELEASE);
}

static void vc_free(VcEntry *e) {
    s_vc_bytes -= e->data ? e->nverts * e->stride + e->nidx * 4 : 0;
    free(e->data); free(e->midx); free(e->idx);
    e->data = e->midx = NULL;
    e->idx = NULL;
}
static void vc_clear(void) {
    for (uint32_t i = 0; i < s_vc_n; i++) vc_free(&s_vc[i]);
    s_vc_n = s_vc_bytes = 0;
    memset(s_vc_slot, 0, sizeof s_vc_slot);
}

static inline uint64_t vc_mix(uint64_t h, uint64_t w) {
    h ^= w * 0x9E3779B97F4A7C15ull;
    return (h << 31 | h >> 33) * 0xBF58476D1CE4E5B9ull;
}
static uint64_t vc_hash(const uint8_t *p, size_t n) {
    uint64_t h = 0x243F6A8885A308D3ull ^ n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, p + i, 8); h = vc_mix(h, w); }
    if (i < n) { uint64_t w = 0; memcpy(&w, p + i, n - i); h = vc_mix(h, w); }
    return h ^ h >> 29;
}

/* What converting a draw depends on besides its bytes and the arrays' contents. */
static uint64_t vc_sig(const DrawSetup *S, const VtxFormat *f, uint32_t stride) {
    uint64_t h = vc_mix(0x13198A2E03707344ull, (uint64_t)S->nops | (uint64_t)stride << 8 | (uint64_t)f->size << 24);
    for (unsigned k = 0; k < S->nops; k++) {
        const VOp *op = &S->ops[k];
        uint64_t a;
        uint32_t sc;
        memcpy(&a, op, 8);
        memcpy(&sc, &op->scale, 4);
        h = vc_mix(vc_mix(h, a), sc);
        if (op->mode >= 2 && op->ri < 12) {
            const Reader *r = &S->rd[op->ri];
            h = vc_mix(vc_mix(h, (uint64_t)(uint32_t)(r->base - g_ram) | (uint64_t)r->stride << 32), r->elem);
        }
    }
    return h;
}

static bool vc_clean(const VcEntry *e) {
    if (__atomic_load_n(&s_dirty_all, __ATOMIC_ACQUIRE) > e->stamp) return false;
    for (uint32_t r = 0; r < e->nranges; r++)
        for (uint32_t p = e->lo[r] >> VC_PAGE_SHIFT, last = (e->hi[r] - 1) >> VC_PAGE_SHIFT; p <= last; p++)
            if (__atomic_load_n(&s_page_gen[p], __ATOMIC_ACQUIRE) > e->stamp) return false;
    return true;
}

static VcEntry *vc_find(uint64_t key) {
    if (!s_vc) return NULL;
    for (uint32_t i = (uint32_t)key & (VC_SLOTS - 1), probe = 0; probe < 16; probe++, i = (i + 1) & (VC_SLOTS - 1)) {
        int32_t s = s_vc_slot[i];
        if (!s) return NULL;
        if (s_vc[s - 1].key == key) return &s_vc[s - 1];
    }
    return NULL;
}

/* Byte offset of the position-matrix index in a vertex, or -1. */
static int vc_pnmtx_off(const DrawSetup *S) {
    unsigned off = 0;
    for (unsigned k = 0; k < S->nops; k++) {
        if (S->ops[k].kind == OP_PNMTX) return (int)off;
        off += S->ops[k].inl;
    }
    return -1;
}

/* The converted vertices with this frame's XF record and matrix slots. */
static void vc_fill(const VcEntry *e, uint8_t *out, uint32_t rec, uint32_t fixed_slot) {
    memcpy(out, e->data, (size_t)e->nverts * e->stride);
    for (uint32_t v = 0; v < e->nverts; v++) {
        uint8_t *ob = out + (size_t)v * e->stride;
        uint32_t slot = e->midx ? mtx_slot(e->midx[v]) : fixed_slot;
        memcpy(ob + 12, &rec, 4);
        memcpy(ob + 16, &slot, 4);
    }
}

/* Append a kept list's output to the frame, as decoding its draws one by one would have. False:
 * not possible here (vertex cap), decode it instead. */
static bool dl_apply(const VcEntry *e, const DrawSetup *S, const VtxFormat *f) {
    Frame *fr = G.frame;
    if (fr->nverts + e->nverts > 400000) return false;
    const uint32_t layout = S->layout, stride = e->stride;
    size_t start = s_vb_frame == fr && s_vb_layout == layout ? fr->vb.len : (fr->vb.len + stride - 1) / stride * stride;
    vec_reserve(&fr->vb, start - fr->vb.len + (size_t)e->nverts * stride);
    vc_fill(e, fr->vb.data + start, S->rec, !f->pnmtx ? mtx_slot(G.cp[0x30] & 0x3F) : 0);
    Batch *bt = batch_for(G.mat_key, &G.mat_tev, S->rect, 0, layout, start);
    if (!bt) return true; /* dropped, as each draw would have been */
    uint32_t base = s_vb_frame == fr && s_vb_layout == layout && start == fr->vb.len ? s_vb_next : (uint32_t)(start / stride);
    fr->vb.len = start + (size_t)e->nverts * stride;
    s_vb_frame = fr; s_vb_layout = layout; s_vb_next = base + e->nverts;
    fr->nverts += e->nverts;
    vec_reserve(&fr->indices, e->nidx);
    uint32_t *ix = fr->indices.data + fr->indices.len;
    for (uint32_t i = 0; i < e->nidx; i++) ix[i] = e->idx[i] + base;
    fr->indices.len += e->nidx;
    bt->vert_count += e->nverts;
    bt->index_count += e->nidx;
    fr->draws += e->ndraws;
    g_gx_draws += e->ndraws;
    return true;
}

/* After decoding list d (addr, size) normally: keep its output if it is a list of draws only and
 * came out as one contiguous run of vertices and indices. Before: the frame's counts before it. */
static VcEntry *dl_store(uint64_t key, uint64_t sig, uint64_t ihash, const DrawSetup *S, const VtxFormat *f, uint32_t addr,
                         uint32_t size, uint32_t stamp, size_t nverts0, size_t ix0, size_t nb0) {
    const uint8_t *d = g_ram + addr;
    Frame *fr = G.frame;
    const uint32_t vat = d[0] & 7, stride = gx_layout_stride(S->layout);
    /* the list: draws (same VAT) and NOPs only; vertices it draws; index ranges per reader */
    uint32_t verts = 0, draws = 0, mn[12], mx[12];
    for (int i = 0; i < 12; i++) { mn[i] = 0xFFFFFFFFu; mx[i] = 0; }
    for (size_t p = 0; p < size;) {
        uint32_t cmd = d[p];
        if (cmd == 0) { p++; continue; }
        if (cmd < 0x80 || cmd > 0xBF || (cmd & 7) != vat || p + 3 > size) return NULL;
        unsigned n = be16(d + p + 1);
        if (p + 3 + (size_t)n * f->size > size) return NULL;
        if (prim_tris(cmd & 0xF8, n)) {
            unsigned off = 0;
            for (unsigned k = 0; k < S->nops; k++) {
                const VOp *op = &S->ops[k];
                if (op->mode >= 2 && op->kind != OP_SKIP && op->ri < 12)
                    for (unsigned v = 0; v < n; v++) {
                        const uint8_t *q = d + p + 3 + (size_t)v * f->size + off;
                        uint32_t ix = op->mode == 2 ? q[0] : be16(q);
                        if (ix < mn[op->ri]) mn[op->ri] = ix;
                        if (ix > mx[op->ri]) mx[op->ri] = ix;
                    }
                off += op->inl;
            }
            verts += n;
            draws++;
        }
        p += 3 + (size_t)n * f->size;
    }
    /* what decoding appended: all of it, in one batch, vertices contiguous */
    size_t nidx = fr->indices.len - ix0;
    if (!verts || fr->nverts - nverts0 != verts || fr->batches.len > nb0 + 1 || !nidx) return NULL;
    uint32_t first = 0xFFFFFFFFu;
    for (size_t i = ix0; i < fr->indices.len; i++) if (fr->indices.data[i] < first) first = fr->indices.data[i];
    if ((size_t)(first + verts) * stride != fr->vb.len) return NULL;
    size_t bytes = (size_t)verts * stride;
    if (s_vc_n >= VC_MAX_ENTRIES || s_vc_bytes + bytes + nidx * 4 > VC_MAX_BYTES) vc_clear();
    if (!s_vc) s_vc = calloc(VC_MAX_ENTRIES, sizeof *s_vc);
    if (!s_vc) return NULL;
    VcEntry *e = vc_find(key);
    if (e) vc_free(e);
    else {
        uint32_t i = (uint32_t)key & (VC_SLOTS - 1), probe = 0;
        while (s_vc_slot[i] && probe < 16) { i = (i + 1) & (VC_SLOTS - 1); probe++; }
        if (s_vc_slot[i]) return NULL; /* crowded */
        s_vc_slot[i] = (int32_t)s_vc_n + 1;
        e = &s_vc[s_vc_n++];
    }
    memset(e, 0, sizeof *e);
    e->key = key; e->sig = sig; e->ihash = ihash; e->off = addr; e->size = size; e->stamp = stamp; e->used = s_vc_frame;
    e->nverts = verts; e->stride = stride; e->nidx = (uint32_t)nidx; e->ndraws = draws;
    e->lo[0] = addr; e->hi[0] = addr + size; e->nranges = 1;
    for (int r = 0; r < 12; r++) {
        if (mn[r] > mx[r]) continue;
        const Reader *rd = &S->rd[r];
        uint64_t lo = (uint64_t)(uint32_t)(rd->base - g_ram) + (uint64_t)mn[r] * rd->stride;
        uint64_t hi = (uint64_t)(uint32_t)(rd->base - g_ram) + (uint64_t)mx[r] * rd->stride + rd->elem;
        if (hi > RAM_SIZE) hi = RAM_SIZE;
        if (lo < hi) { e->lo[e->nranges] = (uint32_t)lo; e->hi[e->nranges] = (uint32_t)hi; e->nranges++; }
    }
    int pm = vc_pnmtx_off(S);
    e->data = malloc(bytes);
    e->idx = malloc(nidx * 4);
    e->midx = pm >= 0 ? malloc(verts) : NULL;
    if (!e->data || !e->idx || (pm >= 0 && !e->midx)) { vc_free(e); e->key = ~0ull; return NULL; }
    memcpy(e->data, fr->vb.data + (size_t)first * stride, bytes);
    for (size_t i = 0; i < nidx; i++) e->idx[i] = fr->indices.data[ix0 + i] - first;
    if (pm >= 0) {
        uint32_t v = 0;
        for (size_t p = 0; p < size;) {
            if (!d[p]) { p++; continue; }
            unsigned n = be16(d + p + 1);
            if (prim_tris(d[p] & 0xF8, n))
                for (unsigned k = 0; k < n; k++) e->midx[v++] = d[p + 3 + (size_t)k * f->size + (unsigned)pm] & 0x3F;
            p += 3 + (size_t)n * f->size;
        }
    }
    s_vc_bytes += (uint32_t)(bytes + nidx * 4);
    return e;
}

static size_t exec(const uint8_t *d, size_t len, unsigned depth);

/* A display-list call (exec, command 0x40): reuse the list's kept output, or decode it and keep it.
 * False: not handled here, decode it normally. */
static bool dl_call(uint32_t addr, uint32_t size, unsigned depth) {
    if (!g_gx_vcache || s_vtxcheck || s_xfcheck || size < 4) return false;
    const uint8_t *d = g_ram + addr;
    if (d[0] < 0x80 || d[0] > 0xBF) return false; /* material and state lists */
    if (bits(G.bp[0x00], 14, 2) == 3) return false;
    const VtxFormat *f;
    DrawSetup *S = draw_prepare(d[0], &f);
    if (!S->gpu || S->ortho) return false;
    for (int i = 0; i < 8; i++) if (f->texmtx[i]) return false;
    if (S->vsig_dirty) { S->vsig = vc_sig(S, f, gx_layout_stride(S->layout)); S->vsig_dirty = false; }
    uint32_t stamp = __atomic_load_n(&s_dirty_counter, __ATOMIC_ACQUIRE);
    uint64_t key = vc_mix(vc_mix(S->vsig, addr), size);
    VcEntry *e = vc_find(key);
    if (e && e->skip_until > s_vc_frame) return false;
    uint64_t ihash = vc_hash(d, size);
    bool hit = e && e->data && e->ihash == ihash && e->sig == S->vsig && e->off == addr && e->size == size && vc_clean(e);
    if (hit && !s_vc_check) {
        if (!dl_apply(e, S, f)) return false;
        e->used = s_vc_frame;
        e->changes = 0;
        s_vc_hit += e->nverts;
        s_vc_lists++;
        return true;
    }
    Frame *fr = G.frame;
    size_t nverts0 = fr->nverts, ix0 = fr->indices.len, nb0 = fr->batches.len;
    exec(d, size, depth);
    s_vc_verts += fr->nverts - nverts0;
    if (hit) { /* MKDD_VCACHECK: what was kept must equal what was just decoded */
        size_t nidx = fr->indices.len - ix0;
        bool same = nidx == e->nidx && fr->nverts - nverts0 == e->nverts;
        uint32_t first = 0xFFFFFFFFu;
        for (size_t i = ix0; same && i < fr->indices.len; i++) if (fr->indices.data[i] < first) first = fr->indices.data[i];
        for (size_t i = 0; same && i < nidx; i++) same = fr->indices.data[ix0 + i] - first == e->idx[i];
        if (same) {
            uint8_t *tmp = malloc((size_t)e->nverts * e->stride);
            vc_fill(e, tmp, S->rec, !f->pnmtx ? mtx_slot(G.cp[0x30] & 0x3F) : 0);
            same = !memcmp(tmp, fr->vb.data + (size_t)first * e->stride, (size_t)e->nverts * e->stride);
            free(tmp);
        }
        if (!same && s_vc_bad++ < 20) rt_log("vcache: CHECK FAILED: list at %#x (%u bytes, %u vertices) differs", addr, size, e->nverts);
        s_vc_hit += e->nverts;
        e->used = s_vc_frame;
        return true;
    }
    if (e) s_vc_changed++;
    uint32_t changes = e ? e->changes + 1 : 0;
    VcEntry *ne = dl_store(key, S->vsig, ihash, S, f, addr, size, stamp, nverts0, ix0, nb0);
    if (ne) {
        s_vc_built++;
        ne->changes = changes;
        if (changes >= 4) { vc_free(ne); ne->changes = 0; ne->skip_until = s_vc_frame + 600; } /* changes every frame */
    }
    return true;
}

/* Per frame (end_frame): statistics now and then; lists unused for 10 s go. */
static void vc_frame(uint32_t frame_verts) {
    s_vc_frame++;
    static uint64_t all_verts;
    all_verts += frame_verts;
    if (s_vc_frame % 600 == 0 && s_vc) {
        uint32_t keep = 0;
        memset(s_vc_slot, 0, sizeof s_vc_slot);
        for (uint32_t i = 0; i < s_vc_n; i++) {
            VcEntry *e = &s_vc[i];
            if ((!e->data && e->skip_until <= s_vc_frame) || s_vc_frame - e->used > 600) { vc_free(e); continue; }
            uint32_t k = (uint32_t)e->key & (VC_SLOTS - 1), probe = 0;
            while (s_vc_slot[k] && probe < 16) { k = (k + 1) & (VC_SLOTS - 1); probe++; }
            if (s_vc_slot[k]) { vc_free(e); continue; }
            s_vc[keep] = *e;
            s_vc_slot[k] = (int32_t)keep + 1;
            keep++;
        }
        s_vc_n = keep;
    }
    if (s_vc_frame % 300 == 0) {
        if (g_gx_vcache)
            rt_log("vcache: %.0f%% of vertices reused (%llu lists a frame reused, %llu kept, %llu changed), %u lists kept, %u KB%s",
                   all_verts ? 100.0 * (double)s_vc_hit / (double)all_verts : 0.0, (unsigned long long)s_vc_lists / 300,
                   (unsigned long long)s_vc_built, (unsigned long long)s_vc_changed, s_vc_n, s_vc_bytes >> 10,
                   s_vc_check ? (s_vc_bad ? " CHECK FAILED" : " (checked)") : "");
        s_vc_hit = s_vc_lists = s_vc_built = s_vc_changed = s_vc_verts = 0;
        all_verts = 0;
    }
}


/* A MODERN HUD picture's scissor (EFB pixels), when the game clips it to less than the HUD's 608 x
 * 448: moved like the picture (the race HUD's units to pixels through the 2D projection). */
static bool modern_rect(const DrawSetup *S, unsigned g, uint32_t out[4]) {
    const HudMove *mv = &g_hud_move[g];
    float ew = G.efb[0], eh = G.efb[1];
    float ax = S->ax * S->pr[0] * ew * 0.5f, bx = (S->ax * S->pr[1] + S->bx + 1) * ew * 0.5f;
    float ay = -S->ay * S->pr[2] * eh * 0.5f, by = (1 - S->ay * S->pr[3] - S->by) * eh * 0.5f;
    if (fabsf(ax) < 1e-6f || fabsf(ay) < 1e-6f) return false;
    const uint32_t *r = S->rect;
    float l = bx, t = by, rr = ax * 608 + bx, bb = ay * 448 + by;
    if ((float)r[0] <= l + 2 && (float)r[1] <= t + 2 && (float)r[2] >= rr - 2 && (float)r[3] >= bb - 2) return false; /* not clipped */
    float px[2] = {(float)r[0], (float)r[2]}, py[2] = {(float)r[1], (float)r[3]};
    for (int i = 0; i < 2; i++) {
        px[i] = clampf(mv->s * (px[i] - bx) + ax * (mv->nx - mv->s * mv->ox) + bx, 0, ew);
        py[i] = clampf(mv->s * (py[i] - by) + ay * (mv->ny - mv->s * mv->oy) + by, 0, eh);
    }
    out[0] = (uint32_t)roundf(px[0]), out[1] = (uint32_t)roundf(py[0]);
    out[2] = (uint32_t)roundf(px[1]), out[3] = (uint32_t)roundf(py[1]);
    if (out[2] < out[0]) out[2] = out[0];
    if (out[3] < out[1]) out[3] = out[1];
    if (g_hud_log) rt_log("hudlog scissor g%u efb %gx%g layout %g,%g-%g,%g rect %u,%u-%u,%u -> %u,%u-%u,%u", g, ew, eh, l, t, rr, bb, r[0], r[1], r[2], r[3], out[0], out[1], out[2], out[3]);
    return true;
}

static void draw(uint32_t cmd, const uint8_t *c) {
    uint32_t prim = cmd & 0xF8;
    if (bits(G.bp[0x00], 14, 2) == 3) return; /* cull all */
    unsigned n = be16(c + 1);
    unsigned tris;
    switch (prim) {
    case 0x80: case 0x88: tris = n / 4 * 2; break;
    case 0x90: tris = n / 3; break;
    case 0x98: case 0xA0: tris = n >= 2 ? n - 2 : 0; break;
    default: tris = 0; /* lines and points are not drawn */
    }
    if (!tris) return;
    /* A desynced FIFO (garbage read as draws) asked for 2 million vertices in one frame and the
     * frame's vertex buffer grew until realloc failed (a crash). Real frames stay under
     * 100k: drop draws past a cap instead. */
    if (G.frame->nverts + n > 400000) {
        static int logged;
        if (logged++ < 4) rt_log("gx: frame vertex cap reached (%lu + %u), dropping draws", (unsigned long)G.frame->nverts, n);
        return;
    }
    const VtxFormat *f;
    DrawSetup *S = draw_prepare(cmd, &f);
    const bool gpu = S->gpu;
    const Reader *rd = S->rd;
    const VOp *ops = S->ops;
    const unsigned nops = S->nops;

    /* Vertices go at the end of the frame, aligned to the layout's stride; they only count once the
     * batch is known (the last batch, which has the same layout, or a new one starting here). */
    Frame *fr = G.frame;
    const uint32_t layout = S->layout, stride = gx_layout_stride(layout);
    /* (no division when the last draw had the same layout: vb.len is then a multiple of its stride) */
    size_t start = s_vb_frame == fr && s_vb_layout == layout ? fr->vb.len : (fr->vb.len + stride - 1) / stride * stride;
    vec_reserve(&fr->vb, start - fr->vb.len + (size_t)n * stride);
    uint8_t *out = fr->vb.data + start;
    uint32_t fixed_slot = gpu && !f->pnmtx ? mtx_slot(G.cp[0x30] & 0x3F) : 0;
    /* 2D draws (orthographic) are HUD candidates for the widescreen 4:3 HUD, unless they span
     * the whole width (see XV_HUD). */
    bool hud = gpu && S->ortho, extent = hud, textured = false;
    float xmin = 1e30f, xmax = -1e30f;
    if (hud) {
        const TevState *t = &G.mat_tev;
        for (unsigned st = 0; st < t->params[0] && st < 16; st++) textured |= (t->stages[st][2] >> 7) & 1;
    }
    bool any_texmtx = false;
    for (int i = 0; i < 8; i++) any_texmtx |= f->texmtx[i];
    if (!gpu) g_gx_cpu_verts += n;
    const uint32_t rec = S->rec;
    size_t o = 3;
    const bool fast = gpu && !s_xfcheck;
    static uint8_t *chk;
    static size_t chk_cap;
    if (fast) {
        size_t end = vtx_fast(S, f, c, o, n, out, stride, rec, fixed_slot, any_texmtx, extent, &xmin, &xmax);
        if (s_vtxcheck) {
            size_t need = (size_t)n * stride;
            if (need > chk_cap) { chk_cap = need * 2; chk = realloc(chk, chk_cap); }
            memcpy(chk, out, need);
            s_vtxcheck_n += n;
        } else {
            o = end;
        }
    }
    for (unsigned vi = 0; vi < n && (!fast || s_vtxcheck); vi++) {
        uint32_t midx = G.cp[0x30] & 0x3F;
        uint8_t tmtx_vtx[8];
        memset(tmtx_vtx, 0xFF, sizeof tmtx_vtx);
        float pos[3] = {0, 0, 0}, nrm[3] = {0, 0, 1}, tc[8][2];
        uint8_t col[2][4] = {{255, 255, 255, 255}, {255, 255, 255, 255}};
        int16_t nq[3] = {0, 0, (int16_t)GX_NRM_SCALE};
        memset(tc, 0, sizeof tc);
        for (unsigned k = 0; k < nops; k++) {
            const VOp *op = &ops[k];
            const uint8_t *b;
            switch (op->kind) {
            case OP_PNMTX: midx = c[o] & 0x3F; break;
            case OP_TMTX: tmtx_vtx[op->dst] = c[o] & 0x3F; break;
            case OP_POS:
                if ((b = op_src(op, rd, c, o))) read_comps(b, op->cnt < 3 ? op->cnt : 3, op->fmt, op->scale, pos);
                break;
            case OP_NRM:
                if ((b = op_src(op, rd, c, o))) {
                    if (op->fmt == 3 && op->scale == 1.0f / 16384.0f) {
                        /* s16 with 14 fraction bits: already the compact format */
                        for (int q = 0; q < 3; q++) nq[q] = (int16_t)be16(b + 2 * q);
                        if (!gpu || s_xfcheck) for (int q = 0; q < 3; q++) nrm[q] = (float)nq[q] * (1.0f / GX_NRM_SCALE);
                    } else {
                        read_comps(b, 3, op->fmt, op->scale, nrm);
                        for (int q = 0; q < 3; q++) nq[q] = nrm_q(nrm[q]);
                    }
                }
                break;
            case OP_COL:
                if ((b = op_src(op, rd, c, o))) read_color8(b, op->fmt, col[op->dst]);
                break;
            case OP_TC:
                if ((b = op_src(op, rd, c, o))) read_comps(b, op->cnt, op->fmt, op->scale, tc[op->dst]);
                break;
            default: break;
            }
            o += op->inl;
        }
        uint8_t *ob = out + (size_t)vi * stride;
        if (!gpu) {
            /* CPU transform: the full layout */
            float vcol[2][4];
            for (int ch = 0; ch < 2; ch++)
                for (int q = 0; q < 4; q++) vcol[ch][q] = (float)col[ch][q] / 255.0f;
            GpuVertex v;
            xform_cpu(S, f->pnmtx ? matrices(midx) : S->fixed_mt, tmtx_vtx, pos, nrm, vcol, tc, &v);
            memcpy(ob, &v, sizeof v);
            continue;
        }
        uint32_t slot = f->pnmtx ? mtx_slot(midx) : fixed_slot;
        if (any_texmtx) slot = texmtx_slot(slot, tmtx_vtx);
        memcpy(ob, pos, 12);
        memcpy(ob + 12, &rec, 4);
        memcpy(ob + 16, &slot, 4);
        memcpy(ob + 20, col[0], 4);
        memcpy(ob + 24, col[1], 4);
        memcpy(ob + 28, nq, 6);
        memset(ob + 34, 0, 2);
        float uv[8][2];
        for (unsigned u = 0; u < layout; u++) {
            int s = S->uv_from[u];
            if (s >= 0) { uv[u][0] = tc[s][0]; uv[u][1] = tc[s][1]; }
            else uv[u][0] = uv[u][1] = 0;
        }
        memcpy(ob + 36, uv, 8 * layout);
        if (extent) {
            const float *m = matrices(midx);
            float x = S->ax * (S->pr[0] * (m[0] * pos[0] + m[1] * pos[1] + m[2] * pos[2] + m[3]) + S->pr[1]) + S->bx;
            if (x < xmin) xmin = x;
            if (x > xmax) xmax = x;
        }
        if (s_xfcheck) {
            GpuVertex gv, ref;
            memset(&gv, 0, sizeof gv);
            memcpy(gv.pos, pos, 12); gv.pos[3] = 1;
            for (int q = 0; q < 3; q++) gv.nrm[q] = (float)nq[q] / GX_NRM_SCALE;
            gv.rec = rec; gv.mtx = slot;
            memcpy(gv.color0, col[0], 4); memcpy(gv.color1, col[1], 4);
            memcpy(gv.uv, uv, 8 * layout);
            /* the CPU path sees the same quantised vertex colours and normal as the shader */
            float q8[2][4], nn[3] = {gv.nrm[0], gv.nrm[1], gv.nrm[2]};
            for (int q = 0; q < 4; q++) { q8[0][q] = (float)col[0][q] / 255.0f; q8[1][q] = (float)col[1][q] / 255.0f; }
            xform_cpu(S, matrices(midx), tmtx_vtx, pos, nn, q8, tc, &ref);
            xf_check(&gv, &ref, S);
        }
    }
    size_t at;
    if (fast && s_vtxcheck && !vtx_same(chk, out, n, stride, &at)) {
        if (s_vtxcheck_bad++ < 20) {
            size_t i = at;
            while (i + 1 < at + stride && chk[i] == out[i]) i++;
            rt_log("vtxcheck: draw of %u vertices (stride %u) differs at byte %u: fast %02x general %02x", n, stride, (unsigned)i, chk[i], out[i]);
        }
    }
    if (hud && xmin <= -0.98f && xmax >= 0.98f && !(textured && G.movie)) hud = false;
    if (G.hud_zone == 3) hud = false; /* WipeManager: fades over the (widened) 3D view */
    uint32_t side = 0;
    if (hud && G.hud_zone) {
        float mid = 0.5f * (xmin + xmax);
        /* +-0.2, not 0.25: the second item box (partner's item) is centred at exactly -0.25, and its
         * frame and icon then fell on different sides (the frame drifted to the middle). Map
         * routine: also the left rank column. */
        side = mid < -0.2f ? 1 : mid > 0.2f || G.hud_zone == 2 ? 2 : 0;
        uint32_t z = G.hud_zone & 0xFF;
        if (z >= 4 && z <= 6) side = z - 4; /* MODERN HUD: the group's side */
    }
    if (hud) {
        uint32_t fl = XV_HUD | (side == 1 ? XV_HUD_LEFT : side == 2 ? XV_HUD_RIGHT : 0);
        for (unsigned vi = 0; vi < n; vi++) {
            uint32_t r;
            memcpy(&r, out + (size_t)vi * stride + 12, 4);
            r |= fl;
            memcpy(out + (size_t)vi * stride + 12, &r, 4);
        }
    }

    const uint32_t *rect = S->rect;
    uint32_t mrect[4];
    unsigned mg = G.hud_zone >> 8;
    if (hud && mg >= MGV_PLACE1 && mg <= MGV_ORDER) fr->vseam = 1;
    if (hud && mg && mg < MG_COUNT && modern_rect(S, mg, mrect)) rect = mrect;
    Batch *bt = batch_for(G.mat_key, &G.mat_tev, rect, hud ? 1 + side : 0, layout, start);
    if (!bt) return;
    uint32_t base = s_vb_frame == fr && s_vb_layout == layout && start == fr->vb.len ? s_vb_next : (uint32_t)(start / stride);
    fr->vb.len = start + (size_t)n * stride;
    s_vb_frame = fr; s_vb_layout = layout; s_vb_next = base + n;
    fr->nverts += n;
    vec_reserve(&fr->indices, (size_t)tris * 3);
    uint32_t *ix = fr->indices.data + fr->indices.len;
    size_t k = 0;
    switch (prim) {
    case 0x80: case 0x88:
        for (uint32_t q = 0; q < n / 4; q++) {
            uint32_t v = base + 4 * q;
            ix[k++] = v; ix[k++] = v + 1; ix[k++] = v + 2; ix[k++] = v; ix[k++] = v + 2; ix[k++] = v + 3;
        }
        break;
    case 0x90:
        for (uint32_t t = 0; t < n / 3; t++) { uint32_t v = base + 3 * t; ix[k++] = v; ix[k++] = v + 1; ix[k++] = v + 2; }
        break;
    case 0x98:
        /* every other strip triangle swaps its first two vertices to keep the strip's winding (culling) */
        for (uint32_t i = 2; i < n; i++) {
            uint32_t odd = i & 1;
            ix[k++] = base + i - 2 + odd; ix[k++] = base + i - 1 - odd; ix[k++] = base + i;
        }
        break;
    default:
        for (uint32_t i = 2; i < n; i++) { ix[k++] = base; ix[k++] = base + i - 1; ix[k++] = base + i; }
    }
    fr->indices.len += k;
    bt->vert_count += n;
    bt->index_count += (uint32_t)k;
    fr->draws++;
    g_gx_draws++;
}

/* Is this display list well formed: only known commands, each inside the list, no nested call?
 * A list the game rewrote or freed before the decoder got to it (it trails the game) is other data:
 * executing it wrote random BP/CP/XF registers, and the wrong state stayed (flat or missing
 * models, broken menus) until the game set those registers again. Such a list is skipped. A real
 * list never fails this (they are padded with NOPs); after a VAT write inside the list the vertex
 * sizes are unknown here, so the rest is accepted. */
uint64_t g_gx_dl_rejected;
static bool dl_valid(const uint8_t *d, size_t len) {
    size_t p = 0;
    while (p < len) {
        uint32_t cmd = d[p];
        size_t need;
        if (cmd == 0x00 || cmd == 0x44 || cmd == 0x48) need = 1;
        else if (cmd == 0x08) {
            if (p + 6 > len) return false;
            if (d[p + 1] >= 0x50 && d[p + 1] < 0xA0) return true;
            need = 6;
        } else if (cmd == 0x10) {
            if (p + 5 > len) return false;
            need = 5 + 4 * (size_t)(((be32(d + p + 1) >> 16) & 0xFFFF) + 1);
        } else if (cmd == 0x20 || cmd == 0x28 || cmd == 0x30 || cmd == 0x38 || cmd == 0x61) need = 5;
        else if (cmd >= 0x80 && cmd <= 0xBF) {
            if (p + 3 > len) return false;
            need = 3 + (size_t)be16(d + p + 1) * format(cmd & 7)->size;
        } else return false; /* unknown, or a call (0x40) inside a list */
        if (p + need > len) return false;
        p += need;
    }
    return true;
}

/* Execute commands; returns bytes consumed (stops at an incomplete command). */
static size_t exec(const uint8_t *d, size_t len, unsigned depth) {
    size_t p = 0;
    while (p < len) {
        uint32_t cmd = d[p];
        size_t need;
        if (cmd == 0x00 || cmd == 0x44 || cmd == 0x48) need = 1;
        else if (cmd == 0x08) need = 6;
        else if (cmd == 0x10) {
            if (p + 5 > len) return p;
            need = 5 + 4 * (size_t)(((be32(d + p + 1) >> 16) & 0xFFFF) + 1);
        } else if (cmd == 0x20 || cmd == 0x28 || cmd == 0x30 || cmd == 0x38) need = 5;
        else if (cmd == 0x40) need = 9;
        else if (cmd == 0x61) need = 5;
        else if (cmd >= 0x80 && cmd <= 0xBF) {
            if (p + 3 > len) return p;
            need = 3 + (size_t)be16(d + p + 1) * format(cmd & 7)->size;
        } else {
            if (G.unknown_logged < 16) {
                G.unknown_logged++;
                /* depth 0 = the FIFO stream, 1+ = a display list in guest RAM */
                rt_log("gx: unknown FIFO command %#04x at depth %u, offset %lu; before: %02x %02x %02x %02x %02x %02x %02x %02x", cmd, depth, (unsigned long)p,
                       p >= 8 ? d[p - 8] : 0, p >= 7 ? d[p - 7] : 0, p >= 6 ? d[p - 6] : 0, p >= 5 ? d[p - 5] : 0,
                       p >= 4 ? d[p - 4] : 0, p >= 3 ? d[p - 3] : 0, p >= 2 ? d[p - 2] : 0, p >= 1 ? d[p - 1] : 0);
            }
            need = 1;
        }
        if (p + need > len) return p;
        const uint8_t *c = d + p;
        switch (cmd) {
        case 0x08: {
            uint32_t reg = c[1];
            if (reg == 0x01) { G.hud_zone = be32(c + 2); break; } /* not a hardware register: port marker */
            G.cp[reg] = be32(c + 2);
            G.cp_gen++;
            if (reg >= 0x50 && reg < 0xA0) { for (int i = 0; i < 8; i++) G.fmt_cache[i].valid = false; G.vat_gen++; }
            break;
        }
        case 0x10: {
            uint32_t addr = be32(c + 1) & 0xFFFF;
            for (size_t i = 0; i < (need - 5) / 4; i++) xf_write(addr + (uint32_t)i, be32(c + 5 + 4 * i));
            break;
        }
        case 0x20: case 0x28: case 0x30: case 0x38: {
            uint32_t v = be32(c + 1), array = 12 + ((cmd - 0x20) >> 3);
            uint32_t idx = v >> 16, cnt = ((v >> 12) & 0xF) + 1, addr = v & 0xFFF;
            uint32_t src = (G.cp[0xA0 + array] & 0x03FFFFFFu) + idx * G.cp[0xB0 + array];
            for (uint32_t i = 0; i < cnt; i++) xf_write(addr + i, (uint32_t)m_read_be(0x80000000u | (src + 4 * i), 4));
            break;
        }
        case 0x40: {
            uint32_t addr = be32(c + 1) & 0x03FFFFFFu, size = be32(c + 5);
            /* Decoded in place from RAM (J3D calls a display list per shape). */
            if (depth >= 4 || (uint64_t)addr + size > RAM_SIZE) break;
            if (!dl_valid(g_ram + addr, size)) {
                if (g_gx_dl_rejected++ < 20) rt_log("gx: display list at %#x (%u bytes) is not commands (memory reused before it was drawn): skipped", addr, size);
                break;
            }
            if (!dl_call(addr, size, depth + 1)) exec(g_ram + addr, size, depth + 1);
            break;
        }
        case 0x61: bp_write(be32(c + 1)); break;
        default:
            if (cmd >= 0x80 && cmd <= 0xBF) draw(cmd, c);
        }
        p += need;
    }
    return p;
}

/* Feed FIFO bytes. Incomplete commands are buffered. */
static void gx_push(const uint8_t *bytes, size_t n) {
    if (G.len + n > G.cap) {
        G.cap = G.cap ? G.cap * 2 : 1 << 20;
        while (G.cap < G.len + n) G.cap *= 2;
        G.buf = realloc(G.buf, G.cap);
    }
    memcpy(G.buf + G.len, bytes, n);
    G.len += n;
    G.pos += exec(G.buf + G.pos, G.len - G.pos, 0);
    if (G.pos == G.len) G.len = G.pos = 0;
    else if (G.pos >= 1 << 16) {
        memmove(G.buf, G.buf + G.pos, G.len - G.pos);
        G.len -= G.pos;
        G.pos = 0;
    }
    if (G.len - G.pos > 1 << 20) {
        rt_log("gx: FIFO desync, dropping buffered bytes");
        G.len = G.pos = 0;
    }
}

/* Set at exit: the decoder drops its input, so it no longer reads guest RAM while the process
 * is torn down (closing from HOME faulted it in a display list). */
static volatile bool s_gpu_stop;
static uint64_t s_bytes_since, s_bytes_frame;
void gx_stop_gpu(void) { s_gpu_stop = true; }

volatile uint32_t g_gx_gpu_host;

/* The decoder trails the game (GXDrawDone doesn't wait for it), so queued commands may still
 * read display lists, vertex arrays and textures in guest RAM. Before a DMA overwrites RAM (a
 * scene load reuses the old scene's memory) the decoder must have read everything queued, or it
 * decodes the new data as commands and its state stays corrupt (flat-coloured or missing models,
 * broken menus). Called by the DVD and ARAM DMA paths, after hw_gx_flush. */
uint64_t g_gx_drains, g_gx_drain_ns;
void gx_drain(void) {
    if (s_gpu_stop || plat_thread_handle() == g_gx_gpu_host) return;
    pmutex_lock(&s_qlock);
    if (s_qlen || s_qheld) {
        uint64_t t0 = plat_now_ns();
        /* bounded: a stuck decoder must not hang the game (the old behaviour is the fallback) */
        while ((s_qlen || s_qheld) && !s_gpu_stop && plat_now_ns() - t0 < 500000000ull)
            pcond_wait_ns(&s_qspace, &s_qlock, 50000000ull);
        g_gx_drains++;
        g_gx_drain_ns += plat_now_ns() - t0;
    }
    pmutex_unlock(&s_qlock);
}

/* While racing the game reads the disc about once a frame in small pieces (the music stream)
 * into buffers the decoder never reads, and the race's models, textures and lists stay where they
 * are until it ends. With two players' scenery queued each wait cost 7-18 ms of the game's frame,
 * so small disc reads during a race (Race2D drew in the last 200 ms, native.c) don't wait. Scene
 * loads (large, or outside a race) still do, and so does every ARAM-to-RAM DMA (hw.c aram_dma: what
 * it brings may be pictures or models queued commands still read); a display list overwritten all
 * the same is caught by dl_valid. */
volatile uint64_t g_racing_ns, g_gx_drain_skips;
void gx_drain_dma(uint32_t len) {
    uint64_t r = g_racing_ns;
    if (len <= 0x10000u && r && plat_now_ns() - r < 200000000ull) { g_gx_drain_skips++; return; }
    gx_drain();
}

static void gpu_thread(void *arg) {
    (void)arg;
    g_gx_gpu_host = plat_thread_handle();
    uint8_t *data = NULL;
    size_t cap = 0;
    for (;;) {
        pmutex_lock(&s_qlock);
        while (s_qlen == 0) {
            s_qbusy = false;
            pcond_wait(&s_qwork, &s_qlock);
        }
        s_qbusy = true;
        /* swap buffers */
        uint8_t *tb = s_qbuf; size_t tl = s_qlen, tc = s_qcap;
        s_qbuf = data; s_qcap = cap; s_qlen = 0;
        s_qheld = tl;
        data = tb; cap = tc;
        pmutex_unlock(&s_qlock);
        pcond_broadcast(&s_qspace);
        s_nlocal = 0;
        uint64_t t0 = plat_now_ns();
        uint64_t frames0 = s_frames_decoded;
        if (!s_gpu_stop) gx_push(data, tl);
        g_gx_busy_ns += plat_now_ns() - t0;
        /* queue limit from the bytes per decoded frame (moving average over ~8 frames) */
        s_bytes_since += tl;
        if (s_frames_decoded != frames0) {
            uint64_t per = s_bytes_since / (s_frames_decoded - frames0);
            s_bytes_frame = s_bytes_frame ? (s_bytes_frame * 7 + per) / 8 : per;
            size_t lim = (size_t)(s_bytes_frame * 3 / 2);
            s_qlimit = lim < GPU_QUEUE_MIN ? GPU_QUEUE_MIN : lim > GPU_QUEUE_MAX ? GPU_QUEUE_MAX : lim;
            s_bytes_since = 0;
        }
        pmutex_lock(&s_qlock);
        s_qheld = 0;
        pmutex_unlock(&s_qlock);
        pcond_broadcast(&s_qspace);
        if (s_nlocal) {
            bool finish = false, token = false;
            pmutex_lock(&s_elock);
            for (int i = 0; i < s_nlocal; i++) {
                if (s_nevents < 1024) s_events[s_nevents++] = s_local_ev[i];
                finish |= s_local_ev[i].kind == GXEV_DRAW_DONE;
                token |= s_local_ev[i].kind == GXEV_TOKEN_INT;
            }
            pmutex_unlock(&s_elock);
            if (finish) sched_raise(IRQ_PE_FINISH);
            if (token) sched_raise(IRQ_PE_TOKEN);
        }
    }
}

void gx_start_gpu(void) {
    if (getenv("MKDD_CPU_XFORM")) g_gx_cpu_xform = true;
    s_xfcheck = getenv("MKDD_XFCHECK") != NULL;
    s_vtxcheck = getenv("MKDD_VTXCHECK") != NULL;
    s_vc_check = getenv("MKDD_VCACHECK") != NULL;
    if (getenv("MKDD_NOVCACHE")) g_gx_vcache = false;
    pmutex_init(&s_flock);
    pmutex_init(&s_pool_lock);
    pmutex_init(&s_tlock);
    pmutex_init(&s_qlock);
    pmutex_init(&s_elock);
    pcond_init(&s_qwork);
    pcond_init(&s_qspace);
    G.bp_mask = 0xFFFFFF;
    for (int i = 0; i < 4; i++) for (int k = 0; k < 4; k++) G.konst[i][k] = 1;
    G.tmem = calloc(1, TMEM_SIZE);
    G.efb[0] = 640;
    G.efb[1] = 480;
    G.frame = frame_new();
    /* Switch: core 2 (core 0 renders, core 1 runs the game; the timer on core 2 mostly sleeps) */
    pthread_start("mkdd-gpu", gpu_thread, NULL, 8 << 20, 2, 0);
}

/* ---- texture decoding (renderer side) ---- */
static void rgb565(uint16_t v, uint8_t o[4]) {
    uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    o[0] = (uint8_t)(r * 255 / 31); o[1] = (uint8_t)(g * 255 / 63); o[2] = (uint8_t)(b * 255 / 31); o[3] = 255;
}
static void rgb5a3(uint16_t v, uint8_t o[4]) {
    if (v & 0x8000) {
        o[0] = (uint8_t)(((v >> 10) & 31) * 255 / 31); o[1] = (uint8_t)(((v >> 5) & 31) * 255 / 31);
        o[2] = (uint8_t)((v & 31) * 255 / 31); o[3] = 255;
    } else {
        o[0] = (uint8_t)(((v >> 8) & 15) * 255 / 15); o[1] = (uint8_t)(((v >> 4) & 15) * 255 / 15);
        o[2] = (uint8_t)((v & 15) * 255 / 15); o[3] = (uint8_t)(((v >> 12) & 7) * 255 / 7);
    }
}
static void tlut_color(const uint8_t *tlut, unsigned idx, uint32_t fmt, uint8_t o[4]) {
    unsigned off = 2 * idx;
    if (!tlut || off + 1 >= 512) { o[0] = o[1] = o[2] = o[3] = 0; return; }
    uint16_t v = (uint16_t)(tlut[off] << 8 | tlut[off + 1]);
    if (fmt == 0) { uint8_t a = (uint8_t)(v >> 8), i = (uint8_t)v; o[0] = o[1] = o[2] = i; o[3] = a; }
    else if (fmt == 1) rgb565(v, o);
    else rgb5a3(v, o);
}

/* Intensity formats without the RGBA expansion: I4 / I8 -> 1 byte per texel (I), IA4 / IA8 ->
 * 2 (I, A), rows of w texels. The renderer uploads them as R8 / RG8 and swizzles back to
 * (I, I, I, I) / (I, I, I, A), the same values gx_texture_decode gives: a quarter (half) of the
 * upload. Movies are three I8 planes per frame. 0: other formats. */
/* CMPR is S3TC/BC1 in another layout: 8x8 tiles of four 4x4 blocks (top left, top right, bottom
 * left, bottom right), big-endian colours, the first texel of a row in an index byte's top bits.
 * BC1 wants the blocks in rows, little-endian colours and the first texel in the low bits; the
 * colour rules are the same (c0 > c1: four colours at thirds; else three and transparent black).
 * Sizes that are not a multiple of 4: false. */
bool gx_texture_cmpr_bc1(const GxTexture *t, uint8_t *out) {
    uint32_t w = t->width, h = t->height;
    if (t->fmt != 14 || (w & 3) || (h & 3) || !w || !h) return false;
    uint32_t bw = w / 4, bh = h / 4, tiles_x = (w + 7) / 8;
    if (t->raw_len < (size_t)tiles_x * ((h + 7) / 8) * 32) return false;
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t *s = t->raw + ((size_t)(by / 2) * tiles_x + bx / 2) * 32 + ((by & 1) * 2 + (bx & 1)) * 8;
            uint8_t *o = out + ((size_t)by * bw + bx) * 8;
            o[0] = s[1]; o[1] = s[0]; o[2] = s[3]; o[3] = s[2];
            for (int r = 0; r < 4; r++) {
                uint8_t v = s[4 + r];
                o[4 + r] = (uint8_t)((v & 0x03) << 6 | (v & 0x0C) << 2 | (v & 0x30) >> 2 | (v & 0xC0) >> 6);
            }
        }
    return true;
}

unsigned gx_texture_decode_compact(const GxTexture *t, uint8_t *out) {
    uint32_t w = t->width, h = t->height, fmt = t->fmt, bw, bh, bytes;
    if (fmt > 3 || !block_info(fmt, &bw, &bh, &bytes)) return 0;
    unsigned n = fmt >= 2 ? 2 : 1;
    uint32_t bx_n = (w + bw - 1) / bw, by_n = (h + bh - 1) / bh;
    const uint8_t *blk = t->raw;
    for (uint32_t by = 0; by < by_n; by++)
        for (uint32_t bx = 0; bx < bx_n; bx++, blk += bytes) {
            uint32_t x0 = bx * bw, y0 = by * bh, cw = w - x0 < bw ? w - x0 : bw;
            for (uint32_t r = 0; r < bh && y0 + r < h; r++) {
                uint8_t *o = out + ((size_t)(y0 + r) * w + x0) * n;
                switch (fmt) {
                case 0: /* I4: 8x8, 4 bytes per row, high nibble first */
                    for (uint32_t x = 0; x < cw; x++) { uint8_t b = blk[r * 4 + x / 2]; o[x] = (uint8_t)((x & 1 ? b & 15 : b >> 4) * 17); }
                    break;
                case 1: /* I8: 8x4, 8 bytes per row */
                    memcpy(o, blk + r * 8, cw);
                    break;
                case 2: /* IA4: 8x4, alpha in the high nibble */
                    for (uint32_t x = 0; x < cw; x++) { uint8_t v = blk[r * 8 + x]; o[2 * x] = (uint8_t)((v & 15) * 17); o[2 * x + 1] = (uint8_t)((v >> 4) * 17); }
                    break;
                default: /* IA8: 4x4, (A, I) pairs */
                    for (uint32_t x = 0; x < cw; x++) { o[2 * x] = blk[r * 8 + 2 * x + 1]; o[2 * x + 1] = blk[r * 8 + 2 * x]; }
                    break;
                }
            }
        }
    return n;
}

bool gx_texture_decode(const GxTexture *t, uint8_t *out) {
    uint32_t w = t->width, h = t->height, fmt = t->fmt, bw, bh, bytes;
    if (!block_info(fmt, &bw, &bh, &bytes)) { memset(out, 255, (size_t)w * h * 4); return false; }
    uint32_t bx_n = (w + bw - 1) / bw, by_n = (h + bh - 1) / bh;
    const uint8_t *d = t->raw;
#define PUT(x, y, c) do { uint32_t _x = (x), _y = (y); if (_x < w && _y < h) memcpy(out + ((size_t)_y * w + _x) * 4, (c), 4); } while (0)
    for (uint32_t by = 0; by < by_n; by++)
        for (uint32_t bx = 0; bx < bx_n; bx++) {
            const uint8_t *blk = d + (size_t)(by * bx_n + bx) * bytes;
            uint32_t x0 = bx * bw, y0 = by * bh;
            uint8_t c[4];
            switch (fmt) {
            case 0: case 8:
                for (uint32_t i = 0; i < 64; i++) {
                    uint8_t b = blk[i / 2], v = i & 1 ? b & 15 : b >> 4;
                    if (fmt == 0) { uint8_t l = (uint8_t)(v * 17); c[0] = c[1] = c[2] = c[3] = l; }
                    else tlut_color(t->tlut, v, t->tlut_fmt, c);
                    PUT(x0 + i % 8, y0 + i / 8, c);
                }
                break;
            case 1: case 9:
                for (uint32_t i = 0; i < 32; i++) {
                    uint8_t v = blk[i];
                    if (fmt == 1) { c[0] = c[1] = c[2] = c[3] = v; }
                    else tlut_color(t->tlut, v, t->tlut_fmt, c);
                    PUT(x0 + i % 8, y0 + i / 8, c);
                }
                break;
            case 2:
                for (uint32_t i = 0; i < 32; i++) {
                    uint8_t v = blk[i], a = (uint8_t)((v >> 4) * 17), l = (uint8_t)((v & 15) * 17);
                    c[0] = c[1] = c[2] = l; c[3] = a;
                    PUT(x0 + i % 8, y0 + i / 8, c);
                }
                break;
            case 3:
                for (uint32_t i = 0; i < 16; i++) {
                    c[0] = c[1] = c[2] = blk[2 * i + 1]; c[3] = blk[2 * i];
                    PUT(x0 + i % 4, y0 + i / 4, c);
                }
                break;
            case 4: case 5: case 10:
                for (uint32_t i = 0; i < 16; i++) {
                    uint16_t v = (uint16_t)(blk[2 * i] << 8 | blk[2 * i + 1]);
                    if (fmt == 4) rgb565(v, c);
                    else if (fmt == 5) rgb5a3(v, c);
                    else tlut_color(t->tlut, v & 0x3FFF, t->tlut_fmt, c);
                    PUT(x0 + i % 4, y0 + i / 4, c);
                }
                break;
            case 6:
                for (uint32_t i = 0; i < 16; i++) {
                    c[0] = blk[2 * i + 1]; c[1] = blk[32 + 2 * i]; c[2] = blk[32 + 2 * i + 1]; c[3] = blk[2 * i];
                    PUT(x0 + i % 4, y0 + i / 4, c);
                }
                break;
            case 14:
                for (uint32_t sub = 0; sub < 4; sub++) {
                    const uint8_t *s = blk + sub * 8;
                    uint16_t c0 = (uint16_t)(s[0] << 8 | s[1]), c1 = (uint16_t)(s[2] << 8 | s[3]);
                    uint8_t pal[4][4];
                    rgb565(c0, pal[0]);
                    rgb565(c1, pal[1]);
                    for (int k = 0; k < 3; k++) {
                        if (c0 > c1) {
                            pal[2][k] = (uint8_t)((pal[0][k] * 2u + pal[1][k]) / 3);
                            pal[3][k] = (uint8_t)((pal[0][k] + pal[1][k] * 2u) / 3);
                        } else {
                            pal[2][k] = (uint8_t)((pal[0][k] + pal[1][k]) / 2);
                            pal[3][k] = 0;
                        }
                    }
                    pal[2][3] = 255;
                    pal[3][3] = c0 > c1 ? 255 : 0;
                    uint32_t sx = x0 + (sub % 2) * 4, sy = y0 + (sub / 2) * 4;
                    for (uint32_t row = 0; row < 4; row++)
                        for (uint32_t col = 0; col < 4; col++) PUT(sx + col, sy + row, pal[(s[4 + row] >> (6 - 2 * col)) & 3]);
                }
                break;
            }
        }
#undef PUT
    return true;
}

/* ---- CPU reference of the XF vertex shader (switch/xf_vs.h): reads only the texels the shader
 * reads, with the same formulas, so MKDD_XFCHECK can validate records and shader logic on PC. ---- */
static inline float xu(const uint32_t *t, unsigned i) { return u2f(t[i]); }
static void rgba8(uint32_t v, float o[4]) { rgba_u32(v, o); }
static void eval_part(const uint32_t *xf, const uint32_t *p, uint32_t lb, const float pos[3], const float n[3], const float vcol[4],
                      bool hv, float acc[4]) {
    uint32_t ctrl = p[0];
    float amb[4];
    rgba8(p[2], amb);
    const float *a0 = (ctrl >> 6 & 1) && hv ? vcol : amb;
    for (int k = 0; k < 4; k++) acc[k] = a0[k];
    uint32_t mask = bits(ctrl, 2, 4) | bits(ctrl, 11, 4) << 4, dfn = bits(ctrl, 7, 2), afn = bits(ctrl, 9, 2);
    for (unsigned l = 0; l < 8; l++) {
        if (!(mask & (1u << l))) continue;
        const uint32_t *t = xf + 4 * (lb + 5 * l);
        float col[4], cs[3], ds[3], lp[3], ld[3];
        rgba8(t[0], col);
        for (int k = 0; k < 3; k++) { cs[k] = xu(t, 4 + k); ds[k] = xu(t, 8 + k); lp[k] = xu(t, 12 + k); ld[k] = xu(t, 16 + k); }
        float tl[3] = {lp[0] - pos[0], lp[1] - pos[1], lp[2] - pos[2]};
        float dir[3], attn;
        if (afn == 1) {
            float k3[3] = {ds[0], ds[1], ds[2]};
            if (dfn != 0) {
                float l2 = dot3(ds, ds);
                float r = l2 > 0 ? 1.0f / sqrtf(l2) : 1.0f;
                for (int k = 0; k < 3; k++) k3[k] = ds[k] * r;
            }
            norm3(tl, dir);
            float a = dot3(n, dir) >= 0 ? fmaxf(dot3(n, ld), 0) : 0;
            float num = fmaxf(cs[0] + cs[1] * a + cs[2] * a * a, 0);
            float den = k3[0] + k3[1] * a + k3[2] * a * a;
            attn = den != 0 ? num / den : 0;
        } else if (afn == 3) {
            float d2 = dot3(tl, tl), d = sqrtf(d2);
            if (d > 0) { float r = 1.0f / d; dir[0] = tl[0] * r; dir[1] = tl[1] * r; dir[2] = tl[2] * r; }
            else memcpy(dir, tl, 12);
            float cc = fmaxf(dot3(dir, ld), 0);
            float num = fmaxf(cs[0] + cs[1] * cc + cs[2] * cc * cc, 0);
            float den = ds[0] + ds[1] * d + ds[2] * d2;
            attn = den != 0 ? num / den : 0;
        } else {
            norm3(tl, dir);
            if (dot3(dir, dir) == 0) memcpy(dir, n, 12);
            attn = 1;
        }
        float ndl = dot3(dir, n);
        float diff = dfn == 0 ? 1 : dfn == 1 ? ndl : fmaxf(ndl, 0);
        float s = attn * diff * 255.0f;
        for (int k = 0; k < 4; k++) acc[k] += round_fast(s * col[k]) * (1.0f / 255.0f);
    }
    for (int k = 0; k < 4; k++) acc[k] = clampf(acc[k], 0, 1);
}

void gx_xf_eval(const uint32_t *xf, const GpuVertex *in, GpuVertex *out) {
    *out = *in;
    if (in->rec == XV_PRE) return;
    uint32_t mslot = in->mtx, tms[8] = {0};
    if (mslot & 0x80000000u) {
        const uint32_t *ind = xf + 4 * (mslot & 0x7FFFFFFFu);
        mslot = ind[0];
        memcpy(tms, ind + 1, sizeof tms);
    }
    const uint32_t *R = xf + 4 * (in->rec & XV_REC_MASK), *M = xf + 4 * mslot;
    const float *pos = in->pos;
    float mt[21];
    for (int k = 0; k < 12; k++) mt[k] = xu(M, (unsigned)k);
    for (int r = 0; r < 3; r++) for (int k = 0; k < 3; k++) mt[12 + 3 * r + k] = xu(M, 12 + 4 * (unsigned)r + (unsigned)k);
    float view[3] = {
        mt[0] * pos[0] + mt[1] * pos[1] + mt[2] * pos[2] + mt[3],
        mt[4] * pos[0] + mt[5] * pos[1] + mt[6] * pos[2] + mt[7],
        mt[8] * pos[0] + mt[9] * pos[1] + mt[10] * pos[2] + mt[11],
    };
    const float *nrm = in->nrm;
    float vn[3] = {
        mt[12] * nrm[0] + mt[13] * nrm[1] + mt[14] * nrm[2],
        mt[15] * nrm[0] + mt[16] * nrm[1] + mt[17] * nrm[2],
        mt[18] * nrm[0] + mt[19] * nrm[1] + mt[20] * nrm[2],
    };
    float l2 = dot3(vn, vn);
    if (l2 > 1e-12f) { float r = 1.0f / sqrtf(l2); vn[0] *= r; vn[1] *= r; vn[2] *= r; }
    uint32_t flags = R[12], zop = bits(flags, 1, 2), chans = bits(flags, 3, 2), lb = R[14], uvmask = R[15];
    float p[4];
    if (!(flags & 1)) { p[0] = xu(R, 0) * view[0] + xu(R, 1) * view[2]; p[1] = xu(R, 2) * view[1] + xu(R, 3) * view[2]; p[2] = xu(R, 4) * view[2] + xu(R, 5); p[3] = -view[2]; }
    else { p[0] = xu(R, 0) * view[0] + xu(R, 1); p[1] = xu(R, 2) * view[1] + xu(R, 3); p[2] = xu(R, 4) * view[2] + xu(R, 5); p[3] = 1; }
    float w = p[3], zs = xu(R, 6), zo = xu(R, 7), zval = xu(R, 13);
    float zc = zop == 2 ? zval * w : zop ? zo * w + zs * p[2] + zval * w : zo * w + zs * p[2];
    out->pos[0] = xu(R, 8) * p[0] + xu(R, 9) * w;
    out->pos[1] = xu(R, 10) * p[1] + xu(R, 11) * w;
    out->pos[2] = zc * (1.0f / 16777215.0f);
    out->pos[3] = w;
    out->rec = XV_PRE;
    for (unsigned ch = 0; ch < 2; ch++) {
        float vcol[4], o[4], lit[4], mc[4], ma[4];
        const uint8_t *vc = ch ? in->color1 : in->color0;
        for (int k = 0; k < 4; k++) vcol[k] = (float)vc[k] / 255.0f;
        bool hv = (flags >> (5 + ch)) & 1;
        if (ch >= (chans ? chans : 1)) { o[0] = o[1] = o[2] = o[3] = 0; }
        else {
            const uint32_t *pc = R + 16 + 8 * ch, *pa = pc + 4;
            rgba8(pc[1], mc);
            rgba8(pa[1], ma);
            const float *mcs = (pc[0] & 1) && hv ? vcol : mc, *mas = (pa[0] & 1) && hv ? vcol : ma;
            if (pc[0] >> 1 & 1) { eval_part(xf, pc, lb, view, vn, vcol, hv, lit); for (int k = 0; k < 3; k++) o[k] = lit[k] * mcs[k]; }
            else for (int k = 0; k < 3; k++) o[k] = mcs[k];
            if (pa[0] >> 1 & 1) { eval_part(xf, pa, lb, view, vn, vcol, hv, lit); o[3] = lit[3] * mas[3]; }
            else o[3] = mas[3];
        }
        uint8_t *dst = ch ? out->color1 : out->color0;
        for (int k = 0; k < 4; k++) dst[k] = (uint8_t)(o[k] * 255.0f + 0.5f);
    }
    for (unsigned i = 0; i < 8; i++) {
        if (!(uvmask & (1u << i))) { out->uv[i][0] = out->uv[i][1] = 0; continue; }
        const uint32_t *T = R + 4 * (8 + 7 * i);
        uint32_t info = T[0], row = bits(info, 7, 5), ty = bits(info, 4, 3);
        bool stq = bits(info, 1, 1);
        float in4[4];
        if (row == 0) { in4[0] = pos[0]; in4[1] = pos[1]; in4[2] = pos[2]; in4[3] = 1; }
        else if (row == 1) { in4[0] = nrm[0]; in4[1] = nrm[1]; in4[2] = nrm[2]; in4[3] = 1; }
        else if (row >= 5 && row <= 12) { in4[0] = in->uv[i][0]; in4[1] = in->uv[i][1]; in4[2] = 1; in4[3] = 1; }
        else { in4[0] = 0; in4[1] = 0; in4[2] = 1; in4[3] = 1; }
        if (ty != 0) { out->uv[i][0] = in4[0]; out->uv[i][1] = in4[1]; continue; }
        const uint32_t *TM = tms[i] ? xf + 4 * tms[i] - 4 : T; /* rows at TM + 4.. */
#define ROW(k) (xu(TM, 4 + 4 * (k)) * in4[0] + xu(TM, 5 + 4 * (k)) * in4[1] + xu(TM, 6 + 4 * (k)) * in4[2] + xu(TM, 7 + 4 * (k)) * in4[3])
        float v[3] = {ROW(0), ROW(1), stq ? ROW(2) : 1.0f};
#undef ROW
        if (T[2]) {
            if (bits(T[1], 8, 1)) {
                float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                if (len > 1e-12f) { v[0] /= len; v[1] /= len; v[2] /= len; }
            }
            float q[3];
            for (int k = 0; k < 3; k++)
                q[k] = xu(T, 16 + 4 * (unsigned)k) * v[0] + xu(T, 17 + 4 * (unsigned)k) * v[1] + xu(T, 18 + 4 * (unsigned)k) * v[2] + xu(T, 19 + 4 * (unsigned)k);
            memcpy(v, q, 12);
        }
        if (stq && fabsf(v[2]) > 1e-6f) { out->uv[i][0] = v[0] / v[2]; out->uv[i][1] = v[1] / v[2]; }
        else { out->uv[i][0] = v[0]; out->uv[i][1] = v[1]; }
    }
}

void gx_xf_report(char *out, size_t n) {
    snprintf(out, n, "xfcheck: %llu vertices compared, %llu mismatches; vtxcheck: %llu vertices, %llu draws differ",
             (unsigned long long)s_xfcheck_n, (unsigned long long)s_xfcheck_bad, (unsigned long long)s_vtxcheck_n,
             (unsigned long long)s_vtxcheck_bad);
}

/* The latest presented frame as text (PC harness: MKDD_DUMP_FRAME): every batch's pixel pipeline, its
 * textures with their average colour, and a few vertices through the XF stage, for comparing a
 * frame that draws wrong with one that draws right. */
bool gx_dump_frame(const char *path) {
    Frame *f = gx_frame_acquire();
    if (!f) return false;
    FILE *o = fopen(path, "w");
    if (!o) { gx_frame_release(f); return false; }
    fprintf(o, "frame: %zu batches, %u verts, %zu copies, efb %.0fx%.0f, clear %.3f %.3f %.3f %.3f, xf %zu texels\n",
            f->batches.len, f->nverts, f->copies.len, f->efb[0], f->efb[1], f->clear[0], f->clear[1], f->clear[2], f->clear[3], f->xf.len / 4);
    for (size_t i = 0; i < f->copies.len; i++) {
        const EfbCopy *e = &f->copies.data[i];
        fprintf(o, "copy %zu at batch %u: cmd %#x src %#x size %ux%u dst %#x\n", i, e->at_batch, e->bp52, e->src,
                (e->size & 0x3FF) + 1, ((e->size >> 10) & 0x3FF) + 1, gx_phys(e->dst));
    }
    static uint8_t *pix;
    for (size_t i = 0; i < f->batches.len; i++) {
        const Batch *b = &f->batches.data[i];
        const TevState *t = &b->tev;
        fprintf(o, "\nbatch %zu: verts %u idx %u layout %u hud %u rect %u,%u-%u,%u z %#x blend %#x cull %u pix %#x stages %u acmp %#x swap %#x\n",
                i, b->vert_count, b->index_count, b->layout, b->hud, b->rect[0], b->rect[1], b->rect[2], b->rect[3],
                t->pipe[0], t->pipe[1], t->pipe[2], t->pipe[3], t->params[0], t->params[1], t->params[2]);
        for (unsigned s = 0; s < t->params[0] && s < 16; s++)
            fprintf(o, "  stage %u: c %#08x a %#08x order %#x ksel %#x\n", s, t->stages[s][0], t->stages[s][1], t->stages[s][2], t->stages[s][3]);
        fprintf(o, "  regs");
        for (int r = 0; r < 4; r++) fprintf(o, " [%.2f %.2f %.2f %.2f]", t->regs[r][0], t->regs[r][1], t->regs[r][2], t->regs[r][3]);
        fprintf(o, "\n  konst");
        for (int r = 0; r < 4; r++) fprintf(o, " [%.2f %.2f %.2f %.2f]", t->konst[r][0], t->konst[r][1], t->konst[r][2], t->konst[r][3]);
        fprintf(o, "\n  fog A %g C %g B %g shift %g colour %.3f %.3f %.3f sel %u\n", t->fog[0], t->fog[1], t->fog[2], t->fog[3],
                t->fog_color[0], t->fog_color[1], t->fog_color[2], (unsigned)t->fog_color[3]);
        for (int m = 0; m < 8; m++) {
            uint64_t k = t->textures[m];
            if (!k) continue;
            if ((k & 0xFFFF000000000000ull) == RT_TAG) { fprintf(o, "  tex %d: render target %#x mode %#x\n", m, (uint32_t)k, (uint32_t)(k >> 32) & 0xFFFF); continue; }
            GxTexture *tx = gx_texture_acquire(k);
            if (!tx) { fprintf(o, "  tex %d: key %#llx NOT IN STORE\n", m, (unsigned long long)k); continue; }
            double sum[4] = {0};
            size_t n = (size_t)tx->width * tx->height;
            pix = realloc(pix, n * 4);
            if (pix && gx_texture_decode(tx, pix))
                for (size_t p = 0; p < n; p++) for (int c = 0; c < 4; c++) sum[c] += pix[p * 4 + c];
            fprintf(o, "  tex %d: key %#llx fmt %u %ux%u tlut %u wrap %u/%u subst %u movie %u avg %.0f %.0f %.0f %.0f\n", m, (unsigned long long)k,
                    tx->fmt, tx->width, tx->height, tx->tlut_fmt, tx->wrap_s, tx->wrap_t, tx->subst, tx->movie,
                    sum[0] / (double)(n ? n : 1), sum[1] / (double)(n ? n : 1), sum[2] / (double)(n ? n : 1), sum[3] / (double)(n ? n : 1));
            gx_texture_release(tx);
        }
        /* a few vertices through the XF stage: lit colours and texture coordinates */
        float umin[2] = {1e30f, 1e30f}, umax[2] = {-1e30f, -1e30f};
        for (uint32_t q = 0; q < b->index_count && q < 256; q++) {
            GpuVertex in, e;
            gx_vertex_unpack(f, b, f->indices.data[b->index_first + q], &in);
            gx_xf_eval(f->xf.data, &in, &e);
            for (int c = 0; c < 2; c++) { if (e.uv[0][c] < umin[c]) umin[c] = e.uv[0][c]; if (e.uv[0][c] > umax[c]) umax[c] = e.uv[0][c]; }
            if (q == 0 || q == b->index_count / 2)
                fprintf(o, "  v%u: rec %#x clip %.3f %.3f %.3f %.3f c0 %02x%02x%02x%02x c1 %02x%02x%02x%02x uv0 %.3f %.3f uv1 %.3f %.3f\n", q, in.rec,
                        e.pos[0], e.pos[1], e.pos[2], e.pos[3], e.color0[0], e.color0[1], e.color0[2], e.color0[3],
                        e.color1[0], e.color1[1], e.color1[2], e.color1[3], e.uv[0][0], e.uv[0][1], e.uv[1][0], e.uv[1][1]);
        }
        if (b->index_count) fprintf(o, "  uv0 range %.3f..%.3f, %.3f..%.3f\n", umin[0], umax[0], umin[1], umax[1]);
        {   /* depth: z/w range of every vertex, and how many fall outside the clip volume */
            float zmin = 1e30f, zmax = -1e30f;
            unsigned behind = 0, nearc = 0, farc = 0;
            for (uint32_t q = 0; q < b->index_count; q++) {
                GpuVertex in, e;
                gx_vertex_unpack(f, b, f->indices.data[b->index_first + q], &in);
                gx_xf_eval(f->xf.data, &in, &e);
                if (e.pos[3] <= 0) { behind++; continue; }
                float z = e.pos[2] / e.pos[3];
                if (z < zmin) zmin = z;
                if (z > zmax) zmax = z;
                if (e.pos[2] < 0) nearc++;
                if (e.pos[2] > e.pos[3]) farc++;
            }
            if (b->index_count) fprintf(o, "  depth z/w %.6f..%.6f, w<=0 %u, before near %u, past far %u (of %u)\n", zmin, zmax, behind, nearc, farc, b->index_count);
        }
    }
    fclose(o);
    gx_frame_release(f);
    return true;
}
