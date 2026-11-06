/* The renderer on Windows: render.h and gfx.h on OpenGL 4.5 core. A port of switch/render_dk.c
 * with the same logic, pass for pass; only the API calls differ. It uses the same GLSL
 * (shaders_glsl.h, xf_vs.h, tev_gen.c), rewritten at compile time for desktop GL (shader_src):
 * "#version 450 core", and the fragment stage's uniform block moved to binding 1, because GL's
 * uniform buffer bindings are shared between stages where deko3d's are per stage.
 *
 * Orientation: glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE) gives deko3d's clip space (NDC y +1 is
 * texel row 0, depth 0..1), so viewports, scissors and copies use the same numbers as render_dk.c.
 * The screen is drawn into an offscreen image (row 0 at the top) and flipped onto the window at swap.
 *
 * Frame data (vertices, indices, XF records, uniform blocks) goes into a persistently mapped ring per
 * present frame (NSLOT in turn, each behind a fence). Textures upload straight from memory. Programs
 * are linked per vertex + fragment pair and kept in config/glprog.bin (glGetProgramBinary). */
#include "../switch/render.h"
#include "../switch/gfx.h"
#include "gl.h"
#include "win.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/gx.h"
#include "../runtime/plat.h"
#include "../runtime/rt.h"
#include "../switch/hdtex.h"
#include "../switch/shaders_glsl.h"
#include "../switch/tev_gen.h"
#include "../switch/xf_vs.h"

#define NSLOT 3
#define TEV_UNIFORM_SIZE 432
#define MAX_COPIES 256
#define MAX_IMG 8192
#define RING_MIN (16u << 20)
#define FS_UBO 1 /* the fragment stage's uniform block binding (shader_src) */
#define ALIGN(x, a) (((x) + (a) - 1) & ~((uint64_t)(a) - 1))

typedef struct { GLuint buf; uint32_t off; } GAddr;
typedef struct {
    GLuint buf;
    uint8_t *cpu;
    uint32_t size, used;
    GLuint old[16]; /* rings replaced while this slot's frame was recorded: deleted after its fence */
    unsigned nold;
    GLsync fence;
    GLuint query[3]; /* GPU timer: timestamps at begin, mid, end */
    bool timed;
} Slot;

static struct {
    Slot slots[NSLOT];
    unsigned slot;
    uint64_t frame;
    bool recording, lost;
    bool mips, blit_xfb, xfb_direct, copy_cap, out4, z24, bc1, parallel; /* switches (cfg_flag) and features */
    GLint ubo_align, ssbo_align;
    GLuint vao, vao_empty, cur_vao;
    GLuint screen_tex, screen_fbo;
    int swap_w, swap_h;
    bool screen_drawn; /* something was drawn on the screen this frame */
    char describe[256];
} D;

static void bind_vao(GLuint v) { if (D.cur_vao != v) { glBindVertexArray(v); D.cur_vao = v; } }

/* ---- ring ---- */
static bool ring_grow(Slot *s, uint32_t need) {
    uint32_t size = s->size ? s->size : RING_MIN;
    while (size < need + s->used / 2 + RING_MIN / 2) size *= 2;
    GLuint b;
    glCreateBuffers(1, &b);
    GLbitfield fl = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    glNamedBufferStorage(b, size, NULL, fl);
    uint8_t *p = glMapNamedBufferRange(b, 0, size, fl);
    if (!p) { glDeleteBuffers(1, &b); rt_log("render: no memory for a %u MB frame ring", size >> 20); return false; }
    if (s->buf) {
        if (s->nold == sizeof s->old / sizeof *s->old) { glFinish(); glDeleteBuffers((GLsizei)s->nold, s->old); s->nold = 0; }
        s->old[s->nold++] = s->buf;
    }
    s->buf = b, s->cpu = p, s->size = size, s->used = 0;
    rt_log("render: frame data ring now %u MB", size >> 20);
    return true;
}
static void *ring_alloc(uint32_t size, uint32_t align, GAddr *g) {
    Slot *s = &D.slots[D.slot];
    uint32_t off = (uint32_t)ALIGN(s->used, align);
    if (!s->buf || off + size > s->size) {
        if (!ring_grow(s, size + align)) return NULL;
        off = 0;
    }
    s->used = off + size;
    g->buf = s->buf, g->off = off;
    return s->cpu + off;
}

/* ---- images: an id each (the GfxTex the front end sees) ---- */
enum { IF_R8, IF_RG8, IF_RGBA8, IF_BC1, IF_BC7, IF_Z32, IF_Z24 };
typedef struct { GLuint tex, fbo; uint32_t w, h, levels; uint8_t fmt, live; } Img;
static Img *s_img;
static bool s_tex_dirty; /* an image was deleted: texture units are bound again (its id may come back at once) */
static uint32_t s_img_free[MAX_IMG], s_nimg_free, s_img_next = 1;

static GLenum gl_ifmt(uint8_t f) {
    static const GLenum m[] = {GL_R8, GL_RG8, GL_RGBA8, 0x83F1 /* COMPRESSED_RGBA_S3TC_DXT1_EXT */,
                               GL_COMPRESSED_RGBA_BPTC_UNORM, GL_DEPTH_COMPONENT32F, GL_DEPTH24_STENCIL8};
    return m[f];
}
/* An image of w x h (levels mip levels) in fmt. 0 on failure. I8 reads as (I, I, I, I), IA8 as (I, I, I, A). */
static uint32_t img_create(uint32_t w, uint32_t h, uint8_t fmt, uint32_t levels, bool render) {
    (void)render;
    uint32_t id;
    if (s_nimg_free) id = s_img_free[--s_nimg_free];
    else if (s_img_next < MAX_IMG) id = s_img_next++;
    else { rt_log("render: out of image ids"); return 0; }
    Img *im = &s_img[id];
    glCreateTextures(GL_TEXTURE_2D, 1, &im->tex);
    glTextureStorage2D(im->tex, (GLsizei)levels, gl_ifmt(fmt), (GLsizei)w, (GLsizei)h);
    glTextureParameteri(im->tex, GL_TEXTURE_MAX_LEVEL, (GLint)levels - 1);
    if (fmt == IF_R8) { GLint sw[4] = {GL_RED, GL_RED, GL_RED, GL_RED}; glTextureParameteriv(im->tex, GL_TEXTURE_SWIZZLE_RGBA, sw); }
    if (fmt == IF_RG8) { GLint sw[4] = {GL_RED, GL_RED, GL_RED, GL_GREEN}; glTextureParameteriv(im->tex, GL_TEXTURE_SWIZZLE_RGBA, sw); }
    im->fbo = 0;
    im->w = w, im->h = h, im->levels = levels, im->fmt = fmt, im->live = 1;
    return id;
}
static void img_free(uint32_t *id) {
    if (!*id || !s_img[*id].live) { *id = 0; return; }
    Img *im = &s_img[*id];
    if (im->fbo) glDeleteFramebuffers(1, &im->fbo);
    glDeleteTextures(1, &im->tex);
    s_tex_dirty = true;
    memset(im, 0, sizeof *im);
    if (s_nimg_free < MAX_IMG) s_img_free[s_nimg_free++] = *id;
    *id = 0;
}
/* a framebuffer with image id as its only colour attachment */
static GLuint img_fbo(uint32_t id) {
    Img *im = &s_img[id];
    if (!im->fbo) {
        glCreateFramebuffers(1, &im->fbo);
        glNamedFramebufferTexture(im->fbo, GL_COLOR_ATTACHMENT0, im->tex, 0);
    }
    return im->fbo;
}

/* ---- samplers: wrap s x wrap t x {nearest, linear, trilinear} ---- */
static float s_aniso = 1.0f;
static GLuint s_smp[1 + 27];
static uint32_t smp_id(unsigned ws, unsigned wt, unsigned filt) { return 1 + (ws * 3 + wt) * 3 + filt; }
static void samplers_write(void) {
    static const GLenum wrap[3] = {GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT};
    for (unsigned s = 0; s < 3; s++)
        for (unsigned t = 0; t < 3; t++)
            for (unsigned f = 0; f < 3; f++) {
                GLuint *sm = &s_smp[smp_id(s, t, f)];
                if (!*sm) glCreateSamplers(1, sm);
                glSamplerParameteri(*sm, GL_TEXTURE_WRAP_S, (GLint)wrap[s]);
                glSamplerParameteri(*sm, GL_TEXTURE_WRAP_T, (GLint)wrap[t]);
                glSamplerParameteri(*sm, GL_TEXTURE_MAG_FILTER, f ? GL_LINEAR : GL_NEAREST);
                glSamplerParameteri(*sm, GL_TEXTURE_MIN_FILTER, f == 2 ? GL_LINEAR_MIPMAP_LINEAR : f ? GL_LINEAR : GL_NEAREST);
                glSamplerParameterf(*sm, 0x84FE /* TEXTURE_MAX_ANISOTROPY */, f ? s_aniso : 1.0f);
            }
}
typedef uint64_t TexH; /* image id | sampler << 32 */
static TexH th(uint32_t img, uint32_t smp) { return img | (uint64_t)smp << 32; }

/* ---- shaders and programs ---- */
typedef struct { GLuint sh; uint64_t key; bool frag, ok; } Shd;
static Shd R_ui_vs, R_ui_fs;
static uint64_t fnv(const void *p, size_t n, uint64_t h) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}
#define PROG_MAGIC 0x4C474B4Du /* "MKGL" */
#define PROG_VERSION 1u
/* program binaries: config/glprog.bin, entries u64 key, u32 format, u32 size, bytes */
static struct { U64Map map; uint8_t *data; size_t size; FILE *out; } C;
static U64Map s_pairs; /* vs key ^ fs key -> program */
static uint64_t s_compile_ns;

/* GLSL ES 3.20 for deko3d -> desktop GLSL 4.50 (malloc'd) */
static char *shader_src(bool frag, const char *glsl) {
    size_t n = strlen(glsl);
    char *s = malloc(n + 64);
    if (!s) return NULL;
    const char *v = strstr(glsl, "#version 320 es");
    if (v) {
        size_t a = (size_t)(v - glsl);
        memcpy(s, glsl, a);
        strcpy(s + a, "#version 450 core");
        strcat(s, v + 15);
    } else memcpy(s, glsl, n + 1);
    if (frag)
        for (char *p = s; (p = strstr(p, "std140, binding=0) uniform")); p++) p[16] = '0' + FS_UBO;
    return s;
}
static uint64_t shader_key(bool frag, const char *src) {
    uint64_t h = fnv(&(uint32_t){PROG_VERSION}, 4, 0xCBF29CE484222325ull);
    h = fnv(&frag, 1, h);
    return fnv(src, strlen(src), h) | 1;
}
static void log_info(GLuint obj, bool prog, const char *what) {
    char log[2048] = "";
    if (prog) glGetProgramInfoLog(obj, sizeof log, NULL, log);
    else glGetShaderInfoLog(obj, sizeof log, NULL, log);
    rt_log("render: %s failed: %s", what, log);
}
/* compile now (the driver may finish it on its threads); false: no shader object */
static bool shd_start(Shd *sh, bool frag, const char *glsl) {
    char *src = shader_src(frag, glsl);
    if (!src) return false;
    sh->frag = frag;
    sh->key = shader_key(frag, src);
    sh->sh = glCreateShader(frag ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
    const char *p = src;
    glShaderSource(sh->sh, 1, &p, NULL);
    glCompileShader(sh->sh);
    free(src);
    return sh->sh != 0;
}
static bool shd_check(Shd *sh, const char *name) {
    GLint ok = 0;
    glGetShaderiv(sh->sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { log_info(sh->sh, false, name); glDeleteShader(sh->sh); sh->sh = 0; }
    sh->ok = ok;
    return ok;
}
static bool shader_get(Shd *sh, bool frag, const char *glsl, const char *name) {
    uint64_t t0 = plat_now_ns();
    bool ok = shd_start(sh, frag, glsl) && shd_check(sh, name);
    s_compile_ns += plat_now_ns() - t0;
    return ok;
}
static void cache_put(uint64_t key, GLuint prog) {
    if (!C.out) return;
    GLint len = 0;
    glGetProgramiv(prog, GL_PROGRAM_BINARY_LENGTH, &len);
    if (len <= 0) return;
    void *b = malloc((size_t)len);
    if (!b) return;
    GLenum fmt = 0;
    GLsizei got = 0;
    glGetProgramBinary(prog, len, &got, &fmt, b);
    if (got > 0) {
        uint32_t f = fmt, sz = (uint32_t)got;
        fwrite(&key, 8, 1, C.out), fwrite(&f, 4, 1, C.out), fwrite(&sz, 4, 1, C.out), fwrite(b, 1, sz, C.out);
        fflush(C.out);
    }
    free(b);
}
static GLuint prog_from_cache(uint64_t key) {
    uint64_t off;
    if (!u64map_get(&C.map, key, &off)) return 0;
    uint32_t fmt, sz;
    memcpy(&fmt, C.data + off - 8, 4);
    memcpy(&sz, C.data + off - 4, 4);
    GLuint p = glCreateProgram();
    glProgramBinary(p, fmt, C.data + off, (GLsizei)sz);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (ok) return p;
    glDeleteProgram(p); /* another driver version: linked again from source */
    u64map_del(&C.map, key);
    return 0;
}
static GLuint prog_link_start(const Shd *vs, const Shd *fs) {
    GLuint p = glCreateProgram();
    glProgramParameteri(p, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    glAttachShader(p, vs->sh);
    glAttachShader(p, fs->sh);
    glLinkProgram(p);
    return p;
}
static bool prog_link_check(GLuint p, uint64_t key, const char *what) {
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { log_info(p, true, what); return false; }
    cache_put(key, p);
    return true;
}
static uint64_t pair_key(const Shd *vs, const Shd *fs) { return fnv(&fs->key, 8, vs->key) | 1; }
/* the program of a vertex + fragment pair: from the cache, or linked now (waits) */
static GLuint pair_prog(const Shd *vs, const Shd *fs) {
    uint64_t key = pair_key(vs, fs), v;
    if (u64map_get(&s_pairs, key, &v)) return (GLuint)v;
    uint64_t t0 = plat_now_ns();
    GLuint p = prog_from_cache(key);
    if (!p) {
        p = prog_link_start(vs, fs);
        if (!prog_link_check(p, key, "link")) { glDeleteProgram(p); p = 0; }
    }
    s_compile_ns += plat_now_ns() - t0;
    u64map_put(&s_pairs, key, p);
    return p;
}
static void cache_open(const char *path) {
    size_t size = 0;
    uint8_t *d = plat_read_file(path, &size);
    uint32_t hdr[2] = {PROG_MAGIC, PROG_VERSION};
    bool valid = d && size >= 8 && !memcmp(d, hdr, 8);
    unsigned n = 0;
    size_t p = 8;
    if (valid) {
        while (p + 16 <= size) {
            uint64_t key;
            uint32_t sz;
            memcpy(&key, d + p, 8);
            memcpy(&sz, d + p + 12, 4);
            if (p + 16 + sz > size) break; /* torn last entry */
            u64map_put(&C.map, key, p + 16);
            p += 16 + sz;
            n++;
        }
        C.data = d, C.size = size;
    } else free(d);
    if (valid && p != size) { /* a torn tail: written again without it */
        plat_write_file_atomic(path, d, p);
    }
    C.out = fopen(path, valid ? "ab" : "wb");
    if (C.out && !valid) { fwrite(hdr, 8, 1, C.out); fflush(C.out); }
    rt_log("render: program cache %s: %u programs", path, n);
}

/* ---- state ---- */
typedef struct { uint32_t id, w, h, addr; uint64_t used; } Target;
#define CHANS_BC1 8
typedef struct { uint32_t id; uint16_t subst; uint8_t wrap_s, wrap_t, chans, hd, movie, enh_set, levels; bool linear; uint64_t used, key; uint32_t w, h, hd_bytes, enh_gen; } GlTexture;
/* a TEV fragment shader: prog = linked with R.vs (0 while compiling or failed) */
typedef struct { Shd fs; GLuint prog; uint64_t key; bool done; } TevProg;

static struct {
    Shd vs, fs_uber, vs_full, fs_copy, fs_copy_half, fs_copy_alpha, fs_blit, fs_blit_shrink, fs_deblock, fs_luma, fs_chroma, fs_mirror;
    Target mirror;
    uint32_t mirror_depth;
    GLuint mirror_fbo;
    bool mirror_z24;
    bool xf_gpu;
    U64Map tev;          /* gx key -> index + 1 into progs (0 failed) */
    TevProg *progs;
    unsigned nprogs, cap_progs;
    U64Map pending;      /* gx key -> index + 1 into progs, still compiling */
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
    GLuint efb_fbo;
    bool efb_z24;
    Target xfb;
    bool have_xfb;
    U64Map rts;
    Target rt[512];
    unsigned nrt, rt_recycled;
    U64Map textures;
    GlTexture *tex;
    unsigned ntex, cap_tex;
    uint64_t tick, shown;
    uint8_t *rgba;
    size_t rgba_cap;
    uint32_t stat_batches, stat_verts;
    uint64_t white_rt, white_tex;
    GLuint cur_prog;
    TexH cur_tex[8];
    uint32_t cur_z, cur_blend, cur_cull, cur_pix;
    bool pipe_valid;
    struct { uint32_t x, y, width, height; } cur_sc;
    int cur_target;      /* 0 none, 1 EFB, 2 other, 3 mirror */
    GAddr vb, ib;        /* this frame's vertices and indices */
} R;

static unsigned (*s_subst)(unsigned id);
void render_set_subst(unsigned (*texture)(unsigned id)) { s_subst = texture; }
static float (*s_shift)(unsigned id);
void render_set_shift(float (*shift)(unsigned id)) { s_shift = shift; }
static unsigned (*s_below)(unsigned id);
void render_set_below(unsigned (*texture)(unsigned id)) { s_below = texture; }

static void state_forget(void) {
    R.cur_prog = 0;
    memset(R.cur_tex, 0xFF, sizeof R.cur_tex);
    R.pipe_valid = false;
    R.cur_cull = ~0u;
    R.cur_sc.width = 0;
    R.cur_target = 0;
    R.squeeze = R.shift_cur = -1;
}

/* ---- frames ---- */
static void frame_begin(void) {
    if (D.recording) return;
    Slot *s = &D.slots[D.slot];
    if (s->fence) {
        uint64_t t0 = plat_now_ns();
        glClientWaitSync(s->fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
        R.gpu_wait_ns += plat_now_ns() - t0;
        glDeleteSync(s->fence);
        s->fence = NULL;
    }
    if (s->nold) glDeleteBuffers((GLsizei)s->nold, s->old), s->nold = 0;
    s->used = 0;
    D.recording = true;
    D.screen_drawn = false;
    state_forget();
}

static void set_viewport(float x, float y, float w, float h) {
    glViewport((GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h);
}
static void bind_fbo(GLuint fbo) { glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo); }

/* the screen image */
static void screen_create(int w, int h) {
    glCreateTextures(GL_TEXTURE_2D, 1, &D.screen_tex);
    glTextureStorage2D(D.screen_tex, 1, GL_RGBA8, w, h);
    glCreateFramebuffers(1, &D.screen_fbo);
    glNamedFramebufferTexture(D.screen_fbo, GL_COLOR_ATTACHMENT0, D.screen_tex, 0);
    D.swap_w = w, D.swap_h = h;
}
static void screen_destroy(void) {
    if (D.screen_fbo) glDeleteFramebuffers(1, &D.screen_fbo);
    if (D.screen_tex) glDeleteTextures(1, &D.screen_tex);
    D.screen_fbo = D.screen_tex = 0;
}
static void bind_screen(void) {
    frame_begin();
    bind_fbo(D.screen_fbo);
    set_viewport(0, 0, (float)D.swap_w, (float)D.swap_h);
    glScissor(0, 0, D.swap_w, D.swap_h);
    R.cur_sc.width = 0;
    R.cur_target = 2;
    D.screen_drawn = true;
}

/* clears (scissored, as the scissor stands): colour channels in mask (1 r, 2 g, 4 b, 8 a), depth */
static void clear_color(unsigned mask, float r, float g, float b, float a) {
    glColorMask(mask & 1, (mask >> 1) & 1, (mask >> 2) & 1, (mask >> 3) & 1);
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
    R.pipe_valid = false;
}
static void clear_depth(float z) {
    glDepthMask(GL_TRUE);
    glClearDepth(z);
    glClear(GL_DEPTH_BUFFER_BIT);
    R.pipe_valid = false;
}

static bool s_ui_ok;

/* ---- gfx.h ---- */
static bool cfg_flag(const char *name) {
    char path[96];
    snprintf(path, sizeof path, "config/%s", name);
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static void APIENTRY gl_debug(GLenum src, GLenum type, GLuint id, GLenum sev, GLsizei len, const GLchar *msg, const void *u) {
    (void)src, (void)id, (void)len, (void)u;
    static unsigned n;
    if (sev == GL_DEBUG_SEVERITY_NOTIFICATION || type == GL_DEBUG_TYPE_PERFORMANCE || n >= 40) return;
    n++;
    rt_log("gl: %s%s", msg, n == 40 ? " (no more GL messages are logged)" : "");
}

static bool has_ext(const char *name) {
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; i++)
        if (!strcmp((const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i), name)) return true;
    return false;
}

bool gfx_open(int w, int h) {
    if (!win_gl_create(w, h)) return false;
    D.mips = !cfg_flag("dk_nomips");
    D.blit_xfb = !cfg_flag("dk_noblit");
    D.xfb_direct = !cfg_flag("dk_noxfbdirect");
    D.copy_cap = !cfg_flag("dk_nocopycap");
    D.out4 = cfg_flag("dk_output4");
    D.z24 = cfg_flag("dk_z24");
    D.bc1 = has_ext("GL_EXT_texture_compression_s3tc");
    D.parallel = has_ext("GL_ARB_parallel_shader_compile") || has_ext("GL_KHR_parallel_shader_compile");
    if (D.parallel && p_glMaxShaderCompilerThreadsARB) glMaxShaderCompilerThreadsARB(0xFFFFFFFFu);
    if (has_ext("GL_KHR_debug")) {
        glEnable(GL_DEBUG_OUTPUT);
        glDebugMessageCallback(gl_debug, NULL);
    }
    glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE);
    glFrontFace(GL_CW);
    glEnable(GL_SCISSOR_TEST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &D.ubo_align);
    glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &D.ssbo_align);
    if (D.ubo_align < 16) D.ubo_align = 256;
    if (D.ssbo_align < 16) D.ssbo_align = 256;
    glCreateVertexArrays(1, &D.vao);
    glCreateVertexArrays(1, &D.vao_empty);
    bind_vao(D.vao_empty);
    s_img = calloc(MAX_IMG, sizeof *s_img);
    if (!s_img) return false;
    samplers_write();
    screen_create(w, h);
    for (int i = 0; i < NSLOT; i++) glGenQueries(3, D.slots[i].query);
    cache_open("config/glprog.bin");
    s_ui_ok = shader_get(&R_ui_vs, false, UI_VS, "ui-vs") && shader_get(&R_ui_fs, true, UI_FS, "ui-fs") && pair_prog(&R_ui_vs, &R_ui_fs);
    snprintf(D.describe, sizeof D.describe, "OpenGL %s, %s (%s), %dx%d; parallel shader compile %s, BC1 %s",
             (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VENDOR), w, h,
             D.parallel ? "on" : "off", D.bc1 ? "on" : "off");
    return true;
}

void gfx_close(void) {
    if (!s_img) return;
    glFinish();
    if (C.out) { fclose(C.out); C.out = NULL; }
    win_gl_destroy();
    rt_log("render: closed");
}
const char *gfx_describe(void) { return D.describe; }
void gfx_size(int *w, int *h) { *w = D.swap_w, *h = D.swap_h; }
void gfx_resize(int w, int h) {
    if (w < 1 || h < 1 || (w == D.swap_w && h == D.swap_h)) return;
    screen_destroy();
    screen_create(w, h);
    rt_log("render: screen now %dx%d", w, h);
}
void gfx_pump(void) { win_pump(); }

/* render_capture: the next presented screen as a .ppm (config/dk_capture: every 300 frames) */
static const char *volatile s_shot_path;
void render_capture(const char *path) { s_shot_path = path; }
static void capture_maybe(void) {
    static int enabled = -1;
    static unsigned n, calls;
    if (enabled < 0) enabled = cfg_flag("dk_capture");
    const char *shot = s_shot_path;
    if (!shot && (!enabled || ++calls % 300)) return;
    s_shot_path = NULL;
    uint32_t w = (uint32_t)D.swap_w, h = (uint32_t)D.swap_h;
    uint8_t *p = malloc((size_t)w * h * 4);
    if (!p) return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, D.screen_fbo);
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, p);
    char path[128];
    if (shot) snprintf(path, sizeof path, "%s", shot);
    else snprintf(path, sizeof path, "logs/dk_shot_%u.ppm", n++ % 8);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (size_t i = 0; i < (size_t)w * h; i++) fwrite(p + i * 4, 3, 1, f);
        fclose(f);
        rt_log("render: screen saved to %s", path);
    }
    free(p);
}

void gfx_swap(void) {
    frame_begin();
    if (!D.screen_drawn) { bind_screen(); glScissor(0, 0, D.swap_w, D.swap_h); clear_color(15, 0, 0, 0, 1); }
    capture_maybe();
    /* the screen image's row 0 is the top: flipped onto the window */
    int ww, wh;
    win_client_size(&ww, &wh);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    R.pipe_valid = false;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitNamedFramebuffer(D.screen_fbo, 0, 0, 0, D.swap_w, D.swap_h, 0, wh, ww, 0, GL_COLOR_BUFFER_BIT,
                           ww == D.swap_w && wh == D.swap_h ? GL_NEAREST : GL_LINEAR);
    glEnable(GL_SCISSOR_TEST);
    Slot *s = &D.slots[D.slot];
    s->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    win_swap();
    D.recording = false;
    D.slot = (D.slot + 1) % NSLOT;
    D.frame++;
}

GfxTex gfx_tex_upload(GfxTex tex, int fmt, int w, int h, const void *px) {
    uint8_t f = fmt == GFX_RG8 ? IF_RG8 : IF_RGBA8;
    if (tex && (s_img[tex].w != (uint32_t)w || s_img[tex].h != (uint32_t)h || s_img[tex].fmt != f)) img_free(&tex);
    if (!tex) tex = img_create((uint32_t)w, (uint32_t)h, f, 1, false);
    if (!tex) return 0;
    glTextureSubImage2D(s_img[tex].tex, 0, 0, 0, w, h, f == IF_RG8 ? GL_RG : GL_RGBA, GL_UNSIGNED_BYTE, px);
    return tex;
}

/* uniform blocks */
static bool ubo_put(GLuint binding, const void *data, uint32_t size) {
    GAddr g;
    void *p = ring_alloc((uint32_t)ALIGN(size, 16), (uint32_t)D.ubo_align, &g);
    if (!p) return false;
    memcpy(p, data, size);
    glBindBufferRange(GL_UNIFORM_BUFFER, binding, g.buf, g.off, (GLsizeiptr)ALIGN(size, 16));
    return true;
}
static void use_prog(GLuint p) {
    if (R.cur_prog == p) return;
    glUseProgram(p);
    R.cur_prog = p;
}
static void bind_shaders2(const Shd *vs, const Shd *fs) { use_prog(pair_prog(vs, fs)); }

/* fixed state of the renderer's own passes: no culling, no depth, blend or not */
static void pass_state(bool blend) {
    glDisable(GL_CULL_FACE);
    if (blend) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    } else glDisable(GL_BLEND);
    glColorMask(1, 1, 1, 1);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    R.pipe_valid = false;
    R.cur_cull = ~0u;
}

static void unit_bind(unsigned unit, TexH h) {
    if (s_tex_dirty) { s_tex_dirty = false; memset(R.cur_tex, 0xFF, sizeof R.cur_tex); }
    if (R.cur_tex[unit] == h) return;
    R.cur_tex[unit] = h;
    glBindTextureUnit(unit, s_img[(uint32_t)h].tex);
    glBindSampler(unit, s_smp[h >> 32]);
}

bool gfx_ui_begin(int w, int h, bool clear) {
    (void)w, (void)h;
    bind_screen();
    if (clear) clear_color(15, 0, 0, 0, 1);
    if (!s_ui_ok) return false;
    pass_state(true);
    bind_vao(D.vao_empty);
    bind_shaders2(&R_ui_vs, &R_ui_fs);
    return true;
}
void gfx_ui_draw(const GfxUiBatch *b, unsigned n, GfxTex tex) {
    if (!n || !s_ui_ok) return;
    if (n > GFX_UI_BATCH) n = GFX_UI_BATCH;
    GfxUiBatch u = *b;
    u.screen[0] = (float)D.swap_w, u.screen[1] = (float)D.swap_h;
    if (!ubo_put(0, &u, sizeof u)) return;
    bind_vao(D.vao_empty);
    bind_shaders2(&R_ui_vs, &R_ui_fs);
    unit_bind(0, th(tex ? tex : R.white, smp_id(0, 0, 1)));
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, (GLsizei)n);
}
void gfx_ui_end(void) {}

/* ---- renderer ---- */
static bool tev_compile_now; /* render_shader_cache: wait for each compile */
static FILE *s_states;       /* shaders.bin: the TEV states seen, for the next boot */
#define CACHE_MAGIC 0x4D4B5456u /* "MKTV" */

static TevProg *tev_new(uint64_t gkey) {
    if (R.nprogs == R.cap_progs) {
        unsigned cap = R.cap_progs ? R.cap_progs * 2 : 256;
        TevProg *p = realloc(R.progs, cap * sizeof *p);
        if (!p) return NULL;
        R.progs = p, R.cap_progs = cap;
    }
    TevProg *t = &R.progs[R.nprogs++];
    memset(t, 0, sizeof *t);
    t->key = gkey;
    return t;
}
/* a TEV program whose link finished: usable, or failed */
static void tev_finish(TevProg *t, unsigned idx) {
    uint64_t pk = pair_key(&R.vs, &t->fs);
    bool ok = prog_link_check(t->prog, pk, "TEV program");
    if (!ok) { glDeleteProgram(t->prog); t->prog = 0; }
    if (t->fs.sh) { glDeleteShader(t->fs.sh); t->fs.sh = 0; }
    t->done = true;
    u64map_put(&R.tev, t->key, ok ? idx + 1 : 0);
}
/* TEV programs the driver finished linking on its threads */
static void collect_jobs(void) {
    if (!R.pending.len) return;
    for (unsigned i = 0; i < R.nprogs; i++) {
        TevProg *t = &R.progs[i];
        if (t->done) continue;
        GLint c = 0;
        glGetProgramiv(t->prog, GL_COMPLETION_STATUS_ARB, &c);
        if (!c) continue;
        u64map_del(&R.pending, t->key);
        tev_finish(t, i);
    }
}

/* Specialised fragment shader for a batch (its program, linked with R.vs), or 0: the uber shader. */
static GLuint tev_program(const TevState *t) {
    uint64_t gkey = tev_shader_key(t), v;
    if (u64map_get(&R.tev, gkey, &v)) return v ? R.progs[v - 1].prog : 0;
    if (u64map_get(&R.pending, gkey, NULL)) return 0;
    /* without the driver's compile threads, a few a frame (the uber shader draws the rest) */
    if (!D.parallel && !tev_compile_now && R.compiles_this_frame >= 2) return 0;
    static char src[65536];
    if (!tev_gen_fs(t, src, sizeof src)) { u64map_put(&R.tev, gkey, 0); return 0; }
    uint64_t t0 = plat_now_ns();
    TevProg *p = tev_new(gkey);
    if (!p) return 0;
    unsigned idx = R.nprogs - 1;
    char *gs = shader_src(true, src);
    if (!gs) { R.nprogs--; return 0; }
    p->fs.key = shader_key(true, gs);
    p->fs.frag = true;
    free(gs);
    GLuint cached = prog_from_cache(pair_key(&R.vs, &p->fs));
    if (cached) {
        p->prog = cached, p->done = true;
        u64map_put(&R.tev, gkey, idx + 1);
        s_compile_ns += plat_now_ns() - t0;
        return cached;
    }
    if (s_states && !tev_compile_now) { fwrite(t, sizeof *t, 1, s_states); fflush(s_states); }
    R.compiles_this_frame++;
    if (!shd_start(&p->fs, true, src)) { p->done = true; u64map_put(&R.tev, gkey, 0); return 0; }
    p->prog = prog_link_start(&R.vs, &p->fs);
    if (D.parallel && !tev_compile_now) {
        u64map_put(&R.pending, gkey, idx + 1);
        s_compile_ns += plat_now_ns() - t0;
        return 0;
    }
    tev_finish(p, idx);
    s_compile_ns += plat_now_ns() - t0;
    return p->prog;
}

bool render_init(void) {
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
    if (!ok || !pair_prog(&R.vs, &R.fs_uber)) return false;
    rt_log("render: fixed shaders ready in %.2f s", (double)(plat_now_ns() - t0) / 1e9);
    static const uint8_t white[4] = {255, 255, 255, 255};
    R.white = gfx_tex_upload(0, GFX_RGBA8, 1, 1, white);
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
    rt_log("render: %u cached TEV states, %u shaders ready in %.1f s", total, R.nprogs - before, (double)(plat_now_ns() - t0) / 1e9);
}

static struct { bool on, clear; uint32_t src[2], size[2]; float gamma; unsigned seen, direct; } s_direct;

static GLuint fbo_cz(GLuint *fbo, uint32_t color, uint32_t depth) {
    if (*fbo) glDeleteFramebuffers(1, fbo);
    glCreateFramebuffers(1, fbo);
    glNamedFramebufferTexture(*fbo, GL_COLOR_ATTACHMENT0, s_img[color].tex, 0);
    glNamedFramebufferTexture(*fbo, s_img[depth].fmt == IF_Z24 ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT, s_img[depth].tex, 0);
    return *fbo;
}

static void ensure_efb(uint32_t w, uint32_t h) {
    if (R.efb_w == w && R.efb_h == h && R.efb_z24 == D.z24) return;
    img_free(&R.efb_color);
    img_free(&R.efb_depth);
    R.efb_color = img_create(w, h, IF_RGBA8, 1, true);
    R.efb_depth = img_create(w, h, D.z24 ? IF_Z24 : IF_Z32, 1, true);
    R.efb_w = w, R.efb_h = h, R.efb_z24 = D.z24;
    s_direct.on = s_direct.clear = false;
    if (!R.efb_color || !R.efb_depth) return;
    bind_fbo(fbo_cz(&R.efb_fbo, R.efb_color, R.efb_depth));
    glScissor(0, 0, (GLsizei)w, (GLsizei)h);
    clear_color(15, 0, 0, 0, 1);
    clear_depth(1.0f);
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
    bind_fbo(R.efb_fbo);
    set_viewport(0, 0, (float)R.efb_w, (float)R.efb_h);
    R.cur_target = 1;
    R.cur_sc.width = 0;
}

static void set_scissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (R.cur_sc.x == x && R.cur_sc.y == y && R.cur_sc.width == w && R.cur_sc.height == h) return;
    R.cur_sc.x = x, R.cur_sc.y = y, R.cur_sc.width = w, R.cur_sc.height = h;
    glScissor((GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h);
}

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

static void img_upload(uint32_t id, uint32_t level, uint32_t y, uint32_t w, uint32_t h, const void *data, uint32_t size) {
    Img *im = &s_img[id];
    if (im->fmt == IF_BC1 || im->fmt == IF_BC7)
        glCompressedTextureSubImage2D(im->tex, (GLint)level, 0, (GLint)y, (GLsizei)w, (GLsizei)h, gl_ifmt(im->fmt), (GLsizei)size, data);
    else
        glTextureSubImage2D(im->tex, (GLint)level, 0, (GLint)y, (GLsizei)w, (GLsizei)h,
                            im->fmt == IF_R8 ? GL_RED : im->fmt == IF_RG8 ? GL_RG : GL_RGBA, GL_UNSIGNED_BYTE, data);
}

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
    if (i < levels) glTextureParameteri(s_img[id].tex, GL_TEXTURE_MAX_LEVEL, (GLint)i - 1);
    return i;
}

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
    GlTexture *gt = NULL;
    if (u64map_get(&R.textures, key, &idx)) gt = &R.tex[idx];
    else {
        GxTexture *t = gx_texture_acquire(key);
        if (!t) { R.white_tex++; unit_bind(unit, th(R.white, smp_id(0, 0, 0))); return; }
        uint64_t t0 = plat_now_ns();
        uint8_t *buf = decode_buf((size_t)t->width * t->height * 4);
        unsigned chans = 0;
        if (buf) {
            chans = D.bc1 && gx_texture_cmpr_bc1(t, buf) ? CHANS_BC1 : gx_texture_decode_compact(t, buf);
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
            GlTexture *p = realloc(R.tex, cap * sizeof *p);
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
    memset(R.cur_tex, 0xFF, sizeof R.cur_tex); /* (an id may be reused at once) */
}

#define HD_UPLOAD_BYTES (8u << 20)
static struct { uint8_t *data; uint64_t tag; uint32_t id; unsigned w, h, levels, level, row; size_t off; } s_up;
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
            GlTexture *gt = u64map_get(&R.textures, s_up.tag, &ti) ? &R.tex[ti] : NULL;
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
        GlTexture *gt = u64map_get(&R.textures, s_up.tag, &ti) ? &R.tex[ti] : NULL;
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
        memset(R.cur_tex, 0xFF, sizeof R.cur_tex);
    }
}

/* GX culling (gen mode 1 = back, 2 = front); front faces wind clockwise (glFrontFace in gfx_open) */
static void apply_cull(uint32_t mode) {
    if (mode == R.cur_cull) return;
    R.cur_cull = mode;
    if (mode == 1 || mode == 2) { glEnable(GL_CULL_FACE); glCullFace(mode == 1 ? GL_BACK : GL_FRONT); }
    else glDisable(GL_CULL_FACE);
}

static bool blend_uses_src_alpha(uint32_t blend) {
    uint32_t sf = (blend >> 8) & 7, df = (blend >> 5) & 7;
    return (blend & 1) && !((blend >> 11) & 1) && (sf == 4 || sf == 5 || df == 4 || df == 5);
}
static uint32_t const_alpha_word(uint32_t blend, uint32_t pix) {
    bool ca = ((blend >> 4) & 1) && ((pix >> 16) & 1);
    return ca && !blend_uses_src_alpha(blend) ? 0x100u | ((pix >> 8) & 0xFFu) : 0u;
}
static GLenum compare(uint32_t f) {
    static const GLenum m[8] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
    return m[f & 7];
}
static GLenum blend_factor(uint32_t f, bool src_side) {
    switch (f & 7) {
    case 0: return GL_ZERO;
    case 1: return GL_ONE;
    case 2: return src_side ? GL_DST_COLOR : GL_SRC_COLOR;
    case 3: return src_side ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
    case 4: return GL_SRC_ALPHA;
    case 5: return GL_ONE_MINUS_SRC_ALPHA;
    case 6: return GL_DST_ALPHA;
    default: return GL_ONE_MINUS_DST_ALPHA;
    }
}

/* GX Z mode (BP 0x40), blend mode (BP 0x41) and constant alpha: as render_dk.c apply_pipe */
static void apply_pipe(uint32_t zmode, uint32_t blend, uint32_t pix) {
    if (R.pipe_valid && R.cur_z == zmode && R.cur_blend == blend && R.cur_pix == pix) return;
    R.pipe_valid = true;
    R.cur_z = zmode, R.cur_blend = blend, R.cur_pix = pix;
    if (zmode & 1) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthMask((zmode & 1) && ((zmode >> 4) & 1));
    glDepthFunc(compare((zmode >> 1) & 7));
    bool color = (blend >> 3) & 1, alpha = (blend >> 4) & 1;
    bool enabled = blend & 1, logic = (blend >> 1) & 1, subtract = (blend >> 11) & 1;
    uint32_t logic_op = (blend >> 12) & 15;
    bool ca = alpha && ((pix >> 16) & 1);
    bool over = ca && blend_uses_src_alpha(blend);
    GLenum sa = GL_ONE, da = over ? GL_ONE_MINUS_SRC_ALPHA : GL_ZERO;
    bool on = true;
    GLenum f[4] = {GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO}, op[2] = {GL_FUNC_ADD, GL_FUNC_ADD};
#define BLEND(sf, df, o) do { f[0] = (sf), f[1] = (df), f[2] = ca ? sa : (sf), f[3] = ca ? da : (df); \
                              op[0] = (o), op[1] = ca ? GL_FUNC_ADD : (o); } while (0)
    if (subtract) BLEND(GL_ONE, GL_ONE, GL_FUNC_REVERSE_SUBTRACT);
    else if (enabled) BLEND(blend_factor((blend >> 8) & 7, true), blend_factor((blend >> 5) & 7, false), GL_FUNC_ADD);
    else if (logic && logic_op == 0) BLEND(GL_ZERO, GL_ZERO, GL_FUNC_ADD);
    else if (logic && logic_op == 5) { on = false; color = alpha = false; }
    else if (logic && logic_op == 15) BLEND(GL_ONE, GL_ONE, GL_MAX);
    else if (ca) BLEND(GL_ONE, GL_ZERO, GL_FUNC_ADD);
    else on = false;
#undef BLEND
    if (on) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(f[0], f[1], f[2], f[3]);
        glBlendEquationSeparate(op[0], op[1]);
    } else glDisable(GL_BLEND);
    glColorMask(color, color, color, alpha);
}

/* vertex attributes per layout (runtime/gx.h) */
static uint32_t s_layout = ~0u;
static void bind_layout(uint32_t layout) {
    bind_vao(D.vao);
    if (s_layout == layout) return;
    s_layout = layout;
    GLuint a = D.vao;
#define ATTR(i, off, n, ty, norm) (glEnableVertexArrayAttrib(a, i), glVertexArrayAttribFormat(a, i, n, ty, norm, (GLuint)(off)), glVertexArrayAttribBinding(a, i, 0))
#define FIXED(i) (glDisableVertexArrayAttrib(a, i), glVertexAttrib4f(i, 0, 0, 0, 0))
    if (layout == GX_LAYOUT_FULL) {
        ATTR(0, offsetof(GpuVertex, pos), 4, GL_FLOAT, GL_FALSE);
        ATTR(1, offsetof(GpuVertex, color0), 4, GL_UNSIGNED_BYTE, GL_TRUE);
        ATTR(2, offsetof(GpuVertex, color1), 4, GL_UNSIGNED_BYTE, GL_TRUE);
        for (int p = 0; p < 4; p++) ATTR(3 + p, offsetof(GpuVertex, uv[2 * p]), 4, GL_FLOAT, GL_FALSE);
        ATTR(7, offsetof(GpuVertex, nrm), 3, GL_FLOAT, GL_FALSE);
        glEnableVertexArrayAttrib(a, 8), glVertexArrayAttribIFormat(a, 8, 2, GL_UNSIGNED_INT, offsetof(GpuVertex, rec)), glVertexArrayAttribBinding(a, 8, 0);
    } else {
        ATTR(0, 0, 3, GL_FLOAT, GL_FALSE);
        ATTR(1, 20, 4, GL_UNSIGNED_BYTE, GL_TRUE);
        ATTR(2, 24, 4, GL_UNSIGNED_BYTE, GL_TRUE);
        for (uint32_t p = 0; p < 4; p++) {
            int comps = 2 * (int)layout - 4 * (int)p;
            if (comps <= 0) FIXED(3 + p);
            else ATTR(3 + p, 36 + 16 * p, comps > 4 ? 4 : comps, GL_FLOAT, GL_FALSE);
        }
        ATTR(7, 28, 3, GL_SHORT, GL_FALSE);
        glEnableVertexArrayAttrib(a, 8), glVertexArrayAttribIFormat(a, 8, 2, GL_UNSIGNED_INT, 12), glVertexArrayAttribBinding(a, 8, 0);
    }
    if (!R.xf_gpu) { glDisableVertexArrayAttrib(a, 7); glDisableVertexArrayAttrib(a, 8); }
#undef ATTR
#undef FIXED
    glVertexArrayVertexBuffer(a, 0, R.vb.buf, R.vb.off, (GLsizei)gx_layout_stride(layout));
}
static void draw_indexed(uint32_t count, uint32_t first) {
    glDrawElements(GL_TRIANGLES, (GLsizei)count, GL_UNSIGNED_INT, (const void *)(uintptr_t)(R.ib.off + (size_t)first * 4));
}
static void draw_full(void) {
    bind_vao(D.vao_empty);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

typedef struct {
    uint32_t at;
    bool xfb, clear;
    uint32_t src[4];
    uint32_t dst_w, dst_h, addr;
    uint32_t regs[8];
    uint32_t mode[4];
    float vfilter[4];
    float half_scale;
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

/* a full-target pass of FULL_VS + fs into image dst (w x h) */
static void pass_target(uint32_t dst, uint32_t w, uint32_t h) {
    bind_fbo(img_fbo(dst));
    set_viewport(0, 0, (float)w, (float)h);
    glScissor(0, 0, (GLsizei)w, (GLsizei)h);
    R.cur_sc.width = 0;
    R.cur_target = 2;
}

static void movie_enhance(GlTexture *y, GlTexture *u, GlTexture *v) {
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
    pass_state(false);
    unit_bind(1, th(R.white, smp_id(0, 0, 0)));
    unit_bind(0, th(y->id, smp_id(0, 0, 0)));
    bind_shaders2(&R.vs_full, &R.fs_deblock);
    pass_target(M.yd, w, h);
    draw_full();
    unit_bind(0, th(M.yd, smp_id(0, 0, 1)));
    bind_shaders2(&R.vs_full, &R.fs_luma);
    pass_target(M.set[s].y, W2, H2);
    draw_full();
    unit_bind(1, th(M.yd, smp_id(0, 0, 1)));
    unit_bind(0, th(u->id, smp_id(0, 0, 0)));
    bind_shaders2(&R.vs_full, &R.fs_chroma);
    pass_target(M.set[s].u, w, h);
    draw_full();
    unit_bind(0, th(v->id, smp_id(0, 0, 0)));
    pass_target(M.set[s].v, w, h);
    draw_full();
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
        GlTexture *pl[4] = {0};
        for (unsigned m = 0; m < 8; m++) {
            uint64_t idx;
            if (!k[m] || (k[m] & 0xFFFF000000000000ull) == RT_TAG) continue;
            if (!u64map_get(&R.textures, k[m], &idx)) {
                bind_slot(m, k[m]);
                if (!u64map_get(&R.textures, k[m], &idx)) continue;
            }
            GlTexture *gt = &R.tex[idx];
            if (gt->movie) pl[gt->movie] = gt;
        }
        if (!pl[1] || !pl[2] || !pl[3] || pl[2]->w * 2 != pl[1]->w || pl[3]->w != pl[2]->w) continue;
        if (pl[1]->enh_gen && M.set[pl[1]->enh_set].gen == pl[1]->enh_gen) continue;
        movie_enhance(pl[1], pl[2], pl[3]);
    }
}

/* image to image, scaled (the scissor would clip a blit: off meanwhile) */
static void blit_fbo(GLuint sf, uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh, GLuint df, uint32_t dx, uint32_t dy, uint32_t dw, uint32_t dh, GLenum filter) {
    glDisable(GL_SCISSOR_TEST);
    glBlitNamedFramebuffer(sf, df, (GLint)sx, (GLint)sy, (GLint)(sx + sw), (GLint)(sy + sh), (GLint)dx, (GLint)dy, (GLint)(dx + dw),
                           (GLint)(dy + dh), GL_COLOR_BUFFER_BIT, filter);
    glEnable(GL_SCISSOR_TEST);
}
static void blit(uint32_t src, uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh, uint32_t dst, uint32_t dx, uint32_t dy, uint32_t dw, uint32_t dh) {
    blit_fbo(img_fbo(src), sx, sy, sw, sh, img_fbo(dst), dx, dy, dw, dh, GL_LINEAR);
}

/* the EFB's colour alone as a read framebuffer (its own FBO has the depth too) */
static GLuint efb_read_fbo(void) { return img_fbo(R.efb_color); }

/* An XFB copy that is a plain 1:1 copy of EFB rows: a blit instead of COPY_FS (render_dk.c xfb_blit) */
static bool xfb_blit(const CopyOp *c, float scale, const Target *dst) {
    if (!D.blit_xfb || !c->xfb || c->mode[1] || c->mode[2] || c->mode[3]) return false;
    int tap = -1;
    for (int i = 0; i < 3; i++) {
        if (c->vfilter[i] == 0.0f) continue;
        if (tap >= 0 || fabsf(c->vfilter[i] - 1.0f) > 0.002f) return false;
        tap = i;
    }
    if (tap < 0 || dst->w != c->dst_w || dst->h != c->dst_h) return false;
    int row = (int)rintf(scale), dy = (tap - 1) * row;
    int x0 = (int)efb_px(c->src[0], scale), y0 = (int)efb_px(c->src[1], scale), w = (int)c->dst_w, h = (int)c->dst_h;
    uint32_t flags = (uint32_t)c->vfilter[3];
    int lo = flags & 1 ? y0 : 0, hi = flags & 2 ? (int)efb_px(c->src[1] + c->src[3], scale) - 1 : (int)R.efb_h - 1;
    if (hi > (int)R.efb_h - 1) hi = (int)R.efb_h - 1;
    if (x0 + w > (int)R.efb_w || lo > hi) return false;
    int ra = lo - y0 - dy > 0 ? lo - y0 - dy : 0, rb = hi - y0 - dy + 1 < h ? hi - y0 - dy + 1 : h;
    if (rb <= ra) return false;
    GLuint sf = efb_read_fbo(), df = img_fbo(dst->id);
    blit_fbo(sf, (uint32_t)x0, (uint32_t)(y0 + ra + dy), (uint32_t)w, (uint32_t)(rb - ra), df, 0, (uint32_t)ra, (uint32_t)w, (uint32_t)(rb - ra), GL_NEAREST);
    if (ra > 0) blit_fbo(sf, (uint32_t)x0, (uint32_t)lo, (uint32_t)w, 1, df, 0, 0, (uint32_t)w, (uint32_t)ra, GL_NEAREST);
    if (rb < h) blit_fbo(sf, (uint32_t)x0, (uint32_t)hi, (uint32_t)w, 1, df, 0, (uint32_t)rb, (uint32_t)w, (uint32_t)(h - rb), GL_NEAREST);
    static bool logged;
    if (!logged) { logged = true; rt_log("render: XFB copies go through a blit (%dx%d, filter row %d)", w, h, tap - 1); }
    return true;
}

static bool xfb_direct(const CopyOp *c, size_t batches) {
    return D.xfb_direct && c->xfb && c->at >= batches && !c->mode[2] && !(c->mode[3] & 1);
}

static void efb_clear(const CopyOp *c, float scale) {
    uint32_t x = efb_px(c->src[0], scale), y = efb_px(c->src[1], scale);
    uint32_t x1 = efb_px(c->src[0] + c->src[2], scale), y1 = efb_px(c->src[1] + c->src[3], scale);
    if (x1 > R.efb_w) x1 = R.efb_w;
    if (y1 > R.efb_h) y1 = R.efb_h;
    if (x1 <= x || y1 <= y) return;
    uint32_t z = c->regs[4], bl = c->regs[5], ar = c->regs[1], gb = c->regs[2], cz = c->regs[3];
    bind_efb();
    set_scissor(x, y, x1 - x, y1 - y);
    bool cu = (bl >> 3) & 1, au = (bl >> 4) & 1, zu = (z >> 4) & 1;
    unsigned mask = (cu ? 7u : 0) | (au ? 8u : 0);
    if (mask)
        clear_color(mask, (float)(ar & 0xFF) / 255.0f, (float)((gb >> 8) & 0xFF) / 255.0f, (float)(gb & 0xFF) / 255.0f,
                    (float)((ar >> 8) & 0xFF) / 255.0f);
    if (zu) clear_depth((float)(cz & 0xFFFFFF) / 16777215.0f);
}

static void seam_line(const Frame *f, float scale) {
    uint32_t cx = (uint32_t)((float)f->efb[0] * scale * 0.5f + 0.5f), h = (uint32_t)((float)f->efb[1] * scale + 0.5f);
    uint32_t line = scale >= 2.0f ? 2 : 1;
    if (!h || cx < line / 2 || cx + line > R.efb_w || h > R.efb_h) return;
    bind_efb();
    set_scissor(cx - line / 2, 0, line, h);
    clear_color(15, 0.8f, 0.8f, 0.8f, 1.0f);
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

static void vs_params(float squeeze, float shift) {
    if (squeeze == R.squeeze && shift == R.shift_cur) return;
    R.squeeze = squeeze, R.shift_cur = shift;
    float v[8] = {squeeze, shift};
    ubo_put(0, v, sizeof v);
}

/* the batch's program: specialised or the uber shader */
static bool use_tev(const TevState *t) {
    GLuint p = tev_program(t);
    if (p) { use_prog(p); return false; }
    bind_shaders2(&R.vs, &R.fs_uber);
    return true;
}

static void mirror_draw(const Frame *f, size_t first, size_t last, float scale, float sq, GAddr ub, const uint32_t *ub_off) {
    const float X0 = 3, X1 = sq > 0 ? 190 : 97, Y0 = 6, Y1 = sq > 0 ? 96 : 50;
    float cx = (float)f->efb[0] * scale * 0.5f, ux = (float)f->efb[0] * scale / 608, uy = (float)f->efb[1] * scale / 448;
    uint32_t bx0 = (uint32_t)(cx + (X0 * ux - cx) * (1 - sq) - sq * cx + 0.5f), bx1 = (uint32_t)(cx + (X1 * ux - cx) * (1 - sq) - sq * cx + 0.5f);
    uint32_t by0 = (uint32_t)(Y0 * uy + 0.5f), by1 = (uint32_t)(Y1 * uy + 0.5f);
    if (bx1 > R.efb_w || by1 > R.efb_h || bx1 <= bx0 + 8 || by1 <= by0 + 8) return;
    uint32_t w = bx1 - bx0, h = by1 - by0;
    static bool shader_ok, shader_bad;
    if (!shader_ok && !shader_bad) {
        shader_ok = shader_get(&R.fs_mirror, true, MIRROR_FS, "mirror");
        shader_bad = !shader_ok;
    }
    if (!shader_ok) return;
    if (R.mirror.w != w || R.mirror.h != h || R.mirror_z24 != D.z24) {
        target_init(&R.mirror, w, h);
        img_free(&R.mirror_depth);
        R.mirror_depth = img_create(w, h, D.z24 ? IF_Z24 : IF_Z32, 1, true);
        R.mirror_z24 = D.z24;
        if (R.mirror.id && R.mirror_depth) fbo_cz(&R.mirror_fbo, R.mirror.id, R.mirror_depth);
    }
    if (!R.mirror.id || !R.mirror_depth) return;
    bind_fbo(R.mirror_fbo);
    R.cur_target = 3;
    set_viewport(0, 0, (float)w, (float)h);
    R.cur_sc.width = 0;
    set_scissor(0, 0, w, h);
    clear_color(15, 0, 0, 0, 1);
    clear_depth(1.0f);
    float v[8] = {0, 0, g_mirror_tilt[0], g_mirror_tilt[1], 1, g_mirror_eye[0], g_mirror_eye[1], ((float)w / (float)R.efb_w) / ((float)h / (float)R.efb_h)};
    if (ubo_put(0, v, sizeof v)) {
        uint32_t cur_ub = ~0u;
        for (size_t i = first; i < last; i++) {
            const Batch *b = &f->batches.data[i];
            if (!b->index_count || b->hud) continue;
            apply_pipe(b->tev.pipe[0], b->tev.pipe[1], b->tev.pipe[3]);
            apply_cull(b->tev.pipe[2]);
            use_tev(&b->tev);
            if (ub_off[i] != cur_ub) {
                cur_ub = ub_off[i];
                glBindBufferRange(GL_UNIFORM_BUFFER, FS_UBO, ub.buf, ub.off + cur_ub, TEV_UNIFORM_SIZE);
            }
            R.below_tex = 0;
            R.shift = 0;
            for (unsigned m = 0; m < 8; m++) bind_slot(m, b->tev.textures[m]);
            bind_layout(b->layout);
            draw_indexed(b->index_count, b->index_first);
        }
    }
    R.below_tex = 0;
    R.shift = 0;
    /* the panel, over the EFB */
    bind_efb();
    pass_state(true);
    bind_shaders2(&R.vs_full, &R.fs_mirror);
    struct { float rect[4], edge[4]; } u = {{(float)bx0, (float)by0, (float)w, (float)h}, {10 * uy, 1.5f * uy, g_look_back & 1 ? 1.0f : 0.0f, 0}};
    if (ubo_put(FS_UBO, &u, sizeof u)) {
        unit_bind(0, th(R.mirror.id, smp_id(0, 0, 1)));
        set_scissor(bx0, by0, w, h);
        draw_full();
    }
    R.squeeze = -1;
    vs_params(sq, 0.0f);
}

static size_t mirror_last(const Frame *f, const CopyOp *copies, unsigned ncopies, size_t i) {
    for (unsigned q = 0; q < ncopies; q++)
        if (!copies[q].xfb && copies[q].src[2] * 2 >= (uint32_t)f->efb[0]) return copies[q].at < i ? copies[q].at : i;
    return i;
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
    } else if (R.tick % 30 == 0 && R.ntex > 2048) evict_textures(120);
    static float cap = 8.0f;
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
    GAddr vb, ib, xb, ub;
    uint8_t *vm = ring_alloc((uint32_t)f->vb.len + 16, 256, &vb);
    uint8_t *im = ring_alloc((uint32_t)(f->indices.len * 4) + 4, 256, &ib);
    uint32_t xn = f->xf.len ? (uint32_t)f->xf.len * 4 : 16;
    uint8_t *xm = ring_alloc(xn, (uint32_t)D.ssbo_align, &xb);
    uint32_t ustride = (uint32_t)ALIGN(TEV_UNIFORM_SIZE, D.ubo_align);
    uint8_t *um = ring_alloc((uint32_t)f->batches.len * ustride + ustride, (uint32_t)D.ubo_align, &ub);
    if (!vm || !im || !xm || !um) { gx_frame_release(f); return false; }
    memcpy(vm, f->vb.data, f->vb.len);
    memset(vm + f->vb.len, 0, 16);
    memcpy(im, f->indices.data, f->indices.len * 4);
    if (f->xf.len) memcpy(xm, f->xf.data, (size_t)f->xf.len * 4); else memset(xm, 0, 16);
    static uint32_t *ub_off;
    static size_t ub_cap;
    if (f->batches.len > ub_cap) {
        size_t cap2 = f->batches.len + f->batches.len / 2;
        uint32_t *p = realloc(ub_off, cap2 * sizeof *p);
        if (!p) { gx_frame_release(f); return false; }
        ub_off = p, ub_cap = cap2;
    }
    {
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
    if (s_direct.on && !direct) {
        /* this frame ends differently: last frame's picture moves to the XFB before the EFB changes */
        if (!R.have_xfb || R.xfb.w != s_direct.size[0] || R.xfb.h != s_direct.size[1]) {
            target_init(&R.xfb, s_direct.size[0], s_direct.size[1]);
            R.have_xfb = R.xfb.id != 0;
        }
        if (R.have_xfb)
            blit_fbo(efb_read_fbo(), s_direct.src[0], s_direct.src[1], s_direct.size[0], s_direct.size[1], img_fbo(R.xfb.id), 0, 0,
                     s_direct.size[0], s_direct.size[1], GL_NEAREST);
        s_direct.on = false;
    }
    s_direct.seen++;
    s_direct.direct += direct;
    if (s_direct.seen == 600) {
        rt_log("render: %u of the last 600 frames shown directly from the EFB", s_direct.direct);
        s_direct.seen = s_direct.direct = 0;
    }
    R.vb = vb, R.ib = ib;
    s_layout = ~0u;
    glVertexArrayElementBuffer(D.vao, ib.buf);
    movie_prepass(f);
    if (s_direct.clear) { s_direct.clear = false; efb_clear(&direct_clear, scale); }
    if (R.xf_gpu) glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, xb.buf, xb.off, xn);
    size_t next = 0;
    R.stat_batches = (uint32_t)f->batches.len;
    R.stat_verts = f->nverts;
    for (unsigned k = 0; k <= ncopies; k++) {
        size_t end = k < ncopies ? copies[k].at : f->batches.len;
        if (end > f->batches.len) end = f->batches.len;
        if (next < end) {
            bind_efb();
            float sq = R.hud43 && k < ncopies && copies[k].xfb ? 0.25f : 0.0f;
            R.squeeze = -1; /* (the copy passes bound other blocks) */
            vs_params(sq, 0.0f);
            uint32_t cur_ub = ~0u;
            bool seam = f->vseam && k < ncopies && copies[k].xfb;
            bool mirror = f->mirror && k < ncopies && copies[k].xfb;
            for (size_t i = next; i < end; i++) {
                const Batch *b = &f->batches.data[i];
                if (seam && b->hud) { seam = false; seam_line(f, scale); bind_efb(); }
                if (mirror && b->hud) {
                    mirror = false;
                    mirror_draw(f, 0, mirror_last(f, copies, ncopies, i), scale, sq, ub, ub_off);
                    cur_ub = ~0u;
                }
                uint32_t x0 = efb_px(b->rect[0], scale), y0 = efb_px(b->rect[1], scale), x1 = efb_px(b->rect[2], scale), y1 = efb_px(b->rect[3], scale);
                if (sq > 0 && b->hud) {
                    float cx = (float)f->efb[0] * scale * 0.5f;
                    float shx = b->hud == 2 ? -sq * cx : b->hud == 3 ? sq * cx : 0.0f;
                    x0 = (uint32_t)(cx + ((float)x0 - cx) * (1.0f - sq) + shx + 0.5f);
                    x1 = (uint32_t)(cx + ((float)x1 - cx) * (1.0f - sq) + shx + 0.5f);
                }
                if (x1 > R.efb_w) x1 = R.efb_w;
                if (y1 > R.efb_h) y1 = R.efb_h;
                if (!b->index_count || x1 <= x0 || y1 <= y0) continue;
                apply_pipe(b->tev.pipe[0], b->tev.pipe[1], b->tev.pipe[3]);
                apply_cull(b->tev.pipe[2]);
                use_tev(&b->tev);
                if (ub_off[i] != cur_ub) {
                    cur_ub = ub_off[i];
                    glBindBufferRange(GL_UNIFORM_BUFFER, FS_UBO, ub.buf, ub.off + cur_ub, TEV_UNIFORM_SIZE);
                }
                R.below_tex = 0;
                R.shift = 0;
                for (unsigned m = 0; m < 8; m++) bind_slot(m, b->tev.textures[m]);
                bind_layout(b->layout);
                bool shifted = R.shift != 0;
                if (shifted) { set_scissor(0, 0, R.efb_w, R.efb_h); vs_params(sq, R.shift / 480.0f); }
                else set_scissor(x0, y0, x1 - x0, y1 - y0);
                draw_indexed(b->index_count, b->index_first);
                if (shifted) vs_params(sq, 0.0f);
                if (R.below_tex) {
                    unit_bind(R.below_unit, th(R.below_tex, smp_id(0, 0, 1)));
                    set_scissor(0, 0, R.efb_w, R.efb_h);
                    vs_params(sq, 32.0f / 480.0f);
                    draw_indexed(b->index_count, b->index_first);
                    vs_params(sq, 0.0f);
                }
            }
            if (mirror) mirror_draw(f, 0, mirror_last(f, copies, ncopies, end), scale, sq, ub, ub_off);
            next = end;
        }
        if (k == ncopies) break;
        CopyOp *c = &copies[k];
        if (direct && k + 1 == ncopies) {
            uint32_t gidx = (c->mode[3] >> 16) & 3;
            s_direct.src[0] = efb_px(c->src[0], scale), s_direct.src[1] = efb_px(c->src[1], scale);
            s_direct.size[0] = c->dst_w, s_direct.size[1] = c->dst_h;
            if (s_direct.src[0] + s_direct.size[0] > R.efb_w) s_direct.size[0] = R.efb_w - s_direct.src[0];
            if (s_direct.src[1] + s_direct.size[1] > R.efb_h) s_direct.size[1] = R.efb_h - s_direct.src[1];
            s_direct.gamma = gidx == 1 ? 1.0f / 1.7f : gidx >= 2 ? 1.0f / 2.2f : 1.0f;
            s_direct.on = true;
            if (c->clear) { direct_clear = *c; s_direct.clear = true; }
            if (R.have_xfb) { img_free(&R.xfb.id); R.have_xfb = false; R.xfb.w = R.xfb.h = 0; }
            continue;
        }
        Target *dst = c->xfb ? (R.have_xfb ? &R.xfb : NULL) : rt_for(c->addr, c->dst_w, c->dst_h);
        if (dst && dst->id && xfb_blit(c, scale, dst)) {}
        else if (dst && dst->id && c->half_scale) {
            pass_state(false);
            bind_shaders2(&R.vs_full, c->mode[0] == 7 ? &R.fs_copy_alpha : &R.fs_copy_half);
            float step = 2.0f * scale / c->half_scale;
            struct { float xf[4]; float opaque, pad[3]; } u = {
                {(float)efb_px(c->src[0], scale) / (float)R.efb_w, (float)efb_px(c->src[1], scale) / (float)R.efb_h,
                 step / (float)R.efb_w, step / (float)R.efb_h},
                c->mode[0] == 4 || ((c->mode[3] >> 8) & 7) != 1 ? 1.0f : 0.0f, {0, 0, 0}};
            if (ubo_put(FS_UBO, &u, sizeof u)) {
                unit_bind(0, th(R.efb_color, smp_id(0, 0, 1)));
                pass_target(dst->id, dst->w, dst->h);
                draw_full();
            }
        } else if (dst && dst->id) {
            pass_state(false);
            bind_shaders2(&R.vs_full, &R.fs_copy);
            struct { uint32_t src[4], mode[4]; float vfilter[4]; float scale, pad[3]; } u;
            memcpy(u.src, c->src, sizeof u.src);
            memcpy(u.mode, c->mode, sizeof u.mode);
            memcpy(u.vfilter, c->vfilter, sizeof u.vfilter);
            u.scale = scale;
            u.pad[0] = u.pad[1] = u.pad[2] = 0;
            if (ubo_put(FS_UBO, &u, sizeof u)) {
                unit_bind(0, th(R.efb_color, smp_id(0, 0, 0)));
                unit_bind(1, th(R.efb_depth, smp_id(0, 0, 0)));
                unit_bind(2, th(R.efb_color, smp_id(0, 0, 1)));
                pass_target(dst->id, dst->w, dst->h);
                draw_full();
            }
        }
        if (c->clear) efb_clear(c, scale);
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
void render_set_gpu_test(bool on) { (void)on; }
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

/* GPU timer: timestamps at begin, mid and end, about once a second, read when the slot comes round */
static struct { unsigned frames, n; double sum, max, part[2]; } s_gt;
void render_gpu_timer_begin(void) {
    frame_begin();
    Slot *s = &D.slots[D.slot];
    if (s->timed) {
        GLuint64 a = 0, b = 0, c = 0;
        glGetQueryObjectui64v(s->query[0], GL_QUERY_RESULT, &a);
        glGetQueryObjectui64v(s->query[1], GL_QUERY_RESULT, &b);
        glGetQueryObjectui64v(s->query[2], GL_QUERY_RESULT, &c);
        if (b >= a && c >= b) {
            double x = (double)(b - a) / 1e6, y = (double)(c - b) / 1e6;
            s_gt.sum += x + y, s_gt.part[0] += x, s_gt.part[1] += y;
            if (x + y > s_gt.max) s_gt.max = x + y;
            s_gt.n++;
        }
        s->timed = false;
    }
    if (++s_gt.frames < 60u) return;
    s_gt.frames = 0;
    s->timed = true;
    glQueryCounter(s->query[0], GL_TIMESTAMP);
}
void render_gpu_timer_mid(void) { if (D.recording && D.slots[D.slot].timed) glQueryCounter(D.slots[D.slot].query[1], GL_TIMESTAMP); }
void render_gpu_timer_end(void) { if (D.recording && D.slots[D.slot].timed) glQueryCounter(D.slots[D.slot].query[2], GL_TIMESTAMP); }
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
    if (have && R.backdrop && vx > 0) {
        uint32_t src = pic, sx = px, sy = py, sw = pw, sh = ph;
        for (int i = 0; i < 4 && sw > 100; i++) {
            uint32_t bw = sw / 2 ? sw / 2 : 1, bh = sh / 2 ? sh / 2 : 1;
            if (R.bd[i].w != bw || R.bd[i].h != bh) target_init(&R.bd[i], bw, bh);
            if (!R.bd[i].id) break;
            blit(src, sx, sy, sw, sh, R.bd[i].id, 0, 0, bw, bh);
            src = R.bd[i].id, sx = sy = 0, sw = bw, sh = bh;
        }
        bind_screen();
        pass_state(false);
        bind_shaders2(&R.vs_full, &R.fs_blit);
        float u[8] = {gamma, adj[1], adj[2], 0.55f, 0, 0, 1, 1};
        if (ubo_put(FS_UBO, u, sizeof u)) {
            unit_bind(0, th(src, smp_id(0, 0, 1)));
            int fh = w * 3 / 4;
            set_viewport(0, (float)((h - fh) / 2), (float)w, (float)fh);
            set_scissor(0, 0, (uint32_t)vx, (uint32_t)h);
            draw_full();
            set_scissor((uint32_t)(vx + (int)vw), 0, (uint32_t)(w - vx - (int)vw), (uint32_t)h);
            draw_full();
        }
    } else bind_screen();
    if (!have) { set_scissor(0, 0, (uint32_t)w, (uint32_t)h); clear_color(15, 0, 0, 0, 1); return; }
    if (vx > 0 && !R.backdrop) {
        set_scissor(0, 0, (uint32_t)vx, (uint32_t)h);
        clear_color(15, 0, 0, 0, 1);
        set_scissor((uint32_t)(vx + (int)vw), 0, (uint32_t)(w - vx - (int)vw), (uint32_t)h);
        clear_color(15, 0, 0, 0, 1);
    }
    if (vy > 0) {
        set_scissor(0, 0, (uint32_t)w, (uint32_t)vy);
        clear_color(15, 0, 0, 0, 1);
        set_scissor(0, (uint32_t)(vy + (int)vh), (uint32_t)w, (uint32_t)(h - vy - (int)vh));
        clear_color(15, 0, 0, 0, 1);
    }
    pass_state(false);
    bool shrink = !D.out4 && (float)pw > vw && (float)ph > vh;
    bind_shaders2(&R.vs_full, shrink ? &R.fs_blit_shrink : &R.fs_blit);
    float u[12] = {gamma, adj[1], adj[2], 0.0f, (float)px / tw, (float)py / th_, (float)pw / tw, (float)ph / th_,
                   0.25f * (float)pw / (tw * vw), 0.25f * (float)ph / (th_ * vh), 0, 0};
    if (!ubo_put(FS_UBO, u, sizeof u)) return;
    unit_bind(0, th(pic, smp_id(0, 0, 1)));
    set_viewport((float)vx, (float)vy, vw, vh);
    set_scissor((uint32_t)vx, (uint32_t)vy, (uint32_t)vw, (uint32_t)vh);
    draw_full();
    /* the overlay draws next with the whole screen */
    set_scissor(0, 0, (uint32_t)w, (uint32_t)h);
    set_viewport(0, 0, (float)w, (float)h);
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
    *compile_ns = s_compile_ns;
}
void render_shader_stats(unsigned *programs, double *compile_ms) {
    *programs = R.nprogs;
    *compile_ms = (double)s_compile_ns / 1e6;
}
