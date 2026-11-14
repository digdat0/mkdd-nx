/* The race HUD (split from native.c): Race2D::draw / drawCourse and the wipes bracketed for the
 * widescreen HUD (hud_zone), the elements the player turned off (g_hud_hide), MODERN and DASHBOARD
 * (RACE HUD STYLE, RACE HUD), the coin counters (coins.c), the trick battle's HUD and results
 * (trick.c), and K2DPicture::drawK2D, where most of it is placed. */
#include "native_impl.h"
#include "trick.h"
#include "hud.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "rt.h"
#include "charpack.h"
#include "coins.h"
#include "plat.h"
#include "gx.h"
#include "gp_inline.h"
#include "route.h"
/* Race2D::draw (the race HUD) and Race2D::drawCourse (the map), any number of players: bracketed by a
 * marker in the FIFO (a CP write to register 0x01, which the hardware does not have) so the
 * decoder can move their draws to the screen edges in the widescreen HUD (gx.c XV_HUD_LEFT). The
 * marker is 1 (place each draw by its position) or 2 (drawCourse: the map and the top-4 rank column;
 * nothing there stays in the middle). The
 * console count is RaceMgr (r13 - 0x5c38) -> +0x38 -> s16 at +0x20 (getConsoleNumber). */
static void hud_zone(uint32_t on) {
    uint8_t b[6], *p = b;
    fifo8(&p, 0x08);
    fifo8(&p, 0x01);
    fifo32(&p, on);
    hw_gather_copy(b, 6);
}
/* HUD elements the player turned off (g_hud_hide, HUD_* bits), 1 to 4 players: drawK2D skips
 * their pictures, found by pane tag (race2d.arc p1_race.blo, p2_race.blo, p4_race.blo for 3 and 4:
 * each player's pieces carry its number, "pos21", "slot3b", "m4_lap", "time2t1"; only one player
 * has a speedometer; drawCourse's map kart icons have tag 0). */
volatile uint32_t g_hud_hide;
static uint32_t s_hud_hide, s_hud_zone; /* while Race2D::draw / drawCourse run: the bits, the marker */
static uint32_t s_hud_call;             /* ... 1 Race2D::draw, 2 drawCourse */
static uint32_t s_hud_racing;           /* ... bit v: view v's kart still racing (its timer hidden) */
static int pane_tag(Ctx *c, uint8_t *M, uint32_t pane, char t[9]) {
    int n = 0;
    for (int i = 0; i < 8; i++) {
        char ch = (char)mld(c, M, pane + 0x10 + i, 1); /* J2DPane mTag: u64, the name right-aligned */
        if (ch) t[n++] = ch;
    }
    t[n] = 0;
    return n;
}
/* The HUD_* element a pane is (0: none); for HUD_TIME, *player its player (0-3). */
static uint32_t hud_piece(Ctx *c, uint8_t *M, uint32_t pane, int *player) {
    char t[9];
    int n = pane_tag(c, M, pane, t);
#define PRE(p) (!strncmp(t, p, sizeof p - 1))
#define PL(i) (t[i] >= '1' && t[i] <= '4') /* a player's number there */
    if (PRE("pos") && PL(3)) return HUD_PLACE;                             /* pos11-48, pos11g-28g */
    if ((PRE("slot") && PL(4)) || (PRE("itm") && PL(3))) return HUD_ITEMS;
    if ((t[0] == 'm' && PL(1) && !strncmp(t + 2, "_lap", 4)) || (PRE("ml") && PL(2) && t[3] == '_')) return HUD_LAPS;
    if (PRE("sp_mtr") || PRE("km_h") || ((PRE("sp1") || PRE("sp2") || PRE("sp3")) && n == 3)) return HUD_SPEED;
    if (PRE("CI_") || PRE("cw1") || PRE("posc") || (PRE("c1") && n == 3)) return HUD_ORDER; /* top-4 column */
    if (PRE("course") || (!n && s_hud_call == 2)) return HUD_MAP;
    int i = PRE("Tlapm") ? 5 : PRE("time") ? 4 : PRE("Tl") ? 2 : 0; /* total time, lap times */
    if (i && PL(i)) return *player = t[i] - '1', HUD_TIME;
#undef PL
#undef PRE
    return 0;
}
static bool hud_hidden(Ctx *c, uint8_t *M, uint32_t pane) {
    int player = 0;
    uint32_t k = hud_piece(c, M, pane, &player) & s_hud_hide;
    return k && (k != HUD_TIME || s_hud_racing >> player & 1); /* the timer: back once that player finishes */
}
/* The race timer comes back once the player's kart has finished, for the finish and its times:
 * RaceMgr +0x38 RaceInfo (+0x114 + 2 * view: s16 target kart of each view), +0x48 KartChecker
 * *[8] (+0x29 mRaceEnd, isGoal). Not known: treated as finished, the timer shows. */
static bool view_racing(Ctx *c, uint8_t *M, int view) {
    uint32_t rm = ld32(c, M, ctx_gpr(c, 13) - 0x5c38), info = IS_RAM(rm) ? ld32(c, M, rm + 0x38) : 0;
    if (!IS_RAM(info) || view < 0 || view > 3) return false;
    int k = (int16_t)ld16(c, M, info + 0x114 + 2 * (uint32_t)view);
    uint32_t chk = k >= 0 && k < 8 ? ld32(c, M, rm + 0x48 + 4 * k) : 0;
    return IS_RAM(chk) && !mld(c, M, chk + 0x29, 1);
}
static bool s_hud_logging;   /* PC harness: log this race frame's HUD (g_hud_log) */
extern volatile uint32_t g_race_frames;
#define HUDLOG(fmt, ...) rt_log("hudlog f%u " fmt, g_race_frames, __VA_ARGS__) /* the frame: rt_log drops repeated lines */
static bool s_modern;        /* this Race2D draw / drawCourse: the MODERN layout (drawK2D) */
static bool s_pill_todo;     /* ... DASHBOARD: its panels still to come */
static bool s_mirror_ask;    /* ... DASHBOARD with the mirror on: no HUD piece drawn yet (drawK2D asks at the first) */
static uint32_t s_pill_seen, s_pill_last; /* DASHBOARD: the groups drawn this frame, the last (bits by group - MGT_PLACE) */
static void pill_new_race(void);
static uint32_t s_coin_todo;           /* this Race2D draw: the counters still to come (bits by view) */
static bool s_coin_track;              /* ... any wanted: the lap count's pictures followed (coin_seen; nothing done without coins) */
static bool s_coin_row;                /* ... DASHBOARD: its panel drawn (pill_panels) */
static float s_coin_e;                 /* ... and the panels' strength */
static void coin_new_race(void);
static void coin_frame_start(Ctx *c, uint8_t *M, int n);
static bool s_hud_one;       /* ... single player (one screen) */
static bool s_trick_hud;     /* ... Race2D::draw of a trick battle (one screen): trick_hud after it, no coin counter of coin_hud */
static int s_trick_views;    /* ... a trick battle on 2-4 screens: trick_hud_views after it (the game's coin counters stay) */
volatile float g_item_band_dy; /* gx.c: the item frames' clip band, layout units below DASHBOARD's (the trick HUD's row) */
volatile float g_item_band_k = 1; /* ... its height times this (the trick HUD's larger boxes) */
#define TH_ITEM_K 1.6f /* the trick HUD's item boxes, times DASHBOARD's: about the coins panel's height (TH_H) */
static float s_th_coin_x1;   /* the trick HUD's coins panel right edge (zone 5), last drawn: the item slots go right of it */
#define TH_Y 12.0f
#define TH_H 36.0f
#define TH_VS_CLOCK_Y 276.0f /* side by side: the clock's middle, above the map */
#define TH_TB_TRICK_X 205.0f /* top and bottom: the trick column's middle; MODERN's item boxes end at 144, the meter (102 wide) and its text clear of them (the game's own boxes end at 196: +120) */
static void trick_hud(Ctx *c, uint8_t *M);
static void trick_hud_views(Ctx *c, uint8_t *M, int n);
static bool s_hud_two;       /* ... two players (top and bottom screens) */
static int s_hud_n;          /* ... the number of screens */
static uint32_t s_zone_now; /* the marker last written */
static uint32_t s_race2d;   /* the Race2D drawing */
/* MODERN per view (one to four screens): 1 while that player races, then down to 0 (the game's
 * layout) over half a second after the finish, so the pieces slide back instead of jumping */
static float s_modern_t[4];
static void modern_tick(Ctx *c, uint8_t *M) {
    int n = screens(c, M);
    for (int v = 0; v < 4; v++) {
        bool on = g_hud_modern && n >= 1 && n <= 4 && v < n && view_racing(c, M, v);
        float t = s_modern_t[v] - 1.0f / 30;
        s_modern_t[v] = on ? 1.0f : t > 0 ? t : 0.0f;
    }
}
static void hud_kmh(Ctx *c, uint8_t *M, uint32_t race2d);
static int player_kart(Ctx *c, uint8_t *M);
static bool kart_is_player(Ctx *c, uint8_t *M, int kart);
static void race_hud_call(Ctx *c, uint8_t *M, uint32_t fn, uint32_t self, uint32_t zone) {
    int n = screens(c, M);
    bool one = n == 1;
    s_hud_call = zone;
    s_hud_zone = s_zone_now = zone; /* any number of screens: each layout keeps its players' pieces at the edges */
    s_hud_hide = n >= 1 && n <= 4 ? g_hud_hide : 0;
    s_hud_racing = 0;
    for (int v = 0; v < n && v < 4 && (s_hud_hide & HUD_TIME); v++) s_hud_racing |= (uint32_t)view_racing(c, M, v) << v;
    s_modern = n >= 1 && n <= 4 && (s_modern_t[0] > 0 || s_modern_t[1] > 0 || s_modern_t[2] > 0 || s_modern_t[3] > 0);
    s_hud_one = one;
    s_hud_two = n == 2;
    s_hud_n = n;
    s_race2d = self;
    bool trick = g_trick_comp && ld32(c, M, 0x803B1464u) == 4; /* gRaceInfo +8: a trick battle (a balloon battle) */
    s_trick_hud = one && zone == 1 && trick;
    s_trick_views = !one && n >= 2 && n <= 4 && zone == 1 && trick ? n : 0;
    s_pill_todo = one && g_hud_pills && s_modern && !trick; /* a trick battle: its own HUD, no DASHBOARD (nor its mirror) */
    s_mirror_ask = s_pill_todo && g_hud_mirror;
    if (zone == 1) s_pill_last = s_pill_seen, s_pill_seen = 0;
    if (zone == 1) coin_frame_start(c, M, n);
    if (n >= 3 && ld16(c, M, 0x803B145Cu + 0x1e) == 3) g_split3_vi = g_vi_count + 1; /* gx.c: three players (gRaceInfo +0x1e), the fourth screen's frame */
    if (one && zone == 1 && g_speed_kmh) hud_kmh(c, M, self);
    /* Bob-omb Blast's stars (+0xb10: x, y of 4 places in each of 3 sets, 0x20 a set): p1_race.blo, which never had
     * a battle, keeps them above the screen. Brought down to the top right, as on the two player screens. */
    if (one && zone == 1 && ld32(c, M, 0x803B1464u) == 6 && IS_RAM(self))
        for (uint32_t k = 0; k < 12; k++) {
            uint32_t y = self + 0xb14 + 8 * k;
            if (ldf(c, M, y) < 0) stf(c, M, y, ldf(c, M, y) + 214.0f);
        }
    /* ... and its layouts for one and two screens have 3 stars shown, for 2 karts' win score: more karts' 4 need the 4th
     * (+0xa50: 4 pictures in each of 3 sets, 0x10 a set, 0x30 a screen). Beside the third, as far from it as it is from the second. */
    if (zone == 1 && n >= 1 && n <= 2 && ld32(c, M, 0x803B1464u) == 6 && IS_RAM(self) && ld16(c, M, (uint32_t)ctx_gpr(c, 13) - 0x7976) == 4)
        for (uint32_t v = 0; v < (uint32_t)n; v++)
            for (uint32_t set = 0; set < 3; set++) {
                uint32_t pic = ld32(c, M, self + 0xa5c + 0x30 * v + 0x10 * set), at = self + 0xb10 + 0x60 * v + 0x20 * set;
                if (!IS_RAM(pic) || mld(c, M, pic + 0xb0, 1)) continue;
                mst(c, M, pic + 0xb0, 1, 1);
                stf(c, M, at + 24, 2 * ldf(c, M, at + 16) - ldf(c, M, at + 8));
                stf(c, M, at + 28, ldf(c, M, at + 12));
                rt_log("bob-omb stars: view %u set %u gets a 4th at %.0f %.0f", v, set, ldf(c, M, at + 24), ldf(c, M, at + 28));
            }
    if (s_hud_logging && zone == 2) {
        int pk = player_kart(c, M);
        uint32_t b = self + 0x4ca0 + 0x38 * (uint32_t)(pk < 0 ? 0 : pk);
        HUDLOG("order: player kart %d modern %d, its pictures shown %u %u %u %u %u", pk, s_modern, (unsigned)mld(c, M, ld32(c, M, b) + 0xb0, 1),
               (unsigned)mld(c, M, ld32(c, M, b + 4) + 0xb0, 1), (unsigned)mld(c, M, ld32(c, M, b + 8) + 0xb0, 1),
               (unsigned)mld(c, M, ld32(c, M, b + 12) + 0xb0, 1), (unsigned)mld(c, M, ld32(c, M, b + 16) + 0xb0, 1));
    }
    hud_zone(zone);
    uint32_t old = ctx_gpr(c, 1), sp = old - 0x20;
    mst(c, M, sp, 4, old);
    ctx_set_gpr(c, 1, sp);
    ctx_set_gpr(c, 3, self);
    ctx_call_orig(c, fn); /* the recompiled function's entry would come back here */
    ctx_set_gpr(c, 1, old);
    if (s_trick_hud && g_trick_live) trick_hud(c, M); /* over the race HUD, in its 2D projection */
    if (s_trick_views && g_trick_live) trick_hud_views(c, M, s_trick_views);
    hud_zone(0);
    s_hud_zone = s_hud_hide = s_hud_call = s_zone_now = s_hud_racing = 0;
    s_modern = s_hud_one = s_hud_two = s_mirror_ask = s_trick_hud = false, s_coin_todo = 0;
    s_hud_n = 0, s_trick_views = 0;
}
volatile uint32_t g_race_frames, g_hud_log; /* g_hud_log: rt.h */
uint64_t i_draw__6Race2DFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    g_race_frames++;
    if (plat_now_ns() - g_racing_ns > 1000000000ull) pill_new_race(); /* no race HUD for a second: another race */
    g_racing_ns = plat_now_ns(); /* gx_drain_dma */
    s_hud_logging = g_hud_log && g_race_frames == g_hud_log; /* once a race: every 300 frames filled the log's lines */
    if (s_hud_logging) rt_log("hudlog frame %u, speed unit flag %u", g_race_frames, ld32(c, M, (uint32_t)ctx_gpr(c, 13) - 0x68dc));
    vsplit_live(c, M);
    vs_goal(c, M);
    aspect_refresh(c, M); /* once per race frame */
    modern_tick(c, M);
    race_hud_call(c, M, 0x80145CB0u, (uint32_t)a0, 1);
    return 0;
}
uint64_t i_drawCourse__6Race2DFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    /* a mod arena whose route file gives its minimap frame (the trick tracks: bigger than the slot's
     * stock arena, so the karts' icons left the map): Race2D's world box (+0x1110 x0 z0 x1 z1, set per
     * course ID by its constructor) and orientation (+0x1120), as getMapPos reads them */
    float box[4];
    int orient;
    if (ram_ptr((uint32_t)a0) && route_map(box, &orient)) stfn(c, M, (uint32_t)a0 + 0x1110, box, 4), mst(c, M, (uint32_t)a0 + 0x1120, 4, (uint32_t)orient);
    race_hud_call(c, M, 0x801483B4u, (uint32_t)a0, 2); /* map, its kart icons and the top-4 rank column */
    return 0;
}
/* WipeManager::draw: the fades and wipes over each kart's 3D view (the black screen of a respawn,
 * race start and end). They fill the 3D viewport, which widescreen widens, so they stay full
 * width: marker 3, any number of players (J2DManager::draw calls it after Race2D::draw). */
uint64_t i_draw__11WipeManagerFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    /* the trick results (drawn in Race2D::draw, before this): a player who lost the battle (every kart
     * but the winner, at time up) has their view faded to half black ~4 s on, which dimmed the results.
     * Not drawn while they are up; the game's end (its battle menu) shows the fade after them. */
    if (g_trick_comp && g_trick_res == 1) return 0;
    hud_zone(3);
    uint32_t old = ctx_gpr(c, 1), sp = old - 0x20;
    mst(c, M, sp, 4, old);
    ctx_set_gpr(c, 1, sp);
    ctx_set_gpr(c, 3, (uint32_t)a0);
    ctx_call_orig(c, 0x8012A358u);
    ctx_set_gpr(c, 1, old);
    hud_zone(0);
    return 0;
}

/* Mario::draw: the black wipe when a race is quit from the pause menu (PauseManager::draw, outside
 * WipeManager). It covers the screen, so it stays full width like the wipes above. */
uint64_t i_draw__5MarioFfQ28JUtility6TColor(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    hud_zone(3);
    uint32_t old = ctx_gpr(c, 1), sp = old - 0x20;
    mst(c, M, sp, 4, old);
    ctx_set_gpr(c, 1, sp);
    ctx_set_gpr(c, 3, (uint32_t)a0);
    ctx_set_gpr(c, 4, (uint32_t)a2);
    WR(c, 0x4008, 8, a1);
    WR(c, 0x5008, 8, a1);
    ctx_call_orig(c, g_native_addr[NAT_draw__5MarioFfQ28JUtility6TColor]);
    ctx_set_gpr(c, 1, old);
    hud_zone(0);
    return 0;
}

/* ---- MODERN race HUD (g_hud_modern, the - menu's RACE HUD STYLE): 1-4 players (below: the
 * single player layout; two, three and four have their own groups), each until that player finishes (the finish and results keep the game's layout). The game's own pictures, its
 * animations kept, made smaller and moved: position and items top left, LAP and TIME side by side
 * at the top centre, the map top right, the race order (top 4, and the player's row as a 5th when
 * lower) bottom left, the speedometer bottom right. drawK2D scales each picture's matrix about its
 * group's corner (608 x 448 layout units) and writes the group's side for the widescreen HUD (gx.c
 * markers 4-6: middle, left, right edge). ---- */
volatile uint32_t g_hud_modern, g_hud_pills, g_speed_kmh;
/* the player's race order row below the top 4: drawCourse puts ranks 5-8 below the screen (CI_05,
 * y 501); moved up to where a 5th row goes */
#define ORDER_ROW5_DY -102.7f
/* two players: the place's room on its screen (layout units): versus moves the place pictures
 * (no timer there), and they are kept inside it as moved */
#define PLACE2_TOP 8.0f
#define PLACE2_BOTTOM 440.0f
/* four players (p4_race.blo): the same beside the place, scaled to its word (32 wide), and the pair
 * moved 8 down to sit level with the place. Left quarters: the count right of the word; right ones:
 * the word moved left of the count, so the count stays beside the place */
#define LAP4_COUNT_DX 30.9f
#define LAP4_COUNT_DY -12.4f
#define LAP4_WORD_DX -45.0f
#define LAP4_WORD_DY 8.0f
/* the group of a picture (by its tag), MG_NONE (not moved) or -1 (not drawn); call: s_hud_call */
static int modern_group(const char *t, int n, uint32_t call) {
#define PRE(p) (!strncmp(t, p, sizeof p - 1))
    if (PRE("pos1") && n == 5) return MG_PLACE;
    if (PRE("slot1") || PRE("itm1")) return MG_ITEMS;
    if (PRE("m1_lap") || PRE("ml1_")) return MG_LAP;
    if (!strcmp(t, "time17")) return -1; /* the word TIME: not drawn */
    if (PRE("time1") || PRE("Tl1") || PRE("Tlapm1")) return MG_TIME; /* and the finish's lap times */
    if (PRE("sp_mtr") || PRE("km_h") || ((PRE("sp1") || PRE("sp2") || PRE("sp3")) && n == 3)) return MG_SPEED;
    if (call == 2 && (PRE("cw1") || PRE("posc") || (PRE("c1") && n == 3))) return MG_ORDER;
    if (call == 2 && (PRE("course") || !n)) return MG_MAP;
#undef PRE
    return MG_NONE;
}
/* two players (p2_race.blo: the tags carry the player, 1 or 2): the group, and in *view the
 * player it follows (0, 1, or -1 for the shared map and race order) */
static int modern_group2(const char *t, int n, uint32_t call, int *view) {
#define PRE(p) (!strncmp(t, p, sizeof p - 1))
    *view = -1;
    if (call == 2 && (PRE("cw1") || PRE("posc") || (PRE("c1") && n == 3))) return MG2_ORDER;
    if (call == 2 && (PRE("course") || !n)) return MG2_MAP;
    for (int p = 0; p < 2; p++) {
        char d = (char)('1' + p);
        int g = -2;
        if (!strncmp(t, "pos", 3) && t[3] == d && n == 5) g = MG2_PLACE1;
        else if ((!strncmp(t, "slot", 4) && t[4] == d) || (!strncmp(t, "itm", 3) && t[3] == d)) g = MG2_ITEMS1;
        else if (t[0] == 'm' && t[1] == d && !strncmp(t + 2, "_lap", 4)) g = -1; /* the word LAP: the count alone */
        else if (PRE("ml") && t[2] == d && t[3] == '_') g = MG2_LAP1;
        else if (!strncmp(t, "time", 4) && t[4] == d && t[5] == '7' && n == 6) g = -1; /* the word TIME */
        else if ((!strncmp(t, "time", 4) && t[4] == d) || (PRE("Tl") && t[2] == d) || (PRE("Tlapm") && t[5] == d)) g = MG2_TIME1;
        if (g == -2) continue;
        *view = p;
        return g < 0 ? g : g + p * (MG2_PLACE2 - MG2_PLACE1);
    }
#undef PRE
    return MG_NONE;
}
/* three and four players (p4_race.blo: the tags carry the player, 1-4, view = player): as
 * modern_group2; the map only with four (three: it fills the fourth quarter, left as it is) */
static int modern_group4(const char *t, int n, uint32_t call, int *view) {
#define PRE(p) (!strncmp(t, p, sizeof p - 1))
    *view = -1;
    if (call == 2 && (PRE("course") || !n)) return s_hud_n == 4 ? MG4_MAP : MG_NONE;
    for (int p = 0; p < 4; p++) {
        char d = (char)('1' + p);
        int g = -1;
        if (!strncmp(t, "pos", 3) && t[3] == d && n == 5) g = MG4_PLACE1;
        else if ((!strncmp(t, "slot", 4) && t[4] == d) || (!strncmp(t, "itm", 3) && t[3] == d)) g = MG4_ITEMS1;
        else if ((t[0] == 'm' && t[1] == d && !strncmp(t + 2, "_lap", 4)) || (PRE("ml") && t[2] == d && t[3] == '_')) g = MG4_LAP1;
        if (g < 0) continue;
        *view = p;
        return g + 3 * p;
    }
#undef PRE
    return MG_NONE;
}
/* drawCourse's race order: the kart a picture belongs to (Race2D +0x4ca0 + 0x38 * kart: its two
 * heads' window and face pictures, then +0x4cb0 its rank number), or -1 */
static int order_kart(Ctx *c, uint8_t *M, uint32_t pic) {
    for (int k = 0; k < 8 && IS_RAM(s_race2d); k++)
        for (uint32_t o = 0; o <= 0x10; o += 4)
            if (ld32(c, M, s_race2d + 0x4ca0 + 0x38 * (uint32_t)k + o) == pic) return k;
    return -1;
}
/* The player's kart: RaceInfo (RaceMgr +0x38) +0x114, view 0's target kart, or -1. Read from the
 * race's data, not J2DManager's tables (their addresses differ in MKDD Plus's main.dol). */
static int view_kart(Ctx *c, uint8_t *M, int view) { /* view 0-3's target kart (+0x114 + 2 * view), or -1 */
    uint32_t rm = ld32(c, M, ctx_gpr(c, 13) - 0x5c38), info = IS_RAM(rm) ? ld32(c, M, rm + 0x38) : 0;
    int k = IS_RAM(info) && view >= 0 && view < 4 ? (int16_t)ld16(c, M, info + 0x114 + 2 * (uint32_t)view) : -1;
    return k >= 0 && k < 8 ? k : -1;
}
static int player_kart(Ctx *c, uint8_t *M) { return view_kart(c, M, 0); }
static bool kart_is_player(Ctx *c, uint8_t *M, int kart) { return kart >= 0 && kart == player_kart(c, M); }
/* The speed of the player's kart, as Race2D's getter (FUN_802b146c): the kart (r13 -0x4e98 +0xa0 + 4 *
 * the kart) +0x444 times a constant, or another constant while +0x578 has 0x200; 0 when not known. */
static uint32_t player_kart_obj(Ctx *c, uint8_t *M) { /* the player's kart object, 0 when not known */
    uint32_t mgr = ld32(c, M, (uint32_t)ctx_gpr(c, 13) - 0x4e98);
    int k = player_kart(c, M);
    uint32_t kart = IS_RAM(mgr) && k >= 0 ? ld32(c, M, mgr + 0xa0 + 4 * (uint32_t)k) : 0;
    return IS_RAM(kart) ? kart : 0;
}
static float hud_speed(Ctx *c, uint8_t *M) {
    uint32_t r2 = (uint32_t)RD(c, REG_R2, 4), kart = player_kart_obj(c, M);
    if (!kart) return 0;
    return ld32(c, M, kart + 0x578) & 0x200 ? ldf(c, M, r2 - 0x2a40) : ldf(c, M, r2 - 0x2a34) * ldf(c, M, kart + 0x444);
}
/* Single player: how far a kart's race order row has rolled from the 4th row (0) to where the
 * game parks ranks 5-8 below the screen (1), from its rank number (+0x4cb0: y 301 in the 4th row,
 * 466 parked; the player's row is drawn larger, 2.2 higher). Read from that picture's matrix
 * (this frame's or, before it is drawn, the last one's), so every picture of the row agrees. */
static float order_roll(Ctx *c, uint8_t *M, int kart) {
    if (kart < 0 || !s_hud_one || !IS_RAM(s_race2d)) return 0;
    uint32_t posc = ld32(c, M, s_race2d + 0x4cb0 + 0x38 * (uint32_t)kart);
    if (!IS_RAM(posc)) return 0;
    float r = (ldf(c, M, posc + 0x6c) - (kart_is_player(c, M, kart) ? 298.8f : 301.0f)) / 165.0f;
    return r > 0 ? (r < 1 ? r : 1) : 0; /* NaN: 0 */
}
/* DASHBOARD (g_hud_pills, the - menu's RACE HUD), one player: MODERN's way of moving the game's
 * pictures (gx.c MGT_*), on rounded translucent panels ("pills"), with the race order as a strip of
 * every kart along the bottom and a dial for the speed; the rear-view mirror is render_dk.c's. A white rounded square in the charpack pool
 * (IA8, alpha its coverage) makes the panels, 9 slices of it through drawK2D's own vertex format;
 * they go first in each of the race HUD's two draws (drawK2D), for the groups drawn the frame before. */
#define PILL_TEX 64
#define PILL_DARK 0x10141C00u          /* panel colour, | alpha at draw */
#define PILL_ALPHA 0xA6
#define PILL_ME 0xF8C81000u            /* the player's slot in the strip */
#define PILL_ME_ALPHA 0xE0
#define STRIP_SLOT 64.0f               /* the race order: a kart's slot, 62 wide x 40 from y 400 */
#define SPEED_PAD 7.0f                /* room left of the speed's digits for the dial */
#define LAP_GROW 1.36f                /* the word LAP at the height of the lap count */
#define LAP_DX 12.5f                  /* ... the count moved right for it */
#define ITEM_FRAME 33.0f               /* an item's frame (its picture, 90 x 90 with a margin) in the row */
enum { ROW_ITEMS, ROW_PLACE, ROW_LAP, ROW_TIME, ROW_SPEED, ROW_COINS, ROWS };
static const float ROW_W[ROWS] = {70, 33, 72 + LAP_DX, 108, 88.5f + SPEED_PAD, 70}; /* the panels' widths, 4 apart; the coins' (native.c coin_hud) */
#define MIRROR_PAD 10.0f /* layout units between the mirror and the row */
/* the mirror's right edge in layout units: mirror_draw's X1 (render_dk.c, render_gl.c: 97, or 190 squeezed
 * as the widescreen HUD's left side). Change them together */
#define MIRROR_EDGE_43 97.0f
#define MIRROR_EDGE_WIDE (608 * (0.5f + ((0.5f + (190.0f / 608 - 0.5f) * 0.75f - 0.125f) - 0.5f) / 0.75f))
static float s_row[ROWS];              /* ... where each starts this frame: the row is centred */
static float s_item_ref[2][3];         /* the two items' frames last drawn: middle, scale */
static float s_item_u[2] = {-0.5f, 0.5f}; /* ... their places in the panel (-0.5 left, 0.5 right): the game's larger (its front
                                       * window, the driver who uses the items; it swaps them as the drivers swap) on the left */
static float s_item_size[2], s_item_big, s_item_small; /* ... their sizes in the game's layout, the two seen together this race */
static uint32_t s_item_seen[2];        /* ... the frame each was last drawn */
static int s_item_side[2];             /* ... the side each was last given (for the log) */
static float s_speed_dx;               /* the speed's digits moved left to keep one or two centred, gently */
static float s_order_w[8][2];          /* each kart's two faces in the game's race order: their sizes (the driver's is larger) */
#define STRIP_Y 400.0f
#define SPEED_TOP 0.9f                 /* the dial at its end: this much of the kart's class top speed */
static const uint32_t METER_COL[7] = {0x00ffc3, 0x55ff00, 0xaaff00, 0xffff00, 0xffaa00, 0xff5400, 0xff2200}; /* the game's arc, its blocks in order */
static uint32_t s_meter_col[7];        /* ... their colours as the game set them last (+0x148), the boost's flash going round them */
static uint32_t s_meter_frame, s_meter_flash; /* ... when, and the frame the flash was last seen */
static uint32_t s_pill_obj;            /* GXTexObj, then the picture; 0: not made yet */
static bool s_pill_bad;
static float s_order_x[8];             /* each kart's slot, moving to its rank's */
static uint32_t s_order_frame, s_order_alpha;
static float s_map_x0, s_map_y0, s_map_x1, s_map_y1; /* the course's picture last drawn */
static float s_map_dy;                 /* DASHBOARD: the map moved so its panel's top meets the row's, wherever this course's picture starts */
static void pill_new_race(void) {
    memset(s_item_ref, 0, sizeof s_item_ref);
    s_item_u[0] = -0.5f, s_item_u[1] = 0.5f;
    memset(s_item_size, 0, sizeof s_item_size);
    memset(s_item_seen, 0, sizeof s_item_seen);
    memset(s_item_side, 0, sizeof s_item_side);
    s_item_big = s_item_small = 0;
    s_speed_dx = s_map_dy = 0;
    s_meter_flash = 0;
    s_pill_seen = s_pill_last = 0;
    coin_new_race();
}
static bool pill_tex(Ctx *c, uint8_t *M) {
    if (s_pill_obj) return true;
    if (s_pill_bad) return false;
    uint32_t size = PILL_TEX * PILL_TEX * 2, a = charpack_reserve(0x20 + size);
    if (!a || !IS_RAM(a + 0x20 + size - 1)) {
        s_pill_bad = true;
        rt_log("hud: no room for the panels' texture");
        return false;
    }
    uint8_t *d = M + ((a + 0x20) & RAM_MASK);
    for (int by = 0; by < PILL_TEX; by += 4)
        for (int bx = 0; bx < PILL_TEX; bx += 4)
            for (int y = by; y < by + 4; y++)
                for (int x = bx; x < bx + 4; x++) {
                    float dx = (float)x + 0.5f - PILL_TEX / 2.0f, dy = (float)y + 0.5f - PILL_TEX / 2.0f;
                    float cov = PILL_TEX / 2.0f - 1 - sqrtf(dx * dx + dy * dy) + 0.5f; /* 1 texel of soft edge */
                    *d++ = (uint8_t)(cov <= 0 ? 0 : cov >= 1 ? 255 : (int)(cov * 255)); /* IA8: alpha, then intensity */
                    *d++ = 255;
                }
    ctx_set_gpr(c, 3, a);
    ctx_set_gpr(c, 4, a + 0x20);
    ctx_set_gpr(c, 5, PILL_TEX);
    ctx_set_gpr(c, 6, PILL_TEX);
    ctx_set_gpr(c, 7, 3); /* GX_TF_IA8 */
    ctx_set_gpr(c, 8, 0);
    ctx_set_gpr(c, 9, 0);
    ctx_set_gpr(c, 10, 0);
    ctx_call(c, 0x800BF358u); /* GXInitTexObj */
    s_pill_obj = a;
    return true;
}
/* One rounded panel (layout units); r: its corners' radius. col: RGBA. */
static void pill_draw(Ctx *c, uint8_t *M, float x, float y, float w, float h, float r, uint32_t col) {
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    const float xs[4] = {x, x + r, x + w - r, x + w}, ys[4] = {y, y + r, y + h - r, y + h};
    /* 0x40: the texture's middle, u8 coordinates with 1.0 at 0x80 */
    const uint8_t ts[4] = {0, 0x40, 0x40, 0x80};
    n_GXBegin(c, M, 0x80, 0, 36, 0, 0, 0, 0, 0);
    uint8_t q[36 * 14], *o = q;
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++)
            for (int v = 0; v < 4; v++, o += 14) {
                int ix = i + (v == 1 || v == 2), iy = j + (v >= 2);
                uint32_t u;
                memcpy(&u, &xs[ix], 4);
                put_be(o, 4, u);
                memcpy(&u, &ys[iy], 4);
                put_be(o + 4, 4, u);
                put_be(o + 8, 4, col);
                o[12] = ts[ix];
                o[13] = ts[iy];
            }
    hw_gather_copy(q, sizeof q);
}
/* A line from (x0, y0) w0 wide to (x1, y1) w1 wide, square ended (layout units). col: RGBA. */
static void pill_line(Ctx *c, uint8_t *M, float x0, float y0, float x1, float y1, float w0, float w1, uint32_t col) {
    float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
    if (!(l > 0.01f)) return;
    float nx = -dy / l / 2, ny = dx / l / 2;
    const float v[4][2] = {{x0 + nx * w0, y0 + ny * w0}, {x1 + nx * w1, y1 + ny * w1}, {x1 - nx * w1, y1 - ny * w1}, {x0 - nx * w0, y0 - ny * w0}};
    n_GXBegin(c, M, 0x80, 0, 4, 0, 0, 0, 0, 0);
    uint8_t q[4 * 14], *o = q;
    for (int k = 0; k < 4; k++, o += 14) {
        uint32_t u;
        memcpy(&u, &v[k][0], 4);
        put_be(o, 4, u);
        memcpy(&u, &v[k][1], 4);
        put_be(o + 4, 4, u);
        put_be(o + 8, 4, col);
        o[12] = o[13] = 0x40; /* the texture's solid middle */
    }
    hw_gather_copy(q, sizeof q);
}
/* the picture being drawn's TEV setup as the game's plain picture with these TEV colours (the texture
 * between them, times the vertex colours), its fields put back after */
static void k2d_tev(Ctx *c, uint8_t *M, uint32_t self, uint32_t black, uint32_t white) {
    uint32_t c14c = ld32(c, M, self + 0x14c), c148 = ld32(c, M, self + 0x148), count = mld(c, M, self + 0x110, 1);
    mst(c, M, self + 0x14c, 4, black);
    mst(c, M, self + 0x148, 4, white);
    mst(c, M, self + 0x110, 1, 1);
    mst(c, M, self + 0xb3, 1, 0); /* not opaque: its mode that uses the vertex colours */
    vcall1(c, 0x8012cae4u, self); /* setTevModeK2D */
    mst(c, M, self + 0x14c, 4, c14c);
    mst(c, M, self + 0x148, 4, c148);
    mst(c, M, self + 0x110, 1, count);
}
static void k2d_identity(Ctx *c, uint8_t *M) {
    uint32_t old_sp = frame_push(c, M, 0x40), sp = old_sp - 0x40;
    const float id[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    stfn(c, M, sp + 8, id, 12);
    n_GXLoadPosMtxImm(c, M, sp + 8, 0, 0, 0, 0, 0, 0, 0);
    frame_pop(c, old_sp);
}
static void pill_zone(uint32_t z) {
    if (s_zone_now != z) hud_zone(s_zone_now = z);
}
static uint32_t pill_col(uint32_t rgb, uint32_t alpha, float e) { return (rgb & 0xFFFFFF00u) | (uint32_t)((float)alpha * e); }
/* The karts' ranks (KartChecker +0x88, 1 first, 0 none yet): each one's place in the strip, moved
 * towards its rank's so a pass slides; the race order's strength from its rank numbers. Returns how
 * many karts have one. */
static int order_tick(Ctx *c, uint8_t *M, int rank[8]) {
    uint32_t rm = ld32(c, M, ctx_gpr(c, 13) - 0x5c38);
    int n = 0;
    uint32_t alpha = 0;
    for (int k = 0; k < 8; k++) {
        uint32_t chk = IS_RAM(rm) ? ld32(c, M, rm + 0x48 + 4 * (uint32_t)k) : 0;
        int32_t r = IS_RAM(chk) && chk ? (int32_t)ld32(c, M, chk + 0x88) : 0;
        rank[k] = r >= 1 && r <= 8 ? r : 0;
        n += rank[k] != 0;
        uint32_t posc = IS_RAM(s_race2d) ? ld32(c, M, s_race2d + 0x4cb0 + 0x38 * (uint32_t)k) : 0;
        if (posc && IS_RAM(posc) && mld(c, M, posc + 0xb2, 1) > alpha) alpha = mld(c, M, posc + 0xb2, 1);
    }
    bool snap = s_order_frame + 1 != g_race_frames && s_order_frame != g_race_frames;
    if (s_order_frame != g_race_frames) {
        float x0 = 304 - STRIP_SLOT * (float)n / 2;
        for (int k = 0; k < 8; k++) {
            float to = x0 + STRIP_SLOT * (float)(rank[k] - 1);
            s_order_x[k] = snap ? to : s_order_x[k] + (to - s_order_x[k]) * 0.25f;
        }
        s_order_frame = g_race_frames;
    }
    s_order_alpha = alpha;
    return n;
}
#define PILL_SEEN(g) (s_pill_last >> ((g) - MGT_PLACE) & 1)
/* The panels of this draw of the race HUD (call: s_hud_call), through the picture being drawn's own
 * TEV setup as the game's plain picture, its fields put back after. */
static void pill_panels(Ctx *c, uint8_t *M, uint32_t self, float e) {
    if (!pill_tex(c, M)) return;
    k2d_tev(c, M, self, 0, 0xFFFFFFFFu);
    vcall1(c, 0x800bd5d8u, 1); /* GXSetNumTexGens */
    i_GXLoadTexObj(c, M, s_pill_obj, 0, 0, 0, 0, 0, 0, 0);
    k2d_identity(c, M);
    uint32_t dark = pill_col(PILL_DARK, PILL_ALPHA, e);
    if (s_hud_call == 2) {
        int rank[8];
        int n = order_tick(c, M, rank), me = player_kart(c, M);
        if (PILL_SEEN(MGT_ORDER) && n) {
            pill_zone(4);
            float a = e * (float)s_order_alpha / 255;
            for (int k = 0; k < 8; k++)
                if (rank[k]) pill_draw(c, M, s_order_x[k], STRIP_Y, STRIP_SLOT - 2, 40, 7, k == me ? pill_col(PILL_ME, PILL_ME_ALPHA, a) : pill_col(PILL_DARK, PILL_ALPHA, a));
        }
        if ((PILL_SEEN(MGT_MAP) || PILL_SEEN(MGT_MAP_BATTLE)) && s_map_x1 > s_map_x0) {
            pill_zone(6);
            pill_draw(c, M, s_map_x0 - 5, s_map_y0 - 4, s_map_x1 - s_map_x0 + 10, s_map_y1 - s_map_y0 + 8, 9, dark);
        }
        return;
    }
    float spd = hud_speed(c, M);
    int shown = (int)(g_speed_kmh ? spd / 2 : spd * 0.35f);
    float to = shown < 10 ? 13.5f : shown < 100 ? 6.75f : 0; /* half a digit per digit missing */
    s_speed_dx += (to - s_speed_dx) * 0.25f;
    if (!(s_speed_dx >= 0)) s_speed_dx = 0; /* NaN too */
    const float *wd = ROW_W;
    const bool on[ROWS] = {[ROW_ITEMS] = PILL_SEEN(MGT_ITEMS) || (!(s_hud_hide & HUD_ITEMS) && (PILL_SEEN(MGT_PLACE) || PILL_SEEN(MGT_LAP))), /* no item held: its panel still there, the row keeps still; none when turned off */
                           [ROW_PLACE] = PILL_SEEN(MGT_PLACE), [ROW_LAP] = PILL_SEEN(MGT_LAP), [ROW_TIME] = PILL_SEEN(MGT_TIME), [ROW_SPEED] = PILL_SEEN(MGT_SPEED),
                           [ROW_COINS] = s_coin_todo && (s_pill_last & ((2u << (MGT_SPEED - MGT_PLACE)) - 1))}; /* with any of the others */
    s_coin_row = on[ROW_COINS], s_coin_e = e;
    float total = -4;
    for (int i = 0; i < ROWS; i++) total += on[i] ? wd[i] + 4 : 0;
    float x = 304 - total / 2;
    /* the mirror (render_gl.c / render_dk.c mirror_draw: right edge 97 layout units, 190 with the
     * widescreen HUD's squeeze) at the top left: the row starts MIRROR_PAD right of it */
    if (g_hud_mirror) {
        float edge = g_hud_wide ? MIRROR_EDGE_WIDE : MIRROR_EDGE_43;
        if (x < edge + MIRROR_PAD) x = edge + MIRROR_PAD;
    }
    pill_zone(4);
    for (int i = 0; i < ROWS; i++)
        if (on[i]) {
            s_row[i] = x;
            pill_draw(c, M, x, 4, wd[i], 34, 11, dark); /* round y 21, as the pictures */
            x += wd[i] + 4;
        }
    if (on[ROW_SPEED]) {
        /* a dial: 7 ticks round 270 degrees from the bottom left in the game's arc's colours, lit to the
         * speed against the class's top (the kart +0x3f0: 126 at 50cc, 161 at 150cc; the game's arc
         * always ends at 150), a needle on it; while the game flashes its arc for a boost, all lit in its colours */
        uint32_t kart = player_kart_obj(c, M);
        float top = kart ? ldf(c, M, kart + 0x3f0) : 0;
        if (!(top >= 50 && top <= 400)) top = 150; /* NaN too */
        float f = spd / (top * SPEED_TOP);
        f = f > 0 ? f < 1 ? f : 1 : 0; /* NaN: 0 */
        int lit = (int)(f * 7 + 0.5f);
        bool fresh = s_meter_frame + 2 > g_race_frames && s_meter_frame <= g_race_frames;
        for (int i = 0; i < 7 && fresh; i++)
            if (s_meter_col[i] != METER_COL[i] && s_meter_col[i] != 0x808080) s_meter_flash = g_race_frames; /* moved round */
        bool flash = fresh && s_meter_flash && s_meter_flash <= g_race_frames && g_race_frames - s_meter_flash < 10;
        const float A0 = 3.9269908f, SWEEP = 4.712389f, RO = 17, RI = 12; /* 225 degrees, 270 */
        float cx = s_row[ROW_SPEED] + 4 + RO, cy = 23.5f; /* its arc's middle at y 21 */
        for (int i = 0; i < 24; i++) { /* the rim */
            float a0 = A0 - SWEEP * (float)i / 24, a1 = A0 - SWEEP * (float)(i + 1) / 24;
            pill_line(c, M, cx + RO * cosf(a0), cy - RO * sinf(a0), cx + RO * cosf(a1), cy - RO * sinf(a1), 0.9f, 0.9f, pill_col(0xFFFFFF00u, 0x60, e));
        }
        for (int i = 0; i < 7; i++) {
            float a = A0 - SWEEP * (float)i / 6, ca = cosf(a), sa = sinf(a);
            pill_line(c, M, cx + RI * ca, cy - RI * sa, cx + RO * ca, cy - RO * sa, 2.4f, 2.4f, flash ? pill_col(s_meter_col[i] << 8, 0xFF, e) : pill_col(METER_COL[i] << 8, i < lit ? 0xFF : 0x80, e));
        }
        float a = A0 - SWEEP * f;
        pill_line(c, M, cx, cy, cx + 14.5f * cosf(a), cy - 14.5f * sinf(a), 3, 0.9f, pill_col(0xFFFFFF00u, 0xFF, e));
        pill_draw(c, M, cx - 2.5f, cy - 2.5f, 5, 5, 2.5f, pill_col(0xFFFFFF00u, 0xFF, e));
    }
}
/* The race order's picture in the strip, in that kart's slot: the number in the corner, the larger
 * face (the game's sizes, c* over its cw* outline) and the smaller beside it. The game's own places
 * aren't used: its rows are of different sizes and it turns the two faces round each other when a
 * kart's drivers swap. false: not drawn (a kart with no rank). */
static bool order_strip(Ctx *c, uint8_t *M, const char *t, int n, int kart, float m[12], float w) {
    uint32_t posc = kart >= 0 && IS_RAM(s_race2d) ? ld32(c, M, s_race2d + 0x4cb0 + 0x38 * (uint32_t)kart) : 0;
    if (!posc || !IS_RAM(posc) || w <= 1 || n < 1) return false;
    float s = g_hud_move[MGT_ORDER].s, x = s_order_x[kart], y = STRIP_Y, size;
    if (!strncmp(t, "posc", 4)) {
        size = 19, x += 1.5f, y += 1;
    } else {
        int who = t[n - 1] == '2';
        s_order_w[kart][who] = w;
        bool big = w >= s_order_w[kart][!who];
        size = big ? 51 : 40.8f;
        x += 8 + s * (big ? 0 : 38.1f), y += 3 + s * (big ? 0 : 16.1f);
        if (t[1] != 'w') x += s * size * 4 / 51, y -= s * size * 3 / 51; /* c*: as far from its outline as the game has it */
    }
    float k = s * size / w;
    memset(m, 0, 12 * sizeof *m);
    m[0] = m[5] = k, m[10] = 1;
    m[3] = x, m[7] = y;
    return true;
}
/* The picture's matrix (12 floats) moved for its group; false: not drawn (MODERN shows nothing
 * that sits off the screen, so parked pictures don't slide in from the middle). The race order
 * moves a rolling row down by one row instead of off the screen: the player's lands in a 5th
 * row and stays, the others fade there (drawK2D). dx, dy: the picture moved within its group
 * first (layout units). */
static bool modern_place(Ctx *c, uint8_t *M, int g, int kart, float roll, float dx, float dy, float m[12], float w, float h) {
    if (g == MG_ORDER && roll > 0) {
        if (roll >= 0.999f && !kart_is_player(c, M, kart)) return false;
        m[7] += ORDER_ROW5_DY * roll;
    }
    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
    for (int k = 0; k < 4; k++) {
        float u = k & 1 ? w : 0, v = k & 2 ? h : 0, x = m[0] * u + m[1] * v + m[3], y = m[4] * u + m[5] * v + m[7];
        x0 = x < x0 ? x : x0, x1 = x > x1 ? x : x1, y0 = y < y0 ? y : y0, y1 = y > y1 ? y : y1;
    }
    if (x1 <= 0 || x0 >= 608 || y1 <= 0 || y0 >= 448) return false;
    m[3] += dx, m[7] += dy;
    float s = g_hud_move[g].s;
    for (int k = 0; k < 3; k++) m[k] *= s, m[4 + k] *= s;
    m[3] = g_hud_move[g].nx + s * (m[3] - g_hud_move[g].ox);
    m[7] = g_hud_move[g].ny + s * (m[7] - g_hud_move[g].oy);
    if (g == MG2_PLACE1 || g == MG2_PLACE2) {
        float top = g_hud_move[g].ny + s * (y0 + dy - g_hud_move[g].oy), bottom = top + s * (y1 - y0);
        if (top < PLACE2_TOP) m[7] += PLACE2_TOP - top;
        else if (bottom > PLACE2_BOTTOM) m[7] -= bottom - PLACE2_BOTTOM;
    }
    return true;
}
/* DASHBOARD: a picture's matrix moved for its group, as modern_place, into its panel of the row. The
 * lap count's total at the size of the lap, the unit turned. The items are fitted by their frames
 * (slot1f / slot1b, the game animates and sizes them), last frame's, both the same size: the game's
 * larger one (the driver who uses the items) in the panel's left half, the other in the right. */
static bool pill_place(Ctx *c, uint8_t *M, uint32_t self, int g, int kart, float m[12], float w, float h) {
    char t[9];
    int n = pane_tag(c, M, self, t);
    if (g == MGT_ORDER) return order_strip(c, M, t, n, kart, m, w);
    if (g == MGT_ITEMS) {
        int back = t[0] == 's' ? t[5] == 'b' : t[4] == 'b';
        float *ref = s_item_ref[back];
        if (t[0] == 's' && n == 6) {
            float sc = sqrtf(m[0] * m[0] + m[4] * m[4]);
            if (sc > 0.05f && w > 1) {
                ref[0] = m[3] + (m[0] * w + m[1] * h) / 2;
                ref[1] = m[7] + (m[4] * w + m[5] * h) / 2;
                ref[2] = ITEM_FRAME / (sc * w);
                float size = sc * w, other = s_item_size[!back];
                s_item_size[back] = size, s_item_seen[back] = g_race_frames;
                bool both = s_item_seen[!back] && s_item_seen[!back] + 1 >= g_race_frames, front;
                if (both && size > 2 && other > 2) s_item_big = fmaxf(size, other), s_item_small = fminf(size, other);
                if (both) front = size > other + 0.5f || (!(fabsf(size - other) > 0.5f) && !back); /* the same: by tag */
                else front = !(s_item_big > s_item_small + 0.5f) || size > (s_item_big + s_item_small) / 2; /* alone: by the sizes seen together */
                float to = front ? -0.5f : 0.5f;
                if (s_item_side[back] != (front ? 1 : 2)) {
                    s_item_side[back] = front ? 1 : 2;
                    rt_log("hud: item %c %s: size %.1f (other %.1f%s, seen %.1f / %.1f) x %.1f y %.1f", back ? 'b' : 'f', front ? "left" : "right", size, other, both ? "" : " gone", s_item_big, s_item_small, ref[0], ref[1]);
                }
                s_item_u[back] += (to - s_item_u[back]) * 0.3f;
                if (!(s_item_u[back] >= -0.5f && s_item_u[back] <= 0.5f)) s_item_u[back] = to;
            }
        }
        float s = ref[2];
        if (s <= 0) return false; /* no frame yet */
        bool trick = s_trick_hud && s_th_coin_x1 > 0; /* trick_item_place: right of the coins panel */
        float gk = trick ? TH_ITEM_K : 1; /* the trick HUD's: as tall as the coins panel, the same shape */
        s *= gk;
        for (int k = 0; k < 3; k++) m[k] *= s, m[4 + k] *= s;
        g_item_band_dy = trick ? TH_Y + TH_H / 2 - 21 : 0; /* gx.c modern_rect: the frames' clip band moved with them */
        g_item_band_k = gk;                                /* ... and grown with them */
        m[3] = (trick ? s_th_coin_x1 + 6 : s_row[ROW_ITEMS]) + gk * (35 + 31 * s_item_u[back]) + s * (m[3] - ref[0]); /* their margins touching */
        m[7] = (trick ? TH_Y + TH_H / 2 : 21) + s * (m[7] - ref[1]);
        return true;
    }
    float dx = 0, dy = 0;
    if (g == MGT_LAP && !strcmp(t, "ml1_2")) {
        const float k = 32.0f / 17;
        m[0] *= k, m[5] *= k;
        dx = 116 - m[3], dy = 384 - m[7];
    } else if (g == MGT_LAP && !strcmp(t, "ml1_s1")) dy = -8;
    if (!modern_place(c, M, g, kart, 0, dx, dy, m, w, h)) return false;
    if (g == MGT_PLACE) m[3] += s_row[ROW_PLACE];
    else if (g == MGT_LAP) {
        m[3] += s_row[ROW_LAP];
        if (!strcmp(t, "m1_lap")) { /* the word: grown round its middle, level with the count */
            for (int k = 0; k < 3; k++) m[k] *= LAP_GROW, m[4 + k] *= LAP_GROW;
            m[7] -= (LAP_GROW - 1) * m[5] / LAP_GROW * h / 2 + 0.9f;
        } else m[3] += LAP_DX;
    }
    else if (g == MGT_TIME) m[3] += s_row[ROW_TIME];
    else if (g == MGT_SPEED || g == MGT_UNIT) {
        m[3] += s_row[ROW_SPEED] + SPEED_PAD - (g == MGT_SPEED ? s_speed_dx : 0);
        if (g == MGT_UNIT) { /* reading downwards */
            float s = m[0];
            m[0] = 0, m[1] = -s, m[4] = s, m[5] = 0;
            m[3] += s * h;
        }
    } else if (g == MGT_MAP) m[7] += s_map_dy;
    return true;
}
/* Two players side by side: a picture of no group (vs. MGV_*) moved from its screen's half (top: the
 * left column, bottom: the right) into that column at VS_SCALE, keeping its distance to the half's
 * nearest edge (thirds: left / middle / right, top / middle / bottom) scaled, so pieces side by side
 * stay together. Bob-omb Blast's stars (right) all keep to the right edge: a row of them spans two thirds.
 * 1 moved, 0 as it is (off the screen), -1 not drawn (a line across the screen: the top and bottom
 * screens' divider). */
#define VS_SCALE 0.6f
static int vsplit_place(float m[12], float w, float h, bool right) {
    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
    for (int k = 0; k < 4; k++) {
        float u = k & 1 ? w : 0, v = k & 2 ? h : 0, x = m[0] * u + m[1] * v + m[3], y = m[4] * u + m[5] * v + m[7];
        x0 = x < x0 ? x : x0, x1 = x > x1 ? x : x1, y0 = y < y0 ? y : y0, y1 = y > y1 ? y : y1;
    }
    if (!(x1 > 0 && x0 < 608 && y1 > 0 && y0 < 448)) return 0; /* NaN too */
    if (x1 - x0 > 400 && y1 - y0 < 16) return -1;
    int v = y0 + y1 < 448 ? 0 : 1;
    float s = VS_SCALE, col = v ? 305.0f : 0.0f, ly0 = y0 - 224.0f * (float)v, cx = (x0 + x1) / 2, cy = (y0 + y1) / 2 - 224.0f * (float)v;
    float nx = right ? col + 303 - (608 - x0) * s : cx < 608.0f / 3 ? col + x0 * s : cx > 608.0f * 2 / 3 ? col + 303 - (608 - x0) * s : col + 151.5f + (x0 - 304) * s;
    float ny = cy < 224.0f / 3 ? ly0 * s : cy > 224.0f * 2 / 3 ? 448 - (224 - ly0) * s : 224 + (ly0 - 112) * s;
    for (int k = 0; k < 3; k++) m[k] *= s, m[4 + k] *= s;
    m[3] = nx + s * (m[3] - x0);
    m[7] = ny + s * (m[7] - y0);
    return 1;
}
/* KMH: the US game shows mph (Race2D::setLayoutData, flag r13 -0x68dc: speed * 0.35); km/h is
 * its other unit, speed / 2. Before Race2D::draw, the three digits (+0x9fc, +0xa00, +0xa04: shown
 * from the first non-zero one) get that number's textures (deg0-9.bti at +0x188) with
 * changeTexture (vtable +0x110), as setLayoutData does. The label is pause_art.c's (S_KMH). */
static void hud_kmh(Ctx *c, uint8_t *M, uint32_t self) {
    if (!IS_RAM(self)) return;
    float sp = hud_speed(c, M);
    int v = sp > 0 && sp < 4000 ? (int)sp / 2 : 0;
    if (v > 999) v = 999;
    if (s_hud_logging) HUDLOG("kmh speed %.2f -> %d (game mph %d)", sp, v, (int)(sp * 0.35f));
    const int d[3] = {v / 100, v / 10 % 10, v % 10};
    const bool show[3] = {v >= 100, v >= 10, true};
    for (int i = 0; i < 3; i++) {
        uint32_t pic = ld32(c, M, self + 0x9fc + 4 * (uint32_t)i), tex = ld32(c, M, self + 0x188 + 4 * (uint32_t)d[i]);
        if (!IS_RAM(pic)) continue;
        mst(c, M, pic + 0xb0, 1, show[i]);
        if (!show[i] || !IS_RAM(tex)) continue;
        ctx_set_gpr(c, 3, pic);
        ctx_set_gpr(c, 4, tex);
        ctx_set_gpr(c, 5, 0);
        ctx_call(c, ld32(c, M, ld32(c, M, pic) + 0x110)); /* changeTexture(ResTIMG *, 0) */
    }
}

/* MKDD Plus colours the LAP counter and timer purple to red (single player: corners a84fb0 top,
 * bb3f57 bottom); they get the game's own colours back, corner by corner (the four words
 * getNewColor wrote: top left, top right, bottom left, bottom right), alpha kept. Any number of
 * players (the tags carry the player: m1_lap, ml2_1, time37, Tl41, Tlapm2), and any purple to
 * pink (green well under red, under blue), which the game's yellow, green and orange never are,
 * so its own colour effects stay. */
static void plus_colours(Ctx *c, uint8_t *M, uint32_t self, uint32_t col) {
    static const uint32_t LAP[4] = {0xffff64, 0xffff64, 0x00d200, 0x00d200}, TIME[4] = {0xffff80, 0xff6500, 0xff4e00, 0xffff80};
    char t[9];
    pane_tag(c, M, self, t);
#define D(i) (t[i] >= '1' && t[i] <= '4')
    bool lap = (t[0] == 'm' && D(1) && !strncmp(t + 2, "_lap", 4)) || (t[0] == 'm' && t[1] == 'l' && D(2) && t[3] == '_');
    bool time = (!strncmp(t, "time", 4) && D(4)) || (t[0] == 'T' && t[1] == 'l' && D(2)) || (!strncmp(t, "Tlapm", 5) && D(5));
#undef D
    if (!lap && !time) return;
    const uint32_t *stock = lap ? LAP : TIME;
    for (uint32_t k = 0; k < 4; k++) {
        uint32_t v = ld32(c, M, col + 4 * k), r = v >> 24, g = v >> 16 & 0xFF, b = v >> 8 & 0xFF;
        if (g + 0x40 <= r && g < b && b >= 0x40) mst(c, M, col + 4 * k, 4, stock[k] << 8 | (v & 0xFF));
    }
}
/* DASHBOARD: the place gold, silver and bronze in the first three (the game's top to bottom gradient
 * replaced corner by corner, alpha kept); 4th and after as the game colours them. */
static void place_colours(Ctx *c, uint8_t *M, uint32_t self, uint32_t col) {
    static const uint32_t TOP[3] = {0xffe878, 0xffffff, 0xffc890}, BOTTOM[3] = {0xe89a00, 0x9aa6b8, 0xb8622a};
    char t[9];
    if (pane_tag(c, M, self, t) != 5 || strncmp(t, "pos1", 4) || t[4] < '1' || t[4] > '3') return;
    int r = t[4] - '1';
    for (uint32_t k = 0; k < 4; k++) mst(c, M, col + 4 * k, 4, (k < 2 ? TOP[r] : BOTTOM[r]) << 8 | (ld32(c, M, col + 4 * k) & 0xFF));
}
/* MODERN speedometer: the arc's 7 blocks (sp_mtr1 at the bottom left, then 7, 6 .. 2 at the top
 * right) tinted green to yellow to red, multiplying the game's colours (alpha kept), by e. */
static void speed_tint(Ctx *c, uint8_t *M, uint32_t self, uint32_t col, float e) {
    static const uint32_t TINT[7] = {0x50e050, 0x98e840, 0xd8f038, 0xffe030, 0xffb028, 0xff7820, 0xff4020};
    char t[9];
    if (pane_tag(c, M, self, t) != 7 || strncmp(t, "sp_mtr", 6) || t[6] < '1' || t[6] > '7') return;
    uint32_t tint = TINT[t[6] == '1' ? 0 : 8 - (t[6] - '0')];
    for (uint32_t k = 0; k < 4; k++) {
        uint32_t v = ld32(c, M, col + 4 * k), out = v & 0xFF;
        for (int s = 8; s < 32; s += 8) {
            float ch = (float)(v >> s & 0xFF), tc = (float)(tint >> (s - 8) & 0xFF) / 255.0f;
            out |= (uint32_t)(ch * (1 - e + e * tc) + 0.5f) << s;
        }
        mst(c, M, col + 4 * k, 4, out);
    }
}
/* While MODERN slides back to the game's layout: the side of the widescreen HUD a picture of the
 * game's layout lands on (gx.c: by the middle of its draw, the map routine's at the right unless
 * at the left): -1 left, 0 middle, 1 right. */
static int classic_side(const float m[12], float w, float h) {
    float mid = (m[0] * w * 0.5f + m[1] * h * 0.5f + m[3]) / 304.0f - 1;
    return mid < -0.2f ? -1 : mid > 0.2f || s_hud_call == 2 ? 1 : 0;
}


/* ---- batch 1: 2D pictures (race HUD) ---- */
/* drawCourse's pictures of a driver: the race order's two heads per kart (Race2D +0x4ca4 / +0x4cac
 * + 0x38 * kart: c11 / c12) and the map's icon per kart (+0x1130 + 4 * kart). A character is one
 * texture for every kart (race2d.arc's), so while some driver races as a variant of it the picture
 * draws its own copy for that kart and head instead (charpack_hud_copy: its JUTTexture's GXTexObj
 * +0xc pointed there while it loads, *at / *was to put back), drawn as that kart's driver of the
 * character (charpack_race_face) whichever was loaded last. */
static void hud_face(Ctx *c, uint8_t *M, uint32_t self, uint32_t *at, uint32_t *was) {
    if (!IS_RAM(s_race2d)) return;
    int kart = -1, slot = -1;
    for (int k = 0; k < 8 && kart < 0; k++) {
        uint32_t row = s_race2d + 0x4ca4 + 0x38 * (uint32_t)k;
        if (ld32(c, M, row) == self) kart = k, slot = 0;
        else if (ld32(c, M, row + 8) == self) kart = k, slot = 1;
        else if (ld32(c, M, s_race2d + 0x1130 + 4 * (uint32_t)k) == self) kart = k;
    }
    uint32_t jt = kart >= 0 ? ld32(c, M, self + 0x100) : 0, timg = IS_RAM(jt) ? ld32(c, M, jt + 0x20) : 0;
    if (!IS_RAM(timg)) return;
    uint32_t fmt = mld(c, M, timg, 1), w = ld16(c, M, timg + 2), h = ld16(c, M, timg + 4);
    uint32_t img3 = ld32(c, M, jt + 0xc), addr = (img3 & 0x1FFFFFu) << 5;
    if (addr != ((timg + ld32(c, M, timg + 0x1c)) & RAM_MASK)) return; /* not its ResTIMG's picture: left alone */
    uint32_t id = gx_subst_at(addr, fmt, w, h), rel = id - GX_SUBST_CHARPACK;
    if (id < GX_SUBST_CHARPACK || rel >= 2 * CP_BASES) return; /* a character's face (K_FACE) or map icon (K_MAP) */
    int kind = rel < CP_BASES ? CP_HUD_FACE : CP_HUD_MAP, b = (int)(rel % CP_BASES);
    bool b0 = dm_back(c, M, g_cp_dm[kart][0]), b1 = dm_back(c, M, g_cp_dm[kart][1]);
    charpack_race_front(kart, b0 ? 1 : b1 ? 0 : 1, b0 != b1);
    int v = charpack_race_face(kart, slot, b);
    if (v == CP_RACE_NONE || (v < 0 && charpack_show(b) < 0)) return; /* drawn as the game's anyway */
    size_t size = gx_tex_size(fmt, w, h);
    uint32_t copy = 0;
    int i = size && size <= 0x8000 ? charpack_hud_copy(kind, kart, slot, b, (uint32_t)size, &copy) : -1;
    if (i < 0 || !IS_RAM(copy) || !IS_RAM(copy + size - 1) || addr + size > RAM_SIZE) return;
    uint8_t *dst = M + (copy & RAM_MASK), *src = M + addr;
    if (memcmp(dst, src, size)) memcpy(dst, src, size);
    gx_subst_addr(copy, CP_HUD_SUBST + (unsigned)i);
    *at = jt + 0xc, *was = img3;
    mst(c, M, jt + 0xc, 4, (img3 & ~0x1FFFFFu) | ((copy & RAM_MASK) >> 5));
}
/* The coin counters (coins.c, HUD ASSETS' COINS), one a screen: the coins its kart holds, in
 * the game's own digits (race2d.arc mariofont_number_0 .. 9, IA4 32 x 32: the lap count's, found by
 * name through JKRGetNameResource's JKRFileLoader::getGlbResource(name, NULL), every mounted archive)
 * and a coin of the panels' texture to their right, white, the size of the lap count's digit (ml1_1; CLASSIC the time's).
 * From the pictures drawn the frame before, so it follows them as they slide: CLASSIC left of the
 * time (time1*: TIME and its digits; right of the lap count when the time is not drawn);
 * MODERN left of the lap count (m1_lap, ml1_*), as far from it as the time (time1*) is on its right;
 * DASHBOARD in a panel of its own after the speed's (ROW_COINS). Drawn at the race HUD's first piece (Race2D::draw), through
 * that picture's TEV setup as the panels; its own drawing puts its state back after. */
#define G_getGlbResource 0x80014594u
#define CDIG_W 32                      /* a digit's texture: 32 x 32 */
#define COIN_ICON 0.8f                 /* the coin's size: the digits' drawn height times this */
typedef struct {
    bool on;
    float x0, y0, x1, y1;
} CoinBox;
typedef struct {
    bool ok;                           /* the lap's digit seen */
    CoinBox lap, time;                 /* the lap's, the time's pictures together */
    float dy0, dy1;                    /* the lap's digit's picture (top, bottom) */
    float th, wx0;                     /* the time's digits' height (its tallest picture but the word); the word's left (0: not drawn) */
    uint32_t alpha, zone, time_zone;   /* ... its alpha and HUD zone; the time's zone */
} CoinAnchor;
static CoinAnchor s_ca_now[4], s_ca_last[4]; /* by view: this frame's, the frame before's */
static uint32_t s_cdig_obj;            /* 10 GXTexObj (0x20 each) in the charpack pool, 0: not made yet */
static uint32_t s_cdig_timg[10];       /* the digits' ResTIMG this race (0: not found yet) */
static bool s_cdig_tried;              /* ... looked for this race */
static uint8_t s_cdig_x0[10], s_cdig_x1[10], s_cdig_y0, s_cdig_y1; /* the digits' drawn columns, rows (alpha) */

static void gtex_new_race(void);
static void coin_new_race(void) {
    gtex_new_race();
    memset(s_cdig_timg, 0, sizeof s_cdig_timg);
    s_cdig_tried = false;
    memset(s_ca_now, 0, sizeof s_ca_now), memset(s_ca_last, 0, sizeof s_ca_last);
}
static bool cdig_valid(Ctx *c, uint8_t *M, uint32_t t) {
    return IS_RAM(t) && IS_RAM(t + 0x20) && mld(c, M, t, 1) == 2 && ld16(c, M, t + 2) == CDIG_W && ld16(c, M, t + 4) == CDIG_W &&
           IS_RAM(t + ld32(c, M, t + 0x1c)) && IS_RAM(t + ld32(c, M, t + 0x1c) + CDIG_W * CDIG_W - 1);
}
/* the 10 digits found and their texture objects made, once a race (again if one stops being a digit) */
static bool coin_digits(Ctx *c, uint8_t *M) {
    bool all = true;
    for (int d = 0; d < 10 && all; d++) all = s_cdig_timg[d] && cdig_valid(c, M, s_cdig_timg[d]);
    if (all) return true;
    if (s_cdig_tried) return false;
    s_cdig_tried = true;
    if (!s_cdig_obj) s_cdig_obj = charpack_reserve(10 * 0x20);
    if (!s_cdig_obj || !IS_RAM(s_cdig_obj + 10 * 0x20 - 1)) {
        s_cdig_obj = 0;
        rt_log("coins: no room for the counter's digits");
        return false;
    }
    s_cdig_y0 = CDIG_W - 1, s_cdig_y1 = 0;
    for (int d = 0; d < 10; d++) {
        char name[32];
        snprintf(name, sizeof name, "mariofont_number_%d.bti", d);
        uint32_t old = frame_push(c, M, 0x40), sp = old - 0x40;
        for (size_t i = 0; i <= strlen(name); i++) mst(c, M, sp + 8 + (uint32_t)i, 1, (uint8_t)name[i]);
        const uint32_t a[] = {sp + 8, 0};
        uint32_t t = ctx_call_guest(c, G_getGlbResource, a, 2);
        frame_pop(c, old);
        if (!cdig_valid(c, M, t)) {
            memset(s_cdig_timg, 0, sizeof s_cdig_timg);
            rt_log("coins: %s not found (%08x): no counter", name, t);
            return false;
        }
        s_cdig_timg[d] = t;
        /* IA4: a byte a texel (alpha the high nibble) in 8 x 4 tiles */
        uint32_t img = t + ld32(c, M, t + 0x1c);
        int x0 = CDIG_W, x1 = -1;
        for (int y = 0; y < CDIG_W; y++)
            for (int x = 0; x < CDIG_W; x++) {
                uint32_t at = (uint32_t)(((y / 4) * (CDIG_W / 8) + x / 8) * 32 + (y % 4) * 8 + x % 8);
                if (mld(c, M, img + at, 1) >> 4 < 3) continue;
                x0 = x < x0 ? x : x0, x1 = x > x1 ? x : x1;
                if (y < s_cdig_y0) s_cdig_y0 = (uint8_t)y;
                if (y > s_cdig_y1) s_cdig_y1 = (uint8_t)y;
            }
        if (x1 < x0) x0 = 4, x1 = CDIG_W - 5; /* empty: a plain width */
        s_cdig_x0[d] = (uint8_t)x0, s_cdig_x1[d] = (uint8_t)x1;
        uint32_t obj = s_cdig_obj + 0x20 * (uint32_t)d;
        const uint32_t init[] = {obj, img, CDIG_W, CDIG_W, 2 /* GX_TF_IA4 */, 0, 0, 0};
        ctx_call_guest(c, 0x800BF358u, init, 8); /* GXInitTexObj */
    }
    if (s_cdig_y1 < s_cdig_y0) s_cdig_y0 = 4, s_cdig_y1 = CDIG_W - 5;
    rt_log("coins: counter digits found (0 at %08x, rows %u-%u)", s_cdig_timg[0], s_cdig_y0, s_cdig_y1);
    return true;
}
/* one textured quad (layout units), corner colours top left, top right, bottom right, bottom left */
static void k2d_quad(Ctx *c, uint8_t *M, float x, float y, float w, float h, const uint32_t col[4]) {
    const float xs[4] = {x, x + w, x + w, x}, ys[4] = {y, y, y + h, y + h};
    const uint8_t ts[4] = {0, 0x80, 0x80, 0}, tt[4] = {0, 0, 0x80, 0x80};
    n_GXBegin(c, M, 0x80, 0, 4, 0, 0, 0, 0, 0);
    uint8_t q[4 * 14], *o = q;
    for (int v = 0; v < 4; v++, o += 14) {
        uint32_t u;
        memcpy(&u, &xs[v], 4);
        put_be(o, 4, u);
        memcpy(&u, &ys[v], 4);
        put_be(o + 4, 4, u);
        put_be(o + 8, 4, col[v]);
        o[12] = ts[v];
        o[13] = tt[v];
    }
    hw_gather_copy(q, sizeof q);
}
/* view's counter wanted this race HUD draw: this race has coins (GAME > COINS at its start), not turned off, its kart
 * known and still racing (gone at its GOAL: the lap count it follows goes, the finish's TIME and results move the
 * pictures it would follow elsewhere) */
static bool coin_wanted(Ctx *c, uint8_t *M, int view) {
    return coins_racing() && !(s_hud_hide & HUD_COINS) && view_kart(c, M, view) >= 0 && view_racing(c, M, view);
}
/* drawK2D, a picture's final matrix (mtx) and colours (cols: drawK2D's four): the lap's, kept for the counter */
static void box_add(CoinBox *b, float x0, float y0, float x1, float y1) {
    if (!b->on) b->on = true, b->x0 = x0, b->y0 = y0, b->x1 = x1, b->y1 = y1;
    b->x0 = fminf(b->x0, x0), b->y0 = fminf(b->y0, y0), b->x1 = fmaxf(b->x1, x1), b->y1 = fmaxf(b->y1, y1);
}
static void coin_seen(Ctx *c, uint8_t *M, uint32_t self, uint32_t mtx, float w, float h, uint32_t cols) {
    char t[9];
    pane_tag(c, M, self, t);
    /* the tags carry the player (1-4): mlN_* the lap count, mN_lap the word LAP, timeN* the time */
    bool lap = (!strncmp(t, "ml", 2) && t[3] == '_') || (t[0] == 'm' && !strcmp(t + 2, "_lap")), tim = !strncmp(t, "time", 4);
    char d = lap ? (t[1] == 'l' ? t[2] : t[1]) : tim ? t[4] : 0;
    if (d < '1' || d > '4') return;
    CoinAnchor *a = &s_ca_now[d - '1'];
    CoinBox *b = lap ? &a->lap : &a->time; /* the word TIME too (CLASSIC) */
    bool word = tim && t[5] == '7' && !t[6]; /* timeN7: the word TIME */
    float m[12];
    ldfn(c, M, mtx, m, 12);
    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
    for (int k = 0; k < 4; k++) {
        float u = k & 1 ? w : 0, v = k & 2 ? h : 0, x = m[0] * u + m[1] * v + m[3], y = m[4] * u + m[5] * v + m[7];
        x0 = x < x0 ? x : x0, x1 = x > x1 ? x : x1, y0 = y < y0 ? y : y0, y1 = y > y1 ? y : y1;
    }
    if (!(x0 > -1000 && x1 < 2000 && y0 > -1000 && y1 < 2000)) return; /* NaN too */
    box_add(b, x0, y0, x1, y1);
    if (b == &a->time) a->time_zone = s_zone_now & 0xFF, a->th = !word ? fmaxf(a->th, y1 - y0) : a->th, a->wx0 = !word ? a->wx0 : x0;
    if (!(t[0] == 'm' && t[1] == 'l' && t[3] == '_' && t[4] == '1' && !t[5])) return; /* mlN_1: the lap's digit */
    a->ok = true, a->dy0 = y0, a->dy1 = y1;
    a->alpha = ld32(c, M, cols) & 0xFF, a->zone = s_zone_now & 0xFF; /* its top left corner's */
}
/* Race2D::draw starting (race_hud_call): last frame's lap kept, the counter to draw this one */
static void coin_frame_start(Ctx *c, uint8_t *M, int n) {
    memcpy(s_ca_last, s_ca_now, sizeof s_ca_now);
    memset(s_ca_now, 0, sizeof s_ca_now);
    s_coin_todo = 0;
    for (int v = 0; v < n && v < 4 && !s_trick_hud; v++) s_coin_todo |= (uint32_t)coin_wanted(c, M, v) << v;
    s_coin_track = s_coin_todo != 0;
    s_coin_row = false;
}
/* the counter (layout units): its digits' quads kq x CDIG_W wide each, from x; ytop their top */
static float digits_width(const char *s, float kq) { /* the digits of s side by side, kq layout units a texel */
    float w = 0;
    for (const char *p = s; *p; p++)
        if (*p >= '0' && *p <= '9') w += (float)(s_cdig_x1[*p - '0'] - s_cdig_x0[*p - '0'] + 1) * kq;
    return w;
}
static float coin_width(const char *s, float kq) {
    float d = (float)(s_cdig_y1 - s_cdig_y0 + 1) * kq * COIN_ICON; /* the coin */
    return digits_width(s, kq) + 0.12f * d + d;
}
/* the digits of s from x, their rows' middle at cy, tinted col (RGBA); returns where they end. The digits' texture objects: coin_digits */
static float digits_draw(Ctx *c, uint8_t *M, const char *s, float x, float cy, float kq, uint32_t col) {
    const uint32_t cols[4] = {col, col, col, col};
    float ytop = cy - (float)(s_cdig_y0 + s_cdig_y1 + 1) / 2 * kq;
    for (const char *p = s; *p; p++) {
        int d = *p - '0';
        if (d < 0 || d > 9) continue;
        i_GXLoadTexObj(c, M, s_cdig_obj + 0x20 * (uint32_t)d, 0, 0, 0, 0, 0, 0, 0);
        k2d_quad(c, M, x - (float)s_cdig_x0[d] * kq, ytop, CDIG_W * kq, CDIG_W * kq, cols);
        x += (float)(s_cdig_x1[d] - s_cdig_x0[d] + 1) * kq;
    }
    return x;
}
/* a coin d across at (cx, cy): a dark rim, gold, a light slot (the panels' texture: pill_tex) */
static void coin_icon(Ctx *c, uint8_t *M, float cx, float cy, float d, uint32_t alpha) {
    i_GXLoadTexObj(c, M, s_pill_obj, 0, 0, 0, 0, 0, 0, 0);
    pill_draw(c, M, cx - d / 2, cy - d / 2, d, d, d / 2, pill_col(0x5A2E0000u, alpha, 1));
    float r = d * 0.42f;
    pill_draw(c, M, cx - r, cy - r, 2 * r, 2 * r, r, pill_col(0xE89A1000u, alpha, 1));
    r = d * 0.34f;
    pill_draw(c, M, cx - r, cy - r, 2 * r, 2 * r, r, pill_col(0xFFCC2800u, alpha, 1));
    pill_draw(c, M, cx - d * 0.08f, cy - d * 0.22f, d * 0.16f, d * 0.44f, d * 0.08f, pill_col(0xFFF6C000u, alpha, 1));
}
static void coin_hud(Ctx *c, uint8_t *M, uint32_t self, int view) {
    int kart = view_kart(c, M, view);
    if (kart < 0 || !coin_digits(c, M)) return;
    char s[4];
    snprintf(s, sizeof s, "%d", coins_held(kart) % 100);
    CoinAnchor a = s_ca_last[view];
    float h, x, ytop, kq;
    uint32_t zone, alpha;
    float dh = a.ok ? a.dy1 - a.dy0 : 0; /* the lap's digit's height */
    if (s_coin_row && s_hud_one) { /* DASHBOARD (one player): its panel */
        h = dh > 8 && dh < 60 ? dh : 22, kq = h / CDIG_W;
        float wd = coin_width(s, kq);
        x = s_row[ROW_COINS] + (ROW_W[ROW_COINS] - wd) / 2;
        ytop = 21 - (float)(s_cdig_y0 + s_cdig_y1 + 1) / 2 * kq; /* the digits' middle at y 21, as the row's pictures */
        alpha = (uint32_t)(255 * s_coin_e), zone = 4;
    } else if (s_modern) {
        if (!a.ok || !a.lap.on || !(s_modern_t[view & 3] >= 0.5f)) return; /* the lap count not drawn, or sliding back at the finish */
        h = dh, kq = h / CDIG_W;
        if (!(h > 4 && h < 200)) return;
        float gap = a.time.on && a.time.x0 > a.lap.x1 ? a.time.x0 - a.lap.x1 : 0.6f * h;
        gap = fminf(fmaxf(gap, 0.25f * h), 3 * h);
        x = a.lap.x0 - gap - coin_width(s, kq);
        ytop = a.dy0;
        alpha = a.alpha, zone = a.zone;
    } else { /* CLASSIC: left of the time, else right of the lap count */
        h = a.time.on && a.th > 4 ? a.th : a.ok ? dh : 28, kq = h / CDIG_W; /* the time's digits' size */
        if (!(h > 4 && h < 200)) return;
        float wd = coin_width(s, kq);
        if (a.time.on) {
            x = (a.wx0 > 0 ? a.wx0 : a.time.x0) - 0.4f * h - wd;
            ytop = (a.time.y0 + a.time.y1) / 2 - (float)(s_cdig_y0 + s_cdig_y1 + 1) / 2 * kq; /* the digits' middle at its middle */
            /* on the time's side of the widescreen HUD (gx.c: a draw's side is its middle's, +-0.2 of
             * the screen; the counter's own middle would leave it in the middle): zones 4-6 a side */
            float mid = (a.time.x0 + a.time.x1) / 608 - 1;
            alpha = 255, zone = (a.time_zone & 0xFF) != 1 ? a.time_zone : mid > 0.2f ? 6 : mid < -0.2f ? 5 : 4;
        } else if (a.ok && a.lap.on) {
            x = a.lap.x1 + 0.25f * h, ytop = a.dy0;
            alpha = a.alpha, zone = a.zone;
        } else
            return;
    }
    /* two players (either split): under the lap count, the lap digit's size (over it when it's in its
     * screen's lower half: the place under it, the bottom screen). Off the screen's left,
     * or the left quarters of three and four (the place left of the lap count): right of the lap
     * count; the right quarters (the place right of it): left of it */
    bool two = s_hud_n == 2, left4 = s_hud_n >= 3 && a.lap.x0 + a.lap.x1 < 608, right4 = s_hud_n >= 3 && !left4;
    if (two && !s_coin_row && a.ok && a.lap.on && dh > 4 && dh < 200) {
        h = dh, kq = h / CDIG_W;
        float top = g_vsplit_on || view == 0 ? 0 : 224, tall = g_vsplit_on ? 448 : 224;
        bool over = a.lap.y0 + a.lap.y1 > 2 * top + tall;
        x = a.lap.x0, alpha = a.alpha, zone = a.zone;
        ytop = over ? a.lap.y0 - 0.2f * h - (float)(s_cdig_y1 + 1) * kq : a.lap.y1 + 0.2f * h - (float)s_cdig_y0 * kq;
    } else if ((x < 2 || left4 || right4) && !s_coin_row && a.ok && a.lap.on) {
        x = right4 ? a.lap.x0 - 0.4f * h - coin_width(s, kq) : a.lap.x1 + 0.25f * h;
        ytop = a.dy0 + (a.dy1 - a.dy0 - h) / 2, alpha = a.alpha, zone = a.zone;
    }
    if (s_hud_logging) HUDLOG("coin view %d x %.1f y %.1f h %.1f zone %u alpha %u", view, x, ytop, h, zone, alpha);
    if (!alpha) return;
    uint32_t col[4] = {0xFFFFFF00u | alpha, 0xFFFFFF00u | alpha, 0xFFFFFF00u | alpha, 0xFFFFFF00u | alpha};
    if (s_zone_now != zone) hud_zone(s_zone_now = zone);
    vcall1(c, 0x800bd5d8u, 1); /* GXSetNumTexGens */
    k2d_identity(c, M);
    /* the digits */
    k2d_tev(c, M, self, 0, 0xFFFFFFFFu);
    float cy = ytop + (float)(s_cdig_y0 + s_cdig_y1 + 1) / 2 * kq;
    float pen = digits_draw(c, M, s, x, cy, kq, col[0]);
    /* the coin */
    if (!pill_tex(c, M)) return;
    float d = (float)(s_cdig_y1 - s_cdig_y0 + 1) * kq * COIN_ICON;
    coin_icon(c, M, pen + 0.12f * d + d / 2, cy, d, alpha);
}

/* ---- a trick battle's HUD (g_trick_comp: a balloon battle, one screen) ----
 * MODERN's look: dark rounded panels (pill_draw) with the game's own digits (coin_digits): the coins at
 * the top left, the points in the middle beside a star, the clock at the top right (red for the last 10
 * seconds). From the countdown (g_trick_live, tk_step) at the race HUD's first picture drawn each
 * frame, through its TEV setup; its own drawing puts its state back after. Zones (gx.c): 5 the left
 * edge, 4 the middle, 6 the right edge of the widescreen HUD. Layout units (608 x 448). */
#define TH_DIG 23.0f   /* the digits' drawn height (their rows) */
#define TH_PAD 11.0f   /* inside a panel, either side */
#define TH_EDGE 18.0f  /* the side panels from the screen's sides */
static uint32_t s_trick_drawn; /* the race frame (g_race_frames) it was last drawn */
/* GX for the HUD's quads on its own (a battle's Race2D draws no K2DPicture to borrow the setup from,
 * drawK2D): k2d_quad's vertices (position f32 x y, colour RGBA8, texture u8 / 128) in format 0, the
 * texture times the vertex colour, alpha blended, no depth; position matrix 0 the identity in the
 * race HUD's own 2D projection (Race2D::draw's, still loaded). */
static void hud_gx(Ctx *c, uint8_t *M) {
#define HG(fn, ...) do { const uint32_t _a[] = {__VA_ARGS__}; ctx_call_guest(c, fn, _a, (int)(sizeof _a / sizeof *_a)); } while (0)
    const uint32_t none[1] = {0};
    ctx_call_guest(c, 0x800bca18u, none, 0); /* GXClearVtxDesc */
    HG(0x800bc100u, 9, 1);                  /* GXSetVtxDesc: position, colour 0, texture 0 direct */
    HG(0x800bc100u, 11, 1);
    HG(0x800bc100u, 13, 1);
    HG(0x800bca50u, 0, 9, 0, 4, 0);         /* GXSetVtxAttrFmt: x y f32 */
    HG(0x800bca50u, 0, 11, 1, 5, 0);        /* ... RGBA8 */
    HG(0x800bca50u, 0, 13, 1, 0, 7);        /* ... s t u8, 7 fraction bits */
    HG(0x800bf040u, 1);                     /* GXSetNumChans */
    n_GXSetChanCtrl(c, M, 4, 0, 0, 1, 0, 0, 2, 0); /* colour 0 + alpha: the vertex colour, unlit */
    HG(0x800bd5d8u, 1);                     /* GXSetNumTexGens */
    HG(0x800bd358u, 0, 1, 4, 60, 0, 125);   /* GXSetTexCoordGen2: texcoord 0 = tex 0, identity */
    HG(0x800c0d94u, 1);                     /* GXSetNumTevStages */
    n_GXSetTevOrder(c, M, 0, 0, 0, 4, 0, 0, 0, 0);
    HG(0x800c0658u, 0, 0);                  /* GXSetTevOp: modulate */
    HG(0x800c1104u, 1, 4, 5, 0);            /* GXSetBlendMode: source alpha */
    HG(0x800c0b28u, 7, 0, 0, 7, 0);         /* GXSetAlphaCompare: always */
    HG(0x800c11b0u, 0, 7, 0);               /* GXSetZMode: off */
    HG(0x800be0e4u, 0);                     /* GXSetCullMode: none */
    HG(0x800c2178u, 0);                     /* GXSetCurrentMtx */
#undef HG
    k2d_identity(c, M);
}
static void trick_star(Ctx *c, uint8_t *M, float cx, float cy, float r, uint32_t col) { /* 5 points, r out: a spike each */
    for (int k = 0; k < 5; k++) {
        float a = -1.5707963f + 1.2566371f * (float)k;
        pill_line(c, M, cx, cy, cx + r * cosf(a), cy + r * sinf(a), 0.65f * r, 0, col);
    }
}
/* ---- the game's own pictures by name (JKRFileLoader::getGlbResource: every mounted archive; in a race
 * race2d.arc's letters, digits and faces, MRAMLoc.arc's places and POINTS), each one's texture object
 * made once a race; IA4 ones' drawn rows and columns (alpha) kept for spacing. Lookups that fail are kept
 * too (not asked again this race). */
#define GT_MAX 64
typedef struct {
    char name[28];
    uint32_t timg, obj;   /* ResTIMG (0: not found), its GXTexObj */
    uint16_t w, h;
    uint8_t fmt, x0, x1, y0, y1;
} GTex;
static GTex s_gt[GT_MAX];
static int s_gtn;
static uint32_t s_gt_pool; /* GT_MAX GXTexObj (0x20 each) in the charpack pool */
static void gtex_new_race(void) { s_gtn = 0; }
static const GTex *gtex(Ctx *c, uint8_t *M, const char *name) {
    for (int i = 0; i < s_gtn; i++) {
        GTex *g = &s_gt[i];
        if (strcmp(g->name, name)) continue;
        if (!g->timg) return NULL;
        /* still the same picture (the archive not dropped) */
        if (mld(c, M, g->timg, 1) == g->fmt && ld16(c, M, g->timg + 2) == g->w && ld16(c, M, g->timg + 4) == g->h) return g;
        g->timg = 0;
        return NULL;
    }
    if (s_gtn >= GT_MAX || strlen(name) >= sizeof s_gt[0].name) return NULL;
    if (!s_gt_pool) s_gt_pool = charpack_reserve(GT_MAX * 0x20);
    if (!s_gt_pool || !IS_RAM(s_gt_pool + GT_MAX * 0x20 - 1)) return s_gt_pool = 0, NULL;
    GTex *g = &s_gt[s_gtn];
    memset(g, 0, sizeof *g);
    snprintf(g->name, sizeof g->name, "%s", name);
    g->obj = s_gt_pool + 0x20 * (uint32_t)s_gtn++;
    uint32_t old = frame_push(c, M, 0x40), sp = old - 0x40;
    for (size_t i = 0; i <= strlen(name); i++) mst(c, M, sp + 8 + (uint32_t)i, 1, (uint8_t)name[i]);
    const uint32_t a[] = {sp + 8, 0};
    uint32_t t = ctx_call_guest(c, G_getGlbResource, a, 2);
    frame_pop(c, old);
    if (!ram_ptr(t) || !ram_ptr(t + 0x1f)) return NULL;
    uint32_t fmt = mld(c, M, t, 1), w = ld16(c, M, t + 2), h = ld16(c, M, t + 4), img = t + ld32(c, M, t + 0x1c);
    size_t size = gx_tex_size(fmt, w, h);
    if ((fmt != 2 && fmt != 3 && fmt != 14) || !w || !h || w > 512 || h > 512 || !size || !ram_ptr(img) || !ram_ptr(img + (uint32_t)size - 1)) {
        rt_log("trick results: %s not usable (%08x fmt %u %ux%u)", name, t, fmt, w, h);
        return NULL;
    }
    g->timg = t, g->fmt = (uint8_t)fmt, g->w = (uint16_t)w, g->h = (uint16_t)h;
    g->x0 = 0, g->x1 = (uint8_t)(w > 255 ? 255 : w - 1), g->y0 = 0, g->y1 = (uint8_t)(h > 255 ? 255 : h - 1);
    if (fmt == 2 && w <= 64 && h <= 64) { /* IA4, 8 x 4 tiles, alpha the high nibble */
        int x0 = (int)w, x1 = -1, y0 = (int)h, y1 = -1;
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                uint32_t at = ((y / 4) * (w / 8) + x / 8) * 32 + (y % 4) * 8 + x % 8;
                if (mld(c, M, img + at, 1) >> 4 < 3) continue;
                x0 = (int)x < x0 ? (int)x : x0, x1 = (int)x > x1 ? (int)x : x1, y0 = (int)y < y0 ? (int)y : y0, y1 = (int)y > y1 ? (int)y : y1;
            }
        if (x1 >= x0) g->x0 = (uint8_t)x0, g->x1 = (uint8_t)x1, g->y0 = (uint8_t)y0, g->y1 = (uint8_t)y1;
    }
    const uint32_t init[] = {g->obj, img, w, h, fmt, 0, 0, 0};
    ctx_call_guest(c, 0x800BF358u, init, 8); /* GXInitTexObj (clamped, no mipmaps) */
    return g;
}
/* the picture whole, w x h at (x, y), corner colours col (top left, top right, bottom right, bottom left) */
static void gtex_draw(Ctx *c, uint8_t *M, const GTex *g, float x, float y, float w, float h, const uint32_t col[4]) {
    i_GXLoadTexObj(c, M, g->obj, 0, 0, 0, 0, 0, 0, 0);
    k2d_quad(c, M, x, y, w, h, col);
}
/* text in the game's race letters (mariofont_*.bti 32 x 32: A-Z, 0-9, . / and space), cells k layout units
 * a texel; a glyph's drawn columns side by side */
static const GTex *glyph(Ctx *c, uint8_t *M, char ch) {
    char n[28];
    if (ch >= 'A' && ch <= 'Z') snprintf(n, sizeof n, "mariofont_%c.bti", ch - 'A' + 'a');
    else if (ch >= '0' && ch <= '9') snprintf(n, sizeof n, "mariofont_number_%c.bti", ch);
    else if (ch == '.') snprintf(n, sizeof n, "mariofontperiod.bti");
    else if (ch == '/') snprintf(n, sizeof n, "mariofontslash.bti");
    else return NULL;
    return gtex(c, M, n);
}
static float text_width(Ctx *c, uint8_t *M, const char *s, float k) {
    float w = 0;
    for (const char *p = s; *p; p++) {
        const GTex *g = glyph(c, M, *p);
        w += g ? (float)(g->x1 - g->x0 + 1) * k - 2.5f * k : 9 * k;
    }
    return w;
}
/* s from x, the cells' middle at cy; the colours top (ct) to bottom (cb) of each cell */
static float text_draw(Ctx *c, uint8_t *M, const char *s, float x, float cy, float k, uint32_t ct, uint32_t cb) {
    const uint32_t col[4] = {ct, ct, cb, cb};
    for (const char *p = s; *p; p++) {
        const GTex *g = glyph(c, M, *p);
        if (!g) { x += 9 * k; continue; }
        gtex_draw(c, M, g, x - (float)g->x0 * k, cy - 16 * k, 32 * k, 32 * k, col);
        x += (float)(g->x1 - g->x0 + 1) * k - 2.5f * k;
    }
    return x;
}

/* ---- the trick battle's results (after time up, before the game's battle menu) ----
 * GRAND PRIX's results' look: the karts ranked by points, a row each sliding in, the game's places
 * (position1_N), both drivers' faces (Race2D's by ECharID), their names in the race letters, the points
 * and the word POINTS; the player's row lit. Shown TR_WAIT frames after time up until A (battle.c
 * trick_results_pad: the pad's buttons held back from the game meanwhile, so its menu waits). */
volatile uint32_t g_trick_res, g_trick_res_t; /* 0 not yet, 1 shown, 2 passed by; frames shown */
#define TR_WAIT 150
static const char *const FACE_TEX[20] = {"baby_mario.bti", "baby_luigi.bti", "pata.bti", "noko.bti", "peach.bti", "daisy.bti",
                                         "mario.bti", "luigi.bti", "wario.bti", "waluigi.bti", "yoshi.bti", "catherine.bti", "donky.bti",
                                         "diddy.bti", "koopa.bti", "koopa_jr.bti", "kinopio.bti", "kinopico.bti", "teresa.bti", "pakkun.bti"};
static const char *const CHAR_NAME[20] = {"BABY MARIO", "BABY LUIGI", "PARATROOPA", "KOOPA", "PEACH", "DAISY", "MARIO", "LUIGI",
                                          "WARIO", "WALUIGI", "YOSHI", "BIRDO", "DONKEY KONG", "DIDDY KONG", "BOWSER", "BOWSER JR.",
                                          "TOAD", "TOADETTE", "KING BOO", "PETEY PIRANHA"};
/* kart k's driver d: its ECharID (gRaceInfo +0x30 KartInfo[8] of 0x18: +4 + 8d the pad, +8 + 8d the SCharDB, +0 the id), 0 none */
static uint32_t kart_char(Ctx *c, uint8_t *M, int k, int d) {
    uint32_t db = ld32(c, M, 0x803B145Cu + 0x30 + 0x18 * (uint32_t)k + 8 + 8 * (uint32_t)d);
    uint32_t id = ram_ptr(db) ? ld16(c, M, db) : 0;
    return id >= 1 && id <= 20 ? id : 0;
}
static bool kart_human(Ctx *c, uint8_t *M, int k) { return ld32(c, M, 0x803B145Cu + 0x30 + 0x18 * (uint32_t)k + 4) != 0; }
static float tr_ease(float t) { t = t < 0 ? 0 : t > 1 ? 1 : t; return 1 - (1 - t) * (1 - t) * (1 - t); }
static void trick_results(Ctx *c, uint8_t *M) {
    uint32_t t = g_trick_res_t < 100000 ? g_trick_res_t++ : g_trick_res_t;
    int n = (int16_t)ld16(c, M, 0x803B145Cu + 0x1c);
    if (n < 1 || n > 8) return;
    int order[8];
    /* a cup: by the cup's points so far (this round's in them), else as the battle's end ranks them */
    bool cup = g_trick_cup != 0;
    const volatile uint32_t *pts = cup ? g_trick_total : g_trick_score;
    trick_rank(order, n);
    if (cup)
        for (int a = 1; a < n; a++)
            for (int b = a; b > 0 && pts[order[b]] > pts[order[b - 1]]; b--) { int x = order[b]; order[b] = order[b - 1]; order[b - 1] = x; }
    if (t == 0) rt_log("trick results: %d karts, first kart %d with %u points%s", n, order[0], pts[order[0]], cup ? " (cup)" : "");
    float fade = tr_ease((float)t / 20);
    pill_zone(4); /* no dimming over the race (the rows have their own panels) */
    /* the title: TRICK STYLE, gold top to orange */
    float tk = 1.15f, ty = 50 - (1 - fade) * 30;
    const char *title = cup ? TRICK_CUP_NAME[(g_trick_cup - 1) % TRICK_CUPS] : "TRICK STYLE";
    uint32_t ta = (uint32_t)(255 * fade);
    if (cup) ty -= 10;
    text_draw(c, M, title, 304 - text_width(c, M, title, tk) / 2, ty, tk, 0xFFF27000u | ta, 0xFF9A1000u | ta);
    if (cup) { /* ROUND 2 OF 4, or the winner after the last */
        char sub[48];
        uint32_t w = kart_char(c, M, order[0], 0);
        if (g_trick_round >= 3) snprintf(sub, sizeof sub, "CUP WINNER  %s", w ? CHAR_NAME[w - 1] : "");
        else snprintf(sub, sizeof sub, "ROUND %u OF 4", (unsigned)g_trick_round + 1);
        text_draw(c, M, sub, 304 - text_width(c, M, sub, 0.5f) / 2, ty + 26, 0.5f, 0xFFFFFF00u | ta, 0xB4DCFF00u | ta);
    }
    /* the rows */
    float rh = n > 6 ? 34 : n > 4 ? 40 : 62, gap = n > 6 ? 5 : n > 4 ? 6 : 10, rw = 540, rx = 304 - rw / 2; /* 8 rows end by 402 */
    float top = 90 + (n > 4 ? 0 : (4 - n) * (rh + gap) / 2);
    static const uint32_t PLACE_TOP[4] = {0xFFF4A0FFu, 0xF0F4FFFFu, 0xFFC890FFu, 0xD8E4FFFFu};
    static const uint32_t PLACE_BOT[4] = {0xF0A000FFu, 0x9AA4B8FFu, 0xB0602CFFu, 0x6C88C8FFu};
    const GTex *pts_word = gtex(c, M, "word_points.bti");
    for (int r = 0; r < n; r++) {
        int k = order[r], place = r;
        while (place > 0 && pts[order[place - 1]] == pts[k]) place--; /* a tie shares the place */
        float e = tr_ease(((float)t - 14 - 9 * (float)r) / 16);
        if (e <= 0) continue;
        uint32_t al = (uint32_t)(255 * e);
        float x = rx + (1 - e) * 560, y = top + (float)r * (rh + gap), cy = y + rh / 2;
        bool you = kart_human(c, M, k);
        i_GXLoadTexObj(c, M, s_pill_obj, 0, 0, 0, 0, 0, 0, 0);
        if (you) pill_draw(c, M, x - 3, y - 3, rw + 6, rh + 6, (rh + 6) / 2, pill_col(0xFFD23C00u, 0xFF, e)); /* a gold rim */
        pill_draw(c, M, x, y, rw, rh, rh / 2, pill_col(you ? 0x1C3C9600u : PILL_DARK, you ? 0xE6 : 0xD0, e));
        /* the place */
        char ps[24];
        snprintf(ps, sizeof ps, "position1_%d.bti", place + 1);
        const GTex *pg = gtex(c, M, ps);
        float pz = rh * 1.12f;
        uint32_t c0 = (PLACE_TOP[place < 4 ? place : 3] & 0xFFFFFF00u) | al, c1 = (PLACE_BOT[place < 4 ? place : 3] & 0xFFFFFF00u) | al;
        const uint32_t pc[4] = {c0, c0, c1, c1};
        if (pg) gtex_draw(c, M, pg, x + 10, cy - pz / 2 - 1, pz, pz, pc);
        /* the faces: the front driver's, the back one's smaller behind it */
        float fx = x + 14 + pz, fz = rh * 0.86f;
        const uint32_t wc[4] = {0xFFFFFF00u | al, 0xFFFFFF00u | al, 0xFFFFFF00u | al, 0xFFFFFF00u | al};
        uint32_t c_front = kart_char(c, M, k, 0), c_back = kart_char(c, M, k, 1);
        const GTex *fb = c_back ? gtex(c, M, FACE_TEX[c_back - 1]) : NULL, *ff = c_front ? gtex(c, M, FACE_TEX[c_front - 1]) : NULL;
        if (fb) gtex_draw(c, M, fb, fx + fz * 0.52f, cy - fz * 0.36f, fz * 0.72f, fz * 0.72f, wc);
        if (ff) gtex_draw(c, M, ff, fx, cy - fz / 2, fz, fz, wc);
        /* the names: the front driver's over the back one's */
        float nx = fx + fz * 1.32f + 8;
        /* the points first (right aligned before the word), the names in the room left */
        char s[12];
        snprintf(s, sizeof s, "%u", (unsigned)(pts[k] % 10000000u));
        float ww = pts_word ? rh * 1.15f : 0, wh = ww / 4;
        float dk = rh * 0.5f / (float)(s_cdig_y1 - s_cdig_y0 + 1);
        float dx = x + rw - 18 - ww - 6 - digits_width(s, dk);
        digits_draw(c, M, s, dx, cy, dk, 0xFFFFFF00u | al);
        if (cup && g_trick_round > 0) { /* this round's points, small, before the cup's (the first round's are the cup's) */
            char r1[16];
            snprintf(r1, sizeof r1, "%u", (unsigned)(g_trick_score[k] % 1000000u));
            float rk = rh / 32 * 0.42f, rw1 = text_width(c, M, r1, rk);
            dx -= rw1 + 12;
            text_draw(c, M, r1, dx, cy, rk, 0xB4DCFF00u | al, 0x8CA8D000u | al);
        }
        if (pts_word) {
            uint32_t w0 = 0xFFF27000u | al, w1 = 0xFFB02000u | al;
            const uint32_t pwc[4] = {w0, w0, w1, w1};
            gtex_draw(c, M, pts_word, x + rw - 18 - ww, cy - wh / 2 + rh * 0.1f, ww, wh, pwc);
        }
        uint32_t n0 = (you ? 0xFFF8D000u : 0xFFFFFF00u) | al, n1 = (you ? 0xFFC04000u : 0xC8D0E000u) | al;
        float room = dx - 14 - nx;
        for (int line = 0; line < (n <= 4 ? 2 : 1); line++) {
            uint32_t id = line ? c_back : c_front;
            if (!id || room < 20) continue;
            float k0 = rh / 32 * (n > 4 ? 0.55f : line ? 0.36f : 0.5f), tw = text_width(c, M, CHAR_NAME[id - 1], k0);
            if (tw > room) k0 *= room / tw; /* a long name narrower */
            float ly = n > 4 ? cy : line ? cy + rh * 0.24f : cy - rh * 0.14f;
            if (line) text_draw(c, M, CHAR_NAME[id - 1], nx, ly, k0, 0xC8D0E000u | al, 0x8C98B000u | al);
            else text_draw(c, M, CHAR_NAME[id - 1], nx, ly, k0, n0, n1);
        }
    }
    /* A to go on, once the rows are in */
    float in = 14 + 9 * (float)n + 16;
    if ((float)t > in + 20 && (t / 30) % 2 == 0) {
        const char *go = "PRESS A";
        float gk = 0.55f;
        text_draw(c, M, go, 304 - text_width(c, M, go, gk) / 2, top + (float)n * (rh + gap) + 14, gk, 0xFFFFFFFFu, 0xC8D0E0FFu);
    }
}
static uint32_t s_res_wait; /* frames since time up */
static uint32_t tk_alpha(uint32_t rgba, float a) { return (rgba & 0xFFFFFF00u) | (uint32_t)((float)(rgba & 0xFF) * (a < 0 ? 0 : a > 1 ? 1 : a)); }
/* text centred on x, a dark panel behind it */
static void tk_text(Ctx *c, uint8_t *M, const char *s, float x, float cy, float k, uint32_t ct, uint32_t cb, float a, uint32_t dark) {
    float w = text_width(c, M, s, k), ph = 30 * k + 4;
    i_GXLoadTexObj(c, M, s_pill_obj, 0, 0, 0, 0, 0, 0, 0);
    pill_draw(c, M, x - w / 2 - 8, cy - ph / 2, w + 16, ph, ph / 2, tk_alpha(dark, a));
    text_draw(c, M, s, x - w / 2, cy, k, tk_alpha(ct, a), tk_alpha(cb, a));
}
/* under the points: the signature's meter, the combo (times its multiplier, the time left to keep it
 * going), the latest trick's name, the landing's grade; for the kart the screen follows */
/* the meter, the combo, the latest trick and the landing: a column centred on cx from top, sizes x k */
/* the one-screen HUD: the combo's line right of the points panel (from x, its middle at y, at most w wide), not in
 * the column under it; w 0: in the column */
static float s_combo_x, s_combo_y, s_combo_w;
static void trick_hud_tricks(Ctx *c, uint8_t *M, int kart, uint32_t dark, float cx, float top, float k, uint32_t zone) {
    if (kart < 0 || kart > 7) return;
    const Trick *t = &g_tk[kart];
    pill_zone(zone);
    /* the meter */
    float mw = 128 * k, mh = 7 * k, mx = cx - mw / 2, my = top, f = t->meter >= METER_FULL ? 1 : (float)t->meter / (float)METER_FULL;
    bool full = f >= 1;
    i_GXLoadTexObj(c, M, s_pill_obj, 0, 0, 0, 0, 0, 0, 0);
    pill_draw(c, M, mx - 3, my - 3, mw + 6, mh + 6, (mh + 6) / 2, dark);
    if (f > 0.03f) pill_draw(c, M, mx, my, mw * f, mh, mh / 2, full ? ((g_race_frames >> 3) & 1 ? 0xFFFFFFFFu : 0xFFD040FFu) : 0xFF9A1EFFu);
    if (full) text_draw(c, M, "READY", mx + mw + 8 * k, my + mh / 2, 0.36f * k, 0xFFF080FFu, 0xFFB030FFu);
    char s[32];
    float y = my + mh + 18 * k, ccx = cx, cy = y, ck = 0.62f * k; /* the combo's line: centre, middle, size */
    bool side = s_combo_w > 60;
    /* this jump's points (on the score at the landing), the combo's bonus to come (its points times
     * the multiplier less one, added when it ends), else the last landing's points or the bonus banked */
    uint32_t m = (uint32_t)__builtin_popcount(t->cids);
    for (int i = 0; i < t->jn; i++) m += !(t->cids >> t->jt[i] & 1);
    m = m < 1 ? 1 : m > COMBO_MULT_MAX ? COMBO_MULT_MAX : m;
    float jp = 0;
    for (int i = 0; i < t->jn; i++) jp += (float)T_PTS[t->jt[i]] * (float)(t->jc[i] * (t->jc[i] + 1) / 2);
    const char *ln = NULL;
    uint32_t lt = 0xFFF4A0FFu, lb = 0xFFB428FFu;
    float la = 1;
    if (t->jn) {
        if (m > 1) snprintf(s, sizeof s, "%u  COMBO X%u", (unsigned)(jp + 0.5f), (unsigned)m);
        else snprintf(s, sizeof s, "%u", (unsigned)(jp + 0.5f));
        ln = s;
    } else if (t->combo > 0.5f && m > 1) {
        snprintf(s, sizeof s, "COMBO X%u  %u", (unsigned)m, (unsigned)(t->combo * (float)(m - 1) + 0.5f));
        ln = s;
    } else if (t->bank_t > 0) {
        snprintf(s, sizeof s, "COMBO BONUS %u", (unsigned)t->bank);
        ln = s, lt = 0xFFFFFFFFu, lb = 0xA0FFA0FFu, la = (float)t->bank_t / 20;
    } else if (t->gain_t > 0) {
        snprintf(s, sizeof s, "%u", (unsigned)t->gain);
        ln = s, lt = 0xFFFFFFFFu, lb = 0xA0FFA0FFu, la = (float)t->gain_t / 20;
    }
    float tw = 0;
    if (ln && side) { /* right of the points: left aligned, narrower to fit */
        tw = text_width(c, M, ln, ck);
        if (tw + 16 > s_combo_w) ck *= (s_combo_w - 16) / tw, tw = text_width(c, M, ln, ck);
        ccx = s_combo_x + 8 + tw / 2, cy = s_combo_y;
    }
    if (ln) tk_text(c, M, ln, ccx, cy, ck, lt, lb, la, dark);
    if (!t->jn && t->combo > 0.5f && t->window > 0 && !t->air && (ln || !side)) { /* the time left to keep the combo going */
        float ww = 110.0f * k * (float)t->window / COMBO_WINDOW, wy = cy + 13 * k;
        if (side) ww = (tw + 16) * (float)t->window / COMBO_WINDOW, wy = cy + 15 * ck + 4; /* as wide as its line */
        i_GXLoadTexObj(c, M, s_pill_obj, 0, 0, 0, 0, 0, 0, 0);
        pill_draw(c, M, ccx - ww / 2, wy, ww, 5, 2.5f, 0xFFC850E0u);
    }
    if (side) y -= 18 * k; /* the trick's name and the landing come up into the line left free */
    /* the latest trick */
    if (t->pop_t > 0 && t->pop[0]) { /* a combo of several ("A + B + C"): narrower to stay on screen */
        float pk = 0.42f * k, pw = text_width(c, M, t->pop, pk), room = 2 * fminf(cx, 608 - cx) - 24;
        if (pw > room && room > 40) pk *= room / pw;
        tk_text(c, M, t->pop, cx, y + 26 * k, pk, 0xFFFFFFFFu, 0xB4DCFFFFu, (float)t->pop_t / 15, dark);
    }
    /* the landing */
    if (t->grade_t > 0 && t->grade) {
        static const char *const GW[5] = {"", "PERFECT", "SKETCHY", "BAIL", "CAUGHT"};
        static const uint32_t GT[5] = {0, 0xFFF46EFFu, 0xFFC878FFu, 0xFF6A5AFFu, 0x9AF0FFFFu};
        static const uint32_t GB[5] = {0, 0x6EE65AFFu, 0xE07A28FFu, 0xC8281EFFu, 0x3CA0FFFFu};
        float age = (float)(70 - t->grade_t), kg = 0.8f * k * (age < 6 ? 0.6f + 0.4f * age / 6 : 1);
        tk_text(c, M, GW[t->grade], cx, y + 62 * k, kg, GT[t->grade], GB[t->grade], (float)t->grade_t / 15, dark);
    }
}
/* each frame of the trick HUD, any number of screens: the results after time up. True: nothing more to draw */
static bool trick_hud_begin(Ctx *c, uint8_t *M) {
    if (s_trick_drawn + 1 != g_race_frames) rt_log("trick hud: from race frame %u", g_race_frames); /* not drawn the frame before */
    s_trick_drawn = g_race_frames;
    if (!g_trick_ended) s_res_wait = 0;
    else if (s_res_wait < 100000 && ++s_res_wait >= TR_WAIT && !g_trick_res) g_trick_res = 1, g_trick_res_t = 0;
    if (!coin_digits(c, M) || !pill_tex(c, M)) return true;
    if (g_trick_res) {
        uint32_t z = s_zone_now;
        hud_gx(c, M);
        if (g_trick_res == 1) trick_results(c, M);
        pill_zone(z);
        return true;
    }
    return false;
}
/* the clock's colour by seconds left: white from 2:00, green 1:00-1:59, yellow 0:30-0:59, red 0:00-0:29 */
static uint32_t clock_col(int secs) {
    return secs >= 120 ? 0xFFFFFFFFu : secs >= 60 ? 0x5AF06EFFu : secs >= 30 ? 0xFFE040FFu : 0xFF5A46FFu;
}
/* the clock's panel: x its left edge (align 1: its right edge, 2: its middle), cy its middle */
static void trick_clock(Ctx *c, uint8_t *M, float x, int align, float cy, float kq, uint32_t dark, uint32_t zone) {
    int left = g_trick_frames < tk_len() ? (int)(tk_len() - g_trick_frames) : 0, secs = (left + 59) / 60;
    char mn[4], ss[4];
    snprintf(mn, sizeof mn, "%d", secs / 60 % 10);
    snprintf(ss, sizeof ss, "%02d", secs % 60);
    uint32_t tc = clock_col(secs);
    float colon = 9, w = TH_PAD + digits_width(mn, kq) + colon + digits_width(ss, kq) + TH_PAD + 2;
    x -= align == 1 ? w : align == 2 ? w / 2 : 0;
    pill_zone(zone);
    i_GXLoadTexObj(c, M, s_pill_obj, 0, 0, 0, 0, 0, 0, 0);
    pill_draw(c, M, x, cy - TH_H / 2, w, TH_H, TH_H / 2, dark);
    float e = digits_draw(c, M, mn, x + TH_PAD + 1, cy, kq, tc);
    i_GXLoadTexObj(c, M, s_pill_obj, 0, 0, 0, 0, 0, 0, 0);
    for (int k = 0; k < 2; k++) { /* the colon: two dots, outlined as the digits */
        float dy = k ? 4.5f : -4.5f;
        pill_draw(c, M, e + colon / 2 - 3.2f, cy + dy - 3.2f, 6.4f, 6.4f, 3.2f, pill_col(0x00000000u, 0xFF, 1));
        pill_draw(c, M, e + colon / 2 - 2, cy + dy - 2, 4, 4, 2, tc);
    }
    digits_draw(c, M, ss, e + colon, cy, kq, tc);
}
/* the points' panel (star, points) at x (right_edge: x its right edge), top y; returns its width */
static float trick_points(Ctx *c, uint8_t *M, int kart, float x, bool right_edge, float y, float kq, uint32_t dark, uint32_t zone) {
    char ps[12];
    snprintf(ps, sizeof ps, "%u", (unsigned)((kart >= 0 && kart < 8 ? g_trick_score[kart] : 0) % 1000000u));
    float kb = kq * 1.25f, r = TH_DIG * 0.6f, h = TH_H + 6, cy = y + h / 2;
    float w = TH_PAD + 2 * r + 8 + digits_width(ps, kb) + TH_PAD + 2;
    if (right_edge) x -= w;
    pill_zone(zone);
    i_GXLoadTexObj(c, M, s_pill_obj, 0, 0, 0, 0, 0, 0, 0);
    pill_draw(c, M, x, y, w, h, h / 2, dark);
    float sx = x + TH_PAD + 2 + r;
    trick_star(c, M, sx, cy + 1, r + 2.6f, pill_col(0x3A220800u, 0xFF, 1));
    trick_star(c, M, sx, cy + 1, r, pill_col(0xFFC81E00u, 0xFF, 1));
    trick_star(c, M, sx, cy, r * 0.45f, pill_col(0xFFEE8C00u, 0xFF, 1));
    digits_draw(c, M, ps, sx + r + 8, cy, kb, 0xFFFFFFFFu);
    return w;
}
/* 2-4 screens (two: top and bottom; three or four: quarters, the fourth a map): one clock in the middle
 * of the screen; each player's points at their view's bottom outer corner, their tricks above */
static void trick_hud_views(Ctx *c, uint8_t *M, int n) {
    if (trick_hud_begin(c, M)) return;
    float kq = TH_DIG / (float)(s_cdig_y1 - s_cdig_y0 + 1);
    uint32_t z0 = s_zone_now, dark = pill_col(PILL_DARK, PILL_ALPHA, 1);
    hud_gx(c, M);
    if (n == 2 && g_vsplit_on) { /* side by side: columns 0..303 and 305..608, the whole height */
        for (int v = 0; v < 2; v++) {
            int kart = view_kart(c, M, v);
            if (kart < 0 || kart > 7) continue;
            float x0 = v ? 305.0f : 0.0f, x1 = x0 + 303, y = 448 - (TH_H + 6) - 10;
            /* points: player 1 at the bottom left, player 2 at the bottom right (the screen's outer corners) */
            trick_points(c, M, kart, v ? x1 - TH_EDGE : x0 + TH_EDGE, v == 1, y, kq, dark, v ? 6 : 5);
            /* tricks: the column's top right (player 1's beside the middle) */
            trick_hud_tricks(c, M, kart, dark, x1 - TH_EDGE - 90, 16, 0.7f, v ? 6 : 4); /* READY right of the meter stays in the column */
        }
        trick_clock(c, M, 304, 2, TH_VS_CLOCK_Y, kq, dark, 4); /* above the map, in the middle */
        pill_zone(z0);
        return;
    }
    int views = n == 3 ? 3 : n;
    for (int v = 0; v < views; v++) {
        int kart = view_kart(c, M, v);
        if (kart < 0 || kart > 7) continue;
        bool quarter = n > 2, right = quarter && (v & 1);
        float vx = quarter ? 304.0f * (float)(v & 1) : 0, vy = quarter ? 224.0f * (float)(v >> 1) : 224.0f * (float)v;
        float vw = quarter ? 304 : 608, vh = 224, ph = TH_H + 6;
        uint32_t zone = right ? 6 : 5;
        float y = vy + vh - ph - 10;
        float w = trick_points(c, M, kart, right ? vx + vw - TH_EDGE : vx + TH_EDGE, right, y, kq, dark, zone);
        float cx = right ? vx + vw - TH_EDGE - w / 2 : vx + TH_EDGE + w / 2;
        if (!quarter) trick_hud_tricks(c, M, kart, dark, s_modern ? TH_TB_TRICK_X : TH_TB_TRICK_X + 120, vy + 18, 0.8f, 5); /* top and bottom: top left, right of the item boxes */
        else trick_hud_tricks(c, M, kart, dark, cx, y - 108, 0.75f, zone);
    }
    trick_clock(c, M, 304, 2, 224, kq, dark, 4);
    pill_zone(z0);
}
static void trick_hud(Ctx *c, uint8_t *M) {
    if (trick_hud_begin(c, M)) return;
    int kart = view_kart(c, M, 0);
    uint32_t pts = kart >= 0 && kart < 8 ? g_trick_score[kart] : 0;
    int left = g_trick_frames < tk_len() ? (int)(tk_len() - g_trick_frames) : 0, secs = (left + 59) / 60;
    char cs[8], ps[12], mn[4], ss[4];
    snprintf(cs, sizeof cs, "%d", kart >= 0 ? coins_held(kart) % 100 : 0);
    snprintf(ps, sizeof ps, "%u", (unsigned)(pts % 1000000u));
    snprintf(mn, sizeof mn, "%d", secs / 60 % 10);
    snprintf(ss, sizeof ss, "%02d", secs % 60);
    float kq = TH_DIG / (float)(s_cdig_y1 - s_cdig_y0 + 1), cy = TH_Y + TH_H / 2;
    uint32_t z0 = s_zone_now, dark = pill_col(PILL_DARK, PILL_ALPHA, 1), white = 0xFFFFFFFFu;
    hud_gx(c, M);
    /* the coins */
    float d = TH_DIG * 0.92f, x = TH_EDGE, w = TH_PAD + d + 7 + digits_width(cs, kq) + TH_PAD;
    pill_zone(5);
    i_GXLoadTexObj(c, M, s_pill_obj, 0, 0, 0, 0, 0, 0, 0);
    pill_draw(c, M, x, TH_Y, w, TH_H, TH_H / 2, dark);
    s_th_coin_x1 = x + w;
    coin_icon(c, M, x + TH_PAD + d / 2, cy, d, 0xFF);
    digits_draw(c, M, cs, x + TH_PAD + d + 7, cy, kq, white);
    /* the points: larger */
    float kb = kq * 1.25f, r = TH_DIG * 0.6f, h = TH_H + 6;
    w = TH_PAD + 2 * r + 8 + digits_width(ps, kb) + TH_PAD + 2, x = 304 - w / 2;
    pill_zone(4);
    i_GXLoadTexObj(c, M, s_pill_obj, 0, 0, 0, 0, 0, 0, 0);
    pill_draw(c, M, x, TH_Y - 3, w, h, h / 2, dark);
    float sx = x + TH_PAD + 2 + r;
    trick_star(c, M, sx, cy + 1, r + 2.6f, pill_col(0x3A220800u, 0xFF, 1));
    trick_star(c, M, sx, cy + 1, r, pill_col(0xFFC81E00u, 0xFF, 1));
    trick_star(c, M, sx, cy, r * 0.45f, pill_col(0xFFEE8C00u, 0xFF, 1));
    digits_draw(c, M, ps, sx + r + 8, cy, kb, white);
    float pts_x1 = x + w;
    /* the clock */
    uint32_t tc = clock_col(secs);
    float colon = 9;
    w = TH_PAD + digits_width(mn, kq) + colon + digits_width(ss, kq) + TH_PAD + 2, x = 608 - TH_EDGE - w;
    pill_zone(6);
    i_GXLoadTexObj(c, M, s_pill_obj, 0, 0, 0, 0, 0, 0, 0);
    pill_draw(c, M, x, TH_Y, w, TH_H, TH_H / 2, dark);
    float e = digits_draw(c, M, mn, x + TH_PAD + 1, cy, kq, tc);
    i_GXLoadTexObj(c, M, s_pill_obj, 0, 0, 0, 0, 0, 0, 0);
    for (int k = 0; k < 2; k++) { /* the colon: two dots, outlined as the digits */
        float dy = k ? 4.5f : -4.5f;
        pill_draw(c, M, e + colon / 2 - 3.2f, cy + dy - 3.2f, 6.4f, 6.4f, 3.2f, pill_col(0x00000000u, 0xFF, 1));
        pill_draw(c, M, e + colon / 2 - 2, cy + dy - 2, 4, 4, 2, tc);
    }
    digits_draw(c, M, ss, e + colon, cy, kq, tc);
    s_combo_x = pts_x1 + 4, s_combo_y = cy, s_combo_w = x - 6 - s_combo_x; /* between the points and the clock */
    trick_hud_tricks(c, M, kart, dark, 304, TH_Y + TH_H + 9, 1, 4);
    s_combo_w = 0;
    pill_zone(z0);
}

/* A trick battle: the item slots (slot1*, itm1*: frames and items) fitted as DASHBOARD fits them
 * (pill_place: each frame ITEM_FRAME across, its item with it, the game's larger on the left), the
 * pair right of the coins panel, level with it (zone 5, the panel's). False: not an item piece, or no
 * frame measured yet. */
static bool trick_item_place(Ctx *c, uint8_t *M, uint32_t self, uint32_t mtx, uint32_t to, float w, float h, uint32_t *z) {
    char t[9];
    int n = pane_tag(c, M, self, t);
    if (!n || (strncmp(t, "slot1", 5) && strncmp(t, "itm1", 4)) || !(s_th_coin_x1 > 0 && s_th_coin_x1 < 608)) return false;
    float m[12];
    ldfn(c, M, mtx, m, 12);
    if (!pill_place(c, M, self, MGT_ITEMS, 0, m, w, h)) return false;
    for (int k = 0; k < 12; k++)
        if (!isfinite(m[k])) return false;
    stfn(c, M, to, m, 12);
    *z = 5 | (uint32_t)MGT_ITEMS << 8; /* zone 5, the items' group: gx.c clips them to their frames' band */
    return true;
}

/* K2DPicture::drawK2D(f32 x, f32 y, f32 w, f32 h, bool tev): when visible (+0xb0): per texture
 * (count u8 +0x110) its load (vtable +0x100), GXSetNumTexGens(count), alpha +0xb3 = +0xb2, the
 * corner colours (getNewColor into four white words), the TEV setup (setTevModeK2D when tev,
 * else the konst colours +0x160 / +0x164 for several textures and the colours +0x14c / +0x148
 * unless those are black / white), makeMatrix(x, y), its matrix (+0x50) as position matrix 0
 * and one quad (w x h, texcoords 0 / 0x80 as u8) written to the FIFO. */
uint64_t i_drawK2D__10K2DPictureFffffb(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t self = (uint32_t)a0;
    if (!mld(c, M, self + 0xb0, 1)) return 0;
    /* The mirror with the race HUD only: not in the course preview or the drop to the start line,
     * where Race2D draws but no piece of it is shown. Asked at its first piece, hidden or not. */
    int player;
    if (s_mirror_ask && hud_piece(c, M, self, &player)) {
        s_mirror_ask = false;
        hud_zone(HUD_ZONE_MIRROR), g_mirror_ns = plat_now_ns(); /* gx.c: the frame's Frame.mirror */
    }
    if (s_hud_hide && hud_hidden(c, M, self)) return 0;
    int mg = MG_NONE, kart = -1;
    float e = 0, roll = 0; /* MODERN: how far (1 fully, sliding back after the finish), the row's roll */
    float ndx = 0, ndy = 0; /* ... a picture moved within its group first (four players' lap count) */
    bool meter = false;     /* DASHBOARD: a block of the speed's arc */
    bool vs = s_hud_two && g_vsplit_on; /* two players side by side: p2_race.blo's groups in columns, any style */
    if (vs) {
        char t[9];
        int n = pane_tag(c, M, self, t), view = 0;
        mg = modern_group2(t, n, s_hud_call, &view);
        if (mg < 0) return 0; /* the words LAP and TIME */
        if (mg >= MG2_PLACE1 && mg <= MG2_ORDER) mg += MGV_PLACE1 - MG2_PLACE1;
        e = 1;
        if (mg == MGV_ORDER) kart = order_kart(c, M, self);
    } else if (s_trick_views == 2 && !s_modern) { /* a trick battle, top and bottom: player 2's items out of the points panel's corner */
        char t[9];
        int n = pane_tag(c, M, self, t), view = 0;
        if (modern_group2(t, n, s_hud_call, &view) == MG2_ITEMS2) mg = MG2_ITEMS2T, e = 1;
    } else if (s_modern) {
        char t[9];
        int n = pane_tag(c, M, self, t), view = 0;
        mg = s_hud_n >= 3 ? modern_group4(t, n, s_hud_call, &view) : s_hud_two ? modern_group2(t, n, s_hud_call, &view) : modern_group(t, n, s_hud_call);
        if (mg == MG_MAP && ld32(c, M, 0x803B1464u) - 4 <= 3) mg = MG_MAP_BATTLE; /* gRaceInfo +8: battles 4-7 */
        if (mg == MG_ITEMS && s_trick_hud) mg = MG_NONE; /* a trick battle's: trick_item_place */
        if (mg == MG2_ITEMS2 && s_trick_views == 2) mg = MG2_ITEMS2T; /* a trick battle's: not under the points panel */
        if (mg >= MG4_PLACE1 && mg < MG4_MAP && (mg - MG4_PLACE1) % 3 == 2) {
            bool count = t[1] == 'l', right = view & 1;
            ndx = count ? (right ? 0 : LAP4_COUNT_DX) : (right ? LAP4_WORD_DX : 0);
            ndy = count ? LAP4_COUNT_DY : LAP4_WORD_DY;
        }
        e = view < 0 ? fmaxf(fmaxf(s_modern_t[0], s_modern_t[1]), fmaxf(s_modern_t[2], s_modern_t[3])) : s_modern_t[view & 3];
        if (mg < 0) {
            if (e > 0.5f) return 0;
            mg = MG_NONE;
        }
        if (e <= 0) mg = MG_NONE;
        if (mg == MG_ORDER || mg == MG2_ORDER) kart = order_kart(c, M, self);
        if (mg == MG_ORDER) roll = order_roll(c, M, kart);
        bool pills = g_hud_pills && !(g_trick_comp && ld32(c, M, 0x803B1464u) == 4); /* a trick battle: no DASHBOARD */
        if (s_hud_one && pills && mg >= MG_PLACE && mg <= MG_SPEED) {
            mg += MGT_PLACE - MG_PLACE, roll = 0;
            if (mg == MGT_SPEED && !strcmp(t, "km_h")) mg = MGT_UNIT;
            /* the arc: the dial instead (pill_panels), itself fading in while the pieces slide back */
            if (mg == MGT_SPEED && !strncmp(t, "sp_mtr", 6)) meter = true, mg = MG_NONE;
            if (meter && t[6] >= '1' && t[6] <= '7') { /* sp_mtr1, then 7 .. 2 */
                s_meter_col[t[6] == '1' ? 0 : 8 - (t[6] - '0')] = ld32(c, M, self + 0x148) >> 8;
                s_meter_frame = g_race_frames;
            }
        } else if (s_hud_one && pills && mg == MG_MAP_BATTLE) mg = MGT_MAP_BATTLE;
        if (mg >= MGT_PLACE && mg <= MGT_MAP_BATTLE) s_pill_seen |= 1u << (mg - MGT_PLACE);
        if (meter) s_pill_seen |= 1u << (MGT_SPEED - MGT_PLACE);
        if (s_pill_todo && (uint32_t)a5 & 0xFF) {
            s_pill_todo = false;
            pill_panels(c, M, self, e);
        }
        if (meter && e >= 1) return 0;
    }
    if (s_coin_todo && (uint32_t)a5 & 0xFF && hud_piece(c, M, self, &player)) {
        uint32_t todo = s_coin_todo;
        s_coin_todo = 0;
        for (int v = 0; v < 4; v++)
            if (todo >> v & 1) coin_hud(c, M, self, v);
    }
    uint32_t old_sp = frame_push(c, M, 0x80), sp = old_sp - 0x80;
    uint32_t hud_at = 0, hud_was = 0;
    if (s_hud_call == 2 && charpack_count()) hud_face(c, M, self, &hud_at, &hud_was);
    for (uint32_t i = 0; (i & 0xFF) < mld(c, M, self + 0x110, 1); i++) vcall2(c, ld32(c, M, ld32(c, M, self) + 0x100), self, i);
    if (hud_at) mst(c, M, hud_at, 4, hud_was);
    vcall1(c, 0x800bd5d8u, mld(c, M, self + 0x110, 1)); /* GXSetNumTexGens */
    mst(c, M, self + 0xb3, 1, mld(c, M, self + 0xb2, 1));
    /* the game fades rows out as they roll off below the top 4 (alpha +0xb2, set each frame): the
     * player's, rolled into the 5th row (modern_place), is shown; the others are gone by then */
    if (roll > 0) mst(c, M, self + 0xb3, 1, kart_is_player(c, M, kart) ? 0xFF : (uint32_t)((float)mld(c, M, self + 0xb2, 1) * (1 - roll)));
    if (mg == MGT_ORDER) mst(c, M, self + 0xb3, 1, s_order_alpha); /* every kart's, not only the top 4's */
    if (meter) mst(c, M, self + 0xb3, 1, (uint32_t)((float)mld(c, M, self + 0xb2, 1) * (1 - e)));
    for (int k = 0; k < 4; k++) mst(c, M, sp + 0x18 + 4 * k, 4, 0xFFFFFFFFu);
    n_getNewColor__10K2DPictureFPQ28JUtility6TColor(c, M, self, sp + 0x18, 0, 0, 0, 0, 0, 0);
    if (s_hud_call) plus_colours(c, M, self, sp + 0x18); /* inside the race HUD */
    if (mg == MGT_PLACE) place_colours(c, M, self, sp + 0x18);
    if (mg == MG_SPEED) speed_tint(c, M, self, sp + 0x18, e);
    /* the race order's faces in full colour (the game tints them), not their outlines; the speed too */
    uint32_t face_black = 0, face_white = 0;
    bool face = false;
    char face_tag[9];
    if (mg == MGT_SPEED || mg == MGT_UNIT || (mg == MGT_ORDER && pane_tag(c, M, self, face_tag) && face_tag[0] == 'c' && face_tag[1] != 'w')) {
        face = true, face_black = ld32(c, M, self + 0x14c), face_white = ld32(c, M, self + 0x148);
        mst(c, M, self + 0x14c, 4, 0);
        mst(c, M, self + 0x148, 4, 0xFFFFFFFFu);
    }
    if (mg == MGT_SPEED || mg == MGT_UNIT) /* the speed in white, whatever a mod colours it */
        for (uint32_t k = 0; k < 4; k++) mst(c, M, sp + 0x18 + 4 * k, 4, ld32(c, M, sp + 0x18 + 4 * k) | 0xFFFFFF00u);
    if ((uint32_t)a5 & 0xFF) {
        vcall1(c, 0x8012cae4u, self); /* setTevModeK2D */
    } else {
        if (mld(c, M, self + 0x110, 1) != 1) {
            mst(c, M, sp + 0x14, 4, ld32(c, M, self + 0x160));
            vcall2(c, 0x800c0934u, 0, sp + 0x14); /* GXSetTevKColor */
            mst(c, M, sp + 0x10, 4, ld32(c, M, self + 0x164));
            vcall2(c, 0x800c0934u, 2, sp + 0x10);
        }
        if (ld32(c, M, self + 0x14c) || ld32(c, M, self + 0x148) + 0x10000u != 0xFFFFu) {
            mst(c, M, sp + 0xc, 4, ld32(c, M, self + 0x14c));
            n_GXSetTevColor(c, M, 1, sp + 0xc, 0, 0, 0, 0, 0, 0);
            mst(c, M, sp + 8, 4, ld32(c, M, self + 0x148));
            n_GXSetTevColor(c, M, 2, sp + 8, 0, 0, 0, 0, 0, 0);
        }
    }
    if (face) {
        mst(c, M, self + 0x14c, 4, face_black);
        mst(c, M, self + 0x148, 4, face_white);
    }
    ctx_set_gpr(c, 3, self);
    WR(c, 0x4008, 8, a1);
    WR(c, 0x4010, 8, a2);
    ctx_call(c, 0x8012ceacu); /* makeMatrix(x, y) */
    double dw, dh;
    memcpy(&dw, &a3, 8);
    memcpy(&dh, &a4, 8);
    float w = (float)dw, h = (float)dh;
    uint32_t mtx = self + 0x50;
    if (mg != MG_NONE) {
        float m[12];
        ldfn(c, M, mtx, m, 12);
        if (s_hud_logging) {
            char t[9];
            pane_tag(c, M, self, t);
            HUDLOG("orig g%d k%d %-8s m %.3f %.3f %.1f / %.3f %.3f %.1f wh %.1f %.1f alpha %u colours %08x %08x %08x %08x tev %08x %08x", mg, kart, t, m[0], m[1], m[3], m[4], m[5], m[7], w, h,
                   (unsigned)mld(c, M, self + 0xb2, 1), ld32(c, M, sp + 0x18), ld32(c, M, sp + 0x1c), ld32(c, M, sp + 0x20), ld32(c, M, sp + 0x24),
                   ld32(c, M, self + 0x14c), ld32(c, M, self + 0x148));
        }
        float mm[12];
        memcpy(mm, m, sizeof m);
        bool shown = mg >= MGT_PLACE && mg <= MGT_UNIT ? pill_place(c, M, self, mg, kart, mm, w, h) : modern_place(c, M, mg, kart, roll, ndx, ndy, mm, w, h), moved = shown;
        if (shown && (mg == MGT_MAP || mg == MGT_MAP_BATTLE)) {
            char t[9];
            if (pane_tag(c, M, self, t) && !strncmp(t, "course", 6)) s_map_x0 = mm[3], s_map_y0 = mm[7], s_map_x1 = mm[3] + mm[0] * w, s_map_y1 = mm[7] + mm[5] * h;
            if (mg == MGT_MAP && !strncmp(t, "course", 6) && isfinite(mm[7])) s_map_dy = 10 - (mm[7] - s_map_dy); /* its top unmoved to the row's, 6, and the panel's margin */
        }
        uint32_t z = s_hud_zone, gz = g_hud_move[mg].zone;
        if (e >= 1) {
            if (!shown) {
                frame_pop(c, old_sp);
                return 0;
            }
            z = gz | (uint32_t)mg << 8;
        } else if (shown) {
            /* sliding back: both places in the middle of the widescreen HUD (a side moves a
             * picture a third of the 4:3 width out, gx.c), then the game's at e = 0 */
            float t = e * e * (3 - 2 * e), third = g_aspect_scale != 1.0f ? 608.0f / 6 : 0;
            float from = m[3] + third * (float)classic_side(m, w, h), to = mm[3] + third * (gz == 5 ? -1.0f : gz == 6 ? 1.0f : 0.0f);
            for (int k = 0; k < 12; k++) mm[k] = m[k] + (mm[k] - m[k]) * t;
            mm[3] = from + (to - from) * t;
            z = 4;
        }
        if (moved) {
            mtx = sp + 0x30; /* 48 bytes, under the 0x80 frame's end */
            stfn(c, M, mtx, mm, 12);
        }
        if (s_zone_now != z) hud_zone(s_zone_now = z);
    } else {
        uint32_t z = s_hud_zone;
        if (vs) {
            float m[12];
            ldfn(c, M, mtx, m, 12);
            char tag[9];
            pane_tag(c, M, self, tag);
            bool star = !strncmp(tag, "Star", 4);
            int r = vsplit_place(m, w, h, star);
            /* (the left column's end at the screen's middle: the HUD's sides, gx.c, would pull a row apart there) */
            if (star && r > 0 && m[3] < 304) z = 4;
            if (r < 0) {
                frame_pop(c, old_sp);
                return 0;
            }
            if (r) mtx = sp + 0x30, stfn(c, M, mtx, m, 12);
        }
        if (s_trick_hud && trick_item_place(c, M, self, mtx, sp + 0x30, w, h, &z)) mtx = sp + 0x30;
        if (s_zone_now != z) hud_zone(s_zone_now = z);
    }
    if (s_hud_logging && s_hud_zone) {
        char t[9];
        pane_tag(c, M, self, t);
        float m[12];
        ldfn(c, M, mtx, m, 12);
        HUDLOG("z%u g%d k%d %-8s m %.3f %.3f %.1f / %.3f %.3f %.1f wh %.1f %.1f", s_zone_now, mg, kart, t, m[0], m[1], m[3], m[4], m[5], m[7], w, h);
    }
    if (s_coin_track && s_hud_call == 1 && s_hud_n >= 1 && s_hud_n <= 4) coin_seen(c, M, self, mtx, w, h, sp + 0x18);
    n_GXLoadPosMtxImm(c, M, mtx, 0, 0, 0, 0, 0, 0, 0);
    n_GXBegin(c, M, 0x80, 0, 4, 0, 0, 0, 0, 0);
    float zero = ldf(c, M, (uint32_t)RD(c, REG_R2, 4) - 0x69a8);
    const float xs[4] = {zero, w, w, zero}, ys[4] = {zero, zero, h, h};
    const uint32_t col[4] = {0x18, 0x1c, 0x24, 0x20};
    const uint8_t ts[4] = {0, 0x80, 0x80, 0}, tt[4] = {0, 0, 0x80, 0x80};
    uint8_t q[4 * 14];
    for (int v = 0; v < 4; v++) {
        uint8_t *o = q + 14 * v;
        uint32_t u;
        memcpy(&u, &xs[v], 4);
        put_be(o, 4, u);
        memcpy(&u, &ys[v], 4);
        put_be(o + 4, 4, u);
        put_be(o + 8, 4, ld32(c, M, sp + col[v]));
        o[12] = ts[v];
        o[13] = tt[v];
    }
    hw_gather_copy(q, sizeof q);
    frame_pop(c, old_sp);
    return 0;
}

