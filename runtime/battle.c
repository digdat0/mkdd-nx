/* Single player battle mode (the − menu's MODS > SINGLE PLAYER BATTLE MODE): battles get computer
 * karts up to 4, which drive the arena's route (route.c) and play: item boxes, chasing, items, the
 * Shine, running away. The hooks here are natives (native.h NATIVE_LIST), called like native.c's. */
#include "native_impl.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "charpack.h"
#include "route.h"
#include "rt.h"

/* the AI's decisions in the log: only with LOGGING on GAME + BATTLE AI (two thirds of a battle's lines otherwise) */
#define BAI_LOGGING (g_log_mask & LOG_BATTLE)

/* RaceInfo::setRace(mode, karts, players, consoles, status) (FUN_8018c878, from
 * SceneMenu::setRaceData; the same code in Plus and RTGP). A battle (modes 4-7) gets one kart per
 * player. In single player battle mode (g_battle_cpus): 4 karts, and setRaceData's loop after the
 * players' karts (while fewer than gRaceInfo +0x1c) fills the rest with computer karts, as it does
 * for a Grand Prix. */
volatile uint32_t g_battle_cpus;
uint64_t i_FUN_8018c878(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    if (g_battle_cpus && a1 >= 4 && a1 <= 7 && (int32_t)a2 < 4) {
        rt_log("battle cpus: mode %u, %d players, 4 karts", (unsigned)a1, (int)(int32_t)a3);
        ctx_set_gpr(c, 5, 4);
    }
    ctx_call_orig(c, g_native_addr[NAT_FUN_8018c878]);
    return 0;
}

/* Course::loadCrsData (the same in Plus and RTGP): Course +0x4dc the CrsData, its +4 the course's
 * BOL. Each race tells route.c its course (ID +0x19, a hash of the 0x7c byte header). In single
 * player battle mode, with a route for this arena (route.c) and none of its own (+0x1a CL point count 0):
 * CrsData reads a copy of the header in our pool instead, its section offsets (+0x40..+0x6c, from
 * the header) moved to still reach the original's sections, with the route as its CL points
 * (0x20 each, after the header): CL groups (route.c), a loop or networks of them, one network for
 * each computer's kart (below), never joined. s_route_cpus then lets the computer's karts drive it
 * (FUN_802b1f60). */
#define RACE_INFO 0x803B145Cu /* gRaceInfo: +0x1c karts, +0x1e players */
#define RACE_MODE 0x803B1464u /* gRaceInfo +8 */
#define BOL_HEAD 0x7c
#define ROUTE_WIDTH 600.0f /* CL point scale: how far either side of the line the karts keep */
static bool s_route_cpus;
/* the route's CL points (s_cl_n, their file data from s_cl_mem + 0x80), its networks (s_nets,
 * network k from point s_net_start[k]) and each kart's: computers' karts one each, in turn
 * (kart players + i: network i, sharing only when there are fewer networks), -1 a player's.
 * s_cl_base: the game's CLPoint array (0x2c each), found from the first one seen. */
static float s_cl_pts[ROUTE_POINTS][3];
static int s_net_start[ROUTE_NETS + 1], s_nets, s_kart_net[8], s_cl_n;
static uint32_t s_cl_mem, s_cl_base;
static void bai_reset(void);
static void bai_graph(const int16_t *grp);
uint64_t i_loadCrsData__6CourseFb(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t crs = ld32(c, M, (uint32_t)a0 + 0x4dc), bol = IS_RAM(crs) ? ld32(c, M, crs + 4) : 0;
    uint32_t mode = ld32(c, M, RACE_MODE);
    bool battle = mode >= 4 && mode <= 7;
    s_route_cpus = false, s_nets = 0, s_cl_n = 0, s_cl_base = 0;
    bai_reset();
    if (IS_RAM(bol) && IS_RAM(bol + BOL_HEAD)) {
        uint32_t h = 2166136261u;
        for (uint32_t i = 0; i < BOL_HEAD; i++) h = (h ^ mld(c, M, bol + i, 1)) * 16777619u;
        route_course(mld(c, M, bol + 0x19, 1), h, battle);
        float (*pts)[3] = s_cl_pts;
        static int16_t grp[ROUTE_POINTS];
        static uint32_t mem;
        int karts = (int16_t)ld16(c, M, RACE_INFO + 0x1c), players = (int16_t)ld16(c, M, RACE_INFO + 0x1e);
        int want = karts - players < 1 ? 1 : karts - players > ROUTE_NETS ? ROUTE_NETS : karts - players;
        int n = g_battle_cpus && battle && ld16(c, M, bol + 0x1a) == 0 ? route_load(pts, grp, ROUTE_POINTS, want, s_net_start, &s_nets) : 0;
        if (n && !mem) mem = charpack_reserve(0x80 + 0x20 * ROUTE_POINTS);
        if (n && mem) {
            s_cl_mem = mem, s_cl_n = n;
            for (int k = 0; k < 8; k++) {
                s_kart_net[k] = k >= players && k < karts && s_nets ? (k - players) % s_nets : -1;
                if (s_kart_net[k] >= 0) rt_log("battle cpus: kart %d drives network %d of the %d loaded", k, s_kart_net[k] + 1, s_nets);
            }
            for (uint32_t i = 0; i < BOL_HEAD; i += 4) mst(c, M, mem + i, 4, ld32(c, M, bol + i));
            for (uint32_t o = 0x40; o <= 0x6c; o += 4) mst(c, M, mem + o, 4, ld32(c, M, bol + o) + bol - mem);
            mst(c, M, mem + 0x1a, 2, (uint32_t)n);
            mst(c, M, mem + 0x44, 4, 0x80);
            int group = -1;
            bool start = false;
            for (int i = 0; i < n; i++) {
                uint32_t p = mem + 0x80 + 0x20 * (uint32_t)i;
                for (uint32_t k = 0; k < 0x20; k += 4) mst(c, M, p + k, 4, 0);
                stf(c, M, p, pts[i][0]), stf(c, M, p + 4, pts[i][1]), stf(c, M, p + 8, pts[i][2]);
                mst(c, M, p + 0xe, 2, (uint16_t)grp[i]); /* groupID: a group's first and last points, -1 between */
                stf(c, M, p + 0x10, ROUTE_WIDTH);
                if (grp[i] >= 0 && (start = !start)) group++; /* (the game's own count: a group ID opens a group, the next closes it) */
                mst(c, M, p + 0x16, 1, (uint32_t)group & 0xff); /* uniqueGroupID */
            }
            mst(c, M, crs + 4, 4, mem);
            s_route_cpus = true;
            bai_graph(grp);
            rt_log("battle cpus: course %#x gets a route of %d points", (unsigned)mld(c, M, bol + 0x19, 1), n);
        }
    }
    ctx_call_orig(c, g_native_addr[NAT_loadCrsData__6CourseFb]);
    return 0;
}
/* KartCtrl::IsMiniGame (battle modes 4-7). A battle sets up no computer drivers (RivalKart) and
 * no route targets (KartTarget). With a route (s_route_cpus) the kart setup and reset
 * (FUN_8029f660, FUN_8029fb84) and KartTarget's Init, DoSearchTarget and DoSearchRescueTarget
 * (FUN_802be4cc, FUN_802be518, FUN_802be8d4) are told it is not one, by their call's return
 * address; everything else (handling, camera, respawn, balloons) still sees the battle. */
static bool lr_in(uint32_t lr, uint32_t f, uint32_t size) { return lr - f < size; }
uint64_t i_FUN_802b1f60(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t lr = (uint32_t)RD(c, 0x1020, 4);
    ctx_call_orig(c, g_native_addr[NAT_FUN_802b1f60]);
    uint32_t r = ctx_gpr(c, 3);
    if (r && s_route_cpus &&
        (lr_in(lr, 0x8029F660u, 1316) || lr_in(lr, 0x8029FB84u, 444) || lr_in(lr, 0x802BE4CCu, 76) ||
         lr_in(lr, 0x802BE518u, 956) || lr_in(lr, 0x802BE8D4u, 64)))
        r = 0;
    return r;
}

/* One network for each computer's kart (s_kart_net). The game puts a kart on the route where its
 * KartTarget (KartCtrl +0x1c0 [kart], +0 the CL point it is at) is: set up on the route's first
 * point (network 0's) by KartTarget::Init, given out by GetKartCenterPtr (to the kart's
 * RivalBodyCtrl, which steers from it), moved on to a neighbouring point (so it stays on the
 * network it is on) and set by DoSearchRescueTarget on a respawn, with RivalBodyCtrl's
 * comebackRescure. Each of those is kept on the kart's network: another network's point becomes
 * the nearest of the kart's own to the kart (KartCtrl +0xa0 [kart] the body, its matrix's w column
 * the position) or, on a respawn, to that point. */
static int cl_index(Ctx *c, uint8_t *M, uint32_t cl) {
    if (!ram_ptr(cl)) return -1;
    uint32_t d = ld32(c, M, cl + 4) - (s_cl_mem + 0x80);
    return d % 0x20 == 0 && d / 0x20 < (uint32_t)s_cl_n ? (int)(d / 0x20) : -1;
}
/* any: the nearest point even when cl is on the kart's network already (the start) */
static uint32_t net_point(Ctx *c, uint8_t *M, int kart, uint32_t cl, const float *ref, bool any) {
    if (!s_route_cpus || s_nets < 2 || kart < 0 || kart > 7 || s_kart_net[kart] < 0) return cl;
    int i = cl_index(c, M, cl), k = s_kart_net[kart];
    if (i < 0 || (!any && i >= s_net_start[k] && i < s_net_start[k + 1])) return cl;
    if (!s_cl_base) s_cl_base = cl - 0x2c * (uint32_t)i;
    const float *p = ref ? ref : s_cl_pts[i];
    int best = -1;
    float bd = 0;
    for (int j = s_net_start[k]; j < s_net_start[k + 1]; j++) {
        float x = s_cl_pts[j][0] - p[0], y = s_cl_pts[j][1] - p[1], z = s_cl_pts[j][2] - p[2], d = x * x + y * y + z * z;
        if (best < 0 || d < bd) best = j, bd = d;
    }
    uint32_t to = s_cl_base + 0x2c * (uint32_t)best;
    if (best < 0 || cl_index(c, M, to) != best) return cl; /* not the array it should be: left alone */
    if (to == cl) return cl;
    rt_log("battle cpus: kart %d onto network %d at point %d", kart, k + 1, best);
    return to;
}
static void kart_pos(Ctx *c, uint8_t *M, uint32_t kc, int kart, float p[3]) {
    uint32_t body = ld32(c, M, kc + 0xa0 + 4 * (uint32_t)kart);
    p[0] = ldf(c, M, body + 0x11c), p[1] = ldf(c, M, body + 0x12c), p[2] = ldf(c, M, body + 0x13c);
}
static int target_kart(Ctx *c, uint8_t *M, uint32_t kc, uint32_t target) {
    for (int k = 0; k < 8; k++)
        if (ld32(c, M, kc + 0x1c0 + 4 * (uint32_t)k) == target) return k;
    return -1;
}
/* KartTarget's point kept on its kart's network */
static void target_fix(Ctx *c, uint8_t *M, uint32_t kc, int kart, uint32_t target, bool any) {
    if (kart < 0 || !ram_ptr(target)) return;
    float p[3];
    kart_pos(c, M, kc, kart, p);
    uint32_t cl = ld32(c, M, target), to = net_point(c, M, kart, cl, isfinite(p[0]) && isfinite(p[1]) && isfinite(p[2]) ? p : NULL, any);
    if (to != cl) mst(c, M, target, 4, to);
}
/* KartCtrl::GetKartCenterPtr(kart) */
uint64_t i_FUN_802b2174(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t kc = (uint32_t)a0, kart = (uint32_t)a1, target = ld32(c, M, kc + 0x1c0 + 4 * kart);
    if (s_route_cpus && s_nets > 1 && kart < 8) target_fix(c, M, kc, (int)kart, target, false);
    return ld32(c, M, target);
}
/* KartTarget::Init */
uint64_t i_FUN_802be4cc(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    ctx_call_orig(c, g_native_addr[NAT_FUN_802be4cc]);
    if (s_route_cpus && s_nets > 1) {
        uint32_t kc = ld32(c, M, (uint32_t)ctx_gpr(c, 13) - 0x4e98);
        target_fix(c, M, kc, target_kart(c, M, kc, (uint32_t)a0), (uint32_t)a0, true); /* every kart from its nearest point, not network 0's first */
    }
    return 0;
}
/* KartTarget::DoSearchRescueTarget(point) */
uint64_t i_FUN_802be8d4(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    if (s_route_cpus && s_nets > 1) {
        uint32_t kc = ld32(c, M, (uint32_t)ctx_gpr(c, 13) - 0x4e98);
        ctx_set_gpr(c, 4, net_point(c, M, target_kart(c, M, kc, (uint32_t)a0), (uint32_t)a1, NULL, false));
    }
    ctx_call_orig(c, g_native_addr[NAT_FUN_802be8d4]);
    return 0;
}
/* RivalBodyCtrl::comebackRescure(point, b) (+4 RivalKart, +0x38 the kart) */
uint64_t i_comebackRescure__13RivalBodyCtrlFP7CLPointb(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    if (s_route_cpus && s_nets > 1) {
        uint32_t rival = ld32(c, M, (uint32_t)a0 + 4);
        ctx_set_gpr(c, 4, net_point(c, M, ram_ptr(rival) ? (int)ld32(c, M, rival + 0x38) : -1, (uint32_t)a1, NULL, false));
    }
    ctx_call_orig(c, g_native_addr[NAT_comebackRescure__13RivalBodyCtrlFP7CLPointb]);
    return 0;
}

/* The battle as the computer's karts see it (bai_tick, each frame; bai_state). With LOGGING on GAME +
 * BATTLE AI it is also logged: every BAI_EVERY frames, and each item change.
 * RaceMgr (r13 - 0x5c38): +0x38 RaceInfo (+0x1c karts, +0x1e players: karts 0.. are the players'),
 * +0x48 KartChecker[8]: +0x2 kart number, +0x5c position, +0x8c flags (4: out of the battle),
 * +0x90 balloons, +0xa6 bomb points, +0xa8 Shine frames left to win.
 * GeoRabbitMarkSupervisor (r13 - 0x5b3c): +0x3c the Shine (+0x4 position, +0x1ae holder, -1 none).
 * ItemObjMgr (r13 - 0x54e0): +0x2cc the item each kart's driver holds (ItemObj *[8][2], +0x7c kind).
 * Item boxes (GeoItemBox, from their calc): +0x4 position, +0x17c mode (doMode: 0, 1, 3 run
 * something, 2 nothing). */
#define BAI_EVERY 120
#define BAI_BOXES 48
static struct { uint32_t obj, seen; float p[3]; int mode; } s_box[BAI_BOXES];
static int s_nbox, s_bai_item[8][2];
static uint32_t s_bai_frame;
static bool s_bai_boxes_said;
static const char *const ITEM_NAME[22] = {"green", "biggreen", "red", "banana", "bigbanana", "mushroom", "star", "chomp",
                                          "bomb", "fireball", "lightning", "egg", "goldmush", "spiny", "heart", "fakebox",
                                          "16", "3green", "3mush", "3red", "bombs", "fireballs"};
static const char *item_name(int k) { return k == -1 ? "-" : k >= 0 && k < 22 ? ITEM_NAME[k] : "?"; }
static float bai_rand(void);
static void bai_state(Ctx *c, uint8_t *M, uint32_t mode, uint32_t rm, int karts, int players, uint32_t shine);
void bai_tick(Ctx *c, uint8_t *M) {
    uint32_t mode = ld32(c, M, RACE_MODE), r13 = (uint32_t)ctx_gpr(c, 13);
    if (!g_battle_cpus || mode < 4 || mode > 7) return;
    uint32_t rm = ld32(c, M, r13 - 0x5c38), info = ram_ptr(rm) ? ld32(c, M, rm + 0x38) : 0;
    if (!ram_ptr(info)) return;
    int karts = (int16_t)ld16(c, M, info + 0x1c), players = (int16_t)ld16(c, M, info + 0x1e);
    if (karts < 1 || karts > 8) return;
    uint32_t sup = ld32(c, M, r13 - 0x5b3c), shine = ram_ptr(sup) ? ld32(c, M, sup + 0x3c) : 0;
    uint32_t items = ld32(c, M, r13 - 0x54e0), now = ++s_bai_frame;
    bai_state(c, M, mode, rm, karts, players, shine);
    if (!BAI_LOGGING) return; /* the rest only logs */
    bool all = now % BAI_EVERY == 0;
    if (all) {
        int modes[4] = {0}, live = 0;
        for (int i = 0; i < s_nbox; i++)
            if (now - s_box[i].seen < 4) live++, modes[s_box[i].mode & 3]++;
        char sh[64] = "";
        if (ram_ptr(shine))
            snprintf(sh, sizeof sh, ", shine holder %d at (%.0f %.0f %.0f)", (int16_t)ld16(c, M, shine + 0x1ae), ldf(c, M, shine + 4),
                     ldf(c, M, shine + 8), ldf(c, M, shine + 0xc));
        rt_log("battle ai: frame %u, mode %u, %d karts (%d players), boxes %d (modes %d %d %d %d)%s", (unsigned)now, (unsigned)mode,
               karts, players, live, modes[0], modes[1], modes[2], modes[3], sh);
        if (!s_bai_boxes_said && s_nbox) {
            s_bai_boxes_said = true;
            for (int i = 0; i < s_nbox; i++)
                rt_log("battle ai: box %d at (%.0f %.0f %.0f) mode %d", i, s_box[i].p[0], s_box[i].p[1], s_box[i].p[2], s_box[i].mode);
        }
    }
    for (int k = 0; k < karts; k++) {
        uint32_t chk = ld32(c, M, rm + 0x48 + 4 * (uint32_t)k);
        if (!ram_ptr(chk)) continue;
        int held[2];
        for (uint32_t d = 0; d < 2; d++) {
            uint32_t it = ram_ptr(items) ? ld32(c, M, items + 0x2cc + 8 * (uint32_t)k + 4 * d) : 0;
            held[d] = ram_ptr(it) ? (int)ld32(c, M, it + 0x7c) : -1;
            if (held[d] != s_bai_item[k][d])
                rt_log("battle ai: frame %u, kart %d driver %u: %s -> %s", (unsigned)now, k, (unsigned)d, item_name(s_bai_item[k][d]),
                       item_name(held[d]));
            s_bai_item[k][d] = held[d];
        }
        if (all)
            rt_log("battle ai:   kart %d%s no %d at (%.0f %.0f %.0f) balloons %d bomb points %d shine left %d flags %#x items %s %s", k,
                   k < players ? " (player)" : "", (int16_t)ld16(c, M, chk + 2), ldf(c, M, chk + 0x5c), ldf(c, M, chk + 0x60),
                   ldf(c, M, chk + 0x64), (int16_t)ld16(c, M, chk + 0x90), (int16_t)ld16(c, M, chk + 0xa6),
                   (int16_t)ld16(c, M, chk + 0xa8), (unsigned)ld16(c, M, chk + 0x8c), item_name(held[0]), item_name(held[1]));
    }
}
/* GeoItemBox::calc, each box each frame: where the boxes are and their state */
uint64_t i_calc__10GeoItemBoxFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    ctx_call_orig(c, g_native_addr[NAT_calc__10GeoItemBoxFv]);
    uint32_t box = (uint32_t)a0;
    if (!g_battle_cpus || !ram_ptr(box)) return 0;
    int i = 0;
    while (i < s_nbox && s_box[i].obj != box) i++;
    if (i == s_nbox) {
        if (s_nbox == BAI_BOXES) return 0;
        s_nbox++;
    }
    s_box[i].obj = box, s_box[i].seen = s_bai_frame, s_box[i].mode = (int)ld32(c, M, box + 0x17c);
    ldfn(c, M, box + 4, s_box[i].p, 3);
    return 0;
}
/* FUN_80226f1c(controller, CL point, random): which of the point's next points (its index) a
 * controller goes to, at random among those not marked as a shortcut. From
 * CLPointControllerRivalKart::findNextPointNo (a computer's kart: controller +0x28 RivalBodyCtrl,
 * its +4 RivalKart, +0x38 the kart's number): logged. CLPoint +4 its file data (position first),
 * +0x18 the next points. */
static char s_why[96]; /* bai_branch's reason, for the log */
static int bai_branch(Ctx *c, uint8_t *M, int kart, uint32_t pt, int game);
uint64_t i_FUN_80226f1c(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t lr = (uint32_t)RD(c, 0x1020, 4);
    ctx_call_orig(c, g_native_addr[NAT_FUN_80226f1c]);
    uint32_t r = (uint32_t)ctx_gpr(c, 3), pt = (uint32_t)a1;
    if (!g_battle_cpus || !s_route_cpus || !lr_in(lr, 0x802462E4u, 88) || !ram_ptr(pt)) return r;
    uint32_t body = ld32(c, M, (uint32_t)a0 + 0x28), rival = ram_ptr(body) ? ld32(c, M, body + 4) : 0;
    int kart = ram_ptr(rival) ? (int)ld32(c, M, rival + 0x38) : -1, ways = 0;
    for (uint32_t i = 0; i < 4; i++) ways += ld32(c, M, pt + 0x18 + 4 * i) != 0;
    if (ways < 2 || r >= 4) return r;
    int pick = bai_branch(c, M, kart, pt, (int)r);
    uint32_t data = ld32(c, M, pt + 4), next = ld32(c, M, pt + 0x18 + 4 * (uint32_t)pick);
    uint32_t nd = ram_ptr(next) ? ld32(c, M, next + 4) : 0;
    if (ram_ptr(data) && ram_ptr(nd))
        if (BAI_LOGGING) rt_log("battle ai: frame %u, kart %d at %d (%.0f %.0f %.0f): %d ways, game %u, took %d to (%.0f %.0f %.0f):%s", (unsigned)s_bai_frame,
               kart, cl_index(c, M, pt), ldf(c, M, data), ldf(c, M, data + 4), ldf(c, M, data + 8), ways, (unsigned)r, pick, ldf(c, M, nd),
               ldf(c, M, nd + 4), ldf(c, M, nd + 8), s_why);
    return (uint64_t)pick;
}

/* Goals: each computer's kart has one each frame (bai_tick): Shine Thief (mode 7) the free
 * Shine, its holder, or away from the others while it holds it; Balloon Battle and Bob-omb Blast
 * (4, 6) the nearest item box nobody took lately while it has no item, else the nearest kart. At a
 * junction (FUN_80226f1c) it takes the part leading there soonest along its network (s_jd: each
 * network's shortest junction-to-junction distances, s_part: its parts), or when fleeing the part
 * keeping furthest from the others; at times (1 - its focus) the game's random pick instead.
 * Kart positions and forward: KartCtrl +0xa0 [kart] the body, its matrix's w and z columns. Dead:
 * KartChecker +0x8c bit 4. */
#define BAI_J 64          /* junctions a network may have (route.c NET_JUNCTIONS) */
#define BAI_INF 1e30f
#define BAI_BOX_GONE 360  /* frames a box counts as taken after a kart came within BAI_BOX_NEAR */
#define BAI_BOX_NEAR 400.0f
enum { GOAL_NONE, GOAL_CHASE, GOAL_FLEE };
static struct { int16_t s, e; uint8_t a, b; float len; } s_part[ROUTE_POINTS / 2];
static int s_nparts;
static int16_t s_pt_part[ROUTE_POINTS];
static float s_pt_off[ROUTE_POINTS];
static float s_jd[ROUTE_NETS][BAI_J][BAI_J];
static bool s_net_graph[ROUTE_NETS];
static struct { int kind, target; float p[3]; } s_goal[8];
static float s_kpos[8][3], s_kfw[8][3], s_focus[8];
static bool s_alive[8];
static int s_balloons[8];
static int s_karts_now, s_players_now, s_shine_holder = -1;
static uint32_t s_mode_now; /* gRaceInfo +8 this frame: 4 balloons, 6 Bob-omb, 7 Shine Thief */
/* Shine Thief, kart k without the Shine: after it (the Shine or its holder) all out. At junctions
 * it always takes the best way, it leaves the route for it from further and at wider angles
 * (BAI_HUNT, BAI_HUNT_KEEP), keeps its shells for the holder and boosts to catch either. */
static bool bai_hunt(int k) { return s_mode_now == 7 && s_goal[k].kind == GOAL_CHASE; }
static uint32_t s_box_gone[BAI_BOXES];
static uint64_t s_bai_rnd = 0x9e3779b97f4a7c15ull;
static float bai_rand(void) { /* 0 to 1 */
    s_bai_rnd ^= s_bai_rnd << 13, s_bai_rnd ^= s_bai_rnd >> 7, s_bai_rnd ^= s_bai_rnd << 17;
    return (float)(s_bai_rnd >> 40) / (float)(1u << 24);
}
static float dist3(const float *a, const float *b) {
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return sqrtf(x * x + y * y + z * z);
}
/* each network's parts (a junction's ID on a part's first and last points, route.c) and the
 * shortest distances between its junctions, parts driven one way */
static void bai_graph(const int16_t *grp) {
    s_nparts = 0;
    for (int k = 0; k < s_nets && k < ROUTE_NETS; k++) {
        bool ok = true;
        int base = BAI_J * k, first = s_nparts;
        for (int i = s_net_start[k]; ok && i < s_net_start[k + 1];) {
            int j = i + 1;
            while (j < s_net_start[k + 1] && grp[j] < 0) j++;
            int a = grp[i] - base, b = j < s_net_start[k + 1] ? grp[j] - base : -1;
            if (a < 0 || a >= BAI_J || b < 0 || b >= BAI_J || s_nparts == ROUTE_POINTS / 2) { ok = false; break; }
            float len = 0;
            for (int q = i; q <= j; q++) {
                if (q > i) len += dist3(s_cl_pts[q], s_cl_pts[q - 1]);
                s_pt_part[q] = (int16_t)s_nparts, s_pt_off[q] = len;
            }
            s_part[s_nparts].s = (int16_t)i, s_part[s_nparts].e = (int16_t)j, s_part[s_nparts].a = (uint8_t)a,
            s_part[s_nparts].b = (uint8_t)b, s_part[s_nparts].len = len, s_nparts++;
            i = j + 1;
        }
        s_net_graph[k] = ok;
        if (!ok) { s_nparts = first; continue; } /* not a network (a loop): the game's pick there */
        float (*d)[BAI_J] = s_jd[k];
        for (int x = 0; x < BAI_J; x++)
            for (int y = 0; y < BAI_J; y++) d[x][y] = x == y ? 0 : BAI_INF;
        for (int p = first; p < s_nparts; p++)
            if (s_part[p].len < d[s_part[p].a][s_part[p].b]) d[s_part[p].a][s_part[p].b] = s_part[p].len;
        for (int m = 0; m < BAI_J; m++)
            for (int x = 0; x < BAI_J; x++)
                if (d[x][m] < BAI_INF)
                    for (int y = 0; y < BAI_J; y++)
                        if (d[x][m] + d[m][y] < d[x][y]) d[x][y] = d[x][m] + d[m][y];
        rt_log("battle ai: network %d: %d parts", k + 1, s_nparts - first);
    }
}
/* the point of network k nearest p */
static int bai_nearest(int k, const float *p) {
    int best = -1;
    float bd = BAI_INF;
    for (int j = s_net_start[k]; j < s_net_start[k + 1]; j++) {
        float d = dist3(s_cl_pts[j], p);
        if (d < bd) bd = d, best = j;
    }
    return best;
}
/* which of CL point pt's next points (0-3) kart takes: game the game's own pick. s_why: how it
 * chose, for the log (no goal, not a network, the random share, the goal off the network, or
 * each next point's index:score, x one not starting a part of the kart's network) */
static int bai_branch(Ctx *c, uint8_t *M, int kart, uint32_t pt, int game) {
    snprintf(s_why, sizeof s_why, "no goal");
    if (kart < 0 || kart > 7 || s_kart_net[kart] < 0 || s_goal[kart].kind == GOAL_NONE) return game;
    int k = s_kart_net[kart];
    snprintf(s_why, sizeof s_why, "%s", k < ROUTE_NETS && s_net_graph[k] ? "random" : "no map");
    if (k >= ROUTE_NETS || !s_net_graph[k] || (!bai_hunt(kart) && bai_rand() > s_focus[kart])) return game;
    int gi = s_goal[kart].kind == GOAL_CHASE ? bai_nearest(k, s_goal[kart].p) : -1;
    snprintf(s_why, sizeof s_why, "goal off");
    if (s_goal[kart].kind == GOAL_CHASE && (gi < 0 || s_pt_part[gi] < 0)) return game;
    s_why[0] = 0;
    int best = game;
    float bscore = s_goal[kart].kind == GOAL_CHASE ? BAI_INF : -1;
    for (int i = 0; i < 4; i++) {
        uint32_t np = ld32(c, M, pt + 0x18 + 4 * (uint32_t)i);
        if (!np) continue;
        int ci = cl_index(c, M, np), pc = ci >= s_net_start[k] && ci < s_net_start[k + 1] ? s_pt_part[ci] : -1;
        size_t w = strlen(s_why);
        if (pc < 0 || s_part[pc].s != ci) {
            snprintf(s_why + w, sizeof s_why - w, " %d:x", ci);
            continue;
        }
        float score;
        if (s_goal[kart].kind == GOAL_CHASE) {
            int pg = s_pt_part[gi];
            score = pc == pg ? s_pt_off[gi] : s_part[pc].len + s_jd[k][s_part[pc].b][s_part[pg].a] + s_pt_off[gi];
            snprintf(s_why + w, sizeof s_why - w, " %d:%.0f", ci, score);
            if (score < bscore) bscore = score, best = i;
        } else { /* away: the part's middle and end, furthest from the nearest of the others */
            const float *mid = s_cl_pts[(s_part[pc].s + s_part[pc].e) / 2], *end = s_cl_pts[s_part[pc].e];
            score = BAI_INF;
            for (int o = 0; o < s_karts_now; o++)
                if (o != kart && s_alive[o]) {
                    float d = fminf(dist3(mid, s_kpos[o]), dist3(end, s_kpos[o]));
                    if (d < score) score = d;
                }
            snprintf(s_why + w, sizeof s_why - w, " %d:away %.0f", ci, score);
            if (score > bscore) bscore = score, best = i;
        }
    }
    return best;
}
/* the nearest living kart to kart, -1 none */
static int bai_nearest_kart(int kart, float *dist) {
    int best = -1;
    float bd = BAI_INF;
    for (int o = 0; o < s_karts_now; o++)
        if (o != kart && s_alive[o]) {
            float d = dist3(s_kpos[o], s_kpos[kart]);
            if (d < bd) bd = d, best = o;
        }
    if (dist) *dist = bd;
    return best;
}
/* each computer's kart's goal this frame (from bai_tick: s_kpos, s_alive, s_box up to date) */
static void bai_goals(uint32_t mode, uint32_t now) {
    for (int k = s_players_now; k < s_karts_now; k++) {
        s_goal[k].kind = GOAL_NONE, s_goal[k].target = -1;
        if (!s_alive[k] || s_kart_net[k] < 0) continue;
        if (mode == 7) {
            int h = s_shine_holder;
            if (h == k) s_goal[k].kind = GOAL_FLEE;
            else if (h >= 0 && h < s_karts_now) s_goal[k].kind = GOAL_CHASE, s_goal[k].target = h, memcpy(s_goal[k].p, s_kpos[h], 12);
            else if (h < 0 && isfinite(s_goal[k].p[0])) s_goal[k].kind = GOAL_CHASE; /* p: the Shine, set by bai_tick */
            continue;
        }
        bool empty = s_bai_item[k][0] < 0 && s_bai_item[k][1] < 0;
        if (mode == 4 && empty && s_balloons[k] == 1) { /* the last balloon, nothing to fight with: away from a kart close by */
            float od;
            if (bai_nearest_kart(k, &od) >= 0 && od < 2500) {
                s_goal[k].kind = GOAL_FLEE;
                continue;
            }
        }
        int box = -1;
        float bd = BAI_INF;
        for (int i = 0; empty && i < s_nbox; i++)
            if (now - s_box[i].seen < 4 && (int32_t)(s_box_gone[i] - now) <= 0) {
                float d = dist3(s_box[i].p, s_kpos[k]);
                if (d < bd) bd = d, box = i;
            }
        if (box >= 0) {
            s_goal[k].kind = GOAL_CHASE, memcpy(s_goal[k].p, s_box[box].p, 12);
            continue;
        }
        int o = bai_nearest_kart(k, NULL);
        if (o >= 0) s_goal[k].kind = GOAL_CHASE, s_goal[k].target = o, memcpy(s_goal[k].p, s_kpos[o], 12);
    }
}

/* Using items on purpose: RivalItemCtrl (+4 RivalKart, +0x38 the kart) runs the item its kart's
 * driver holds (doItemCtrl: +0x34 bit 2 one being run, +0x3c it, ItemObj +0x7c its kind) through
 * that kind's useItem function: it waits until +0x30 (frames, counted up each call) passes +0x38
 * (a random wait), then throws forward with +0x34 bit 4 (setThrowFlg's random pick), else drops it
 * behind once another kart is near or +0x30 passes 360 (+0x40: a triple's at once); mushrooms and
 * Stars on a straight bit of the route. For each computer's kart in a battle, before the game's
 * own: an item waits (+0x38 held high) for a kart in front in range (bit 4 set, the wait ended),
 * one close behind (bit 4 clear, +0x30 past 360), or, a boost, its chase target ahead or a kart
 * closing in; BAI_HOLD frames without one, the game's own timing again. */
#define BAI_HOLD 600
#define BAI_REACT 20 /* frames an item is held at least */
#define BAI_WAIT 100000u
static uint32_t s_hold_item[8], s_hold_frames[8];
/* the nearest living kart in kart's cone (cos of its half angle; < 0: behind) within range */
static int bai_in_cone(int kart, float cone, float range, float *dist) {
    int best = -1;
    float bd = range;
    for (int o = 0; o < s_karts_now; o++) {
        if (o == kart || !s_alive[o]) continue;
        float d[3] = {s_kpos[o][0] - s_kpos[kart][0], s_kpos[o][1] - s_kpos[kart][1], s_kpos[o][2] - s_kpos[kart][2]};
        float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (l < 1 || l >= bd) continue;
        float cs = (d[0] * s_kfw[kart][0] + d[1] * s_kfw[kart][1] + d[2] * s_kfw[kart][2]) / l;
        if (cone >= 0 ? cs > cone : cs < cone) bd = l, best = o;
    }
    if (dist) *dist = bd;
    return best;
}
static void bai_item(Ctx *c, uint8_t *M, uint32_t ctrl, int k) {
    uint32_t flags = ld32(c, M, ctrl + 0x34), item = ld32(c, M, ctrl + 0x3c);
    if (!(flags & 2) || !ram_ptr(item)) { s_hold_item[k] = 0; return; }
    if (item != s_hold_item[k]) s_hold_item[k] = item, s_hold_frames[k] = 0;
    uint32_t held = ++s_hold_frames[k];
    if (held > BAI_HOLD) {
        if (held == BAI_HOLD + 1) mst(c, M, ctrl + 0x38, 4, 0); /* the game's own from now */
        return;
    }
    int kind = (int)ld32(c, M, item + 0x7c), front = -1, back = -1;
    float fd = 0, bd = 0;
    bool boost = false, drop = false;
    switch (kind) {
    case 0: case 1: case 9: case 17: case 21: /* green shells (Bowser's, triple), fireballs */
        front = bai_in_cone(k, 0.97f, 3500, &fd), back = bai_in_cone(k, -0.85f, 2000, &bd); break;
    case 2: case 11: case 19: /* red shells, triple reds, Yoshi's egg: they home in */
        front = bai_in_cone(k, 0.75f, 7000, &fd), back = bai_in_cone(k, -0.85f, 1500, &bd); break;
    case 8: case 20: /* bob-omb (Bob-omb Blast's: 20): lands a way ahead */
        front = bai_in_cone(k, 0.9f, 2500, &fd), back = bai_in_cone(k, -0.85f, 1200, &bd); break;
    case 3: case 4: case 15: /* bananas, fake box: mostly dropped */
        front = bai_in_cone(k, 0.95f, 1500, &fd), back = bai_in_cone(k, -0.8f, 1800, &bd), drop = true; break;
    case 5: case 6: case 12: case 18: /* mushrooms, Star: catch the target or get away */
        boost = true; break;
    default: return; /* lightning, Chain Chomp, Spiny, heart: the game's own */
    }
    if (held < BAI_REACT) { mst(c, M, ctrl + 0x38, 4, BAI_WAIT); return; }
    const char *use = NULL;
    int at = -1;
    float ad = 0;
    int holder = bai_hunt(k) ? s_goal[k].target : -1; /* the Shine's holder: shells for it only */
    if (holder >= 0 && !boost && !drop) {
        if (front != holder) front = -1;
        if (back != holder) back = -1;
    }
    if (boost) {
        int t = s_goal[k].target, near = bai_in_cone(k, -0.7f, 1500, &bd);
        float d[3], l = 0;
        bool free_shine = bai_hunt(k) && t < 0 && isfinite(s_goal[k].p[0]); /* the Shine on the ground */
        if ((t >= 0 && s_alive[t]) || free_shine) {
            const float *q = free_shine ? s_goal[k].p : s_kpos[t];
            for (int i = 0; i < 3; i++) d[i] = q[i] - s_kpos[k][i], l += d[i] * d[i];
            l = sqrtf(l);
        }
        bool ahead = ((t >= 0 && s_alive[t]) || free_shine) && l > 1 && l < 3000 &&
                     (d[0] * s_kfw[k][0] + d[1] * s_kfw[k][1] + d[2] * s_kfw[k][2]) / l > 0.9f;
        if (ahead) use = "catch", at = t, ad = l;
        else if (near >= 0 && (s_goal[k].kind == GOAL_FLEE || kind == 6)) use = "escape", at = near, ad = bd;
        if (use) mst(c, M, ctrl + 0x38, 4, 0);
    } else if (front >= 0 && (!drop || back < 0)) {
        use = "forward", at = front, ad = fd;
        mst(c, M, ctrl + 0x34, 4, flags | 4), mst(c, M, ctrl + 0x38, 4, 0), mst(c, M, ctrl + 0x40, 1, 0);
    } else if (back >= 0) {
        use = "back", at = back, ad = bd;
        mst(c, M, ctrl + 0x34, 4, flags & ~4u), mst(c, M, ctrl + 0x38, 4, 0), mst(c, M, ctrl + 0x30, 4, 400), mst(c, M, ctrl + 0x40, 1, 0);
    }
    if (!use) { mst(c, M, ctrl + 0x38, 4, BAI_WAIT); return; }
    if (BAI_LOGGING) rt_log("battle ai: frame %u, kart %d %s %s at kart %d, %.0f away (held %u)", (unsigned)s_bai_frame, k, item_name(kind), use, at, ad,
           (unsigned)held);
    s_hold_frames[k] = 0; /* a triple's next one waits again */
}
/* RivalItemCtrl::doItemCtrl */
uint64_t i_doItemCtrl__13RivalItemCtrlFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t ctrl = (uint32_t)a0, mode = ld32(c, M, RACE_MODE);
    if (g_battle_cpus && s_route_cpus && mode >= 4 && mode <= 7 && ram_ptr(ctrl) && s_karts_now) {
        uint32_t rival = ld32(c, M, ctrl + 4);
        int k = ram_ptr(rival) ? (int)ld32(c, M, rival + 0x38) : -1;
        if (k >= s_players_now && k < s_karts_now && k < 8 && s_alive[k]) bai_item(c, M, ctrl, k);
    }
    ctx_call_orig(c, g_native_addr[NAT_doItemCtrl__13RivalItemCtrlFv]);
    return 0;
}

/* BombStart2D (Bob-omb Blast's start banner) has layouts for 2 and 3-4 screens only: for one
 * (a SINGLE PLAYER BATTLE MODE Bob-omb Blast) the constructor sets up an empty J2DScreen, whose
 * own white fill then covers the screen while the banner shows (the countdown's 3 and 2), and
 * leaves its three animations stale heap words that init/calc/start write frames through. With
 * one screen it is built as for two (RaceMgr +0x38 its RaceInfo, +0x20 the screens; all the
 * layouts are in Race2D.arc) and its screen hidden (J2DPane +0xb0). */
uint64_t i___ct__11BombStart2DFP7JKRHeap(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t self = (uint32_t)a0, rm = ld32(c, M, (uint32_t)ctx_gpr(c, 13) - 0x5c38), ri = ram_ptr(rm) ? ld32(c, M, rm + 0x38) : 0;
    bool one = ram_ptr(ri) && ld16(c, M, ri + 0x20) == 1;
    if (one) mst(c, M, ri + 0x20, 2, 2);
    ctx_call_orig(c, g_native_addr[NAT___ct__11BombStart2DFP7JKRHeap]);
    if (one) {
        mst(c, M, ri + 0x20, 2, 1);
        uint32_t scr = ld32(c, M, self + 0x1c);
        if (ram_ptr(scr)) mst(c, M, scr + 0xb0, 1, 0);
        rt_log("bomb start banner: one screen, built as two and hidden");
    }
    ctx_set_gpr(c, 3, self);
    return self;
}
/* Leaving the route for a close target: RivalBodyCtrl::doSterrCtrl (+4 RivalKart, +0x38 the kart)
 * steers at +0x58, the point it drives to (the direction to it, flat, through its virtual
 * doRotPosSterr), and moves on along the route as the kart passes its points. A computer's kart
 * chasing a kart, a box or the Shine close by (BAI_DIRECT, BAI_DIRECT_KEEP once on), in front and
 * on its level, steers at the target itself instead, for that call only (+0x58 put back after).
 * Only where the way there along the kart's network is not much longer than the straight line:
 * a target the network goes round to is behind a wall or a drop. A change either way holds for
 * BAI_DIRECT_HOLD frames (it flipped back within a few frames a third of the time: a wobble), but
 * never keeps it steering at a target that is gone. */
#define BAI_DIRECT 2000.0f
#define BAI_DIRECT_KEEP 2600.0f
#define BAI_DIRECT_HOLD 20u
#define BAI_HUNT 4000.0f
#define BAI_HUNT_KEEP 5000.0f
#define BAI_FREE_NEAR 1500.0f
static bool s_direct[8];
static uint32_t s_direct_at[8]; /* s_bai_frame of s_direct's last change */
/* network k's way between points i and j (either way round), BAI_INF when unknown */
static float bai_path(int k, int i, int j) {
    int pi = i >= 0 ? s_pt_part[i] : -1, pj = j >= 0 ? s_pt_part[j] : -1;
    if (pi < 0 || pj < 0 || !s_net_graph[k]) return BAI_INF;
    if (pi == pj) return fabsf(s_pt_off[j] - s_pt_off[i]);
    float a = s_part[pi].len - s_pt_off[i] + s_jd[k][s_part[pi].b][s_part[pj].a] + s_pt_off[j];
    float b = s_part[pj].len - s_pt_off[j] + s_jd[k][s_part[pj].b][s_part[pi].a] + s_pt_off[i];
    return fminf(a, b);
}
static bool bai_direct(int k) {
    int n = s_kart_net[k];
    if (s_goal[k].kind != GOAL_CHASE || n < 0 || n >= ROUTE_NETS || !s_net_graph[n] || !isfinite(s_goal[k].p[0])) return false;
    const float *t = s_goal[k].p, *p = s_kpos[k], *f = s_kfw[k];
    bool hunt = bai_hunt(k), on_ground = hunt && s_goal[k].target < 0; /* on_ground: the Shine on the ground, its centre sFootLength (200) up */
    float d[3] = {t[0] - p[0], t[1] - (on_ground ? 200 : 0) - p[1], t[2] - p[2]}, l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (l < 1) return s_direct[k];
    float range = hunt ? (s_direct[k] ? BAI_HUNT_KEEP : BAI_HUNT) : s_direct[k] ? BAI_DIRECT_KEEP : BAI_DIRECT;
    if (l > range || fabsf(d[1]) > 300) return false;
    /* a free Shine close by: whichever way the kart faces (it turns round for it) */
    if (!(on_ground && l < BAI_FREE_NEAR) && (d[0] * f[0] + d[1] * f[1] + d[2] * f[2]) / l < (s_direct[k] ? (hunt ? -0.6f : -0.3f) : (hunt ? -0.3f : 0.0f)))
        return false;
    int ik = bai_nearest(n, p), it = bai_nearest(n, t);
    if (ik < 0 || it < 0 || dist3(s_cl_pts[ik], p) > 800) return false;
    /* a free Shine can lie off the network (it bounces off where it was dropped): the way to the
     * network's point nearest it, then straight on */
    float off = dist3(s_cl_pts[it], t);
    if (off > (on_ground ? BAI_FREE_NEAR : 800)) return false;
    return bai_path(n, ik, it) + (on_ground ? off : 0) <= 1.5f * l + 500;
}
/* Getting unstuck. The game's own wall handling (doColWallCtrl: a kart against a wall, slow, for a
 * while) turns the kart in place (RivalBodyCtrl +0x84 state 3) toward its next point (+0x28), then
 * drives on. In a battle that point is often past the wall (the kart was knocked off its route or
 * chased a target into it), and objects don't count as walls, so a kart can sit there. Here a
 * computer's kart that has moved less than STUCK_MOVE in STUCK_FRAMES is stuck, whatever holds it:
 * it is turned the same way (state 3) toward a place it drove through a moment ago (its trail, a
 * point every CRUMB_GAP, at least ESCAPE_FROM away: reachable, it came from there), drives there
 * (steering at it, as for a target), then rejoins its network at the point nearest it, as after
 * Lakitu (comebackRescure). Stuck again soon after: an older place on the trail. */
#define STUCK_MOVE 250.0f
#define STUCK_FRAMES 90u
#define CRUMBS 16
#define CRUMB_GAP 300.0f
#define ESCAPE_FROM 700.0f
#define ESCAPE_NEAR 350.0f  /* close enough to the place: back on the network */
#define ESCAPE_FRAMES 150u  /* at most, from the start */
#define ESCAPE_AGAIN 360u   /* stuck again within this: the trail further back */
static struct stuck {
    float anchor[3], last[3], crumb[CRUMBS][3], esc[3];
    uint32_t at, esc_at;
    int ncrumb, tries;
    bool armed, escaping;
} s_stuck[8];
/* each frame (bai_state), a computer's kart k at p: how long it has stayed put, its trail. busy: hit,
 * spinning, tumbling (KartBody +0x570 status, the bits doColWallCtrl leaves alone: r13 - 0x6f98,
 * - 0x6f94), not stuck */
static void bai_track(int k, const float *p, uint32_t now, bool busy) {
    struct stuck *st = &s_stuck[k];
    if (!s_alive[k] || !st->ncrumb || dist3(p, st->last) > 1500) { /* out, starting, or put back by Lakitu */
        memcpy(st->anchor, p, 12), memcpy(st->last, p, 12), st->at = now, st->escaping = false;
        st->ncrumb = s_alive[k];
        memcpy(st->crumb[0], p, 12);
        return;
    }
    memcpy(st->last, p, 12);
    if (busy && !st->escaping) memcpy(st->anchor, p, 12), st->at = now;
    if (dist3(p, st->anchor) > STUCK_MOVE) {
        memcpy(st->anchor, p, 12), st->at = now, st->armed = true; /* (armed: it has driven, past the countdown) */
        if (now - st->esc_at > ESCAPE_AGAIN) st->tries = 0;
    }
    if (!st->escaping && dist3(p, st->crumb[(st->ncrumb - 1) % CRUMBS]) > CRUMB_GAP) memcpy(st->crumb[st->ncrumb++ % CRUMBS], p, 12);
}
/* kart k is stuck: turn it toward a place on its trail */
static void bai_escape(Ctx *c, uint8_t *M, uint32_t bc, int k) {
    struct stuck *st = &s_stuck[k];
    const float *p = s_kpos[k], *to = NULL;
    int skip = 2 * st->tries, n = st->ncrumb < CRUMBS ? st->ncrumb : CRUMBS;
    for (int i = 1; i <= n && !to; i++) {
        const float *q = st->crumb[(st->ncrumb - i) % CRUMBS];
        if (dist3(q, p) >= ESCAPE_FROM && skip-- <= 0) to = q;
    }
    if (!to && n) to = st->crumb[(st->ncrumb - n) % CRUMBS]; /* the oldest */
    st->at = s_bai_frame;
    if (!to || dist3(to, p) < STUCK_MOVE) return;
    memcpy(st->esc, to, 12), st->escaping = true, st->esc_at = s_bai_frame, st->tries++;
    mst(c, M, bc + 0x84, 4, 3);
    for (int i = 0; i < 3; i++) stf(c, M, bc + 0x28 + 4 * (uint32_t)i, to[i]);
    rt_log("battle cpus: kart %d stuck at (%.0f %.0f %.0f): backs out to (%.0f %.0f %.0f), try %d", k, p[0], p[1], p[2], to[0], to[1], to[2],
           st->tries);
}
/* the place reached (or time up): kart k rejoins its network at the point nearest it */
static void bai_rejoin(Ctx *c, uint8_t *M, uint32_t bc, int k) {
    s_stuck[k].escaping = false, s_stuck[k].at = s_bai_frame;
    uint32_t ctrl = ld32(c, M, bc + 0x24), cur = ram_ptr(ctrl) ? ld32(c, M, ctrl + 4) : 0;
    int n = s_kart_net[k], i = cl_index(c, M, cur), best = n >= 0 && n < s_nets ? bai_nearest(n, s_kpos[k]) : -1;
    if (ld32(c, M, bc + 0x84) == 3) mst(c, M, bc + 0x84, 4, 0);
    if (i < 0 || best < 0) return;
    uint32_t to = cur - 0x2c * (uint32_t)i + 0x2c * (uint32_t)best;
    if (cl_index(c, M, to) != best) return;
    ctx_set_gpr(c, 3, bc), ctx_set_gpr(c, 4, to), ctx_set_gpr(c, 5, 0);
    i_comebackRescure__13RivalBodyCtrlFP7CLPointb(c, M, bc, to, 0, 0, 0, 0, 0, 0);
    uint32_t kc = ld32(c, M, (uint32_t)ctx_gpr(c, 13) - 0x4e98);
    if (ram_ptr(kc)) target_fix(c, M, kc, k, ld32(c, M, kc + 0x1c0 + 4 * (uint32_t)k), true);
    rt_log("battle cpus: kart %d back on network %d at point %d", k, n + 1, best);
}
uint64_t i_doSterrCtrl__13RivalBodyCtrlFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t bc = (uint32_t)a0, mode = ld32(c, M, RACE_MODE), rival = ram_ptr(bc) ? ld32(c, M, bc + 4) : 0;
    int k = ram_ptr(rival) ? (int)ld32(c, M, rival + 0x38) : -1;
    bool can = g_battle_cpus && s_route_cpus && mode >= 4 && mode <= 7 && k >= s_players_now && k < s_karts_now && k < 8 && s_alive[k];
    if (can) {
        struct stuck *st = &s_stuck[k];
        if (st->escaping && (dist3(s_kpos[k], st->esc) < ESCAPE_NEAR || s_bai_frame - st->esc_at > ESCAPE_FRAMES)) bai_rejoin(c, M, bc, k);
        else if (!st->escaping && st->armed && s_bai_frame - st->at > STUCK_FRAMES) bai_escape(c, M, bc, k);
        if (st->escaping) {
            s_direct[k] = false, s_direct_at[k] = s_bai_frame;
            if (ld32(c, M, bc + 0x84) == 3) { /* turning in place (doColWallCtrl), toward the place */
                for (int i = 0; i < 3; i++) stf(c, M, bc + 0x28 + 4 * (uint32_t)i, st->esc[i]);
                return 0;
            }
            float keep[3] = {ldf(c, M, bc + 0x58), ldf(c, M, bc + 0x5c), ldf(c, M, bc + 0x60)};
            for (int i = 0; i < 3; i++) stf(c, M, bc + 0x58 + 4 * (uint32_t)i, st->esc[i]);
            ctx_call_orig(c, g_native_addr[NAT_doSterrCtrl__13RivalBodyCtrlFv]);
            for (int i = 0; i < 3; i++) stf(c, M, bc + 0x58 + 4 * (uint32_t)i, keep[i]);
            return 0;
        }
    }
    bool on = can && bai_direct(k);
    if (k >= 0 && k < 8 && on != s_direct[k] && s_bai_frame - s_direct_at[k] < BAI_DIRECT_HOLD &&
        (on || (can && s_goal[k].kind == GOAL_CHASE && isfinite(s_goal[k].p[0]))))
        on = s_direct[k];
    if (k >= 0 && k < 8 && on != s_direct[k]) {
        s_direct[k] = on, s_direct_at[k] = s_bai_frame;
        if (BAI_LOGGING) rt_log("battle ai: frame %u, kart %d %s (target %d at %.0f)", (unsigned)s_bai_frame, k, on ? "leaves the route for its target" : "back on the route",
               s_goal[k].target, dist3(s_goal[k].p, s_kpos[k]));
    }
    if (!on) {
        ctx_call_orig(c, g_native_addr[NAT_doSterrCtrl__13RivalBodyCtrlFv]);
        return 0;
    }
    float keep[3] = {ldf(c, M, bc + 0x58), ldf(c, M, bc + 0x5c), ldf(c, M, bc + 0x60)};
    for (int i = 0; i < 3; i++) stf(c, M, bc + 0x58 + 4 * (uint32_t)i, s_goal[k].p[i]);
    ctx_call_orig(c, g_native_addr[NAT_doSterrCtrl__13RivalBodyCtrlFv]);
    for (int i = 0; i < 3; i++) stf(c, M, bc + 0x58 + 4 * (uint32_t)i, keep[i]);
    return 0;
}
/* a battle's start: the boxes are found again, each computer's kart's focus (how often it takes the
 * best way rather than the game's random one: 0.6 to 0.9) drawn anew */
static void bai_reset(void) {
    s_nbox = 0, s_bai_frame = 0, s_bai_boxes_said = false, s_karts_now = s_players_now = 0, s_shine_holder = -1;
    for (int k = 0; k < 8; k++) {
        s_bai_item[k][0] = s_bai_item[k][1] = -1, s_hold_item[k] = s_hold_frames[k] = 0, s_alive[k] = false, s_direct[k] = false, s_direct_at[k] = 0;
        s_goal[k].kind = GOAL_NONE, s_goal[k].target = -1, s_goal[k].p[0] = NAN;
        s_focus[k] = 0.6f + 0.3f * bai_rand();
        memset(&s_stuck[k], 0, sizeof s_stuck[k]);
    }
    for (int i = 0; i < BAI_BOXES; i++) s_box_gone[i] = 0;
    if (!(s_bai_rnd & 0xffff)) s_bai_rnd ^= plat_now_ns() | 1;
    s_bai_rnd ^= plat_now_ns() << 1;
    if (!s_bai_rnd) s_bai_rnd = 1;
}
/* the karts this frame (bai_tick): positions, forward, living; the boxes taken; the goals */
static void bai_state(Ctx *c, uint8_t *M, uint32_t mode, uint32_t rm, int karts, int players, uint32_t shine) {
    uint32_t kc = ld32(c, M, (uint32_t)ctx_gpr(c, 13) - 0x4e98), now = s_bai_frame;
    s_karts_now = 0;
    if (!ram_ptr(kc) || karts > 8) return;
    for (int k = 0; k < karts; k++) {
        uint32_t body = ld32(c, M, kc + 0xa0 + 4 * (uint32_t)k), chk = ld32(c, M, rm + 0x48 + 4 * (uint32_t)k);
        s_alive[k] = false;
        if (!ram_ptr(body) || !ram_ptr(chk)) continue;
        float p[3] = {ldf(c, M, body + 0x11c), ldf(c, M, body + 0x12c), ldf(c, M, body + 0x13c)};
        float f[3] = {ldf(c, M, body + 0x118), ldf(c, M, body + 0x128), ldf(c, M, body + 0x138)};
        if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2]) || !isfinite(f[0]) || !isfinite(f[1]) || !isfinite(f[2])) continue;
        memcpy(s_kpos[k], p, 12), memcpy(s_kfw[k], f, 12);
        s_alive[k] = !(ld16(c, M, chk + 0x8c) & 4), s_balloons[k] = (int16_t)ld16(c, M, chk + 0x90);
        if (k >= players) {
            uint32_t r13 = (uint32_t)ctx_gpr(c, 13);
            bool busy = (ld32(c, M, body + 0x570) & ld32(c, M, r13 - 0x6f98)) | (ld32(c, M, body + 0x574) & ld32(c, M, r13 - 0x6f94));
            bai_track(k, p, now, busy);
        }
    }
    s_karts_now = karts, s_players_now = players, s_mode_now = mode;
    for (int i = 0; i < s_nbox; i++)
        for (int k = 0; k < karts; k++)
            if (s_alive[k] && dist3(s_box[i].p, s_kpos[k]) < BAI_BOX_NEAR) s_box_gone[i] = now + BAI_BOX_GONE;
    s_shine_holder = -1;
    if (mode == 7 && ram_ptr(shine)) {
        float sp[3] = {ldf(c, M, shine + 4), ldf(c, M, shine + 8), ldf(c, M, shine + 0xc)};
        s_shine_holder = (int16_t)ld16(c, M, shine + 0x1ae);
        for (int k = players; k < karts; k++)
            if (isfinite(sp[0]) && isfinite(sp[1]) && isfinite(sp[2])) memcpy(s_goal[k].p, sp, 12);
            else s_goal[k].p[0] = NAN;
    }
    bai_goals(mode, now);
    if (now % BAI_EVERY == 0)
        for (int k = players; k < karts; k++)
            if (s_goal[k].kind != GOAL_NONE)
                if (BAI_LOGGING) rt_log("battle ai:   kart %d %s %d (%.0f %.0f %.0f), focus %.2f", k, s_goal[k].kind == GOAL_FLEE ? "flees" : "chases",
                       s_goal[k].target, s_goal[k].p[0], s_goal[k].p[1], s_goal[k].p[2], s_focus[k]);
}

/* 1-player battles (single player battle mode): the 1-player mode page (SceneMenu +0x20fc karts - 1 and
 * +0x20f4 players - 1 both 0) offers Grand Prix, Time Trial and Battle (+0x2100 mode 0, 2, 3) in
 * the 2-player page's three rows (selectmode.blo N_Mode2: Grand Prix, Versus, Battle), Time Trial
 * in Versus's row. The page's entries are {anim, pane, frame} (pane + 0xb0 visible, + 4 the
 * pane's index into the page's .bck, + 0x10 its tag); a pane takes another's place by taking its
 * index. The 1-player page's code runs the page (Battle, which it lacks, shown to it as Grand Prix
 * until decided) and its Time Trial panes (MNtaP11, MtamN) stay, now in Versus's place; its other
 * panes are hidden. Then the 2-player page's code, with its input and timers held, runs again
 * for the three rows' frames (Time Trial as Versus). A on Battle decides as on that page (the
 * battle type page, the character page, the arena), and setRaceData, which has no 1-kart battle,
 * gets its setRace from here. */
#define SM_KARTS 0x20fc
#define SM_PLAYERS 0x20f4
#define SM_MODE 0x2100
#define SM_DECIDED 0x2108
#define SM_SAVING 0x210a   /* u8: the memory card's turn */
#define SM_TIMERS 0x210c   /* three s32 timers: input waits while any runs */
#define SM_MODE2 0xc10     /* N_Mode2: the three rows */
#define SM_VS_PIC 0xc4c    /* MNvsP21 */
#define SM_VS_TITLE 0xcc4  /* MvsN */
#define SM_PW_VS 0xd48     /* N_Pw_vs: Versus's engine class */
#define SM_MODE1 0xd9c     /* N_Mode1: the two rows */
#define SM_TA_PIC 0xdd8    /* MNtaP11 */
#define SM_TA_TITLE 0xe38  /* MtamN */
#define MENU_PAD 0x803527F8u /* the menu's JUTGamePad: +0x1c buttons pressed, +0x30 repeat */
#define MENU_A 0x100u
#define MENU_UP 0x08000000u
#define MENU_DOWN 0x04000000u
/* the two-row panes hidden on the three-row page: M4crsr4, M4crsr5, MNgpP11, MgpmN2, N_Mode12,
 * N_Pw_gp1; the 2-player page's own that it has no use for: MNvsP21, MvsN, N_Pw_vs */
static const uint32_t SM_HIDE[] = {0xda8, 0xdb4, 0xdcc, 0xe20, 0xe50, 0xeec, SM_VS_PIC, SM_VS_TITLE, SM_PW_VS};
static const uint32_t SM_ROWS2[] = {SM_MODE2, 0xc1c, 0xc28, 0xc34, 0xcf4}; /* N_Mode2, M4crsr1-3, N_Pw_gp2 */
static bool s_three;
static uint16_t s_ta_id[2];
static bool solo_menu(Ctx *c, uint8_t *M, uint32_t m) {
    return g_battle_cpus && IS_RAM(m) && ld32(c, M, m + SM_KARTS) == 0 && ld32(c, M, m + SM_PLAYERS) == 0 &&
           IS_RAM(ld32(c, M, m + SM_MODE2)) && IS_RAM(ld32(c, M, m + SM_VS_PIC)) && IS_RAM(ld32(c, M, m + SM_VS_TITLE)) &&
           IS_RAM(ld32(c, M, m + SM_TA_PIC)) && IS_RAM(ld32(c, M, m + SM_TA_TITLE));
}
static void pane_show(Ctx *c, uint8_t *M, uint32_t p, bool on) {
    if (IS_RAM(p)) mst(c, M, p + 0xb0, 1, on);
}
/* N_Mode1's child M4bar4 or M4bar5 (not on the page's list): its JSUTree at +0xdc, links' +0 the
 * pane, +0xc the next */
static uint32_t mode1_bar(Ctx *c, uint8_t *M, uint32_t m, char n) {
    uint32_t parent = ld32(c, M, m + SM_MODE1), link = IS_RAM(parent) ? ld32(c, M, parent + 0xdc) : 0;
    for (int i = 0; i < 64 && IS_RAM(link); i++, link = ld32(c, M, link + 0xc)) {
        uint32_t p = ld32(c, M, link);
        if (IS_RAM(p) && ld32(c, M, p + 0x10) == 0x00004d34u /* "M4" */ && ld32(c, M, p + 0x14) == (0x62617200u | (uint8_t)n))
            return p;
    }
    return 0;
}
static void three_rows(Ctx *c, uint8_t *M, uint32_t m, bool on) {
    uint32_t ta[2] = {ld32(c, M, m + SM_TA_PIC), ld32(c, M, m + SM_TA_TITLE)};
    uint32_t vs[2] = {ld32(c, M, m + SM_VS_PIC), ld32(c, M, m + SM_VS_TITLE)};
    if (on && !s_three)
        for (int i = 0; i < 2; i++) s_ta_id[i] = (uint16_t)ld16(c, M, ta[i] + 4);
    for (int i = 0; i < 2; i++) mst(c, M, ta[i] + 4, 2, on ? ld16(c, M, vs[i] + 4) : s_ta_id[i]);
    if (on) {
        for (size_t i = 0; i < sizeof SM_HIDE / sizeof *SM_HIDE; i++) pane_show(c, M, ld32(c, M, m + SM_HIDE[i]), false);
    } else { /* the panes the game never shows itself */
        pane_show(c, M, ld32(c, M, m + 0xe20), true), pane_show(c, M, vs[0], true), pane_show(c, M, vs[1], true);
    }
    pane_show(c, M, mode1_bar(c, M, m, '4'), !on), pane_show(c, M, mode1_bar(c, M, m, '5'), !on);
    s_three = on;
}
/* run fn (mode_init or mode) as the 2-player page for the three rows: Time Trial as Versus (1),
 * input and the timers held (one at 1: input waits, none runs out), the memory card left alone */
static void as_two_rows(Ctx *c, uint8_t *M, uint32_t m, uint32_t fn) {
    uint32_t mode = ld32(c, M, m + SM_MODE), saving = mld(c, M, m + SM_SAVING, 1), t[3];
    for (int i = 0; i < 3; i++) t[i] = ld32(c, M, m + SM_TIMERS + 4 * i);
    mst(c, M, m + SM_KARTS, 4, 1), mst(c, M, m + SM_MODE, 4, mode == 2 ? 1 : mode);
    mst(c, M, m + SM_SAVING, 1, 0), mst(c, M, m + SM_TIMERS, 4, 1), mst(c, M, m + SM_TIMERS + 4, 4, 0), mst(c, M, m + SM_TIMERS + 8, 4, 0);
    ctx_set_gpr(c, 3, m);
    ctx_call_orig(c, fn);
    mst(c, M, m + SM_KARTS, 4, 0), mst(c, M, m + SM_MODE, 4, mode), mst(c, M, m + SM_SAVING, 1, saving);
    for (int i = 0; i < 3; i++) mst(c, M, m + SM_TIMERS + 4 * i, 4, t[i]);
}
uint64_t i_mode_init__9SceneMenuFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t m = (uint32_t)a0;
    bool solo = solo_menu(c, M, m), bat = solo && ld32(c, M, m + SM_MODE) == 3;
    if (solo) { /* first: its last call (+0x20ec, the intro's count, at 15) ends the intro (count 0,
                 * page state +0x20e0 to the mode page, the old in +0x20e4), the 1-player call's to do */
        static const uint32_t keep[3] = {0x20e0, 0x20e4, 0x20ec};
        uint32_t st[3];
        for (int i = 0; i < 3; i++) st[i] = ld32(c, M, m + keep[i]);
        as_two_rows(c, M, m, g_native_addr[NAT_mode_init__9SceneMenuFv]);
        for (int i = 0; i < 3; i++) mst(c, M, m + keep[i], 4, st[i]);
    }
    if (bat) mst(c, M, m + SM_MODE, 4, 0); /* back from the battle type page: set up as Grand Prix */
    ctx_set_gpr(c, 3, m); /* the call above left its own r3 */
    ctx_call_orig(c, g_native_addr[NAT_mode_init__9SceneMenuFv]);
    if (bat) mst(c, M, m + SM_MODE, 4, 3);
    if (solo) {
        three_rows(c, M, m, true);
    } else if (s_three && IS_RAM(m)) {
        three_rows(c, M, m, false);
    }
    return 0;
}
uint64_t i_mode__9SceneMenuFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t m = (uint32_t)a0;
    bool solo = solo_menu(c, M, m) && s_three;
    uint32_t pad = ld32(c, M, MENU_PAD), m0 = solo ? ld32(c, M, m + SM_MODE) : 0;
    uint32_t press = IS_RAM(pad) ? ld32(c, M, pad + 0x1c) : 0, rep = IS_RAM(pad) ? ld32(c, M, pad + 0x30) : 0;
    /* on Battle, not yet decided: the 1-player code sees Grand Prix (its A decides any mode the same) */
    bool as_gp = solo && m0 == 3 && !mld(c, M, m + SM_DECIDED, 1) && !(press & MENU_A);
    if (as_gp) mst(c, M, m + SM_MODE, 4, 0);
    ctx_call_orig(c, g_native_addr[NAT_mode__9SceneMenuFv]);
    if (!solo) return 0;
    uint32_t m1 = ld32(c, M, m + SM_MODE); /* the 1-player code goes 0 <-> 2 */
    if (as_gp) m1 = m1 == 2 ? (rep & MENU_UP ? 2 : 0) : 3;
    else if (m0 == 0 && m1 == 2 && (rep & MENU_UP)) m1 = 3;
    else if (m0 == 2 && m1 == 0 && (rep & MENU_DOWN)) m1 = 3;
    mst(c, M, m + SM_MODE, 4, m1);
    as_two_rows(c, M, m, g_native_addr[NAT_mode__9SceneMenuFv]);
    return 0;
}
uint64_t i_mode_end__9SceneMenuFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t m = (uint32_t)a0;
    ctx_call_orig(c, g_native_addr[NAT_mode_end__9SceneMenuFv]);
    if (!s_three || !IS_RAM(m)) return 0;
    /* the three rows leave with the two: N_Mode2 as N_Mode1, N_Pw_gp2 fading as N_Pw_gp1 */
    stf(c, M, m + SM_MODE2 + 4, ldf(c, M, m + SM_MODE1 + 4));
    stf(c, M, m + 0xcf8, ldf(c, M, m + 0xef0));
    uint32_t rows = ld32(c, M, m + SM_MODE1);
    if (IS_RAM(rows) && !mld(c, M, rows + 0xb0, 1)) { /* the page is gone */
        for (size_t i = 0; i < sizeof SM_ROWS2 / sizeof *SM_ROWS2; i++) pane_show(c, M, ld32(c, M, m + SM_ROWS2[i]), false);
        three_rows(c, M, m, false);
    }
    return 0;
}
/* setRaceData sets up a battle only for 2-4 karts: a 1-player battle gets setRace(mode, 4 karts,
 * 1 player, 1 screen) here first (the battle type +0x2118: 0 balloons, 1 Shine Thief, 2 Bob-omb);
 * the call from here runs the recompiled setRace, not i_FUN_8018c878. */
uint64_t i_setRaceData__9SceneMenuFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t m = (uint32_t)a0, kind = solo_menu(c, M, m) && ld32(c, M, m + SM_MODE) == 3 ? ld32(c, M, m + 0x2118) : 3;
    if (kind < 3) {
        static const uint32_t race_mode[3] = {4, 7, 6};
        const uint32_t args[6] = {0x803B145Cu /* gRaceInfo */, race_mode[kind], 4, 1, 1, 1};
        uint32_t old_sp = frame_push(c, M, 0x20);
        rt_log("1-player battle: mode %u", race_mode[kind]);
        ctx_call_guest(c, g_native_addr[NAT_FUN_8018c878], args, 6);
        frame_pop(c, old_sp);
        ctx_set_gpr(c, 3, m);
    }
    ctx_call_orig(c, g_native_addr[NAT_setRaceData__9SceneMenuFv]);
    return 0;
}
/* SceneMenu::getCharSel(&a, &b): setRaceData's two cells for each computer's kart, from the cells
 * still free (+0x22a0 + cell, 1 free: the players' and the locked ones cleared), each taken as it
 * is chosen. The game seeds from OSGetTime on each call and (at its level 0, a battle's) keeps
 * official partners together. A single player battle mode battle (gRaceInfo +8 modes 4-7, set before) draws any
 * two free cells from a seed of its own instead; the kart is still the heavier one's (setRaceData). */
uint64_t i_getCharSel__9SceneMenuFRiRi(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t m = (uint32_t)a0, mode = ld32(c, M, RACE_MODE);
    if (!g_battle_cpus || mode < 4 || mode > 7 || !ram_ptr(m) || !ram_ptr((uint32_t)a1) || !ram_ptr((uint32_t)a2)) {
        ctx_call_orig(c, g_native_addr[NAT_getCharSel__9SceneMenuFRiRi]);
        return 0;
    }
    static uint64_t s;
    if (!s) s = plat_now_ns() | 1;
    int cell[2];
    for (int k = 0; k < 2; k++) {
        int free_[20], n = 0;
        for (int i = 0; i < 20; i++)
            if (mld(c, M, m + 0x22a0 + (uint32_t)i, 1)) free_[n++] = i;
        if (!n) { /* nothing left (cannot happen with 4 karts): the game's own */
            ctx_call_orig(c, g_native_addr[NAT_getCharSel__9SceneMenuFRiRi]);
            return 0;
        }
        s ^= s << 13, s ^= s >> 7, s ^= s << 17;
        cell[k] = free_[s % (uint64_t)n];
        mst(c, M, m + 0x22a0 + (uint32_t)cell[k], 1, 0);
    }
    mst(c, M, (uint32_t)a1, 4, (uint32_t)cell[0]), mst(c, M, (uint32_t)a2, 4, (uint32_t)cell[1]);
    char n0[16] = "?", n1[16] = "?";
    for (int k = 0; k < 2; k++) { /* the cell's ECharID (+0xc + 12 * cell), DriverLoader's names by ECharID - 1 */
        uint32_t id = ld32(c, M, m + 0xc + 12 * (uint32_t)cell[k]);
        if (id >= 1 && id <= 20) guest_str(c, M, ld32(c, M, CP_DRIVER_NAMES + 4 * (id - 1)), k ? n1 : n0, 16);
    }
    rt_log("battle cpus: computer team %s + %s (cells %d, %d)", n0, n1, cell[0], cell[1]);
    return 0;
}
/* The balloons (FUN_801f1cf4, GeographyObjManager's: a TBalloonManager at +0x328 + 4k for each
 * kart k below gRaceInfo +0x1e, the player count): in single player battle mode the computer's karts get
 * theirs too (the count at +0x1c for the call). */
#define RACE_INFO 0x803B145Cu
uint64_t i_FUN_801f1cf4(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t players = ld16(c, M, RACE_INFO + 0x1e), karts = ld16(c, M, RACE_INFO + 0x1c);
    bool all = g_battle_cpus && ld32(c, M, RACE_MODE) == 4 && karts > players && karts <= 4;
    if (all) mst(c, M, RACE_INFO + 0x1e, 2, karts);
    ctx_call_orig(c, g_native_addr[NAT_FUN_801f1cf4]);
    if (all) mst(c, M, RACE_INFO + 0x1e, 2, players);
    return 0;
}
