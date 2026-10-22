/* GX command-stream decoder output, shared between the GPU thread (gx.c) and the renderer.
 * C port of gc-runtime's gx.rs data model.
 *
 * A Frame holds one vertex array and one index array; each Batch is a contiguous range of both
 * (draws only ever append to the last batch), so the renderer uploads a frame with one copy. */
#pragma once
#include "plat.h"

/* One vertex (108 bytes): the CPU-transform layout (GX_LAYOUT_FULL), and the unpacked form of a
 * compact vertex (gx_vertex_unpack) for the XF reference and diagnostics. Two kinds:
 *  - rec == XV_PRE: transformed on the CPU. pos = homogeneous clip coordinates over the whole EFB
 *    (y up, z 0..1 = near..far), color0/1 = lit colour channels, uv = final texture coordinates.
 *  - otherwise (GPU transform): pos = model-space position (w unused), nrm = model normal,
 *    color0/1 = vertex colours, uv[i] = source coordinates of texgen i; rec / mtx are texel offsets
 *    into Frame.xf of the draw's XF record and of its position/normal matrix. The vertex shader
 *    (xf_vs.h, mirrored by gx_xf_eval) does the XF stage: transform, lighting and texgen. */
#define XV_PRE 0xFFFFFFFFu
/* GPU vertices: rec bit 30 marks a 2D (orthographic) draw that a 4:3 HUD in widescreen narrows
 * to the middle of the screen: all but full-width ones (backgrounds, fades, dimming), which
 * stretch to the edges. While a movie plays, textured full-width draws (the movie) narrow too. */
#define XV_HUD 0x40000000u
/* With XV_HUD: the draw belongs to the race HUD (Race2D, any number of players) and sits in the left / right
 * third of the screen, so the widescreen HUD moves it to that screen edge instead of the middle. */
#define XV_HUD_LEFT 0x20000000u
#define XV_HUD_RIGHT 0x10000000u
/* The MODERN race HUD (native.c): groups of the game's HUD pictures, each moved (x, y) -> (nx, ny) +
 * s * ((x, y) - (ox, oy)) in the race HUD's 608 x 448 units and kept at one side of the widescreen
 * HUD (zone: the decoder's marker 4 middle, 5 left, 6 right). The marker's bits 8 and up name the
 * group, so the decoder moves a clipped picture's scissor with it (the item roulette's window). */
enum { MG_NONE, MG_PLACE, MG_ITEMS, MG_LAP, MG_TIME, MG_MAP, MG_ORDER, MG_SPEED,
       /* two players (p2_race.blo, top / bottom screens): each player's pieces, the shared map and order */
       MG2_PLACE1, MG2_ITEMS1, MG2_LAP1, MG2_TIME1, MG2_PLACE2, MG2_ITEMS2, MG2_LAP2, MG2_TIME2, MG2_MAP, MG2_ORDER,
       /* three and four players (p4_race.blo, quarters): each player's pieces (player p: + 3p), the map */
       MG4_PLACE1, MG4_ITEMS1, MG4_LAP1, MG4_MAP = MG4_PLACE1 + 12, MG_COUNT };
typedef struct { float ox, oy, nx, ny, s; uint32_t zone; } HudMove;
extern const HudMove g_hud_move[MG_COUNT];
#define XV_REC_MASK 0x0FFFFFFFu
typedef struct {
    float pos[4];
    float nrm[3];
    uint32_t rec, mtx;
    uint8_t color0[4];
    uint8_t color1[4];
    float uv[8][2];
} GpuVertex;

/* Compact vertex of GPU-transformed draws (layouts 0..8 = number of texgen slots k):
 *   0 float pos[3]   12 uint32 rec, mtx   20 uint8 color0[4], color1[4]
 *   28 int16 nrm[3] (x 16384), pad        36 float uv[k][2] (source coordinates of texgen 0..k-1)
 * 36 + 8k bytes instead of 108: the decoder writes and the renderer uploads half as much or less.
 * Every batch has one layout; its first vertex starts at a multiple of the stride, so indices
 * count in strides of the batch's layout. */
#define GX_LAYOUT_FULL 15u
#define GX_NRM_SCALE 16384.0f
static inline uint32_t gx_layout_stride(uint32_t layout) { return layout == GX_LAYOUT_FULL ? 108u : 36u + 8u * layout; }

enum { BLEND_OPAQUE, BLEND_SORTED };

/* Complete pixel-pipeline state of a batch, evaluated per pixel by the TEV shader. */
typedef struct {
    /* Per stage: [colour env, alpha env, order, ksel], order = map | texcoord << 4 | enable << 7 |
     * ras channel << 8, ksel = konst colour sel | konst alpha sel << 8. */
    uint32_t stages[16][4];
    float regs[4][4];      /* PREV, C0, C1, C2 (signed 11-bit range scaled so 1.0 = 255) */
    float konst[4][4];
    uint32_t params[4];    /* [stages, alpha compare (BP 0xF3), swap tables (4 x 8 bits), 0] */
    uint32_t pipe[4];      /* [Z mode (BP 0x40), blend mode (BP 0x41), cull mode (gen mode bits 14-15),
                            *  EFB pixel format (BP 0x43 bits 0-2) | constant dst alpha (BP 0x42, 9 bits) << 8] */
    float fog[4];          /* [A, C, B magnitude, B shift] */
    float fog_color[4];    /* [r, g, b, fsel | proj << 3] */
    uint64_t textures[8];  /* texture cache keys (0 = none) */
    int blend;
} TevState;

typedef struct {
    uint64_t key;
    TevState tev;
    uint32_t vert_first, vert_count, index_first, index_count;
    uint32_t rect[4];      /* pixel rectangle [x0, y0, x1, y1) in EFB pixels, y down */
    uint32_t hud;          /* 0, or vertices carry XV_HUD: 1 middle, 2 left edge, 3 right edge (a 4:3 HUD also moves the rectangle) */
    uint32_t layout;       /* vertex layout (see gx_layout_stride); vert_first counts in its stride */
} Batch;

typedef struct {
    uint32_t at_batch, bp52, src, size, dst;
    uint32_t regs[8];      /* PE control, clear AR/GB, clear Z, Z mode, blend, copy filter 0x53/0x54 */
} EfbCopy;

typedef struct Frame {
    VEC(Batch) batches;
    VEC(uint8_t) vb;       /* vertex bytes, per batch in its layout */
    uint32_t nverts;
    VEC(uint32_t) indices; /* vertex indices, in units of each batch's stride */
    VEC(EfbCopy) copies;
    VEC(uint32_t) xf;      /* XF records, light blocks and matrices for GPU-transformed vertices (RGBA32UI texels) */
    float clear[4];
    float efb[2];
    uint32_t draws;
    volatile int refs;
} Frame;

/* Texture keys with this tag name an EFB-copy render target: bits 0-31 = RAM address,
 * bits 32-47 = texture mode (wrap / filter bits). */
#define RT_TAG (0xEFB0ull << 48)
/* RAM address from a 24-bit GX address register (texture image, TLUT source, copy destination: the
 * address in 32-byte units). The hardware decodes 25 bits, so higher bits a game leaves set are
 * ignored, as in Dolphin. MKDD's race intro loads the cup icon's palette with stray high bits
 * (they change from run to run); without the mask it read past RAM and the icon vanished. */
static inline uint32_t gx_phys(uint32_t reg) { return (reg << 5) & 0x01FFFFFFu; }
static inline uint64_t rt_key(uint32_t addr, uint32_t mode) { return RT_TAG | ((uint64_t)(mode & 0xFFFF) << 32) | addr; }

typedef struct GxTexture {
    uint64_t key;
    uint32_t width, height, fmt, tlut_fmt;
    uint8_t *raw;          /* raw GX texture bytes, decoded by the renderer */
    size_t raw_len;
    uint8_t *tlut;         /* 512 bytes of TMEM palette, or NULL */
    uint8_t wrap_s, wrap_t; /* 0 clamp, 1 repeat, 2 mirror */
    bool linear;
    uint8_t subst;         /* gx_subst_register id of a texture the front end draws its own way, or 0 */
    uint8_t movie;         /* plane of a decoded movie frame: 1 Y, 2 U, 3 V (gx_movie_planes), else 0 */
    volatile int refs;
} GxTexture;

/* Textures the front end replaces (the pause screen's controller picture and button icons),
 * recognised by format, size and an FNV-1a hash of their bytes. Register before the game runs. */
void gx_subst_register(unsigned id, uint32_t fmt, uint32_t w, uint32_t h, uint64_t hash);
/* The I8 textures a movie frame was just decoded into (Y, U, V), so the renderer can scale those
 * planes its own way (thp.c). The last few sets are remembered: players cycle a ring of buffers. */
void gx_movie_planes(const uint32_t planes[3]);
/* Bytes of a GX texture of this format and size (0: unknown format). */
size_t gx_tex_size(uint32_t fmt, uint32_t w, uint32_t h);

typedef enum { GXEV_DRAW_DONE, GXEV_TOKEN, GXEV_TOKEN_INT } GxEventKind;
typedef struct { GxEventKind kind; uint16_t token; } GxEvent;

/* GPU thread / FIFO */
void gx_start_gpu(void);
void gx_stop_gpu(void);
void gx_submit(const uint8_t *bytes, size_t n);
bool gx_gpu_idle(void);
bool gx_gpu_busy_hint(void);
int gx_take_events(GxEvent *out, int max);

/* Frames: the VI retrace (gx_scanout) presents the last copied frame. */
void gx_scanout(void);
uint64_t gx_frame_count(void);
Frame *gx_frame_acquire(void);       /* latest presented frame, or NULL; release when done */
void gx_frame_release(Frame *f);

/* Textures by key (refcounted). */
GxTexture *gx_texture_acquire(uint64_t key);
void gx_texture_release(GxTexture *t);
/* Decode to RGBA8 (w*h*4 bytes); false if the format is unknown (out filled white). */
bool gx_texture_decode(const GxTexture *t, uint8_t *out);
/* I4 / I8 / IA4 / IA8 as 1 (I) or 2 (I, A) bytes per texel; 0 for other formats (gx.c). */
unsigned gx_texture_decode_compact(const GxTexture *t, uint8_t *out);
/* CMPR as BC1 blocks (GL_COMPRESSED_RGBA_S3TC_DXT1_EXT), (w/4)*(h/4)*8 bytes; false: not CMPR or a
 * size BC1 can't hold exactly. */
bool gx_texture_cmpr_bc1(const GxTexture *t, uint8_t *out);

/* Vertex idx (an index value) of batch b as a full GpuVertex. */
void gx_vertex_unpack(const Frame *f, const Batch *b, uint32_t idx, GpuVertex *out);
/* CPU reference of the XF vertex shader: GPU-path vertex in -> CPU-path vertex out. */
void gx_xf_eval(const uint32_t *xf, const GpuVertex *in, GpuVertex *out);
/* Transform on the CPU instead of in the vertex shader (Switch: sdmc:/switch/MKDD-NX/config/cpu_xform). */
/* Guest memory written behind the GPU's back (cache flushes, DMA): cached vertex conversions that
 * read it are redone. _all: everything (scene loads). g_gx_vcache: the vertex cache on (default). */
void gx_mem_dirty(uint32_t addr, uint32_t len);
void gx_drain(void); /* wait for the decoder to read all queued commands (before DMA into RAM) */
void gx_mem_dirty_all(void);
extern bool g_gx_vcache;
extern bool g_gx_cpu_xform;
extern bool g_gx_cpu_xform_forced; /* set by the renderer when the XF shader is unusable */
void gx_xf_report(char *out, size_t n); /* MKDD_XFCHECK result */
bool gx_dump_frame(const char *path); /* debug: the latest frame as text */

/* Decoder statistics: time spent decoding, draw commands decoded, draw setups rebuilt, vertices
 * transformed on the CPU, time the game waited for queue space. */
extern uint64_t g_gx_drains, g_gx_drain_ns;
extern uint64_t g_gx_busy_ns, g_gx_draws, g_gx_setups, g_gx_cpu_verts, g_gx_qwait_ns;
/* sched.c: time the guest core idled (no guest thread runnable), and the part of it while the
 * decoder was still busy. */
extern uint64_t g_guest_idle_ns, g_guest_idle_gpu_ns;
extern uint64_t g_sched_switches, g_sched_deliveries; /* sched.c: token handoffs, interrupt deliveries */
