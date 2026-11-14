/* The cameras: the widescreen aspect, two players side by side (MULTIPLAYER > SPLIT SCREEN), the field of
 * view and first person (moved from native.c; natives registered there, declared in camera.h). */
#include "camera.h"
#include "hud.h"
#include "drivers.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#include "charpack.h"
#include "plat.h"
#include "table.h"
#include "gx.h"
#include "gp_inline.h"

/* ---- widescreen: the game's camera aspect (System::getPerspAspect*, read by every kart camera
 * and the lens flare) and C_MTXPerspective (menus, character select, replays) times
 * g_aspect_scale. Exact originals at 1.0. ---- */
float g_aspect_scale = 1.0f;
uint32_t g_native_addr[NATIVE_COUNT];
static uint64_t aspect_sda(Ctx *c, uint8_t *M, uint32_t off) {
    double v = ldf(c, M, (uint32_t)RD(c, REG_R13, 4) - off);
    return dbits(g_aspect_scale == 1.0f ? v : rs(v * g_aspect_scale));
}
uint64_t i_getPerspAspectDiv2__6SystemFv(Ctx *c, uint8_t *M, ARGS) { UNUSED_ARGS; return aspect_sda(c, M, 0x5618); }
uint64_t i_getPerspAspectDiv4__6SystemFv(Ctx *c, uint8_t *M, ARGS) { UNUSED_ARGS; return aspect_sda(c, M, 0x5614); }
uint64_t i_getPerspAspectSub__6SystemFv(Ctx *c, uint8_t *M, ARGS) { UNUSED_ARGS; return aspect_sda(c, M, 0x5618); }
uint64_t i_C_MTXPerspective(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    if (g_aspect_scale != 1.0f) {
        uint64_t a = dbits(rs(ps_d(a2) * g_aspect_scale));
        WR(c, 0x4010, 8, a);
        WR(c, 0x5010, 8, a);
    }
    ctx_call_orig(c, g_native_addr[NAT_C_MTXPerspective]);
    return 0;
}
/* FUN_8029e0a8(KartCam *, ...): sets up a race camera and stores its aspect (from the getters
 * above) at +0x168 once, so a display mode change mid-race only stretched the picture. The
 * cameras are remembered here and aspect_refresh rescales them on the next race frame; a slot is
 * only rewritten while it still holds the value last stored there (cameras of an old race are
 * gone). */
static struct { uint32_t cam, bits; float base; } s_cams[8];
static unsigned s_cam_next;
static float s_cam_scale = 1.0f;
/* ---- two players side by side (g_vsplit, MULTIPLAYER > SPLIT SCREEN). System's two-screen tables
 * (filled at start-up; the getters FUN_801d0ce4.. and FUN_801d0e5c.., screen n at +0x10 * n): the 3D
 * viewport (f32 x, y, w, h: 0,0,608,223 and 0,225,608,223) and the 3D scissor (s32, the same). Every
 * user reads them (the race cameras FUN_8029e0a8, SiUtil::getViewport for 2D over the 3D), so they
 * are swapped for columns 0-303 and 305-608 while it is on, the game's put back when off. Found from
 * the getters' code (lis / addi), or left alone. ---- */
volatile uint32_t g_vsplit;
#define VS_ASPECT ((float)((304.0 / 448.0) / (608.0 / 223.0)))
bool g_vsplit_on; /* the tables hold the columns */
static uint32_t s_vs_orig[16];
static bool s_vs_saved;
static uint32_t vs_table(Ctx *c, uint8_t *M, uint32_t getter) {
    uint32_t hi = ld32(c, M, getter), lo = ld32(c, M, getter + 8);
    if ((hi & 0xFFFF0000u) != 0x3C800000u || (lo & 0xFFFF0000u) != 0x38640000u) return 0; /* lis r4 / addi r3,r4 */
    uint32_t a = (hi << 16) + (uint32_t)(int32_t)(int16_t)(lo & 0xFFFF);
    return IS_RAM(a) ? a : 0;
}
static uint32_t s_vs_vp, s_vs_sc; /* the tables, 0 not found */
static void vs_find(Ctx *c, uint8_t *M) {
    static bool looked;
    if (!looked) {
        looked = true, s_vs_vp = vs_table(c, M, 0x801D0CE4u), s_vs_sc = vs_table(c, M, 0x801D0E5Cu);
        if (!s_vs_vp || !s_vs_sc) rt_log("split screen: System's two-screen tables not found (%#x %#x): top and bottom only", s_vs_vp, s_vs_sc);
    }
}
static void vsplit_sync(Ctx *c, uint8_t *M) {
    vs_find(c, M);
    if (!s_vs_vp || !s_vs_sc) return;
    float w;
    uint32_t u = ld32(c, M, s_vs_vp + 8);
    memcpy(&w, &u, 4);
    bool on = g_vsplit != 0;
    static uint32_t last = ~0u;
    if (last != (on | ld32(c, M, s_vs_sc + 8) << 1)) {
        last = on | ld32(c, M, s_vs_sc + 8) << 1;
        rt_log("split screen: setting %s, tables %#x %#x: screen 0 w %.0f, scissor w %u h %u, saved %d", on ? "side by side" : "top and bottom", s_vs_vp, s_vs_sc, w, ld32(c, M, s_vs_sc + 8), ld32(c, M, s_vs_sc + 12), s_vs_saved);
    }
    /* the game's (top and bottom, full width): kept to put back */
    if (!g_vsplit_on && w > 400.0f && w < 1000.0f && ld32(c, M, s_vs_sc + 8) > 400) {
        for (uint32_t i = 0; i < 8; i++) s_vs_orig[i] = ld32(c, M, s_vs_vp + 4 * i), s_vs_orig[8 + i] = ld32(c, M, s_vs_sc + 4 * i);
        s_vs_saved = true;
    }
    if (on == g_vsplit_on || !s_vs_saved) return;
    if (on) {
        /* no gap: the renderer draws a thin line where they meet (Frame.vseam) */
        static const float V[8] = {0, 0, 304, 448, 304, 0, 304, 448};
        static const uint32_t S[8] = {0, 0, 304, 448, 304, 0, 304, 448};
        for (uint32_t i = 0; i < 8; i++) memcpy(&u, &V[i], 4), mst(c, M, s_vs_vp + 4 * i, 4, u), mst(c, M, s_vs_sc + 4 * i, 4, S[i]);
    } else {
        for (uint32_t i = 0; i < 8; i++) mst(c, M, s_vs_vp + 4 * i, 4, s_vs_orig[i]), mst(c, M, s_vs_sc + 4 * i, 4, s_vs_orig[8 + i]);
    }
    g_vsplit_on = on;
    rt_log("split screen: two players %s", on ? "side by side" : "top and bottom");
}
uint64_t i_FUN_8029e0a8(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    float scale = g_aspect_scale;
    vsplit_sync(c, M);
    ctx_call_orig(c, g_native_addr[NAT_FUN_8029e0a8]);
    uint32_t cam = (uint32_t)a0;
    /* a two-screen camera (+0xe4 1 top, 2 bottom) side by side: its aspect from 303 x 448, not 608 x 223 */
    unsigned kind = mld(c, M, cam + 0xe4, 1);
    if (g_vsplit_on && (kind == 1 || kind == 2)) stf(c, M, cam + 0x168, ldf(c, M, cam + 0x168) * VS_ASPECT);
    unsigned k = 0;
    while (k < 8 && s_cams[k].cam != cam) k++;
    if (k == 8) k = s_cam_next++ & 7;
    s_cams[k].cam = cam;
    s_cams[k].bits = ld32(c, M, cam + 0x168);
    s_cams[k].base = ldf(c, M, cam + 0x168) / scale;
    return 0;
}
/* J2DGrafContext::setScissor (FUN_80027ebc: x1, y1, x2, y2 as floats to +0x14..+0x20). The race's 2D
 * contexts take a screen's rectangle from the scissor table once, at race start, and would clip to it
 * for the rest of the race (GOAL cut off at the middle after changing SPLIT SCREEN): those contexts
 * are remembered for vsplit_live. A remembered one is only rewritten while it still holds that
 * screen's rectangle (contexts of an old race are gone). */
static void vs_rect(Ctx *c, uint8_t *M, unsigned screen, uint32_t out[4]) {
    uint32_t q = s_vs_sc + 0x10 * screen;
    int32_t x = (int32_t)ld32(c, M, q), y = (int32_t)ld32(c, M, q + 4);
    float v[4] = {(float)x, (float)y, (float)(x + (int32_t)ld32(c, M, q + 8)), (float)(y + (int32_t)ld32(c, M, q + 12))};
    memcpy(out, v, sizeof v);
}
#define VS_GC_MAX 256 /* the race's own, kept from its start to its GOAL, among those the HUD sets up as it draws */
static struct { uint32_t gc, screen; } s_vs_gc[VS_GC_MAX];
uint64_t i_FUN_80027ebc(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t gc = (uint32_t)a0, v[4], r[4];
    for (uint32_t i = 0; i < 4; i++) v[i] = ld32(c, M, (uint32_t)a1 + 4 * i), mst(c, M, gc + 0x14 + 4 * i, 4, v[i]);
    vs_find(c, M);
    if (!s_vs_sc) return 0;
    for (unsigned n = 0; n < 2; n++) {
        vs_rect(c, M, n, r);
        if (memcmp(r, v, sizeof v)) continue;
        unsigned k = 0, free = VS_GC_MAX;
        for (; k < VS_GC_MAX && s_vs_gc[k].gc != gc; k++)
            if (!s_vs_gc[k].gc && free == VS_GC_MAX) free = k;
        if (k == VS_GC_MAX) k = free;
        /* full: only those no longer holding their screen's rectangle make room, never a live one */
        for (unsigned i = 0; k == VS_GC_MAX && i < VS_GC_MAX; i++) {
            uint32_t h[4], w[4];
            vs_rect(c, M, s_vs_gc[i].screen, w);
            for (unsigned j = 0; j < 4; j++) h[j] = ld32(c, M, s_vs_gc[i].gc + 0x14 + 4 * j);
            if (memcmp(h, w, sizeof h)) k = i;
        }
        if (k == VS_GC_MAX) {
            static bool told;
            if (!told) told = true, rt_log("split screen: more than %d 2D contexts: %#x left out", VS_GC_MAX, gc);
            break;
        }
        s_vs_gc[k].gc = gc, s_vs_gc[k].screen = n;
        break;
    }
    return 0;
}
/* SPLIT SCREEN changed during a race: the tables swapped, the 2D contexts given their new rectangle,
 * and the two-screen cameras given their
 * new screen as FUN_8029e0a8 does from them (screen kind - 1: scissor +0xec/+0xf0 and its w/h
 * floats +0x128/+0x12c, viewport w/h +0x140/+0x144 and x/y ints +0xf4/+0xf8, +0x110 half the
 * height difference) and their aspect, from the next frame */
void vsplit_live(Ctx *c, uint8_t *M) {
    bool was = g_vsplit_on;
    uint32_t before[2][4];
    vs_find(c, M);
    if (s_vs_sc) vs_rect(c, M, 0, before[0]), vs_rect(c, M, 1, before[1]);
    vsplit_sync(c, M);
    if (g_vsplit_on == was || !s_vs_vp || !s_vs_sc) return;
    unsigned moved = 0, dropped = 0;
    for (unsigned k = 0; k < VS_GC_MAX; k++) {
        uint32_t gc = s_vs_gc[k].gc, r[4];
        unsigned n = s_vs_gc[k].screen, i = 0;
        if (!gc || !IS_RAM(gc)) continue;
        while (i < 4 && ld32(c, M, gc + 0x14 + 4 * i) == before[n][i]) i++;
        if (i < 4) { s_vs_gc[k].gc = 0; dropped++; continue; }
        vs_rect(c, M, n, r);
        for (i = 0; i < 4; i++) mst(c, M, gc + 0x14 + 4 * i, 4, r[i]);
        moved++;
    }
    rt_log("split screen: %u 2D contexts moved, %u left as they are", moved, dropped);
    for (unsigned k = 0; k < 8; k++) {
        uint32_t cam = s_cams[k].cam;
        if (!cam || !IS_RAM(cam) || ld32(c, M, cam + 0x168) != s_cams[k].bits) continue;
        unsigned kind = mld(c, M, cam + 0xe4, 1);
        if (kind != 1 && kind != 2) continue;
        uint32_t v = s_vs_vp + 0x10 * (kind - 1), q = s_vs_sc + 0x10 * (kind - 1);
        float vw = ldf(c, M, v + 8), vh = ldf(c, M, v + 12);
        int32_t sh = (int32_t)ld32(c, M, q + 12);
        mst(c, M, cam + 0xec, 4, ld32(c, M, q)), mst(c, M, cam + 0xf0, 4, ld32(c, M, q + 4));
        stf(c, M, cam + 0x128, (float)(int32_t)ld32(c, M, q + 8)), stf(c, M, cam + 0x12c, (float)sh);
        stf(c, M, cam + 0x140, vw), stf(c, M, cam + 0x144, vh);
        mst(c, M, cam + 0x110, 4, (uint32_t)(((int32_t)vh - sh) / 2));
        mst(c, M, cam + 0xf4, 4, (uint32_t)(int32_t)ldf(c, M, v)), mst(c, M, cam + 0xf8, 4, (uint32_t)(int32_t)ldf(c, M, v + 4));
        stf(c, M, cam + 0x168, g_vsplit_on ? ldf(c, M, cam + 0x168) * VS_ASPECT : ldf(c, M, cam + 0x168) / VS_ASPECT);
        s_cams[k].bits = ld32(c, M, cam + 0x168);
        s_cams[k].base = ldf(c, M, cam + 0x168) / s_cam_scale;
    }
}
/* Goal2D (J2DManager, r13 - 0x5d58, +0x38) keeps a 2D context per screen (+4, +8) and where to draw
 * GOAL on it (+0xa4 / +0xa8, then every 0x10), all worked out once from the tables, for a screen twice
 * as big as the viewport with GOAL's middle at (304, 218). They follow the layout from the first side by side
 * on; side by side a bigger area (2.5x), so GOAL's 360 pixels fit a column of 304. A race only ever
 * top and bottom keeps the game's own. Left alone unless both contexts look like J2DOrthoGraphs. */
void vs_goal(Ctx *c, uint8_t *M) {
    static bool used;
    if (!s_vs_vp || !s_vs_sc || !(used |= g_vsplit_on)) return;
    uint32_t mgr = ld32(c, M, (uint32_t)ctx_gpr(c, 13) - 0x5d58);
    uint32_t goal = IS_RAM(mgr) ? ld32(c, M, mgr + 0x38) : 0;
    uint32_t race = ld32(c, M, (uint32_t)ctx_gpr(c, 13) - 0x5c38);
    uint32_t info = IS_RAM(race) ? ld32(c, M, race + 0x38) : 0;
    if (!IS_RAM(goal) || !IS_RAM(info) || mld(c, M, info + 0x22, 2) != 2) return;
    uint32_t gc[2] = {ld32(c, M, goal + 4), ld32(c, M, goal + 8)};
    if (!IS_RAM(gc[0]) || !IS_RAM(gc[1])) return;
    uint32_t vt = ld32(c, M, gc[0]);
    if (!IS_RAM(vt) || ld32(c, M, gc[1]) != vt) return;
    for (uint32_t n = 0; n < 2; n++) {
        uint32_t v = s_vs_vp + 0x10 * n, r[4], g = gc[n], at = goal + 0x10 * n;
        float x = ldf(c, M, v), y = ldf(c, M, v + 4), w = ldf(c, M, v + 8), h = ldf(c, M, v + 12);
        float W = w * (g_vsplit_on ? 2.5f : 2.0f), H = h * (g_vsplit_on ? 2.5f : 2.0f);
        const float bounds[4] = {x, y, x + w, y + h}, ortho[4] = {x, y, x + W, y + H};
        vs_rect(c, M, n, r);
        for (uint32_t i = 0; i < 4; i++) {
            stf(c, M, g + 4 + 4 * i, bounds[i]);
            mst(c, M, g + 0x14 + 4 * i, 4, r[i]);
            stf(c, M, g + 0xbc + 4 * i, ortho[i]);
        }
        stf(c, M, at + 0xa4, g_vsplit_on ? x + W / 2 - 304 : x + w / 2);
        stf(c, M, at + 0xa8, g_vsplit_on ? y + H / 2 - 218 : y);
    }
}
void aspect_refresh(Ctx *c, uint8_t *M) {
    float scale = g_aspect_scale;
    if (scale == s_cam_scale) return;
    s_cam_scale = scale;
    for (unsigned k = 0; k < 8; k++) {
        if (!s_cams[k].cam || ld32(c, M, s_cams[k].cam + 0x168) != s_cams[k].bits) { s_cams[k].cam = 0; continue; }
        stf(c, M, s_cams[k].cam + 0x168, (float)rs(s_cams[k].base * scale));
        s_cams[k].bits = ld32(c, M, s_cams[k].cam + 0x168);
    }
}

/* ---- field of view (the - menu's FIELD OF VIEW): the race cameras' projection (FUN_801a77e8, from
 * GrafPort::setPort: matrix, fovy, aspect, near, far) and their view-frustum culling (FUN_8000a260,
 * J3DUClipper::calcViewFrustum: fovy in degrees at +0x4c) both see tan(fovy / 2) times
 * g_fov_scale, so the culling matches the wider picture. Exact originals at 1.0. ---- */
float g_fov_scale = 1.0f;
static double fov_widen(double deg) {
    double r = atan(tan(deg * (3.14159265358979323846 / 360.0)) * g_fov_scale) * (360.0 / 3.14159265358979323846);
    return r < 170.0 ? r : 170.0;
}
uint64_t i_FUN_801a77e8(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    if (g_fov_scale != 1.0f) {
        uint64_t f = dbits(rs(fov_widen(ps_d(a1))));
        WR(c, 0x4008, 8, f);
        WR(c, 0x5008, 8, f);
    }
    ctx_call_orig(c, g_native_addr[NAT_FUN_801a77e8]);
    return 0;
}
uint64_t i_FUN_8000a260(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t clip = (uint32_t)a0;
    float fovy = ldf(c, M, clip + 0x4c);
    bool widen = g_fov_scale != 1.0f;
    if (widen) stf(c, M, clip + 0x4c, (float)rs(fov_widen(fovy)));
    ctx_call_orig(c, g_native_addr[NAT_FUN_8000a260]);
    if (widen) stf(c, M, clip + 0x4c, fovy);
    return 0;
}

/* ---- first person (the - menu's CAMERA): FUN_802b6bd4 is KartCam's chase view (camera mode
 * +0xe1 == 0, DoCameraMode), ending with C_MTXLookAt(+0x80, pos +0x1e8, up, look +0x1f4). In
 * single player, after it, the camera sits at the driver's eye in the kart body's frame (body
 * = KartCam +0: KartBody +0x110, a 3x4 matrix: columns x, y up, z forward, w position) and looks
 * along its forward axis with its up, unsmoothed: every bump, tilt and spin of the kart shows.
 * The respawn, cannon, pipe and demo views are other modes and stay the game's. Split screen (2 to 4
 * players) too: each view's camera on its own kart. g_look_back (bit v: view v, any number of
 * screens) turns the view around: first person looks backwards from the same eye, the chase
 * camera is mirrored to the kart's front. FREE CAMERA: while g_cam_pan[v] is off centre, the
 * view turns round the kart's up axis (chase: the camera and its look point round the kart;
 * first person: the look direction round the eye), up to PAN_RATE a frame at full tilt, as far
 * as it is held (a whole turn and more). Centred, g_cam_back says how it returns: at once, gliding
 * (PAN_EASE of the way left each frame, the short way round: about a third of a second), or not
 * until CAMERA RESET; a new camera (next race, respawn) starts straight. ---- */
volatile uint32_t g_first_person;
volatile uint32_t g_look_back, g_cam_frames[4];
volatile uint32_t g_hud_mirror, g_hud_wide, g_split3_vi;
volatile float g_mirror_eye[2], g_mirror_tilt[2] = {0.25882f, 0.96593f};
#define MIRROR_BEHIND 170.0f /* this far behind the kart's centre: clear of the kart */
#define MIRROR_TILT 0.2618f         /* the mirror looks this much further down than the camera (15 degrees) */
#define MIRROR_TILT_DRIVER 0.0873f  /* ... with DRIVER FPV, plus its own tilt (5 degrees) */
/* The rear-view mirror looks backwards from just behind the kart, which it doesn't show: its eye
 * in the view of the camera eye / at / up (the one the screen is drawn with), tilted down by tilt. */
static void mirror_eye(Ctx *c, uint8_t *M, uint32_t body, const float e[3], const float a[3], const float u[3], float tilt) {
    float z[3], x[3], kx = 0, kz = 0;
    for (int i = 0; i < 3; i++) z[i] = e[i] - a[i];
    v3_norm(z);
    x[0] = u[1] * z[2] - u[2] * z[1], x[1] = u[2] * z[0] - u[0] * z[2], x[2] = u[0] * z[1] - u[1] * z[0];
    v3_norm(x);
    for (int i = 0; i < 3; i++) {
        float d = ldf(c, M, body + 0x110 + 4 * (4 * (uint32_t)i + 3)) - e[i];
        kx += x[i] * d, kz += z[i] * d;
    }
    kz += MIRROR_BEHIND;
    g_mirror_eye[0] = isfinite(kx) ? kx : 0, g_mirror_eye[1] = isfinite(kz) ? kz : 0;
    tilt = tilt < 0 ? 0 : tilt > 0.7f ? 0.7f : tilt;
    g_mirror_tilt[0] = sinf(tilt), g_mirror_tilt[1] = cosf(tilt);
}
volatile float g_cam_pan[4];
volatile uint32_t g_cam_back[4], g_cam_reset;
volatile float g_cam_height[4] = {1.0f, 1.0f, 1.0f, 1.0f}, g_cam_dist[4] = {1.0f, 1.0f, 1.0f, 1.0f};
#define CAM_MIN_UP 60.0f /* CAMERA HEIGHT's lowest: this far above the kart's centre (about a wheel's height) */
#define PAN_RATE 0.06f /* radians a frame: half a turn in under a second */
#define PAN_EASE 0.8f  /* gliding back: this much of the angle kept each frame (1% after 20 frames) */
static float s_pan[4];
static uint32_t s_pan_cam[4];
static float pan_step(unsigned view, uint32_t cam) {
    float pan = g_cam_pan[view], a = s_pan_cam[view] == cam ? s_pan[view] : 0;
    s_pan_cam[view] = cam;
    if (g_cam_reset >> view & 1) a = 0;
    else if (pan != 0) a = fmodf(a - pan * PAN_RATE, 6.2831853f);
    else if (g_cam_back[view] == CAM_EASE && a != 0) {
        if (a > 3.1415927f) a -= 6.2831853f;
        else if (a < -3.1415927f) a += 6.2831853f;
        a *= PAN_EASE;
        if (a > -0.005f && a < 0.005f) a = 0;
    } else if (g_cam_back[view] != CAM_STAY) a = 0;
    return s_pan[view] = a;
}
/* v turned by a round the unit axis k (Rodrigues) */
static void v3_turn(float v[3], const float k[3], float a) {
    float c = cosf(a), s = sinf(a), d = k[0] * v[0] + k[1] * v[1] + k[2] * v[2];
    float x[3] = {k[1] * v[2] - k[2] * v[1], k[2] * v[0] - k[0] * v[2], k[0] * v[1] - k[1] * v[0]};
    for (int i = 0; i < 3; i++) v[i] = v[i] * c + x[i] * s + k[i] * d * (1 - c);
}
static uint32_t s_fp_cam[4], s_fp_body[4]; /* per view (KartCam +0xe3): the camera last moved to first person, its kart body */
volatile uint32_t g_fp_kart;
#include "kart_eye.h"
/* KART FPV: added to the kart's eye (KART_EYE); a mod's karts have no entry, so there the eye is
 * KART_UP above and KART_FWD ahead of the head, KART_TILT radians down */
#define KART_UP 25.0f
#define KART_FWD 10.0f
#define KART_TILT 0.17f
float g_kart_up, g_kart_fwd, g_kart_tilt, g_kart_add[21][3];
volatile int g_kart_last = -1;
volatile uint32_t g_kart_eye_on;
volatile int g_kart_test_on, g_kart_test[3]; /* tools/fpv_test.sh: this eye in place of the table's */
/* the kart's EKartID (RaceInfo +0x30: KartInfo[8], 0x18 each, +0 its SKartDB, +0 the ID), -1 when it can't be read */
static int kart_id(Ctx *c, uint8_t *M, uint32_t cam) {
    uint32_t rm = ld32(c, M, ctx_gpr(c, 13) - 0x5c38), info = rm ? ld32(c, M, rm + 0x38) : 0;
    uint32_t n = mld(c, M, cam + 0xe0, 1);
    uint32_t db = ram_ptr(info) && n < 8 ? ld32(c, M, info + 0x30 + 0x18 * n) : 0;
    uint32_t id = ram_ptr(db) ? ld32(c, M, db) : 21;
    return id < 21 ? (int)id : -1;
}
/* DRIVER FPV's and KART FPV's eye from the driver's head, so every kart and character gets its own: measured
 * while the drivers are drawn (the start's fly-by, the respawn, any view but first person),
 * kept while first person hides them (their joints are not worked out then). KartBody +0x18:
 * the two DriverModels (ExModel: +0xc J3DModel **). J3DModel +0x4 J3DModelData (+0x2c joint
 * count), +0x24 the model's base matrix, +0x84 J3DMtxBuffer (+0xc the joints' world matrices).
 * Each driver's top joint in its own base's frame (up, forward); the front one's is the head,
 * averaged over the frames seen. Until one is seen, HEAD_UP / HEAD_FWD above the kart's centre. */
#define HEAD_FWD -10.0f
static uint32_t s_head_body[4];
static float s_head[4][2];
static int s_head_n[4];
/* the top joint of DriverModel d in its base's frame: false when it can't be read */
bool driver_top(Ctx *c, uint8_t *M, uint32_t d, float out[5]) { /* up, forward, joint, joints, base scale */
    uint32_t mp = ram_ptr(d) ? ld32(c, M, d + 0xc) : 0, m = ram_ptr(mp) ? ld32(c, M, mp) : 0;
    uint32_t md = ram_ptr(m) ? ld32(c, M, m + 0x4) : 0, mb = ram_ptr(m) ? ld32(c, M, m + 0x84) : 0;
    uint32_t w = ram_ptr(mb) ? ld32(c, M, mb + 0xc) : 0;
    int n = ram_ptr(md) ? ld16(c, M, md + 0x2c) : 0;
    if (!ram_ptr(w) || n < 2 || n > 160) return false;
    float base[12];
    ldfn(c, M, m + 0x24, base, 12);
    float up[3] = {base[1], base[5], base[9]}, fw[3] = {base[2], base[6], base[10]};
    float lu = up[0] * up[0] + up[1] * up[1] + up[2] * up[2], lf = fw[0] * fw[0] + fw[1] * fw[1] + fw[2] * fw[2];
    if (lu < 1e-6f || lf < 1e-6f) return false;
    lu = sqrtf(lu), lf = sqrtf(lf); /* world units along the base's axes, whatever its scale */
    out[3] = (float)n, out[4] = lu;
    bool any = false;
    for (int j = 0; j < n; j++) {
        float t[12];
        ldfn(c, M, w + (uint32_t)j * 48, t, 12);
        float p[3] = {t[3] - base[3], t[7] - base[7], t[11] - base[11]};
        float u = (p[0] * up[0] + p[1] * up[1] + p[2] * up[2]) / lu, f = (p[0] * fw[0] + p[1] * fw[1] + p[2] * fw[2]) / lf;
        if (!(u > -50 && u < 400 && f > -300 && f < 300)) return false; /* not posed yet, or stale */
        if (!any || u > out[0]) out[0] = u, out[1] = f, out[2] = (float)j, any = true;
    }
    return any;
}
static void head_measure(Ctx *c, uint8_t *M, unsigned view, uint32_t body) {
    if (s_head_body[view] != body) s_head_body[view] = body, s_head_n[view] = 0;
    float a[5], b[5], *h;
    bool ha = driver_top(c, M, ld32(c, M, body + 0x18), a), hb = driver_top(c, M, ld32(c, M, body + 0x1c), b);
    if (!ha && !hb) return;
    h = ha && hb ? (a[1] >= b[1] ? a : b) : ha ? a : b; /* the one in front drives */
    if (h[0] < 30) return;
    int n = s_head_n[view];
    float k = n < 30 ? 1.0f / (float)(n + 1) : 1.0f / 30;
    s_head[view][0] += (h[0] - s_head[view][0]) * (n ? k : 1), s_head[view][1] += (h[1] - s_head[view][1]) * (n ? k : 1);
    if (n == 0)
        rt_log("kart view: view %u's driver's head %.0f up, %.0f ahead of the kart's centre (joint %.0f of %.0f, base scale %.2f; the other %s %.0f up, %.0f ahead)",
               view, h[0], h[1], h[2], h[3], h[4], ha && hb ? "at" : "not read,", h == a ? b[0] : a[0], h == a ? b[1] : a[1]);
    s_head_n[view] = n + 1;
}
float g_fp_up = 0.0f, g_fp_fwd = 20.0f, g_fp_tilt = 0.0f, g_fp_look = 1000.0f; /* DRIVER FPV: eye above and ahead of the
                                                                 * head, radians down; look distance (the chase view sits
                                                                 * about 380 behind, 220 up) */
int screens(Ctx *c, uint8_t *M) {
    uint32_t rm = ld32(c, M, ctx_gpr(c, 13) - 0x5c38), info = rm ? ld32(c, M, rm + 0x38) : 0;
    return info ? (int16_t)ld16(c, M, info + 0x20) : 0;
}
static bool fp_screens(Ctx *c, uint8_t *M) { int n = screens(c, M); return n >= 1 && n <= 4; }
void v3_norm(float v[3]) {
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-6f) v[0] /= l, v[1] /= l, v[2] /= l;
}
/* C_MTXLookAt(cam +0x80, eye, up, at) natively, eye and at also stored as the camera's position
 * (+0x1e8) and look point (+0x1f4): rows right, up, back (eye - at), each with -dot(row, eye). */
static void cam_look_at2(Ctx *c, uint8_t *M, uint32_t cam, const float eye[3], const float at[3], const float up[3], bool pos) {
    float z[3] = {eye[0] - at[0], eye[1] - at[1], eye[2] - at[2]};
    v3_norm(z);
    float x[3] = {up[1] * z[2] - up[2] * z[1], up[2] * z[0] - up[0] * z[2], up[0] * z[1] - up[1] * z[0]};
    v3_norm(x);
    float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    const float *r[3] = {x, y, z};
    float m[12];
    for (int i = 0; i < 3; i++) {
        m[4 * i] = r[i][0], m[4 * i + 1] = r[i][1], m[4 * i + 2] = r[i][2];
        m[4 * i + 3] = -(r[i][0] * eye[0] + r[i][1] * eye[1] + r[i][2] * eye[2]);
    }
    for (int i = 0; i < 12; i++)
        if (!isfinite(m[i])) return;
    stfn(c, M, cam + 0x80, m, 12);
    if (!pos) return;
    stfn(c, M, cam + 0x1e8, eye, 3);
    stfn(c, M, cam + 0x1f4, at, 3);
}
static void cam_look_at(Ctx *c, uint8_t *M, uint32_t cam, const float eye[3], const float at[3], const float up[3]) {
    cam_look_at2(c, M, cam, eye, at, up, true);
}
uint64_t i_FUN_802b6bd4(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t cam = (uint32_t)a0;
    ctx_call_orig(c, g_native_addr[NAT_FUN_802b6bd4]);
    uint32_t body = ld32(c, M, cam);
    uint32_t view = mld(c, M, cam + 0xe3, 1);
    if (view == 0 && mld(c, M, cam + 0xe1, 1) == 0) bai_tick(c, M);
    int n = screens(c, M);
    if (view <= 3 && ram_ptr(body) && (mld(c, M, cam + 0xe1, 1) != 0 || !(g_first_person >> view & 1)))
        head_measure(c, M, view, body); /* its drivers are drawn */
    if (mld(c, M, cam + 0xe1, 1) != 0 || !IS_RAM(body) || view > 3 || n < 1 || n > 4) return 0;
    g_cam_frames[view]++;
    if (view == 0) {
        int k = kart_id(c, M, cam);
        if (k >= 0) g_kart_last = k;
    }
    bool back = g_look_back >> view & 1, fp = (g_first_person >> view & 1) && fp_screens(c, M);
    if (view) g_mirror_off = 8;
    else if (g_mirror_off) g_mirror_off--;
    if (view == 0 && g_hud_mirror) { /* the game's camera, kept when none of ours below changes it */
        float e[3], a[3], u[3];
        ldfn(c, M, cam + 0x1e8, e, 3);
        ldfn(c, M, cam + 0x1f4, a, 3);
        ldfn(c, M, cam + 0x90, u, 3);
        mirror_eye(c, M, body, e, a, u, MIRROR_TILT);
    }
    if (pan_step(view, cam) == 0 && !fp && !back && g_cam_height[view] == 1.0f && g_cam_dist[view] == 1.0f) return 0;
    float b[12];
    ldfn(c, M, body + 0x110, b, 12);
    float up[3] = {b[1], b[5], b[9]}, fw[3] = {b[2], b[6], b[10]}, eye[3], at[3];
    v3_norm(up);
    v3_norm(fw);
    if (fp) {
        bool kart = g_fp_kart >> view & 1;
        bool head = s_head_body[view] == body && s_head_n[view];
        float eu = (head ? s_head[view][0] : HEAD_UP) + (kart ? KART_UP + g_kart_up : g_fp_up);
        float ef = (head ? s_head[view][1] : HEAD_FWD) + (kart ? KART_FWD + g_kart_fwd : g_fp_fwd);
        float tilt = kart ? KART_TILT + g_kart_tilt : g_fp_tilt;
        int id = kart && g_kart_eye_on ? kart_id(c, M, cam) : -1;
        if (id >= 0) {
            bool t = g_kart_test_on;
            eu = (float)(t ? g_kart_test[0] : KART_EYE[id].up) + g_kart_add[id][0];
            ef = (float)(t ? g_kart_test[1] : KART_EYE[id].fwd) + g_kart_add[id][1];
            tilt = (float)(t ? g_kart_test[2] : KART_EYE[id].tilt) * 0.017453293f + g_kart_add[id][2];
        }
        float lf = back ? -g_fp_look : g_fp_look, ld = back ? 0 : g_fp_look * tanf(tilt);
        for (int i = 0; i < 3; i++) {
            eye[i] = b[4 * i + 3] + up[i] * eu + fw[i] * ef;
            at[i] = eye[i] + fw[i] * lf - up[i] * ld;
        }
        s_fp_cam[view] = cam;
        s_fp_body[view] = body;
    } else {
        /* the game's chase camera mirrored to the kart's front: position and look point
         * reflected across the plane through the kart square to its forward axis; its up kept */
        ldfn(c, M, cam + 0x1e8, eye, 3);
        ldfn(c, M, cam + 0x1f4, at, 3);
        ldfn(c, M, cam + 0x90, up, 3);
        float de = 0, da = 0, he = 0, ha = 0, ku[3] = {b[1], b[5], b[9]};
        v3_norm(ku);
        for (int i = 0; i < 3; i++) {
            de += (eye[i] - b[4 * i + 3]) * fw[i], da += (at[i] - b[4 * i + 3]) * fw[i];
            he += (eye[i] - b[4 * i + 3]) * ku[i], ha += (at[i] - b[4 * i + 3]) * ku[i];
        }
        if (back)
            for (int i = 0; i < 3; i++) eye[i] -= 2 * de * fw[i], at[i] -= 2 * da * fw[i];
        /* CAMERA HEIGHT: the camera's and its look point's heights above the kart scaled (lower
         * looks flatter), the camera never under CAM_MIN_UP above the kart (nor raised by it) */
        /* CAMERA DISTANCE: the camera's offset from the kart across its up axis scaled; the look point stays */
        float sd = g_cam_dist[view];
        if (sd != 1.0f)
            for (int i = 0; i < 3; i++) eye[i] = b[4 * i + 3] + he * ku[i] + (eye[i] - b[4 * i + 3] - he * ku[i]) * sd;
        float sc = g_cam_height[view];
        if (sc != 1.0f) {
            float he2 = he * sc;
            if (he2 < CAM_MIN_UP) he2 = he < CAM_MIN_UP ? he : CAM_MIN_UP;
            for (int i = 0; i < 3; i++) eye[i] += (he2 - he) * ku[i], at[i] += ha * (sc - 1) * ku[i];
        }
    }
    if (s_pan[view] != 0) {
        /* round the kart's up axis: first person turns the look point round the eye, the chase
         * camera both points round the kart. The up vector turns with them: the chase camera's
         * (its matrix's up row) leans toward where it looks, and left alone that lean would roll
         * a sideways view. */
        float ku[3] = {b[1], b[5], b[9]}, o[3], e[3], t[3];
        v3_norm(ku);
        for (int i = 0; i < 3; i++) o[i] = fp ? eye[i] : b[4 * i + 3], e[i] = eye[i] - o[i], t[i] = at[i] - o[i];
        v3_turn(e, ku, s_pan[view]);
        v3_turn(t, ku, s_pan[view]);
        v3_turn(up, ku, s_pan[view]);
        for (int i = 0; i < 3; i++) eye[i] = o[i] + e[i], at[i] = o[i] + t[i];
    }
    /* DRIVER FPV looks level, the chase camera and KART FPV down at the kart: the mirror, turned
     * round, would look that much further down than theirs */
    if (view == 0 && g_hud_mirror) mirror_eye(c, M, body, eye, at, up, fp && !(g_fp_kart & 1) ? MIRROR_TILT_DRIVER + g_fp_tilt : MIRROR_TILT);
    cam_look_at(c, M, cam, eye, at, up);
    return 0;
}
/* KartDrawer::drawKart / drawDriver(this, viewNo, stage) (FUN_801908b4 / FUN_80190a88, their
 * enableDrawing inlined): the kart (body, wheels, parts) and its two characters, skipped in the
 * view whose first-person camera looks from it while it still does (its mode is 0 and its target
 * the same body), so the player's own kart and hands don't fill the screen; the KART view keeps
 * the kart (not while looking back). KartDrawer +0 = kart
 * number; KartCam +0xe0 = target kart number, +0xe3 = view. Other views (the other player's
 * screen) and the shadow keep it. */
static bool fp_hidden(Ctx *c, uint8_t *M, uint32_t drawer, uint32_t view) {
    if (view > 3 || !(g_first_person >> view & 1)) return false;
    uint32_t cam = s_fp_cam[view];
    return cam && ld32(c, M, cam) == s_fp_body[view] && mld(c, M, cam + 0xe1, 1) == 0 &&
           mld(c, M, cam + 0xe3, 1) == view && mld(c, M, cam + 0xe0, 1) == ld32(c, M, drawer) && fp_screens(c, M);
}
uint64_t i_FUN_801908b4(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t v = (uint32_t)a1;
    bool kart = v < 4 && (g_fp_kart >> v & 1) && !(g_look_back >> v & 1);
    if (kart || !fp_hidden(c, M, (uint32_t)a0, v)) ctx_call_orig(c, g_native_addr[NAT_FUN_801908b4]);
    return 0;
}
uint64_t i_FUN_80190a88(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    if (!fp_hidden(c, M, (uint32_t)a0, (uint32_t)a1)) ctx_call_orig(c, g_native_addr[NAT_FUN_80190a88]);
    return 0;
}

/* DRIVER / KART FPV: a view whose first-person camera sits in this kart turns with its trick (trick.c:
 * r, the same turn as the kart's matrices, this frame's: the cameras ran before MakeKartDisp). Only the
 * view matrix (+0x80); the eye and look point stay the kart's for the next frame and the sound. */
void fp_cams_turn(Ctx *c, uint8_t *M, uint32_t body, const float r12[12]) {
    for (unsigned v = 0; v < 4; v++) {
        uint32_t cam = s_fp_cam[v];
        if (!(g_first_person >> v & 1) || !ram_ptr(cam) || s_fp_body[v] != body || ld32(c, M, cam) != body ||
            mld(c, M, cam + 0xe1, 1) != 0 || mld(c, M, cam + 0xe3, 1) != v || !fp_screens(c, M))
            continue;
        float e[3], a[3], u[3], e2[3], a2[3], u2[3];
        ldfn(c, M, cam + 0x1e8, e, 3);
        ldfn(c, M, cam + 0x1f4, a, 3);
        ldfn(c, M, cam + 0x90, u, 3);
        for (int i = 0; i < 3; i++) {
            const float *r = r12 + 4 * i;
            e2[i] = r[0] * e[0] + r[1] * e[1] + r[2] * e[2] + r[3];
            a2[i] = r[0] * a[0] + r[1] * a[1] + r[2] * a[2] + r[3];
            u2[i] = r[0] * u[0] + r[1] * u[1] + r[2] * u[2];
        }
        cam_look_at2(c, M, cam, e2, a2, u2, false);
    }
}
