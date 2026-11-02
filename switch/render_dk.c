/* The renderer: render.h and gfx.h on deko3d (devkitPro's low-level Maxwell API). It replays each
 * decoded GX frame on an emulated EFB. docs/DEKO3D_MIGRATION.md has the design and how it came about.
 *
 * Memory: a few large blocks, sub-allocated. Per present frame (NSLOT in turn, each behind a fence)
 * a data ring (vertices, indices, XF records, uniform blocks, texture staging) and command memory.
 * Images (textures and render targets) live in an image heap; every image has an image descriptor
 * slot (its id, also the GfxTex the front end sees). Freed images and descriptor slots wait NSLOT + 1
 * frames, so nothing the GPU may still read is reused.
 *
 * Shaders: GLSL (shaders_glsl.h, xf_vs.h, tev_gen.c), compiled by the
 * vendored uam on a worker thread and cached as DKSH (sdmc:/switch/MKDD-NX/config/dksh.bin).
 *
 * Orientation: deko3d's clip space has y up (NDC +1 is image row 0) with depth 0..1, so texel row 0 is
 * the top of every target. */
#include "render.h"
#include "gfx.h"
#include <deko3d.h>
#include <malloc.h>
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <unistd.h>
#include "../runtime/gx.h"
#include "../runtime/plat.h"
#include "../runtime/rt.h"
#include "hdtex.h"
#include "shaders_glsl.h"
#include "tev_gen.h"
#include "uam_c.h"
#include "xf_vs.h"

#define NSLOT 3
#define TEV_UNIFORM_SIZE 432
#define MAX_COPIES 256
#define MAX_IMG 4096                 /* image descriptors: textures and render targets */
#define MAX_SMP 64                   /* sampler descriptors */
#define CODE_SIZE (16u << 20)        /* DKSH code */
#define IMG_CHUNK (32u << 20)        /* image heap blocks */
#define RING_MIN (8u << 20)          /* per-slot data ring, grows */
#define CMD_SIZE (1u << 20)          /* per-slot command memory, more added on demand */
#define CMD_SLACK 256                /* deko3d 0.5.0 writes a few words past a piece's end */
#define PROF_MAX 2048                /* GPU profile reports per frame */
static bool s_gpu_test; /* DEBUG > GPU TEST */
#define SUBMIT_DRAWS 128             /* submit the commands recorded so far every N draws */

#define ALIGN(x, a) (((x) + (a) - 1) & ~((uint64_t)(a) - 1))

/* ---- device, queue, frames ---- */
typedef struct { DkMemBlock blk; uint8_t *cpu; DkGpuAddr gpu; uint32_t size, used; } Ring;
typedef struct {
    Ring ring;
    DkMemBlock cmdmem;
    DkMemBlock old[16];  /* blocks replaced while this slot's frame was recorded: destroyed after its fence */
    unsigned nold;
    DkFence fence;
    bool fenced;
    DkMemBlock query;    /* GPU timer reports (begin, mid, end) */
    bool timed, test_ok;
    uint8_t test;
    DkMemBlock prof;     /* config/dk_profile: a timestamp after each draw, copy and clear of one frame */
    unsigned nprof;      /* reports written this frame (0: not profiled) */
} Slot;

static struct {
    DkDevice dev;
    DkQueue q;
    DkCmdBuf cmd;
    Slot slots[NSLOT];
    unsigned slot;
    uint64_t frame;      /* present frames started */
    bool recording;      /* a frame's commands are being recorded */
    bool lost;           /* the queue is in an error state: draw nothing more */
    bool zcull, zcomp, mips, tiled, profile, blit_xfb, xfb_direct, copy_cap, out4, z24, test; /* GPU switches (cfg_flag, read in gfx_open) */
    unsigned draws_since_submit;
    /* swapchain */
    DkSwapchain swap;
    DkImage swap_img[3];
    DkMemBlock swap_mem[3];
    int swap_w, swap_h, acquired; /* acquired: image slot, -1 none */
    /* descriptors */
    DkMemBlock desc;
    DkGpuAddr img_desc, smp_desc;
    uint8_t *desc_cpu;
    bool desc_dirty;     /* descriptors written this frame: invalidate before the next use */
    /* code */
    DkMemBlock code;
    uint32_t code_used;
    char describe[256];
} D;

static void dk_log(void *u, const char *ctx, DkResult r, const char *msg) {
    (void)u;
    rt_log("deko3d: %s: %s (result %d)", ctx ? ctx : "?", msg ? msg : "", (int)r);
}

/* Every block, for gfx_close: a block left at exit keeps its heap pages locked to the GPU, and the
 * loader then can't take the heap back for the next .nro (the error screen, with no crash report). */
#define BLOCKS_MAX 1024
static DkMemBlock s_blocks[BLOCKS_MAX];
static unsigned s_nblocks;
static DkMemBlock memblock(uint32_t size, uint32_t flags) {
    DkMemBlockMaker m;
    dkMemBlockMakerDefaults(&m, D.dev, (uint32_t)ALIGN(size, DK_MEMBLOCK_ALIGNMENT));
    m.flags = flags;
    DkMemBlock b = dkMemBlockCreate(&m);
    if (b && s_nblocks < BLOCKS_MAX) s_blocks[s_nblocks++] = b;
    return b;
}
static void block_free(DkMemBlock b) {
    for (unsigned i = 0; i < s_nblocks; i++)
        if (s_blocks[i] == b) { s_blocks[i] = s_blocks[--s_nblocks]; break; }
    dkMemBlockDestroy(b);
}

/* a block replaced or freed while the current frame may use it: destroyed after the slot's fence */
static void block_later(DkMemBlock b) {
    Slot *s = &D.slots[D.slot];
    if (!b) return;
    if (s->nold == sizeof s->old / sizeof *s->old) {
        dkQueueWaitIdle(D.q); /* (never in practice) */
        for (unsigned i = 0; i < s->nold; i++) block_free(s->old[i]);
        s->nold = 0;
    }
    s->old[s->nold++] = b;
}

/* ---- data ring: per slot, bump allocated, replaced by a bigger block when full ---- */
static bool ring_grow(Ring *r, uint32_t need) {
    uint32_t size = r->size ? r->size : RING_MIN;
    while (size < need + r->used / 2 + RING_MIN / 2) size *= 2;
    DkMemBlock b = memblock(size, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    if (!b) return false;
    block_later(r->blk);
    r->blk = b;
    r->cpu = dkMemBlockGetCpuAddr(b);
    r->gpu = dkMemBlockGetGpuAddr(b);
    r->size = size;
    r->used = 0;
    rt_log("render: frame data ring now %u MB", size >> 20);
    return true;
}
/* size bytes in this frame's ring: CPU pointer, GPU address in *gpu; NULL if out of memory */
static void *ring_alloc(uint32_t size, uint32_t align, DkGpuAddr *gpu) {
    Ring *r = &D.slots[D.slot].ring;
    uint32_t off = (uint32_t)ALIGN(r->used, align);
    if (!r->blk || off + size > r->size) {
        if (!ring_grow(r, size + align)) return NULL;
        off = 0;
    }
    r->used = off + size;
    *gpu = r->gpu + off;
    return r->cpu + off;
}

/* ---- command memory ---- */
static void cmd_addmem(void *u, DkCmdBuf cmd, size_t need) {
    (void)u;
    uint32_t size = need + CMD_SLACK > CMD_SIZE ? (uint32_t)ALIGN(need + CMD_SLACK, DK_MEMBLOCK_ALIGNMENT) : CMD_SIZE;
    DkMemBlock b = memblock(size, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    if (!b) { rt_log("render: out of command memory"); return; }
    block_later(b);
    dkCmdBufAddMemory(cmd, b, 0, size - CMD_SLACK);
}

/* ---- image heap: IMG_CHUNK blocks, first fit with coalescing ---- */
typedef struct { DkMemBlock blk; uint32_t off, size; int chunk; } ImgAlloc;
#define MAX_CHUNKS 32
#define MAX_FREE 1024
typedef struct { uint32_t off, size; } Span;
static struct {
    DkMemBlock blk[MAX_CHUNKS];
    uint32_t size[MAX_CHUNKS];
    Span free_[MAX_CHUNKS][MAX_FREE];
    unsigned nfree[MAX_CHUNKS];
    int nchunks;
    uint64_t bytes;
} H;

static void span_free(int c, uint32_t off, uint32_t size) {
    Span *f = H.free_[c];
    unsigned n = H.nfree[c], i = 0;
    while (i < n && f[i].off < off) i++;
    bool merge_prev = i > 0 && f[i - 1].off + f[i - 1].size == off;
    bool merge_next = i < n && off + size == f[i].off;
    if (merge_prev && merge_next) {
        f[i - 1].size += size + f[i].size;
        memmove(&f[i], &f[i + 1], (n - i - 1) * sizeof *f);
        H.nfree[c] = n - 1;
    } else if (merge_prev) f[i - 1].size += size;
    else if (merge_next) { f[i].off = off; f[i].size += size; }
    else if (n < MAX_FREE) {
        memmove(&f[i + 1], &f[i], (n - i) * sizeof *f);
        f[i] = (Span){off, size};
        H.nfree[c] = n + 1;
    } /* else: leaked (the free list is full; never in practice) */
}

static bool img_mem(uint32_t size, uint32_t align, ImgAlloc *out) {
    size = (uint32_t)ALIGN(size, 512);
    if (align < 512) align = 512;
    for (int pass = 0; pass < 2; pass++) {
        for (int c = 0; c < H.nchunks; c++)
            for (unsigned i = 0; i < H.nfree[c]; i++) {
                Span *s = &H.free_[c][i];
                uint32_t a = (uint32_t)ALIGN(s->off, align), pad = a - s->off;
                if (s->size < pad + size) continue;
                uint32_t end = s->off + s->size;
                Span keep = *s;
                memmove(s, s + 1, (H.nfree[c] - i - 1) * sizeof *s);
                H.nfree[c]--;
                if (pad) span_free(c, keep.off, pad);
                if (a + size < end) span_free(c, a + size, end - a - size);
                *out = (ImgAlloc){H.blk[c], a, size, c};
                H.bytes += size;
                return true;
            }
        if (H.nchunks == MAX_CHUNKS) return false;
        uint32_t csize = size + align > IMG_CHUNK ? (uint32_t)ALIGN(size + align, DK_MEMBLOCK_ALIGNMENT) : IMG_CHUNK;
        DkMemBlock b = memblock(csize, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
        if (!b) {
            static bool told;
            if (!told) rt_log("render: no memory to grow the image heap past %d MB", (int)(((uint64_t)H.nchunks * IMG_CHUNK) >> 20));
            told = true;
            return false;
        }
        int c = H.nchunks++;
        H.blk[c] = b;
        H.size[c] = csize;
        H.nfree[c] = 1;
        H.free_[c][0] = (Span){0, csize};
        rt_log("render: image heap %d MB", (int)(((uint64_t)H.nchunks * IMG_CHUNK) >> 20));
    }
    return false;
}

/* ---- images: one descriptor slot each (the id) ---- */
enum { IF_R8, IF_RG8, IF_RGBA8, IF_BC1, IF_BC7, IF_Z32, IF_Z24 };
typedef struct {
    DkImage img;
    ImgAlloc mem;
    uint32_t w, h, levels;
    uint8_t fmt, live;
    uint64_t freed;      /* frame it was freed (slot reusable NSLOT + 1 frames later) */
} Img;
static Img *s_img;       /* [MAX_IMG], 0 unused */
static uint32_t s_img_free[MAX_IMG], s_nimg_free; /* freed ids, oldest first */
static uint32_t s_img_next = 1;

static void desc_write(uint32_t off, const void *d, uint32_t size) {
    if (D.recording) { dkCmdBufPushData(D.cmd, dkMemBlockGetGpuAddr(D.desc) + off, d, size); D.desc_dirty = true; }
    else memcpy(D.desc_cpu + off, d, size);
}

/* deko3d 0.5.0 picks a tile height for level 0 larger than the image, then shrinks it for the level
 * offsets but not for level 0 itself: images of 6-8 block rows (BC textures 24-32 px tall) get their
 * mip 1 written over level 0. Asking for the shrunk size makes both agree (SwitchWakerHD). */
static void tile_size_fix(DkImageLayoutMaker *m, uint32_t rows) {
    if (m->flags & (DkImageFlags_PitchLinear | DkImageFlags_CustomTileSize)) return;
    uint32_t gobs = (rows + rows / 2 + 7) / 8;
    uint32_t tile = gobs >= 16 ? 4 : gobs >= 8 ? 3 : gobs >= 4 ? 2 : gobs >= 2 ? 1 : 0, shrunk = tile;
    while (shrunk && (8u << (shrunk - 1)) >= rows) shrunk--;
    if (shrunk == tile) return;
    m->flags |= DkImageFlags_CustomTileSize;
    m->tileSize = (DkTileSize)shrunk;
}

static DkImageFormat dk_format(uint8_t f) {
    static const DkImageFormat m[] = {DkImageFormat_R8_Unorm, DkImageFormat_RG8_Unorm, DkImageFormat_RGBA8_Unorm,
                                      DkImageFormat_RGBA_BC1, DkImageFormat_RGBA_BC7_Unorm, DkImageFormat_ZF32, DkImageFormat_Z24S8};
    return m[f];
}

/* An image of w x h (levels mip levels) in fmt; render: usable as a render target. 0 on failure. The
 * descriptor views I8 as (I, I, I, I) and IA8 as (I, I, I, A). */
static uint32_t img_create(uint32_t w, uint32_t h, uint8_t fmt, uint32_t levels, bool render) {
    uint32_t id;
    if (s_nimg_free && s_img[s_img_free[0]].freed + NSLOT + 1 <= D.frame) {
        id = s_img_free[0];
        memmove(s_img_free, s_img_free + 1, --s_nimg_free * sizeof *s_img_free);
    } else if (s_img_next < MAX_IMG) id = s_img_next++;
    else { rt_log("render: out of image descriptors"); return 0; }
    Img *im = &s_img[id];
    if (im->mem.blk) { span_free(im->mem.chunk, im->mem.off, im->mem.size); H.bytes -= im->mem.size; im->mem.blk = NULL; }
    DkImageLayoutMaker m;
    dkImageLayoutMakerDefaults(&m, D.dev);
    m.format = dk_format(fmt);
    m.dimensions[0] = w;
    m.dimensions[1] = h;
    m.mipLevels = levels;
    if (render) m.flags |= DkImageFlags_UsageRender | (fmt != IF_Z32 && fmt != IF_Z24 ? DkImageFlags_Usage2DEngine | DkImageFlags_HwCompression : D.zcomp ? DkImageFlags_HwCompression : 0);
    bool bc = fmt == IF_BC1 || fmt == IF_BC7;
    tile_size_fix(&m, bc ? (h + 3) / 4 : h);
    DkImageLayout lay;
    dkImageLayoutInitialize(&lay, &m);
    if (!img_mem((uint32_t)dkImageLayoutGetSize(&lay), dkImageLayoutGetAlignment(&lay), &im->mem)) {
        rt_log("render: out of image memory (%ux%u)", w, h);
        im->freed = 0;
        if (s_nimg_free < MAX_IMG) { memmove(s_img_free + 1, s_img_free, s_nimg_free * sizeof *s_img_free); s_img_free[0] = id; s_nimg_free++; }
        return 0;
    }
    dkImageInitialize(&im->img, &lay, im->mem.blk, im->mem.off);
    im->w = w, im->h = h, im->levels = levels, im->fmt = fmt, im->live = 1;
    DkImageView v;
    dkImageViewDefaults(&v, &im->img);
    if (fmt == IF_R8) v.swizzle[1] = v.swizzle[2] = v.swizzle[3] = DkImageSwizzle_Red;
    if (fmt == IF_RG8) { v.swizzle[1] = v.swizzle[2] = DkImageSwizzle_Red; v.swizzle[3] = DkImageSwizzle_Green; }
    DkImageDescriptor d;
    dkImageDescriptorInitialize(&d, &v, false, false);
    desc_write(id * sizeof d, &d, sizeof d);
    return id;
}

static void img_free(uint32_t *id) {
    if (!*id || !s_img[*id].live) { *id = 0; return; }
    Img *im = &s_img[*id];
    /* the memory and the slot wait until no submitted frame can use them */
    im->live = 0;
    im->freed = D.frame;
    if (s_nimg_free < MAX_IMG) s_img_free[s_nimg_free++] = *id;
    *id = 0;
}
/* memory of images freed long enough ago goes back to the heap (each frame) */
static void img_reclaim(void) {
    for (unsigned i = 0; i < s_nimg_free; i++) {
        Img *im = &s_img[s_img_free[i]];
        if (im->mem.blk && im->freed + NSLOT + 1 <= D.frame) {
            span_free(im->mem.chunk, im->mem.off, im->mem.size);
            H.bytes -= im->mem.size;
            im->mem.blk = NULL;
        }
    }
}
static void img_view(uint32_t id, DkImageView *v) { dkImageViewDefaults(v, &s_img[id].img); }

/* ---- samplers: wrap s x wrap t x {nearest, linear, trilinear} ---- */
static float s_aniso = 1.0f;
static uint32_t smp_id(unsigned ws, unsigned wt, unsigned filt) { return 1 + (ws * 3 + wt) * 3 + filt; }
static void samplers_write(void) {
    static const DkWrapMode wrap[3] = {DkWrapMode_ClampToEdge, DkWrapMode_Repeat, DkWrapMode_MirroredRepeat};
    for (unsigned s = 0; s < 3; s++)
        for (unsigned t = 0; t < 3; t++)
            for (unsigned f = 0; f < 3; f++) {
                DkSampler sm;
                dkSamplerDefaults(&sm);
                sm.wrapMode[0] = wrap[s];
                sm.wrapMode[1] = wrap[t];
                sm.minFilter = sm.magFilter = f ? DkFilter_Linear : DkFilter_Nearest;
                sm.mipFilter = f == 2 ? DkMipFilter_Linear : DkMipFilter_None;
                if (f) sm.maxAnisotropy = s_aniso;
                DkSamplerDescriptor d;
                dkSamplerDescriptorInitialize(&d, &sm);
                desc_write((uint32_t)(MAX_IMG * sizeof(DkImageDescriptor) + smp_id(s, t, f) * sizeof d), &d, sizeof d);
            }
}
static DkResHandle th(uint32_t img, uint32_t smp) { return dkMakeTextureHandle(img, smp); }

/* ---- shaders ---- */
typedef struct { DkShader s; bool ok; } Shd;
static Shd R_ui_vs, R_ui_fs; /* the overlay's */
static uint64_t fnv(const void *p, size_t n, uint64_t h) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}
#define DKSH_MAGIC 0x4B444B4Du /* "MKDK" */
#define DKSH_VERSION 1u        /* bump with the uam patch level or the GLSL conventions */
static struct {
    U64Map map;          /* key -> offset of the entry's DKSH in data */
    uint8_t *data;       /* the file as loaded (entries: u64 key, u32 size, bytes) */
    size_t size;
    FILE *out;           /* appended to */
    char path[512];
} C;

static bool dksh_load(Shd *sh, const uint8_t *dksh, size_t size) {
    typedef struct { uint32_t magic, header_sz, control_sz, code_sz, programs_off, num_programs; } Hdr;
    if (size < sizeof(Hdr)) return false;
    Hdr h;
    memcpy(&h, dksh, sizeof h);
    if (h.control_sz + h.code_sz > size) return false;
    uint32_t off = (uint32_t)ALIGN(D.code_used, DK_SHADER_CODE_ALIGNMENT);
    if (off + h.code_sz + DK_SHADER_CODE_UNUSABLE_SIZE > CODE_SIZE) { rt_log("render: shader code memory full"); return false; }
    memcpy((uint8_t *)dkMemBlockGetCpuAddr(D.code) + off, dksh + h.control_sz, h.code_sz);
    D.code_used = off + h.code_sz;
    DkShaderMaker m;
    dkShaderMakerDefaults(&m, D.code, off);
    m.control = dksh;
    m.programId = 0;
    dkShaderInitialize(&sh->s, &m);
    sh->ok = dkShaderIsValid(&sh->s);
    return sh->ok;
}

/* Compile worker: uam needs one thread with a big stack. Jobs are posted and either waited for
 * (fixed shaders at start-up) or collected later (TEV variants: the uber shader draws meanwhile). */
/* key: what the result is filed under (shader map); ckey: its DKSH cache key (the source's) */
typedef struct Job { struct Job *next; uint64_t key, ckey; bool frag; char *src; uint8_t *dksh; size_t size; bool done, ok; } Job;
static struct {
    Thread th;
    Mutex mx;
    CondVar cv, done_cv;
    Job *head, *tail, *done;
    bool running, quit;
    uint64_t compile_ns;
} W;

static void worker(void *arg) {
    (void)arg;
    uamc_init();
    mutexLock(&W.mx);
    while (!W.quit) {
        Job *j = W.head;
        if (!j) { condvarWait(&W.cv, &W.mx); continue; }
        W.head = j->next;
        if (!W.head) W.tail = NULL;
        mutexUnlock(&W.mx);
        char log[1024] = "";
        uint64_t t0 = plat_now_ns();
        j->ok = uamc_compile(j->frag, j->src, &j->dksh, &j->size, log, sizeof log);
        uint64_t dt = plat_now_ns() - t0;
        if (!j->ok) rt_log("render: uam failed (%s): %s", j->frag ? "fragment" : "vertex", log);
        else if (log[0]) rt_log("render: uam: %s", log);
        mutexLock(&W.mx);
        W.compile_ns += dt;
        j->done = true;
        j->next = W.done;
        W.done = j;
        condvarWakeAll(&W.done_cv);
    }
    mutexUnlock(&W.mx);
}

static bool worker_start(void) {
    mutexInit(&W.mx);
    condvarInit(&W.cv);
    condvarInit(&W.done_cv);
    /* priority 0x3B: below the game (0x2C); 0x3C+ fails */
    Result rc = threadCreate(&W.th, worker, NULL, NULL, 8u << 20, 0x3B, -2);
    if (R_FAILED(rc)) { rt_log("render: shader worker thread failed (%#x)", rc); return false; }
    threadStart(&W.th);
    W.running = true;
    return true;
}

static uint64_t shader_key(bool frag, const char *src) {
    uint64_t h = fnv(&(uint32_t){DKSH_VERSION}, 4, 0xCBF29CE484222325ull);
    h = fnv(&frag, 1, h);
    return fnv(src, strlen(src), h) | 1;
}

static void cache_put(uint64_t key, const uint8_t *dksh, size_t size) {
    if (!C.out) return;
    uint32_t sz = (uint32_t)size;
    fwrite(&key, 8, 1, C.out);
    fwrite(&sz, 4, 1, C.out);
    fwrite(dksh, 1, size, C.out);
    fflush(C.out);
    fsync(fileno(C.out)); /* a HOME exit doesn't close files */
}

static Job *job_post(bool frag, const char *glsl, uint64_t key) {
    Job *j = calloc(1, sizeof *j);
    if (!j) return NULL;
    j->key = j->ckey = key;
    j->frag = frag;
    j->src = strdup(glsl);
    if (!j->src) { free(j); return NULL; }
    mutexLock(&W.mx);
    if (W.tail) W.tail->next = j; else W.head = j;
    W.tail = j;
    condvarWakeOne(&W.cv);
    mutexUnlock(&W.mx);
    return j;
}
static void job_unlink_done(Job *j) {
    for (Job **p = &W.done; *p; p = &(*p)->next)
        if (*p == j) { *p = j->next; return; }
}
static void job_free(Job *j) { free(j->src); free(j->dksh); free(j); }

/* A shader from the cache or compiled now (waits). */
static bool shader_get(Shd *sh, bool frag, const char *glsl, const char *name) {
    char *src = strdup(glsl);
    if (!src) return false;
    uint64_t key = shader_key(frag, src), off;
    free(src);
    if (u64map_get(&C.map, key, &off)) {
        uint32_t sz;
        memcpy(&sz, C.data + off - 4, 4);
        if (dksh_load(sh, C.data + off, sz)) return true;
    }
    if (!W.running) return false;
    Job *j = job_post(frag, glsl, key);
    if (!j) return false;
    mutexLock(&W.mx);
    while (!j->done) condvarWait(&W.done_cv, &W.mx);
    job_unlink_done(j);
    mutexUnlock(&W.mx);
    bool ok = j->ok && dksh_load(sh, j->dksh, j->size);
    if (ok) cache_put(key, j->dksh, j->size);
    else rt_log("render: shader %s unusable", name);
    job_free(j);
    return ok;
}

static void cache_open(const char *path) {
    snprintf(C.path, sizeof C.path, "%s", path);
    size_t size = 0;
    uint8_t *d = plat_read_file(path, &size);
    uint32_t hdr[2] = {DKSH_MAGIC, DKSH_VERSION};
    bool valid = d && size >= 8 && !memcmp(d, hdr, 8);
    unsigned n = 0;
    if (valid) {
        size_t p = 8;
        while (p + 12 <= size) {
            uint64_t key;
            uint32_t sz;
            memcpy(&key, d + p, 8);
            memcpy(&sz, d + p + 8, 4);
            if (p + 12 + sz > size) break; /* torn last entry */
            u64map_put(&C.map, key, p + 12);
            p += 12 + sz;
            n++;
        }
        C.data = d;
        C.size = size;
        /* drop a torn tail before appending */
        if (p != size) { FILE *f = fopen(path, "r+b"); if (f) { ftruncate(fileno(f), (off_t)p); fclose(f); } }
    } else free(d);
    C.out = fopen(path, valid ? "ab" : "wb");
    if (C.out && !valid) { fwrite(hdr, 8, 1, C.out); fflush(C.out); }
    rt_log("render: DKSH cache %s: %u shaders", path, n);
}

/* ---- state ---- */
typedef struct { uint32_t id, w, h, addr; uint64_t used; } Target;
#define CHANS_BC1 8
typedef struct { uint32_t id; uint16_t subst; uint8_t wrap_s, wrap_t, chans, hd, movie, enh_set, levels; bool linear; uint64_t used, key; uint32_t w, h, hd_bytes, enh_gen; } DkTexture;

static struct {
    Shd vs, fs_uber, vs_full, fs_copy, fs_copy_half, fs_copy_alpha, fs_blit, fs_blit_shrink, fs_deblock, fs_luma, fs_chroma;
    bool xf_gpu;
    /* specialised TEV fragment shaders: key -> index + 1 into progs (0 failed) */
    U64Map tev;
    Shd *progs;
    unsigned nprogs, cap_progs;
    U64Map pending;      /* keys posted to the worker */
    unsigned compiles_this_frame;
    uint64_t new_tex, new_tex_ns;
    float squeeze, shift_cur;
    uint32_t below_tex;
    float shift;
    unsigned below_unit;
    bool hud43, backdrop;
    Target bd[4];
    uint64_t gpu_wait_ns;
    bool hd_on, hd_flush;
    uint64_t hd_bytes;
    uint32_t white;
    uint32_t efb_color, efb_depth, efb_w, efb_h;
    bool efb_z24;
    Target xfb;
    bool have_xfb;
    /* since the last Fragments or Full barrier: the EFB was drawn or cleared; a copy target was written
     * (a copy reads the EFB: the next draw or clear of it waits for the copy) */
    bool efb_written, copy_written;
    U64Map rts;
    Target rt[512];
    unsigned nrt, rt_recycled;
    U64Map textures;
    DkTexture *tex;
    unsigned ntex, cap_tex;
    uint64_t tick, shown;
    uint8_t *rgba;
    size_t rgba_cap;
    uint32_t stat_batches, stat_verts;
    uint64_t white_rt, white_tex;
    bool uploads_pending; /* copies into images since the last barrier */
    /* what's bound (skipped when unchanged) */
    const DkShader *cur_fs, *cur_vs;
    DkResHandle cur_tex[8];
    uint32_t cur_z, cur_blend, cur_cull, cur_pix;
    bool pipe_valid;
    DkScissor cur_sc;
    int cur_target;      /* 0 none, 1 EFB, 2 other */
    DkGpuAddr vs_ubo;
} R;

static unsigned (*s_subst)(unsigned id);
void render_set_subst(unsigned (*texture)(unsigned id)) { s_subst = texture; }
static float (*s_shift)(unsigned id);
void render_set_shift(float (*shift)(unsigned id)) { s_shift = shift; }
static unsigned (*s_below)(unsigned id);
void render_set_below(unsigned (*texture)(unsigned id)) { s_below = texture; }

static void state_forget(void) {
    R.cur_fs = R.cur_vs = NULL;
    memset(R.cur_tex, 0xFF, sizeof R.cur_tex);
    R.pipe_valid = false;
    R.cur_cull = ~0u;
    R.cur_sc.width = 0;
    R.cur_target = 0;
    R.squeeze = R.shift_cur = -1;
}

/* With the tiled cache on (config/dk_tiled, a test), binned primitives finish before a barrier orders
 * them: deko3d flushes it itself only at fences and reports (SwitchWakerHD's gpu_sync.cpp). */
static void barrier(DkBarrier mode, uint32_t invalidate) {
    if (D.tiled && mode != DkBarrier_None) dkCmdBufTiledCacheOp(D.cmd, DkTiledCacheOp_Flush);
    dkCmdBufBarrier(D.cmd, mode, invalidate);
    if (mode >= DkBarrier_Fragments) R.efb_written = R.copy_written = false;
}

/* ---- GPU profile (config/dk_profile): one frame in 300 gets a timestamp after each draw, copy and
 * clear; when its fence has passed, the time between two timestamps goes to what ended at the second.
 * Timestamps let work overlap, so the split ranks the costs rather than measuring them exactly. */
enum { PK_DRAW = 1, PK_UBER, PK_COPY, PK_CLEAR, PK_OTHER };
typedef struct { uint8_t kind, pass, stages, ntex; uint16_t blend, z; uint32_t batch, tris, px; } ProfTag;
static ProfTag s_ptag[NSLOT][PROF_MAX];
static void prof_mark(unsigned kind, const Batch *b, size_t batch, unsigned pass, uint32_t px) {
    Slot *s = &D.slots[D.slot];
    if (!s->nprof || s->nprof >= PROF_MAX) return;
    ProfTag *t = &s_ptag[D.slot][s->nprof];
    memset(t, 0, sizeof *t);
    t->kind = (uint8_t)kind, t->pass = (uint8_t)pass, t->batch = (uint32_t)batch, t->px = px;
    if (b) {
        t->tris = b->index_count / 3;
        t->stages = (uint8_t)(b->tev.params[0] & 31);
        for (int m = 0; m < 8; m++) t->ntex += b->tev.textures[m] != 0;
        t->blend = (uint16_t)b->tev.pipe[1], t->z = (uint16_t)b->tev.pipe[0];
    }
    /* the drain makes each timestamp time only its own work (the profiled frame runs slower) */
    dkCmdBufBarrier(D.cmd, DkBarrier_Fragments, 0);
    dkCmdBufReportCounter(D.cmd, DkCounter_Timestamp, dkMemBlockGetGpuAddr(s->prof) + 16u * s->nprof++);
}
static void prof_report(Slot *s, unsigned slot) {
    const uint64_t *r = dkMemBlockGetCpuAddr(s->prof);
    const ProfTag *tag = s_ptag[slot];
    double sum[6] = {0}, total = 0, topms[10];
    unsigned cnt[6] = {0}, top[10], ntop = 0;
    for (unsigned i = 1; i < s->nprof; i++) {
        if (r[2 * i + 1] < r[2 * i - 1]) continue;
        double ms = (double)dkTimestampToNs(r[2 * i + 1] - r[2 * i - 1]) / 1e6;
        unsigned k = tag[i].kind >= PK_DRAW && tag[i].kind <= PK_OTHER ? tag[i].kind : PK_OTHER;
        sum[k] += ms, cnt[k]++, total += ms;
        if (k == PK_COPY && ms >= 0.05) rt_log("gpu profile:   copy %.2f ms (pass %u, %u px)", ms, tag[i].pass, tag[i].px);
        if (k != PK_DRAW && k != PK_UBER) continue;
        if (ntop == 10 && ms <= topms[9]) continue;
        unsigned j = ntop < 10 ? ntop++ : 9;
        while (j > 0 && topms[j - 1] < ms) { topms[j] = topms[j - 1]; top[j] = top[j - 1]; j--; }
        topms[j] = ms, top[j] = i;
    }
    rt_log("gpu profile: %.2f ms: %u draws %.2f ms, %u with the uber shader %.2f ms, %u copies %.2f ms, %u clears %.2f ms, other %.2f ms%s",
           total, cnt[PK_DRAW], sum[PK_DRAW], cnt[PK_UBER], sum[PK_UBER], cnt[PK_COPY], sum[PK_COPY], cnt[PK_CLEAR], sum[PK_CLEAR],
           sum[PK_OTHER], s->nprof >= PROF_MAX ? " (report limit reached)" : "");
    for (unsigned j = 0; j < ntop; j++) {
        const ProfTag *t = &tag[top[j]];
        rt_log("gpu profile:   %.2f ms batch %u (pass %u)%s: %u tris, %u stages, %u textures, scissor %u px, blend %#x, z %#x",
               topms[j], t->batch, t->pass, t->kind == PK_UBER ? " UBER" : "", t->tris, t->stages, t->ntex, t->px, t->blend, t->z);
    }
}

/* ---- frames ---- */
static void frame_begin(void) {
    if (D.recording) return;
    Slot *s = &D.slots[D.slot];
    if (s->fenced) {
        uint64_t t0 = plat_now_ns();
        dkFenceWait(&s->fence, -1);
        R.gpu_wait_ns += plat_now_ns() - t0;
        s->fenced = false;
        if (s->nprof) { prof_report(s, D.slot); s->nprof = 0; }
    }
    for (unsigned i = 0; i < s->nold; i++) block_free(s->old[i]);
    s->nold = 0;
    s->ring.used = 0;
    img_reclaim();
    dkCmdBufClear(D.cmd);
    dkCmdBufAddMemory(D.cmd, s->cmdmem, 0, CMD_SIZE - CMD_SLACK);
    D.recording = true;
    D.draws_since_submit = 0;
    dkCmdBufBindImageDescriptorSet(D.cmd, D.img_desc, MAX_IMG);
    dkCmdBufBindSamplerDescriptorSet(D.cmd, D.smp_desc, MAX_SMP);
    dkCmdBufTiledCacheOp(D.cmd, D.tiled ? DkTiledCacheOp_Enable : DkTiledCacheOp_Disable); /* deko3d never writes it */
    state_forget();
}

static void queue_failed_report(void);
static void submit(void) {
    if (!D.recording) return;
    if (dkQueueIsInErrorState(D.q)) queue_failed_report(); /* deko3d aborts below: the log must reach the card first */
    dkQueueSubmitCommands(D.q, dkCmdBufFinishList(D.cmd));
    dkQueueFlush(D.q);
    D.draws_since_submit = 0;
}

static bool queue_ok(const char *what) {
    if (D.lost) return false;
    if (dkQueueIsInErrorState(D.q)) {
        D.lost = true;
        rt_log("render: the GPU queue failed (%s): nothing more is drawn", what);
        return false;
    }
    return true;
}

/* before a draw, clear or copy: descriptors written and images uploaded this frame become visible */
static void sync_before_use(void) {
    if (D.desc_dirty) { barrier(DkBarrier_None, DkInvalidateFlags_Descriptors); D.desc_dirty = false; }
    if (R.uploads_pending) { barrier(DkBarrier_Full, DkInvalidateFlags_Image); R.uploads_pending = false; }
    if (R.copy_written) barrier(DkBarrier_Fragments, DkInvalidateFlags_Image);
}
/* what was rendered is complete before it is sampled (or rendered over by a pass reading it) */
static void sync_rendered(void) { barrier(DkBarrier_Fragments, DkInvalidateFlags_Image); }

/* ---- swapchain ---- */
static void swap_create(int w, int h) {
    const DkImage *imgs[3];
    for (int i = 0; i < 3; i++) {
        DkImageLayoutMaker m;
        dkImageLayoutMakerDefaults(&m, D.dev);
        m.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine | DkImageFlags_HwCompression;
        m.format = DkImageFormat_RGBA8_Unorm;
        m.dimensions[0] = (uint32_t)w;
        m.dimensions[1] = (uint32_t)h;
        DkImageLayout lay;
        dkImageLayoutInitialize(&lay, &m);
        D.swap_mem[i] = memblock((uint32_t)ALIGN(dkImageLayoutGetSize(&lay), dkImageLayoutGetAlignment(&lay)),
                                 DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
        dkImageInitialize(&D.swap_img[i], &lay, D.swap_mem[i], 0);
        imgs[i] = &D.swap_img[i];
    }
    DkSwapchainMaker sm;
    dkSwapchainMakerDefaults(&sm, D.dev, nwindowGetDefault(), imgs, 3);
    D.swap = dkSwapchainCreate(&sm);
    dkSwapchainSetSwapInterval(D.swap, 1);
    D.swap_w = w, D.swap_h = h, D.acquired = -1;
}
static void swap_destroy(void) {
    dkQueueWaitIdle(D.q);
    if (D.swap) dkSwapchainDestroy(D.swap);
    D.swap = NULL;
    for (int i = 0; i < 3; i++) { if (D.swap_mem[i]) block_free(D.swap_mem[i]); D.swap_mem[i] = NULL; }
}

/* the screen image of this frame, acquired on first use */
static void bind_screen(void) {
    frame_begin();
    if (D.acquired < 0) D.acquired = dkQueueAcquireImage(D.q, D.swap);
    DkImageView v;
    dkImageViewDefaults(&v, &D.swap_img[D.acquired]);
    dkCmdBufBindRenderTarget(D.cmd, &v, NULL);
    DkViewport vp = {0, 0, (float)D.swap_w, (float)D.swap_h, 0, 1};
    dkCmdBufSetViewports(D.cmd, 0, &vp, 1);
    DkScissor sc = {0, 0, (uint32_t)D.swap_w, (uint32_t)D.swap_h};
    dkCmdBufSetScissors(D.cmd, 0, &sc, 1);
    R.cur_sc.width = 0;
    R.cur_target = 2;
}

static bool s_ui_ok;
static bool shader_get(Shd *sh, bool frag, const char *glsl, const char *name);
static bool worker_start(void);
static void cache_open(const char *path);

/* ---- gfx.h ---- */
/* GPU switches for hardware A/B tests: a file of that name in sdmc:/switch/MKDD-NX/config/ */
static bool cfg_flag(const char *name) {
    char path[96];
    snprintf(path, sizeof path, "sdmc:/switch/MKDD-NX/config/%s", name);
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

bool gfx_open(int w, int h) {
    D.zcull = !cfg_flag("dk_nozcull"); /* on since the 2026-10-30 hardware run: no artefacts, a little faster */
    D.zcomp = !cfg_flag("dk_nozcomp");
    D.mips = !cfg_flag("dk_nomips");
    D.tiled = cfg_flag("dk_tiled");
    D.blit_xfb = !cfg_flag("dk_noblit");
    D.profile = cfg_flag("dk_profile");
    D.xfb_direct = !cfg_flag("dk_noxfbdirect");
    D.copy_cap = !cfg_flag("dk_nocopycap");
    D.out4 = cfg_flag("dk_output4");
    D.z24 = cfg_flag("dk_z24");
    D.test = s_gpu_test; /* the GPU comparison, with the profile: see gpu_test */
    if (D.test) D.profile = true;
    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    dm.cbDebug = dk_log;
    dm.flags = DkDeviceFlags_DepthZeroToOne | DkDeviceFlags_OriginUpperLeft;
    D.dev = dkDeviceCreate(&dm);
    if (!D.dev) return false;
    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, D.dev);
    /* zcull: SwitchWakerHD lost whole tiles of depth-biased decals with it; MKDD showed nothing wrong on
     * the console. config/dk_nozcull turns it off */
    qm.flags = DkQueueFlags_Graphics | DkQueueFlags_MediumPrio | (D.zcull ? DkQueueFlags_EnableZcull : DkQueueFlags_DisableZcull);
    qm.commandMemorySize = 1u << 20;
    qm.flushThreshold = qm.commandMemorySize / 8;
    /* local memory: the uber TEV shader's dynamically indexed arrays live there */
    qm.perWarpScratchMemorySize = 64 * DK_PER_WARP_SCRATCH_MEM_ALIGNMENT;
    D.q = dkQueueCreate(&qm);
    DkCmdBufMaker cm;
    dkCmdBufMakerDefaults(&cm, D.dev);
    cm.cbAddMem = cmd_addmem;
    D.cmd = dkCmdBufCreate(&cm);
    for (int i = 0; i < NSLOT; i++) {
        D.slots[i].cmdmem = memblock(CMD_SIZE, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
        D.slots[i].query = memblock(DK_MEMBLOCK_ALIGNMENT, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuUncached);
        if (!D.slots[i].cmdmem || !D.slots[i].query) return false;
        if (D.profile && !(D.slots[i].prof = memblock(PROF_MAX * 16, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuUncached))) D.profile = false;
    }
    uint32_t dsize = MAX_IMG * sizeof(DkImageDescriptor) + MAX_SMP * sizeof(DkSamplerDescriptor);
    D.desc = memblock(dsize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    D.code = memblock(CODE_SIZE, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code);
    if (!D.desc || !D.code) return false;
    D.desc_cpu = dkMemBlockGetCpuAddr(D.desc);
    memset(D.desc_cpu, 0, dsize);
    D.img_desc = dkMemBlockGetGpuAddr(D.desc);
    D.smp_desc = D.img_desc + MAX_IMG * sizeof(DkImageDescriptor);
    s_img = calloc(MAX_IMG, sizeof *s_img);
    if (!s_img) return false;
    samplers_write();
    swap_create(w, h);
    if (worker_start()) {
        cache_open("sdmc:/switch/MKDD-NX/config/dksh.bin");
        s_ui_ok = shader_get(&R_ui_vs, false, UI_VS, "ui-vs") && shader_get(&R_ui_fs, true, UI_FS, "ui-fs");
    }
    snprintf(D.describe, sizeof D.describe, "deko3d (%s library), %dx%d swapchain of 3; zcull %s, depth compression %s, game mipmaps %s, tiled cache %s, XFB by 2D blit %s%s",
#ifdef MKDD_DEKO3D_DEBUG
             "debug",
#else
             "release",
#endif
             w, h, D.zcull ? "on" : "off", D.zcomp ? "on" : "off", D.mips ? "on" : "off", D.tiled ? "on" : "off", D.blit_xfb ? "on" : "off",
             D.profile ? "; GPU profile every 5 s" : "");
    return true;
}

void gfx_close(void) {
    if (!D.dev) return;
    dkQueueWaitIdle(D.q);
    if (W.running) {
        mutexLock(&W.mx);
        W.quit = true;
        condvarWakeAll(&W.cv);
        mutexUnlock(&W.mx);
        threadWaitForExit(&W.th);
        threadClose(&W.th);
        W.running = false;
    }
    if (C.out) { fclose(C.out); C.out = NULL; }
    swap_destroy();
    dkCmdBufDestroy(D.cmd);
    dkQueueDestroy(D.q);
    unsigned blocks = s_nblocks;
    while (s_nblocks) block_free(s_blocks[s_nblocks - 1]);
    dkDeviceDestroy(D.dev);
    D.dev = NULL;
    rt_log("render: closed, %u memory blocks freed", blocks);
}
const char *gfx_describe(void) { return D.describe; }
void gfx_size(int *w, int *h) { *w = D.swap_w, *h = D.swap_h; }
void gfx_resize(int w, int h) {
    if (w == D.swap_w && h == D.swap_h) return;
    if (D.recording) { submit(); D.recording = false; } /* (the loop resizes before drawing) */
    swap_destroy();
    swap_create(w, h);
    rt_log("render: swapchain now %dx%d", w, h);
}
void gfx_pump(void) { appletMainLoop(); }

/* Debug: while sdmc:/switch/MKDD-NX/config/dk_capture exists, the screen is saved every 300 frames
 * as logs/dk_shot_N.ppm (the picture with the overlay, as presented). Waits for the GPU: debug only. */
static const char *volatile s_shot_path; /* render_capture: the next screen goes here */
void render_capture(const char *path) { s_shot_path = path; }
static void capture_maybe(void) {
    static int enabled = -1;
    static unsigned n;
    if (enabled < 0) { FILE *f = fopen("sdmc:/switch/MKDD-NX/config/dk_capture", "rb"); enabled = f != NULL; if (f) fclose(f); }
    static unsigned calls;
    const char *shot = s_shot_path;
    if (!shot && (!enabled || ++calls % 300)) return;
    s_shot_path = NULL;
    uint32_t w = (uint32_t)D.swap_w, h = (uint32_t)D.swap_h, size = w * h * 4;
    /* made once for the largest screen and kept: later in a session the heap may not have a free 8 MB */
    static DkMemBlock b;
    if (!b) b = memblock(1920 * 1080 * 4, DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuUncached);
    if (!b || size > 1920 * 1080 * 4) { rt_log("render: no memory for a screenshot"); enabled = 0; return; }
    DkImageView v;
    dkImageViewDefaults(&v, &D.swap_img[D.acquired]);
    DkImageRect r = {0, 0, 0, w, h, 1};
    DkCopyBuf dst = {dkMemBlockGetGpuAddr(b), 0, 0};
    barrier(DkBarrier_Full, 0);
    dkCmdBufCopyImageToBuffer(D.cmd, &v, &r, &dst, 0);
    submit();
    dkQueueWaitIdle(D.q);
    dkMemBlockFlushCpuCache(b, 0, size);
    char path[128];
    if (shot) snprintf(path, sizeof path, "%s", shot);
    else snprintf(path, sizeof path, "sdmc:/switch/MKDD-NX/logs/dk_shot_%u.ppm", n++ % 8);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        const uint8_t *p = dkMemBlockGetCpuAddr(b);
        static uint8_t row[1920 * 3];
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w && x < 1920; x++) memcpy(row + x * 3, p + ((size_t)y * w + x) * 4, 3);
            fwrite(row, 3, w < 1920 ? w : 1920, f);
        }
        fclose(f);
        rt_log("render: screen saved to %s", path);
    } else rt_log("render: cannot write %s (errno %d)", path, errno);
}

void gfx_swap(void) {
    if (D.lost) { svcSleepThread(16000000); return; }
    if (D.acquired < 0) { /* nothing drawn on the screen this frame: black */
        bind_screen();
        dkCmdBufClearColorFloat(D.cmd, 0, DkColorMask_RGBA, 0, 0, 0, 1);
    }
    capture_maybe();
    Slot *s = &D.slots[D.slot];
    dkCmdBufSignalFence(D.cmd, &s->fence, true);
    s->fenced = true;
    submit();
    if (!queue_ok("present")) return;
    dkQueuePresentImage(D.q, D.swap, D.acquired);
    D.acquired = -1;
    D.recording = false;
    D.slot = (D.slot + 1) % NSLOT;
    D.frame++;
}

GfxTex gfx_tex_upload(GfxTex tex, int fmt, int w, int h, const void *px) {
    uint8_t f = fmt == GFX_RG8 ? IF_RG8 : IF_RGBA8;
    if (tex && (s_img[tex].w != (uint32_t)w || s_img[tex].h != (uint32_t)h || s_img[tex].fmt != f)) img_free(&tex);
    if (!tex) tex = img_create((uint32_t)w, (uint32_t)h, f, 1, false);
    if (!tex) return 0;
    frame_begin();
    uint32_t bpp = f == IF_RG8 ? 2 : 4, size = (uint32_t)w * (uint32_t)h * bpp;
    DkGpuAddr g;
    void *p = ring_alloc(size, 256, &g);
    if (!p) return tex;
    memcpy(p, px, size);
    DkImageView v;
    img_view(tex, &v);
    DkCopyBuf src = {g, 0, 0};
    DkImageRect rect = {0, 0, 0, (uint32_t)w, (uint32_t)h, 1};
    sync_before_use();
    dkCmdBufCopyBufferToImage(D.cmd, &src, &v, &rect, 0);
    R.uploads_pending = true;
    return tex;
}

/* the overlay */
static DkGpuAddr ubo_put(const void *data, uint32_t size) {
    DkGpuAddr g = 0;
    void *p = ring_alloc((uint32_t)ALIGN(size, 16), DK_UNIFORM_BUF_ALIGNMENT, &g);
    if (p) memcpy(p, data, size);
    return p ? g : 0;
}
static void bind_shaders2(const Shd *vs, const Shd *fs) {
    if (R.cur_vs == &vs->s && R.cur_fs == &fs->s) return;
    const DkShader *sh[2] = {&vs->s, &fs->s};
    dkCmdBufBindShaders(D.cmd, DkStageFlag_GraphicsMask, sh, 2);
    R.cur_vs = &vs->s, R.cur_fs = &fs->s;
}
/* fixed state of the renderer's own passes: no culling, no depth, blend or not */
static void pass_state(bool blend) {
    DkRasterizerState rs;
    dkRasterizerStateDefaults(&rs);
    rs.cullMode = DkFace_None;
    dkCmdBufBindRasterizerState(D.cmd, &rs);
    DkColorState cs;
    dkColorStateDefaults(&cs);
    dkColorStateSetBlendEnable(&cs, 0, blend);
    dkCmdBufBindColorState(D.cmd, &cs);
    DkColorWriteState cw;
    dkColorWriteStateDefaults(&cw);
    dkCmdBufBindColorWriteState(D.cmd, &cw);
    DkBlendState bs;
    dkBlendStateDefaults(&bs);
    dkCmdBufBindBlendStates(D.cmd, 0, &bs, 1);
    DkDepthStencilState ds;
    dkDepthStencilStateDefaults(&ds);
    ds.depthTestEnable = ds.depthWriteEnable = false;
    dkCmdBufBindDepthStencilState(D.cmd, &ds);
    R.pipe_valid = false;
    R.cur_cull = ~0u;
}

bool gfx_ui_begin(int w, int h, bool clear) {
    (void)w, (void)h;
    if (D.lost) return false;
    bind_screen();
    if (clear) dkCmdBufClearColorFloat(D.cmd, 0, DkColorMask_RGBA, 0, 0, 0, 1);
    if (!s_ui_ok) return false;
    sync_before_use();
    pass_state(true);
    bind_shaders2(&R_ui_vs, &R_ui_fs);
    return true;
}
void gfx_ui_draw(const GfxUiBatch *b, unsigned n, GfxTex tex) {
    if (!n || !s_ui_ok) return;
    if (n > GFX_UI_BATCH) n = GFX_UI_BATCH;
    GfxUiBatch u = *b;
    u.screen[0] = (float)D.swap_w, u.screen[1] = (float)D.swap_h;
    DkGpuAddr g = ubo_put(&u, sizeof u);
    if (!g) return;
    sync_before_use();
    dkCmdBufBindUniformBuffer(D.cmd, DkStage_Vertex, 0, g, (uint32_t)ALIGN(sizeof u, 16));
    dkCmdBufBindTexture(D.cmd, DkStage_Fragment, 0, th(tex ? tex : R.white, smp_id(0, 0, 1)));
    R.cur_tex[0] = ~0u;
    dkCmdBufDraw(D.cmd, DkPrimitive_Triangles, 6, n, 0, 0);
}
void gfx_ui_end(void) {}

/* ---- renderer ---- */
static bool tev_compile_now; /* render_shader_cache: wait for each compile */

static bool tev_install(uint64_t key, const uint8_t *dksh, size_t size) {
    if (R.nprogs == R.cap_progs) {
        unsigned cap = R.cap_progs ? R.cap_progs * 2 : 256;
        Shd *p = realloc(R.progs, cap * sizeof *p);
        if (!p) return false;
        R.progs = p, R.cap_progs = cap;
    }
    Shd *sh = &R.progs[R.nprogs];
    bool ok = dksh_load(sh, dksh, size);
    u64map_put(&R.tev, key, ok ? R.nprogs + 1 : 0);
    if (ok) R.nprogs++;
    return ok;
}

/* finished worker jobs (TEV variants) into code memory */
static void collect_jobs(void) {
    if (!W.running) return;
    mutexLock(&W.mx);
    Job *done = W.done;
    W.done = NULL;
    mutexUnlock(&W.mx);
    while (done) {
        Job *j = done;
        done = j->next;
        u64map_del(&R.pending, j->key);
        if (j->ok && tev_install(j->key, j->dksh, j->size)) cache_put(j->ckey, j->dksh, j->size);
        else u64map_put(&R.tev, j->key, 0);
        job_free(j);
    }
}

static FILE *s_states; /* shaders.bin: the TEV states seen, for the next boot */
#define CACHE_MAGIC 0x4D4B5456u /* "MKTV" */

/* Specialised fragment shader for a batch, or the uber shader until it is compiled. */
static const Shd *tev_program(const TevState *t) {
    uint64_t gkey = tev_shader_key(t), v;
    if (u64map_get(&R.tev, gkey, &v)) return v ? &R.progs[v - 1] : &R.fs_uber;
    if (u64map_get(&R.pending, gkey, NULL)) return &R.fs_uber;
    static char src[65536];
    if (!tev_gen_fs(t, src, sizeof src)) { u64map_put(&R.tev, gkey, 0); return &R.fs_uber; }
    char *dsrc = strdup(src);
    if (!dsrc) return &R.fs_uber;
    uint64_t key = shader_key(true, dsrc), off;
    free(dsrc);
    if (u64map_get(&C.map, key, &off)) {
        uint32_t sz;
        memcpy(&sz, C.data + off - 4, 4);
        if (tev_install(gkey, C.data + off, sz)) return &R.progs[R.nprogs - 1];
    }
    if (s_states && !tev_compile_now) { fwrite(t, sizeof *t, 1, s_states); fflush(s_states); }
    /* the worker compiles it; the cache stores it under its source key */
    Job *j = W.running ? job_post(true, src, key) : NULL;
    if (!j) { u64map_put(&R.tev, gkey, 0); return &R.fs_uber; }
    j->key = gkey;
    if (!tev_compile_now) {
        u64map_put(&R.pending, gkey, 1);
        return &R.fs_uber;
    }
    mutexLock(&W.mx);
    while (!j->done) condvarWait(&W.done_cv, &W.mx);
    job_unlink_done(j);
    mutexUnlock(&W.mx);
    bool ok = j->ok && tev_install(gkey, j->dksh, j->size);
    if (ok) cache_put(key, j->dksh, j->size);
    else u64map_put(&R.tev, gkey, 0);
    job_free(j);
    return ok ? &R.progs[R.nprogs - 1] : &R.fs_uber;
}

bool render_init(void) {
    if (!W.running) return false;
    uint64_t t0 = plat_now_ns();
    R.xf_gpu = shader_get(&R.vs, false, XF_VS, "xf-vs");
    if (!R.xf_gpu) {
        rt_log("render: XF vertex shader failed, transforming vertices on the CPU");
        g_gx_cpu_xform_forced = true;
        if (!shader_get(&R.vs, false, PRE_VS, "pre-vs")) return false;
    }
    bool ok = shader_get(&R.fs_uber, true, TEV_FS, "tev") && shader_get(&R.vs_full, false, FULL_VS, "full-vs") &&
              shader_get(&R.fs_copy, true, COPY_FS, "copy") && shader_get(&R.fs_copy_half, true, COPY_HALF_FS, "copy-half") &&
              shader_get(&R.fs_copy_alpha, true, COPY_HALF_ALPHA_FS, "copy-half-alpha") &&
              shader_get(&R.fs_blit, true, BLIT_FS, "blit") && shader_get(&R.fs_blit_shrink, true, BLIT_SHRINK_FS, "blit-shrink");
    if (!ok) return false;
    rt_log("render: fixed shaders ready in %.2f s (%u KB of code)", (double)(plat_now_ns() - t0) / 1e9, D.code_used >> 10);
    static const uint8_t white[4] = {255, 255, 255, 255};
    R.white = gfx_tex_upload(0, GFX_RGBA8, 1, 1, white);
    rt_log("render: anisotropic filtering up to 16x");
    rt_log("render: BC7 textures supported; CMPR textures kept compressed (BC1)");
    return R.white != 0;
}

void render_shader_cache(const char *path, void (*progress)(unsigned done, unsigned total)) {
    size_t size = 0;
    uint8_t *d = plat_read_file(path, &size);
    uint32_t hdr[2] = {CACHE_MAGIC, (uint32_t)sizeof(TevState)};
    bool valid = d && size >= 8 && !memcmp(d, hdr, 8);
    unsigned total = valid ? (unsigned)((size - 8) / sizeof(TevState)) : 0, before = R.nprogs;
    uint64_t t0 = plat_now_ns();
    tev_compile_now = true;
    for (unsigned i = 0; i < total; i++) {
        TevState t;
        memcpy(&t, d + 8 + (size_t)i * sizeof t, sizeof t);
        tev_program(&t);
        if (progress && (i % 8 == 0 || i + 1 == total)) progress(i + 1, total);
    }
    tev_compile_now = false;
    free(d);
    s_states = fopen(path, valid ? "ab" : "wb");
    if (s_states && !valid) { fwrite(hdr, 8, 1, s_states); fflush(s_states); }
    rt_log("render: %u cached TEV states, %u shaders ready in %.1f s", total, R.nprogs - before,
           (double)(plat_now_ns() - t0) / 1e9);
}

/* The picture is shown from the EFB itself (see xfb_direct) */
static struct { bool on, clear; uint32_t src[2], size[2]; float gamma; unsigned seen, direct; } s_direct;

static void ensure_efb(uint32_t w, uint32_t h) {
    if (R.efb_w == w && R.efb_h == h && R.efb_z24 == D.z24) return;
    img_free(&R.efb_color);
    img_free(&R.efb_depth);
    R.efb_color = img_create(w, h, IF_RGBA8, 1, true);
    R.efb_depth = img_create(w, h, D.z24 ? IF_Z24 : IF_Z32, 1, true);
    R.efb_w = w, R.efb_h = h, R.efb_z24 = D.z24;
    s_direct.on = s_direct.clear = false; /* the picture was in the old image */
    if (!R.efb_color || !R.efb_depth) return;
    /* power-on state: black, depth at the far plane */
    DkImageView c, z;
    img_view(R.efb_color, &c);
    img_view(R.efb_depth, &z);
    dkCmdBufBindRenderTarget(D.cmd, &c, &z);
    DkScissor sc = {0, 0, w, h};
    dkCmdBufSetScissors(D.cmd, 0, &sc, 1);
    dkCmdBufClearColorFloat(D.cmd, 0, DkColorMask_RGBA, 0, 0, 0, 1);
    dkCmdBufClearDepthStencil(D.cmd, true, 1.0f, 0, 0);
    if (D.zcull) barrier(DkBarrier_None, DkInvalidateFlags_Zcull);
    R.cur_target = 0;
    R.cur_sc.width = 0;
}

static void target_init(Target *t, uint32_t w, uint32_t h) {
    img_free(&t->id);
    t->id = img_create(w, h, IF_RGBA8, 1, true);
    t->w = w, t->h = h;
}

static Target *rt_for(uint32_t addr, uint32_t w, uint32_t h) {
    uint64_t idx;
    if (u64map_get(&R.rts, addr, &idx)) {
        Target *t = &R.rt[idx];
        if (t->w != w || t->h != h) target_init(t, w, h);
        t->used = R.tick;
        return t;
    }
    Target *t;
    if (R.nrt < sizeof R.rt / sizeof *R.rt) {
        idx = R.nrt++;
        t = &R.rt[idx];
        memset(t, 0, sizeof *t);
    } else {
        idx = 0;
        for (unsigned i = 1; i < R.nrt; i++) if (R.rt[i].used < R.rt[idx].used) idx = i;
        t = &R.rt[idx];
        u64map_del(&R.rts, t->addr);
        if (R.rt_recycled++ < 5) rt_log("render: render target table full, %#x reuses the one of %#x", addr, t->addr);
    }
    target_init(t, w, h);
    t->addr = addr;
    t->used = R.tick;
    u64map_put(&R.rts, addr, idx);
    return t;
}

static void bind_efb(void) {
    if (R.cur_target == 1) return;
    DkImageView c, z;
    img_view(R.efb_color, &c);
    img_view(R.efb_depth, &z);
    dkCmdBufBindRenderTarget(D.cmd, &c, &z);
    DkViewport vp = {0, 0, (float)R.efb_w, (float)R.efb_h, 0, 1};
    dkCmdBufSetViewports(D.cmd, 0, &vp, 1);
    R.cur_target = 1;
    R.cur_sc.width = 0;
}

static void set_scissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (R.cur_sc.x == x && R.cur_sc.y == y && R.cur_sc.width == w && R.cur_sc.height == h) return;
    R.cur_sc = (DkScissor){x, y, w, h};
    dkCmdBufSetScissors(D.cmd, 0, &R.cur_sc, 1);
}

static void unit_bind(unsigned unit, DkResHandle h) {
    if (R.cur_tex[unit] == h) return;
    R.cur_tex[unit] = h;
    dkCmdBufBindTexture(D.cmd, DkStage_Fragment, unit, h);
}

/* Redrawn movie planes (MOVIE_SETS sets in turn) */
#define MOVIE_SETS 4
static struct {
    int state;
    uint32_t yd, w, h;
    struct { uint32_t y, u, v, gen; } set[MOVIE_SETS];
    unsigned next;
    uint32_t gen;
    uint64_t seen_frames, seen_tick;
} M;

static uint8_t *decode_buf(size_t need) {
    if (need > R.rgba_cap) {
        uint8_t *p = realloc(R.rgba, need);
        if (!p) return NULL;
        R.rgba = p, R.rgba_cap = need;
    }
    return R.rgba;
}

/* upload w x h texels (or BC blocks) of image id's level from data */
static void img_upload(uint32_t id, uint32_t level, uint32_t y, uint32_t w, uint32_t h, const void *data, uint32_t size) {
    DkGpuAddr g;
    void *p = ring_alloc(size, 256, &g);
    if (!p) return;
    memcpy(p, data, size);
    DkImageView v;
    img_view(id, &v);
    v.mipLevelOffset = (uint8_t)level;
    v.mipLevelCount = 1;
    DkCopyBuf src = {g, 0, 0};
    DkImageRect rect = {0, y, 0, w, h, 1};
    /* no barrier between copies: each writes its own (unused) image; the next draw's sync_before_use
     * waits for them all at once */
    dkCmdBufCopyBufferToImage(D.cmd, &src, &v, &rect, 0);
    R.uploads_pending = true;
}

/* The game's mip levels 1.. of t (raw after level 0) into image id, decoded as level 0 was (chans).
 * Returns the levels the image holds: a level that fails to decode ends the chain there, and the
 * descriptor then shows only the levels before it. */
static unsigned upload_mips(const GxTexture *t, uint32_t id, unsigned chans, unsigned levels, uint8_t *buf) {
    size_t off = t->raw_len;
    unsigned i;
    for (i = 1; i < levels; i++) {
        GxTexture lt = *t;
        lt.width = t->width >> i, lt.height = t->height >> i;
        lt.raw = t->raw + off;
        lt.raw_len = gx_tex_size(t->fmt, lt.width, lt.height);
        off += lt.raw_len;
        bool ok = chans == CHANS_BC1 ? gx_texture_cmpr_bc1(&lt, buf) : chans < 4 ? gx_texture_decode_compact(&lt, buf) == chans : gx_texture_decode(&lt, buf);
        if (!ok) break;
        uint32_t size = chans == CHANS_BC1 ? lt.width / 4 * (lt.height / 4) * 8 : lt.width * lt.height * chans;
        img_upload(id, i, 0, lt.width, lt.height, buf, size);
    }
    if (i < levels) {
        Img *im = &s_img[id];
        DkImageView v;
        dkImageViewDefaults(&v, &im->img);
        v.mipLevelCount = (uint8_t)i;
        if (im->fmt == IF_R8) v.swizzle[1] = v.swizzle[2] = v.swizzle[3] = DkImageSwizzle_Red;
        if (im->fmt == IF_RG8) { v.swizzle[1] = v.swizzle[2] = DkImageSwizzle_Red; v.swizzle[3] = DkImageSwizzle_Green; }
        DkImageDescriptor d;
        dkImageDescriptorInitialize(&d, &v, false, false);
        desc_write(id * sizeof d, &d, sizeof d);
    }
    return i;
}

/* texture handle for a texture slot key (decoded texture, EFB copy, or white) */
static void bind_slot(unsigned unit, uint64_t key) {
    if (!key) { unit_bind(unit, th(R.white, smp_id(0, 0, 0))); return; }
    if ((key & 0xFFFF000000000000ull) == RT_TAG) {
        uint32_t mode = (uint32_t)((key >> 32) & 0xFFFF);
        uint64_t idx;
        unsigned ws = mode & 3, wt = (mode >> 2) & 3;
        if (ws > 2) ws = 0;
        if (wt > 2) wt = 0;
        if (u64map_get(&R.rts, (uint32_t)key, &idx) && R.rt[idx].id) {
            R.rt[idx].used = R.tick;
            unit_bind(unit, th(R.rt[idx].id, smp_id(ws, wt, (mode >> 4) & 1)));
        } else {
            R.white_rt++;
            unit_bind(unit, th(R.white, smp_id(ws, wt, (mode >> 4) & 1)));
        }
        return;
    }
    uint64_t idx;
    DkTexture *gt = NULL;
    if (u64map_get(&R.textures, key, &idx)) gt = &R.tex[idx];
    else {
        GxTexture *t = gx_texture_acquire(key);
        if (!t) { R.white_tex++; unit_bind(unit, th(R.white, smp_id(0, 0, 0))); return; }
        uint64_t t0 = plat_now_ns();
        uint8_t *buf = decode_buf((size_t)t->width * t->height * 4);
        unsigned chans = 0;
        if (buf) {
            chans = gx_texture_cmpr_bc1(t, buf) ? CHANS_BC1 : gx_texture_decode_compact(t, buf);
            if (!chans) { gx_texture_decode(t, buf); chans = 4; }
        }
        uint8_t fmt = chans == CHANS_BC1 ? IF_BC1 : chans == 1 ? IF_R8 : chans == 2 ? IF_RG8 : IF_RGBA8;
        unsigned levels = D.mips && t->levels > 1 && !t->movie ? t->levels : 1;
        uint32_t id = buf ? img_create(t->width, t->height, fmt, levels, false) : 0;
        if (!id) { gx_texture_release(t); R.white_tex++; unit_bind(unit, th(R.white, smp_id(0, 0, 0))); return; }
        uint32_t size = chans == CHANS_BC1 ? t->width / 4 * (t->height / 4) * 8 : t->width * t->height * chans;
        img_upload(id, 0, 0, t->width, t->height, buf, size);
        if (levels > 1) levels = upload_mips(t, id, chans, levels, buf);
        if (R.ntex == R.cap_tex) {
            unsigned cap = R.cap_tex ? R.cap_tex * 2 : 256;
            DkTexture *p = realloc(R.tex, cap * sizeof *p);
            if (!p) { img_free(&id); gx_texture_release(t); return; }
            R.tex = p, R.cap_tex = cap;
        }
        gt = &R.tex[R.ntex];
        memset(gt, 0, sizeof *gt);
        gt->id = id;
        gt->chans = (uint8_t)chans;
        gt->w = t->width, gt->h = t->height;
        gt->wrap_s = t->wrap_s > 2 ? 0 : t->wrap_s;
        gt->wrap_t = t->wrap_t > 2 ? 0 : t->wrap_t;
        gt->linear = t->linear;
        gt->levels = (uint8_t)levels;
        gt->subst = t->subst;
        gt->movie = t->movie;
        gt->key = key;
        if (R.hd_on && (!t->subst || t->subst >= GX_SUBST_CHARPACK)) {
            int hd = hdtex_find(t->fmt, t->width, t->height, t->raw, t->raw_len);
            if (hd >= 0 && hdtex_request(hd, key)) gt->hd = 1;
        }
        u64map_put(&R.textures, key, R.ntex++);
        gx_texture_release(t);
        R.new_tex++;
        R.new_tex_ns += plat_now_ns() - t0;
    }
    gt->used = R.tick;
    if (gt->movie && gt->enh_gen && M.set[gt->enh_set].gen == gt->enh_gen) {
        uint32_t e = gt->movie == 1 ? M.set[gt->enh_set].y : gt->movie == 2 ? M.set[gt->enh_set].u : M.set[gt->enh_set].v;
        unit_bind(unit, th(e, smp_id(gt->wrap_s, gt->wrap_t, 1)));
        return;
    }
    uint32_t sub = gt->subst && s_subst ? s_subst(gt->subst) : 0;
    uint32_t below = sub && s_below ? s_below(gt->subst) : 0;
    if (gt->subst && s_shift) {
        float f = s_shift(gt->subst);
        if (f != 0) R.shift = f;
    }
    if (sub) {
        unit_bind(unit, th(sub, gt->subst >= GX_SUBST_CHARPACK ? smp_id(gt->wrap_s, gt->wrap_t, gt->linear) : smp_id(0, 0, 1)));
        if (below) { R.below_tex = below; R.below_unit = unit; }
        return;
    }
    bool tri = gt->hd == 2 || (gt->levels > 1 && gt->linear);
    unit_bind(unit, th(gt->id, smp_id(gt->wrap_s, gt->wrap_t, tri ? 2 : gt->linear)));
}

static void evict_textures(uint64_t age) {
    uint64_t cutoff = R.tick > age ? R.tick - age : 0;
    unsigned w = 0;
    u64map_clear(&R.textures);
    for (unsigned i = 0; i < R.ntex; i++) {
        if (R.tex[i].used < cutoff) {
            if (R.tex[i].hd == 2) R.hd_bytes -= R.tex[i].hd_bytes;
            img_free(&R.tex[i].id);
            continue;
        }
        R.tex[w] = R.tex[i];
        u64map_put(&R.textures, R.tex[w].key, w);
        w++;
    }
    R.ntex = w;
}

/* HD replacements (BC7 with mipmaps), uploaded over frames (no driver staging cost here,
 * so only the byte budget) */
#define HD_UPLOAD_BYTES (4u << 20)
static struct {
    uint8_t *data;
    uint64_t tag;
    uint32_t id;
    unsigned w, h, levels, level, row;
    size_t off;
} s_up;

static void hd_upload_drop(bool free_img) {
    if (free_img) img_free(&s_up.id);
    free(s_up.data);
    memset(&s_up, 0, sizeof s_up);
}

static void hd_upload(void) {
    size_t budget = HD_UPLOAD_BYTES;
    uint64_t ti;
    while (budget) {
        if (!s_up.data) {
            int idx;
            uint8_t *data;
            if (!hdtex_take(&idx, &s_up.tag, &data)) return;
            DkTexture *gt = u64map_get(&R.textures, s_up.tag, &ti) ? &R.tex[ti] : NULL;
            if (!gt || gt->hd != 1 || !data || !R.hd_on) {
                if (gt && gt->hd == 1) gt->hd = 0;
                free(data);
                continue;
            }
            s_up.data = data;
            hdtex_info(idx, &s_up.w, &s_up.h, &s_up.levels);
            s_up.id = img_create(s_up.w, s_up.h, IF_BC7, s_up.levels, false);
            if (!s_up.id) { gt->hd = 0; hd_upload_drop(false); continue; }
        }
        DkTexture *gt = u64map_get(&R.textures, s_up.tag, &ti) ? &R.tex[ti] : NULL;
        if (!gt || gt->hd != 1 || !R.hd_on) {
            if (gt && gt->hd == 1) gt->hd = 0;
            hd_upload_drop(true);
            continue;
        }
        unsigned l = s_up.level, lw = s_up.w >> l ? s_up.w >> l : 1, lh = s_up.h >> l ? s_up.h >> l : 1;
        unsigned brows = (lh + 3) / 4;
        size_t rowbytes = (size_t)((lw + 3) / 4) * 16;
        unsigned n = (unsigned)(budget / rowbytes);
        if (n < 1) n = 1;
        if (n > brows - s_up.row) n = brows - s_up.row;
        unsigned y = s_up.row * 4, hgt = n * 4 < lh - y ? n * 4 : lh - y;
        img_upload(s_up.id, l, y, lw, hgt, s_up.data + s_up.off + (size_t)s_up.row * rowbytes, (uint32_t)(n * rowbytes));
        budget = n * rowbytes >= budget ? 0 : budget - n * rowbytes;
        s_up.row += n;
        if (s_up.row < brows) continue;
        s_up.off += (size_t)brows * rowbytes;
        s_up.row = 0;
        if (++s_up.level < s_up.levels) continue;
        img_free(&gt->id);
        gt->id = s_up.id;
        gt->hd = 2;
        gt->hd_bytes = (uint32_t)s_up.off;
        R.hd_bytes += s_up.off;
        s_up.id = 0;
        hd_upload_drop(false);
    }
}

/* GX culling (gen mode 1 = back, 2 = front): GX front faces wind clockwise in GX clip space (y up),
 * which deko3d sees unflipped. */
static void apply_cull(uint32_t mode) {
    if (mode == R.cur_cull) return;
    R.cur_cull = mode;
    DkRasterizerState rs;
    dkRasterizerStateDefaults(&rs);
    rs.frontFace = DkFrontFace_CW;
    rs.cullMode = mode == 1 ? DkFace_Back : mode == 2 ? DkFace_Front : DkFace_None;
    dkCmdBufBindRasterizerState(D.cmd, &rs);
}

static bool blend_uses_src_alpha(uint32_t blend) {
    uint32_t sf = (blend >> 8) & 7, df = (blend >> 5) & 7;
    return (blend & 1) && !((blend >> 11) & 1) && (sf == 4 || sf == 5 || df == 4 || df == 5);
}
static uint32_t const_alpha_word(uint32_t blend, uint32_t pix) {
    bool ca = ((blend >> 4) & 1) && ((pix >> 16) & 1);
    return ca && !blend_uses_src_alpha(blend) ? 0x100u | ((pix >> 8) & 0xFFu) : 0u;
}
static DkCompareOp compare(uint32_t f) {
    static const DkCompareOp m[8] = {DkCompareOp_Never, DkCompareOp_Less, DkCompareOp_Equal, DkCompareOp_Lequal,
                                     DkCompareOp_Greater, DkCompareOp_NotEqual, DkCompareOp_Gequal, DkCompareOp_Always};
    return m[f & 7];
}
static DkBlendFactor blend_factor(uint32_t f, bool src_side) {
    switch (f & 7) {
    case 0: return DkBlendFactor_Zero;
    case 1: return DkBlendFactor_One;
    case 2: return src_side ? DkBlendFactor_DstColor : DkBlendFactor_SrcColor;
    case 3: return src_side ? DkBlendFactor_InvDstColor : DkBlendFactor_InvSrcColor;
    case 4: return DkBlendFactor_SrcAlpha;
    case 5: return DkBlendFactor_InvSrcAlpha;
    case 6: return DkBlendFactor_DstAlpha;
    default: return DkBlendFactor_InvDstAlpha;
    }
}

/* GX Z mode (BP 0x40), blend mode (BP 0x41) and constant alpha */
static void apply_pipe(uint32_t zmode, uint32_t blend, uint32_t pix) {
    if (R.pipe_valid && R.cur_z == zmode && R.cur_blend == blend && R.cur_pix == pix) return;
    R.pipe_valid = true;
    R.cur_z = zmode, R.cur_blend = blend, R.cur_pix = pix;
    DkDepthStencilState ds;
    dkDepthStencilStateDefaults(&ds);
    ds.depthTestEnable = zmode & 1;
    ds.depthWriteEnable = (zmode & 1) && ((zmode >> 4) & 1);
    ds.depthCompareOp = compare((zmode >> 1) & 7);
    dkCmdBufBindDepthStencilState(D.cmd, &ds);
    bool color = (blend >> 3) & 1, alpha = (blend >> 4) & 1;
    bool enabled = blend & 1, logic = (blend >> 1) & 1, subtract = (blend >> 11) & 1;
    uint32_t logic_op = (blend >> 12) & 15;
    bool ca = alpha && ((pix >> 16) & 1);
    /* Constant dst alpha under source alpha blending: the shader's alpha has to stay the blend's, so the EFB's goes
     * towards full instead of to the constant: solid draws fill it (Plus's courses blend everything; left empty, the
     * 4-player shadow pass, which reads the EFB's alpha, painted them navy), see-through ones over a full EFB keep it. */
    bool over = ca && blend_uses_src_alpha(blend);
    DkBlendFactor sa = DkBlendFactor_One, da = over ? DkBlendFactor_InvSrcAlpha : DkBlendFactor_Zero;
    bool on = true;
    DkBlendState bs;
    dkBlendStateDefaults(&bs);
#define BLEND(sf, df, op) do { dkBlendStateSetFactors(&bs, sf, df, ca ? sa : (sf), ca ? da : (df)); \
                               dkBlendStateSetOps(&bs, op, ca ? DkBlendOp_Add : (op)); } while (0)
    if (subtract) BLEND(DkBlendFactor_One, DkBlendFactor_One, DkBlendOp_RevSub);
    else if (enabled) BLEND(blend_factor((blend >> 8) & 7, true), blend_factor((blend >> 5) & 7, false), DkBlendOp_Add);
    else if (logic && logic_op == 0) BLEND(DkBlendFactor_Zero, DkBlendFactor_Zero, DkBlendOp_Add);
    else if (logic && logic_op == 5) { on = false; color = alpha = false; }
    else if (logic && logic_op == 15) BLEND(DkBlendFactor_One, DkBlendFactor_One, DkBlendOp_Max);
    else if (ca) BLEND(DkBlendFactor_One, DkBlendFactor_Zero, DkBlendOp_Add);
    else on = false;
#undef BLEND
    DkColorState cs;
    dkColorStateDefaults(&cs);
    dkColorStateSetBlendEnable(&cs, 0, on);
    dkCmdBufBindColorState(D.cmd, &cs);
    if (on) dkCmdBufBindBlendStates(D.cmd, 0, &bs, 1);
    DkColorWriteState cw;
    dkColorWriteStateDefaults(&cw);
    dkColorWriteStateSetMask(&cw, 0, (color ? DkColorMask_RGB : 0) | (alpha ? DkColorMask_A : 0));
    dkCmdBufBindColorWriteState(D.cmd, &cw);
}

/* vertex attributes per layout (runtime/gx.h) */
static uint32_t s_layout = ~0u;
static void bind_layout(uint32_t layout) {
    if (s_layout == layout) return;
    s_layout = layout;
    DkVtxAttribState a[9];
    memset(a, 0, sizeof a);
#define ATTR(i, off, sz, ty) (a[i] = (DkVtxAttribState){.bufferId = 0, .isFixed = 0, .offset = (off), .size = (sz), .type = (ty)})
    if (layout == GX_LAYOUT_FULL) {
        ATTR(0, offsetof(GpuVertex, pos), DkVtxAttribSize_4x32, DkVtxAttribType_Float);
        ATTR(1, offsetof(GpuVertex, color0), DkVtxAttribSize_4x8, DkVtxAttribType_Unorm);
        ATTR(2, offsetof(GpuVertex, color1), DkVtxAttribSize_4x8, DkVtxAttribType_Unorm);
        for (int p = 0; p < 4; p++) ATTR(3 + p, offsetof(GpuVertex, uv[2 * p]), DkVtxAttribSize_4x32, DkVtxAttribType_Float);
        ATTR(7, offsetof(GpuVertex, nrm), DkVtxAttribSize_3x32, DkVtxAttribType_Float);
        ATTR(8, offsetof(GpuVertex, rec), DkVtxAttribSize_2x32, DkVtxAttribType_Uint);
    } else {
        static const DkVtxAttribSize fsz[5] = {0, DkVtxAttribSize_1x32, DkVtxAttribSize_2x32, DkVtxAttribSize_3x32, DkVtxAttribSize_4x32};
        ATTR(0, 0, DkVtxAttribSize_3x32, DkVtxAttribType_Float);
        ATTR(1, 20, DkVtxAttribSize_4x8, DkVtxAttribType_Unorm);
        ATTR(2, 24, DkVtxAttribSize_4x8, DkVtxAttribType_Unorm);
        for (uint32_t p = 0; p < 4; p++) {
            int comps = 2 * (int)layout - 4 * (int)p;
            if (comps <= 0) a[3 + p] = (DkVtxAttribState){.bufferId = 0, .isFixed = 1, .offset = 0, .size = DkVtxAttribSize_4x32, .type = DkVtxAttribType_Float};
            else ATTR(3 + p, 36 + 16 * p, fsz[comps > 4 ? 4 : comps], DkVtxAttribType_Float);
        }
        ATTR(7, 28, DkVtxAttribSize_3x16, DkVtxAttribType_Sscaled);
        ATTR(8, 12, DkVtxAttribSize_2x32, DkVtxAttribType_Uint);
    }
#undef ATTR
    dkCmdBufBindVtxAttribState(D.cmd, a, R.xf_gpu ? 9 : 7);
    DkVtxBufferState b = {gx_layout_stride(layout), 0};
    dkCmdBufBindVtxBufferState(D.cmd, &b, 1);
}

typedef struct {
    uint32_t at;
    bool xfb, clear;
    uint32_t src[4];
    uint32_t dst_w, dst_h, addr;
    uint32_t regs[8];
    uint32_t mode[4];
    float vfilter[4];
    float half_scale; /* not 0: COPY_HALF_FS, from the EFB at this scale's size */
} CopyOp;
static inline uint32_t bitsu(uint32_t v, uint32_t lo, uint32_t n) { return (v >> lo) & ((1u << n) - 1); }
static uint32_t efb_px(uint32_t v, float scale) { return (uint32_t)((float)v * scale + 0.5f); }

static void decode_copy(const EfbCopy *e, float scale, CopyOp *c) {
    uint32_t cmd = e->bp52;
    memset(c, 0, sizeof *c);
    c->at = e->at_batch;
    c->xfb = cmd & (1u << 14);
    c->clear = (cmd >> 11) & 1;
    c->src[0] = bitsu(e->src, 0, 10);
    c->src[1] = bitsu(e->src, 10, 10);
    c->src[2] = bitsu(e->size, 0, 10) + 1;
    c->src[3] = bitsu(e->size, 10, 10) + 1;
    memcpy(c->regs, e->regs, sizeof c->regs);
    bool half = !c->xfb && (cmd >> 9) & 1;
    /* Half-size colour and alpha copies feed the game's blurs and glows. The console's are half its own
     * size: above 2x the extra pixels only cost, so they stop growing there. (An alpha copy needs an
     * alpha channel in the EFB, pixel format 1. It was the frame's costliest copy at 4x: 3.2 ms.) */
    uint32_t cfmt = bitsu(cmd, 4, 3) | bitsu(cmd, 3, 1) << 3, cpix = c->regs[0] & 7;
    bool fast = D.copy_cap && half && cpix != 3 && !((cmd >> 15) & 1) && (cfmt == 4 || cfmt == 6 || (cfmt == 7 && cpix == 1));
    float cs = fast && scale > 2.0f ? 2.0f : scale;
    if (fast) c->half_scale = cs;
    uint32_t sw = efb_px(c->src[0] + c->src[2], cs) - efb_px(c->src[0], cs);
    uint32_t sh = efb_px(c->src[1] + c->src[3], cs) - efb_px(c->src[1], cs);
    c->dst_w = half ? (sw / 2 ? sw / 2 : 1) : sw;
    c->dst_h = half ? (sh / 2 ? sh / 2 : 1) : sh;
    c->addr = gx_phys(e->dst);
    uint32_t fmt = c->xfb ? 6 : bitsu(cmd, 4, 3) | bitsu(cmd, 3, 1) << 3;
    uint32_t pixfmt = c->regs[0] & 7;
    uint32_t gamma = c->xfb ? (cmd >> 7) & 3 : 0;
    c->mode[0] = fmt;
    c->mode[1] = half;
    c->mode[2] = pixfmt == 3;
    c->mode[3] = ((cmd >> 15) & 1) | (pixfmt << 8) | (gamma << 16);
    uint32_t f0 = c->regs[6], f1 = c->regs[7];
#define W(r, i) (float)(((r) >> (6 * (i))) & 0x3F)
    if (((f0 | f1) & 0xFFFFFF) == 0) { c->vfilter[0] = 0; c->vfilter[1] = 1; c->vfilter[2] = 0; }
    else {
        c->vfilter[0] = (W(f0, 0) + W(f0, 1) + W(f0, 2)) / 64.0f;
        c->vfilter[1] = W(f0, 3) / 64.0f;
        c->vfilter[2] = (W(f1, 0) + W(f1, 1) + W(f1, 2)) / 64.0f;
    }
#undef W
    c->vfilter[3] = (float)(cmd & 3);
}

/* a full-target pass of FULL_VS + fs into image dst (w x h), fs's textures and uniform block bound */
static void pass_target(uint32_t dst, uint32_t w, uint32_t h) {
    DkImageView v;
    img_view(dst, &v);
    dkCmdBufBindRenderTarget(D.cmd, &v, NULL);
    DkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    dkCmdBufSetViewports(D.cmd, 0, &vp, 1);
    DkScissor sc = {0, 0, w, h};
    dkCmdBufSetScissors(D.cmd, 0, &sc, 1);
    R.cur_sc.width = 0;
    R.cur_target = 2;
}

static void movie_enhance(DkTexture *y, DkTexture *u, DkTexture *v) {
    uint32_t w = y->w, h = y->h, W2 = w * 2, H2 = h * 2;
    if (M.w != w || M.h != h) {
        img_free(&M.yd);
        M.yd = img_create(w, h, IF_R8, 1, true);
        for (unsigned s = 0; s < MOVIE_SETS; s++) {
            img_free(&M.set[s].y); img_free(&M.set[s].u); img_free(&M.set[s].v);
            M.set[s].y = img_create(W2, H2, IF_R8, 1, true);
            M.set[s].u = img_create(w, h, IF_R8, 1, true);
            M.set[s].v = img_create(w, h, IF_R8, 1, true);
            M.set[s].gen = 0;
        }
        M.w = w, M.h = h;
    }
    unsigned s = M.next++ % MOVIE_SETS;
    if (!M.yd || !M.set[s].y || !M.set[s].u || !M.set[s].v) return;
    sync_before_use();
    pass_state(false);
    unit_bind(1, th(R.white, smp_id(0, 0, 0)));
    unit_bind(0, th(y->id, smp_id(0, 0, 0)));
    bind_shaders2(&R.vs_full, &R.fs_deblock);
    pass_target(M.yd, w, h);
    dkCmdBufDraw(D.cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
    sync_rendered();
    unit_bind(0, th(M.yd, smp_id(0, 0, 1)));
    bind_shaders2(&R.vs_full, &R.fs_luma);
    pass_target(M.set[s].y, W2, H2);
    dkCmdBufDraw(D.cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
    unit_bind(1, th(M.yd, smp_id(0, 0, 1)));
    unit_bind(0, th(u->id, smp_id(0, 0, 0)));
    bind_shaders2(&R.vs_full, &R.fs_chroma);
    pass_target(M.set[s].u, w, h);
    dkCmdBufDraw(D.cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
    unit_bind(0, th(v->id, smp_id(0, 0, 0)));
    pass_target(M.set[s].v, w, h);
    dkCmdBufDraw(D.cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
    sync_rendered();
    M.set[s].gen = ++M.gen;
    y->enh_set = u->enh_set = v->enh_set = (uint8_t)s;
    y->enh_gen = u->enh_gen = v->enh_gen = M.gen;
}

static void movie_prepass(const Frame *f) {
    if (g_thp_frames != M.seen_frames) { M.seen_frames = g_thp_frames; M.seen_tick = R.tick; }
    if (!M.seen_tick || R.tick - M.seen_tick > 60 || M.state < 0) return;
    if (!M.state) {
        bool ok = shader_get(&R.fs_deblock, true, MOVIE_DEBLOCK_FS, "movie-deblock") && shader_get(&R.fs_luma, true, MOVIE_LUMA_FS, "movie-luma") &&
                  shader_get(&R.fs_chroma, true, MOVIE_CHROMA_FS, "movie-chroma");
        M.state = ok ? 1 : -1;
        if (ok) rt_log("render: movie frames are redrawn (deblocked, brightness 2x, colour guided by brightness)");
        if (!ok) return;
    }
    for (size_t i = 0; i < f->batches.len; i++) {
        const uint64_t *k = f->batches.data[i].tev.textures;
        DkTexture *pl[4] = {0};
        for (unsigned m = 0; m < 8; m++) {
            uint64_t idx;
            if (!k[m] || (k[m] & 0xFFFF000000000000ull) == RT_TAG) continue;
            if (!u64map_get(&R.textures, k[m], &idx)) {
                bind_slot(m, k[m]);
                if (!u64map_get(&R.textures, k[m], &idx)) continue;
            }
            DkTexture *gt = &R.tex[idx];
            if (gt->movie) pl[gt->movie] = gt;
        }
        if (!pl[1] || !pl[2] || !pl[3] || pl[2]->w * 2 != pl[1]->w || pl[3]->w != pl[2]->w) continue;
        if (pl[1]->enh_gen && M.set[pl[1]->enh_set].gen == pl[1]->enh_gen) continue;
        movie_enhance(pl[1], pl[2], pl[3]);
    }
}

static void blit(uint32_t src, uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh, uint32_t dst, uint32_t dx, uint32_t dy, uint32_t dw, uint32_t dh) {
    DkImageView s, d;
    img_view(src, &s);
    img_view(dst, &d);
    DkImageRect sr = {sx, sy, 0, sw, sh, 1}, dr = {dx, dy, 0, dw, dh, 1};
    dkCmdBufBlitImage(D.cmd, &s, &sr, &d, &dr, DkBlitFlag_FilterLinear, 0);
}

/* An XFB copy that is a plain 1:1 copy of EFB rows (no box filter, gamma, intensity or pixel-format
 * rounding; one vertical filter row, of weight 1) goes through the 2D engine instead of COPY_FS: the
 * shader took 2.65 ms of GPU at 1.5x on the console. The present reads only the colour, so the
 * alpha the shader would force to 1 does not matter. Rows whose filter row lies outside the clamp range
 * repeat the edge row, as copy_row does. False: the copy needs the shader. */
static bool xfb_blit(const CopyOp *c, float scale, const Target *dst) {
    if (!D.blit_xfb || !c->xfb || c->mode[1] || c->mode[2] || c->mode[3]) return false;
    int tap = -1;
    for (int i = 0; i < 3; i++) {
        if (c->vfilter[i] == 0.0f) continue;
        if (tap >= 0 || fabsf(c->vfilter[i] - 1.0f) > 0.002f) return false;
        tap = i;
    }
    if (tap < 0 || dst->w != c->dst_w || dst->h != c->dst_h) return false;
    int row = (int)rintf(scale), dy = (tap - 1) * row; /* as COPY_FS: round(u_scale) rows per console row */
    int x0 = (int)efb_px(c->src[0], scale), y0 = (int)efb_px(c->src[1], scale), w = (int)c->dst_w, h = (int)c->dst_h;
    uint32_t flags = (uint32_t)c->vfilter[3];
    int lo = flags & 1 ? y0 : 0, hi = flags & 2 ? (int)efb_px(c->src[1] + c->src[3], scale) - 1 : (int)R.efb_h - 1;
    if (hi > (int)R.efb_h - 1) hi = (int)R.efb_h - 1;
    if (x0 + w > (int)R.efb_w || lo > hi) return false;
    int ra = lo - y0 - dy > 0 ? lo - y0 - dy : 0, rb = hi - y0 - dy + 1 < h ? hi - y0 - dy + 1 : h;
    if (rb <= ra) return false;
    barrier(DkBarrier_Full, DkInvalidateFlags_Image); /* the 2D engine reads what the 3D one drew */
    DkImageView sv, dv;
    img_view(R.efb_color, &sv);
    img_view(dst->id, &dv);
    DkImageRect sr = {(uint32_t)x0, (uint32_t)(y0 + ra + dy), 0, (uint32_t)w, (uint32_t)(rb - ra), 1};
    DkImageRect dr = {0, (uint32_t)ra, 0, (uint32_t)w, (uint32_t)(rb - ra), 1};
    dkCmdBufBlitImage(D.cmd, &sv, &sr, &dv, &dr, DkBlitFlag_FilterNearest, 0);
    if (ra > 0) {
        DkImageRect s1 = {(uint32_t)x0, (uint32_t)lo, 0, (uint32_t)w, 1, 1}, d1 = {0, 0, 0, (uint32_t)w, (uint32_t)ra, 1};
        dkCmdBufBlitImage(D.cmd, &sv, &s1, &dv, &d1, DkBlitFlag_FilterNearest, 0);
    }
    if (rb < h) {
        DkImageRect s2 = {(uint32_t)x0, (uint32_t)hi, 0, (uint32_t)w, 1, 1}, d2 = {0, (uint32_t)rb, 0, (uint32_t)w, (uint32_t)(h - rb), 1};
        dkCmdBufBlitImage(D.cmd, &sv, &s2, &dv, &d2, DkBlitFlag_FilterNearest, 0);
    }
    barrier(DkBarrier_Full, DkInvalidateFlags_Image); /* before the EFB is drawn over or the picture sampled */
    static bool logged;
    if (!logged) { logged = true; rt_log("render: XFB copies go through the 2D engine (%dx%d, filter row %d)", w, h, tap - 1); }
    return true;
}

/* The frame's last copy is the TV picture, with nothing drawn after it, and the next frame starts by
 * clearing: then nothing overwrites the EFB before the present, which can read the picture there. A
 * copy of the whole image is saved, the largest single read and write of the frame at high resolution.
 * The vertical filter is dropped (the console's blends neighbouring rows of its low resolution) and so
 * is the pixel format's rounding to fewer bits. The copy's gamma is applied by the present. */
static bool xfb_direct(const CopyOp *c, size_t batches) {
    if (!D.xfb_direct || !c->xfb || c->at < batches || c->mode[2] || (c->mode[3] & 1)) return false;
    return true;
}

static void efb_clear(const CopyOp *c, float scale, size_t next, unsigned k) {
    uint32_t x = efb_px(c->src[0], scale), y = efb_px(c->src[1], scale);
    uint32_t x1 = efb_px(c->src[0] + c->src[2], scale), y1 = efb_px(c->src[1] + c->src[3], scale);
    if (x1 > R.efb_w) x1 = R.efb_w;
    if (y1 > R.efb_h) y1 = R.efb_h;
    if (x1 <= x || y1 <= y) return;
    uint32_t z = c->regs[4], bl = c->regs[5], ar = c->regs[1], gb = c->regs[2], cz = c->regs[3];
    bind_efb();
    if (R.copy_written) sync_rendered();
    R.efb_written = true;
    set_scissor(x, y, x1 - x, y1 - y);
    bool cu = (bl >> 3) & 1, au = (bl >> 4) & 1, zu = (z >> 4) & 1;
    uint32_t mask = (cu ? DkColorMask_RGB : 0) | (au ? DkColorMask_A : 0);
    if (mask)
        dkCmdBufClearColorFloat(D.cmd, 0, mask, (float)(ar & 0xFF) / 255.0f, (float)((gb >> 8) & 0xFF) / 255.0f,
                                (float)(gb & 0xFF) / 255.0f, (float)((ar >> 8) & 0xFF) / 255.0f);
    if (zu) dkCmdBufClearDepthStencil(D.cmd, true, (float)(cz & 0xFFFFFF) / 16777215.0f, 0, 0);
    prof_mark(PK_CLEAR, NULL, next, k, (x1 - x) * (y1 - y));
}

/* Two players side by side (Frame.vseam): a thin light line where the two 3D pictures meet, before
 * the HUD. (A blur around it, as the GL renderer had, showed as a pale band beside the line.) */
static void seam_line(const Frame *f, float scale) {
    uint32_t cx = (uint32_t)((float)f->efb[0] * scale * 0.5f + 0.5f), h = (uint32_t)((float)f->efb[1] * scale + 0.5f);
    uint32_t line = scale >= 2.0f ? 2 : 1;
    if (!h || cx < line / 2 || cx + line > R.efb_w || h > R.efb_h) return;
    if (R.copy_written) sync_rendered();
    bind_efb();
    set_scissor(cx - line / 2, 0, line, h);
    dkCmdBufClearColorFloat(D.cmd, 0, DkColorMask_RGBA, 0.8f, 0.8f, 0.8f, 1.0f);
    R.efb_written = true;
}

static void tev_bytes(const TevState *t, uint8_t *b) {
    memcpy(b, t->stages, 256);
    memcpy(b + 256, t->regs, 64);
    memcpy(b + 320, t->konst, 64);
    memcpy(b + 384, t->params, 12);
    uint32_t cw = const_alpha_word(t->pipe[1], t->pipe[3]);
    memcpy(b + 396, &cw, 4);
    memcpy(b + 400, t->fog, 16);
    memcpy(b + 416, t->fog_color, 16);
}

/* the vertex shader's block (u_squeeze, u_shift) when either changes */
static void vs_params(float squeeze, float shift) {
    if (squeeze == R.squeeze && shift == R.shift_cur) return;
    R.squeeze = squeeze, R.shift_cur = shift;
    float v[4] = {squeeze, shift, 0, 0};
    DkGpuAddr g = ubo_put(v, sizeof v);
    if (g) dkCmdBufBindUniformBuffer(D.cmd, DkStage_Vertex, 0, g, sizeof v);
}

bool render_update(float scale_in) {
    if (D.lost) return false;
    collect_jobs();
    Frame *f = gx_frame_acquire();
    if (!f) return false;
    uint64_t n = gx_frame_count();
    if (n == R.shown) { gx_frame_release(f); return false; }
    R.shown = n;
    R.tick++;
    R.compiles_this_frame = 0;
    frame_begin();
    if (R.hd_flush) { R.hd_flush = false; evict_textures(0); }
    if (hdtex_available()) hd_upload();
    if (R.tick % 600 == 0) {
        evict_textures(600);
        if (R.white_rt || R.white_tex)
            rt_log("render: %llu texture binds drawn white in 10 s (%llu without a render target, %llu not in the store)",
                   (unsigned long long)(R.white_rt + R.white_tex), (unsigned long long)R.white_rt, (unsigned long long)R.white_tex);
        R.white_rt = R.white_tex = 0;
    }
    else if (R.tick % 30 == 0 && R.ntex > 768) evict_textures(120);
    /* a scale the memory can't hold steps down for good, not a black screen */
    static float cap = 4.0f;
    float scale = scale_in < 1.0f ? 1.0f : scale_in > cap ? cap : scale_in;
    ensure_efb(efb_px(f->efb[0], scale), efb_px(f->efb[1], scale));
    if (!R.efb_color || !R.efb_depth) {
        if (scale > 1.0f) {
            cap = scale - 0.5f < 1.0f ? 1.0f : scale - 0.5f;
            R.efb_w = R.efb_h = 0;
            rt_log("render: no memory for %.2fx: at most %.2fx from now on", (double)scale, (double)cap);
        }
        gx_frame_release(f);
        return false;
    }

    unsigned ncopies = f->copies.len < MAX_COPIES ? (unsigned)f->copies.len : MAX_COPIES;
    static CopyOp copies[MAX_COPIES];
    static CopyOp direct_clear;
    bool direct = false;
    for (unsigned k = 0; k < ncopies; k++) {
        decode_copy(&f->copies.data[k], scale, &copies[k]);
        CopyOp *c = &copies[k];
        if (k + 1 == ncopies && xfb_direct(c, f->batches.len)) direct = true;
        else if (c->xfb) {
            if (!R.have_xfb || R.xfb.w != c->dst_w || R.xfb.h != c->dst_h) { target_init(&R.xfb, c->dst_w, c->dst_h); R.have_xfb = R.xfb.id != 0; }
        } else rt_for(c->addr, c->dst_w, c->dst_h);
    }

    /* the frame's vertices, indices, XF records and TEV blocks into the ring */
    DkGpuAddr vb, ib, xb, ub;
    uint8_t *vm = ring_alloc((uint32_t)f->vb.len + 16, 256, &vb);
    uint8_t *im = ring_alloc((uint32_t)(f->indices.len * 4) + 4, 256, &ib);
    uint32_t xn = f->xf.len ? (uint32_t)f->xf.len * 4 : 16;
    uint8_t *xm = ring_alloc(xn, 256, &xb);
    uint32_t ustride = (uint32_t)ALIGN(TEV_UNIFORM_SIZE, DK_UNIFORM_BUF_ALIGNMENT);
    uint8_t *um = ring_alloc((uint32_t)f->batches.len * ustride + ustride, DK_UNIFORM_BUF_ALIGNMENT, &ub);
    if (!vm || !im || !xm || !um) { gx_frame_release(f); return false; }
    memcpy(vm, f->vb.data, f->vb.len);
    memset(vm + f->vb.len, 0, 16);
    memcpy(im, f->indices.data, f->indices.len * 4);
    if (f->xf.len) memcpy(xm, f->xf.data, (size_t)f->xf.len * 4); else memset(xm, 0, 16);
    static uint32_t *ub_off;
    static size_t ub_cap;
    if (f->batches.len > ub_cap) {
        size_t cap = f->batches.len + f->batches.len / 2;
        uint32_t *p = realloc(ub_off, cap * sizeof *p);
        if (!p) { gx_frame_release(f); return false; }
        ub_off = p, ub_cap = cap;
    }
    {
        /* a batch whose block equals the previous one's shares it */
        static uint8_t tmp[TEV_UNIFORM_SIZE], prev[TEV_UNIFORM_SIZE];
        size_t used = 0;
        for (size_t i = 0; i < f->batches.len; i++) {
            tev_bytes(&f->batches.data[i].tev, tmp);
            if (used && !memcmp(tmp, prev, TEV_UNIFORM_SIZE)) { ub_off[i] = (uint32_t)((used - 1) * ustride); continue; }
            memcpy(um + used * ustride, tmp, TEV_UNIFORM_SIZE);
            memcpy(prev, tmp, TEV_UNIFORM_SIZE);
            ub_off[i] = (uint32_t)(used * ustride);
            used++;
        }
    }

    if (D.profile && R.tick % 300 == 150) { /* report 0: the start */
        dkCmdBufBarrier(D.cmd, DkBarrier_Fragments, 0);
        dkCmdBufReportCounter(D.cmd, DkCounter_Timestamp, dkMemBlockGetGpuAddr(D.slots[D.slot].prof));
        D.slots[D.slot].nprof = 1;
        static unsigned listed;
        for (unsigned k = 0; k < ncopies && listed < 3; k++) {
            const CopyOp *c = &copies[k];
            rt_log("gpu profile: copy %u at batch %u: src %u,%u %ux%u -> %ux%u%s%s%s, fmt %u, depth %u, vfilter %.2f/%.2f/%.2f, to %#x",
                   k, c->at, c->src[0], c->src[1], c->src[2], c->src[3], c->dst_w, c->dst_h, c->xfb ? " XFB" : "",
                   c->mode[1] ? " half" : "", c->clear ? " clear" : "", c->mode[0], c->mode[2], c->vfilter[0], c->vfilter[1],
                   c->vfilter[2], c->addr);
        }
        listed++;
    }
    if (s_direct.on && !direct) {
        /* This frame ends differently: last frame's picture moves to the XFB before the EFB changes. */
        if (!R.have_xfb || R.xfb.w != s_direct.size[0] || R.xfb.h != s_direct.size[1]) {
            target_init(&R.xfb, s_direct.size[0], s_direct.size[1]);
            R.have_xfb = R.xfb.id != 0;
        }
        if (R.have_xfb) {
            barrier(DkBarrier_Full, DkInvalidateFlags_Image);
            DkImageView sv, dv;
            img_view(R.efb_color, &sv);
            img_view(R.xfb.id, &dv);
            DkImageRect sr = {s_direct.src[0], s_direct.src[1], 0, s_direct.size[0], s_direct.size[1], 1};
            DkImageRect dr = {0, 0, 0, s_direct.size[0], s_direct.size[1], 1};
            dkCmdBufBlitImage(D.cmd, &sv, &sr, &dv, &dr, DkBlitFlag_FilterNearest, 0);
            barrier(DkBarrier_Full, DkInvalidateFlags_Image);
        }
        s_direct.on = false;
    }
    s_direct.seen++;
    s_direct.direct += direct;
    if (s_direct.seen == 600) {
        rt_log("render: %u of the last 600 frames shown directly from the EFB", s_direct.direct);
        s_direct.seen = s_direct.direct = 0;
    }
    movie_prepass(f);
    sync_before_use();
    if (s_direct.clear) { s_direct.clear = false; efb_clear(&direct_clear, scale, 0, 0); }
    dkCmdBufBindVtxBuffer(D.cmd, 0, vb, (uint32_t)f->vb.len + 16);
    dkCmdBufBindIdxBuffer(D.cmd, DkIdxFormat_Uint32, ib);
    if (R.xf_gpu) dkCmdBufBindStorageBuffer(D.cmd, DkStage_Vertex, 0, xb, xn);
    s_layout = ~0u;
    size_t next = 0;
    R.stat_batches = (uint32_t)f->batches.len;
    R.stat_verts = f->nverts;
    for (unsigned k = 0; k <= ncopies; k++) {
        size_t end = k < ncopies ? copies[k].at : f->batches.len;
        if (end > f->batches.len) end = f->batches.len;
        if (next < end) {
            bind_efb();
            float sq = R.hud43 && k < ncopies && copies[k].xfb ? 0.25f : 0.0f;
            vs_params(sq, 0.0f);
            uint32_t cur_ub = ~0u;
            bool seam = f->vseam && k < ncopies && copies[k].xfb;
            for (size_t i = next; i < end; i++) {
                const Batch *b = &f->batches.data[i];
                if (seam && b->hud) { seam = false; seam_line(f, scale); bind_efb(); prof_mark(PK_OTHER, NULL, i, k, 0); }
                uint32_t x0 = efb_px(b->rect[0], scale), y0 = efb_px(b->rect[1], scale), x1 = efb_px(b->rect[2], scale), y1 = efb_px(b->rect[3], scale);
                if (sq > 0 && b->hud) {
                    float cx = (float)f->efb[0] * scale * 0.5f;
                    float sh = b->hud == 2 ? -sq * cx : b->hud == 3 ? sq * cx : 0.0f;
                    x0 = (uint32_t)(cx + ((float)x0 - cx) * (1.0f - sq) + sh + 0.5f);
                    x1 = (uint32_t)(cx + ((float)x1 - cx) * (1.0f - sq) + sh + 0.5f);
                }
                if (x1 > R.efb_w) x1 = R.efb_w;
                if (y1 > R.efb_h) y1 = R.efb_h;
                if (!b->index_count || x1 <= x0 || y1 <= y0) continue;
                apply_pipe(b->tev.pipe[0], b->tev.pipe[1], b->tev.pipe[3]);
                apply_cull(b->tev.pipe[2]);
                const Shd *fs = tev_program(&b->tev);
                bind_shaders2(&R.vs, fs);
                if (ub_off[i] != cur_ub) {
                    cur_ub = ub_off[i];
                    dkCmdBufBindUniformBuffer(D.cmd, DkStage_Fragment, 0, ub + cur_ub, TEV_UNIFORM_SIZE);
                }
                R.below_tex = 0;
                R.shift = 0;
                for (unsigned m = 0; m < 8; m++) bind_slot(m, b->tev.textures[m]);
                bind_layout(b->layout);
                sync_before_use();
                bool shifted = R.shift != 0;
                if (shifted) { set_scissor(0, 0, R.efb_w, R.efb_h); vs_params(sq, R.shift / 480.0f); }
                else set_scissor(x0, y0, x1 - x0, y1 - y0);
                dkCmdBufDrawIndexed(D.cmd, DkPrimitive_Triangles, b->index_count, 1, b->index_first, 0, 0);
                if (shifted) vs_params(sq, 0.0f);
                if (R.below_tex) {
                    unit_bind(R.below_unit, th(R.below_tex, smp_id(0, 0, 1)));
                    set_scissor(0, 0, R.efb_w, R.efb_h);
                    vs_params(sq, 32.0f / 480.0f);
                    dkCmdBufDrawIndexed(D.cmd, DkPrimitive_Triangles, b->index_count, 1, b->index_first, 0, 0);
                    vs_params(sq, 0.0f);
                }
                R.efb_written = true;
                prof_mark(fs == &R.fs_uber ? PK_UBER : PK_DRAW, b, i, k, (x1 - x0) * (y1 - y0));
                /* a profiled frame goes to the GPU whole: mid-frame the GPU waits for the next piece, and
                 * that wait landed in whichever draw or copy came first after it */
                if (++D.draws_since_submit >= SUBMIT_DRAWS && !D.slots[D.slot].nprof) submit();
            }
            next = end;
        }
        if (k == ncopies) break;
        CopyOp *c = &copies[k];
        if (direct && k + 1 == ncopies) {
            /* no copy; the clear waits until the present has read the picture */
            uint32_t gidx = (c->mode[3] >> 16) & 3;
            s_direct.src[0] = efb_px(c->src[0], scale), s_direct.src[1] = efb_px(c->src[1], scale);
            s_direct.size[0] = c->dst_w, s_direct.size[1] = c->dst_h;
            if (s_direct.src[0] + s_direct.size[0] > R.efb_w) s_direct.size[0] = R.efb_w - s_direct.src[0];
            if (s_direct.src[1] + s_direct.size[1] > R.efb_h) s_direct.size[1] = R.efb_h - s_direct.src[1];
            s_direct.gamma = gidx == 1 ? 1.0f / 1.7f : gidx >= 2 ? 1.0f / 2.2f : 1.0f;
            s_direct.on = true;
            if (c->clear) { direct_clear = *c; s_direct.clear = true; }
            if (R.have_xfb) { img_free(&R.xfb.id); R.have_xfb = false; R.xfb.w = R.xfb.h = 0; }
            static bool logged;
            if (!logged) {
                logged = true;
                rt_log("render: the picture is shown from the EFB (%ux%u at %u,%u; pixel format %u, gamma %u, filter %.2f/%.2f/%.2f)",
                       s_direct.size[0], s_direct.size[1], s_direct.src[0], s_direct.src[1], (c->mode[3] >> 8) & 7, gidx,
                       (double)c->vfilter[0], (double)c->vfilter[1], (double)c->vfilter[2]);
            }
            continue;
        }
        Target *dst = c->xfb ? (R.have_xfb ? &R.xfb : NULL) : rt_for(c->addr, c->dst_w, c->dst_h);
        if (dst && dst->id && xfb_blit(c, scale, dst)) prof_mark(PK_COPY, NULL, next, k, dst->w * dst->h);
        else if (dst && dst->id && c->half_scale) {
            if (R.efb_written) sync_rendered();
            pass_state(false);
            bind_shaders2(&R.vs_full, c->mode[0] == 7 ? &R.fs_copy_alpha : &R.fs_copy_half);
            float step = 2.0f * scale / c->half_scale;
            struct { float xf[4]; float opaque, pad[3]; } u = {
                {(float)efb_px(c->src[0], scale) / (float)R.efb_w, (float)efb_px(c->src[1], scale) / (float)R.efb_h,
                 step / (float)R.efb_w, step / (float)R.efb_h},
                c->mode[0] == 4 || ((c->mode[3] >> 8) & 7) != 1 ? 1.0f : 0.0f, {0, 0, 0}};
            DkGpuAddr g = ubo_put(&u, sizeof u);
            if (g) {
                dkCmdBufBindUniformBuffer(D.cmd, DkStage_Fragment, 0, g, sizeof u);
                unit_bind(0, th(R.efb_color, smp_id(0, 0, 1)));
                pass_target(dst->id, dst->w, dst->h);
                dkCmdBufDraw(D.cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
            }
            static bool logged;
            if (!logged && c->mode[0] == 7) { logged = true; rt_log("render: half-size alpha copies are capped (%ux%u at scale %.2f)", dst->w, dst->h, (double)scale); }
            R.copy_written = true;
            prof_mark(PK_COPY, NULL, next, k, dst->w * dst->h);
        }
        else if (dst && dst->id) {
            /* copies in a row share one barrier: each reads the EFB and writes its own target */
            if (R.efb_written) sync_rendered();
            pass_state(false);
            bind_shaders2(&R.vs_full, &R.fs_copy);
            struct { uint32_t src[4], mode[4]; float vfilter[4]; float scale, pad[3]; } u;
            memcpy(u.src, c->src, sizeof u.src);
            memcpy(u.mode, c->mode, sizeof u.mode);
            memcpy(u.vfilter, c->vfilter, sizeof u.vfilter);
            u.scale = scale;
            DkGpuAddr g = ubo_put(&u, sizeof u);
            if (g) {
                dkCmdBufBindUniformBuffer(D.cmd, DkStage_Fragment, 0, g, sizeof u);
                unit_bind(0, th(R.efb_color, smp_id(0, 0, 0)));
                unit_bind(1, th(R.efb_depth, smp_id(0, 0, 0)));
                unit_bind(2, th(R.efb_color, smp_id(0, 0, 1)));
                pass_target(dst->id, dst->w, dst->h);
                dkCmdBufDraw(D.cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
            }
            R.copy_written = true; /* sync_before_use waits for it before the EFB is drawn over or it is sampled */
            prof_mark(PK_COPY, NULL, next, k, dst->w * dst->h);
        }
        if (c->clear) efb_clear(c, scale, next, k);
    }
    gx_frame_release(f);
    return true;
}

void render_set_hud43(bool on) { R.hud43 = on; }
void render_set_backdrop(bool on) { R.backdrop = on; }
void render_set_aniso(float level) {
    if (level > 16.0f) level = 16.0f;
    if (level < 1.0f) level = 1.0f;
    if (level == s_aniso) return;
    s_aniso = level;
    samplers_write();
}
void render_set_gpu_test(bool on) { s_gpu_test = on; }
void render_set_hd(bool on) {
    on = on && hdtex_available();
    if (on != R.hd_on) { R.hd_on = on; R.hd_flush = true; }
}
uint64_t render_hd_bytes(void) { return R.hd_bytes; }
unsigned render_hd_in_use(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < R.ntex; i++) n += R.tex[i].hd == 2;
    return n;
}

/* GPU timer: timestamps (16-byte reports) at begin, mid and end, about once a second, read back when
 * the slot comes round again. deko3d's dkTimestampToNs already scales the 19.2 MHz counter. */
static struct { unsigned frames, n; double sum, max, part[2]; bool logged; } s_gt;

/* DEBUG > GPU TEST: in races, every second switches between the normal settings and one change, so one
 * race compares them all over the same stretches of road. GPU time per loop is sampled every 4 loops. */
static const char *const TEST_NAME[] = {"normal", "XFB copy back", "uncapped half copies", "4-tap output"};
enum { TEST_N = sizeof TEST_NAME / sizeof *TEST_NAME };
static struct { unsigned loops, step, cur; double sum[TEST_N]; unsigned n[TEST_N]; bool base_direct, base_cap, base_out4; } s_test;
static void gpu_test_set(unsigned v) {
    D.xfb_direct = v == 1 ? false : s_test.base_direct;
    D.copy_cap = v == 2 ? false : s_test.base_cap;
    D.out4 = v == 3 ? true : s_test.base_out4;
    s_test.cur = v;
}
static void gpu_test_tick(void) {
    if (!s_test.loops++) s_test.base_direct = D.xfb_direct, s_test.base_cap = D.copy_cap, s_test.base_out4 = D.out4;
    if (s_test.loops % 60 == 0) {
        s_test.step++;
        unsigned odd = s_test.step % 2 ? (s_test.step / 2) % (TEST_N - 1) + 1 : 0;
        gpu_test_set(odd);
    }
    if (s_test.loops % 1800 == 0) {
        for (unsigned i = 0; i < TEST_N; i++)
            if (s_test.n[i]) rt_log("gpu test: %s: %.2f ms (%u samples)", TEST_NAME[i], s_test.sum[i] / s_test.n[i], s_test.n[i]);
        memset(s_test.sum, 0, sizeof s_test.sum);
        memset(s_test.n, 0, sizeof s_test.n);
    }
}
static void gt_report(int i) {
    Slot *s = &D.slots[D.slot];
    dkCmdBufReportCounter(D.cmd, DkCounter_Timestamp, dkMemBlockGetGpuAddr(s->query) + 16u * (unsigned)i);
}
void render_gpu_timer_begin(void) {
    if (!s_gt.logged) { s_gt.logged = true; rt_log("render: GPU timer available"); }
    if (D.test) gpu_test_tick(); /* (before frame_begin: the tiled cache is set there) */
    frame_begin();
    Slot *s = &D.slots[D.slot];
    if (s->timed) { /* this slot's last timed frame is done (frame_begin waited for it) */
        const uint64_t *r = dkMemBlockGetCpuAddr(s->query);
        uint64_t a = r[1], b = r[3], c = r[5];
        if (b >= a && c >= b) {
            double x = (double)dkTimestampToNs(b - a) / 1e6, y = (double)dkTimestampToNs(c - b) / 1e6;
            s_gt.sum += x + y;
            s_gt.part[0] += x;
            s_gt.part[1] += y;
            if (x + y > s_gt.max) s_gt.max = x + y;
            s_gt.n++;
            /* a race (many batches), not in the first frames of a setting: its targets are new */
            if (D.test && R.stat_batches >= 150 && s->test_ok) { s_test.sum[s->test] += x + y; s_test.n[s->test]++; }
        }
        s->timed = false;
    }
    if (++s_gt.frames < (D.test ? 4u : 60u)) return;
    s_gt.frames = 0;
    s->timed = true;
    s->test = s_test.cur;
    s->test_ok = s_test.loops % 60 >= 12;
    gt_report(0);
}
void render_gpu_timer_mid(void) { if (D.recording && D.slots[D.slot].timed) gt_report(1); }
void render_gpu_timer_end(void) { if (D.recording && D.slots[D.slot].timed) gt_report(2); }
unsigned render_gpu_timer_take(double *avg_ms, double *max_ms, double *image_ms, double *output_ms) {
    unsigned n = s_gt.n;
    *avg_ms = n ? s_gt.sum / n : 0;
    *image_ms = n ? s_gt.part[0] / n : 0;
    *output_ms = n ? s_gt.part[1] / n : 0;
    *max_ms = s_gt.max;
    s_gt.n = 0;
    s_gt.sum = s_gt.max = s_gt.part[0] = s_gt.part[1] = 0;
    return n;
}
uint64_t render_take_gpu_wait(void) { uint64_t v = R.gpu_wait_ns; R.gpu_wait_ns = 0; return v; }

void render_present(int w, int h, const float adj[3], bool wide) {
    if (D.lost) return;
    float vw = (float)w, vh = (float)h;
    float aspect = wide ? 16.0f / 9.0f : 4.0f / 3.0f;
    if (vw / vh > aspect) vw = vh * aspect;
    else vh = vw / aspect;
    int vx = (int)(((float)w - vw) / 2), vy = (int)(((float)h - vh) / 2);
    bool direct = s_direct.on && R.efb_color, have = direct || R.have_xfb;
    uint32_t pic = direct ? R.efb_color : R.xfb.id;
    uint32_t pw = direct ? s_direct.size[0] : R.xfb.w, ph = direct ? s_direct.size[1] : R.xfb.h;
    uint32_t px = direct ? s_direct.src[0] : 0, py = direct ? s_direct.src[1] : 0;
    float tw = direct ? (float)R.efb_w : (float)pw, th_ = direct ? (float)R.efb_h : (float)ph;
    float gamma = direct ? adj[0] * s_direct.gamma : adj[0];
    if (have) barrier(DkBarrier_Full, DkInvalidateFlags_Image); /* (the backdrop's blits too) */
    if (have && R.backdrop && vx > 0) {
        uint32_t src = pic, sx = px, sy = py, sw = pw, sh = ph;
        for (int i = 0; i < 4 && sw > 100; i++) {
            uint32_t bw = sw / 2 ? sw / 2 : 1, bh = sh / 2 ? sh / 2 : 1;
            if (R.bd[i].w != bw || R.bd[i].h != bh) target_init(&R.bd[i], bw, bh);
            if (!R.bd[i].id) break;
            blit(src, sx, sy, sw, sh, R.bd[i].id, 0, 0, bw, bh);
            src = R.bd[i].id, sx = sy = 0, sw = bw, sh = bh;
        }
        barrier(DkBarrier_Full, DkInvalidateFlags_Image);
        bind_screen();
        pass_state(false);
        bind_shaders2(&R.vs_full, &R.fs_blit);
        float u[8] = {gamma, adj[1], adj[2], 0.55f, 0, 0, 1, 1};
        DkGpuAddr g = ubo_put(u, sizeof u);
        if (g) {
            dkCmdBufBindUniformBuffer(D.cmd, DkStage_Fragment, 0, g, sizeof u);
            unit_bind(0, th(src, smp_id(0, 0, 1)));
            int fh = w * 3 / 4;
            DkViewport vp = {0, (float)((h - fh) / 2), (float)w, (float)fh, 0, 1};
            dkCmdBufSetViewports(D.cmd, 0, &vp, 1);
            set_scissor(0, 0, (uint32_t)vx, (uint32_t)h);
            dkCmdBufDraw(D.cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
            set_scissor((uint32_t)(vx + (int)vw), 0, (uint32_t)(w - vx - (int)vw), (uint32_t)h);
            dkCmdBufDraw(D.cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
        }
    } else bind_screen();
    if (!have) { dkCmdBufClearColorFloat(D.cmd, 0, DkColorMask_RGBA, 0, 0, 0, 1); return; }
    /* clear only the bars beside / above the picture */
    if (vx > 0 && !R.backdrop) {
        set_scissor(0, 0, (uint32_t)vx, (uint32_t)h);
        dkCmdBufClearColorFloat(D.cmd, 0, DkColorMask_RGBA, 0, 0, 0, 1);
        set_scissor((uint32_t)(vx + (int)vw), 0, (uint32_t)(w - vx - (int)vw), (uint32_t)h);
        dkCmdBufClearColorFloat(D.cmd, 0, DkColorMask_RGBA, 0, 0, 0, 1);
    }
    if (vy > 0) {
        set_scissor(0, 0, (uint32_t)w, (uint32_t)vy);
        dkCmdBufClearColorFloat(D.cmd, 0, DkColorMask_RGBA, 0, 0, 0, 1);
        set_scissor(0, (uint32_t)(vy + (int)vh), (uint32_t)w, (uint32_t)(h - vy - (int)vh));
        dkCmdBufClearColorFloat(D.cmd, 0, DkColorMask_RGBA, 0, 0, 0, 1);
    }
    pass_state(false);
    bool shrink = !D.out4 && (float)pw > vw && (float)ph > vh;
    bind_shaders2(&R.vs_full, shrink ? &R.fs_blit_shrink : &R.fs_blit);
    float u[12] = {gamma, adj[1], adj[2], 0.0f, (float)px / tw, (float)py / th_, (float)pw / tw, (float)ph / th_,
                   0.25f * (float)pw / (tw * vw), 0.25f * (float)ph / (th_ * vh), 0, 0};
    DkGpuAddr g = ubo_put(u, sizeof u);
    if (!g) return;
    dkCmdBufBindUniformBuffer(D.cmd, DkStage_Fragment, 0, g, sizeof u);
    unit_bind(0, th(pic, smp_id(0, 0, 1)));
    DkViewport vp = {(float)vx, (float)vy, vw, vh, 0, 1};
    dkCmdBufSetViewports(D.cmd, 0, &vp, 1);
    set_scissor((uint32_t)vx, (uint32_t)vy, (uint32_t)vw, (uint32_t)vh);
    dkCmdBufDraw(D.cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
    if (direct) R.copy_written = true; /* the next frame's first draw or clear of the EFB waits for this read */
    /* the overlay draws next with the whole screen */
    set_scissor(0, 0, (uint32_t)w, (uint32_t)h);
    DkViewport full = {0, 0, (float)w, (float)h, 0, 1};
    dkCmdBufSetViewports(D.cmd, 0, &full, 1);
}

static void queue_failed_report(void) {
    rt_log("render: the GPU queue failed: frame %llu slot %u, %u draws since the last submit, last frame %u batches %u verts, "
           "%u textures, image heap %llu MB in %d chunks, ring %u MB, EFB %ux%u, direct %d, XFB %d, HD upload %ux%u level %u row %u",
           (unsigned long long)D.frame, D.slot, D.draws_since_submit, R.stat_batches, R.stat_verts, R.ntex,
           (unsigned long long)(H.bytes >> 20), H.nchunks, D.slots[D.slot].ring.size >> 20, R.efb_w, R.efb_h, s_direct.on, R.have_xfb,
           s_up.w, s_up.h, s_up.level, s_up.row);
    diag_flush();
}

void render_stats(uint32_t *batches, uint32_t *verts, uint32_t *textures) {
    *batches = R.stat_batches;
    *verts = R.stat_verts;
    *textures = R.ntex;
}
void render_hitch_stats(uint64_t *new_tex, uint64_t *tex_ns, unsigned *programs, uint64_t *compile_ns) {
    *new_tex = R.new_tex;
    *tex_ns = R.new_tex_ns;
    *programs = R.nprogs;
    mutexLock(&W.mx);
    *compile_ns = W.compile_ns;
    mutexUnlock(&W.mx);
}
void render_shader_stats(unsigned *programs, double *compile_ms) {
    *programs = R.nprogs;
    mutexLock(&W.mx);
    *compile_ms = (double)W.compile_ns / 1e6;
    mutexUnlock(&W.mx);
}
