/* TRICKS (rt.h) and Trick Style. Below the battle code: the kart's tricks themselves (the turns, the
 * rider's tricks, landings, scoring, the signature and the computers' planner; trick.h is what
 * native.c's trick HUD and results read of them). Trick Style (the trick competition, g_trick_comp):
 * a balloon battle on the trick arenas, scored by those tricks. Here the battle around it: the stage files, the battle
 * type page left out, the pause and end menus without CHANGE BATTLE, no balloons, the TRICK STYLE
 * banner and pictures, the results' buttons, and time up ranked by the points. The menu's TRICKS row
 * is battle.c's (it calls trick_comp_set). */
#include "native_impl.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "rt.h"
#include "trick.h"
#include "trickfx.h"
/* A trick battle (a balloon battle, g_trick_comp): no balloon is lost (KartChecker +0x8e, the time
 * before the next can go, kept up), and when the clock runs out (g_trick_ended, native.c) the battle
 * ends ranked by the points: the most keeps its balloons, the others lose theirs (+0x90 0, +0x8c the
 * dead flag 4) with death times (+0x94) in the order of their points, which RaceMgr's balloon ranking
 * (the earliest death last) and RaceDirector's end check (one kart with balloons) read. */
void trick_rank(int order[8], int n) {
    for (int k = 0; k < n; k++) order[k] = k;
    for (int a = 1; a < n; a++)
        for (int b = a; b > 0 && g_trick_score[order[b]] > g_trick_score[order[b - 1]]; b--) { int t = order[b]; order[b] = order[b - 1]; order[b - 1] = t; }
}

void trick_battle_tick(Ctx *c, uint8_t *M, uint32_t rm, int karts) {
    static bool done;
    if (!g_trick_ended) { done = false; }
    int order[8], n = karts > 8 ? 8 : karts;
    for (int k = 0; k < n; k++) {
        uint32_t chk = ld32(c, M, rm + 0x48 + 4 * (uint32_t)k);
        if (ram_ptr(chk) && !done) mst(c, M, chk + 0x8e, 2, 60);
    }
    if (!g_trick_ended || done) return;
    done = true;
    trick_rank(order, n);
    for (int r = 0; r < n; r++) {
        uint32_t chk = ld32(c, M, rm + 0x48 + 4 * (uint32_t)order[r]);
        if (!ram_ptr(chk)) continue;
        if (r == 0) {
            if ((int16_t)ld16(c, M, chk + 0x90) < 1) mst(c, M, chk + 0x90, 2, 1);
            continue;
        }
        mst(c, M, chk + 0x90, 2, 0);
        mst(c, M, chk + 0x8c, 2, ld16(c, M, chk + 0x8c) | 4);
        mst(c, M, chk + 0x94, 4, (uint32_t)(1000 * (n - r))); /* the fewest points died first */
    }
    rt_log("trick comp: time up, kart %d wins with %u points", order[0], g_trick_score[order[0]]);
}
/* ---- the trick battles' stages ----
 * A trick battle is a balloon battle on a trick track. All sixteen (four cups of four, tools/build7.py <out> <track>
 * <stem>: each built onto Cookie Land's slot) are Course/Trick/Tnn.arc; the one picked (trick_pick, from the track
 * picker over the stage page) is what Cookie Land's file (and its L copy, for 3-4 players) points at in the game's
 * file table (in RAM: OSGlobals +0x38), and its course intro name (CourseName/Trick/Tnn_name.bti, tools/art8.py) what
 * Cookie Land's name in every language folder does. The picker always picks Cookie Land, the stage page's first.
 * ResMgr::loadCourseData loads the course again each time (its check for the mounted one never matches). */
const char *const TRICK_CUP_NAME[TRICK_CUPS] = {"KICKER CUP", "CANYON CUP", "CITY CUP", "SUMMIT CUP"};
const char *const TRICK_TRACK_NAME[TRICK_TRACKS] = {
    "TRICK STADIUM",  "MEADOW MOTOPLEX", "CORAL COVE",      "FLOWLINE CANYON", "DUST DEVIL MESA",   "REDROCK RALLY",
    "SUNBURST SPEEDWAY", "VOLCANO VENT", "HARBOR HIGHWAY",  "RHYTHM RIDGE SX", "STEELWORKS",        "NEON NIGHTS",
    "FROSTBITE BIG AIR", "GLACIER GAP",  "ALPINE SUMMIT",   "MEGA RAMP ARENA"};
#define TRICK_SLOT "Mini7"
static const char *const TRICK_LANGS[] = {"English", "French", "German", "Italian", "Spanish", "Japanese"};
#define TRICK_NLANG (sizeof TRICK_LANGS / sizeof *TRICK_LANGS)
#define TRICK_NCRS (2 + TRICK_NLANG) /* the slot's file, its L copy, its name per language */
static struct { bool on; int pick; uint32_t entry[TRICK_NCRS], off[TRICK_NCRS], size[TRICK_NCRS]; } s_tf = {.pick = -1};
/* the game's file table entry of path ("Course/Trick/T01.arc"), 0 if none */
static uint32_t guest_fst_find(Ctx *c, uint8_t *M, const char *path) {
    uint32_t fst = ld32(c, M, 0x80000038u);
    if (!ram_ptr(fst)) return 0;
    uint32_t n = ld32(c, M, fst + 8), names = fst + n * 12;
    if (n < 2 || n > 0x10000) return 0;
    uint32_t i = 1, end = n;
    while (*path) {
        const char *slash = strchr(path, '/');
        size_t len = slash ? (size_t)(slash - path) : strlen(path);
        uint32_t found = 0;
        while (i < end) {
            uint32_t e = fst + i * 12, w = ld32(c, M, e), no = w & 0xffffff;
            bool dir = (w >> 24) != 0;
            bool same = true;
            for (size_t k = 0; k <= len && same; k++) {
                int ch = (int)mld(c, M, names + no + (uint32_t)k, 1);
                int want = k < len ? (unsigned char)path[k] : 0;
                if (tolower(ch) != tolower(want)) same = false;
            }
            if (same && dir == (slash != NULL)) { found = i; break; }
            uint32_t next = dir ? ld32(c, M, e + 8) : i + 1;
            if (next <= i) return 0;
            i = next;
        }
        if (!found) return 0;
        if (!slash) return fst + found * 12;
        end = ld32(c, M, fst + found * 12 + 8), i = found + 1, path = slash + 1;
    }
    return 0;
}
/* the slot's files point at track k's (0..15), the game's own back for k < 0 */
static void trick_files_to(Ctx *c, uint8_t *M, int k) {
    if (k == s_tf.pick) return;
    int n = 0;
    for (size_t i = 0; i < TRICK_NCRS; i++) {
        char a[64], b[64];
        if (i < 2) {
            snprintf(a, sizeof a, "Course/%s%s.arc", TRICK_SLOT, i ? "L" : "");
            snprintf(b, sizeof b, "Course/Trick/T%02d.arc", k + 1);
        } else {
            snprintf(a, sizeof a, "CourseName/%s/%s_name.bti", TRICK_LANGS[i - 2], TRICK_SLOT);
            snprintf(b, sizeof b, "CourseName/Trick/T%02d_name.bti", k + 1);
        }
        if (s_tf.pick < 0) { /* the game's, kept the first time */
            uint32_t e = guest_fst_find(c, M, a);
            s_tf.entry[i] = e;
            if (e) s_tf.off[i] = ld32(c, M, e + 4), s_tf.size[i] = ld32(c, M, e + 8);
        }
        uint32_t e = s_tf.entry[i];
        if (!e) continue;
        uint32_t t = k >= 0 ? guest_fst_find(c, M, b) : 0;
        if (k >= 0 && !t) continue;
        mst(c, M, e + 4, 4, t ? ld32(c, M, t + 4) : s_tf.off[i]), mst(c, M, e + 8, 4, t ? ld32(c, M, t + 8) : s_tf.size[i]);
        n++;
    }
    if (k >= 0) rt_log("trick comp: %s is track %d, %s (%d files)", TRICK_SLOT, k + 1, TRICK_TRACK_NAME[k], n);
    else rt_log("trick comp: the stages are the game's again");
    s_tf.pick = k;
    if (k < 0) memset(s_tf.entry, 0, sizeof s_tf.entry);
}
void trick_pick(Ctx *c, uint8_t *M, int k) {
    if (k >= 0 && k < TRICK_TRACKS) trick_files_to(c, M, k);
}
static void trick_files(Ctx *c, uint8_t *M, bool on) {
    if (on == s_tf.on) return;
    trick_files_to(c, M, on ? 0 : -1);
    s_tf.on = on;
}
/* the competition on (row 4 decided) or off (the mode page again, row 4 not decided) */
/* A battle's karts load the mini-game settings (KartBody::InitBodyParam: KartCtrl::IsMiniGame picks
 * settingMiniPtr2, the setting folder's _car_mini.set files), slower than a race's at any class: a trick battle drives
 * with the race ones (settingPtr2 0x80360f08, settingMiniPtr2 right after it, 21 pointers each). */
#define SET_RACE 0x80360F08u
#define SET_MINI 0x80360F5Cu
static void trick_race_settings(uint8_t *M, bool on) {
    static uint8_t mini[84];
    static bool saved;
    if (on && !saved) {
        /* checked: Mario's entries name i_mario_car.set and i_mario_car_mini.set (another game build: left alone) */
        uint32_t r = get_be(M + (SET_RACE & RAM_MASK), 4), n = get_be(M + (SET_MINI & RAM_MASK), 4);
        r = IS_RAM(r) ? get_be(M + (r & RAM_MASK), 4) : 0, n = IS_RAM(n) ? get_be(M + (n & RAM_MASK), 4) : 0;
        if (!IS_RAM(r) || !IS_RAM(n) || strncmp((char *)M + (r & RAM_MASK), "/setting/i_mario_car.set", 25) ||
            strncmp((char *)M + (n & RAM_MASK), "/setting/i_mario_car_mini.set", 30)) return;
        memcpy(mini, M + (SET_MINI & RAM_MASK), 84), saved = true;
    }
    if (saved) memcpy(M + (SET_MINI & RAM_MASK), on ? M + (SET_RACE & RAM_MASK) : mini, 84);
}
void trick_comp_set(Ctx *c, uint8_t *M, bool on) {
    if (on == (g_trick_comp != 0)) return;
    g_trick_comp = on;
    trick_race_settings(M, on);
    trick_files(c, M, on);
    cheats_trick_maps(on); /* all six stages on the stage page */
    if (on) memset((void *)g_trick_total, 0, sizeof g_trick_total), g_trick_track = 0;
    g_trick_cup = 0, g_trick_round = 0;
}

/* ---- the battle type page, never shown ----
 * Row 4 decides Battle (SceneMenu +0x2100 mode 3); a trick battle is always a balloon battle, so its
 * battle type page is left out of the page states (+0x20e0; +0x20e4 the one before; each page's
 * _end goes to the next it set): 6 mode_init, 8 mode_end, 9 battle_init (the type page, 10 battle,
 * 11 battle_end), 12 character_init. Coming to 9 from the mode page (mode_end, 8), the character page
 * comes next, on balloons (+0x2118 0); from the character page (its B), the mode page. trick_type_skip
 * runs after mode_end (i_mode_end) and from PADRead, so battle_init never runs. */
#define SM_STATE 0x20e0u
#define SM_OLD 0x20e4u
#define SM_COUNT 0x20ecu
#define SM_BTYPE 0x2118u
static uint32_t trick_menu(Ctx *c, uint8_t *M) { /* mScenes[SCENE_MENU] while it runs (msScene 1), else 0 */
    uint32_t seq = ld32(c, M, 0x803CBE20u), m = ram_ptr(seq) ? ld32(c, M, seq + 0x10 + 1 * 4) : 0;
    return ram_ptr(m) && ld32(c, M, 0x803C9CE0u) == 1 ? m : 0;
}
void trick_type_skip(Ctx *c, uint8_t *M, uint32_t m) {
    if (!g_trick_comp || !ram_ptr(m) || ld32(c, M, m + SM_STATE) != 9) return;
    bool fwd = ld32(c, M, m + SM_OLD) == 8;
    mst(c, M, m + SM_BTYPE, 4, 0);
    mst(c, M, m + SM_OLD, 4, 11), mst(c, M, m + SM_STATE, 4, fwd ? 12 : 6), mst(c, M, m + SM_COUNT, 4, 0);
    rt_log("trick comp: past the battle type page to the %s page", fwd ? "character" : "mode");
}
/* The pause screen of a trick battle (pause6.blo, as a balloon battle's) has no CHANGE BATTLE
 * (lanpause.c: its fifth line shows QUIT, the sixth is hidden): Pause2D::mPauseSelector (0x803CB6F8:
 * 0 1 3 4 6 8, the sixth line 8) goes on past 8, the way it was going, and the fifth line's choice
 * (PauseManager::mPauseChoice 0x803CB6E4, 6) is QUIT (8). */
#define P2D_SELECTOR 0x803CB6F8u
#define PM_CHOICE_ 0x803CB6E4u
/* The menu after time up is Result2D's (PauseManager::mThis +0x20), not the pause screen: drawMG names
 * its five lines' pictures every frame (+0xc8 RETRY, +0x134 CHANGE STAGE, +0x1a0 CHANGE CHARACTERS,
 * +0x20c CHANGE BATTLE from the string at 0x8032C780, +0x278 QUIT; the lines +0x50..+0x60). In a trick
 * battle that string reads Mozi_Quit.bti (put back after), the fifth line is hidden, and
 * Result2D::mResultSelector (0x803CB72C: 1 3 4 6 8) skips 8 the way it was going; the fourth line's
 * choice (6) is QUIT's as on the pause screen. */
#define R2D_SELECTOR 0x803CB72Cu
#define R2D_BATTLE_NAME 0x8032C780u
static void trick_result_menu(Ctx *c, uint8_t *M, bool on) {
    static const char game[] = "Mozi_Change_MiniGame.bti", ours[] = "Mozi_Quit.bti";
    const char *want = on ? ours : game, *other = on ? game : ours;
    bool is_other = true;
    for (uint32_t i = 0; i < sizeof game && is_other; i++) {
        if (mld(c, M, R2D_BATTLE_NAME + i, 1) != (uint8_t)other[i]) is_other = false;
        if (!other[i]) break;
    }
    if (is_other) /* only over the game's string or ours */
        for (uint32_t i = 0; i < sizeof game; i++) mst(c, M, R2D_BATTLE_NAME + i, 1, i < strlen(want) ? (uint8_t)want[i] : 0);
    if (!on) return;
    uint32_t pm = ld32(c, M, 0x803CB6D8u), r2d = ram_ptr(pm) ? ld32(c, M, pm + 0x20) : 0;
    if (!ram_ptr(r2d)) return;
    uint32_t line5 = ld32(c, M, r2d + 0x60), quit = ld32(c, M, r2d + 0x278);
    if (ram_ptr(line5)) mst(c, M, line5 + 0xb0, 1, 0);
    if (ram_ptr(quit)) mst(c, M, quit + 0xb0, 1, 0);
    static uint32_t prev;
    uint32_t sel = ld32(c, M, R2D_SELECTOR);
    if (sel == 8) sel = prev == 6 ? 1 : 6, mst(c, M, R2D_SELECTOR, 4, sel);
    prev = sel;
}
static void trick_pause_skip(Ctx *c, uint8_t *M) {
    static uint32_t prev;
    uint32_t sel = ld32(c, M, P2D_SELECTOR);
    bool on = g_trick_comp && ld32(c, M, 0x803B1464u) == 4;
    if (on && !g_trick_cup) { /* a cup's pause menu is the Grand Prix's */
        if (sel == 8) sel = prev == 6 ? 0 : 6, mst(c, M, P2D_SELECTOR, 4, sel);
        if (ld32(c, M, PM_CHOICE_) == 6) mst(c, M, PM_CHOICE_, 4, 8);
    }
    prev = sel;
    trick_result_menu(c, M, on);
}
/* No balloons on the karts in a trick battle, from the race's first frame (the course intro and the
 * countdown too): TBalloonSupervisor (TBalloonString::getSupervisor, r13 - 0x4fbc) +0x28 its list (+0
 * the TBalloonString, +0xc the next); TBalloonString::drawBalloon draws none with +0xec bit 0x20. Only
 * while a race runs (msScene 11, RaceDirector's phase set): the pointer outlives the race before. */
extern volatile uint32_t g_race_phase;
static void trick_balloons(Ctx *c, uint8_t *M) {
    if (!g_trick_comp || ld32(c, M, 0x803B1464u) != 4 || ld32(c, M, 0x803C9CE0u) != 11 || !g_race_phase) return;
    uint32_t sup = ld32(c, M, (uint32_t)ctx_gpr(c, 13) - 0x4fbc), link = ram_ptr(sup) ? ld32(c, M, sup + 0x28) : 0;
    for (int i = 0; i < 32 && ram_ptr(link); i++, link = ld32(c, M, link + 0xc)) {
        uint32_t str = ld32(c, M, link), vt = ram_ptr(str) ? ld32(c, M, str) : 0;
        if (vt >= 0x80300000u && vt < 0x803E0000u) mst(c, M, str + 0xec, 4, ld32(c, M, str + 0xec) | 0x20); /* a vtable in the DOL's data */
    }
}
/* The battle's name over the course intro (the game's font, its strings in the DOL's data, one per
 * language: GM4E01 0x8032dfe8 English, then French, German, Italian, Spanish) reads TRICK STYLE while
 * the trick battle is on, each string's own back after. Each slot is checked for the game's string (or
 * ours) before it is written. */
static const struct { uint32_t at, room; const char *game; } BATTLE_NAME[5] = {
    {0x8032dfe8u, 16, "BALLOON BATTLE"},       {0x8032dff8u, 20, "BATAILLE DE BALLONS"}, {0x8032e00cu, 20, "BALLON-WETTKAMPF"},
    {0x8032e020u, 24, "BATTAGLIA PALLONCINI"}, {0x8032e038u, 20, "BATALLA DE GLOBOS"}};
#define TRICK_NAME "TRICK STYLE"
static bool guest_str_is(Ctx *c, uint8_t *M, uint32_t at, const char *s) { /* s and its null at at */
    for (uint32_t i = 0;; i++) {
        if (mld(c, M, at + i, 1) != (uint8_t)s[i]) return false;
        if (!s[i]) return true;
    }
}
static void guest_str_put(Ctx *c, uint8_t *M, uint32_t at, const char *s) {
    for (uint32_t i = 0;; i++) {
        mst(c, M, at + i, 1, (uint8_t)s[i]);
        if (!s[i]) return;
    }
}
/* ... and the pictures of it: the stage page's name (battlename.arc timg/mozi_battle1.bti, IA4 256 x 32)
 * and the balloon beside it there and in the course intro (race2d.arc timg/cup_pict_balloon.bti, C8 40 x 40,
 * 48 RGB5A3 palette entries). The trick menu.arc (mods/trick, menu4.py from the player's disc) carries
 * timg/mozi_trickbattle.bti (TRICK STYLE) and timg/cup_pict_trick.bti (a ramp), the same formats, kept
 * once found (JKRFileLoader::getGlbResource, every mounted archive); their texels (and palette) go over
 * the game's while the trick battle is on (looked at twice a second: an archive loaded again puts the
 * game's back), the game's own back after. */
#define G_getGlbResource 0x80014594u
#define PIC_MAX (192 * 136 / 2) /* the biggest: a stage thumbnail (CMPR 192 x 136) */
typedef struct {
    const char *game, *ours, *what; /* ours: a texture of the trick menu.arc, or with file set a BTI in <trick dir>/Art */
    bool file;
    uint8_t fmt;
    uint16_t w, h;
    uint32_t img_bytes, pal_bytes;  /* texels, palette (0: none) */
    uint8_t now[PIC_MAX], old[PIC_MAX];
    bool have;
    uint32_t img_at, pal_at;        /* the game's (RAM offsets) while they hold ours, else 0 */
    uint32_t tried;                 /* file: the tick it was last looked for */
} PicSwap;
/* (A file picture (p->file): a BTI from <trick dir>/Art. The stage page's own pictures need none: the track picker
 * covers that page.) */
static PicSwap s_pics[] = {
    {"mozi_battle1.bti", "mozi_trickbattle.bti", "the name", false, 2, 256, 32, 256 * 32, 0},
    {"cup_pict_balloon.bti", "cup_pict_trick.bti", "the balloon", false, 9, 40, 40, 40 * 40, 48 * 2},
};
#define NPICS (sizeof s_pics / sizeof *s_pics)
/* ours from <trick dir>/Art/<name>: true when it is the picture p wants (format, size, texels whole) */
static bool pic_file(PicSwap *p) {
    const char *dir = disc_overlay_trick_dir();
    if (!dir) return false;
    char path[512];
    snprintf(path, sizeof path, "%s/Art/%s", dir, p->ours);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t h[0x20];
    bool ok = fread(h, 1, sizeof h, f) == sizeof h && h[0] == p->fmt && (h[2] << 8 | h[3]) == p->w && (h[4] << 8 | h[5]) == p->h && !p->pal_bytes;
    uint32_t off = ok ? (uint32_t)h[0x1c] << 24 | (uint32_t)h[0x1d] << 16 | (uint32_t)h[0x1e] << 8 | h[0x1f] : 0;
    ok = ok && off >= 0x20 && off < 0x10000 && fseek(f, (long)off, SEEK_SET) == 0 && fread(p->now, 1, p->img_bytes, f) == p->img_bytes;
    fclose(f);
    if (!ok) rt_log("trick comp: %s is not a %ux%u picture of format %u", path, p->w, p->h, p->fmt);
    return ok;
}
/* the picture's texels and palette (RAM offsets) when it is as p wants, else false */
static bool pic_find(Ctx *c, uint8_t *M, const PicSwap *p, const char *name, uint32_t *img, uint32_t *pal) {
    uint32_t old = frame_push(c, M, 0x40), sp = old - 0x40;
    for (size_t i = 0; i <= strlen(name) && i < 0x30; i++) mst(c, M, sp + 8 + (uint32_t)i, 1, (uint8_t)name[i]);
    const uint32_t a[2] = {sp + 8, 0};
    uint32_t t = ctx_call_guest(c, G_getGlbResource, a, 2);
    frame_pop(c, old);
    if (!ram_ptr(t) || !ram_ptr(t + 0x1f) || mld(c, M, t, 1) != p->fmt || ld16(c, M, t + 2) != p->w || ld16(c, M, t + 4) != p->h) return false;
    uint32_t i = t + ld32(c, M, t + 0x1c);
    if (!ram_ptr(i) || !ram_ptr(i + p->img_bytes - 1)) return false;
    *img = i & RAM_MASK, *pal = 0;
    if (p->pal_bytes) {
        uint32_t q = t + ld32(c, M, t + 0xc);
        if (mld(c, M, t + 9, 1) != 2 || ld16(c, M, t + 0xa) * 2u != p->pal_bytes || !ram_ptr(q) || !ram_ptr(q + p->pal_bytes - 1)) return false;
        *pal = q & RAM_MASK;
    }
    return true;
}
static void pic_get(const PicSwap *p, const uint8_t *M, uint32_t img, uint32_t pal, uint8_t *to) {
    memcpy(to, M + img, p->img_bytes);
    if (p->pal_bytes) memcpy(to + p->img_bytes, M + pal, p->pal_bytes);
}
static void pic_put(const PicSwap *p, uint8_t *M, uint32_t img, uint32_t pal, const uint8_t *from) {
    memcpy(M + img, from, p->img_bytes);
    if (p->pal_bytes) memcpy(M + pal, from + p->img_bytes, p->pal_bytes);
}
static bool pic_is(const PicSwap *p, const uint8_t *M, uint32_t img, uint32_t pal, const uint8_t *what) {
    return !memcmp(M + img, what, p->img_bytes) && (!p->pal_bytes || !memcmp(M + pal, what + p->img_bytes, p->pal_bytes));
}
static void pic_swap(Ctx *c, uint8_t *M, PicSwap *p, bool want, uint32_t tick) {
    static uint32_t n;
    bool ours = p->img_at && pic_is(p, M, p->img_at, p->pal_at, p->now);
    if (!want) {
        if (ours) pic_put(p, M, p->img_at, p->pal_at, p->old), rt_log("trick comp: the game's %s picture back", p->what);
        p->img_at = p->pal_at = 0;
        return;
    }
    /* every frame out of a race (an archive loaded again, the stage page opened from a race's pause menu,
     * must not show the game's pictures for even a moment: it can sit somewhere else while the old copy,
     * freed, still holds ours), twice a second while a race runs (g_race_frames moving) */
    extern volatile uint32_t g_race_frames;
    static uint32_t rf_seen, rf_tick;
    if (g_race_frames != rf_seen) rf_seen = g_race_frames, rf_tick = tick;
    bool racing = tick - rf_tick < 3;
    if (racing && tick % 30) return;
    uint32_t img, pal;
    if (!p->have) {
        if (p->file) {
            if (p->tried && tick - p->tried < 600) return; /* a missing or bad file: looked for again every 10 s */
            p->tried = tick;
            if (!pic_file(p)) return;
            p->have = true;
        } else {
            if (!pic_find(c, M, p, p->ours, &img, &pal)) return;
            pic_get(p, M, img, pal, p->now), p->have = true;
        }
    }
    if (!pic_find(c, M, p, p->game, &img, &pal)) return;
    if (pic_is(p, M, img, pal, p->now)) { /* already ours (where it was put, or a copy of an archive holding it) */
        if (img != p->img_at && !ours) p->img_at = img, p->pal_at = pal;
        return;
    }
    pic_get(p, M, img, pal, p->old);
    pic_put(p, M, img, pal, p->now);
    p->img_at = img, p->pal_at = pal;
    rt_log("trick comp: %s picture at %08x is Trick Style's (%u)", p->what, 0x80000000u | img, ++n); /* the count: rt_log drops repeated lines */
}
static void ban_picture(Ctx *c, uint8_t *M, bool want) {
    static uint32_t tick;
    tick++;
    for (size_t i = 0; i < NPICS; i++) pic_swap(c, M, &s_pics[i], want, tick);
}
static void trick_banner(Ctx *c, uint8_t *M) {
    static bool on;
    bool want = g_trick_comp != 0;
    ban_picture(c, M, want);
    if (want == on) return;
    int done = 0;
    for (int i = 0; i < 5; i++) {
        const char *from = want ? BATTLE_NAME[i].game : TRICK_NAME, *to = want ? TRICK_NAME : BATTLE_NAME[i].game;
        if (strlen(to) >= BATTLE_NAME[i].room || !guest_str_is(c, M, BATTLE_NAME[i].at, from)) continue;
        guest_str_put(c, M, BATTLE_NAME[i].at, to), done++;
    }
    on = want;
    rt_log("trick comp: the battle's name %s (%d of 5 languages)", want ? TRICK_NAME : "the game's again", done);
}
/* The trick results (native.c trick_results) wait for A: meanwhile the game sees no button (its battle
 * menu behind them waits); the A that passes them by is held back until let go. */
static void trick_results_pad(uint16_t *btn) {
    static bool held, was;
    bool a = (*btn & 0x0100) != 0;
    if (g_trick_res != 1) {
        was = false;
        if (held && a) *btn &= (uint16_t)~0x0100u;
        else held = false;
        return;
    }
    if (!was) was = true, held = a; /* a held A doesn't count */
    if (a && !held && g_trick_res_t >= 60) g_trick_res = 2, rt_log("trick results: passed by");
    held = a;
    *btn &= (uint16_t)~0x1F0Fu; /* A B X Y START, the d-pad */
}
/* ... and the game's own end (its WIN! box and battle menu, PauseManager::exec) waits behind them: its
 * net byte (PauseManager::mThis +0, set in a LAN race, where exec skips all of it) set from time up
 * until the results are passed by. lanpause.c owns the byte in LAN races (no trick battles there). */
static void trick_results_hold(Ctx *c, uint8_t *M) {
    static bool set;
    uint32_t pm = ld32(c, M, 0x803CB6D8u);
    bool want = g_trick_comp && g_trick_ended && g_trick_res != 2 && !g_lan_play;
    if (!ram_ptr(pm)) { set = false; return; }
    if (want) {
        if (!set) rt_log("trick results: the game's end waits");
        mst(c, M, pm, 1, 1), set = true;
    } else if (set) {
        mst(c, M, pm, 1, 0), set = false;
        rt_log("trick results: the game's end goes on");
    }
}
void trick_battle_pad(Ctx *c, uint8_t *M, uint16_t *btn) {
    trick_results_pad(btn);
    trick_results_hold(c, M);
    trick_pause_skip(c, M);
    trick_type_skip(c, M, trick_menu(c, M));
    trick_balloons(c, M);
    trick_banner(c, M);
}
/* The trick guide (switch/menu.c menu_trick_guide): a page of its own over a trick battle's pause
 * screen (PauseManager::mIsPause 0x803CB6DC), opened with Y (the pause screen leaves Y alone),
 * Left / Right between its pages, B, Y or START back to the pause screen. While it is open, and
 * until the button that closed it is let go, the game sees no buttons and the sticks centred, so
 * the pause menu stays where it was. Any controller. */
#define PM_IS_PAUSE 0x803CB6DCu
#define GUIDE_PAGES 3 /* HOW IT WORKS, KART TRICKS, RIDER TRICKS (menu.c) */
volatile uint32_t g_trick_guide, g_trick_guide_hint;
void trick_guide_pad(Ctx *c, uint8_t *M, PadStatus p[4]) {
    static uint16_t was[4];
    static bool eat[4];
    bool paused = g_trick_comp && !g_trick_ended && ld32(c, M, 0x803B1464u) == 4 && ld32(c, M, 0x803C9CE0u) == 11 &&
                  mld(c, M, PM_IS_PAUSE, 1);
    uint32_t open = paused ? g_trick_guide : 0;
    for (int i = 0; i < 4; i++) {
        uint16_t b = p[i].button | (p[i].stick_x > 50 ? PAD_RIGHT : p[i].stick_x < -50 ? PAD_LEFT : 0); /* the stick turns pages too */
        uint16_t press = (uint16_t)(b & ~was[i]);
        was[i] = b;
        if (!paused || !p[i].connected) continue;
        if (!open) {
            if (press & PAD_Y) open = 1, rt_log("trick guide: open");
        } else if (press & (PAD_B | PAD_Y | PAD_START)) {
            open = 0;
            for (int j = 0; j < 4; j++) eat[j] = true;
            rt_log("trick guide: closed");
        } else if (press & (PAD_RIGHT | PAD_TRIGGER_R)) {
            open = open % GUIDE_PAGES + 1;
        } else if (press & (PAD_LEFT | PAD_TRIGGER_L)) {
            open = (open + GUIDE_PAGES - 2) % GUIDE_PAGES + 1;
        }
    }
    g_trick_guide = open, g_trick_guide_hint = paused && !open;
    for (int i = 0; i < 4; i++) {
        if (eat[i] && !p[i].button) eat[i] = false;
        if (open || eat[i]) p[i].button = 0, p[i].stick_x = p[i].stick_y = p[i].substick_x = p[i].substick_y = 0, p[i].trigger_l = p[i].trigger_r = 0;
    }
}

/* ---- the track picker and the cups ----
 * In a trick battle the stage page (SceneMapSelect, msScene 4) is covered by the track picker (menu.c
 * menu_trick_pick): SINGLE TRACK or TRICK CUP, then a cup (four tracks each), then for a single track one of its
 * four. The game sees none of the buttons meanwhile; once picked, the track is put on Cookie Land's files
 * (trick_pick) and A pressed for the game every few frames (the stage page's cursor is always on Cookie Land, its
 * first) until the race loads. B on the first page: B for the game, back to the character page.
 * A cup: four rounds, the points of each added to g_trick_total (each kart's), ranked by them. After a round's
 * results (g_trick_res 2) the game's end menu is given CHANGE STAGE (Result2D::mResultSelector 3) and A, so the stage
 * page comes again and the picker shows the next round (page 4) a moment and goes on to it. After the last round's
 * results the same way to the podium (page 5: first to third; RACE AGAIN, SELECT CUP, QUIT), else back to the cups. Any other way to the stage page (the pause menu's CHANGE STAGE or
 * CHANGE CHARACTERS) ends the cup. */
volatile uint32_t g_tp_page, g_tp_mode, g_tp_cup, g_tp_track, g_tp_t, g_trick_cup, g_trick_round, g_trick_pick, g_tp_confirm, g_tp_csel, g_tp_place;
static bool s_cup_next, s_cup_done;
static int s_go;           /* 1: A for the game until the race loads, 2: B until the character page */
static uint32_t s_go_t;
static uint16_t pad_dirs(const PadStatus *p) {
    return (uint16_t)(p->button | (p->stick_x > 50 ? PAD_RIGHT : p->stick_x < -50 ? PAD_LEFT : 0) |
                      (p->stick_y > 50 ? PAD_UP : p->stick_y < -50 ? PAD_DOWN : 0));
}
static void pad_none(PadStatus p[4]) {
    for (int i = 0; i < 4; i++) p[i].button = 0, p[i].stick_x = p[i].stick_y = p[i].substick_x = p[i].substick_y = 0, p[i].trigger_l = p[i].trigger_r = 0;
}
/* the buttons newly pressed on any connected pad (was: last frame's, kept by the caller) */
static uint16_t pad_press(const PadStatus p[4], uint16_t was[4], uint16_t *held_out) {
    uint16_t press = 0, held = 0;
    for (int i = 0; i < 4; i++) {
        if (!p[i].connected) { was[i] = 0; continue; }
        uint16_t b = pad_dirs(&p[i]);
        press |= (uint16_t)(b & ~was[i]), held |= b, was[i] = b;
    }
    if (held_out) *held_out = held;
    return press;
}
static void tp_go(Ctx *c, uint8_t *M, int k) {
    g_trick_pick = (uint32_t)k;
    trick_pick(c, M, k);
    s_go = 1, s_go_t = 0;
    rt_log("trick picker: %s%s, track %d %s", g_trick_cup ? TRICK_CUP_NAME[g_trick_cup - 1] : "single track", g_trick_cup ? " round" : "",
           k + 1, TRICK_TRACK_NAME[k]);
}
/* a cup from its first round (the cup page's A, the podium's RACE AGAIN) */
static void cup_start(Ctx *c, uint8_t *M) {
    g_trick_cup = g_tp_cup + 1, g_trick_round = 0;
    memset((void *)g_trick_total, 0, sizeof g_trick_total);
    tp_go(c, M, (int)g_tp_cup * 4);
}
/* a cup's round over and its results passed by: CHANGE STAGE on the end menu, as a Grand Prix goes on to its next
 * course by itself (after the last round: back to the cups) */
static void cup_between(Ctx *c, uint8_t *M, PadStatus p[4], uint32_t scene) {
    static uint32_t t;
    if (!g_trick_cup || !g_trick_comp || scene != 11 || g_trick_res != 2) { t = 0; return; }
    if (!t) {
        if (g_trick_round >= 3) { /* your place in the cup (kart 0, ties share the place) */
            int n = (int16_t)ld16(c, M, 0x803B145Cu + 0x1c);
            uint32_t place = 0;
            for (int i = 1; i < n && i < 8; i++) place += g_trick_total[i] > g_trick_total[0];
            const char *fp = getenv("MKDD_TRICK_PLACE"); /* tests: the place (1-4) */
            if (fp && atoi(fp) >= 1) place = (uint32_t)atoi(fp) - 1;
            g_tp_place = place;
            rt_log("trick cup: %s over, you %u points, place %u of %d", TRICK_CUP_NAME[g_trick_cup - 1], g_trick_total[0], place + 1, n);
        } else rt_log("trick cup: round %u done, to the next", g_trick_round + 1);
    }
    s_cup_next = g_trick_round < 3, s_cup_done = g_trick_round >= 3;
    /* the end menu (RETRY, CHANGE STAGE ...) is never seen: its lines and their words hidden */
    uint32_t pm = ld32(c, M, 0x803CB6D8u), r2d = ram_ptr(pm) ? ld32(c, M, pm + 0x20) : 0;
    if (ram_ptr(r2d)) {
        static const uint16_t PANE[] = {0x50, 0x54, 0x58, 0x5c, 0x60, 0xc8, 0x134, 0x1a0, 0x20c, 0x278};
        for (size_t i = 0; i < sizeof PANE / sizeof *PANE; i++) {
            uint32_t q = ld32(c, M, r2d + PANE[i]);
            if (ram_ptr(q)) mst(c, M, q + 0xb0, 1, 0);
        }
    }
    pad_none(p);
    mst(c, M, R2D_SELECTOR, 4, 3); /* CHANGE STAGE */
    if (++t > 30 && t % 16 < 3) p[0].button = PAD_A;
}
/* A cup's pause menu is the Grand Prix's (lanpause.c: CONTINUE, QUIT). QUIT asks first (menu.c draws it): YES is the
 * game's soft reset to the title screen, NO (or B) closes the pause menu: CONTINUE and A for the game. */
#define PM_IS_PAUSE_ 0x803CB6DCu
#define RESET_STATE_ 0x803CBE80u
static void soft_reset(Ctx *c, uint8_t *M) { /* to the title screen */
    if (!ld32(c, M, RESET_STATE_) && !mld(c, M, RESET_STATE_ + 9, 1)) {
        mst(c, M, RESET_STATE_ + 8, 1, 0); /* ResetAgent::reset(false) */
        mst(c, M, RESET_STATE_, 4, 1);
    }
}
static void cup_pause_pad(Ctx *c, uint8_t *M, PadStatus p[4], uint32_t scene) {
    static uint16_t was[4];
    static uint32_t resume;
    bool paused = g_trick_cup && g_trick_comp && scene == 11 && mld(c, M, PM_IS_PAUSE_, 1);
    uint16_t press = pad_press(p, was, NULL);
    if (!paused) { g_tp_confirm = 0, resume = 0; return; }
    if (resume) { /* NO: CONTINUE picked, A until the pause menu closes */
        pad_none(p);
        mst(c, M, P2D_SELECTOR, 4, 0);
        if (++resume % 12 < 3) p[0].button = PAD_A;
        if (resume > 300) resume = 0;
        return;
    }
    if (!g_tp_confirm) {
        if ((press & PAD_A) && ld32(c, M, P2D_SELECTOR) == 8) {
            g_tp_confirm = 1, g_tp_csel = 1;
            pad_none(p);
            rt_log("trick cup: QUIT? asked");
        }
        return;
    }
    pad_none(p);
    g_tp_t++; /* menu.c's QUIT? pulses by it (the picker's clock, ticked only here and in trick_pick_pad) */
    if (press & (PAD_LEFT | PAD_RIGHT | PAD_UP | PAD_DOWN)) g_tp_csel ^= 1;
    if ((press & PAD_B) || ((press & PAD_A) && g_tp_csel)) {
        g_tp_confirm = 0, resume = 1;
        rt_log("trick cup: QUIT? no, back to the race");
    } else if (press & PAD_A) {
        g_tp_confirm = 0;
        soft_reset(c, M);
        rt_log("trick cup: QUIT? yes, %s left for the title screen", TRICK_CUP_NAME[g_trick_cup - 1]);
        g_trick_cup = 0, s_cup_next = s_cup_done = false;
    }
}
void trick_pick_pad(Ctx *c, uint8_t *M, PadStatus p[4]) {
    static uint16_t was[4];
    static bool on, eat;
    uint32_t scene = ld32(c, M, 0x803C9CE0u);
    cup_between(c, M, p, scene);
    cup_pause_pad(c, M, p, scene);
    if (!g_trick_comp || g_lan_play || scene != 4) {
        if (on) rt_log("trick picker: closed");
        on = false, g_tp_page = 0, s_go = 0;
        if (scene == 4 && !g_trick_comp) g_trick_cup = 0, s_cup_next = s_cup_done = false;
        return;
    }
    if (!on) {
        on = true, s_go = 0, eat = true, g_tp_t = 0;
        if (s_cup_next && g_trick_cup && g_trick_round < 3) g_trick_round++, g_tp_page = 4;
        else if (s_cup_done && g_tp_place < 3) g_trick_cup = 0, g_tp_mode = 1, g_tp_page = 5, g_tp_csel = 0; /* the podium */
        else if (s_cup_done) g_trick_cup = 0, g_tp_mode = 1, g_tp_page = 2;
        else g_trick_cup = 0, g_tp_page = 1;
        s_cup_next = s_cup_done = false;
        rt_log("trick picker: open (page %u)", g_tp_page);
    }
    g_tp_t++;
    uint16_t held;
    uint16_t press = pad_press(p, was, &held);
    if (eat) press = 0, eat = held != 0; /* the A that ended the character page */
    if (s_go || g_tp_t < 12) press = 0;
    bool up = press & (PAD_UP | PAD_LEFT), down = press & (PAD_DOWN | PAD_RIGHT), a = press & PAD_A, b = press & PAD_B;
    switch (g_tp_page) {
    case 1: /* SINGLE TRACK / TRICK CUP */
        if (up || down) g_tp_mode ^= 1;
        if (a) g_tp_page = 2, g_tp_t = 12;
        if (b) s_go = 2, s_go_t = 0, rt_log("trick picker: back to the characters");
        break;
    case 5: /* the podium: RACE AGAIN, SELECT CUP, QUIT */
        if (press & (PAD_UP | PAD_LEFT)) g_tp_csel = (g_tp_csel + 2) % 3;
        if (press & (PAD_DOWN | PAD_RIGHT)) g_tp_csel = (g_tp_csel + 1) % 3;
        if (b || (a && g_tp_csel == 1)) g_tp_page = 2, g_tp_t = 12;
        else if (a && g_tp_csel == 2) {
            g_tp_page = 0, s_go = 3; /* nothing more for the game: the reset takes it */
            soft_reset(c, M);
            rt_log("trick cup: podium, quit to the title screen");
        } else if (a) cup_start(c, M);
        break;
    case 2: /* the cups */
        if (press & PAD_LEFT) g_tp_cup = (g_tp_cup + TRICK_CUPS - 1) % TRICK_CUPS, g_tp_t = 12;
        if (press & PAD_RIGHT) g_tp_cup = (g_tp_cup + 1) % TRICK_CUPS, g_tp_t = 12;
        if (a && g_tp_mode) cup_start(c, M);
        else if (a) g_tp_page = 3, g_tp_track = 0;
        if (b) g_tp_page = 1;
        break;
    case 3: /* a cup's tracks: the list */
        if (press & PAD_UP) g_tp_track = (g_tp_track + 3) % 4;
        if (press & PAD_DOWN) g_tp_track = (g_tp_track + 1) % 4;
        if (a) g_trick_cup = 0, tp_go(c, M, (int)(g_tp_cup * 4 + g_tp_track));
        if (b) g_tp_page = 2;
        break;
    case 4: /* the cup's next round: shown a moment, then on */
        if (!s_go && g_tp_t > 150) tp_go(c, M, (int)((g_trick_cup - 1) * 4 + g_trick_round));
        break;
    }
    pad_none(p);
    if (s_go) {
        uint32_t t = s_go_t++;
        if (s_go < 3 && t >= 6 && t % 16 < 3) p[0].button = s_go == 1 ? PAD_A : PAD_B;
        if (t > 900) s_go = 0, rt_log("trick picker: the stage page didn't go on");
    }
}

/* ---- TRICKS (rt.h): the drawn kart turns, the physics don't. FUN_802d3c60 (KartDisp::MakeKartDisp,
 * each kart each frame, after the cameras: KartCtrl::DynamicsStage2) builds the display matrices
 * from the kart's pose (KartBody +0x110): the body's display pose +0x1a0 (the body model, both
 * drivers and the shadow get it: body +0 KartLoader (+0x14 the body's ExModel), +0x20 / +0x24
 * the drivers, +0x28 the ShadowModel, its height on the ground at +0xa4) and each wheel's
 * (KartSus +0x158 wheel, +0x1e8 arm, +0x248 shock; KartCtrl (r13 - 0x4e98) +0xc0: KartSus *[4 per
 * kart]), then FUN_802d320c (MakeTireDispMatrix) and the body's step hand them to the models.
 * Before FUN_802d320c those matrices are turned round a point above the kart's centre; after
 * MakeKartDisp they are put back and the shadow gets the level pose again, so nothing else
 * (water checks, the next frame's matrices) sees the turn. KartDisp +0 = KartBody, +0x5b3 its kart
 * number; +0x5a4 non-zero while its wheels are on the ground; +0xd4 its KartStar, +0xdc its KartCrash.
 * Tricks start once the kart is TRICK_MIN_AIR frames in the air and TRICK_MIN_H up (or
 * TRICK_LONG_AIR frames in the air): a ramp or a drop, not the drift hop.
 * The D-pad (or a stick flick, the front end's), HELD: the kart turns while it is held, SPIN_RATE at
 * full speed: up flips it forward, down back, left / right spin it flat, a diagonal both at once.
 * Let go it stops (and rights itself when close to upright). A short tap then the same direction
 * again: that direction's special (right a barrel roll, left a whip, up a twister, down a back
 * twister), from where the kart was before the tap. JUMP: the back rider's tricks, superman then
 * cliffhanger (the DriverModels, +0x18 / +0x1c: their J3DModels' base matrices are the display
 * pose, set in MakeKartDisp; turned again after it round the rider's own point); a direction just
 * after JUMP a pose instead (right sidewinder, left reverse sidewinder, up handstand, down lazy boy).
 * ZR (the game's R) pressed in the air, then a direction within POSE_IN: a rider trick (up kiss of death,
 * down cordova, left nac-nac, right can-can; the same again soon after, ZR held: flair, switcheroo (the game's
 * driver swap), body varial, rock solid).
 * R held from a drift doesn't count (a press in the air), nor with ZL (the signature), nor just before touchdown (a catch).
 * Touchdown is graded by how far the drawn kart is from the real one: PERFECT (LAND_PERFECT) the
 * mini-turbo's boost (+0x570 0x200, +0x59e frames left, which DoDash counts down) for the turns
 * done, SKETCHY (LAND_SKETCHY) none and some speed (KartBody +0x444) lost, else (or upside down) a
 * BAIL: KartCrash::MakeHalfSpin (FUN_802c9a34, as a wall hit at speed does). L / R pressed just
 * before touchdown and held CATCHES it: sketchy counts as clean. The competition (g_trick_comp)
 * scores: each trick's points (more for each extra turn, less when it is among the kart's last
 * few), a combo carried from jump to jump by clean landings within COMBO_WINDOW, times the number
 * of different tricks in it, banked when it ends (a bail loses it); a meter filled by tricks, full
 * ZL + ZR in the air: the signature (HURRICANE; with up METEOR, down TEMPEST, left CYCLONE, right TYPHOON): a star (KartStar::MakeStar
 * 0x802C79D0, its timer cut to SIG_STAR) and a long boost. Speeds, rules and the CPUs' planner
 * (tk_cpu) below. A first-person camera in the kart turns with it (tk_fp_cams). ---- */
volatile uint32_t g_trick_comp, g_trick_frames, g_trick_score[8], g_trick_live; /* trick competition (rt.h) */
volatile uint32_t g_trick_total[8], g_trick_track, g_trick_ended, g_trick_music, g_trick_newrace;
static bool s_tk_force; /* the 2:00 is up: kart 0's KartChecker is forced to the goal after this kart step (i_FUN_802d3c60) */
volatile uint32_t g_tricks, g_trick_pad; /* g_tricks: GAME > TRICKS, 0 off, 1 on, 2 with wipeouts (menu.c cheats_apply) */
#define TRICK_FRAMES 36     /* the specials' lengths are for this; MKDD_TRICK_FRAMES slows them (tests) */
#define TRICK_MIN_AIR 8     /* frames in the air before tricks start (the drift hop lands sooner), */
#define TRICK_MIN_H 150.0f  /* and this high above the ground (the shadow), */
#define TRICK_LONG_AIR 40   /* or this long in the air and TRICK_LATE_H up: a bump stays lower and lands sooner */
#define TRICK_LATE_H 120.0f
#define TRICK_MIN_RISE 150.0f /* ... or risen this far since takeoff (off a ramp the shadow sits on its far face:
                              * h stayed under TRICK_MIN_H until 25 frames from the landing) */
#define TRICK_H_MAX 20000.0f /* a kart higher above its shadow than this has none: the reading is not used */
#define SPIN_RATE 0.2618f   /* a held direction's turn a frame at full speed, radians: 900 degrees a second (a ramp
                             * here gives 50-65 frames of air, 35-50 of them past the start: a 360, or a 720 off the big ones) */
#define SPIN_RAMP 2         /* frames from still to full speed (a tap turns it a little) */
#define SPIN_STOP 0.55f     /* let go: this much of the speed kept each frame */
#define SPIN_LATE 4         /* a direction pressed this few frames from the landing is steering, not a trick */
#define TURN_SLACK 0.35f    /* a full turn counts this short of 360 degrees (20) */
#define ASSIST_DEG 50.0f    /* nothing held or running: within this of upright the kart rights itself, */
#define ASSIST_KEEP 0.85f   /* keeping this much of the angle each frame; */
#define ASSIST_LAND 14      /* ... and this few frames from touchdown from any angle short of ASSIST_LAND_MAX, */
#define ASSIST_LAND_MAX 120.0f
#define ASSIST_LAND_KEEP 0.8f /* faster (forgiving landings, 2026-11-10) */
#define DTAP_IN 20          /* frames: the same direction again this soon after a short tap is its special */
#define DTAP_TAP 12         /* ... a tap held at most this long */
#define POSE_IN 12          /* frames after JUMP in the air: a direction makes the rider's pose (and after ZR: a ZR trick) */
#define LAND_PERFECT 25.0f  /* degrees between the drawn and the real kart at touchdown: perfect, */
#define LAND_SKETCHY 75.0f  /* sketchy; more, or upside down, a bail */
#define LAND_COUNT 0.85f    /* a special or rider trick this far through at touchdown still counts */
#define CATCH_IN 24         /* frames before touchdown: L / R pressed then and still held catches the landing */
#define PRE_BUF 12          /* frames: JUMP, ZR or a direction pressed this soon before takeoff counts in the air */
#define DIR_EARLY 6         /* frames: a direction pressed this soon before JUMP or ZR (still held) is that button's trick, its turn undone */
#define METER_SHARE 3       /* the meter gains a landed jump's points / METER_SHARE */
#define SIG_STAR 240        /* the signature's star, frames */
#define SIG_BOOST 180       /* ... and its landing's boost */
#define SKETCHY_SPEED 0.8f  /* a sketchy landing keeps this much speed */
#define SQUASH_DY 9.0f      /* a perfect landing: the body dips this far onto its springs, */
#define SQUASH_KEEP 0.8f    /* coming back up */
#define TRICK_BOOST 45       /* a clean landing's boost, frames: this, */
#define TRICK_BOOST_TURN 25  /* and this for each turn past the first, */
#define TRICK_BOOST_MAX 150  /* at most (2026-11-10: was 20 / 15 / 75) */
#define TRICK_PIVOT_UP 40.0f
#define LAND_HOP_IN 14       /* off the ground again this few frames after a jump's touchdown: the landing's bounce, */
#define LAND_HOP_KEEP 0.35f  /* its upward speed (KartBody::mVel +0x260, y +0x264) kept each frame: the trick tracks' drops are
                              * deeper than the game's, and GroundReflection threw the kart back up off them (2026-11-12) */
#define TRICK_SETTLE 0.6f   /* landed: this much of the turn left to upright kept each frame */
#define TRICK_BLEND 0.8f    /* a special started mid-turn: this much of the step between kept each frame */
#define TRICK_JUMP 16       /* tk_pad: the JUMP button (g_jump_held), the rider's tricks */
#define TRICK_L 32          /* ... the game's L and R (g_pad_buttons) */
#define TRICK_R 64
#define TRICK_SIG 128       /* ... the signature's ZL + ZR (g_trick_pad bit TRICK_SIG_BIT + port) */
enum { TK_IDLE, TK_QUEUED, TK_SPIN, TK_SETTLE };
static const char *const T_NAME[T_COUNT] = {"", "FRONT FLIP", "BACKFLIP", "SPIN", "BARREL ROLL", "WHIP", "TWISTER",
                                            "BACK TWISTER", "SUPERMAN", "CLIFFHANGER", "SIDEWINDER", "REVERSE SIDEWINDER",
                                            "HANDSTAND", "LAZY BOY", "HURRICANE", "TEMPEST", "METEOR", "CYCLONE",
                                            "TYPHOON", "KISS OF DEATH", "CORDOVA", "NAC-NAC", "CAN-CAN", "BODY VARIAL", "ROCK SOLID", "FLAIR", "SWITCHEROO",
                                            "HELICOPTER", "SOMERSAULT", "CARTWHEEL", "SURFER", "RODEO", "CORKSCREW"};
enum { K_NONE, K_ROLL, K_WHIP, K_TWIST, K_BTWIST, K_HURRICANE, K_TEMPEST, K_METEOR, K_CYCLONE, K_TYPHOON, K_COUNT }; /* the kart's canned tricks */
static const uint8_t K_TRICK[K_COUNT] = {T_NONE, T_ROLL, T_WHIP, T_TWIST, T_BTWIST, T_HURRICANE, T_TEMPEST, T_METEOR, T_CYCLONE, T_TYPHOON};
static const uint8_t K_LEN[K_COUNT] = {0, 28, 32, 36, 36, 46, 46, 46, 46, 46}; /* frames at TRICK_FRAMES */
enum { RD_SUPERMAN = 1, RD_CLIFF, RD_SIDEW, RD_RSIDEW, RD_HANDSTAND, RD_LAZY,
       RD_KISS, RD_CORDOVA, RD_NACNAC, RD_CANCAN, RD_VARIAL, RD_ROCKSOLID, RD_FLAIR, RD_SWITCH,
       RD_HELI, RD_SOMER, RD_CART, RD_SURF, RD_RODEO, RD_CORK, RD_COUNT }; /* the rider's tricks: RD_TRICK */
static const uint8_t RD_TRICK[RD_COUNT] = {T_NONE, T_SUPERMAN, T_CLIFF, T_SIDEW, T_RSIDEW, T_HANDSTAND, T_LAZY,
                                           T_KISS, T_CORDOVA, T_NACNAC, T_CANCAN, T_VARIAL, T_ROCKSOLID, T_FLAIR, T_SWITCH,
                                           T_HELI, T_SOMER, T_CART, T_SURF, T_RODEO, T_CORK};
Trick g_tk[8];
extern volatile uint32_t g_race_phase; /* hle.c: RaceDirector's phase (0x11 running) */
static float s_tk_a[12];           /* this frame's turn for the kart being built: rotation rows + translation */
static float s_tk_dip[3];          /* ... and the body's own dip (not the wheels') */
static uint32_t s_tk_body;          /* that kart's body while its matrices are turned, 0 none */
static float s_tk_save[13][12];     /* the matrices before the turn: +0x1a0, then 4 wheels x 3 */
static uint32_t s_tk_sus[4];
static float s_rd_t[12];            /* this frame's turn for the back rider, in its model's own frame; */
static bool s_rd_on;                /* set by tk_step for the kart being built */
static bool s_tk_wipe;              /* that kart spins out after MakeKartDisp */
static float tk_ease(float t) { return t <= 0 ? 0 : t >= 1 ? 1 : t * t * (3 - 2 * t); }
/* out, hold, back: 0 to 1 over the first 30%, 1, back to 0 over the last 35% */
static float tk_hold(float t) { return t < 0.3f ? tk_ease(t / 0.3f) : t > 0.65f ? tk_ease((1 - t) / 0.35f) : 1; }
static void q_axis(float q[4], int axis, float a) { q[0] = cosf(a * 0.5f), q[1] = q[2] = q[3] = 0, q[1 + axis] = sinf(a * 0.5f); }
static void q_mul(float o[4], const float a[4], const float b[4]) {
    float r[4] = {a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3], a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
                  a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1], a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0]};
    memcpy(o, r, sizeof r);
}
static void q_id(float q[4]) { q[0] = 1, q[1] = q[2] = q[3] = 0; }
static void q_norm(float q[4]) {
    float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(n > 1e-6f) || !isfinite(n)) { q_id(q); return; }
    for (int i = 0; i < 4; i++) q[i] /= n;
}
/* q's turn the short way round, k times as far; returns that turn's angle before scaling */
static float q_scale(float q[4], float k) {
    float s = q[0] < 0 ? -1.0f : 1.0f, w = q[0] * s, v = sqrtf(q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    float a = 2 * atan2f(v, w);
    if (!(v > 1e-7f) || !isfinite(a)) { q_id(q); return 0; }
    float h = a * k * 0.5f, f = sinf(h) / v * s;
    q[0] = cosf(h), q[1] *= f, q[2] *= f, q[3] *= f;
    return a;
}
static float q_deg(const float q[4]) { return 2 * acosf(fminf(1.0f, fabsf(q[0]))) * 57.29578f; } /* its angle the short way */
/* a canned trick's turn (the kart's own frame: x across, y up, z forward) at t (0-1) */
static void tk_quat(float q[4], int kind, float t) {
    float e = 6.2831853f * tk_ease(t), h = tk_hold(t), a[4], b[4];
    switch (kind) {
    case K_ROLL: q_axis(q, 2, -e); return;
    case K_WHIP: q_axis(a, 1, 1.2f * h), q_axis(b, 2, 0.5f * h), q_mul(q, a, b); return; /* thrown sideways, laid over */
    case K_TWIST: q_axis(a, 0, e), q_axis(b, 1, e), q_mul(q, a, b); return;           /* a front flip and a spin at once */
    case K_BTWIST: q_axis(a, 0, -e), q_axis(b, 1, -e), q_mul(q, a, b); return;
    case K_HURRICANE: q_axis(a, 1, 3 * e), q_axis(b, 2, 0.45f * sinf(3.1415927f * t)), q_mul(q, a, b); return; /* three spins, leaning */
    case K_TEMPEST: q_axis(a, 0, -2 * e), q_axis(b, 1, e), q_mul(q, a, b); return;   /* a double backflip round a spin */
    case K_METEOR: q_axis(q, 0, 3 * e); return;                                       /* three front flips */
    case K_CYCLONE: /* two spins left, thrown on its side halfway through */
        q_axis(a, 1, 2 * e), q_axis(b, 2, 1.4f * sinf(3.1415927f * t)), q_mul(q, a, b); return;
    case K_TYPHOON: q_axis(a, 2, -2 * e), q_axis(b, 0, e), q_mul(q, a, b); return;    /* two barrel rolls right round a front flip */
    default: q_id(q);
    }
}
/* the jump's trick id once more (n: how many now, 0 one more) */
static void tk_add(Trick *t, int id, int n, uint32_t kart) {
    int i = 0;
    while (i < t->jn && t->jt[i] != id) i++;
    if (i == t->jn) {
        if (t->jn >= 8) return;
        t->jt[t->jn] = (uint8_t)id, t->jc[t->jn++] = 0;
    }
    t->jc[i] = (uint8_t)(n ? n : t->jc[i] + 1 > 9 ? 9 : t->jc[i] + 1);
    /* the whole jump's tricks so far, in the order first done: "SPIN + DOUBLE FLIP + ROLL" */
    size_t o = 0;
    t->pop[0] = 0;
    for (int j = 0; j < t->jn && o < sizeof t->pop; j++) {
        int c = t->jc[j], tid = t->jt[j];
        const char *sep = j ? " + " : "";
        int r;
        if (tid == T_SPIN) r = snprintf(t->pop + o, sizeof t->pop - o, "%s%d", sep, 360 * c);
        else if (c == 1) r = snprintf(t->pop + o, sizeof t->pop - o, "%s%s", sep, T_NAME[tid]);
        else if (c == 2) r = snprintf(t->pop + o, sizeof t->pop - o, "%sDOUBLE %s", sep, T_NAME[tid]);
        else if (c == 3) r = snprintf(t->pop + o, sizeof t->pop - o, "%sTRIPLE %s", sep, T_NAME[tid]);
        else r = snprintf(t->pop + o, sizeof t->pop - o, "%s%dX %s", sep, c, T_NAME[tid]);
        if (r < 0) break;
        o += (size_t)r;
    }
    t->pop_t = 75;
    rt_log("trick: kart %u %s", kart, t->pop);
}
static uint32_t tk_mult(const Trick *t) {
    uint32_t n = (uint32_t)__builtin_popcount(t->cids);
    return n < 1 ? 1 : n > COMBO_MULT_MAX ? COMBO_MULT_MAX : n;
}
/* the combo ends: the multiplier's bonus on its points to the kart's score (force: at time up). Each
 * landing's own points went on the score at touchdown (tk_land), so the HUD's score moves at once. */
static void tk_bank(Trick *t, uint32_t kart, bool force) {
    if (t->combo > 0.5f) {
        uint32_t mult = tk_mult(t), pts = (uint32_t)(t->combo * (float)(mult - 1) + 0.5f);
        if (g_trick_comp && (!g_trick_ended || force) && pts) {
            g_trick_score[kart] += pts;
            t->bank = pts, t->bank_t = 100;
        }
        rt_log("trick: kart %u combo banked: %.0f x%u, bonus %u (score %u)", kart, t->combo, mult, pts, g_trick_score[kart]);
    }
    t->combo = 0, t->cids = 0, t->window = 0;
}
/* start canned kind from base (the kart's turn under it), blended from where the kart is */
static void tk_can(Trick *t, int kind, const float base[4], int frames, uint32_t kart) {
    float inv[4] = {base[0], -base[1], -base[2], -base[3]};
    t->ck = kind, t->cf = 0, t->cl = K_LEN[kind] * frames / TRICK_FRAMES;
    if (t->cl < 10) t->cl = 10;
    memcpy(t->cbase, base, sizeof t->cbase);
    q_mul(t->cblend, t->qc, inv);
    t->w[0] = t->w[1] = 0;
    rt_log("trick: kart %u starts a %s", kart, T_NAME[K_TRICK[kind]]);
}
/* out = a (3x4: rotation + translation) applied to the 3x4 matrix at addr, plus dip (NULL none) */
static void tk_apply(Ctx *c, uint8_t *M, uint32_t addr, float save[12], const float *dip) {
    float m[12], o[12];
    ldfn(c, M, addr, m, 12);
    memcpy(save, m, sizeof m);
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 4; k++)
            o[4 * r + k] = s_tk_a[4 * r] * m[k] + s_tk_a[4 * r + 1] * m[4 + k] + s_tk_a[4 * r + 2] * m[8 + k] + (k == 3 ? s_tk_a[4 * r + 3] + (dip ? dip[r] : 0) : 0);
    for (int i = 0; i < 12; i++)
        if (!isfinite(o[i])) return;
    stfn(c, M, addr, o, 12);
}
/* the controller ports driving this kart (driver, rider): TRICK_* bits, TRICK_JUMP, TRICK_L / R; *human
 * when a port drives it */
static uint32_t tk_pad(Ctx *c, uint8_t *M, uint32_t kart, bool *human) {
    uint32_t ctrl = ld32(c, M, (uint32_t)ctx_gpr(c, 13) - 0x4e98), bits = 0;
    *human = false;
    for (int r = 0; r < 2 && ram_ptr(ctrl); r++) {
        uint32_t pad = ld32(c, M, ctrl + 0x20 + 4 * r + 8 * kart);
        uint32_t port = ram_ptr(pad) ? ld32(c, M, pad + 0xac) : 4;
        if (port >= 4) continue;
        uint16_t b = g_pad_buttons[port];
        bits |= (g_trick_pad >> 4 * port & 15) | (g_jump_held >> port & 1 ? TRICK_JUMP : 0) | (b & PAD_TRIGGER_L ? TRICK_L : 0) |
                (b & PAD_TRIGGER_R ? TRICK_R : 0) | (g_trick_pad >> (TRICK_SIG_BIT + port) & 1 ? TRICK_SIG : 0);
        *human = true;
    }
    return bits;
}
/* a computer's buttons for the jump: one plan picked once tricks are possible (a hold for one or two
 * turns, as many as the jump looks long enough for; a special; a rider trick or pose; the signature
 * when its meter is full), and a catch at the landing now and then */
static uint32_t tk_cpu(Trick *t, uint32_t kart, bool ground, bool up, float h, float land) {
    static uint32_t rng = 0x2545F491u;
    rng ^= rng << 13, rng ^= rng >> 17, rng ^= rng << 5;
    uint32_t r = rng + kart * 2654435761u;
    if (ground) { t->cp_plan = 0; return 0; }
    if (!up) return 0;
    static const uint8_t DIRS[8] = {TRICK_UP, TRICK_DOWN, TRICK_LEFT, TRICK_RIGHT, TRICK_UP | TRICK_LEFT, TRICK_UP | TRICK_RIGHT,
                                    TRICK_DOWN | TRICK_LEFT, TRICK_DOWN | TRICK_RIGHT};
    if (!t->cp_plan) {
        /* frames left in the air: up at -fall, then down from the top (h low off a ramp: short, so careful) */
        float g = t->grav, vr = -t->fall, left = 45;
        if (g > 0.05f) left = (vr > 0 ? vr / g : 0) + sqrtf(2 * (h + (vr > 0 ? vr * vr / (2 * g) : 0)) / g);
        if (land < 1e8f) left = land;
        if (left < 40 && h >= 400) left = 40; /* h is to the shadow, on the ramp's far face early on: a ramp gives 40-50 more */
        uint32_t p = (r >> 8) % 100;
        t->cp_t = 0, t->cp_stop = 0, t->cp_catch = (r >> 16) % 100 < 55;
        int dir = (int)((r >> 20) % 12);
        t->cp_dir = dir < 8 ? DIRS[dir & 3] : DIRS[4 + (dir - 8)];
        bool diag = dir >= 8;
        float need1 = (diag ? 34.0f : 24.0f) + SPIN_RAMP + 4;
        if (left < 28) t->cp_plan = 6; /* too short: nothing */
        else if (g_trick_comp && t->meter >= METER_FULL && left >= 38) t->cp_plan = 5, t->cp_dir = dir < 4 ? DIRS[dir] : 0; /* a signature, any of the five */
        else if (p < 55 && left >= need1) {
            t->cp_plan = 1, t->cp_goal = 6.2831853f - 0.3f; /* one turn; another if the landing is far enough then */
        } else if (p < 75 && left >= 38) t->cp_plan = 2, t->cp_dir = DIRS[dir & 3];
        else if (p < 84 || left < 38) t->cp_plan = 3;
        else if (p < 92) t->cp_plan = (r >> 28) & 1 ? 9 : 4, t->cp_dir = DIRS[dir & 3]; /* 9: JUMP held, the direction twice */
        else t->cp_plan = (r >> 26) & 1 ? 10 : (r >> 24) & 1 ? 8 : 7, t->cp_dir = DIRS[dir & 3]; /* a ZR trick (8: the same direction twice; 10: ZR alone, RODEO / CORKSCREW in turn) */
        rt_log("trick: kart %u plans %d (%.0f frames left, gravity %.2f, rising %.1f, %.0f high)", kart, t->cp_plan, left, g, vr, h);
    }
    int f = ++t->cp_t;
    uint32_t bits = 0;
    switch (t->cp_plan) {
    case 1: { /* hold until every axis it turns has its turns */
        bool fl = t->cp_dir & (TRICK_UP | TRICK_DOWN), sp = t->cp_dir & (TRICK_LEFT | TRICK_RIGHT);
        bool done = (!fl || fabsf(t->acc[0]) >= t->cp_goal) && (!sp || fabsf(t->acc[1]) >= t->cp_goal);
        if (done && !t->cp_stop && t->cp_goal < 12 && land < 1e8f && land > (fl && sp ? 38 : 28) && (r >> 12) % 100 < 60) t->cp_goal += 6.2831853f, done = false;
        if (done) t->cp_stop = 1;
        if (!t->cp_stop) bits = (uint32_t)t->cp_dir;
        break;
    }
    case 2: bits = f <= 3 || (f >= 7 && f <= 10) ? (uint32_t)t->cp_dir : 0; break;
    case 3: bits = f <= 3 ? TRICK_JUMP : 0; break;
    case 4: bits = (f <= 4 ? TRICK_JUMP : 0) | (f >= 3 && f <= 6 ? (uint32_t)t->cp_dir : 0); break;
    case 5: bits = f <= 4 ? TRICK_SIG | (uint32_t)t->cp_dir : 0; break;
    case 7: bits = (f <= 6 ? TRICK_R : 0) | (f >= 3 && f <= 5 ? (uint32_t)t->cp_dir : 0); break;
    case 8: bits = (f <= 12 ? TRICK_R : 0) | ((f >= 3 && f <= 5) || (f >= 8 && f <= 10) ? (uint32_t)t->cp_dir : 0); break;
    case 10: bits = f <= 3 ? TRICK_R : 0; break;
    case 9: bits = (f <= 12 ? TRICK_JUMP : 0) | ((f >= 3 && f <= 5) || (f >= 8 && f <= 10) ? (uint32_t)t->cp_dir : 0); break;
    }
    if (t->cp_catch && land < 9 && land > 0) bits |= TRICK_R;
    return bits;
}
/* the rider's next trick k: in place of one just queued or started, else after the one running */
static void rd_pick(Trick *t, int k) {
    if (t->rnext) t->rnext = k;
    else if (t->rstate == TK_QUEUED || (t->rstate == TK_SPIN && t->rframe <= POSE_IN)) t->rkind = k;
    else t->rnext = k;
    if (t->rstate == TK_IDLE || t->rstate == TK_SETTLE) t->rkind = k, t->rnext = 0, t->rstate = TK_QUEUED;
}
/* the rider's trick base, if it is the next, queued or just started, becomes big (the same direction again) */
static void rd_up(Trick *t, int base, int big, int frames) {
    if (t->rnext == base) t->rnext = big;
    else if (t->rkind == base && t->rstate == TK_QUEUED) t->rkind = big;
    else if (t->rkind == base && t->rstate == TK_SPIN) t->rkind = big, t->rframe = 0, t->rlen = frames;
}
/* the back rider's turn this frame (s_rd_t, in its model's own frame) round its head (up, fwd): pitch round x
 * (across; x is left), yaw round y (up; + faces it left), roll round z (forward), applied pitch, yaw, then roll */
static void tk_rider_mtx(const Trick *t, float up, float fwd) {
    float h = t->rh, ax = 0, ay = 0, az = 0, p[3] = {0, 0, fwd}, tr[3] = {0, 0, 0};
    float f = t->rlen > 0 ? (float)t->rframe / (float)t->rlen : 0, e = f < 0 ? 0 : f > 1 ? 1 : f;
    e = e * e * (3 - 2 * e); /* eased through the trick: a whole turn */
    switch (t->rkind) {
    case RD_SUPERMAN: ax = 1.4f * h, p[1] = up * 0.75f, tr[1] = 30 * h; break; /* laid out flat behind, face down */
    case RD_CLIFF: ax = -0.6f * h, tr[1] = 75 * h; break;                      /* stood up on the kart, leaning back */
    case RD_SIDEW: az = 1.1f * h, tr[0] = -38 * h, tr[1] = 22 * h; break;      /* hung out over the right side */
    case RD_RSIDEW: az = -1.1f * h, tr[0] = 38 * h, tr[1] = 22 * h; break;     /* ... the left */
    case RD_HANDSTAND: ax = 2.7f * h, p[1] = up * 0.5f, tr[1] = up * 0.85f * h; break; /* upside down above the seat */
    case RD_LAZY: ax = -1.25f * h, tr[1] = 20 * h, tr[2] = -10 * h; break;     /* lying back */
    /* ZR + a direction */
    case RD_KISS: ax = 2.95f * h, p[1] = up * 0.5f, tr[1] = up * 1.05f * h, tr[2] = 45 * h; break; /* vertical, feet up, out over the front */
    case RD_CORDOVA: ax = -2.3f * h, tr[1] = 12 * h, tr[2] = -55 * h; break;   /* tipped back past flat, head hanging behind */
    case RD_NACNAC: ay = 1.57f * h, ax = -0.3f * h, tr[0] = 32 * h, tr[1] = 30 * h; break; /* swung round to face left, out over that side */
    case RD_CANCAN: ay = -1.57f * h, az = 0.5f * h, tr[0] = -34 * h, tr[1] = 12 * h; break; /* turned to face right, leaning back out */
    case RD_VARIAL: ay = 6.2831853f * e, tr[1] = 40 * h; break;               /* a whole turn round, lifted clear of the seat */
    case RD_ROCKSOLID: ax = -0.25f * h, tr[1] = up * 1.3f * h, tr[2] = 15 * h; break; /* off the kart, floating high above it */
    case RD_FLAIR: ax = -6.2831853f * e, ay = 6.2831853f * e, p[1] = up * 0.5f, tr[1] = up * 0.7f * h; break; /* a backflip and a whole turn at once, up off the seat */
    case RD_SWITCH: tr[1] = 30 * h; break;                                      /* a hop while the game swaps the two over (tk_step) */
    /* JUMP + a direction twice */
    case RD_HELI: ax = 2.7f * h, ay = 12.566371f * e, p[1] = up * 0.5f, tr[1] = up * 0.85f * h; break; /* the handstand, spun round twice */
    case RD_SOMER: ax = -6.2831853f * e, p[1] = up * 0.5f, tr[1] = up * 0.6f * h, tr[2] = -10 * h; break; /* a tucked backward somersault above the seat */
    case RD_CART: az = -6.2831853f * e, p[1] = up * 0.5f, tr[0] = 30 * h, tr[1] = up * 0.6f * h; break; /* a whole sideways turn out over the left */
    case RD_SURF: ay = -1.57f * h, az = 0.35f * h, tr[0] = -20 * h, tr[1] = 75 * h; break; /* stood up sideways on the kart, leaning out right */
    /* ZR alone, and again */
    case RD_RODEO: az = 0.5f * h * sinf(12.566371f * e), ax = -0.3f * h, tr[1] = 60 * h; break; /* stood up, rocking side to side twice */
    case RD_CORK: ay = 6.2831853f * e, az = 6.2831853f * e, p[1] = up * 0.5f, tr[1] = up * 0.9f * h; break; /* a roll and a whole turn at once, high off the seat */
    }
    float cx = cosf(ax), sx = sinf(ax), cy = cosf(ay), sy = sinf(ay), cz = cosf(az), sz = sinf(az);
    float rx[9] = {1, 0, 0, 0, cx, -sx, 0, sx, cx}, ry[9] = {cy, 0, sy, 0, 1, 0, -sy, 0, cy}, rz[9] = {cz, -sz, 0, sz, cz, 0, 0, 0, 1};
    float m[9], r[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) m[3 * i + j] = ry[3 * i] * rx[j] + ry[3 * i + 1] * rx[3 + j] + ry[3 * i + 2] * rx[6 + j];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r[3 * i + j] = rz[3 * i] * m[j] + rz[3 * i + 1] * m[3 + j] + rz[3 * i + 2] * m[6 + j];
    for (int i = 0; i < 3; i++) {
        s_rd_t[4 * i] = r[3 * i], s_rd_t[4 * i + 1] = r[3 * i + 1], s_rd_t[4 * i + 2] = r[3 * i + 2];
        s_rd_t[4 * i + 3] = p[i] - (r[3 * i] * p[0] + r[3 * i + 1] * p[1] + r[3 * i + 2] * p[2]) + tr[i];
    }
}
/* a computer, or MKDD_TRICK_AUTO=1 the player's kart too (tests) */
static bool tk_auto(void) {
    static int a = -1;
    if (a < 0) a = getenv("MKDD_TRICK_AUTO") && atoi(getenv("MKDD_TRICK_AUTO")) == 1;
    return a == 1;
}
/* a signature's star: KartStar::MakeStar (0x802C79D0) after the karts' frame (i_FUN_8029fd40, where the
 * drafting's boost is made too), its timer then cut to SIG_STAR. Made from MakeKartDisp it broke
 * MakeTireDispMatrix's pointers that frame (a crash). */
static uint8_t s_tk_star[8];
void tk_star_frame(Ctx *c, uint8_t *M, uint32_t ctrl) {
    for (uint32_t k = 0; k < 8; k++) {
        if (!s_tk_star[k]) continue;
        s_tk_star[k] = 0;
        uint32_t body = ram_ptr(ctrl) ? ld32(c, M, ctrl + 0xa0 + 4 * k) : 0;
        if (!ram_ptr(body) || body != g_tk[k].body || mld(c, M, body + 0x5b3, 1) != k) continue;
        uint32_t star = ld32(c, M, body + 0xd4);
        if (!ram_ptr(star) || ld32(c, M, 0x802C79D0u) != 0x9421FFF0u || ld32(c, M, 0x802C79DCu) != 0x3C000004u) continue; /* another main.dol */
        const uint32_t a[1] = {star};
        ctx_call_guest(c, 0x802C79D0u, a, 1);
        mst(c, M, star + 6, 2, SIG_STAR); /* its timer (DoStarCrl counts it down) */
        rt_log("trick: kart %u star (%s)", k, ld32(c, M, body + 0x574) & 0x40000 ? "on" : "NOT on");
    }
}
/* the battle's length, frames: TRICK_COMP_FRAMES (rt.h), MKDD_TRICK_SECS=<n> n seconds (tests) */
uint32_t tk_len(void) {
    static uint32_t n;
    if (!n) {
        const char *e = getenv("MKDD_TRICK_SECS");
        int s = e ? atoi(e) : 0;
        n = s >= 5 && s <= 600 ? (uint32_t)s * 60u : TRICK_COMP_FRAMES;
    }
    return n;
}
/* MKDD_TRICK_METER=1: every meter starts full (tests) */
static bool tk_meter_test(void) {
    static int a = -1;
    if (a < 0) a = getenv("MKDD_TRICK_METER") && atoi(getenv("MKDD_TRICK_METER")) == 1;
    return a == 1;
}
/* touchdown: the grade, the boost or the price, the jump's points into the combo */
static void tk_land(Ctx *c, uint8_t *M, uint32_t body, Trick *t, uint32_t kart, uint32_t pad, bool wipes) {
    if (t->ck && t->cf >= t->cl * LAND_COUNT && !t->sig) tk_add(t, K_TRICK[t->ck], 0, kart);
    bool rmid = t->rstate == TK_SPIN && t->rframe < t->rlen * LAND_COUNT;
    if (t->rstate == TK_SPIN && !rmid) tk_add(t, RD_TRICK[t->rkind], 0, kart);
    float ang = q_deg(t->qc), upy = 1 - 2 * (t->qc[1] * t->qc[1] + t->qc[3] * t->qc[3]);
    bool tried = t->jn || (t->upjump && (ang > 3 || t->ck || t->rstate == TK_SPIN)); /* not a bounce after a landing */
    t->ck = 0;
    if (t->rstate == TK_SPIN) t->rstate = TK_SETTLE;
    else if (t->rstate == TK_QUEUED) t->rstate = TK_IDLE;
    if (!tried) return;
    int g = t->sig ? G_PERFECT : ang <= LAND_PERFECT && upy > 0 ? G_PERFECT : ang <= LAND_SKETCHY && upy > 0 ? G_SKETCHY : G_BAIL;
    if (g == G_PERFECT && rmid) g = G_SKETCHY;
    bool caught = t->lr_since <= CATCH_IN && (pad & (TRICK_L | TRICK_R));
    if (g == G_SKETCHY && caught) g = G_CATCH;
    float jp = 0;
    int turns = 0;
    uint32_t ids = 0;
    for (int i = 0; i < t->jn; i++) {
        int id = t->jt[i], n = t->jc[i], seen = 0;
        for (int k = 0; k < t->histn; k++) seen += t->hist[k] == id;
        float p = (float)T_PTS[id] * (float)(n * (n + 1) / 2);
        for (int k = 0; k < seen; k++) p *= STALE_KEEP;
        jp += p, turns += n, ids |= 1u << id;
        if (t->histn == STALE_N) memmove(t->hist, t->hist + 1, STALE_N - 1), t->histn--;
        t->hist[t->histn++] = (uint8_t)id;
    }
    if (t->jn) jp += (t->peak < 4000.0f ? t->peak : 4000.0f) * 0.2f; /* height bonus, at most 800 */
    static const char *const GN[5] = {"", "perfect", "sketchy", "bail", "caught"};
    rt_log("trick: kart %u lands %s (%.0f degrees%s%s), %d tricks, %.0f points", kart, GN[g], ang, upy > 0 ? "" : ", upside down",
           caught ? ", L/R" : "", t->jn, jp);
    t->grade = g, t->grade_t = 70;
    if (g == G_BAIL) {
        t->combo = 0, t->cids = 0, t->window = 0;
        if (g_trick_comp) t->meter = 0;
        if (wipes) s_tk_wipe = true;
    } else {
        int n = t->sigdone ? SIG_BOOST : TRICK_BOOST + TRICK_BOOST_TURN * (turns - 1);
        if (n > TRICK_BOOST_MAX && !t->sigdone) n = TRICK_BOOST_MAX;
        if ((g == G_PERFECT || g == G_CATCH) && (turns > 0 || t->sigdone)) {
            uint32_t fl = (uint32_t)mld(c, M, body + 0x570, 4), left = (uint32_t)mld(c, M, body + 0x59e, 2);
            mst(c, M, body + 0x570, 4, fl | 0x200);
            if (left < (uint32_t)n) mst(c, M, body + 0x59e, 2, (uint32_t)n);
            rt_log("trick: kart %u boost %d frames", kart, n);
        }
        if (g == G_PERFECT) t->sq = 1;
        if (g == G_SKETCHY && wipes) {
            float s = ldf(c, M, body + 0x444) * SKETCHY_SPEED;
            if (isfinite(s)) stfn(c, M, body + 0x444, &s, 1);
        }
        t->combo += jp, t->cids |= ids;
        if (g_trick_comp && !g_trick_ended && jp > 0.5f) {
            g_trick_score[kart] += (uint32_t)(jp + 0.5f); /* the landing's points now; the combo bonus when it ends */
            t->gain = (uint32_t)(jp + 0.5f), t->gain_t = 70;
        }
        if (g_trick_comp) t->meter = t->meter + (uint32_t)(jp / METER_SHARE) > METER_FULL ? METER_FULL : t->meter + (uint32_t)(jp / METER_SHARE);
        if (g == G_SKETCHY) tk_bank(t, kart, false);
        else if (t->combo > 0.5f) t->window = COMBO_WINDOW;
    }
    t->sig = t->sigdone = 0;
}
/* this frame's trick for the kart: false when its kart is drawn as the game has it (s_rd_on: its rider not) */
static bool tk_step(Ctx *c, uint8_t *M, uint32_t body) {
    uint32_t kart = mld(c, M, body + 0x5b3, 1);
    uint32_t st = ld32(c, M, body + 0x578);
    /* st bit 2: a player's kart, bit 8: a CPU's (the competition lets CPUs trick) */
    if (kart > 7 || !(g_tricks || g_trick_comp) || g_lan_play || !(st & (g_trick_comp ? 10 : 2)) || (st & 0x90)) return false;
    Trick *t = &g_tk[kart];
    if (g_trick_comp && kart == 0) {
        /* the clock starts on GO: the kart's start state (KartBody +0xc4, byte 0) is 2 in the countdown, 3 or
         * more from GO, and already 3 or more in the course intro before it: GO only after a countdown */
        static bool went, counted;
        if (t->body != body || g_trick_newrace) { /* a new race */
            g_trick_newrace = 0, g_trick_frames = 0, went = counted = false, g_trick_ended = 0, g_trick_live = 0, g_trick_res = 0;
            memset((void *)g_trick_score, 0, sizeof g_trick_score);
            /* which arena: each trick arena keeps its battle stage's music id (mods/trick, tools/build5.py):
             * the stage page's Cookie Land, Block City, Nintendo GameCube, Pipe Plaza, then the two locked ones */
            static const uint8_t ARENA[6] = {0x3a, 0x36, 0x35, 0x38, 0x34, 0x3b};
            g_trick_track = 0;
            for (uint32_t i = 0; i < 6; i++)
                if (g_trick_music == ARENA[i]) g_trick_track = i + 1;
            if (!g_trick_cup) memset((void *)g_trick_total, 0, sizeof g_trick_total); /* each battle stands alone, a cup's rounds add up */
            for (int i = 0; i < 8; i++) g_tk[i].combo = 0, g_tk[i].cids = 0, g_tk[i].window = 0, g_tk[i].meter = 0, g_tk[i].histn = 0;
            rt_log("trick comp: arena %u (music %#x)", g_trick_track, g_trick_music);
        }
        uint32_t sp = ld32(c, M, body + 0xc4), start = ram_ptr(sp) ? mld(c, M, sp, 1) : 0;
        if (start == 2) {
            if (went) { /* the countdown again without a load (RETRY): a new battle */
                went = false, g_trick_frames = 0, g_trick_ended = 0, g_trick_res = 0;
                memset((void *)g_trick_score, 0, sizeof g_trick_score);
                if (!g_trick_cup) memset((void *)g_trick_total, 0, sizeof g_trick_total);
                for (int i = 0; i < 8; i++) g_tk[i].combo = 0, g_tk[i].cids = 0, g_tk[i].window = 0, g_tk[i].meter = 0, g_tk[i].histn = 0;
                rt_log("trick comp: retry on arena %u", g_trick_track);
            }
            if (!counted) rt_log("trick comp: the countdown on arena %u, kart 0 at (%.0f %.0f) facing (%.2f %.2f)", g_trick_track, ldf(c, M, body + 0x11c), ldf(c, M, body + 0x13c), ldf(c, M, body + 0x118), ldf(c, M, body + 0x138)); /* forward: the pose's z column */
            if (!counted && tk_meter_test())
                for (int i = 0; i < 8; i++) g_tk[i].meter = METER_FULL;
            counted = true, g_trick_live = 1; /* the HUD from here (native.c trick_hud) */
        }
        if (start >= 3 && counted) went = true;
        if (went && g_trick_frames < tk_len()) g_trick_frames++;
        if (went && !g_trick_ended && g_trick_frames >= tk_len()) { /* time up: the combos bank, the points are banked and the race ends */
            for (uint32_t i = 0; i < 8; i++)
                if (g_tk[i].body) tk_bank(&g_tk[i], i, true);
            g_trick_ended = 1;
            for (int i = 0; i < 8; i++) g_trick_total[i] += g_trick_score[i];
            s_tk_force = ld32(c, M, 0x803B1464u) < 4; /* a race ends on a forced goal; a trick battle (gRaceInfo +8 mode 4) by its ranking (battle.c bai_tick) */
            rt_log("trick comp: time up on arena %u, you %u cpu %u %u %u", g_trick_track, g_trick_total[0], g_trick_total[1], g_trick_total[2], g_trick_total[3]);
        }
    }
    if (t->body != body) {
        memset(t, 0, sizeof *t), t->body = body, q_id(t->qc), t->lr_since = 999, t->jump_since = 999, t->since_land = 999;
        if (g_trick_comp && tk_meter_test()) t->meter = METER_FULL;
        for (int i = 0; i < 4; i++) t->tap_since[i] = 999;
    }
    bool ground = ld32(c, M, body + 0x5a4) != 0;
    uint32_t shadow = ld32(c, M, body + 0x28);
    float h = ram_ptr(shadow) ? ldf(c, M, body + 0x12c) - ldf(c, M, shadow + 0xa4) : 0;
    if (!isfinite(h) || h > TRICK_H_MAX) h = 0; /* no ground under the shadow (against a wall, off the floor): not a height */
    static int test = -1; /* MKDD_TRICK_TEST=1: tricks on the ground too (screenshots of the spin) */
    if (test < 0) test = getenv("MKDD_TRICK_TEST") && atoi(getenv("MKDD_TRICK_TEST")) == 1;
    if (test) ground = false;
    /* only while the race runs (RaceDirector PHASE_RUNNING) from GO (the kart's start state 3 or more): not in
     * the course intro, the countdown or a drop onto the course */
    uint32_t gp = ld32(c, M, body + 0xc4), go = ram_ptr(gp) ? mld(c, M, gp, 1) : 0;
    /* a fall: Lakitu's rescue (KartBody +0xb4 KartRescue, +0x74 flags 0x20, on and off while it carries the kart) until
     * it touches the ground again. The carry and the drop at the respawn counted as a jump: tricks and a landing's
     * points for falling off (or a bail's spin-out at the respawn). A fall ends the combo, as a bail */
    uint32_t rs = ld32(c, M, body + 0xb4);
    if (ram_ptr(rs) && (mld(c, M, rs + 0x74, 1) & 0x20)) {
        if (!t->resc) t->combo = 0, t->cids = 0, t->window = 0;
        t->resc = 1;
    } else if (t->resc && ld32(c, M, body + 0x5a4)) {
        t->resc = 0;
        rt_log("trick: kart %u back on the ground after Lakitu", kart);
    }
    if (t->resc && g_race_phase == 0x11) fx_kart(c, M, body, kart, ground); /* (its pushes stopped there) */
    if (g_race_phase != 0x11 || go < 3 || t->resc) { /* (tests too: a computer's star while the karts load crashed) */
        t->air = 0, t->upjump = 0, t->jn = 0, t->ck = 0, t->sig = t->sigdone = 0, t->rstate = TK_IDLE, t->rh = 0, t->mask = 0;
        t->w[0] = t->w[1] = t->acc[0] = t->acc[1] = 0, q_id(t->qc), t->pad_was = 0;
        return false;
    }
    if (fx_kart(c, M, body, kart, ground)) t->since_land = 999; /* a spring pad, geyser or tornado threw it up: not a landing's bounce
                                                                * (the peak stays: it counts for the height bonus) */
    if (!ground && h > t->peak) t->peak = h;
    static int frames;
    if (!frames) { /* MKDD_TRICK_FRAMES: slower specials for screenshots (tests) */
        const char *e = getenv("MKDD_TRICK_FRAMES");
        frames = e && atoi(e) >= 10 && atoi(e) <= 1000 ? atoi(e) : TRICK_FRAMES;
    }
    /* frames to the landing (h = fall n + grav n^2 / 2), fall and its gain from the kart's own height, not from
     * h: the ground below a long flight slopes, which made h drop several times faster; 1e9 while it can't be told */
    float y = ldf(c, M, body + 0x12c), v = t->air > 1 ? t->hprev - y : 0, land = 1e9f;
    if (!isfinite(v) || fabsf(v - t->fall) >= 30) v = t->fall; /* a reset or teleport, not a fall */
    if (t->air > 2) {
        float a = t->grav * 0.7f + (v - t->fall) * 0.3f;
        t->grav = a < 0 ? 0 : a > 10 ? 10 : a;
    } else t->grav = 0;
    t->fall = t->air > 1 ? v : 0;
    t->hprev = isfinite(y) ? y : 0;
    /* still rising (fall < 0): no landing guess yet. Off a ramp's top the shadow sits on the jump's far face,
     * far above the ground the kart will land on, so h (and the guess) came out several times too small */
    if (!ground && !test && t->air > 4 && t->fall >= -1.0f) {
        float a = t->grav, f = t->fall;
        if (a > 0.01f) land = (sqrtf(f * f + 2 * a * (h > 0 ? h : 0)) - f) / a;
        else if (f > 0.5f) land = h / f;
    }
    if (!ground && !t->air) t->y0 = isfinite(y) ? y : 0, t->x0 = ldf(c, M, body + 0x11c), t->z0 = ldf(c, M, body + 0x13c), t->v0 = ldf(c, M, body + 0x444);
    float rise = !ground && isfinite(y) ? y - t->y0 : 0;
    if (!isfinite(rise) || fabsf(rise) > 1e5f) rise = 0;
    bool up = (t->air >= TRICK_LONG_AIR && h >= TRICK_LATE_H) || (t->air >= TRICK_MIN_AIR && (h >= TRICK_MIN_H || rise >= TRICK_MIN_RISE)) || test; /* a ramp or a drop, not a bump */
    if (rise > t->rmax) t->rmax = rise;
    if (up && !ground && !t->upjump) t->upjump = 1, t->air_up = t->air;
    bool human;
    uint32_t pad = tk_pad(c, M, kart, &human);
    if (g_trick_comp && (!human || (kart == 0 && tk_auto()))) pad = tk_cpu(t, kart, ground, up || t->upjump, h > rise ? h : rise, land);
    uint32_t press = pad & ~t->pad_was, rel = t->pad_was & ~pad;
    t->pad_was = pad;
    bool wipes = g_tricks == 2 || g_trick_comp; /* bails spin out, sketchy landings cost speed */
    if (press & (TRICK_L | TRICK_R)) t->lr_since = 0;
    else if (t->lr_since < 999) t->lr_since++;
    if (t->pop_t > 0) t->pop_t--;
    if (t->grade_t > 0) t->grade_t--;
    if (t->bank_t > 0) t->bank_t--;
    if (t->gain_t > 0) t->gain_t--;
    s_tk_wipe = false;
    if (ground) {
        if (press & (15u | TRICK_JUMP | TRICK_R)) t->gbuf |= press & (15u | TRICK_JUMP | TRICK_R), t->gbuf_age = 0;
        else if (++t->gbuf_age > PRE_BUF) t->gbuf = 0, t->gbuf_age = PRE_BUF;
    } else if (!t->air && t->gbuf) { /* just took off: what was pressed on the lip (a direction still held) */
        press |= (t->gbuf & (TRICK_JUMP | TRICK_R)) | (t->gbuf & pad & 15u);
        t->gbuf = 0;
    }
    if (t->since_land < 999) t->since_land++;
    if (!ground && t->since_land <= LAND_HOP_IN) { /* the landing's bounce */
        float vy = ldf(c, M, body + 0x264);
        if (isfinite(vy) && vy > 0) {
            if (kart == 0 && vy > 3) rt_log("trick: kart 0 bounced %d frames after the landing, up at %.1f, damped", t->since_land, vy);
            vy *= LAND_HOP_KEEP, stfn(c, M, body + 0x264, &vy, 1);
        }
    }
    if (!ground) {
        t->air++;
        if (press) t->presses++;
        uint32_t mask_was = t->mask;
        t->mask &= pad; /* let go: turns again */
        if (land < SPIN_LATE) t->mask |= press & 15u;
        /* The two rider buttons work alike. JUMP, or ZR (not with JUMP: the signature; not just before touchdown: the
         * catch), pressed in the air or on the lip starts its own trick at once, each press the other of its two
         * (SUPERMAN / CLIFFHANGER, RODEO / CORKSCREW). A direction within POSE_IN after it, or pressed up to DIR_EARLY
         * before it and still held (the kart's turn since then undone), makes that button's direction trick in its
         * place; the same direction again soon after, the button held: the bigger one. */
        static const int DIR[4] = {TRICK_UP, TRICK_DOWN, TRICK_LEFT, TRICK_RIGHT};
        static const int ZRK[4] = {RD_KISS, RD_CORDOVA, RD_NACNAC, RD_CANCAN};
        static const int POSE[4] = {RD_HANDSTAND, RD_LAZY, RD_RSIDEW, RD_SIDEW};
        bool zrok = !(pad & (TRICK_JUMP | TRICK_SIG)), zr = (pad & TRICK_R) && zrok;
        bool jp = (press & TRICK_JUMP) != 0, rp = (press & TRICK_R) && zrok && !(land < CATCH_IN);
        uint32_t late = 0;
        if (jp || rp) {
            int k = jp ? (t->rcount++ & 1 ? RD_CLIFF : RD_SUPERMAN) : (t->zr_count++ & 1 ? RD_CORK : RD_RODEO);
            if (t->rstate == TK_SPIN) t->rnext = k;
            else t->rkind = k, t->rstate = TK_QUEUED;
            if (jp) t->jump_since = 0;
            else t->zr_win = POSE_IN + 1, rt_log("trick: kart %u ZR: %s", kart, T_NAME[RD_TRICK[k]]);
            for (int i = 0; i < 4 && !late; i++)
                if ((pad & (uint32_t)DIR[i]) && !(t->mask & (uint32_t)DIR[i]) && t->tap_down[i] <= DIR_EARLY) late = (uint32_t)DIR[i];
        }
        if (!jp && t->jump_since < 999) t->jump_since++;
        if (!rp && t->zr_win > 0) t->zr_win--;
        if (t->zr_up > 0) t->zr_up--;
        if (t->zl_up > 0) t->zl_up--;
        static const int SPECIAL[4] = {K_TWIST, K_BTWIST, K_WHIP, K_ROLL};
        for (int i = 0; i < 4; i++) {
            uint32_t b = (uint32_t)DIR[i];
            if (late & b) { /* pressed just before the button: its trick; the kart's turn since then undone */
                memcpy(t->qc, t->tap_q[i], sizeof t->qc), t->acc[0] = t->tap_acc[i][0], t->acc[1] = t->tap_acc[i][1];
                t->w[0] = t->w[1] = 0;
                rd_pick(t, rp ? ZRK[i] : POSE[i]);
                t->mask |= b;
                if (rp) t->zr_win = 0, t->zr_last = i, t->zr_up = DTAP_IN;
                else t->jump_since = 999, t->zl_last = i, t->zl_up = DTAP_IN;
                rt_log("trick: kart %u %s (direction first): %s", kart, rp ? "ZR" : "JUMP", T_NAME[RD_TRICK[rp ? ZRK[i] : POSE[i]]]);
            } else if (press & b & ~t->mask) {
                if (zr && t->zr_up > 0 && t->zr_last == i) { /* again: FLAIR, SWITCHEROO, BODY VARIAL, ROCK SOLID */
                    static const int BIG[4] = {RD_FLAIR, RD_SWITCH, RD_VARIAL, RD_ROCKSOLID};
                    rd_up(t, ZRK[i], BIG[i], frames);
                    t->mask |= b, t->zr_up = 0;
                    rt_log("trick: kart %u ZR again: %s", kart, T_NAME[RD_TRICK[BIG[i]]]);
                    continue;
                }
                if ((pad & TRICK_JUMP) && t->zl_up > 0 && t->zl_last == i) { /* the pose's direction again, JUMP held */
                    static const int ZLBIG[4] = {RD_HELI, RD_SOMER, RD_CART, RD_SURF};
                    rd_up(t, POSE[i], ZLBIG[i], frames);
                    t->mask |= b, t->zl_up = 0;
                    rt_log("trick: kart %u JUMP again: %s", kart, T_NAME[RD_TRICK[ZLBIG[i]]]);
                    continue;
                }
                if (zrok && t->zr_win > 0) { /* ZR then a direction */
                    rd_pick(t, ZRK[i]);
                    t->mask |= b, t->zr_win = 0, t->zr_last = i, t->zr_up = DTAP_IN;
                    rt_log("trick: kart %u ZR: %s", kart, T_NAME[RD_TRICK[ZRK[i]]]);
                    continue;
                }
                if (t->jump_since <= POSE_IN) { /* a direction just after JUMP: the rider's pose */
                    rd_pick(t, POSE[i]);
                    t->mask |= b, t->jump_since = 999, t->zl_last = i, t->zl_up = DTAP_IN;
                } else if ((up || t->upjump) && !t->ck && t->tap_since[i] <= DTAP_IN) { /* tapped, then again: the special */
                    t->acc[0] = t->tap_acc[i][0], t->acc[1] = t->tap_acc[i][1];
                    tk_can(t, SPECIAL[i], t->tap_q[i], frames, kart);
                    t->mask |= pad & 15u, t->tap_since[i] = 999;
                } else if (!(up || t->upjump) && !t->ck && t->tap_since[i] <= DTAP_IN) { /* ... before tricks start: when they do */
                    t->pend = i + 1, t->mask |= b, t->tap_since[i] = 999;
                } else memcpy(t->tap_q[i], t->qc, sizeof t->qc), t->tap_acc[i][0] = t->acc[0], t->tap_acc[i][1] = t->acc[1], t->tap_down[i] = 0;
            }
            if (pad & b) t->tap_down[i]++;
            else if (rel & b) t->tap_since[i] = t->tap_down[i] <= DTAP_TAP && !(mask_was & b) ? 0 : 999;
            else if (t->tap_since[i] < 999) t->tap_since[i]++;
        }
        /* ZL + ZR with the meter full: the signature, a direction held picks which */
        if (g_trick_comp && (up || t->upjump) && !t->sig && t->meter >= METER_FULL && (press & TRICK_SIG)) {
            float id[4];
            q_id(id);
            tk_can(t, pad & TRICK_UP ? K_METEOR : pad & TRICK_DOWN ? K_TEMPEST : pad & TRICK_LEFT ? K_CYCLONE : pad & TRICK_RIGHT ? K_TYPHOON : K_HURRICANE,
                   id, frames, kart);
            t->sig = t->sigdone = 1, t->meter = 0, t->mask |= pad & 15u;
            tk_add(t, K_TRICK[t->ck], 0, kart);
            if (t->rstate != TK_SPIN) t->rkind = RD_SUPERMAN, t->rstate = TK_QUEUED;
            s_tk_star[kart] = 1; /* the star after the karts' frame (tk_star_frame) */
        }
        if (t->pend && (up || t->upjump) && !t->ck) {
            tk_can(t, SPECIAL[t->pend - 1], t->qc, frames, kart);
            t->mask |= pad & 15u, t->pend = 0;
        }
        if (t->rstate == TK_QUEUED && (up || t->upjump)) {
            t->rstate = TK_SPIN, t->rframe = 0, t->rlen = frames, t->rd = 0;
            rt_log("trick: kart %u rider %s", kart, T_NAME[RD_TRICK[t->rkind]]);
        }
        /* the SWITCHEROO: the game's own driver swap (KartGame::DoChange 0x802AAF58 swaps when its Z buffer, KartGame
         * +0x0b, is set and no swap is running; KartBody +0xf0 the KartGame), once a trick; CPUs too */
        if (t->rstate == TK_SPIN && t->rkind == RD_SWITCH) {
            uint32_t kg = ld32(c, M, body + 0xf0);
            if (!t->rswap && ram_ptr(kg)) mst(c, M, kg + 0x0b, 1, 20), t->rswap = 1, rt_log("trick: kart %u swaps drivers", kart);
        } else t->rswap = 0;
        /* the kart: a canned trick, else held directions */
        if (t->ck) {
            float q[4], b[4];
            q_scale(t->cblend, TRICK_BLEND);
            tk_quat(q, t->ck, (float)++t->cf / (float)t->cl);
            q_mul(b, t->cbase, q);
            q_mul(t->qc, t->cblend, b);
            if (t->cf >= t->cl) {
                if (!t->sig) tk_add(t, K_TRICK[t->ck], 0, kart);
                q_mul(t->qc, t->cblend, t->cbase);
                t->ck = 0, t->sig = 0;
            }
        } else if (up || t->upjump) {
            uint32_t dir = pad & 15u & ~t->mask;
            float tx = (dir & TRICK_UP ? 1.0f : 0) - (dir & TRICK_DOWN ? 1.0f : 0), ty = (dir & TRICK_LEFT ? 1.0f : 0) - (dir & TRICK_RIGHT ? 1.0f : 0);
            float n = sqrtf(tx * tx + ty * ty), tw[2] = {n > 0 ? tx / n * SPIN_RATE : 0, n > 0 ? ty / n * SPIN_RATE : 0};
            for (int k = 0; k < 2; k++) {
                if (n > 0) {
                    float d = tw[k] - t->w[k], s = SPIN_RATE / SPIN_RAMP;
                    t->w[k] += d > s ? s : d < -s ? -s : d;
                } else t->w[k] *= SPIN_STOP;
                if (fabsf(t->w[k]) < 1e-4f) t->w[k] = 0;
            }
            if (t->w[0] || t->w[1]) {
                float a[4], b[4], q[4];
                q_axis(a, 0, t->w[0]), q_axis(b, 1, t->w[1]), q_mul(q, a, b);
                q_mul(a, t->qc, q); /* in the drawn kart's own frame */
                memcpy(t->qc, a, sizeof a), q_norm(t->qc);
                for (int k = 0; k < 2; k++) {
                    t->acc[k] += t->w[k];
                    int s = t->acc[k] < 0, m = (int)((fabsf(t->acc[k]) + TURN_SLACK) / 6.2831853f);
                    if (m > t->nturn[k][s]) t->nturn[k][s] = m, tk_add(t, k ? T_SPIN : s ? T_BACK : T_FRONT, m, kart);
                }
            } else if (!dir) { /* close, or about to land: rights itself */
                float d = q_deg(t->qc);
                if (land < ASSIST_LAND && d < ASSIST_LAND_MAX) q_scale(t->qc, ASSIST_LAND_KEEP);
                else if (d < ASSIST_DEG) q_scale(t->qc, ASSIST_KEEP);
            }
        }
    } else {
        if (t->air) {
            if (t->air >= 10) {
                float dx = ldf(c, M, body + 0x11c) - t->x0, dz = ldf(c, M, body + 0x13c) - t->z0;
                rt_log("trick: kart %u was %d frames in the air (tricks from %d), %.0f high at most, %.0f above takeoff, %u presses; "
                       "%.0f long from (%.0f, %.0f) at speed %.1f", kart, t->air, t->air_up, t->peak, t->rmax, t->presses,
                       isfinite(dx) && isfinite(dz) ? sqrtf(dx * dx + dz * dz) : 0.0f, t->x0, t->z0, t->v0);
            }
            tk_land(c, M, body, t, kart, pad, wipes);
            if (t->air >= TRICK_MIN_AIR) t->since_land = 0;
            t->air = 0, t->presses = 0, t->upjump = 0, t->air_up = 0, t->rmax = 0, t->jn = 0, t->mask = 0, t->peak = 0, t->jump_since = 999;
            t->w[0] = t->w[1] = t->acc[0] = t->acc[1] = 0;
            memset(t->nturn, 0, sizeof t->nturn);
            for (int i = 0; i < 4; i++) t->tap_since[i] = 999, t->tap_down[i] = 999; /* held from the ground: not "just pressed" (DIR_EARLY) */
            t->pend = 0, t->zr_win = 0, t->zr_up = 0, t->zl_up = 0;
        }
        if (q_scale(t->qc, TRICK_SETTLE) < 0.03f) q_id(t->qc);
        if (t->window > 0 && --t->window == 0) tk_bank(t, kart, false);
    }
    /* the rider */
    if (t->rstate == TK_SPIN) {
        t->rh = tk_hold((float)++t->rframe / (float)t->rlen);
        if (t->rframe >= t->rlen) {
            if (!ground && !test) tk_add(t, RD_TRICK[t->rkind], 0, kart);
            t->rh = 0, t->rstate = TK_IDLE;
            if (t->rnext && !ground) {
                t->rkind = t->rnext, t->rnext = 0, t->rstate = TK_SPIN, t->rframe = 0, t->rd = 0;
                rt_log("trick: kart %u then rider %s", kart, T_NAME[RD_TRICK[t->rkind]]);
            }
        }
    } else if (t->rstate == TK_SETTLE) {
        t->rh *= TRICK_SETTLE;
        if (t->rh < 0.02f) t->rh = 0, t->rstate = TK_IDLE;
    }
    s_rd_on = t->rh > 0; /* tk_rider turns it */
    t->sq *= SQUASH_KEEP;
    if (t->sq < 0.02f) t->sq = 0;
    if (fabsf(t->qc[0]) >= 0.99999f && !t->sq) return false;
    /* the turn in the kart's own frame (x across, y up, z forward), carried into the world */
    float q[9], *w = t->qc;
    q[0] = 1 - 2 * (w[2] * w[2] + w[3] * w[3]), q[1] = 2 * (w[1] * w[2] - w[0] * w[3]), q[2] = 2 * (w[1] * w[3] + w[0] * w[2]);
    q[3] = 2 * (w[1] * w[2] + w[0] * w[3]), q[4] = 1 - 2 * (w[1] * w[1] + w[3] * w[3]), q[5] = 2 * (w[2] * w[3] - w[0] * w[1]);
    q[6] = 2 * (w[1] * w[3] - w[0] * w[2]), q[7] = 2 * (w[2] * w[3] + w[0] * w[1]), q[8] = 1 - 2 * (w[1] * w[1] + w[2] * w[2]);
    float b[12], u[3][3];
    ldfn(c, M, body + 0x110, b, 12);
    for (int k = 0; k < 3; k++) { /* u[k]: the kart's axis k, unit length */
        float ax[3] = {b[k], b[4 + k], b[8 + k]};
        v3_norm(ax);
        memcpy(u[k], ax, sizeof ax);
    }
    float p[3], rw[9];
    for (int i = 0; i < 3; i++) p[i] = b[4 * i + 3] + u[1][i] * TRICK_PIVOT_UP;
    for (int i = 0; i < 3; i++) /* rw = U q U^T, U's columns the axes */
        for (int j = 0; j < 3; j++) {
            float s = 0;
            for (int k = 0; k < 3; k++)
                for (int l = 0; l < 3; l++) s += u[k][i] * q[3 * k + l] * u[l][j];
            rw[3 * i + j] = s;
        }
    for (int i = 0; i < 3; i++) {
        s_tk_a[4 * i] = rw[3 * i], s_tk_a[4 * i + 1] = rw[3 * i + 1], s_tk_a[4 * i + 2] = rw[3 * i + 2];
        s_tk_a[4 * i + 3] = p[i] - (rw[3 * i] * p[0] + rw[3 * i + 1] * p[1] + rw[3 * i + 2] * p[2]);
        s_tk_dip[i] = -u[1][i] * SQUASH_DY * sinf(t->sq * 1.5707963f);
    }
    for (int i = 0; i < 12; i++)
        if (!isfinite(s_tk_a[i])) return false;
    return true;
}
/* the back rider's models (the DriverModel whose head is further back) turned by s_rd_t after
 * MakeKartDisp gave them the display pose: base matrix = base * s_rd_t, round its own head */
/* the back rider and its head in the model's frame: false when neither driver reads */
static bool tk_rider_measure(Ctx *c, uint8_t *M, uint32_t body, uint32_t *d, float *up, float *fwd) {
    float a[5], b[5];
    uint32_t d0 = ld32(c, M, body + 0x18), d1 = ld32(c, M, body + 0x1c);
    bool ha = driver_top(c, M, d0, a), hb = driver_top(c, M, d1, b);
    *d = ha && hb ? (a[1] < b[1] ? d0 : d1) : ha ? d0 : d1; /* the one further back */
    const float *hd = ha || hb ? (*d == d0 ? a : b) : NULL;
    float sc = hd && hd[4] > 1e-3f ? hd[4] : 1;
    *up = hd ? hd[0] / sc : HEAD_UP, *fwd = hd ? hd[1] / sc : -40;
    return hd != NULL;
}
/* in the air with nothing turning (the joints lag the base a frame, so a measure taken mid-flip is
 * off by the flip's last step): kept for a rider trick started later in the jump */
static void tk_rider_rest(Ctx *c, uint8_t *M, uint32_t body, Trick *t, bool turning) {
    t->calm = turning || t->rh > 0 ? 0 : t->calm < 100 ? t->calm + 1 : t->calm;
    if (!t->air) t->rdc = 0; /* a new jump, maybe swapped riders */
    if (!t->air || t->calm < 2 || (t->air & 3)) return; /* every 4th frame is plenty */
    uint32_t d;
    float up, fwd;
    if (tk_rider_measure(c, M, body, &d, &up, &fwd)) t->rdc = d, t->rupc = up, t->rfwdc = fwd;
}
static void tk_rider(Ctx *c, uint8_t *M, uint32_t body, Trick *t) {
    if (!t->rd) { /* measured once per trick, upright (re-measuring the turned joints made it flicker) */
        bool cached = t->rdc && (fabsf(t->qc[0]) < 0.99999f || t->calm < 2) && (t->rdc == ld32(c, M, body + 0x18) || t->rdc == ld32(c, M, body + 0x1c));
        bool read = true;
        if (cached) t->rd = t->rdc, t->rup = t->rupc, t->rfwd = t->rfwdc;
        else read = tk_rider_measure(c, M, body, &t->rd, &t->rup, &t->rfwd);
        rt_log("trick: rider %08x head %.0f up, %.0f ahead%s", t->rd, t->rup, t->rfwd,
               cached ? " (upright)" : read ? "" : " (not read)");
    }
    uint32_t d = t->rd;
    if (!ram_ptr(d) || (d != ld32(c, M, body + 0x18) && d != ld32(c, M, body + 0x1c))) return;
    tk_rider_mtx(t, t->rup, t->rfwd);
    uint32_t n = ram_ptr(d) ? mld(c, M, d + 4, 2) : 0, mods = ram_ptr(d) ? ld32(c, M, d + 0xc) : 0;
    for (uint32_t i = 0; i < n && i < 8; i++) {
        uint32_t m = ram_ptr(mods) ? ld32(c, M, mods + 4 * i) : 0;
        if (!ram_ptr(m)) continue;
        float x[12], o[12];
        ldfn(c, M, m + 0x24, x, 12);
        for (int r = 0; r < 3; r++)
            for (int k = 0; k < 4; k++)
                o[4 * r + k] = x[4 * r] * s_rd_t[k] + x[4 * r + 1] * s_rd_t[4 + k] + x[4 * r + 2] * s_rd_t[8 + k] + (k == 3 ? x[4 * r + 3] : 0);
        bool ok = true;
        for (int j = 0; j < 12; j++) ok = ok && isfinite(o[j]);
        if (ok) stfn(c, M, m + 0x24, o, 12);
    }
}
/* the kart's matrices as the game made them again, the shadow level */
static void tk_restore(Ctx *c, uint8_t *M, uint32_t body) {
    if (s_tk_sus[0] == 0) return; /* FUN_802d320c didn't turn them */
    stfn(c, M, body + 0x1a0, s_tk_save[0], 12);
    for (int w = 0; w < 4; w++)
        for (int k = 0; k < 3; k++) stfn(c, M, s_tk_sus[w] + (k == 0 ? 0x158 : k == 1 ? 0x1e8 : 0x248), s_tk_save[1 + 3 * w + k], 12);
    memset(s_tk_sus, 0, sizeof s_tk_sus);
    /* ExModel::setBaseTRMtx's copy (+0x4 levels, +0xc J3DModel *[levels] (base matrix +0x24),
     * +0x2c its own while a level has no model) */
    uint32_t sh = ld32(c, M, body + 0x28);
    if (!ram_ptr(sh)) return;
    uint32_t n = mld(c, M, sh + 4, 2), mods = ld32(c, M, sh + 0xc);
    for (uint32_t i = 0; i < n && i < 8; i++) {
        uint32_t m = ram_ptr(mods) ? ld32(c, M, mods + 4 * i) : 0;
        stfn(c, M, ram_ptr(m) ? m + 0x24 : sh + 0x2c, s_tk_save[0], 12);
    }
}
uint64_t i_FUN_802d3c60(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t body = ld32(c, M, (uint32_t)a0);
    s_tk_body = 0, s_rd_on = false, s_tk_wipe = false;
    bool on = ram_ptr(body) && tk_step(c, M, body);
    bool rider = s_rd_on, wipe = s_tk_wipe;
    if (on) s_tk_body = body, fp_cams_turn(c, M, body, s_tk_a);
    ctx_call_orig(c, g_native_addr[NAT_FUN_802d3c60]);
    uint32_t kart = ram_ptr(body) ? mld(c, M, body + 0x5b3, 1) : 8;
    if (rider && kart < 8) tk_rider(c, M, body, &g_tk[kart]);
    else if ((g_tricks || g_trick_comp) && kart < 8 && g_tk[kart].body == body) tk_rider_rest(c, M, body, &g_tk[kart], on);
    if (on && s_tk_body == body) tk_restore(c, M, body);
    s_tk_body = 0;
    if (s_tk_force) { /* KartChecker::setForceGoal(checker 0): the player finishes, the race ends (RaceDirector::checkRaceEndForRace), the computers follow */
        s_tk_force = false;
        uint32_t rm = ld32(c, M, (uint32_t)ctx_gpr(c, 13) - 0x5c38), chk = ram_ptr(rm) ? ld32(c, M, rm + 0x48) : 0;
        if (ram_ptr(chk) && !mld(c, M, chk + 0x29, 1)) {
            const uint32_t gargs[1] = {chk};
            ctx_call_guest(c, 0x80186A84u, gargs, 1);
            rt_log("trick comp: forced the goal (%s)", mld(c, M, chk + 0x29, 1) ? "done" : "NOT set");
        }
    }
    uint32_t crash = wipe ? ld32(c, M, body + 0xdc) : 0;
    if (ram_ptr(crash)) { /* KartCrash::MakeHalfSpin(crash, no item): the wall hit's spin-out */
        const uint32_t args[2] = {crash, 0};
        rt_log("trick: kart %u wiped out", kart);
        ctx_call_guest(c, 0x802C9A34u, args, 2);
    }
    return 0;
}
uint64_t i_FUN_802d320c(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t body = ld32(c, M, (uint32_t)a0);
    if (s_tk_body && body == s_tk_body) {
        uint32_t ctrl = ld32(c, M, (uint32_t)ctx_gpr(c, 13) - 0x4e98), kart = mld(c, M, body + 0x5b3, 1);
        uint32_t sus[4];
        bool ok = ram_ptr(ctrl) && kart < 8;
        for (int w = 0; w < 4 && ok; w++) sus[w] = ld32(c, M, ctrl + 0xc0 + 4 * (4 * kart + w)), ok = ram_ptr(sus[w]);
        if (ok) {
            tk_apply(c, M, body + 0x1a0, s_tk_save[0], s_tk_dip);
            for (int w = 0; w < 4; w++)
                for (int k = 0; k < 3; k++) tk_apply(c, M, sus[w] + (k == 0 ? 0x158 : k == 1 ? 0x1e8 : 0x248), s_tk_save[1 + 3 * w + k], NULL);
            memcpy(s_tk_sus, sus, sizeof sus);
        }
    }
    ctx_call_orig(c, g_native_addr[NAT_FUN_802d320c]);
    return 0;
}
