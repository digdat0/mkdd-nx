/* Trick Style track effects (2026-11-12 unique tracks): spring pads, geysers and wandering tornadoes, laid out by the
 * track builder (MKDD_TrickTracks tools/build7.py, tracks9.py) in mods/trick/fx/<course ID>_<BOL hash>.txt, the key
 * the arena's route and coins use. One effect a line ('#' a comment):
 *   pad x y z heading hw hl vy [fw]         a kart on the ground inside the pad (heading in degrees, half width and
 *                                           half length) is thrown up at vy; fw (optional): a forward speed along the
 *                                           pad's heading (mVel units, about 3x a frame's travel), held until it lands
 *   geyser x y z r period on vy kind phase  erupts for on frames of every period (offset by phase): a kart within r
 *                                           is thrown up at vy; kind 0 water, 1 lava, 2 steam (the colour)
 *   tornado x y z ax az px pz r h lift      wanders round (x, z), ax / az across, periods px / pz frames; a kart
 *                                           within r below h is lifted (lift a frame) and spun, then thrown clear
 * The pushes are the kart's world velocity (KartBody +0x260); trick.c calls fx_kart for every kart while a race
 * runs (and keeps its landing-bounce damping off a kart thrown up here). Drawn after the race's opaque items as the
 * coins are (coins.c): translucent vertex-coloured triangles through the game's own GX calls. */
#include "native_impl.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#include "trickfx.h"

extern volatile uint32_t g_race_phase; /* hle.c: RaceDirector's phase (0 out of a race) */

#define FX_MAX 48
#define FX_KARTS 8
enum { FX_PAD, FX_GEYSER, FX_TORNADO };
typedef struct {
    int type, kind, period, on, phase, px, pz;
    float x, y, z, a, hw, hl, vy, r, ax, az, h, lift, fw;
} Fx;
static Fx s_fx[FX_MAX];
static int s_n;
static char s_key[32];
static bool s_dirty;
static uint32_t s_tick;
#define LFW_MAX 110   /* a pad's forward speed: frames at most (Neon's skyway gap takes about 70) */
#define LFW_UNITS 3.0f /* mVel per unit moved in a frame (about) */
static struct {
    uint16_t cool;  /* frames before an effect may push this kart again */
    uint16_t held;  /* frames a tornado has held it */
    int8_t storm;   /* the tornado holding it (-1 none) */
    uint8_t launch; /* 2026-11-13: frames left of a spring pad's or geyser's push (one frame's push gave only 100 up) */
    float lvy, ly;  /* that push's speed up and where it started */
    float lfw, lax, laz; /* 2026-11-13: a pad's forward speed, held until the kart lands (a gap jump: height alone added little reach) */
    uint16_t lft;    /* frames that forward speed has been held (it ends at LFW_MAX, or when the kart stops moving along it) */
    float lpx, lpz;  /* where the kart was the frame before (blocked: a wall or a building in the way) */
    bool resc, deep; /* (the log: Lakitu carrying it, far below the course) */
    bool carried;    /* Lakitu has carried it since it last touched the ground: no pushes (a geyser or tornado took it from the carry) */
} s_k[FX_KARTS];

void fx_course(uint32_t id, uint32_t hash) {
    snprintf(s_key, sizeof s_key, "%02x_%08x", (unsigned)id & 0xff, (unsigned)hash);
    s_dirty = true, s_n = 0, s_tick = 0;
    memset(s_k, 0, sizeof s_k);
    for (int i = 0; i < FX_KARTS; i++) s_k[i].storm = -1;
}

void fx_restart(void) {
    memset(s_k, 0, sizeof s_k);
    for (int i = 0; i < FX_KARTS; i++) s_k[i].storm = -1;
}

static void fx_load(void) {
    s_dirty = false, s_n = 0;
    char path[64], line[256];
    snprintf(path, sizeof path, "mods/trick/fx/%s.txt", s_key);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f) && s_n < FX_MAX) {
        Fx e;
        memset(&e, 0, sizeof e);
        if (sscanf(line, "pad %f %f %f %f %f %f %f %f", &e.x, &e.y, &e.z, &e.a, &e.hw, &e.hl, &e.vy, &e.fw) >= 7) e.type = FX_PAD; /* (fw optional) */
        else if (sscanf(line, "geyser %f %f %f %f %d %d %f %d %d", &e.x, &e.y, &e.z, &e.r, &e.period, &e.on, &e.vy, &e.kind, &e.phase) == 9 && e.period > 0) e.type = FX_GEYSER;
        else if (sscanf(line, "tornado %f %f %f %f %f %d %d %f %f %f", &e.x, &e.y, &e.z, &e.ax, &e.az, &e.px, &e.pz, &e.r, &e.h, &e.lift) == 10 && e.px > 0 && e.pz > 0) e.type = FX_TORNADO;
        else continue;
        e.a *= 0.017453293f;
        s_fx[s_n++] = e;
    }
    fclose(f);
    rt_log("trick fx: %s, %d effects", path, s_n);
}

static bool geyser_on(const Fx *e, int *p) {
    int q = (int)((s_tick + (uint32_t)e->phase) % (uint32_t)e->period);
    if (p) *p = q;
    return q < e->on;
}
static void storm_at(const Fx *e, float *x, float *z) {
    *x = e->x + e->ax * sinf(6.2831853f * (float)s_tick / (float)e->px);
    *z = e->z + e->az * sinf(6.2831853f * (float)s_tick / (float)e->pz + 1.3f);
}

bool fx_kart(Ctx *c, uint8_t *M, uint32_t body, uint32_t kart, bool ground) {
    if (s_dirty) fx_load();
    if (!s_n || kart >= FX_KARTS) return false;
    if (kart == 0) s_tick++;
    float p[3] = {ldf(c, M, body + 0x11c), ldf(c, M, body + 0x12c), ldf(c, M, body + 0x13c)};
    if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2])) return false;
    float v[3];
    ldfn(c, M, body + 0x260, v, 3);
    if (s_k[kart].cool) s_k[kart].cool--;
    { /* a hazard's fall (lava, deep water, a canyon): Lakitu's rescue (KartBody +0xb4 KartRescue, +0x74 flags 0x20), logged */
        uint32_t rs = ld32(c, M, body + 0xb4);
        bool resc = ram_ptr(rs) && (mld(c, M, rs + 0x74, 1) & 0x20);
        if (resc && !s_k[kart].resc) rt_log("trick fx: kart %u falls at (%.0f %.0f %.0f): Lakitu", kart, p[0], p[1], p[2]);
        if (!resc && p[1] < -3000 && !s_k[kart].deep) rt_log("trick fx: kart %u is down at (%.0f %.0f %.0f) with no rescue", kart, p[0], p[1], p[2]);
        s_k[kart].resc = resc, s_k[kart].deep = !resc && p[1] < -3000;
        if (resc) s_k[kart].carried = true;
        else if (ground) s_k[kart].carried = false;
        if (s_k[kart].carried) { /* no push carried on to the respawn, and none during it (a tornado's hold threw it from there) */
            s_k[kart].launch = 0, s_k[kart].lfw = 0, s_k[kart].storm = -1, s_k[kart].held = 0;
            return false;
        }
    }
    bool up = false;
    for (int i = 0; i < s_n; i++) {
        const Fx *e = &s_fx[i];
        float dx = p[0] - e->x, dz = p[2] - e->z;
        if (e->type == FX_PAD) {
            if (!ground || s_k[kart].cool || fabsf(p[1] - e->y) > 300) continue;
            float u = dx * cosf(e->a) + dz * sinf(e->a), w = -dx * sinf(e->a) + dz * cosf(e->a);
            if (fabsf(u) > e->hl + 60 || fabsf(w) > e->hw) continue;
            s_k[kart].launch = 45, s_k[kart].lvy = e->vy, s_k[kart].ly = p[1], s_k[kart].cool = 40, s_k[kart].lfw = e->fw, s_k[kart].lax = cosf(e->a), s_k[kart].laz = sinf(e->a), s_k[kart].lft = 0, s_k[kart].lpx = p[0], s_k[kart].lpz = p[2];
            rt_log("trick fx: spring pad %d throws kart %u up at %.0f", i, kart, e->vy);
        } else if (e->type == FX_GEYSER) {
            if (s_k[kart].cool || !geyser_on(e, NULL) || dx * dx + dz * dz > e->r * e->r || p[1] > e->y + 900) continue;
            s_k[kart].launch = 45, s_k[kart].lvy = e->vy, s_k[kart].ly = p[1], s_k[kart].cool = 40, s_k[kart].lfw = 0;
            rt_log("trick fx: geyser %d throws kart %u up at %.0f", i, kart, e->vy);
        } else if (e->type == FX_TORNADO) {
            float sx, sz;
            storm_at(e, &sx, &sz);
            float tx = p[0] - sx, tz = p[2] - sz, d = sqrtf(tx * tx + tz * tz);
            bool held = s_k[kart].storm == i;
            if (held && (d > e->r * 1.6f || p[1] > e->y + e->h * 0.8f || s_k[kart].held > 90)) { /* thrown clear: up and out */
                float o = d > 1 ? 1 / d : 0;
                v[0] += tx * o * 18, v[2] += tz * o * 18, v[1] = fmaxf(v[1], 25);
                s_k[kart].storm = -1, s_k[kart].cool = 150, up = true;
                rt_log("trick fx: tornado %d throws kart %u clear after %u frames, %.0f up", i, kart, s_k[kart].held, p[1] - e->y);
                continue;
            }
            if (!held && (s_k[kart].cool || d > e->r || p[1] > e->y + e->h * 0.6f)) continue;
            if (!held) s_k[kart].storm = (int8_t)i, s_k[kart].held = 0;
            s_k[kart].held++;
            float o = d > 1 ? 1 / d : 0;
            v[1] = fmaxf(v[1], e->lift);                       /* up */
            v[0] += -tz * o * 3.0f - tx * o * 1.2f;            /* round (anticlockwise) and a little in */
            v[2] += tx * o * 3.0f - tz * o * 1.2f;
            float sp = sqrtf(v[0] * v[0] + v[2] * v[2]);
            if (sp > 45) v[0] *= 45 / sp, v[2] *= 45 / sp;
            up = true;
        }
    }
    if (s_k[kart].launch) { /* held up while it climbs (below) */
        s_k[kart].launch--;
        if (p[1] > s_k[kart].ly + s_k[kart].lvy * 18) s_k[kart].launch = 0; /* the game re-derives its climb each frame (a push
                                                                         * fades in a few frames, as the tornado's lift): pushed up to about 18 x vy */
        if (v[1] < s_k[kart].lvy) v[1] = s_k[kart].lvy;
        up = true;
    }
    if (s_k[kart].lfw > 0) { /* carried along the pad's line (either way) until it lands: mVel units, about 3x a frame's
                              * travel; the game's air drag let karts sink into a gap after it, and CPUs drifted off the side */
        /* 2026-11-13 (Switch): held on until a touchdown that never came (Neon's skyway: past the landing, pinned in the
         * air against the scenery, pushed on forever): it ends after LFW_MAX frames or when the kart stops getting
         * anywhere along it (under a third of the speed, after the first few frames) */
        float moved = (p[0] - s_k[kart].lpx) * s_k[kart].lax + (p[2] - s_k[kart].lpz) * s_k[kart].laz;
        s_k[kart].lpx = p[0], s_k[kart].lpz = p[2];
        bool blocked = s_k[kart].lft > 6 && fabsf(moved) < s_k[kart].lfw / 3 / LFW_UNITS;
        if ((ground && !s_k[kart].launch && s_k[kart].cool < 30) || ++s_k[kart].lft > LFW_MAX || blocked) {
            if (!ground) rt_log("trick fx: kart %u's forward push ends in the air after %u frames%s", kart, s_k[kart].lft, blocked ? " (blocked)" : "");
            s_k[kart].lfw = 0;
        } else {
            float d = v[0] * s_k[kart].lax + v[2] * s_k[kart].laz, sd = d < 0 ? -1.0f : 1.0f;
            if (fabsf(d) < s_k[kart].lfw) d = sd * s_k[kart].lfw;
            v[0] = s_k[kart].lax * d, v[2] = s_k[kart].laz * d, up = true;
        }
    }
    if (up) stfn(c, M, body + 0x260, v, 3);
    return up;
}

/* ---- drawing (as coins.c's) ---- */
#define J3DSYS 0x803acf90u
#define J3D_MTX_CACHE 0x803afe54u
#define G_GXClearVtxDesc 0x800BCA18u
#define G_GXSetVtxDesc 0x800BC100u
#define G_GXSetVtxAttrFmt 0x800BCA50u
#define G_GXSetNumChans 0x800BF040u
#define G_GXSetChanCtrl 0x800BF07Cu
#define G_GXSetNumTexGens 0x800BD5D8u
#define G_GXSetNumIndStages 0x800C0530u
#define G_GXSetTevDirect 0x800C0554u
#define G_GXSetNumTevStages 0x800C0D94u
#define G_GXSetTevOrder 0x800C0BF8u
#define G_GXSetTevOp 0x800C0658u
#define G_GXSetTevSwapMode 0x800C0A60u
#define G_GXSetAlphaCompare 0x800C0B28u
#define G_GXSetZCompLoc 0x800C11E4u
#define G_GXSetBlendMode 0x800C1104u
#define G_GXSetZMode 0x800C11B0u
#define G_GXSetCullMode 0x800BE0E4u
#define G_GXSetCurrentMtx 0x800C2178u
#define G_GXLoadPosMtxImm 0x800C20D8u
#define G_GXBegin 0x800BDEC4u
#define FX_FMT 7
#define FX_VMAX 6000
#define FX_FAR 26000.0f
static uint8_t s_v[FX_VMAX * 16];
static uint8_t *s_o;
static int s_nv;

static void gxcall(Ctx *c, uint8_t *M, uint32_t fn, const uint32_t *a, unsigned n) {
    uint32_t old = frame_push(c, M, 0x40);
    ctx_call_guest(c, fn, a, n);
    frame_pop(c, old);
}
#define GX(fn, ...) do { const uint32_t a_[] = {__VA_ARGS__}; gxcall(c, M, fn, a_, sizeof a_ / sizeof a_[0]); } while (0)

static void vtx(const float p[3], uint32_t rgba) {
    if (s_nv >= FX_VMAX) return;
    uint8_t *o = s_o;
    for (int i = 0; i < 3; i++) {
        uint32_t u;
        memcpy(&u, &p[i], 4);
        o[0] = (uint8_t)(u >> 24), o[1] = (uint8_t)(u >> 16), o[2] = (uint8_t)(u >> 8), o[3] = (uint8_t)u;
        o += 4;
    }
    o[0] = (uint8_t)(rgba >> 24), o[1] = (uint8_t)(rgba >> 16), o[2] = (uint8_t)(rgba >> 8), o[3] = (uint8_t)rgba;
    s_o = o + 4, s_nv++;
}
static uint32_t rgba(uint32_t rgb, float a) {
    int ai = (int)(a * 255);
    return rgb << 8 | (uint32_t)(ai < 0 ? 0 : ai > 255 ? 255 : ai);
}
static void quad(const float a[3], const float b[3], const float cc[3], const float d[3], uint32_t ca, uint32_t cb) {
    if (s_nv + 6 > FX_VMAX) return;
    vtx(a, ca), vtx(b, ca), vtx(cc, cb), vtx(a, ca), vtx(cc, cb), vtx(d, cb);
}

/* a twisted funnel of rings: radius r0 at the bottom growing to r1 at height h; colour rgb, alpha a, swirl turn */
static void funnel(float x, float y, float z, float h, float r0, float r1, uint32_t rgb, float a, float turn, int rings, int seg) {
    for (int j = 0; j < rings; j++) {
        float t0 = (float)j / rings, t1 = (float)(j + 1) / rings;
        float y0 = y + h * t0, y1 = y + h * t1;
        float q0 = r0 + (r1 - r0) * powf(t0, 1.6f), q1 = r0 + (r1 - r0) * powf(t1, 1.6f);
        float w0 = turn + t0 * 2.2f, w1 = turn + t1 * 2.2f;
        for (int k = 0; k < seg; k++) {
            float g0 = 6.2831853f * k / seg, g1 = 6.2831853f * (k + 1) / seg;
            float A[3] = {x + q0 * cosf(g0 + w0), y0, z + q0 * sinf(g0 + w0)}, B[3] = {x + q0 * cosf(g1 + w0), y0, z + q0 * sinf(g1 + w0)};
            float Cc[3] = {x + q1 * cosf(g1 + w1), y1, z + q1 * sinf(g1 + w1)}, D[3] = {x + q1 * cosf(g0 + w1), y1, z + q1 * sinf(g0 + w1)};
            float st = 0.6f + 0.4f * sinf(3 * g0 + 4 * t0 - turn * 2); /* streaks */
            quad(A, B, Cc, D, rgba(rgb, a * st * (0.6f + 0.4f * t0)), rgba(rgb, a * st * (0.6f + 0.4f * t1)));
        }
    }
}

void fx_draw(Ctx *c, uint8_t *M, uint32_t buf) {
    if (!s_n || g_race_phase == 0) return;
    uint32_t rm = ld32(c, M, ctx_gpr(c, 13) - 0x5c38);
    if (!ram_ptr(rm)) return;
    uint32_t rd = ld32(c, M, rm + 0x1c);
    if (!ram_ptr(rd)) return;
    uint32_t items = ld32(c, M, rd + 0x30);
    if (!ram_ptr(items) || buf != ld32(c, M, items + 8)) return;
    float v[12];
    ldfn(c, M, J3DSYS, v, 12);
    float cam[3];
    for (int j = 0; j < 3; j++) cam[j] = -(v[j] * v[3] + v[4 + j] * v[7] + v[8 + j] * v[11]);
    s_o = s_v, s_nv = 0;
    float spin = (float)s_tick * 0.12f;
    for (int i = 0; i < s_n; i++) {
        const Fx *e = &s_fx[i];
        float x = e->x, z = e->z;
        if (e->type == FX_TORNADO) storm_at(e, &x, &z);
        if (e->type == FX_PAD) continue; /* (drawn in the course model) */
        float dx = x - cam[0], dz = z - cam[2];
        if (dx * dx + dz * dz > FX_FAR * FX_FAR) continue;
        if (e->type == FX_TORNADO) {
            funnel(x, e->y - 50, z, e->h, e->r * 0.22f, e->r * 1.7f, 0xb8a27c, 0.78f, spin, 12, 14);
            funnel(x, e->y - 50, z, e->h * 0.94f, e->r * 0.14f, e->r * 1.2f, 0xe2d2b0, 0.6f, -spin * 1.4f, 10, 12);
            funnel(x, e->y - 30, z, 700, e->r * 1.0f, e->r * 2.1f, 0xc4ac84, 0.5f, spin * 0.6f, 2, 16); /* the dust at its foot */
        } else {
            static const uint32_t COL[3] = {0xd8f0ff, 0xff8a20, 0xf0f0f0};
            static const float ALPHA[3] = {0.65f, 0.9f, 0.5f};
            uint32_t col = COL[e->kind >= 0 && e->kind < 3 ? e->kind : 0];
            int q;
            bool on = geyser_on(e, &q);
            float h;
            if (on) h = 5200.0f * fminf(1, q / 8.0f) * (q > e->on - 16 ? (e->on - q) / 16.0f : 1);
            else {
                int left = e->period - q; /* the warning: a puffing column the last 45 frames before it blows */
                h = left < 45 ? 350.0f + 150.0f * sinf((float)s_tick * 0.8f) : 90.0f;
            }
            if (h < 40) h = 40;
            funnel(x, e->y - 40, z, h, e->r * 0.45f, e->r * (on ? 0.75f : 0.5f), col, ALPHA[e->kind >= 0 && e->kind < 3 ? e->kind : 0], spin, on ? 6 : 1, 10);
            if (on) funnel(x, e->y + h * 0.85f, z, h * 0.2f, e->r * 0.75f, e->r * 1.4f, col, 0.4f, -spin, 2, 10); /* the spray at its top */
        }
    }
    if (!s_nv) return;
    GX(G_GXClearVtxDesc, 0);
    GX(G_GXSetVtxDesc, 9, 1);
    GX(G_GXSetVtxDesc, 11, 1);
    GX(G_GXSetVtxAttrFmt, FX_FMT, 9, 1, 4, 0);
    GX(G_GXSetVtxAttrFmt, FX_FMT, 11, 1, 5, 0);
    GX(G_GXSetNumChans, 1);
    GX(G_GXSetChanCtrl, 4, 0, 0, 1, 0, 0, 2);
    GX(G_GXSetNumTexGens, 0);
    GX(G_GXSetNumIndStages, 0);
    GX(G_GXSetTevDirect, 0);
    GX(G_GXSetNumTevStages, 1);
    GX(G_GXSetTevOrder, 0, 0xff, 0xff, 4);
    GX(G_GXSetTevOp, 0, 4);
    GX(G_GXSetTevSwapMode, 0, 0, 0);
    GX(G_GXSetAlphaCompare, 7, 0, 0, 7, 0);
    GX(G_GXSetZCompLoc, 1);
    GX(G_GXSetBlendMode, 1, 4, 5, 0); /* blend: source alpha, 1 - source alpha */
    GX(G_GXSetZMode, 1, 3, 0);        /* tested, not written */
    GX(G_GXSetCullMode, 0);
    GX(G_GXSetCurrentMtx, 0);
    GX(G_GXLoadPosMtxImm, J3DSYS, 0);
    GX(G_GXBegin, 0x90, FX_FMT, (uint32_t)s_nv);
    hw_gather_copy(s_v, (size_t)(s_o - s_v));
    GX(G_GXSetBlendMode, 0, 1, 0, 0);
    GX(G_GXSetZMode, 1, 3, 1);
    mst(c, M, ctx_gpr(c, 13) - 0x6688, 4, 0);
    for (int i = 0; i < 10; i++) mst(c, M, J3D_MTX_CACHE + 2 * i, 2, 0xffff);
}
