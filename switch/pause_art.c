/* The pause screen's controller picture, as a Switch: the game draws a GameCube controller with
 * lines to boxes ("DRIFT", "STEER", ...) and a button icon on each box. The picture and the eight
 * icons are separate textures in race2d.arc; their bytes are read from the disc at start-up and
 * registered with the GX layer, and the renderer draws ours in their place (same panes, so the
 * game's own animation and text stay; its lines are hidden, see LINE_PANES). Icons follow the button mapping of the controller
 * that paused; a player on a GameCube controller sees the game's own picture. Art:
 * tools/make_pause_art.py (pause_art_data.S).
 * Also the mods' title bars (SELECT CUP / COURSE / STAGE "/ Y X", RTGP "/ Y"), whose GameCube buttons are
 * part of the picture: shown without them, with a line below that names the Switch buttons. */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>
#include <GLES3/gl32.h>

#include "menu.h"
#include "pause_art.h"
#include "ui_font.h"
#include "../runtime/gx.h"
#include "../runtime/rt.h"

volatile int g_pause_kind;

extern const uint8_t pause_console_rgba[], pause_badges_rgba[];
#define CON_W 352
#define CON_H 256
#define CELL 64
#define BADGE_COLS 6
/* tools/make_pause_art.py BADGES */
enum { B_A, B_B, B_X, B_Y, B_L, B_R, B_ZL, B_ZR, B_PLUS, B_LS, B_RS, B_UP, B_DOWN, B_LEFT, B_RIGHT, B_SL, B_SR, B_STICK, B_DPAD, B_NONE = -1 };
static const struct { uint64_t bit; int badge; } BITS[] = {
    {HidNpadButton_A, B_A}, {HidNpadButton_B, B_B}, {HidNpadButton_X, B_X}, {HidNpadButton_Y, B_Y},
    {HidNpadButton_L, B_L}, {HidNpadButton_R, B_R}, {HidNpadButton_ZL, B_ZL}, {HidNpadButton_ZR, B_ZR},
    {HidNpadButton_Plus, B_PLUS}, {HidNpadButton_StickL, B_LS}, {HidNpadButton_StickR, B_RS},
    {HidNpadButton_Up, B_UP}, {HidNpadButton_Down, B_DOWN}, {HidNpadButton_Left, B_LEFT}, {HidNpadButton_Right, B_RIGHT},
};

/* Substitution ids (1 = the picture) and the GameCube button each icon shows. */
enum { S_CONSOLE = 1, S_L, S_R, S_Z, S_X, S_Y, S_STICK, S_A, S_B, S_MENUPAD, S_KMH, S_COUNT };
/* S_MENUPAD: the GameCube controller the "how many players" screen shows once per player
 * (menu.arc, 80x56), drawn as the pause screen's Switch at the same height. Ours is 2x the size
 * for a sharper picture: same shape, so the game's pane shows it the same. */
#define PAD_W 160
#define PAD_H 112
#define KMH_W 256 /* 4x the game's 64x16, its pixel letters kept sharp */
#define KMH_H 64
#define JUMP_TW 148 /* the JUMP box, 2 pixels a layout unit: 74 x 71, REAR_MARGIN above */
#define JUMP_TH 158
static const struct { const char *file; int gc; int w, h; const char *arc; } SLOTS[S_COUNT] = {
    [S_CONSOLE] = {"fs_gc_contorollerop_s3tc.bti", -1, CON_W, CON_H},
    /* the buttons: a row of three (patch_layout ICON_ROW x ICON_H); Y: the REAR VIEW box (box_draw) */
    [S_L] = {"l_trigger.bti", GC_L, JUMP_TW, JUMP_TH}, /* NSingle: the JUMP box (patch_layout I_JUMP) */      [S_R] = {"r_trigger.bti", GC_R, 192, 64},
    [S_Z] = {"fs_btz_s3tc.bti", GC_Z, 192, 64},    [S_X] = {"fs_btx_ia4.bti", GC_X, 192, 64},
    [S_Y] = {"fs_bty_ia4.bti", GC_Y, 376, 158},    [S_STICK] = {"fs_btcst_ia4.bti", -1, 128, 60},
    [S_A] = {"fs_bta_s3tc.bti", GC_A, 192, 64},    [S_B] = {"fs_btb_s3tc.bti", GC_B, 192, 64},
    [S_MENUPAD] = {"gc_controller.bti", -1, PAD_W, PAD_H, "SceneData/English/menu.arc"},
    /* the speedometer's "mph" (64x16): "km/h" when the - menu's SPEED is KM/H (build_kmh) */
    [S_KMH] = {"speed_km.bti", -1, KMH_W, KMH_H, "MRAM_Locale/English/MRAMLoc.arc"},
};
static GLuint s_tex[S_COUNT];

/* MKDD Plus's title bars (titleline.arc, 512x32 IA4) end in "/ Y X" with the GameCube's Y and X
 * buttons drawn in (the buttons that change the track set); Retro Track Grand Prix's in "/ Y" (its X
 * changes the set too). They are shown without the slash and
 * the buttons, and the renderer draws a hint line right below (render_set_below): "PRESS (Y) OR
 * (X) FOR MORE CUPS" in the game's lettering, with the Switch buttons mapped to GameCube Y and X.
 * The original game's bars have no buttons (fewer shapes in the picture) and are left alone. */
#define BAR_ARC "SceneData/English/titleline.arc"
#define BAR_W 512
#define BAR_H 32
enum { BAR_FIRST = S_COUNT, BAR_COUNT = 3 };
static const char *const BAR_FILES[BAR_COUNT] = {"selectcup.bti", "selectcourse.bti", "selectmap.bti"};
static const char *const BAR_MORE[BAR_COUNT] = {"CUPS", "COURSES", "STAGES"};
/* The cup and course pages' "L + R: RANDOM" (courseselect.arc: the word, and "+" between the L and
 * R triggers, S_L / S_R) sits on the hint line's right half: while a cup or course bar's hint shows
 * it is drawn RANDOM_DY lower, centred in the gap between the cups and the course box. */
#define RND_ARC "SceneData/English/courseselect.arc"
enum { RND_PLUS = BAR_FIRST + BAR_COUNT, RND_WORD, RND_END };
static const char *const RND_FILES[2] = {"mariofont_number_plus.bti", "mozi_ramdomselect.bti"};
#define RANDOM_DY 115.0f
static u64 s_hint_tick; /* when a cup or course bar's hint line was last drawn */
static volatile u64 s_random_tick; /* ... and the word RANDOM (any game: pause_art_random_shown) */
static uint8_t *s_bar[BAR_COUNT];     /* RGBA, the slash and buttons erased */
static int s_bar_l[BAR_COUNT], s_bar_r[BAR_COUNT]; /* the words left: their left and right edge */
static GLuint s_bar_tex[BAR_COUNT], s_hint_tex[BAR_COUNT];
static uint64_t s_hint_key[BAR_COUNT];
#define HINT_W (BAR_W * 2)
#define HINT_H (BAR_H * 2)
/* The game's own stick icon (decoded at start-up): a GameCube controller's STEER icon, centred in
 * the widened pane instead of stretched. */
#define GC_STICK_SIZE 32
static uint8_t *s_gc_stick;
/* The game's own icon of each button slot (a GameCube controller's), decoded at start-up: drawn in
 * the rows (patch_layout changed their panes' shape, so the game's pictures would stretch). */
static uint8_t *s_gc_icon[S_COUNT];
static int s_gc_w[S_COUNT], s_gc_h[S_COUNT];
static uint64_t s_key = ~0ull;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* Layout changes to pause2.blo (MRAM_Locale/<language>/MRAMLoc.arc), single-player (NSingle) and
 * two-per-kart (NTag) panels, made as the layout is read from the disc: the game's lines from each
 * box to its GameCube button get zero width (the boxes and icons say which button does what), the
 * STEER icon (PICT_062, PICT_032) gets room for the stick and the D-pad side by side, and the other
 * icons become a row of up to three buttons centred on their box's header (ICON_ROW wide, as STEER).
 * DRIFT shows GameCube L's buttons with R's (both drift), so L's own icon goes; NSingle's Y icon
 * becomes a whole REAR VIEW box of its own (box_draw), drawn by us, in the gap above the
 * controller between DRIFT and SWITCH; NTag has no room for it, so its Y icon goes. NSingle's L icon
 * (hidden otherwise) becomes the JUMP box (box_draw): left of BRAKE / REVERSE, under STEER. */
#define STEER_W 62.0f
#define ICON_ROW 87.0f    /* three 29-unit buttons */
#define ICON_H 29.0f
#define HEADER_Y 8.0f     /* a box's icons: centred this far below its top edge */
#define REAR_MARGIN 8.0f  /* the REAR VIEW pane: room above its box for the buttons on its top edge */
static const char *const STEER_PANES[] = {"PICT_062", "PICT_032"};
static const char *const LINE_PANES[] = {
    "PICT_042", "PICT_043", "PICT_044", "PICT_045", "PICT_046", "PICT_047", "PICT_048", "PICT_049",
    "PICT_051", "PICT_052", "PICT_053", "PICT_054", "PICT_055", "PICT_057", "PICT_070", "PICT_024",
    "PICT_008", "PICT_009", "PICT_016", "PICT_017", "PICT_018", "PICT_019", "PICT_020", "PICT_021",
    "PICT_022", "PICT_023", "PICT_026", "PICT_027", "PICT_028", "PICT_029", "PICT_005",
};
/* icon panes: hidden, a row centred on a sibling box's header, a row centred on its parent box's
 * header (NTag: the icons are the boxes' children, placed from the box's top-middle anchor), only
 * made a row where it is, or NSingle's REAR VIEW box */
enum { I_HIDE, I_ROW, I_CHILD, I_WIDEN, I_REAR, I_JUMP };
static const struct { const char *pane, *box; int how; } ICON_PANES[] = {
    {"PICT_059", NULL, I_JUMP},       {"PICT_060", "WIND_008", I_ROW}, {"PICT_061", "WIND_009", I_ROW},
    {"PICT_063", "WIND_010", I_ROW},  {"PICT_065", "WIND_012", I_ROW}, {"PICT_069", "WIND_014", I_ROW},
    {"PICT_064", NULL, I_REAR},
    {"PICT_014", NULL, I_HIDE},       {"PICT_015", NULL, I_CHILD},     {"PICT_030", NULL, I_CHILD},
    {"PICT_033", NULL, I_CHILD},      {"PICT_034", NULL, I_HIDE},      {"PICT_035", NULL, I_CHILD},
    {"PICT_013", NULL, I_WIDEN},
};
/* pan2 fields: anchor (0-8: left/middle/right times top/centre/bottom) at +0xD, tag at +0x10,
 * width +0x20, height +0x24, rotation +0x38, x +0x3C, y +0x40 (from the parent's anchor point) */
typedef struct { char tag[9]; uint32_t at; unsigned key; int anchor; float x, y, w, h; } Pane;
static unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
/* panes moved (their animation joints get the new place too, patch_anim) and hidden (their
 * joints' values are free for that) */
typedef struct { unsigned key; float x, y; } Move;
static Move s_move[32];
static unsigned s_nmove, s_hide[64], s_nhide;
static float bef(const uint8_t *p) {
    uint32_t u = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static const Pane *pane_find(const Pane *v, unsigned n, const char *tag) {
    for (unsigned i = 0; i < n; i++)
        if (!strcmp(v[i].tag, tag)) return &v[i];
    return NULL;
}
static float pane_left(const Pane *p) { return p->x - (float)(p->anchor % 3) * p->w / 2; }
static float pane_top(const Pane *p) { return p->y - (float)(p->anchor / 3) * p->h / 2; }
static void patch_f32(uint64_t at, float f) {
    uint32_t u;
    uint8_t b[4];
    memcpy(&u, &f, 4);
    for (int k = 0; k < 4; k++) b[k] = (uint8_t)(u >> (24 - 8 * k));
    disc_patch(at, b, 4);
}
static unsigned set_field(uint64_t off, const Pane *p, unsigned field, float f) {
    patch_f32(off + p->at + field, f);
    if (field == 0x20 && f == 0.0f && s_nhide < 64) s_hide[s_nhide++] = p->key;
    return 1;
}
static void moved(const Pane *p, float x, float y) {
    if (s_nmove < 32) s_move[s_nmove++] = (Move){p->key, x, y};
}
/* a centred (anchor 4) pane: centre (x, y), w x h, upright */
static unsigned place(uint64_t off, const Pane *p, float x, float y, float w, float h) {
    if (p->anchor != 4) return 0;
    moved(p, x, y);
    set_field(off, p, 0x3C, x);
    set_field(off, p, 0x40, y);
    set_field(off, p, 0x20, w);
    set_field(off, p, 0x24, h);
    return set_field(off, p, 0x38, 0.0f);
}

/* pause2.bck (ANK1) sets every pane's place each frame from its joint (the pane's key, pan2 +0xA):
 * 9 (count, index, tangent) u16 triples, scale / rotation / translation for x, y, z; the pane's x is
 * translation x, its y translation z, its angle rotation y. Joints share values, so a moved pane's
 * joint is pointed at values of its own: equal ones already there, or ones only hidden panes' joints
 * (or none) read. */
static int anim_value(uint64_t off, uint8_t *b, uint32_t to, unsigned nt, uint8_t *busy, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    for (unsigned i = 0; i < nt; i++)
        if (be32(b + to + 4 * i) == u) { busy[i] = 1; return (int)i; }
    for (unsigned i = 0; i < nt; i++)
        if (!busy[i]) {
            busy[i] = 1;
            for (int k = 0; k < 4; k++) b[to + 4 * i + k] = (uint8_t)(u >> (24 - 8 * k));
            patch_f32(off + to + 4 * i, f);
            return (int)i;
        }
    return -1;
}
static void anim_set(uint64_t at, unsigned ix) {
    uint8_t t[6] = {0, 1, (uint8_t)(ix >> 8), (uint8_t)ix, 0, 0};
    disc_patch(at, t, 6);
}
static unsigned patch_anim(const char *arc) {
    uint64_t off;
    uint32_t size, a = 0x20;
    unsigned n = 0;
    if (!s_nmove || !arc_locate(arc, "pause2.bck", &off, &size) || size < a + 0x24 || size > (1u << 20)) return 0;
    uint8_t *b = malloc(size), *busy = NULL;
    if (!b) return 0;
    disc_read(off, b, size);
    unsigned nj = be16(b + a + 12), nr = be16(b + a + 16), nt = be16(b + a + 18);
    uint32_t jo = a + be32(b + a + 0x14), ro = a + be32(b + a + 0x1C), to = a + be32(b + a + 0x20);
    int r0 = -1; /* a rotation value of 0 */
    if (!memcmp(b + a, "ANK1", 4) && jo + nj * 54 <= size && ro + nr * 2 <= size && to + nt * 4 <= size && nt &&
        (busy = calloc(nt, 1))) {
        for (unsigned i = 0; i < nr && r0 < 0; i++)
            if (!be16(b + ro + 2 * i)) r0 = (int)i;
        for (unsigned j = 0; j < nj; j++) {
            bool hidden = false;
            for (unsigned k = 0; k < s_nhide; k++) hidden |= s_hide[k] == j;
            for (int t = 2; t < 9; t += 3) {
                const uint8_t *q = b + jo + j * 54 + t * 6;
                unsigned c = be16(q), ix = be16(q + 2), len = c > 1 ? c * (be16(q + 4) ? 4 : 3) : 1;
                for (unsigned i = ix; i < ix + len && i < nt; i++) busy[i] |= !hidden;
            }
        }
        for (unsigned m = 0; m < s_nmove && r0 >= 0; m++) {
            if (s_move[m].key >= nj) continue;
            uint32_t q = jo + s_move[m].key * 54;
            int x = anim_value(off, b, to, nt, busy, s_move[m].x), y = anim_value(off, b, to, nt, busy, s_move[m].y);
            if (x < 0 || y < 0) continue;
            anim_set(off + q + 2 * 6, (unsigned)x);
            anim_set(off + q + 8 * 6, (unsigned)y);
            anim_set(off + q + 4 * 6, (unsigned)r0);
            n++;
        }
    }
    free(busy);
    free(b);
    return n;
}

static unsigned patch_layout(const char *arc) {
    uint64_t off;
    uint32_t size, n = 0;
    s_nmove = s_nhide = 0;
    if (!arc_locate(arc, "pause2.blo", &off, &size) || size > (1u << 20)) return 0;
    uint8_t *b = malloc(size);
    Pane *v = calloc(512, sizeof *v);
    unsigned nv = 0;
    if (!b || !v) { free(b); free(v); return 0; }
    disc_read(off, b, size);
    if (size > 0x20 && !memcmp(b, "SCRNblo2", 8)) {
        for (uint32_t i = 0x20; i + 8 <= size;) {
            uint32_t bsz = be32(b + i + 4);
            if (bsz < 8 || i + bsz > size) break;
            if (!memcmp(b + i, "PIC2", 4) || !memcmp(b + i, "WIN2", 4)) {
                /* pan2 block: its fields from its start */
                for (uint32_t j = i + 8; j + 0x44 <= i + bsz; j++) {
                    if (memcmp(b + j, "pan2", 4)) continue;
                    if (nv < 512) {
                        Pane *p = &v[nv++];
                        int t = 0;
                        for (int k = 0; k < 8; k++)
                            if (b[j + 16 + k]) p->tag[t++] = (char)b[j + 16 + k];
                        p->tag[t] = 0;
                        p->at = j;
                        p->key = be16(b + j + 0xA);
                        p->anchor = b[j + 0xD] <= 8 ? b[j + 0xD] : 0;
                        p->w = bef(b + j + 0x20), p->h = bef(b + j + 0x24), p->x = bef(b + j + 0x3C), p->y = bef(b + j + 0x40);
                    }
                    break;
                }
            }
            i += bsz;
        }
        const Pane *p;
        for (unsigned k = 0; k < sizeof LINE_PANES / sizeof LINE_PANES[0]; k++)
            if ((p = pane_find(v, nv, LINE_PANES[k]))) n += set_field(off, p, 0x20, 0.0f);
        for (unsigned k = 0; k < sizeof STEER_PANES / sizeof STEER_PANES[0]; k++)
            if ((p = pane_find(v, nv, STEER_PANES[k]))) n += set_field(off, p, 0x20, STEER_W);
        for (unsigned k = 0; k < sizeof ICON_PANES / sizeof ICON_PANES[0]; k++) {
            const Pane *box = ICON_PANES[k].box ? pane_find(v, nv, ICON_PANES[k].box) : NULL;
            if (!(p = pane_find(v, nv, ICON_PANES[k].pane))) continue;
            switch (ICON_PANES[k].how) {
            case I_HIDE: n += set_field(off, p, 0x20, 0.0f); break;
            case I_ROW:
                if (box) n += place(off, p, pane_left(box) + box->w / 2, pane_top(box) + HEADER_Y, ICON_ROW, ICON_H);
                break;
            case I_CHILD: n += place(off, p, 0.0f, HEADER_Y, ICON_ROW, ICON_H); break;
            case I_WIDEN: n += place(off, p, p->x, p->y, ICON_ROW, ICON_H); break;
            case I_REAR: {
                /* a box the size of DRIFT's, centred in the gap between DRIFT and SWITCH, its top
                 * just below SWITCH's; the pane (anchored top-left) starts REAR_MARGIN higher */
                const Pane *drift = pane_find(v, nv, "WIND_008"), *sw = pane_find(v, nv, "WIND_009");
                if (!drift || !sw || p->anchor != 0) { n += set_field(off, p, 0x20, 0.0f); break; }
                float cx = (pane_left(drift) + drift->w + pane_left(sw)) / 2, top = pane_top(sw) + 3.0f;
                set_field(off, p, 0x3C, cx - drift->w / 2);
                set_field(off, p, 0x40, top - REAR_MARGIN);
                moved(p, cx - drift->w / 2, top - REAR_MARGIN);
                set_field(off, p, 0x20, drift->w);
                n += set_field(off, p, 0x24, drift->h + REAR_MARGIN);
                break;
            }
            case I_JUMP: {
                /* from STEER's left edge to 8 short of BRAKE / REVERSE's, as high as it and level
                 * with it; JUMP_TW x JUMP_TH is drawn for 74 x 71 */
                const Pane *steer = pane_find(v, nv, "WIND_011"), *brake = pane_find(v, nv, "WIND_014");
                float l = steer ? pane_left(steer) : 0, r = brake ? pane_left(brake) - 8.0f : 0;
                if (!steer || !brake || r - l < 40.0f) { n += set_field(off, p, 0x20, 0.0f); break; }
                float top = pane_top(brake) - REAR_MARGIN, h = brake->h + REAR_MARGIN;
                n += place(off, p, (l + r) / 2, top + h / 2, r - l, h);
                break;
            }
            }
        }
    }
    free(v);
    free(b);
    unsigned m = patch_anim(arc);
    if (n && m < s_nmove) rt_log("pause art: %s: %u of %u moved panes' animation changed", arc, m, s_nmove);
    return n;
}

/* The picture's shapes from left to right (runs of columns with visible pixels, wider than 3):
 * Plus's bars are "SELECT", "CUP", "/", Y, X, RTGP's "SELECT", "CUP", "/", Y; the slash and the
 * buttons are the narrow shapes at the end (~30 px, a word is wider). Returns the left edge of the
 * slash (-1: not a bar with buttons), *l and *r the edges of the words before it. */
static int bar_buttons(const uint8_t *rgba, int *l, int *r) {
    int start[16], end[16], n = 0, run = -1;
    for (int x = 0; x <= BAR_W; x++) {
        bool on = false;
        for (int y = 0; x < BAR_W && y < BAR_H && !on; y++) on = rgba[((size_t)y * BAR_W + x) * 4 + 3] > 40;
        if (on && run < 0) run = x;
        if (!on && run >= 0) {
            if (x - run > 3 && n < 16) { start[n] = run; end[n] = x; n++; }
            run = -1;
        }
    }
    int t = 0;
    while (t < n && end[n - 1 - t] - start[n - 1 - t] <= 40) t++;
    if (t < 2 || t > 3 || n - t < 1) return -1;
    *l = start[0];
    *r = end[n - t - 1];
    return start[n - t] - 2;
}

static void bars_init(void) {
    unsigned n = 0;
    for (int i = 0; i < BAR_COUNT; i++) {
        uint64_t off;
        uint32_t size;
        uint8_t *b = NULL;
        if (!arc_locate(BAR_ARC, BAR_FILES[i], &off, &size) || size > (1u << 20) || !(b = malloc(size))) continue;
        disc_read(off, b, size);
        uint32_t fmt = b[0], w = (uint32_t)(b[2] << 8 | b[3]), h = (uint32_t)(b[4] << 8 | b[5]), doff = be32(b + 0x1C);
        size_t bytes = gx_tex_size(fmt, w, h);
        uint8_t *rgba = fmt == 2 && w == BAR_W && h == BAR_H && bytes && doff + bytes <= size ? malloc((size_t)w * h * 4) : NULL;
        if (rgba) {
            GxTexture tx = {0};
            tx.width = w; tx.height = h; tx.fmt = fmt; tx.raw = b + doff; tx.raw_len = bytes;
            gx_texture_decode(&tx, rgba);
            int l, r, x0 = bar_buttons(rgba, &l, &r);
            if (x0 > 0) {
                /* erase the slash and buttons, then centre the words left (as the bar was meant to be) */
                int sh = BAR_W / 2 - (l + r) / 2;
                for (int y = 0; y < BAR_H; y++) {
                    uint8_t *row = rgba + (size_t)y * BAR_W * 4;
                    memset(row + (size_t)x0 * 4, 0, (size_t)(BAR_W - x0) * 4);
                    if (sh > 0) {
                        memmove(row + (size_t)sh * 4, row, (size_t)(BAR_W - sh) * 4);
                        memset(row, 0, (size_t)sh * 4);
                    }
                }
                if (sh > 0) { l += sh; r += sh; }
                s_bar[i] = rgba;
                s_bar_l[i] = l;
                s_bar_r[i] = r;
                s_hint_key[i] = ~0ull;
                gx_subst_register(BAR_FIRST + i, fmt, w, h, fnv_bytes(FNV_INIT, b + doff, bytes));
                n++;
            } else free(rgba);
        }
        free(b);
    }
    if (n) rt_log("pause art: %u title bars with GameCube buttons (a mod's), shown with a Switch button hint", n);
    for (int i = 0; i < 2; i++) {
        uint64_t off;
        uint32_t size;
        uint8_t *b = NULL;
        if (!arc_locate(RND_ARC, RND_FILES[i], &off, &size) || size > (1u << 20) || !(b = malloc(size))) continue;
        disc_read(off, b, size);
        uint32_t fmt = b[0], w = (uint32_t)(b[2] << 8 | b[3]), h = (uint32_t)(b[4] << 8 | b[5]), doff = be32(b + 0x1C);
        size_t bytes = gx_tex_size(fmt, w, h);
        if (bytes && doff + bytes <= size) gx_subst_register(RND_PLUS + i, fmt, w, h, fnv_bytes(FNV_INIT, b + doff, bytes));
        free(b);
    }
}

static void kf_init(void);
void pause_art_init(void) {
    static const char *const LANGS[] = {"English", "French", "German", "Italian", "Spanish", "Japanese"};
    for (unsigned i = 0; i < sizeof LANGS / sizeof LANGS[0]; i++) {
        char arc[64];
        snprintf(arc, sizeof arc, "MRAM_Locale/%s/MRAMLoc.arc", LANGS[i]);
        unsigned h = patch_layout(arc);
        if (h) rt_log("pause art: %s layout, %u panes changed", LANGS[i], h);
    }
    unsigned n = 0;
    for (unsigned id = 1; id < S_COUNT; id++) {
        uint32_t size = 0;
        uint8_t *b = NULL;
        if (SLOTS[id].arc) {
            uint64_t off;
            if (arc_locate(SLOTS[id].arc, SLOTS[id].file, &off, &size) && size < (1u << 20) && (b = malloc(size))) disc_read(off, b, size);
        } else b = race2d_read(SLOTS[id].file, &size);
        if (!b) continue;
        uint32_t fmt = b[0], w = (uint32_t)(b[2] << 8 | b[3]), h = (uint32_t)(b[4] << 8 | b[5]), off = be32(b + 0x1C);
        size_t bytes = gx_tex_size(fmt, w, h);
        if (bytes && off + bytes <= size) {
            gx_subst_register(id, fmt, w, h, fnv_bytes(FNV_INIT, b + off, bytes));
            n++;
            if (id == S_STICK && w == GC_STICK_SIZE && h == GC_STICK_SIZE && (s_gc_stick = malloc(w * h * 4))) {
                GxTexture t = {0};
                t.width = w; t.height = h; t.fmt = fmt; t.raw = b + off; t.raw_len = bytes;
                gx_texture_decode(&t, s_gc_stick);
            } else if (id >= S_L && id <= S_B && id != S_STICK && w <= 256 && h <= 256 && (s_gc_icon[id] = malloc(w * h * 4))) {
                GxTexture t = {0};
                t.width = w; t.height = h; t.fmt = fmt; t.raw = b + off; t.raw_len = bytes;
                gx_texture_decode(&t, s_gc_icon[id]);
                s_gc_w[id] = (int)w, s_gc_h[id] = (int)h;
            }
        }
        free(b);
    }
    rt_log("pause art: %u of %d pictures recognised", n, S_COUNT - 1);
    bars_init();
    kf_init();
}

/* The Switch buttons (badges) that give the GameCube button gc on the pausing controller, up to max. */
static int badges_for(int gc, int kind, int *out, int max) {
    int n = 0;
    if (max <= 0) return 0;
    if (kind == PAUSE_LEFT || kind == PAUSE_RIGHT) { /* main_switch.c map_sideways */
        bool l = kind == PAUSE_LEFT;
        switch (gc) {
        case GC_A: out[n++] = l ? B_UP : B_B; break;
        case GC_B: out[n++] = l ? B_RIGHT : B_Y; break;
        case GC_X: out[n++] = l ? B_LEFT : B_A; break;
        case GC_Y: out[n++] = l ? B_DOWN : B_X; break;
        case GC_L: out[n++] = B_SL; break;
        case GC_R: out[n++] = B_SR; break;
        case GC_Z: out[n++] = l ? B_L : B_R; break;
        }
        return n;
    }
    for (unsigned i = 0; i < sizeof BITS / sizeof BITS[0] && n < max; i++)
        if (g_set.map[gc] & BITS[i].bit) out[n++] = BITS[i].badge;
    return n;
}

/* An RGBA picture (rows stride bytes apart): a badge of the atlas or one of the game's icons. */
typedef struct { const uint8_t *px; int w, h, stride; } Img;
static Img badge_img(int b) {
    Img m = {pause_badges_rgba + ((size_t)(b / BADGE_COLS) * CELL * BADGE_COLS * CELL + (size_t)(b % BADGE_COLS) * CELL) * 4,
             CELL, CELL, BADGE_COLS * CELL * 4};
    return m;
}

/* Bilinear copy of src into a dw x dh rectangle at (x, y) of a w x h RGBA canvas, over what is
 * there (premultiplied while filtering, so edges don't darken); parts off the canvas are left out. */
static void put_img(uint8_t *dst, int w, int h, int x, int y, int dw, int dh, Img src) {
    for (int j = 0; j < dh; j++)
        for (int i = 0; i < dw; i++) {
            if (x + i < 0 || x + i >= w || y + j < 0 || y + j >= h) continue;
            float fx = ((float)i + 0.5f) * src.w / (float)dw - 0.5f, fy = ((float)j + 0.5f) * src.h / (float)dh - 0.5f;
            int x0 = (int)fx, y0 = (int)fy;
            if (fx < 0) { fx = 0; x0 = 0; }
            if (fy < 0) { fy = 0; y0 = 0; }
            int x1 = x0 + 1 < src.w ? x0 + 1 : x0, y1 = y0 + 1 < src.h ? y0 + 1 : y0;
            float ax = fx - (float)x0, ay = fy - (float)y0, acc[4] = {0};
            const int xs[2] = {x0, x1}, ys[2] = {y0, y1};
            for (int k = 0; k < 4; k++) {
                const uint8_t *p = src.px + (size_t)ys[k >> 1] * src.stride + (size_t)xs[k & 1] * 4;
                float wt = ((k & 1) ? ax : 1 - ax) * ((k >> 1) ? ay : 1 - ay), a = p[3] / 255.0f;
                for (int c = 0; c < 3; c++) acc[c] += p[c] * a * wt;
                acc[3] += a * wt;
            }
            if (acc[3] <= 0.0f) continue;
            uint8_t *q = dst + ((size_t)(y + j) * w + (size_t)(x + i)) * 4;
            float da = q[3] / 255.0f * (1 - acc[3]), oa = acc[3] + da;
            for (int c = 0; c < 3; c++) q[c] = (uint8_t)((acc[c] + q[c] * da) / oa + 0.5f);
            q[3] = (uint8_t)(oa * 255.0f + 0.5f);
        }
}
static void put_cell(uint8_t *dst, int w, int x, int y, int size, const uint8_t *src, int stride, int cell) {
    Img m = {src, cell, cell, stride};
    put_img(dst, w, 1 << 30, x, y, size, size, m);
}
static void put_badge(uint8_t *dst, int w, int x, int y, int size, int b) { put_img(dst, w, 1 << 30, x, y, size, size, badge_img(b)); }
/* src fitted into a cell x cell square centred on (cx, cy), its shape kept */
static void put_fit(uint8_t *dst, int w, int h, int cx, int cy, int cell, Img src) {
    int dw = cell, dh = cell;
    if (src.w > src.h) dh = cell * src.h / src.w;
    else dw = cell * src.w / src.h;
    put_img(dst, w, h, cx - dw / 2, cy - dh / 2, dw, dh, src);
}
/* n pictures side by side, cell wide each, centred on (cx, cy) */
static void put_row(uint8_t *dst, int w, int h, int cx, int cy, int cell, const Img *m, int n) {
    for (int i = 0; i < n; i++) put_fit(dst, w, h, cx - n * cell / 2 + i * cell + cell / 2, cy, cell, m[i]);
}

/* The game's own icon of each slot (a GameCube controller's), decoded at start-up. */
/* What a slot's row shows: DRIFT (S_R) both triggers' buttons, the others their own GameCube
 * button's; on a GameCube controller the game's own icons. Up to 3. */
static int slot_images(unsigned id, int kind, Img out[3]) {
    int n = 0;
    if (kind == PAUSE_GC) {
        const unsigned ids[2] = {id == S_R ? S_L : id, id == S_R ? S_R : 0};
        for (int k = 0; k < 2; k++)
            if (ids[k] && s_gc_icon[ids[k]]) out[n++] = (Img){s_gc_icon[ids[k]], s_gc_w[ids[k]], s_gc_h[ids[k]], s_gc_w[ids[k]] * 4};
        return n;
    }
    int b[3];
    n = badges_for(SLOTS[id].gc, kind, b, 3);
    if (id == S_R) { /* GameCube L's buttons too, each once */
        int l[3], nl = badges_for(GC_L, kind, l, 3);
        for (int k = 0; k < nl && n < 3; k++) {
            bool dup = false;
            for (int i = 0; i < n; i++) dup |= b[i] == l[k];
            if (!dup) b[n++] = l[k];
        }
    }
    for (int i = 0; i < n; i++) out[i] = badge_img(b[i]);
    return n;
}

/* One MarioFont letter (rgb, dark outline) of height size at (x, y) of a w x h canvas, or its
 * advance only. */
static float font_letter(uint8_t *dst, int w, int h, char c, float x, int y, int size, uint32_t rgb, bool draw) {
    float sc = (float)size / 32.0f, uv[4], g0, g1;
    if (c == ' ') return 11 * sc;
    int aw, ah;
    const uint8_t *atlas = uifont_atlas(&aw, &ah);
    if (!atlas || !uifont_glyph(c, uv, &g0, &g1)) return 0;
    if (draw) {
        uint8_t cell[32 * 32 * 4];
        int cx = (int)(uv[0] * aw + 0.5f), cy = (int)(uv[1] * ah + 0.5f);
        for (int v = 0; v < 32; v++)
            for (int u = 0; u < 32; u++) {
                const uint8_t *s = atlas + ((size_t)(cy + v) * aw + (size_t)(cx + u)) * 2;
                uint8_t *o = cell + ((size_t)v * 32 + u) * 4;
                o[0] = (uint8_t)(s[0] * (rgb >> 16 & 0xFF) / 255);
                o[1] = (uint8_t)(s[0] * (rgb >> 8 & 0xFF) / 255);
                o[2] = (uint8_t)(s[0] * (rgb & 0xFF) / 255);
                o[3] = s[1];
            }
        Img m = {cell, 32, 32, 32 * 4};
        put_img(dst, w, h, (int)(x - g0 * sc + 0.5f), y, size, size, m);
    }
    return (g1 - g0 - 2) * sc;
}
static float font_text(uint8_t *dst, int w, int h, const char *s, float x, int y, int size, uint32_t rgb, bool draw) {
    float x0 = x;
    for (; *s; s++) x += font_letter(dst, w, h, *s, x, y, size, rgb, draw);
    return x - x0;
}

/* kartfont.bfn, the font of the pause boxes' names, read from main.dol's data sections at start-up
 * (the game keeps it in the DOL; MKDD Plus and RTGP too): 24x24 I4 cells, codes from MAP1's
 * first one up (linear), WID1 = kerning and width per code. NULL: not found (the box's name is
 * then drawn in MarioFont). */
#define KF_CELL 24
static uint8_t *s_kf_px;            /* intensity, KF_CELL x KF_CELL per glyph */
static int s_kf_first, s_kf_count;
static uint8_t s_kf_kern[256], s_kf_width[256];

static bool kf_parse(const uint8_t *b, uint32_t size) {
    const uint8_t *gly = NULL, *map = NULL, *wid = NULL;
    uint32_t nb = be32(b + 12), p = 0x20;
    for (uint32_t k = 0; k < nb && p + 8 <= size; k++) {
        uint32_t bs = be32(b + p + 4);
        if (bs < 8 || p + bs > size) return false;
        if (!memcmp(b + p, "GLY1", 4) && bs >= 0x20) gly = b + p;
        if (!memcmp(b + p, "MAP1", 4) && bs >= 0x10) map = b + p;
        if (!memcmp(b + p, "WID1", 4) && bs >= 0x0C) wid = b + p;
        p += bs;
    }
    if (!gly || !map || !wid) return false;
    int g0 = gly[8] << 8 | gly[9], g1 = gly[10] << 8 | gly[11], cw = gly[12] << 8 | gly[13], ch = gly[14] << 8 | gly[15];
    uint32_t ssize = be32(gly + 16);
    int fmt = gly[20] << 8 | gly[21], cols = gly[22] << 8 | gly[23], rows = gly[24] << 8 | gly[25];
    int sw = gly[26] << 8 | gly[27], sh = gly[28] << 8 | gly[29];
    int method = map[8] << 8 | map[9], first = map[10] << 8 | map[11];
    if (cw != KF_CELL || ch != KF_CELL || fmt != 0 || method != 0 || cols <= 0 || rows <= 0 || g1 < g0 || g1 - g0 > 255 ||
        sw % 8 || sh % 8 || cols * KF_CELL > sw || rows * KF_CELL > sh || ssize != (uint32_t)(sw * sh / 2)) return false;
    int count = g1 - g0 + 1, per = cols * rows, sheets = (count + per - 1) / per;
    if ((size_t)(gly - b) + 0x20 + (size_t)sheets * ssize > size || !(s_kf_px = calloc((size_t)count, KF_CELL * KF_CELL))) return false;
    for (int g = 0; g < count; g++) {
        const uint8_t *sheet = gly + 0x20 + (size_t)(g / per) * ssize;
        int cx = (g % per) % cols * KF_CELL, cy = (g % per) / cols * KF_CELL;
        for (int y = 0; y < KF_CELL; y++)
            for (int x = 0; x < KF_CELL; x++) {
                int sx = cx + x, sy = cy + y; /* I4: 8x8 tiles, two texels a byte, high nibble first */
                size_t o = ((size_t)(sy / 8) * (sw / 8) + (size_t)(sx / 8)) * 32 + (size_t)(sy % 8) * 4 + (size_t)(sx % 8) / 2;
                uint8_t v = sheet[o];
                s_kf_px[(size_t)g * KF_CELL * KF_CELL + (size_t)y * KF_CELL + x] = (uint8_t)(((sx & 1) ? v & 15 : v >> 4) * 17);
            }
    }
    s_kf_first = first;
    s_kf_count = count;
    int w0 = wid[8] << 8 | wid[9], w1 = wid[10] << 8 | wid[11];
    for (int c = w0; c <= w1 && c - w0 < 256 && (uint32_t)(wid - b) + 12 + 2 * (c - w0) + 2 <= size; c++) {
        int k = c - w0 + first; /* WID1 codes are glyph indices from the first mapped code */
        if (k >= 0 && k < 256) s_kf_kern[k] = wid[12 + 2 * (c - w0)], s_kf_width[k] = wid[13 + 2 * (c - w0)];
    }
    return true;
}
static void kf_init(void) {
    uint32_t dol = disc_read_u32(0x420);
    uint8_t hdr[0x100];
    disc_read(dol, hdr, sizeof hdr);
    for (int s = 0; s < 11 && !s_kf_px; s++) { /* the data sections */
        uint32_t off = be32(hdr + 0x1C + 4 * s), size = be32(hdr + 0xAC + 4 * s);
        if (!off || !size || size > (8u << 20)) continue;
        uint8_t *d = malloc(size);
        if (!d) continue;
        disc_read(dol + off, d, size);
        for (uint32_t i = 0; i + 0x20 <= size && !s_kf_px; i += 4) {
            if (memcmp(d + i, "FONTbfn1", 8)) continue;
            uint32_t fs = be32(d + i + 8);
            if (fs >= 0x20 && fs <= size - i) kf_parse(d + i, fs);
        }
        free(d);
    }
    rt_log("pause art: %s", s_kf_px ? "the game's text font found" : "no text font in main.dol (MarioFont instead)");
}
/* One kartfont letter (rgb, a dark outline) of height size at (x, y) of a w x h canvas, or its
 * advance only: its ink starts kerning texels into the cell, so the cell is drawn that far left. */
static float kf_letter(uint8_t *dst, int w, int h, char ch, float x, int y, int size, uint32_t rgb, bool draw) {
    int c = (unsigned char)ch, g = c - s_kf_first;
    float sc = (float)size / KF_CELL;
    if (g < 0 || g >= s_kf_count) return 0;
    if (draw) {
        /* the glyph with a one-texel outline around it, in a cell one texel larger each side */
        enum { C = KF_CELL + 2 };
        uint8_t cell[C * C * 4];
        const uint8_t *src = s_kf_px + (size_t)g * KF_CELL * KF_CELL;
        for (int v = 0; v < C; v++)
            for (int u = 0; u < C; u++) {
                int in = 0, near = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int sx = u - 1 + dx, sy = v - 1 + dy;
                        int t = sx >= 0 && sx < KF_CELL && sy >= 0 && sy < KF_CELL ? src[sy * KF_CELL + sx] : 0;
                        if (!dx && !dy) in = t;
                        if (t > near) near = t;
                    }
                uint8_t *o = cell + ((size_t)v * C + u) * 4;
                float a = in / 255.0f, oa = near / 255.0f * 0.85f;
                float ta = a + oa * (1 - a);
                for (int k = 0; k < 3; k++) {
                    float fg = (float)(rgb >> (16 - 8 * k) & 0xFF), bg = 18.0f;
                    o[k] = (uint8_t)(ta > 0 ? (fg * a + bg * oa * (1 - a)) / ta + 0.5f : 0);
                }
                o[3] = (uint8_t)(ta * 255.0f + 0.5f);
            }
        Img m = {cell, C, C, C * 4};
        put_img(dst, w, h, (int)(x - (s_kf_kern[c] + 1) * sc + 0.5f), (int)(y - sc + 0.5f), (int)(C * sc + 0.5f), (int)(C * sc + 0.5f), m);
    }
    return (float)s_kf_width[c] * sc;
}
static float kf_text(uint8_t *dst, int w, int h, const char *s, float x, int y, int size, uint32_t rgb, bool draw) {
    float x0 = x;
    for (; *s; s++) x += kf_letter(dst, w, h, *s, x, y, size, rgb, draw);
    return x - x0;
}

/* Coverage (0-1, a pixel of smoothing) of a rounded rectangle at pixel centre (px, py). */
static float round_rect(float px, float py, float x0, float y0, float x1, float y1, float r) {
    float qx = fabsf(px - (x0 + x1) / 2) - (x1 - x0) / 2 + r, qy = fabsf(py - (y0 + y1) / 2) - (y1 - y0) / 2 + r;
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0, in = qx > qy ? qx : qy;
    float d = sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - r, c = 0.5f - d;
    return c < 0 ? 0 : c > 1 ? 1 : c;
}
/* The REAR VIEW and JUMP boxes (NSingle's GameCube Y and L icon panes, see patch_layout), W x H,
 * drawn like the game's: a blue rounded border, a dark see-through inside, a blue line under the
 * header, the buttons b on the top edge and the name in yellow MarioFont letters. 2 pixels a
 * layout unit, REAR_MARGIN above the box for the buttons. */
static void box_draw(uint8_t *px, int W, int H, const Img *b, int n, const char *name) {
    const int m = (int)(REAR_MARGIN * 2);
    const float bh = (float)(H - m);
    for (int j = m; j < H; j++)
        for (int i = 0; i < W; i++) {
            float x = (float)i + 0.5f, y = (float)j + 0.5f;
            float outer = round_rect(x, y, 0, (float)m, (float)W, (float)H, 12);
            float inner = round_rect(x, y, 4, (float)m + 4, (float)W - 4, (float)H - 4, 9);
            float fill = round_rect(x, y, 6, (float)m + 6, (float)W - 6, (float)H - 6, 7);
            /* premultiplied: border, its dark inner rim, the inside */
            float c[4] = {0};
            const float cols[3][4] = {{56, 104, 160, 1.0f}, {6, 8, 14, 0.9f}, {14, 16, 26, 0.78f}};
            const float cov[3] = {outer - inner, inner - fill, fill};
            for (int k = 0; k < 3; k++) {
                float a = cols[k][3] * cov[k];
                for (int ch = 0; ch < 3; ch++) c[ch] += cols[k][ch] * a;
                c[3] += a;
            }
            /* the header's line */
            float ly = (float)m + bh * 0.29f;
            if (y > ly - 1.5f && y < ly + 1.5f && x > 6 && x < (float)W - 6) {
                c[0] = 61, c[1] = 104, c[2] = 155, c[3] = 1;
            }
            uint8_t *q = px + ((size_t)j * W + (size_t)i) * 4;
            if (c[3] <= 0) continue;
            if (c[3] < 1) for (int ch = 0; ch < 3; ch++) c[ch] /= c[3];
            for (int ch = 0; ch < 3; ch++) q[ch] = (uint8_t)(c[ch] + 0.5f);
            q[3] = (uint8_t)(c[3] * 255.0f + 0.5f);
        }
    put_row(px, W, H, W / 2, m + (int)(HEADER_Y * 2), (int)(ICON_H * 2), b, n);
    /* the name as the other boxes': kartfont about 23 units high, centred at 61% of the box */
    float (*text)(uint8_t *, int, int, const char *, float, int, int, uint32_t, bool) = s_kf_px ? kf_text : font_text;
    const int size = s_kf_px ? 46 : 36;
    float tw = text(px, W, H, name, 0, 0, size, 0, false);
    text(px, W, H, name, ((float)W - tw) / 2, m + (int)(bh * 0.61f) - size / 2, size, 0xFFD800u, true);
}

static void upload(unsigned id, const uint8_t *rgba) {
    if (!s_tex[id]) glGenTextures(1, &s_tex[id]);
    glBindTexture(GL_TEXTURE_2D, s_tex[id]);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, SLOTS[id].w, SLOTS[id].h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindTexture(GL_TEXTURE_2D, 0);
}

/* Rebuild the icons when the mapping or the pausing controller changes. */
static void refresh_icons(int kind) {
    uint64_t key = fnv_bytes(FNV_INIT, g_set.map, sizeof g_set.map) ^ (uint64_t)kind;
    if (key == s_key) return;
    s_key = key;
    for (unsigned id = S_L; id <= S_B; id++) {
        int w = SLOTS[id].w, h = SLOTS[id].h, s = w < h ? w : h;
        uint8_t *px = calloc((size_t)w * h, 4);
        if (!px) continue;
        if (id == S_STICK) {
            /* STEER: the stick, and the D-pad where it steers (map_full: not on a sideways Joy-Con) */
            if (kind == PAUSE_GC) {
                if (s_gc_stick) put_cell(px, w, (w - s) / 2, 0, s, s_gc_stick, GC_STICK_SIZE * 4, GC_STICK_SIZE);
            } else if (kind == PAUSE_FULL) {
                put_badge(px, w, 0, 0, s, B_STICK);
                put_badge(px, w, w - s, 0, s, B_DPAD);
            } else {
                put_badge(px, w, (w - s) / 2, 0, s, B_STICK);
            }
        } else if (id == S_Y) {
            Img b[3];
            box_draw(px, w, h, b, slot_images(S_Y, kind, b), "REAR VIEW");
        } else if (id == S_L) { /* JUMP (L's own buttons show in DRIFT's row): its button, none on a GameCube pad */
            int j[3], nj = kind == PAUSE_GC ? 0 : badges_for(MAP_JUMP, kind, j, 3);
            Img b[3];
            for (int k = 0; k < nj; k++) b[k] = badge_img(j[k]);
            box_draw(px, w, h, b, nj, "JUMP");
        } else {
            Img b[3];
            int n = slot_images(id, kind, b);
            put_row(px, w, h, w / 2, h / 2, h, b, n);
        }
        upload(id, px);
        free(px);
    }
}

/* "km/h" in the game's speedometer lettering (its "mph": white pixel letters 8 high, ascenders 3
 * more, a black edge one pixel wide all round, letters two pixels apart), on the 64x16 grid, drawn
 * 4x. Ink masks, rows 2-11 of the label. */
static void build_kmh(void) {
    static const char *const K[10] = {"###.....", "###.....", "###.....", "###..###", "###.###.", "######..", "######..", "###.###.", "###..###", "###..###"};
    static const char *const MM[10] = {"..........", "..........", "..........", "##########", "##########", "##########", "##..##..##", "##..##..##", "##..##..##", "##..##..##"};
    static const char *const SL[10] = {"....##", "....##", "...##.", "...##.", "..##..", "..##..", ".##...", ".##...", "##....", "##...."};
    static const char *const H[10] = {"###.....", "###.....", "###.....", "###.....", "########", "########", "###..###", "###..###", "###..###", "###..###"};
    const char *const *glyph[4] = {K, MM, SL, H};
    uint8_t ink[16][64] = {{0}};
    int x = 12;
    for (int g = 0; g < 4; g++) {
        int gw = (int)strlen(glyph[g][0]);
        for (int r = 0; r < 10; r++)
            for (int i = 0; i < gw; i++)
                if (glyph[g][r][i] == '#') ink[2 + r][x + i] = 1;
        x += gw + 3; /* two edge pixels between letters */
    }
    uint8_t *px = calloc((size_t)KMH_W * KMH_H, 4);
    if (!px) return;
    for (int y = 0; y < 16; y++)
        for (int i = 0; i < 64; i++) {
            bool in = ink[y][i], edge = false;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int yy = y + dy, xx = i + dx;
                    edge |= yy >= 0 && yy < 16 && xx >= 0 && xx < 64 && ink[yy][xx];
                }
            if (!edge) continue;
            uint8_t v = in ? 255 : 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    uint8_t *o = px + (((size_t)y * 4 + sy) * KMH_W + (size_t)i * 4 + sx) * 4;
                    o[0] = o[1] = o[2] = v;
                    o[3] = 255;
                }
        }
    upload(S_KMH, px);
    free(px);
}

/* The pause screen's Switch, fitted to the full height of a PAD_W x PAD_H picture and centred. */
static void build_menupad(void) {
    uint8_t *px = calloc((size_t)PAD_W * PAD_H, 4);
    if (!px) return;
    float sc = (float)CON_H / PAD_H;
    int dw = (int)(CON_W / sc + 0.5f), x0 = (PAD_W - dw) / 2;
    for (int j = 0; j < PAD_H; j++)
        for (int i = 0; i < dw && x0 + i < PAD_W; i++) {
            /* box filter over the source pixels this one covers, premultiplied */
            float sx0 = i * sc, sy0 = j * sc, acc[4] = {0}, wsum = 0;
            for (int y = (int)sy0; y < (int)(sy0 + sc) && y < CON_H; y++)
                for (int x = (int)sx0; x < (int)(sx0 + sc) && x < CON_W; x++) {
                    const uint8_t *q = pause_console_rgba + ((size_t)y * CON_W + x) * 4;
                    float a = q[3] / 255.0f;
                    for (int c = 0; c < 3; c++) acc[c] += q[c] * a;
                    acc[3] += a;
                    wsum += 1;
                }
            if (wsum <= 0 || acc[3] <= 0) continue;
            uint8_t *o = px + ((size_t)j * PAD_W + x0 + i) * 4;
            for (int c = 0; c < 3; c++) o[c] = (uint8_t)(acc[c] / acc[3] + 0.5f);
            o[3] = (uint8_t)(acc[3] / wsum * 255.0f + 0.5f);
        }
    upload(S_MENUPAD, px);
    free(px);
}

static GLuint upload_rgba(GLuint tex, int w, int h, const uint8_t *rgba) {
    if (!tex) glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

static unsigned bar_texture(int i) {
    if (!s_bar_tex[i]) s_bar_tex[i] = upload_rgba(0, BAR_W, BAR_H, s_bar[i]);
    return s_bar_tex[i];
}

/* "PRESS (Y) OR (X) FOR MORE CUPS" (more: "CUPS"), or its width; badges by, bx: the buttons' icons. */
static float hint_line(uint8_t *dst, const char *more, float x, int by, int bx, bool draw) {
    /* pad: moved down a little, clear of the bar above (the badge art has a margin) */
    const int size = 32, badge = 48, step = 40, pad = 6, y = (HINT_H - size) / 2 + pad;
    char tail[48];
    snprintf(tail, sizeof tail, " FOR MORE %s", more);
    const char *parts[5] = {"PRESS ", NULL, " OR ", NULL, tail};
    const int badges[5] = {0, by, 0, bx, 0};
    float x0 = x;
    for (int k = 0; k < 5; k++) {
        if (!parts[k]) {
            if (draw) put_badge(dst, HINT_W, (int)(x + 0.5f) - (badge - step) / 2, (HINT_H - badge) / 2 + pad, badge, badges[k]);
            x += step;
            continue;
        }
        for (const char *s = parts[k]; *s; s++) x += font_letter(dst, HINT_W, HINT_H, *s, x, y, size, 0xFFFFFFu, draw);
    }
    return x - x0;
}

static unsigned hint_texture(int i) {
    /* a menu: always the standard buttons, never BUTTON MAPPING's (that is for driving only) */
    const int by = B_Y, bx = B_X;
    uint64_t key = (uint64_t)by << 8 | (uint64_t)bx;
    if (s_hint_tex[i] && key == s_hint_key[i]) return s_hint_tex[i];
    int aw, ah;
    if (!uifont_atlas(&aw, &ah)) return 0; /* no MarioFont on the disc: no hint */
    uint8_t *px = calloc((size_t)HINT_W * HINT_H, 4);
    if (!px) return s_hint_tex[i];
    /* centred under the bar's words */
    float w = hint_line(px, BAR_MORE[i], 0, by, bx, false), x = (float)(s_bar_l[i] + s_bar_r[i]) - w / 2;
    if (x < 0) x = 0;
    if (x + w > HINT_W) x = HINT_W - w;
    hint_line(px, BAR_MORE[i], x, by, bx, true);
    s_hint_tex[i] = upload_rgba(s_hint_tex[i], HINT_W, HINT_H, px);
    free(px);
    s_hint_key[i] = key;
    return s_hint_tex[i];
}

/* The character select screen's page hint (menu.c menu_char_hint): "PRESS (Y) OR (X) FOR MORE
 * CHARACTERS (1/3)" (page of pages, from 1) as the cup page draws its line, centred in a HINT_W x
 * HINT_H picture; redrawn when the page changes. */
unsigned pause_art_char_hint(int page, int pages, int *w, int *h) {
    static GLuint tex;
    static int key = -1;
    *w = HINT_W;
    *h = HINT_H;
    int aw, ah;
    if ((tex && key == page * 1000 + pages) || !uifont_atlas(&aw, &ah)) return tex;
    uint8_t *px = calloc((size_t)HINT_W * HINT_H, 4);
    if (!px) return tex;
    char more[32];
    snprintf(more, sizeof more, "CHARACTERS (%d/%d)", page, pages);
    float lw = hint_line(px, more, 0, B_Y, B_X, false), x = (HINT_W - lw) / 2;
    hint_line(px, more, x < 0 ? 0 : x, B_Y, B_X, true);
    tex = upload_rgba(tex, HINT_W, HINT_H, px);
    key = page * 1000 + pages;
    free(px);
    /* drawn by the menu overlay without a sampler: no mipmaps, so a plain filter (the default
     * mipmapped one makes it incomplete: black) */
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

unsigned pause_art_below(unsigned id) {
    if (id == BAR_FIRST || id == BAR_FIRST + 1) s_hint_tick = armGetSystemTick();
    return id >= BAR_FIRST && id < BAR_FIRST + BAR_COUNT && s_bar[id - BAR_FIRST] ? hint_texture((int)(id - BAR_FIRST)) : 0;
}

float pause_art_shift(unsigned id) {
    if (id == RND_WORD) s_random_tick = armGetSystemTick();
    if (id != S_L && id != S_R && id != RND_PLUS && id != RND_WORD) return 0;
    /* the bar is drawn first; the character select's L + R (no hint bar) stays */
    return s_hint_tick && armTicksToNs(armGetSystemTick() - s_hint_tick) < 200000000ull ? RANDOM_DY : 0;
}

bool pause_art_random_shown(void) {
    u64 t = s_random_tick;
    return t && armTicksToNs(armGetSystemTick() - t) < 200000000ull;
}

unsigned pause_art_texture(unsigned id) {
    if (id >= BAR_FIRST && id < BAR_FIRST + BAR_COUNT) return s_bar[id - BAR_FIRST] ? bar_texture((int)(id - BAR_FIRST)) : 0;
    int kind = g_pause_kind;
    if (id == S_MENUPAD) {
        if (!s_tex[id]) build_menupad();
        return s_tex[id];
    }
    if (id == S_KMH) {
        if (!g_speed_kmh) return 0; /* the game's "mph" */
        if (!s_tex[id]) build_kmh();
        return s_tex[id];
    }
    /* a GameCube controller: the game's own controller picture; the buttons are ours (their panes changed) */
    if (id == 0 || id >= S_COUNT || (kind == PAUSE_GC && id == S_CONSOLE)) return 0;
    if (id == S_CONSOLE) {
        if (!s_tex[id]) upload(id, pause_console_rgba);
        return s_tex[id];
    }
    /* L and R are the JUMP box and DRIFT's row only on the pause screen: the character select's
     * "L + R: RANDOM" uses the same two pictures. A race is on screen while Race2D::draw ran within
     * the last half second (it keeps drawing under the pause screen). */
    if (id == S_L || id == S_R) {
        extern volatile uint32_t g_race_frames;
        static uint32_t seen;
        static u64 tick;
        if (g_race_frames != seen) seen = g_race_frames, tick = armGetSystemTick();
        if (!tick || armTicksToNs(armGetSystemTick() - tick) >= 500000000ull) return 0;
    }
    refresh_icons(kind);
    return s_tex[id];
}
