/* The drivers: character packs' models and face animations in a race, and the character select screen
 * (moved from native.c; its natives are registered there, declared in drivers.h). */
#include "drivers.h"
#include "hud.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "rt.h"
#include "charpack.h"
#include "plat.h"
#include "table.h"
#include "gx.h"
#include "gp_inline.h"

/* ---- character packs (charpack.c): a driver's model and face animations from its variant ----
 * DriverModel(KartLoader *, int driverNo, ...) builds one of a kart's two drivers (KartLoader +0
 * the kart's number, +4 its KartInfo: +4 + 8 * driverNo that driver's pad, 0 for the computer, set on one driver of a player's kart). Its
 * DriverLoader and face animations read "/Driver/<Base>/<file>" from MRAM.arc through
 * ResMgr::getPtr(ArchiveId, const char *) while it runs, then "/Driver/<cmn folder>/<file>" for
 * what that folder lacks (the shared animations: cmn_Hige is Mario's, Luigi's and King Boo's).
 *
 * A variant moving as another character (charpack_anim: Mario's skeleton on Bowser Jr.) gets that
 * character's animations: from MRAM.arc its .bca files the variant has none of, and in the race the
 * ones DriverAnimBufMgr::loadAnimation streams from ARAM.arc ("DriverAnm/<folder>/<file>", the
 * folder its own or its cmn one by DriverManager +0x60 + 0x52 * driver + anim: 0 its own). Its .bas
 * stay its own (their sound events are its voice). Driver numbers are the DriverLoader's data IDs
 * (DriverLoader::mscpaDriverName, ECharID - 1). */
/* each cell's (charpack.c BASES) ECharID */
static const uint32_t CP_CHAR_ID[CP_BASES] = {7, 8, 5, 6, 11, 12, 1, 2, 17, 18, 4, 3, 13, 14, 15, 16, 9, 10, 20, 19};
static int s_cp_kart = -1, s_cp_drv, s_cp_base = -1;
static bool s_cp_human;
#define CP_DRIVER_CMN 0x80351310u   /* DriverLoader::mscpaDriverFolderName2 */
static int s_race_anim[20] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
/* DriverManager +0x7e0 + driver: it has e_good_b2.bca of its own; the character it moves as's while
 * it does (the game's back at the next race) */
static uint32_t s_race_dm;
static int s_race_good2[20];
static void race_anim_reset(Ctx *c, uint8_t *M) {
    for (int d = 0; d < 20; d++) {
        if (s_race_anim[d] >= 0 && IS_RAM(s_race_dm)) mst(c, M, s_race_dm + 0x7e0 + d, 1, (uint32_t)s_race_good2[d]);
        s_race_anim[d] = -1;
    }
}
static void race_anim_set(Ctx *c, uint8_t *M, int d, int a) {
    uint32_t dm = ld32(c, M, (uint32_t)RD(c, REG_R13, 4) - 0x5770); /* DriverManager::ptr()'s */
    if (d < 0 || d >= 20 || a < 0 || a >= 20 || !IS_RAM(dm) || s_race_anim[d] >= 0) return;
    s_race_dm = dm;
    s_race_good2[d] = (int)mld(c, M, dm + 0x7e0 + d, 1);
    mst(c, M, dm + 0x7e0 + d, 1, mld(c, M, dm + 0x7e0 + a, 1));
    s_race_anim[d] = a;
}
/* each kart's DriverModels (per driver: the loader's index), for which sits in front */
uint32_t g_cp_dm[8][2];
/* DriverModel::IsBack: its state (+0xa8) one of the back seat's (the function's jump table at
 * 0x803513f0 going to its "true", 0x801c0330, else 7 or 0x68) */
bool dm_back(Ctx *c, uint8_t *M, uint32_t dm) {
    if (!IS_RAM(dm)) return false;
    uint32_t st = ld32(c, M, dm + 0xa8);
    if (st <= 0x93 && ld32(c, M, 0x803513f0u + 4 * st) == 0x801c0330u) return true;
    return st == 7 || st == 0x68;
}
uint64_t i___ct__11DriverModelFP10KartLoaderiUsbi(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    if (IS_RAM((uint32_t)a1)) {
        uint32_t k = ld32(c, M, (uint32_t)a1);
        if (k < 8) g_cp_dm[k][a2 & 1] = (uint32_t)a0;
    }
    uint32_t loader = (uint32_t)a1, info = IS_RAM(loader) ? ld32(c, M, loader + 4) : 0;
    int kart = s_cp_kart, drv = s_cp_drv;
    bool human = s_cp_human;
    s_cp_kart = IS_RAM(loader) ? (int)ld32(c, M, loader) : -1, s_cp_drv = (int)(a2 & 1);
    /* a player's kart: either driver has the pad (the game keeps it on one of them only) */
    s_cp_human = IS_RAM(info) && (ld32(c, M, info + 4) != 0 || ld32(c, M, info + 0xc) != 0);
    if (s_cp_kart == 0 && s_cp_drv == 0) charpack_race_begin(), race_anim_reset(c, M); /* a race's first driver */
    int base = s_cp_base;
    s_cp_base = -1;
    ctx_call_orig(c, g_native_addr[NAT___ct__11DriverModelFP10KartLoaderiUsbi]);
    s_cp_kart = kart, s_cp_drv = drv, s_cp_human = human, s_cp_base = base;
    return ctx_gpr(c, 3);
}
/* each driver's CharacterSoundMgr (built inside its DriverModel): setVoice on it is that kart and
 * driver speaking (charpack_voice_speak: the line plays as its variant or the game's own) */
static uint32_t s_voice_mgr[8][2];
uint64_t i___ct__Q29GameAudio17CharacterSoundMgrFP3VecP7JKRHeapUc7ECharID(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t mgr = (uint32_t)a0;
    ctx_call_orig(c, g_native_addr[NAT___ct__Q29GameAudio17CharacterSoundMgrFP3VecP7JKRHeapUc7ECharID]);
    if (s_cp_kart >= 0 && s_cp_kart < 8) {
        s_voice_mgr[s_cp_kart][s_cp_drv & 1] = mgr;
        if (charpack_count()) rt_log("charvoice: kart %d driver %d's voice %#x", s_cp_kart, s_cp_drv & 1, mgr);
    }
    return ctx_gpr(c, 3);
}
uint64_t i_setVoice__Q29GameAudio17CharacterSoundMgrFUc(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    for (int k = 0; k < 8 && a0 && charpack_count(); k++)
        for (int d = 0; d < 2; d++)
            if (s_voice_mgr[k][d] == (uint32_t)a0) charpack_voice_speak(k, d);
    ctx_call_orig(c, g_native_addr[NAT_setVoice__Q29GameAudio17CharacterSoundMgrFUc]);
    return 0;
}
/* DriverManager, built inside the first DriverLoader, keeps Mario's model as every driver's
 * reference: always the game's own */
uint64_t i___ct__13DriverManagerFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    int kart = s_cp_kart;
    s_cp_kart = -1;
    ctx_call_orig(c, g_native_addr[NAT___ct__13DriverManagerFv]);
    s_cp_kart = kart;
    return ctx_gpr(c, 3);
}
uint64_t i_getPtr__6ResMgrFQ26ResMgr9ArchiveIdPCc(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    if (s_cp_kart >= 0 && a0 == 0 /* mcArcMRAM */ && IS_RAM((uint32_t)a1) && charpack_count()) {
        char p[64];
        unsigned n = 0;
        for (; n < sizeof p - 1 && (p[n] = (char)mld(c, M, (uint32_t)a1 + n, 1)); n++) {}
        p[n] = 0;
        const char *s = p + (p[0] == '/'), *sl = strncasecmp(s, "Driver/", 7) ? NULL : strchr(s + 7, '/');
        bool own = sl && charpack_base(s + 7, (size_t)(sl - s - 7)) >= 0;
        if (own) s_cp_base = charpack_base(s + 7, (size_t)(sl - s - 7)); /* the driver being built (its cmn folder's come after) */
        int b = sl ? s_cp_base : -1;
        int v = b < 0 ? -1 : charpack_pick(s_cp_kart, s_cp_drv, b, s_cp_human);
        char folder[24];
        snprintf(folder, sizeof folder, "%.*s", sl ? (int)(sl - s - 7) : 0, s + 7);
        uint32_t size, a = v < 0 ? 0 : own ? charpack_file(v, sl + 1, &size) : charpack_folder_file(v, folder, sl + 1, &size);
        if (a) {
            rt_log("charpack: kart %d driver %d (%s): %s from variant %d", s_cp_kart, s_cp_drv, s_cp_human ? "player" : "computer", s, v + 1);
            if (own && !strcasecmp(sl + 1, "driver.bmd")) {
                charpack_loaded(b, v);
                charpack_race_driver(s_cp_kart, s_cp_drv, b, v);
                if (charpack_anim(v) >= 0) race_anim_set(c, M, CP_CHAR_ID[b] - 1, CP_CHAR_ID[charpack_anim(v)] - 1);
            }
            return a;
        }
        if (own && !strcasecmp(sl + 1, "driver.bmd")) charpack_race_driver(s_cp_kart, s_cp_drv, b, -1);
        /* a variant moving as another character: that one's .bca (its own folder's, else its cmn one's) */
        size_t fl = sl ? strlen(sl + 1) : 0;
        if (v >= 0 && charpack_anim(v) >= 0 && fl > 4 && !strcasecmp(sl + 1 + fl - 4, ".bca")) {
            static uint32_t path_mem;
            if (!path_mem) path_mem = charpack_reserve(64);
            int d = CP_CHAR_ID[charpack_anim(v)] - 1;
            uint32_t lr = (uint32_t)RD(c, 0x1020, 4);
            for (int k = 0; k < 2 && path_mem; k++) {
                char name[24], q[64];
                guest_str(c, M, ld32(c, M, (k ? CP_DRIVER_CMN : CP_DRIVER_NAMES) + 4 * d), name, sizeof name);
                int ql = snprintf(q, sizeof q, "/Driver/%s/%s", name, sl + 1);
                if (!name[0] || ql <= 0 || ql >= (int)sizeof q) break;
                for (int i = 0; i <= ql; i++) mst(c, M, path_mem + i, 1, (uint8_t)q[i]);
                const uint32_t args[2] = {0 /* mcArcMRAM */, path_mem};
                uint32_t r = ctx_call_guest(c, g_native_addr[NAT_getPtr__6ResMgrFQ26ResMgr9ArchiveIdPCc], args, 2);
                if (r) {
                    WR(c, 0x1020, 4, lr);
                    rt_log("charpack: kart %d driver %d: %s as %s", s_cp_kart, s_cp_drv, s, q);
                    return r;
                }
            }
            WR(c, 0x1020, 4, lr);
            ctx_set_gpr(c, 3, (uint32_t)a0), ctx_set_gpr(c, 4, (uint32_t)a1); /* the game's own path */
        }
    }
    ctx_call_orig(c, g_native_addr[NAT_getPtr__6ResMgrFQ26ResMgr9ArchiveIdPCc]);
    return ctx_gpr(c, 3);
}
/* DriverAnimBufMgr::loadAnimation(anim, driver): a race streams a driver's animation into a free
 * buffer. A variant moving as another character asks for that one's: the folder entries and the
 * own-or-cmn flag are that character's for the call (the request keeps the folder's string). */
uint64_t i_loadAnimation__16DriverAnimBufMgrFii(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    int anm = (int)a1, d = (int)a2, a = d >= 0 && d < 20 ? s_race_anim[d] : -1;
    uint32_t dm = a >= 0 ? ld32(c, M, (uint32_t)RD(c, REG_R13, 4) - 0x5770) : 0;
    if (a < 0 || !IS_RAM(dm) || anm < 0 || anm >= 0x52) {
        ctx_call_orig(c, g_native_addr[NAT_loadAnimation__16DriverAnimBufMgrFii]);
        return ctx_gpr(c, 3);
    }
    uint32_t name = ld32(c, M, CP_DRIVER_NAMES + 4 * d), cmn = ld32(c, M, CP_DRIVER_CMN + 4 * d);
    uint32_t flag = dm + 0x60 + 0x52 * d + anm, own = mld(c, M, flag, 1);
    mst(c, M, CP_DRIVER_NAMES + 4 * d, 4, ld32(c, M, CP_DRIVER_NAMES + 4 * a));
    mst(c, M, CP_DRIVER_CMN + 4 * d, 4, ld32(c, M, CP_DRIVER_CMN + 4 * a));
    mst(c, M, flag, 1, mld(c, M, dm + 0x60 + 0x52 * a + anm, 1));
    ctx_call_orig(c, g_native_addr[NAT_loadAnimation__16DriverAnimBufMgrFii]);
    mst(c, M, CP_DRIVER_NAMES + 4 * d, 4, name), mst(c, M, CP_DRIVER_CMN + 4 * d, 4, cmn), mst(c, M, flag, 1, own);
    return ctx_gpr(c, 3);
}
/* SceneMenu::character, each frame of the character select screen. +0x20f4 the number of players
 * - 1; player p's team (two drivers) is +0x211c + 4p. Blocks of 0x1c: player p's cursor cell
 * +0x212c (-1 none); team t's drivers' players +0x2130 / +0x2134, chosen cells +0x2138 / +0x213c,
 * kart +0x2140, state +0x2144 (0 choosing the drivers, 1 the kart). Cells number the grid column by
 * column (charpack.c BASES). The front end's page buttons (g_char_cycle) turn charpack.c's pages.
 *
 * A variant stands for its base: on the frame a player's A goes down on one shown in another
 * character's cell (charpack.c layout) their cursor is the base's cell, so selectA chooses the base:
 * its voice and animation, and after it setRaceData, the kart step and the computer players are the
 * base's. charpack.c keeps each team's driver's pick (charpack_pending), so one character's variants
 * and the character itself are each a character of their own: the menu's flags (+0x228c + cell, 1
 * free: the cursors skip the others; setCharacterFlag sets them from the chosen cells and the locked
 * characters) say taken only where the very pick a cell offers on this page is chosen
 * (charpack_cell_taken), so two teams (or a team's two drivers) can choose the same base's cell.
 * Players are ports (the pads PADRead gave, hle.c).
 *
 * The random pick (L + R: rndSelect chooses each team's two cells at once) chooses the cell it lands
 * on: a variant there is picked as above. A pick made in another character's cell shows there while
 * the page shows it there (charpack_slot_shown): its select animation (per cell: +0x21ec + 8 * cell
 * set, +0x21f0 the player, the pane's frame +0x1374 + 12 * cell) and, while characterAnm runs, its
 * highlight.
 *
 * The 3D models: CharacterSelect3D has one block per character (sel_models), and each team's
 * drivers show the block setCharacter(team, driver, ECharID) names (+0xc0c + 0x70 * team + 0x38 *
 * driver; +0xc08 set). Each chosen driver and each cursor's preview gets a block: the game's own
 * characters their own, a variant its base's when that is free, else the free one of the nearest
 * select screen scale, which then shows the variant's model (charpack_borrow). */
/* CharacterSelect3D::mCharacterDefScale, by ECharID - 1 */
static const float SEL_SCALE[20] = {1, 1, 1.15f, 1.15f, 1, 1, 1, 1, 0.95f, 1, 1.05f, 1.05f, 1.45f, 1.05f, 1.18f, 1.3f, 1, 1, 1, 0.8f};
#define SEL_SET_FLAGS 0x801639B4u /* SceneMenu::setCharacterFlag */
/* per team and driver: the ECharID characterAnm's setCharacter names (the game's choice), the one
 * its preview shows instead (0 the game's), and the one written last */
static uint32_t s_sel_from[4][2], s_sel_want[4][2], s_sel_wrote[4][2];
static bool s_sel_cursor[4][2]; /* ... a cursor's preview: the driver it lands on is the game's */
static uint32_t s_sel_flags_sig = ~0u;

/* The cell team t's driver k's pick shows in on this page (its own cell when this page offers it
 * there, else the cell it was picked in when shown there), -1 none: the pick is on another page. */
static int sel_pick_here(int t, int k) {
    int v, cell = charpack_slot(t, k, &v), vc = charpack_slot_shown(t, k);
    if (cell < 0 || vc >= 0) return cell < 0 ? -1 : vc;
    int on = charpack_cell_variant(cell);
    return (v >= 0 ? on == v : on < 0 && charpack_page() == 0) ? cell : -1;
}
/* The third row (cells CP_BASES + column: see charpack.h). Each column screen has Cha3 under its two
 * (cc3_make), the game knowing nothing of it: its face is a copy of the face texture per column
 * (drawn as that cell's variant, charpack_art.c K_ROW3), and its look follows the players whose
 * cursor is in it (s_row3: the column, -1 none; the game's cursor stays on the cell above, the
 * row's second, its marker there moved to Cha3's CC3). */
typedef struct {
    uint32_t cha, cp, cc, cw, cp_jt, cc_jt;
    uint32_t mine[4], two[4]; /* CPB3 CW3 CP3 CWC3, and the same of Cha2 (their looks: row3_follow) */
} Row3;
static Row3 s_r3[CP_ROW];
/* The column screen's materials (J2DScreen +0x102 their count, +0x104 them, 0x88 each; a material's
 * +0x84 its animations): Cha3's are the s_r3_m after the game's s_r3_nmat (cc3_make), each a copy
 * of one of Cha1's, s_r3_src[k] the Cha2 one whose animations it takes. They are bound by name, so
 * Cha3's (renamed) get none: row3_follow gives them copies of a cell's each frame (R3_ANM bytes
 * per column and material at s_r3_anm: the pointers' block, then its colour, SRT and TEV ones). */
static unsigned s_r3_nmat, s_r3_m;
static int s_r3_src[16];
static bool s_r3_two[16]; /* s_r3_src[k] is a Cha2 material (not the fallback) */
static uint8_t s_r3_age[CP_ROW]; /* frames each second-row cell has been free (its look settled) */
#define R3_SETTLE 45
/* A cell above a third-row cursor keeps the game's cursor (its look the one Cha3 takes) and is drawn
 * free: what it was before that (restored before characterAnm) */
typedef struct { uint32_t at, n; uint8_t b[0x1c]; } R3Save;
static R3Save s_r3_save[CP_ROW * 16];
static int s_r3_nsave;
static uint32_t s_r3_anm;
#define R3_ANM 0x248
#define R3_GRID_UP 16.0f /* the faces moved up (the title banner too: i_calc__13MenuTitleLineFv) */
static bool s_r3_ok;
static uint32_t s_r3_copy[CP_ROW], s_r3_copy_size[CP_ROW];
static int s_row3[4] = {-1, -1, -1, -1};
static bool row3_has(int col) { return col >= 0 && col < CP_ROW && charpack_cell_variant(CP_BASES + col) >= 0; }
/* the third row's column with a character from col on: dir -1 / 1 the next that way (around), 0 the
 * nearest (col itself first); -1 none */
static int row3_next(int col, int dir) {
    for (int d = dir ? 1 : 0; d < CP_ROW; d++) {
        int a = dir ? ((col + dir * d) % CP_ROW + CP_ROW) % CP_ROW : col - d, b = col + d;
        if (row3_has(a)) return a;
        if (!dir && row3_has(b)) return b;
    }
    return -1;
}
/* a pointer into MEM1 (IS_RAM also takes 0 and the physical / uncached views: a list's end, garbage) */
#define PTR_OK(a) ((a) >= 0x80000000u && (a) < 0x80000000u + RAM_SIZE - 0x200)
/* a J2DPictureEx's texture (its material +0x168, the material's TEV block +0x70 holding it): the
 * JUTTexture there whose GXTexObj draws its ResTIMG's picture (+0xc image address >> 5, +0x20 the
 * ResTIMG, +0x1c the picture's offset in it) */
static uint32_t pic_tex(Ctx *c, uint8_t *M, uint32_t pic) {
    uint32_t mat = PTR_OK(pic) ? ld32(c, M, pic + 0x168) : 0, tev = PTR_OK(mat) ? ld32(c, M, mat + 0x70) : 0;
    for (uint32_t o = 4; PTR_OK(tev) && o < 0x100; o += 4) {
        uint32_t jt = ld32(c, M, tev + o), timg = PTR_OK(jt) ? ld32(c, M, jt + 0x20) : 0;
        if (!PTR_OK(timg)) continue;
        if (((ld32(c, M, jt + 0xc) & 0x1FFFFFu) << 5) == ((timg + ld32(c, M, timg + 0x1c)) & RAM_MASK)) return jt;
    }
    return 0;
}
/* the pane tagged tag under pane (J2DPane +0x10 the tag, right-aligned; +0xdc the children) */
static uint32_t pane_find(Ctx *c, uint8_t *M, uint32_t pane, const char *tag, int depth) {
    if (!PTR_OK(pane) || depth > 4) return 0;
    size_t n = strlen(tag);
    bool ok = n <= 8;
    for (size_t i = 0; ok && i < 8; i++) ok = (char)mld(c, M, pane + 0x10 + (uint32_t)i, 1) == (i < 8 - n ? 0 : tag[i - (8 - n)]);
    if (ok) return pane;
    for (uint32_t link = ld32(c, M, pane + 0xdc), k = 0; PTR_OK(link) && k < 16; link = ld32(c, M, link + 0xc), k++) {
        uint32_t f = pane_find(c, M, ld32(c, M, link), tag, depth + 1);
        if (f) return f;
    }
    return 0;
}
/* after SceneMenu's constructor: each column's Cha3 panes, and its face's own copy */
static void row3_setup(Ctx *c, uint8_t *M, uint32_t menu) {
    s_r3_ok = false, s_r3_nsave = 0;
    memset(s_r3_age, 0, sizeof s_r3_age);
    for (int p = 0; p < 4; p++) s_row3[p] = -1;
    if (!IS_RAM(menu) || !charpack_count()) { rt_log("row3: not set up (menu %#x, %u characters)", menu, charpack_count()); return; }
    for (int col = 0; col < CP_ROW; col++) {
        uint32_t scr = ld32(c, M, menu + 0x1f88 + 4 * (uint32_t)col);
        Row3 *r = &s_r3[col];
        r->cha = pane_find(c, M, scr, "Cha3", 0), r->cp = pane_find(c, M, scr, "CP3", 0);
        r->cc = pane_find(c, M, scr, "CC3", 0), r->cw = pane_find(c, M, scr, "CW3", 0);
        r->cp_jt = pic_tex(c, M, r->cp), r->cc_jt = pic_tex(c, M, r->cc);
        static const char *const TAGS[2][4] = {{"CPB3", "CW3", "CP3", "CWC3"}, {"CPB2", "CW2", "CP2", "CWC2"}};
        for (int i = 0; i < 4; i++) r->mine[i] = pane_find(c, M, scr, TAGS[0][i], 0), r->two[i] = pane_find(c, M, scr, TAGS[1][i], 0);
        if (ld16(c, M, scr + 0x102) != s_r3_nmat + s_r3_m) rt_log("row3: column %d has %u materials, not %u", col, ld16(c, M, scr + 0x102), s_r3_nmat + s_r3_m);
        if (r->cha) mst(c, M, r->cha + 0xb0, 1, 0); /* shown once all is set */
        if (!r->cha || !r->cc || !r->cw || !r->cp_jt || !r->cc_jt) { rt_log("row3: column %d has no third cell", col); return; }
        uint32_t timg = ld32(c, M, r->cp_jt + 0x20), img3 = ld32(c, M, r->cp_jt + 0xc), addr = (img3 & 0x1FFFFFu) << 5;
        size_t size = gx_tex_size(mld(c, M, timg, 1), ld16(c, M, timg + 2), ld16(c, M, timg + 4));
        if (!size || size > 0x10000 || addr + size > RAM_SIZE) { rt_log("row3: column %d's face: %#x, %lu bytes", col, addr, (unsigned long)size); return; }
        if (!s_r3_copy[col] || s_r3_copy_size[col] < size) s_r3_copy[col] = charpack_reserve((uint32_t)size), s_r3_copy_size[col] = (uint32_t)size;
        if (!s_r3_copy[col]) { rt_log("row3: no room for column %d's face", col); return; }
        /* drawn as its substitute, so its own bytes are only a name: never the face's (a texture
         * known by its contents, as the HD pack's, would be the row above's too) */
        memset(M + (s_r3_copy[col] & RAM_MASK), 0xA5 ^ col, size);
        M[s_r3_copy[col] & RAM_MASK] = (uint8_t)col;
        gx_subst_addr(s_r3_copy[col], CP_ROW3_SUBST + (unsigned)col);
        mst(c, M, r->cp_jt + 0xc, 4, (img3 & ~0x1FFFFFu) | ((s_r3_copy[col] & RAM_MASK) >> 5));
    }
    if (!s_r3_anm) s_r3_anm = charpack_reserve(CP_ROW * 16 * R3_ANM);
    for (int col = 0; col < CP_ROW; col++) {
        uint32_t scr = ld32(c, M, menu + 0x1f88 + 4 * (uint32_t)col), up;
        float f = -R3_GRID_UP;
        memcpy(&up, &f, 4);
        mst(c, M, scr + 0xd8, 4, up); /* the screen's own offset (not animated) */
    }
    s_r3_ok = true;
    rt_log("row3: the third row's %d cells set up", CP_ROW);
}
/* a cell's look as a free one (characterAnm's state 0: the panes +0x14d8 / +0x14f0 by cell shown,
 * their frames and +0x16bc / +0x16d4 / +0x16ec's the SDA2 floats -0x6218 and -0x61f8; its marker
 * +0x16e8 hidden) */
static void cell_free(Ctx *c, uint8_t *M, uint32_t menu, int cell) {
    uint32_t r2 = (uint32_t)RD(c, REG_R2, 4), f1 = ld32(c, M, r2 - 0x6218), f0 = ld32(c, M, r2 - 0x61f8);
    uint32_t col = (uint32_t)(cell / 2), row = (uint32_t)(cell % 2);
    uint32_t a = menu + 0x30 * col + 0xc * row, b = menu + 0x48 * col + 0xc * row;
    uint32_t mark = ld32(c, M, b + 0x16e8), p0 = ld32(c, M, a + 0x14d8), p1 = ld32(c, M, a + 0x14f0);
    if (IS_RAM(mark)) mst(c, M, mark + 0xb0, 1, 0);
    if (IS_RAM(p0)) mst(c, M, p0 + 0xb0, 1, 1);
    if (IS_RAM(p1)) mst(c, M, p1 + 0xb0, 1, 1);
    mst(c, M, a + 0x14dc, 4, f1), mst(c, M, a + 0x14f4, 4, f1);
    mst(c, M, b + 0x16bc, 4, f1), mst(c, M, b + 0x16d4, 4, f0), mst(c, M, b + 0x16ec, 4, f1);
    mst(c, M, menu + 0x219c + 4 * (uint32_t)cell, 4, 0);
}
/* the player p's cursor is choosing (its team +0x211c + 4 * p, the team's +0x2144 0) */
static bool sel_choosing(Ctx *c, uint8_t *M, uint32_t menu, int p) {
    uint32_t t = ld32(c, M, menu + 0x211c + 4 * (uint32_t)p), teams = ld32(c, M, menu + 0x20fc) + 1;
    return t < teams && t < 4 && ld32(c, M, menu + 0x1c * t + 0x2144) == 0;
}
/* material dm takes copies of the animations ap (a material's +0x84 block; not the texture pattern's)
 * in buf (R3_ANM bytes) */
static void anm_follow(Ctx *c, uint8_t *M, uint32_t dm, uint32_t ap, uint32_t buf) {
    if (!PTR_OK(ap)) { mst(c, M, dm + 0x84, 4, 0); return; }
    memcpy(M + (buf & RAM_MASK), M + (ap & RAM_MASK), 0x48);
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t a = ld32(c, M, ap + 4 * i), to = buf + 0x48 + 0x80 * i;
        if (i == 2 || !PTR_OK(a)) { mst(c, M, buf + 4 * i, 4, 0); continue; }
        memcpy(M + (to & RAM_MASK), M + (a & RAM_MASK), 0x80);
        mst(c, M, buf + 4 * i, 4, to);
    }
    mst(c, M, dm + 0x84, 4, buf);
}
/* pane fields copied (shown, cull, alpha at +0xb0; angles, anchor, scale at +0xb8), saved first when save */
static void r3_put(Ctx *c, uint8_t *M, uint32_t at, uint32_t n, const uint8_t *from, bool save) {
    if (save && s_r3_nsave < (int)(sizeof s_r3_save / sizeof s_r3_save[0]) && n <= 0x1c) {
        R3Save *v = &s_r3_save[s_r3_nsave++];
        v->at = at, v->n = n;
        memcpy(v->b, M + (at & RAM_MASK), n);
    } else if (save) return; /* nothing unsaved is changed */
    memmove(M + (at & RAM_MASK), from, n);
}
static void pane_follow(Ctx *c, uint8_t *M, uint32_t to, uint32_t from, bool save) {
    if (!PTR_OK(to) || !PTR_OK(from)) return;
    r3_put(c, M, to + 0xb0, 4, M + ((from + 0xb0) & RAM_MASK), save);
    r3_put(c, M, to + 0xb8, 0x1c, M + ((from + 0xb8) & RAM_MASK), save);
}
/* before characterAnm: the cells row3_look drew free as they were */
static void row3_restore(Ctx *c, uint8_t *M) {
    while (s_r3_nsave > 0) {
        const R3Save *v = &s_r3_save[--s_r3_nsave];
        memcpy(M + (v->at & RAM_MASK), v->b, v->n);
    }
}
/* Cha3 of column col looks as Cha2 of column sc: its materials get copies of that one's animations
 * (not its texture pattern: the face is ours), its panes that one's visibility, alpha, rotation and
 * scale (animated per pane). */
static void row3_follow(Ctx *c, uint8_t *M, uint32_t menu, int col, int sc) {
    uint32_t mats = ld32(c, M, ld32(c, M, menu + 0x1f88 + 4 * (uint32_t)col) + 0x104);
    uint32_t from = ld32(c, M, ld32(c, M, menu + 0x1f88 + 4 * (uint32_t)sc) + 0x104);
    if (!PTR_OK(mats) || !PTR_OK(from) || !s_r3_anm) return;
    for (unsigned k = 0; k < s_r3_m && k < 8; k++)
        anm_follow(c, M, mats + (s_r3_nmat + k) * 0x88, s_r3_src[k] >= 0 ? ld32(c, M, from + (uint32_t)s_r3_src[k] * 0x88 + 0x84) : 0,
                   s_r3_anm + ((uint32_t)col * 16 + k) * R3_ANM);
    const Row3 *r = &s_r3[col], *q = &s_r3[sc];
    for (int i = 0; i < 4; i++) pane_follow(c, M, r->mine[i], q->two[i], false);
}
/* Cha2 of column col drawn as Cha2 of column sc (free): its materials, panes, the cell's own panes
 * (characterAnm's +0x14d8 / +0x14f0 shown when free) and its marker hidden, all restored before the
 * next characterAnm */
static void row2_as(Ctx *c, uint8_t *M, uint32_t menu, int col, int sc) {
    uint32_t mats = ld32(c, M, ld32(c, M, menu + 0x1f88 + 4 * (uint32_t)col) + 0x104);
    uint32_t from = ld32(c, M, ld32(c, M, menu + 0x1f88 + 4 * (uint32_t)sc) + 0x104);
    if (!PTR_OK(mats) || !PTR_OK(from) || !s_r3_anm) return;
    for (unsigned k = 0; k < s_r3_m && k < 8; k++) {
        if (!s_r3_two[k]) continue;
        uint32_t dm = mats + (uint32_t)s_r3_src[k] * 0x88;
        r3_put(c, M, dm + 0x84, 4, M + ((dm + 0x84) & RAM_MASK), true);
        if (s_r3_nsave && s_r3_save[s_r3_nsave - 1].at == dm + 0x84)
            anm_follow(c, M, dm, ld32(c, M, from + (uint32_t)s_r3_src[k] * 0x88 + 0x84), s_r3_anm + ((uint32_t)col * 16 + 8 + k) * R3_ANM);
    }
    const Row3 *r = &s_r3[col], *q = &s_r3[sc];
    for (int i = 0; i < 4; i++) pane_follow(c, M, r->two[i], q->two[i], true);
    for (uint32_t j = 0; j < 2; j++) {
        uint32_t p = ld32(c, M, menu + 0x30 * (uint32_t)col + 0xc + 0x14d8 + 0x18 * j), f = ld32(c, M, menu + 0x30 * (uint32_t)sc + 0xc + 0x14d8 + 0x18 * j);
        if (PTR_OK(p) && PTR_OK(f)) r3_put(c, M, p + 0xb0, 1, M + ((f + 0xb0) & RAM_MASK), true);
    }
    uint32_t mark = ld32(c, M, menu + 0x48 * (uint32_t)col + 0xc + 0x16e8);
    static const uint8_t hide = 0;
    if (PTR_OK(mark)) r3_put(c, M, mark + 0xb0, 1, &hide, true);
}
/* each frame after characterAnm: the third row's look (a cursor's marker, a pick there dimmed) */
static void row3_look(Ctx *c, uint8_t *M, uint32_t menu) {
    if (!s_r3_ok) return;
    bool other[CP_BASES] = {false};
    int who[CP_ROW];
    for (int i = 0; i < CP_ROW; i++) who[i] = -1;
    for (int p = 0; p < 4; p++) {
        if (!sel_choosing(c, M, menu, p)) continue;
        int g = (int)ld32(c, M, menu + 0x1c * (uint32_t)p + 0x212c);
        if (s_row3[p] >= 0 && s_row3[p] < CP_ROW) { if (who[s_row3[p]] < 0) who[s_row3[p]] = p; }
        else if (g >= 0 && g < CP_BASES) other[g] = true;
    }
    /* a free look to copy: a second-row cell free long enough for its last look's animation to end
     * (one just left still fades out), the nearest such, else the one free longest */
    uint32_t st[CP_ROW];
    for (int t = 0; t < CP_ROW; t++) {
        st[t] = ld32(c, M, menu + 0x219c + 4 * (uint32_t)(t * 2 + 1));
        s_r3_age[t] = st[t] ? 0 : s_r3_age[t] < 255 ? s_r3_age[t] + 1 : 255;
    }
    int fsrc[CP_ROW];
    for (int col = 0; col < CP_ROW; col++) {
        int best = -1;
        for (int i = 0; i < CP_ROW; i++) {
            int t = (col + i) % CP_ROW;
            if (st[t] || who[t] >= 0) continue;
            if (s_r3_age[t] >= R3_SETTLE) { best = t; break; }
            if (best < 0 || s_r3_age[t] > s_r3_age[best]) best = t;
        }
        fsrc[col] = best;
    }
    /* Cha3's look: under a cursor the cell above's (the game's cursor is there), else a free one;
     * all before any cell above is drawn free (row2_as) */
    bool hover[CP_ROW];
    for (int col = 0; col < CP_ROW; col++) {
        const Row3 *r = &s_r3[col];
        int g = col * 2 + 1, v = charpack_cell_variant(CP_BASES + col);
        hover[col] = who[col] >= 0 && !other[g] && st[col] == 2;
        int sc = hover[col] ? col : fsrc[col] >= 0 ? fsrc[col] : col;
        row3_follow(c, M, menu, col, sc);
        uint32_t chip = ld32(c, M, menu + 0x16e8 + 0x48 * (uint32_t)col + 0xc);
        if (who[col] >= 0) {
            uint32_t jt = pic_tex(c, M, chip);
            if (jt) memcpy(M + (r->cc_jt & RAM_MASK), M + (jt & RAM_MASK), 0x20); /* its GXTexObj: the player's marker */
        }
        mst(c, M, r->cha + 0xb0, 1, 1);
        mst(c, M, r->cc + 0xb0, 1, who[col] >= 0);
        mst(c, M, r->cp + 0xb2, 1, v >= 0 && charpack_variant_taken(v) ? 0x60 : 0xFF);
    }
    for (int col = 0; col < CP_ROW; col++)
        if (hover[col] && fsrc[col] >= 0) row2_as(c, M, menu, col, fsrc[col]);
}
/* characterAnm draws each cell by its state (+0x219c + 4 * cell: 0 free, 1 chosen, 2 under a
 * cursor): a chosen one's marker (its pane, +0x16e8 by cell, +0xb0 shown) and dimmed look. A pick
 * is chosen only on the page it shows on, in the cell it shows in: elsewhere the cell offers another
 * character, free, so after the game's a chosen cell no pick shows in here is drawn as a free one
 * (characterAnm's state 0: the panes +0x14d8 / +0x14f0 by cell shown, their frames and +0x16bc /
 * +0x16d4 / +0x16ec's the SDA2 floats -0x6218 and -0x61f8). */
static void sel_marks(Ctx *c, uint8_t *M, uint32_t menu, uint32_t teams) {
    bool here[CP_BASES] = {false};
    for (uint32_t t = 0; t < teams && t < 4; t++)
        for (uint32_t k = 0; k < 2; k++) {
            int h = sel_pick_here((int)t, (int)k);
            if (h >= 0 && h < CP_BASES) here[h] = true;
        }
    for (int cell = 0; cell < CP_BASES; cell++)
        if (!here[cell] && ld32(c, M, menu + 0x219c + 4 * (uint32_t)cell) == 1) cell_free(c, M, menu, cell);
}
/* B on a chosen driver: the game puts the cursor on the cell the driver holds, a variant's base's (another
 * cell, page or none of the third row). Where it was when A chose it comes back instead: per team and driver,
 * the cursor as cursor[] has it (CP_BASES + r: third row r) and the page. */
static struct { bool on; int cursor, page; } s_picked[4][2];
/* this frame of SceneMenu::character: the chosen cells before the game's and their players (s_undo_teams 0
 * outside its call), and the cell to draw each player's cursor in when A has it on the base's for the game
 * (-1 no such): characterAnm draws within the call, and one frame on another cell shows */
static int s_undo_before[4][2], s_undo_owner[4][2], s_anm_cursor[4] = {-1, -1, -1, -1};
static uint32_t s_undo_teams;
static void sel_undo(Ctx *c, uint8_t *M, uint32_t menu) {
    for (uint32_t t = 0; t < s_undo_teams; t++)
        for (uint32_t k = 0; k < 2; k++) {
            int p = s_undo_owner[t][k];
            if (s_undo_before[t][k] < 0 || (int)ld32(c, M, menu + 0x1c * t + 0x2138 + 4 * k) >= 0 || p < 0) continue;
            if (!s_picked[t][k].on) continue;
            int to = s_picked[t][k].cursor;
            s_picked[t][k].on = false;
            charpack_page_set(s_picked[t][k].page);
            s_row3[p] = to >= CP_BASES ? to - CP_BASES : -1;
            mst(c, M, menu + 0x1c * (uint32_t)p + 0x212c, 4, (uint32_t)(to >= CP_BASES ? (to - CP_BASES) * 2 + 1 : to));
            rt_log("charpack: player %d's undo: cursor back on %d, page %d", p + 1, to, s_picked[t][k].page + 1);
        }
}
static void park_up(Ctx *c, uint8_t *M, uint32_t pane, int depth);
static void vp_place(Ctx *c, uint8_t *M, uint32_t menu);
/* SceneMenu::characterAnm, from SceneMenu::character: the grid's highlights and the 3D models'
 * characters (setCharacter, from the menu's table) of the chosen cells and the cursors. A pick shown
 * in another character's cell is highlighted there; then each driver's preview its block. */
uint64_t i_characterAnm__9SceneMenuFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t menu = (uint32_t)a0, at[8];
    int was[8], n = 0;
    uint32_t teams = IS_RAM(menu) && charpack_count() ? ld32(c, M, menu + 0x20fc) + 1 : 0;
    for (uint32_t t = 0; t < teams && t < 4; t++)
        for (uint32_t k = 0; k < 2; k++) {
            uint32_t a = menu + 0x1c * t + 0x2138 + 4 * k;
            int vc = charpack_slot_shown((int)t, (int)k);
            if (vc < 0) continue;
            at[n] = a, was[n++] = (int)ld32(c, M, a);
            mst(c, M, a, 4, (uint32_t)vc);
        }
    if (IS_RAM(menu)) row3_restore(c, M);
    int cur_was[4];
    for (int p = 0; p < 4; p++) cur_was[p] = -2;
    if (IS_RAM(menu)) {
        sel_undo(c, M, menu);
        for (int p = 0; p < 4; p++) {
            if (s_anm_cursor[p] < 0) continue;
            cur_was[p] = (int)ld32(c, M, menu + 0x1c * (uint32_t)p + 0x212c);
            mst(c, M, menu + 0x1c * (uint32_t)p + 0x212c, 4, (uint32_t)s_anm_cursor[p]);
        }
    }
    ctx_call_orig(c, g_native_addr[NAT_characterAnm__9SceneMenuFv]);
    for (int p = 0; p < 4; p++)
        if (cur_was[p] != -2) mst(c, M, menu + 0x1c * (uint32_t)p + 0x212c, 4, (uint32_t)cur_was[p]);
    for (int i = 0; i < n; i++) mst(c, M, at[i], 4, (uint32_t)was[i]);
    if (teams) sel_marks(c, M, menu, teams), row3_look(c, M, menu);
    vp_place(c, M, menu);
    for (uint32_t q = 0; s_r3_ok && q < 5; q++) park_up(c, M, ld32(c, M, menu + 0x2000 + 4 * q), 0);
    uint32_t cs = IS_RAM(menu) ? ld32(c, M, menu + 0x2014) : 0;
    if (!IS_RAM(cs)) return 0;
    bool wrote[4][2] = {{false}};
    uint32_t game[4][2] = {{0}};
    for (uint32_t t = 0; t < teams && t < 4; t++)
        for (uint32_t k = 0; k < 2; k++) {
            uint32_t want = s_sel_want[t][k];
            if (!want || want == s_sel_from[t][k]) continue;
            /* a cursor's preview may be on the team's other driver: only one not chosen, showing the
             * game's choice for that cursor */
            for (uint32_t j = 0; j < 2; j++) {
                uint32_t kk = j ? !k : k, e = cs + 0xc08 + 0x70 * t + 0x38 * kk;
                int v;
                if (j && (!s_sel_cursor[t][k] || charpack_slot((int)t, (int)kk, &v) >= 0)) break;
                if (!mld(c, M, e, 1) || wrote[t][kk]) continue;
                uint32_t id = ld32(c, M, e + 4);
                if (id != s_sel_from[t][k] && (j || id != s_sel_wrote[t][kk])) continue;
                game[t][kk] = id == s_sel_wrote[t][kk] ? s_sel_from[t][k] : id;
                if (id != want) mst(c, M, e + 4, 4, want);
                s_sel_wrote[t][kk] = want, wrote[t][kk] = true;
                break;
            }
        }
    /* never one block in two previews (one model drawn twice loops J3D's draw lists): a block named
     * twice goes back to the game's choice where it was ours */
    for (uint32_t t = 0; t < teams && t < 4; t++)
        for (uint32_t k = 0; k < 2; k++) {
            uint32_t e = cs + 0xc08 + 0x70 * t + 0x38 * k;
            if (!wrote[t][k] || !mld(c, M, e, 1)) continue;
            uint32_t id = ld32(c, M, e + 4);
            for (uint32_t u = 0; u < teams && u < 4; u++)
                for (uint32_t j = 0; j < 2; j++) {
                    uint32_t f = cs + 0xc08 + 0x70 * u + 0x38 * j;
                    if ((u != t || j != k) && mld(c, M, f, 1) && ld32(c, M, f + 4) == id && game[t][k]) {
                        rt_log("charpack: preview %u/%u would show block %u twice: the game's %u", t + 1, k + 1, id, game[t][k]);
                        mst(c, M, e + 4, 4, game[t][k]), s_sel_wrote[t][k] = 0, wrote[t][k] = false;
                        id = 0;
                    }
                }
        }
    {
        static uint32_t shown_was[4][2];
        for (uint32_t t = 0; t < teams && t < 4; t++)
            for (uint32_t k = 0; k < 2; k++) {
                uint32_t e = cs + 0xc08 + 0x70 * t + 0x38 * k, id = mld(c, M, e, 1) ? ld32(c, M, e + 4) : 0;
                if (id != shown_was[t][k])
                    rt_log("charpack: preview %u/%u shows block %u (the game's %u, wanted %u)", t + 1, k + 1, id, game[t][k], s_sel_want[t][k]);
                shown_was[t][k] = id;
            }
    }
    /* ... and any block still named twice (the game's own: a pick of a base whose block another
     * driver shows, on the frame it is made): the later one a block nobody shows, for this frame */
    for (uint32_t i = 0; i < teams * 2 && i < 8; i++) {
        uint32_t e = cs + 0xc08 + 0x70 * (i / 2) + 0x38 * (i % 2), id = ld32(c, M, e + 4);
        if (!mld(c, M, e, 1) || id < 1 || id > 20) continue;
        bool dup = false, shown[21] = {false};
        for (uint32_t j = 0; j < teams * 2 && j < 8; j++) {
            uint32_t f = cs + 0xc08 + 0x70 * (j / 2) + 0x38 * (j % 2), fid = ld32(c, M, f + 4);
            if (j != i && mld(c, M, f, 1) && fid >= 1 && fid <= 20) shown[fid] = true, dup |= j < i && fid == id;
        }
        if (!dup) continue;
        uint32_t best = 0;
        float d = 1e9f;
        for (uint32_t b2 = 1; b2 <= 16; b2++) { /* 17-20 have models of their own */
            float db = fabsf(SEL_SCALE[b2 - 1] - SEL_SCALE[id - 1]);
            if (!shown[b2] && db < d) best = b2, d = db;
        }
        rt_log("charpack: preview %u/%u shows block %u twice: block %u this frame", i / 2 + 1, i % 2 + 1, id, best);
        if (best) mst(c, M, e + 4, 4, best), s_sel_wrote[i / 2][i % 2] = 0;
    }
    return 0;
}

/* The select screen's 3D models of variants. CharacterSelect3D (SceneMenu +0x2014) builds one
 * model per character when it is made: blocks of 0x30 from +0x28 by ECharID - 1, +0 the
 * J3DModelData, +4 its ExModel, then the animations (+0x30.. bca and their J3DMtxCalcs, +0x54 the
 * btp), which calcCharacter puts on the model data's root joint each frame and draw reads the
 * ExModel from there. A variant previewed or chosen (charpack_model) gets a model built the same
 * way from its driver.bmd, in a heap of our own (JKRExpHeap in the pool, a child of the root heap),
 * and its block's model data and ExModel are its own while it is: the base's animations move it
 * (same skeleton: the joint counts must match). A variant with another character's skeleton
 * (charpack_anim: Mario's on Bowser Jr.) moves with that character's: its block's animations
 * (+8..+0x2c, the bcas, their calcs and the btp) are that character's while it shows (calcCharacter
 * sets their frames and calculates the model at once, so two blocks can share them). Its face follows
 * the block's btp (gut.btp of selectAnm.arc, the chosen pose's face) as the constructor binds it to
 * the base's: a J3DMaterialAnm on each material, then the btp searched against the model's materials
 * and entered in its table. The btp is the screen's: bound again whenever it is another one.
 * Toadette's hair is a model of its own (+0x3e8 its J3DModelData, +0x3ec its ExModel; calcCharacter
 * puts the hair's animations on it the same way): a variant with a kinoko_hair.bmd gets that one.
 * A block can show a variant of another character (charpack_borrow: a second player previewing a
 * variant of the same character): its model and its character's animations. */
#define SEL_HEAP_SIZE (3u << 20)
#define SEL_MAX 64
#define SEL_HAIR_CELL 9 /* Toadette (charpack.c BASES) */
typedef struct {
    int v;
    bool hair;     /* her kinoko_hair.bmd, else its driver.bmd */
    uint32_t md, ex, solid;
    uint32_t btp;  /* the btp its materials follow now, 0 none */
    int ntex;      /* its TEX1 count */
    bool anm;      /* its materials have their J3DMaterialAnm */
    int pose;      /* its own select screen poses: 0 not looked for, 1 in pose_w, -1 none (sel_poses) */
    uint32_t pose_w[10];
} SelModel;
static SelModel s_sel[SEL_MAX];
static int s_nsel;
static uint32_t s_sel_heap, s_sel_base_md[CP_BASES], s_sel_base_ex[CP_BASES], s_sel_base_anm[CP_BASES][10];
static uint32_t s_sel_hair_md, s_sel_hair_ex; /* the game's hair model */
static bool s_sel_bad[2][CP_VARIANTS], s_sel_off;

uint32_t sel_call(Ctx *c, uint32_t fn, uint32_t a, uint32_t b, uint32_t c3, uint32_t d, uint32_t e) {
    const uint32_t args[5] = {a, b, c3, d, e};
    return ctx_call_guest(c, fn, args, 5);
}
/* the address of a .bmd's section (INF1, JNT1, TEX1), 0 none */
static uint32_t bmd_section(Ctx *c, uint8_t *M, uint32_t bmd, uint32_t size, uint32_t want) {
    if (!IS_RAM(bmd) || size < 0x28 || ld32(c, M, bmd) != 0x4A334432u /* J3D2 */) return 0;
    for (uint32_t o = 0x20; o + 12 <= size;) {
        uint32_t magic = ld32(c, M, bmd + o), n = ld32(c, M, bmd + o + 4);
        if (magic == want) return bmd + o;
        if (n < 8) break;
        o += n;
    }
    return 0;
}
/* the count of a .bmd's section (JNT1 joints, TEX1 textures), -1 none */
static int bmd_count(Ctx *c, uint8_t *M, uint32_t bmd, uint32_t size, uint32_t want) {
    uint32_t s = bmd_section(c, M, bmd, size, want);
    return s ? (int)mld(c, M, s + 8, 2) : -1;
}
static bool sel_ours(uint32_t md) {
    for (int i = 0; i < s_nsel; i++)
        if (s_sel[i].md == md) return true;
    return false;
}
/* back to the game's own models, then everything built so far gone (the heap full) */
static void sel_wipe(Ctx *c, uint8_t *M, uint32_t cs) {
    for (int b = 0; b < CP_BASES && IS_RAM(cs); b++) {
        uint32_t slot = cs + 0x28 + 0x30 * (CP_CHAR_ID[b] - 1);
        if (!s_sel_base_md[b] || !sel_ours(ld32(c, M, slot))) continue;
        mst(c, M, slot, 4, s_sel_base_md[b]), mst(c, M, slot + 4, 4, s_sel_base_ex[b]);
        for (int k = 0; k < 10; k++) mst(c, M, slot + 8 + 4 * k, 4, s_sel_base_anm[b][k]);
    }
    if (IS_RAM(cs) && s_sel_hair_md && sel_ours(ld32(c, M, cs + 0x3e8)))
        mst(c, M, cs + 0x3e8, 4, s_sel_hair_md), mst(c, M, cs + 0x3ec, 4, s_sel_hair_ex);
    for (int i = 0; i < s_nsel; i++)
        if (s_sel[i].solid) sel_call(c, 0x80014B38u, s_sel[i].solid, 0, 0, 0, 0); /* JKRHeap::destroy */
    if (IS_RAM(s_sel_heap)) sel_call(c, 0x80014CB8u, s_sel_heap, 0, 0, 0, 0); /* JKRHeap::freeAll */
    s_nsel = 0;
}
/* a .bmd in host memory: its joint count (-1 not one) and INF1 flags */
static int host_bmd_info(const uint8_t *b, uint32_t n, uint32_t *inf) {
    int joints = -1;
    if (!b || n < 0x28 || memcmp(b, "J3D2", 4)) return -1;
    for (uint32_t o = 0x20; o + 12 <= n;) {
        uint32_t size = (uint32_t)b[o + 4] << 24 | (uint32_t)b[o + 5] << 16 | (uint32_t)b[o + 6] << 8 | b[o + 7];
        if (!memcmp(b + o, "JNT1", 4)) joints = b[o + 8] << 8 | b[o + 9];
        if (!memcmp(b + o, "INF1", 4)) *inf = (uint32_t)(b[o + 8] << 8 | b[o + 9]);
        if (size < 8) break;
        o += size;
    }
    return joints;
}
/* variant v's model (its driver.bmd, or with hair its kinoko_hair.bmd) for a block of the select
 * screen (built now if need be), NULL when it can't have one */
static SelModel *sel_model(Ctx *c, uint8_t *M, uint32_t cs, int v, bool hair) {
    for (int i = 0; i < s_nsel; i++)
        if (s_sel[i].v == v && s_sel[i].hair == hair) return &s_sel[i];
    if (s_sel_off || v < 0 || v >= CP_VARIANTS || s_sel_bad[hair][v]) return NULL;
    uint32_t r13 = (uint32_t)RD(c, REG_R13, 4);
    if (!s_sel_heap) {
        uint32_t mem = charpack_reserve(SEL_HEAP_SIZE), root = ld32(c, M, r13 - 0x67b0);
        s_sel_heap = mem && IS_RAM(root) ? sel_call(c, 0x8001287Cu, mem, SEL_HEAP_SIZE, root, 0, 0) : 0; /* JKRExpHeap::create */
        rt_log("charpack: select screen models' heap %#x (%u KiB at %#x)", s_sel_heap, SEL_HEAP_SIZE >> 10, mem);
        if (!IS_RAM(s_sel_heap)) { s_sel_off = true; return NULL; }
    }
    uint32_t size, bmd = charpack_file(v, hair ? "kinoko_hair.bmd" : "driver.bmd", &size);
    int vb = charpack_variant_base(v);
    int sk = charpack_anim(v) >= 0 ? charpack_anim(v) : vb; /* the character whose animations move it */
    /* the model it stands in for: the character's driver.bmd (in MRAM), or the game's hair (the disc's) */
    uint32_t binf_flags = 0;
    int bj = -1;
    if (hair) {
        static int hair_joints = -2;
        static uint32_t hair_inf;
        if (hair_joints == -2) {
            uint32_t n = 0;
            uint8_t *h = charpack_orig(SEL_HAIR_CELL, "kinoko_hair.bmd", &n);
            hair_joints = host_bmd_info(h, n, &hair_inf);
            free(h);
        }
        bj = hair_joints, binf_flags = hair_inf;
    } else {
        uint32_t base_bmd = sel_call(c, 0x801CF0ACu, 0, CP_CHAR_ID[sk] - 1, 0, 0, 0); /* ResMgr::getPtr(mcDriverDataBmd, driver) */
        bj = bmd_count(c, M, base_bmd, 1u << 20, 0x4A4E5431u /* JNT1 */);
        uint32_t binf = bmd_section(c, M, base_bmd, 1u << 20, 0x494E4631u /* INF1 */);
        binf_flags = binf ? mld(c, M, binf + 8, 2) : 0;
    }
    int vj = bmd_count(c, M, bmd, size, 0x4A4E5431u);
    if (!bmd || vj < 0 || vj != bj) {
        s_sel_bad[hair][v] = true;
        if (bmd || !hair)
            rt_log("charpack: %s keeps %s's select screen %s (its joints %d, %s's %d)", charpack_name(v), charpack_base_name(vb),
                   hair ? "hair" : "model", vj, charpack_base_name(hair ? SEL_HAIR_CELL : sk), bj);
        return NULL;
    }
    /* room for it: a model takes about twice its file (the display lists, matrices); else start over */
    uint32_t need = 2 * size + (128u << 10);
    if (sel_call(c, 0x80014D3Cu, s_sel_heap, 0, 0, 0, 0) < need || s_nsel == SEL_MAX) sel_wipe(c, M, cs);
    if (sel_call(c, 0x80014D3Cu, s_sel_heap, 0, 0, 0, 0) < need) { s_sel_bad[hair][v] = true; return NULL; }
    /* as CharacterSelect3D's constructor builds each character's */
    uint32_t prev = sel_call(c, 0x80014B28u, s_sel_heap, 0, 0, 0, 0); /* becomeCurrentHeap */
    /* loaded as the base's joints calculate (INF1's low 4 bits: 1 Softimage, 2 Maya, the game's):
     * the block's J3DMtxCalcs are made for the base's model, and with another kind the screen's
     * scale is lost (a model exported as Softimage shows at about half size in the back place,
     * scale 1.85). Only for this load: the race's model is the file's own. */
    uint32_t inf = bmd_section(c, M, bmd, size, 0x494E4631u /* INF1 */);
    uint32_t flags = inf ? mld(c, M, inf + 8, 2) : 0, want = binf_flags ? (flags & ~0xFu) | (binf_flags & 0xFu) : flags;
    if (inf && want != flags) mst(c, M, inf + 8, 2, want);
    uint32_t md = sel_call(c, 0x80060588u, bmd, 0x11020030u, 0, 0, 0); /* J3DModelLoaderDataBase::load */
    if (inf && want != flags) {
        mst(c, M, inf + 8, 2, flags);
        rt_log("charpack: %s's select screen %s calculates its joints as %s's (INF1 %#x, the file's %#x)", charpack_name(v),
               hair ? "hair" : "model", charpack_base_name(hair ? SEL_HAIR_CELL : sk), want, flags);
    }
    uint32_t ex = 0, solid = 0;
    if (IS_RAM(md)) {
        sel_call(c, 0x8018244Cu, md, 0, 0, 0, 0); /* ExModel::disableTexLOD */
        uint32_t n = mld(c, M, md + 0x7c, 2), shapes = ld32(c, M, md + 0x80);
        for (uint32_t i = 0; i < n && IS_RAM(shapes); i++) {
            uint32_t sh = ld32(c, M, shapes + 4 * i);
            if (IS_RAM(sh)) mst(c, M, sh + 0xc, 4, (ld32(c, M, sh + 0xc) & ~0xF000u) | 0x2000u);
        }
        sel_call(c, 0x80074A58u, md, 0x40000, 0, 0, 0); /* J3DModelData::newSharedDisplayList */
        ex = sel_call(c, 0x8001563Cu, 0x8c, s_sel_heap, 0, 0, 0); /* operator new(size, heap, align) */
    }
    if (IS_RAM(ex)) {
        sel_call(c, 0x80182164u, ex, 1, 0, 0, 0); /* ExModel::ExModel(1) */
        sel_call(c, 0x80182298u, ex, md, 0, 0, 0); /* ExModel::setModelData */
        solid = sel_call(c, 0x80015F40u, sel_call(c, 0x80014D3Cu, s_sel_heap, 0, 0, 0, 0), s_sel_heap, 0, 0, 0); /* JKRSolidHeap::create(free size) */
    }
    if (IS_RAM(solid)) {
        uint32_t keep = sel_call(c, 0x80014B28u, solid, 0, 0, 0, 0);
        sel_call(c, ld32(c, M, ld32(c, M, ex) + 0xc), ex, solid, 1, 0x01080202u, 1); /* ExModel::createModel */
        sel_call(c, 0x80182A2Cu, ex, 0, 0, 0, 0); /* ExModel::makeSharedDL */
        sel_call(c, 0x80182AB4u, ex, 0, 0, 0, 0); /* ExModel::lock */
        sel_call(c, 0x80014B28u, keep, 0, 0, 0, 0);
        sel_call(c, 0x80016108u, solid, 0, 0, 0, 0); /* JKRSolidHeap::adjustSize */
    }
    sel_call(c, 0x80014B28u, prev, 0, 0, 0, 0);
    bool ok = IS_RAM(solid) && IS_RAM(ld32(c, M, ex + 0xc)) && IS_RAM(ld32(c, M, ld32(c, M, ex + 0xc)));
    rt_log("charpack: %s's select screen %s %s (md %#x, heap %u KiB free)", charpack_name(v), hair ? "hair" : "model",
           ok ? "built" : "failed", md, sel_call(c, 0x80014D3Cu, s_sel_heap, 0, 0, 0, 0) >> 10);
    if (!ok) {
        s_sel_bad[hair][v] = true;
        if (IS_RAM(solid)) sel_call(c, 0x80014B38u, solid, 0, 0, 0, 0);
        return NULL;
    }
    s_sel[s_nsel] = (SelModel){v, hair, md, ex, solid, 0, bmd_count(c, M, bmd, size, 0x54455831u /* TEX1 */), false, 0, {0}};
    return &s_sel[s_nsel++];
}
/* A variant's own select screen poses (selectanm/: enter.bck, wait.bck, gut.bck, gut.btp, as the
 * MKDD Patcher's selectAnm.arc has them): a block's animations (+8, 10 words) are enter (0), its
 * J3DMtxCalc (1), wait (2) and calc (3), gut (4) and calc (5), three more calcs of those (6..8, the
 * ones blending them) and gut's btp (9); a calc's J3DAnmTransform at +4. Each file of its own is
 * loaded as the constructor loads the game's (J3DAnmLoaderDataBase::load) in the select screen's
 * heap, and the calcs copied pointing at its poses instead of the base's (the base's words a, for
 * a pose it has none of). Only a pose for as many joints as its model. */
#define POSE_CALC 0x1c  /* calcs 1, 3, 5: their size (the game's laid one after another) */
#define POSE_BLEND 0x38 /* calcs 6..8 */
static void sel_poses(Ctx *c, uint8_t *M, SelModel *m, const uint32_t base[10]) {
    if (m->pose || m->hair) { if (!m->pose) m->pose = -1; return; }
    m->pose = -1;
    static const char *const FILES[4] = {"enter.bck", "wait.bck", "gut.bck", "gut.btp"};
    static const int AT[4] = {0, 2, 4, 9};
    uint32_t own[4] = {0}, bsize, bmd = charpack_file(m->v, "driver.bmd", &bsize);
    int joints = bmd_count(c, M, bmd, bsize, 0x4A4E5431u /* JNT1 */), n = 0;
    uint32_t prev = sel_call(c, 0x80014B28u, s_sel_heap, 0, 0, 0, 0); /* becomeCurrentHeap */
    for (int i = 0; i < 4; i++) {
        uint32_t size, f = charpack_select_file(m->v, FILES[i], &size);
        if (!f || size < 0x40 || !IS_RAM(base[AT[i]])) continue;
        /* J3D1bck1 / J3D1btp1, then ANK1 (+0xc its joints) / TPT1 */
        bool bck = i < 3, ok = ld32(c, M, f + 4) == (bck ? 0x62636B31u : 0x62747031u) && ld32(c, M, f + 0x20) == (bck ? 0x414E4B31u : 0x54505431u);
        if (ok && bck && (int)mld(c, M, f + 0x2c, 2) != joints) {
            rt_log("charpack: %s's select screen %s is for %u joints, its model has %d: the base's", charpack_name(m->v), FILES[i], (unsigned)mld(c, M, f + 0x2c, 2), joints);
            continue;
        }
        uint32_t a = ok ? sel_call(c, 0x80063154u, f, 0, 0, 0, 0) : 0; /* J3DAnmLoaderDataBase::load */
        if (IS_RAM(a) && ld32(c, M, a) == ld32(c, M, base[AT[i]])) own[i] = a, n++;
        else rt_log("charpack: %s's select screen %s not loaded (%#x)", charpack_name(m->v), FILES[i], a);
    }
    for (int k = 0; k < 10; k++) m->pose_w[k] = base[k];
    for (int i = 0; i < 4; i++)
        if (own[i]) m->pose_w[AT[i]] = own[i];
    static const int CALCS[6] = {1, 3, 5, 6, 7, 8};
    for (int j = 0; n && (own[0] || own[1] || own[2]) && j < 6; j++) {
        uint32_t from = base[CALCS[j]], size = CALCS[j] < 6 ? POSE_CALC : POSE_BLEND;
        uint32_t to = IS_RAM(from) ? sel_call(c, 0x8001563Cu, size, s_sel_heap, 0, 0, 0) : 0; /* operator new */
        if (!IS_RAM(to)) { n = 0; break; }
        memcpy(M + (to & RAM_MASK), M + (from & RAM_MASK), size);
        for (uint32_t o = 4; o < size; o += 4)
            for (int i = 0; i < 3; i++)
                if (own[i] && ld32(c, M, to + o) == base[AT[i]]) mst(c, M, to + o, 4, own[i]);
        m->pose_w[CALCS[j]] = to;
    }
    sel_call(c, 0x80014B28u, prev, 0, 0, 0, 0);
    if (n) m->pose = 1, rt_log("charpack: %s's select screen poses: %d of its own", charpack_name(m->v), n);
}
/* m's materials follow btp (the block's J3DAnmTexPattern), as the constructor does for the base's;
 * not when the btp names a texture m doesn't have */
static void sel_bind(Ctx *c, uint8_t *M, SelModel *m, uint32_t btp) {
    if (!IS_RAM(btp) || m->btp == btp) return;
    m->btp = btp;
    uint32_t n = mld(c, M, btp + 0x14, 2), idx = ld32(c, M, btp + 0xc), top = 0;
    for (uint32_t i = 0; i < n && i < 1024 && IS_RAM(idx); i++) {
        uint32_t t = mld(c, M, idx + 2 * i, 2);
        if (t > top) top = t;
    }
    if (!IS_RAM(idx) || n > 1024 || (int)top >= m->ntex) {
        rt_log("charpack: %s's select screen face doesn't follow the btp (texture %u of %d)", charpack_name(m->v), top, m->ntex);
        return;
    }
    uint32_t nmat = mld(c, M, m->md + 0x5c, 2), mats = ld32(c, M, m->md + 0x60);
    if (!m->anm) {
        for (uint32_t i = 0; i < nmat && i < 256 && IS_RAM(mats); i++) {
            uint32_t mat = ld32(c, M, mats + 4 * i);
            uint32_t a = IS_RAM(mat) ? sel_call(c, 0x8001563Cu, 0xf4, s_sel_heap, 0, 0, 0) : 0; /* operator new */
            if (!IS_RAM(a)) return; /* m->anm stays false: nothing entered */
            mst(c, M, a, 4, 0x8033FC30u); /* J3DMaterialAnm's vtable, then its arrays as its constructor makes them */
            static const uint32_t arr[5][5] = {{0x4, 0x80074EC0u, 0x8005B164u, 8, 2}, {0x14, 0x80074EFCu, 0x8005B1A8u, 8, 8},
                                               {0x54, 0x80074ED8u, 0x8005B1ECu, 0xc, 8}, {0xb4, 0x80074F2Cu, 0x8005B23Cu, 8, 4},
                                               {0xd4, 0x80074F14u, 0x8005B280u, 8, 4}};
            for (int k = 0; k < 5; k++) sel_call(c, 0x800FF1ECu, a + arr[k][0], arr[k][1], arr[k][2], arr[k][3], arr[k][4]); /* __construct_array */
            sel_call(c, 0x8005AC84u, a, 0, 0, 0, 0); /* J3DMaterialAnm::initialize */
            sel_call(c, ld32(c, M, ld32(c, M, mat) + 0x2c), mat, 0, 0, 0, 0); /* J3DMaterial::change */
            mst(c, M, mat + 0x3c, 4, a);
        }
        m->anm = true;
    }
    sel_call(c, 0x80059990u, btp, m->md, 0, 0, 0); /* J3DAnmTexPattern::searchUpdateMaterialID */
    /* (the IDs searched are only read here: the base's materials keep what they were entered with) */
    sel_call(c, 0x80075360u, m->md + 0x58, btp, 0, 0, 0); /* J3DMaterialTable::entryTexNoAnimator */
}
/* each frame of the select screen, after the game's: each character's block shows the model of
 * the variant standing in for it, else the game's own */
static void sel_models(Ctx *c, uint8_t *M, uint32_t menu) {
    uint32_t cs = IS_RAM(menu) ? ld32(c, M, menu + 0x2014) : 0;
    if (!IS_RAM(cs) || !charpack_count()) return;
    uint32_t lr = (uint32_t)RD(c, 0x1020, 4);
    /* the game's own models and animations, kept when a screen is new (the variants' first) */
    for (int b = 0; b < CP_BASES; b++) {
        uint32_t slot = cs + 0x28 + 0x30 * (CP_CHAR_ID[b] - 1), cur = ld32(c, M, slot);
        if (!IS_RAM(cur) || cur == s_sel_base_md[b] || sel_ours(cur)) continue;
        s_sel_base_md[b] = cur, s_sel_base_ex[b] = ld32(c, M, slot + 4);
        for (int k = 0; k < 10; k++) s_sel_base_anm[b][k] = ld32(c, M, slot + 8 + 4 * k);
        for (int i = 0; i < s_nsel; i++) s_sel[i].btp = 0; /* its own btps */
    }
    uint32_t hmd = ld32(c, M, cs + 0x3e8);
    if (IS_RAM(hmd) && hmd != s_sel_hair_md && !sel_ours(hmd)) s_sel_hair_md = hmd, s_sel_hair_ex = ld32(c, M, cs + 0x3ec);
    SelModel *hair = NULL;
    for (int b = 0; b < CP_BASES; b++) {
        uint32_t slot = cs + 0x28 + 0x30 * (CP_CHAR_ID[b] - 1);
        if (!IS_RAM(ld32(c, M, slot)) || !s_sel_base_md[b]) continue;
        int v = charpack_model(b), vb = charpack_variant_base(v);
        SelModel *m = v >= 0 && vb >= 0 && s_sel_base_md[vb] ? sel_model(c, M, cs, v, false) : NULL;
        int a = !m ? b : charpack_anim(v) >= 0 && s_sel_base_md[charpack_anim(v)] ? charpack_anim(v) : vb;
        uint32_t md = m ? m->md : s_sel_base_md[b], ex = m ? m->ex : s_sel_base_ex[b];
        if (ld32(c, M, slot) != md) mst(c, M, slot, 4, md), mst(c, M, slot + 4, 4, ex);
        if (m) sel_poses(c, M, m, s_sel_base_anm[a]);
        const uint32_t *w = m && m->pose > 0 ? m->pose_w : s_sel_base_anm[a];
        for (int k = 0; k < 10; k++)
            if (ld32(c, M, slot + 8 + 4 * k) != w[k]) mst(c, M, slot + 8 + 4 * k, 4, w[k]);
        if (m) sel_bind(c, M, m, w[9]);
        if (m && b == SEL_HAIR_CELL && vb == SEL_HAIR_CELL) hair = sel_model(c, M, cs, v, true);
    }
    if (s_sel_hair_md) {
        uint32_t md = hair ? hair->md : s_sel_hair_md, ex = hair ? hair->ex : s_sel_hair_ex;
        if (ld32(c, M, cs + 0x3e8) != md) mst(c, M, cs + 0x3e8, 4, md), mst(c, M, cs + 0x3ec, 4, ex);
    }
    WR(c, 0x1020, 4, lr);
}
/* a cell with no character on this page (a page past the first with no variant there): its
 * picture the empty tile (charpack_art.c), the cursors skip it and A does nothing there */
static bool sel_empty(int cell) {
    return (charpack_page() > 0 || cell >= CP_BASES) && cell >= 0 && cell < CP_CELLS && charpack_cell_variant(cell) < 0;
}
static void sel_flags_empty(Ctx *c, uint8_t *M, uint32_t menu) {
    for (int i = 0; i < CP_BASES; i++)
        if (sel_empty(i)) mst(c, M, menu + 0x228c + i, 1, 0);
}
/* each chosen cell (the menu's own, read now) and each cell a pick shows in: taken only where its
 * very pick is (the game's setCharacterFlag made them all taken); the empty cells taken */
static void sel_flags_fix(Ctx *c, uint8_t *M, uint32_t menu) {
    if (!IS_RAM(menu) || !charpack_count() || !charpack_select_active()) return;
    uint32_t teams = ld32(c, M, menu + 0x20fc) + 1;
    bool mine[CP_BASES] = {false};
    for (uint32_t t = 0; t < teams && t < 4; t++)
        for (uint32_t k = 0; k < 2; k++) {
            int cell = (int)ld32(c, M, menu + 0x1c * t + 0x2138 + 4 * k), vc = charpack_slot_shown((int)t, (int)k);
            if (cell >= 0 && cell < CP_BASES) mine[cell] = true;
            if (vc >= 0) mine[vc] = true;
        }
    for (int i = 0; i < CP_BASES; i++)
        if (mine[i]) mst(c, M, menu + 0x228c + i, 1, !charpack_cell_taken(i));
    sel_flags_empty(c, M, menu);
}
/* SceneMenu::setCharacterFlag, also from moveCursor on each cursor move: the game's flags, then the
 * picks' (sel_flags_fix), or the cursors would skip a chosen character's cell on every page */
uint64_t i_setCharacterFlag__9SceneMenuFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    ctx_call_orig(c, g_native_addr[NAT_setCharacterFlag__9SceneMenuFv]);
    sel_flags_fix(c, M, (uint32_t)a0);
    return 0;
}
/* the menu's flags as the picks have them (see SceneMenu::character): the game's (locked
 * characters, chosen cells) made again when the picks or the page changed, then each chosen cell, and
 * each cell a pick shows in, taken only where its very pick is */
static void sel_flags(Ctx *c, uint8_t *M, uint32_t menu, const int cells[4][2]) {
    uint32_t sig = (uint32_t)charpack_page() * 2654435761u;
    for (int t = 0; t < 4; t++)
        for (int k = 0; k < 2; k++) {
            int v, cell = charpack_slot(t, k, &v);
            sig = (sig ^ (uint32_t)(cell + 1) ^ (uint32_t)(v + 2) << 8 ^ (uint32_t)(cells[t][k] + 1) << 20) * 16777619u;
        }
    if (sig != s_sel_flags_sig) {
        uint32_t lr = (uint32_t)RD(c, 0x1020, 4);
        sel_call(c, SEL_SET_FLAGS, menu, 0, 0, 0, 0);
        WR(c, 0x1020, 4, lr);
        s_sel_flags_sig = sig;
    }
    bool mine[CP_BASES] = {false};
    for (int t = 0; t < 4; t++)
        for (int k = 0; k < 2; k++) {
            if (cells[t][k] >= 0 && cells[t][k] < CP_BASES) mine[cells[t][k]] = true;
            int vc = charpack_slot_shown(t, k);
            if (vc >= 0) mine[vc] = true;
        }
    for (int i = 0; i < CP_BASES; i++)
        if (mine[i]) mst(c, M, menu + 0x228c + i, 1, !charpack_cell_taken(i));
    sel_flags_empty(c, M, menu);
}
/* The select screen's third row (prototype). Each of the grid's 10 columns is a J2DScreen made from
 * menu.arc's select_character_cc.blo: Cha1 (the top cell) and Cha2, each a pane with its box (CPB),
 * frame (CW), face (CP), player colour (CC) and glow (CWC). Before SceneMenu's constructor
 * (FUN_8015639c, r4 its JKRArchive) builds them, the archive's entry for that file (+0x4c file
 * entries of 0x14: +4 flags and name offset, +0xc size, +0x10 the data once fetched; +0x44 info, +8
 * their count; +0x54 names) is pointed at a copy with a third cell, Cha3: Cha1's blocks again, the
 * tags ending in 3. Nothing of the disc's is changed or kept. */
static uint32_t be32p(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void wbe32p(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }
static bool blo_pane(const uint8_t *b) {
    return !memcmp(b, "PAN2", 4) || !memcmp(b, "PIC2", 4) || !memcmp(b, "WIN2", 4) || !memcmp(b, "TBX2", 4);
}
/* a pane block's pane info (J2DPane's stream: +0xa animation index, +0x10 tag, +0x3c / +0x40 x, y) */
static uint8_t *blo_info(uint8_t *b) { return memcmp(b, "PAN2", 4) ? b + 8 : b; }
static uint16_t be16p(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void wbe16p(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; }
/* the material indices a pane block uses (PIC2: its J2DPictureEx's; WIN2: the frame's four and the
 * contents'), as places in the block */
static unsigned blo_mats(uint8_t *b, uint8_t **at) {
    uint32_t sz = be32p(b + 4);
    if (!memcmp(b, "PIC2", 4) && sz >= 0x48 + 8 + 6) return at[0] = b + 8 + 0x48 + 4, 1;
    if (!memcmp(b, "WIN2", 4) && sz >= 0x48 + 8 + 0x26) {
        uint8_t *e = b + 8 + 0x48;
        for (int i = 0; i < 4; i++) at[i] = e + 0x10 + 2 * i;
        return at[4] = e + 0x24, 5;
    }
    return 0;
}
/* JUTNameTab's key for a name */
static unsigned ntab_key(const char *s) {
    unsigned k = 0;
    while (*s) k = ((unsigned)(uint8_t)*s++ + k * 3) & 0xFFFF;
    return k;
}
/* MAT1 with the materials old[0..m) again after its n (material n + k is old[k]'s copy, named
 * "<name>3"): its init data (0xe8 each), index table and name table grow, the rest moves along */
#define MAT_INIT 0xe8
#define MAT_OFFS 23 /* J2DMaterialBlock's offsets, +0xc to +0x64 */
static uint32_t mat1_grow(const uint8_t *src, uint8_t *dst, uint32_t cap, const unsigned *old, unsigned m) {
    uint32_t sz = be32p(src + 4), off[MAT_OFFS];
    unsigned n = be16p(src + 8);
    if (sz < 0xc + 4 * MAT_OFFS) return 0;
    for (int i = 0; i < MAT_OFFS; i++) off[i] = be32p(src + 0xc + 4 * i);
    uint32_t rest = sz;
    for (int i = 3; i < MAT_OFFS; i++)
        if (off[i] && off[i] < rest) rest = off[i];
    if (off[1] - off[0] != n * MAT_INIT || off[2] < off[1] + 2 * n || rest < off[2] || rest > sz) return 0;
    const uint8_t *nt = src + off[2];
    if (be16p(nt) != n) return 0;
    uint32_t init_end = off[0] + (n + m) * MAT_INIT, idx_end = init_end + ((2 * (n + m) + 3) & ~3u);
    /* the name table: count, pad, (key, offset) each, the names */
    char names[64][24];
    if (n + m > 64) return 0;
    for (unsigned i = 0; i < n; i++) {
        unsigned o = be16p(nt + 4 + 4 * i + 2);
        if (off[2] + o >= rest) return 0;
        snprintf(names[i], sizeof names[i], "%s", (const char *)nt + o);
    }
    for (unsigned k = 0; k < m; k++) { /* old[k] < n: never the copy itself */
        size_t l = strnlen(names[old[k]], 20);
        memcpy(names[n + k], names[old[k]], l), names[n + k][l] = '3', names[n + k][l + 1] = 0;
    }
    /* each copy follows the Cha2 material named as its Cha1 one with a 1 a 2, the last one that
     * names one (mat_CP1_v: mat_CP2_v; mat_CW1_1: mat_CW2_1), else that one */
    s_r3_nmat = n, s_r3_m = m;
    for (unsigned k = 0; k < m && k < 16; k++) {
        s_r3_src[k] = (int)old[k], s_r3_two[k] = false;
        for (int at = (int)strlen(names[old[k]]) - 1; at >= 0 && s_r3_src[k] == (int)old[k]; at--) {
            if (names[old[k]][at] != '1') continue;
            char want[24];
            snprintf(want, sizeof want, "%s", names[old[k]]);
            want[at] = '2';
            for (unsigned i = 0; i < n; i++)
                if (!strcmp(names[i], want)) { s_r3_src[k] = (int)i, s_r3_two[k] = true; break; }
        }
        rt_log("row3: material %s follows %s", names[old[k]], names[s_r3_src[k]]);
    }
    uint32_t nt_size = 4 + 4 * (n + m);
    for (unsigned i = 0; i < n + m; i++) nt_size += (uint32_t)strlen(names[i]) + 1;
    uint32_t nt_end = idx_end + ((nt_size + 3) & ~3u), total = nt_end + (sz - rest);
    if (total > cap) return 0;
    memset(dst, 0, total);
    memcpy(dst, src, off[0]);
    memcpy(dst + off[0], src + off[0], n * MAT_INIT);
    for (unsigned k = 0; k < m; k++) memcpy(dst + off[0] + (n + k) * MAT_INIT, src + off[0] + old[k] * MAT_INIT, MAT_INIT);
    for (unsigned i = 0; i < n; i++) wbe16p(dst + init_end + 2 * i, be16p(src + off[1] + 2 * i));
    for (unsigned k = 0; k < m; k++) wbe16p(dst + init_end + 2 * (n + k), n + k);
    uint8_t *t = dst + idx_end;
    wbe16p(t, n + m), wbe16p(t + 2, 0xFFFF);
    uint32_t so = 4 + 4 * (n + m);
    for (unsigned i = 0; i < n + m; i++) {
        wbe16p(t + 4 + 4 * i, ntab_key(names[i])), wbe16p(t + 6 + 4 * i, so);
        memcpy(t + so, names[i], strlen(names[i]) + 1), so += (uint32_t)strlen(names[i]) + 1;
    }
    memcpy(dst + nt_end, src + rest, sz - rest);
    wbe32p(dst + 4, total), wbe16p(dst + 8, n + m);
    wbe32p(dst + 0xc + 4, init_end), wbe32p(dst + 0xc + 8, idx_end);
    for (int i = 3; i < MAT_OFFS; i++)
        if (off[i]) wbe32p(dst + 0xc + 4 * i, off[i] - rest + nt_end);
    return total;
}
static uint32_t cc3_make(const uint8_t *src, uint32_t n, uint8_t *dst, uint32_t cap) {
    if (n < 0x20 || memcmp(src, "SCRNblo2", 8)) return 0;
    uint32_t i = 0x20, from = 0, to = 0, blocks = 0, mat = 0;
    int depth = 0, at = -1;
    while (i + 8 <= n) {
        uint32_t sz = be32p(src + i + 4);
        if (sz < 8 || i + sz > n || !memcmp(src + i, "EXT1", 4)) break;
        if (!memcmp(src + i, "MAT1", 4)) mat = i;
        if (!memcmp(src + i, "BGN1", 4)) depth++;
        else if (!memcmp(src + i, "END1", 4) && --depth == at) { to = i + sz, blocks++; break; }
        else if (at < 0 && !memcmp(src + i, "PAN2", 4) && !memcmp(src + i + 0x14, "Cha1", 4)) at = depth, from = i;
        if (at >= 0) blocks++;
        i += sz;
    }
    if (!to || !mat || mat > from) return 0;
    /* Cha1's blocks again (in a scratch copy first: their materials renumbered) */
    static uint8_t cl[0x1000];
    uint32_t cn = to - from;
    if (cn > sizeof cl) return 0;
    memcpy(cl, src + from, cn);
    unsigned old[16], m = 0, nmat = be16p(src + mat + 8);
    for (uint32_t j = 0; j < cn;) {
        uint8_t *b = cl + j, *ma[5];
        uint32_t sz = be32p(b + 4);
        if (blo_pane(b) && sz >= 0x48) {
            uint8_t *p = blo_info(b);
            if (p[0x17] == '1') p[0x17] = '3';
            if (p[0x15] == 'C' && p[0x16] == 'C' && p[0x17] == '3') /* its marker under it, as Cha2's */
                wbe32p(p + 0x3c, 0xc1a00000u /* -20 */), wbe32p(p + 0x40, 0x41d80000u /* 27 */);
            p[0xa] = p[0xb] = 0xFF; /* unanimated (J2DPane::updateTransform leaves it): a look of its own */
            for (unsigned q = 0, nm = blo_mats(b, ma); q < nm; q++) {
                unsigned mi = be16p(ma[q]), k = 0;
                if (mi >= nmat) continue;
                while (k < m && old[k] != mi) k++;
                if (k == m && m < 16) old[m++] = mi;
                if (k < m) wbe16p(ma[q], nmat + k);
            }
        }
        j += sz;
    }
    /* Cha3 a row below Cha2 (Cha1 at 1, Cha2 at 58) */
    wbe32p(cl + 0x3c, 0), wbe32p(cl + 0x40, 0x42e60000u /* 115.0f */);
    /* the file: up to MAT1, MAT1 grown, on to Cha1's end, Cha3, the rest */
    uint32_t o = mat;
    if (o > cap) return 0;
    memcpy(dst, src, mat);
    uint32_t mn = mat1_grow(src + mat, dst + o, cap - o, old, m);
    if (!mn) return 0;
    o += mn;
    uint32_t mid = to - (mat + be32p(src + mat + 4));
    if (o + mid + cn + (n - to) > cap) return 0;
    memcpy(dst + o, src + mat + be32p(src + mat + 4), mid), o += mid;
    memcpy(dst + o, cl, cn), o += cn;
    memcpy(dst + o, src + to, n - to), o += n - to;
    wbe32p(dst + 8, o);
    wbe32p(dst + 0xc, be32p(src + 0xc) + blocks);
    return o;
}
static uint32_t s_cc3, s_cc3_size;
static void cc3_patch(Ctx *c, uint8_t *M, uint32_t arc) {
    if (!charpack_count()) return; /* no third row without characters for it */
    uint32_t info = arc && IS_RAM(arc) ? ld32(c, M, arc + 0x44) : 0, files = IS_RAM(arc) ? ld32(c, M, arc + 0x4c) : 0, str = IS_RAM(arc) ? ld32(c, M, arc + 0x54) : 0;
    if (!IS_RAM(info) || !IS_RAM(files) || !IS_RAM(str)) return;
    uint32_t nf = ld32(c, M, info + 8);
    for (uint32_t i = 0; i < nf && i < 4096; i++) {
        uint32_t e = files + 0x14 * i, flag = ld32(c, M, e + 4), name = str + (flag & 0xFFFFFF);
        if (flag >> 24 & 2 || !IS_RAM(name) || !IS_RAM(name + 24)) continue;
        if (memcmp(M + (name & RAM_MASK), "select_character_cc.blo", 24)) continue;
        /* not fetched yet: in the archive's data (a JKRMemArchive's RARC is whole in memory, its
         * header 0x20 before the info: +0xc the data's offset from +0x20) */
        uint32_t data = ld32(c, M, e + 0x10), size = ld32(c, M, e + 0xc), hdr = info - 0x20;
        if (data == s_cc3 && s_cc3) return;
        if (!data && ld32(c, M, hdr) == 0x52415243u /* RARC */) data = hdr + 0x20 + ld32(c, M, hdr + 0xc) + ld32(c, M, e + 8);
        if (!data || !IS_RAM(data) || !IS_RAM(data + size - 1) || size > 0x4000) { rt_log("row3: select_character_cc.blo not in memory"); return; }
        if (!s_cc3) s_cc3 = charpack_reserve(0x8000);
        if (!s_cc3) return;
        s_cc3_size = cc3_make(M + (data & RAM_MASK), size, M + (s_cc3 & RAM_MASK), 0x8000);
        if (!s_cc3_size) { rt_log("row3: no Cha1 in it"); return; }
        mst(c, M, e + 0x10, 4, s_cc3), mst(c, M, e + 0xc, 4, s_cc3_size);
        rt_log("row3: the column screen with a third cell: %u bytes at %#x", s_cc3_size, s_cc3);
        return;
    }
}
/* The players' boxes below a third row: each player's screen (+0x2000 + 4 * p: the box, +0x2004
 * also the names and stats) and 3D viewports (x, y, w, h, set here, placed by the draw each frame;
 * one table per player count: +0x2040 1, +0x2050 2, +0x2070 3, +0x20a0 4, 0x10 each) scaled by BOX_SCALE about BOX_PIVOT (the 1-player box's bottom middle) */
#define BOX_SCALE 0.8f
#define BOX_PX 304.0f
#define BOX_PY 395.0f
/* a still pane parked wholly above the screen (the names' pictures, PICT_001..020 at y -413..-107,
 * placed again after the constructor): further up, or the scale about the boxes' bottom brings it
 * onto the screen. Each frame (characterAnm). */
static void park_up(Ctx *c, uint8_t *M, uint32_t pane, int depth) {
    if (!PTR_OK(pane) || depth > 5) return;
    uint32_t u = ld32(c, M, pane + 0xd8), b = ld32(c, M, pane + 0x2c);
    float y, y1;
    memcpy(&y, &u, 4), memcpy(&y1, &b, 4);
    if (!PTR_OK(ld32(c, M, pane + 0xf8)) && depth > 0 && y < -50 && y + y1 < 0) {
        if (y < -1500) return;
        /* its translation and the position matrix drawn from (+0x50, row 1's translation +0x6c), made
         * from it only when the game moves it */
        uint32_t mu = ld32(c, M, pane + 0x6c);
        float m, ny = -2000;
        memcpy(&m, &mu, 4);
        memcpy(&u, &ny, 4);
        mst(c, M, pane + 0xd8, 4, u);
        if (fabsf(m - y) < 1) { mst(c, M, pane + 0x6c, 4, u); static int n; if (n++ < 2) rt_log("names: a picture parked above the screen moved off it (from y %.0f)", y); }
        else { static int n; if (n++ < 4) rt_log("names: pane matrix y %.1f, not its %.1f", m, y); }
        return;
    }
    for (uint32_t link = ld32(c, M, pane + 0xdc), k = 0; PTR_OK(link) && k < 64; link = ld32(c, M, link + 0xc), k++) park_up(c, M, ld32(c, M, link), depth + 1);
}
/* the viewports as set up (scaled with a third row), kept for vp_place; s_vp_menu 0 none */
static float s_vp[10][4];
static uint32_t s_vp_menu;
static void row3_boxes(Ctx *c, uint8_t *M, uint32_t menu) {
    s_vp_menu = 0;
    if (!IS_RAM(menu)) return;
    for (uint32_t p = 0; s_r3_ok && p < 5; p++) {
        uint32_t scr = ld32(c, M, menu + 0x2000 + 4 * p), u;
        if (!PTR_OK(scr)) continue;
        const float v[4] = {BOX_SCALE, BOX_SCALE, BOX_PX * (1 - BOX_SCALE), BOX_PY * (1 - BOX_SCALE)};
        for (uint32_t i = 0; i < 4; i++) memcpy(&u, &v[i], 4), mst(c, M, scr + 0xcc + 4 * i, 4, u); /* its scale, its offset */
    }
    for (uint32_t p = 0; p < 10; p++) {
        uint32_t at = menu + 0x2040 + 0x10 * p, u[4];
        float f[4];
        for (int i = 0; i < 4; i++) u[i] = ld32(c, M, at + 4 * (uint32_t)i), memcpy(&f[i], &u[i], 4);
        if (!(f[2] > 1 && f[3] > 1 && f[2] < 1000 && f[3] < 1000)) { s_vp[p][2] = 0; continue; }
        if (s_r3_ok) f[0] = BOX_PX + (f[0] - BOX_PX) * BOX_SCALE, f[1] = BOX_PY + (f[1] - BOX_PY) * BOX_SCALE, f[2] *= BOX_SCALE, f[3] *= BOX_SCALE;
        for (int i = 0; i < 4; i++) memcpy(&u[i], &f[i], 4), mst(c, M, at + 4 * (uint32_t)i, 4, u[i]), s_vp[p][i] = f[i];
    }
    s_vp_menu = menu;
}
/* Each frame: in widescreen the 2D (the boxes) is drawn narrowed to the middle (a 4:3 HUD, x / the
 * aspect scale about the middle) while a viewport keeps its EFB place, so a box off the middle (2 to
 * 4 players) had its models out of it: each viewport's middle is narrowed the same way (its size
 * kept: the projection is widened instead). */
static void vp_place(Ctx *c, uint8_t *M, uint32_t menu) {
    if (menu != s_vp_menu || !(g_aspect_scale > 0.5f && g_aspect_scale < 4)) return;
    for (uint32_t p = 0; p < 10; p++) {
        if (s_vp[p][2] <= 0) continue;
        float x = BOX_PX + (s_vp[p][0] + s_vp[p][2] / 2 - BOX_PX) / g_aspect_scale - s_vp[p][2] / 2;
        uint32_t u;
        memcpy(&u, &x, 4);
        mst(c, M, menu + 0x2040 + 0x10 * p, 4, u);
    }
}
/* MenuTitleLine::calc: the banner (its J2DScreen at +8, the screen's offset +0xd8) raised on the
 * character select (+4 the title shown, 1) while there is a third row */
uint64_t i_calc__13MenuTitleLineFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    ctx_call_orig(c, g_native_addr[NAT_calc__13MenuTitleLineFv]);
    uint32_t self = (uint32_t)a0, u;
    if (!PTR_OK(self)) return 0;
    float y = s_r3_ok && ld32(c, M, self + 4) == 1 ? -R3_GRID_UP : 0;
    memcpy(&u, &y, 4);
    mst(c, M, self + 8 + 0xd8, 4, u);
    return 0;
}
uint64_t i_FUN_8015639c(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    cc3_patch(c, M, (uint32_t)a1);
    ctx_call_orig(c, g_native_addr[NAT_FUN_8015639c]);
    row3_setup(c, M, (uint32_t)a0);
    battle_menu_new();
    row3_boxes(c, M, (uint32_t)a0);
    return ctx_gpr(c, 3);
}
/* the cursor's moves (SceneMenu::selectUp / Down / Left / Right, player a1): the game's first (its
 * sound, its move on the 20), then the third row's: down from the second row into it (the game's
 * cursor stays above), up out of it, left / right along it to the next cell with a character */
static void sel_move(Ctx *c, uint8_t *M, uint32_t menu, uint64_t pl, int dir, unsigned nat) {
    int p = pl < 4 ? (int)pl : -1;
    uint32_t at = menu + 0x1c * (uint32_t)(p < 0 ? 0 : p) + 0x212c;
    int was = p >= 0 && IS_RAM(menu) ? (int)ld32(c, M, at) : -1, r3 = p >= 0 ? s_row3[p] : -1;
    ctx_call_orig(c, g_native_addr[nat]);
    if (!s_r3_ok || was < 0 || was >= CP_BASES || !sel_choosing(c, M, menu, p)) return;
    int to = r3;
    if (r3 >= 0) to = dir == 0 ? -1 : dir == 1 ? r3 : row3_next(r3, dir == 2 ? -1 : 1);
    if (r3 >= 0 && dir >= 2 && to < 0) to = r3; /* the row's only character */
    else if (dir == 1 && was % 2 == 1) to = row3_next(was / 2, 0);
    if (to == r3 && r3 < 0) return;
    s_row3[p] = to;
    mst(c, M, at, 4, (uint32_t)((to >= 0 ? to : r3) * 2 + 1));
}
uint64_t i_selectUp__9SceneMenuFi(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    sel_move(c, M, (uint32_t)a0, a1, 0, NAT_selectUp__9SceneMenuFi);
    return 0;
}
uint64_t i_selectDown__9SceneMenuFi(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    sel_move(c, M, (uint32_t)a0, a1, 1, NAT_selectDown__9SceneMenuFi);
    return 0;
}
uint64_t i_selectLeft__9SceneMenuFi(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    sel_move(c, M, (uint32_t)a0, a1, 2, NAT_selectLeft__9SceneMenuFi);
    return 0;
}
uint64_t i_selectRight__9SceneMenuFi(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    sel_move(c, M, (uint32_t)a0, a1, 3, NAT_selectRight__9SceneMenuFi);
    return 0;
}
/* Debug (tools/char_test.sh): each variant in turn from g_chartest_start: under player 1's cursor, picked,
 * the pick undone. The log's "chartest:" lines say which one a freeze happened on. */
volatile int g_chartest_start = -1;
volatile uint32_t g_chartest_btn;
volatile bool g_chartest_in;
static void chartest_step(Ctx *c, uint8_t *M, uint32_t menu, const int cells[4][2]) {
    static int v = -1, st, wait = 120, hold;
    static uint32_t btn;
    if (g_chartest_start < 0) return;
    g_chartest_in = true;
    if (v < 0) v = g_chartest_start;
    g_chartest_btn = hold > 0 ? btn : 0;
    if (hold > 0) hold--;
    if (wait-- > 0) return;
    if (st == 0) {
        if (cells[0][0] >= 0 || cells[0][1] >= 0) { btn = 0x0200, hold = 6, wait = 40; return; } /* B */
        if ((unsigned)v >= charpack_count()) { rt_log("chartest: done"); g_chartest_start = -1, g_chartest_btn = 0; return; }
        if (charpack_pool_left() < (2u << 20)) { /* as a new screen: a visit's files are kept until then */
            if (s_nsel) sel_wipe(c, M, ld32(c, M, menu + 0x2014));
            kart_wipe(c, M, ld32(c, M, menu + 0x2014));
            charpack_pool_reset();
        }
        int page, cell;
        charpack_variant_place(v, &page, &cell);
        while (charpack_page() != page) charpack_page_step(1);
        s_row3[0] = cell >= CP_BASES ? cell - CP_BASES : -1;
        mst(c, M, menu + 0x212c, 4, (uint32_t)(cell >= CP_BASES ? (cell - CP_BASES) * 2 + 1 : cell));
        rt_log("chartest: %d %s hover", v, charpack_name(v));
        st = 1, wait = 45;
    } else if (st == 1) {
        rt_log("chartest: %d %s pick", v, charpack_name(v));
        btn = 0x0100, hold = 6, wait = 70, st = 2; /* A */
    } else {
        rt_log("chartest: %d %s ok", v, charpack_name(v));
        v++, st = 0;
    }
}
uint64_t i_character__9SceneMenuFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t menu = (uint32_t)a0, req = __atomic_exchange_n(&g_char_cycle, 0, __ATOMIC_ACQ_REL);
    if (!IS_RAM(menu) || !charpack_count()) {
        ctx_call_orig(c, g_native_addr[NAT_character__9SceneMenuFv]);
        return 0;
    }
    int cursor[4] = {-1, -1, -1, -1}, gcur[4] = {-1, -1, -1, -1}, team[4] = {-1, -1, -1, -1}, cells[4][2], owner[4][2];
    uint32_t last = ld32(c, M, menu + 0x20f4), teams = ld32(c, M, menu + 0x20fc) + 1;
    if (teams > 4) teams = 4;
    for (uint32_t t = 0; t < 4; t++)
        for (uint32_t k = 0; k < 2; k++) {
            int cell = t < teams ? (int)ld32(c, M, menu + 0x1c * t + 0x2138 + 4 * k) : -1;
            uint32_t o = t < teams ? ld32(c, M, menu + 0x1c * t + 0x2130 + 4 * k) : 4;
            cells[t][k] = cell >= 0 && cell < CP_BASES ? cell : -1;
            owner[t][k] = o < 4 ? (int)o : -1;
        }
    chartest_step(c, M, menu, cells);
    for (uint32_t p = 0; p <= last && p < 4; p++) {
        uint32_t t = ld32(c, M, menu + 0x211c + 4 * p);
        if (t >= teams) continue;
        team[p] = (int)t;
        int cell = (int)ld32(c, M, menu + 0x1c * p + 0x212c);
        if (ld32(c, M, menu + 0x1c * t + 0x2144) == 0 && cell >= 0 && cell < CP_BASES) cursor[p] = gcur[p] = cell;
    }
    /* a select screen opened anew (from the title, a race, a cancel): its first page, and the
     * previews of the last one gone (the game's own models back in their blocks), then the files
     * they and the race after it were built from (charpack_pool_reset); then any player's X: the
     * next page, Y: the previous one */
    if (!charpack_select_active()) {
        rt_log("charpack: select screen opened anew");
        charpack_page_first();
        if (s_nsel) sel_wipe(c, M, ld32(c, M, menu + 0x2014));
        kart_wipe(c, M, ld32(c, M, menu + 0x2014));
        charpack_pool_reset();
        s_sel_flags_sig = ~0u;
        memset(s_sel_wrote, 0, sizeof s_sel_wrote);
        memset(s_picked, 0, sizeof s_picked);
    }
    if (req & 0x0f) charpack_page_step(1);
    else if (req & 0xf0) charpack_page_step(-1);
    /* a cursor in the third row: there while the game's stays on the cell above; a page change
     * moves it to the row's nearest character, or up when the row has none */
    for (int p = 0; p < 4; p++) {
        int r = s_row3[p];
        if (r < 0) continue;
        if (!s_r3_ok || cursor[p] != r * 2 + 1) { s_row3[p] = -1; continue; }
        if (!row3_has(r)) s_row3[p] = r = row3_next(r, 0);
        if (r < 0) continue;
        cursor[p] = CP_BASES + r, gcur[p] = r * 2 + 1;
        mst(c, M, menu + 0x1c * p + 0x212c, 4, (uint32_t)gcur[p]);
    }
    /* a cursor left on an empty cell by the page change: the nearest cell with a character */
    for (int p = 0; p < 4 && req; p++) {
        if (cursor[p] >= CP_BASES || !sel_empty(cursor[p])) continue;
        for (int d = 1; d < CP_BASES; d++) {
            int to = !sel_empty(cursor[p] - d) && cursor[p] - d >= 0 ? cursor[p] - d
                   : !sel_empty(cursor[p] + d) && cursor[p] + d < CP_BASES ? cursor[p] + d : -1;
            if (to < 0) continue;
            cursor[p] = to;
            mst(c, M, menu + 0x1c * p + 0x212c, 4, (uint32_t)to);
            break;
        }
    }
    charpack_select_frame(cursor, cells, owner);

    /* the 3D models: each chosen driver's and each cursor's preview in a block (see above) */
    typedef struct { int t, k, base, v; uint32_t from; bool cursor; } Item;
    Item it[16];
    int ni = 0;
    for (int t = 0; t < (int)teams; t++)
        for (int k = 0; k < 2; k++) {
            int v, cell = charpack_slot(t, k, &v);
            if (cell < 0) continue;
            int vc = charpack_slot_shown(t, k);
            it[ni++] = (Item){t, k, cell, v, CP_CHAR_ID[vc >= 0 ? vc : cell], false};
        }
    for (int p = 0; p < 4; p++) {
        int t = team[p], k = t < 0 ? 2 : cells[t][0] < 0 ? 0 : cells[t][1] < 0 ? 1 : 2;
        if (cursor[p] < 0 || k > 1) continue;
        int v = charpack_cell_variant(cursor[p]);
        it[ni++] = (Item){t, k, charpack_real_base(cursor[p]), v, CP_CHAR_ID[gcur[p]], true};
    }
    bool used[21] = {false};
    int shows[21];
    for (int e = 0; e <= 20; e++) shows[e] = -1;
    memset(s_sel_want, 0, sizeof s_sel_want), memset(s_sel_from, 0, sizeof s_sel_from), memset(s_sel_cursor, 0, sizeof s_sel_cursor);
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < ni; i++) {
            Item *x = &it[i];
            if ((x->v >= 0) != pass || x->base < 0) continue;
            int id = (int)CP_CHAR_ID[x->base];
            if (used[id] && pass) { /* a variant whose base's block shows another: the free one nearest its scale */
                int best = 0;
                float want = SEL_SCALE[id - 1], d = 1e9f;
                for (int e = 1; e <= 16; e++) { /* 17-20 have models of their own (Toadette's hair, King Boo's) */
                    float de = fabsf(SEL_SCALE[e - 1] - want);
                    if (!used[e] && de < d) best = e, d = de;
                }
                if (best) id = best;
            }
            used[id] = true;
            if (pass) shows[id] = x->v;
            s_sel_want[x->t][x->k] = (uint32_t)id, s_sel_from[x->t][x->k] = x->from, s_sel_cursor[x->t][x->k] = x->cursor;
        }
    for (int b = 0; b < CP_BASES; b++) charpack_borrow(b, shows[CP_CHAR_ID[b]]);
    charvoice_update();

    /* A: on a variant in another character's cell, the base's cell for this frame (free for it); on
     * a cell whose pick is taken (it became so under the cursor), nothing */
    static uint16_t a_was[4];
    bool a_down[4];
    for (int p = 0; p < 4; p++) a_down[p] = (g_pad_buttons[p] & 0x0100) && !(a_was[p] & 0x0100), a_was[p] = g_pad_buttons[p];
    int cur_set[4] = {-2, -2, -2, -2};
    sel_flags(c, M, menu, cells);
    for (int p = 0; p < 4; p++) {
        int cell = cursor[p], rb = cell >= 0 ? charpack_real_base(cell) : -1, v = cell >= 0 ? charpack_cell_variant(cell) : -1;
        if (cell < 0 || !a_down[p]) continue;
        bool taken = rb < 0 || charpack_cell_taken(cell) || sel_empty(cell);
        if (!taken && team[p] >= 0 && (cells[team[p]][0] < 0 || cells[team[p]][1] < 0)) {
            int k = cells[team[p]][0] < 0 ? 0 : 1;
            s_picked[team[p]][k].on = true, s_picked[team[p]][k].cursor = cell, s_picked[team[p]][k].page = charpack_page();
        }
        if (!taken && rb == cell) continue; /* the game's own choice of its cell */
        cur_set[p] = taken ? -1 : rb;
        mst(c, M, menu + 0x1c * p + 0x212c, 4, (uint32_t)cur_set[p]);
        if (!taken) charpack_pending(p, v, cell), mst(c, M, menu + 0x228c + rb, 1, 1);
        rt_log("charpack: player %d %s %s in %s's cell", p + 1, taken ? "can't pick (taken)" : "picks", v >= 0 ? charpack_name(v) : charpack_base_name(cell), charpack_base_name(rb));
    }
    int before[4][2];
    for (uint32_t t = 0; t < teams; t++)
        for (uint32_t k = 0; k < 2; k++) {
            before[t][k] = (int)ld32(c, M, menu + 0x1c * t + 0x2138 + 4 * k);
            s_undo_before[t][k] = before[t][k], s_undo_owner[t][k] = owner[t][k];
        }
    s_undo_teams = teams;
    for (int p = 0; p < 4; p++) s_anm_cursor[p] = cur_set[p] != -2 ? gcur[p] : -1;
    ctx_call_orig(c, g_native_addr[NAT_character__9SceneMenuFv]);
    for (int p = 0; p < 4; p++) s_anm_cursor[p] = -1;
    for (int p = 0; p < 4; p++)
        if (cur_set[p] != -2 && (int)ld32(c, M, menu + 0x1c * p + 0x212c) == cur_set[p])
            mst(c, M, menu + 0x1c * p + 0x212c, 4, (uint32_t)gcur[p]);
    sel_undo(c, M, menu);
    s_undo_teams = 0;
    /* a pick in another character's cell: its select animation there (selectA started it on the base's) */
    for (int p = 0; p < 4; p++) {
        int rb = cur_set[p], vc = cursor[p];
        /* (from the third row it stays in the base's cell: the game takes the pick when it ends) */
        if (rb < 0 || vc < 0 || vc >= CP_BASES || rb == vc) continue;
        uint32_t fr = menu + 0x21ec + 8 * rb, fv = menu + 0x21ec + 8 * vc;
        if (!mld(c, M, fr, 1) || ld32(c, M, fr + 4) != (uint32_t)p || mld(c, M, fv, 1)) continue;
        mst(c, M, fv, 1, 1), mst(c, M, fv + 4, 4, (uint32_t)p), mst(c, M, fr, 1, 0);
        uint32_t tr = ld32(c, M, menu + 0x1374 + 12 * rb), tv = ld32(c, M, menu + 0x1374 + 12 * vc);
        mst(c, M, menu + 0x1374 + 12 * rb, 4, tv), mst(c, M, menu + 0x1374 + 12 * vc, 4, tr);
    }
    /* the random pick (the game's roulette never gives a character chosen already, and only this
     * page's): each driver it chose is one of the 20 cells' or, as often as there are free ones, a
     * variant not picked yet from any page, a variant of a chosen character included. Its select
     * animation stays in the cell the roulette chose. */
    static uint32_t rnd;
    if (!rnd) rnd = (uint32_t)time(NULL) * 2654435761u | 1;
    bool drawn[CP_VARIANTS] = {false};
    for (uint32_t t = 0; t < teams; t++)
        for (uint32_t k = 0; k < 2; k++) {
            uint32_t a = menu + 0x1c * t + 0x2138 + 4 * k, o = ld32(c, M, menu + 0x1c * t + 0x2130 + 4 * k);
            int x = (int)ld32(c, M, a);
            if (before[t][k] != -1 || x < 0 || x >= CP_BASES || (o < 4 && a_down[o])) continue;
            s_picked[t][k].on = false; /* the random pick's: no place of its own to go back to */
            int rb = charpack_real_base(x), v = charpack_cell_variant(x);
            int nv = (int)charpack_count(), free_n = 0, pickv = -1;
            for (int i = 0; i < nv && i < CP_VARIANTS; i++) free_n += !charpack_variant_taken(i) && !drawn[i] && i != v;
            rnd ^= rnd << 13, rnd ^= rnd >> 17, rnd ^= rnd << 5;
            int r = (int)(rnd % (uint32_t)(CP_BASES + free_n)) - CP_BASES;
            for (int i = 0; r >= 0 && i < nv && i < CP_VARIANTS; i++)
                if (!charpack_variant_taken(i) && !drawn[i] && i != v && r-- == 0) pickv = i;
            if (pickv >= 0 && charpack_variant_base(pickv) >= 0 && o < 4) {
                rb = charpack_variant_base(pickv), v = pickv;
                drawn[v] = true;
                rt_log("charpack: the random pick: %s as %s", charpack_base_name(rb), charpack_name(v));
                mst(c, M, a, 4, (uint32_t)rb);
                charpack_pending((int)o, v, rb);
                continue;
            }
            if (v >= 0 && v < CP_VARIANTS) drawn[v] = true;
            rt_log("charpack: the random pick lands in %s's cell: %s as %s", charpack_base_name(x), charpack_base_name(rb),
                   v < 0 ? "the game's own" : charpack_name(v));
            if (rb == x || o >= 4) continue;
            mst(c, M, a, 4, (uint32_t)rb);
            charpack_pending((int)o, v, x);
        }
    sel_flags(c, M, menu, cells);
    sel_models(c, M, menu);
    charpack_select_keep();
    return 0;
}

