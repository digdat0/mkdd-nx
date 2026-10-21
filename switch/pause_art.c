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
enum { S_CONSOLE = 1, S_L, S_R, S_Z, S_X, S_Y, S_STICK, S_A, S_B, S_MENUPAD, S_COUNT };
/* S_MENUPAD: the GameCube controller the "how many players" screen shows once per player
 * (menu.arc, 80x56), drawn as the pause screen's Switch at the same height. Ours is 2x the size
 * for a sharper picture: same shape, so the game's pane shows it the same. */
#define PAD_W 160
#define PAD_H 112
static const struct { const char *file; int gc; int w, h; const char *arc; } SLOTS[S_COUNT] = {
    [S_CONSOLE] = {"fs_gc_contorollerop_s3tc.bti", -1, CON_W, CON_H},
    [S_L] = {"l_trigger.bti", GC_L, 64, 64},       [S_R] = {"r_trigger.bti", GC_R, 64, 64},
    [S_Z] = {"fs_btz_s3tc.bti", GC_Z, 64, 64},     [S_X] = {"fs_btx_ia4.bti", GC_X, 64, 96},
    [S_Y] = {"fs_bty_ia4.bti", GC_Y, 96, 64},      [S_STICK] = {"fs_btcst_ia4.bti", -1, 128, 60},
    [S_A] = {"fs_bta_s3tc.bti", GC_A, 64, 64},     [S_B] = {"fs_btb_s3tc.bti", GC_B, 64, 64},
    [S_MENUPAD] = {"gc_controller.bti", -1, PAD_W, PAD_H, "SceneData/English/menu.arc"},
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
static uint64_t s_key = ~0ull;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* Layout changes to pause2.blo (MRAM_Locale/<language>/MRAMLoc.arc), single-player (NSingle) and
 * two-per-kart (NTag) panels, made as the layout is read from the disc: the game's lines from each
 * box to its GameCube button get zero width (the boxes and icons say which button does what), and
 * the STEER icon (PICT_062, PICT_032) gets room for the stick and the D-pad side by side. */
#define STEER_W 62.0f
static const char *const STEER_PANES[] = {"PICT_062", "PICT_032"};
static const char *const LINE_PANES[] = {
    "PICT_042", "PICT_043", "PICT_044", "PICT_045", "PICT_046", "PICT_047", "PICT_048", "PICT_049",
    "PICT_051", "PICT_052", "PICT_053", "PICT_054", "PICT_055", "PICT_057", "PICT_070", "PICT_024",
    "PICT_008", "PICT_009", "PICT_016", "PICT_017", "PICT_018", "PICT_019", "PICT_020", "PICT_021",
    "PICT_022", "PICT_023", "PICT_026", "PICT_027", "PICT_028", "PICT_029", "PICT_005",
};

static unsigned patch_layout(const char *arc) {
    uint64_t off;
    uint32_t size, n = 0;
    if (!arc_locate(arc, "pause2.blo", &off, &size) || size > (1u << 20)) return 0;
    uint8_t *b = malloc(size);
    if (!b) return 0;
    disc_read(off, b, size);
    if (size > 0x20 && !memcmp(b, "SCRNblo2", 8)) {
        static const uint8_t zero[4];
        uint32_t wide;
        uint8_t steer_w[4];
        float f = STEER_W;
        memcpy(&wide, &f, 4);
        for (int k = 0; k < 4; k++) steer_w[k] = (uint8_t)(wide >> (24 - 8 * k));
        for (uint32_t i = 0x20; i + 8 <= size;) {
            uint32_t bsz = be32(b + i + 4);
            if (bsz < 8 || i + bsz > size) break;
            if (!memcmp(b + i, "PIC2", 4)) {
                /* pan2 block: tag (8 bytes) at +16, width at +0x20 */
                for (uint32_t j = i + 8; j + 0x24 <= i + bsz; j++) {
                    if (memcmp(b + j, "pan2", 4)) continue;
                    for (unsigned k = 0; k < sizeof LINE_PANES / sizeof LINE_PANES[0]; k++)
                        if (!memcmp(b + j + 16, LINE_PANES[k], 8)) { disc_patch(off + j + 0x20, zero, 4); n++; }
                    for (unsigned k = 0; k < sizeof STEER_PANES / sizeof STEER_PANES[0]; k++)
                        if (!memcmp(b + j + 16, STEER_PANES[k], 8)) { disc_patch(off + j + 0x20, steer_w, 4); n++; }
                    break;
                }
            }
            i += bsz;
        }
    }
    free(b);
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
}

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
            }
        }
        free(b);
    }
    rt_log("pause art: %u of %d pictures recognised", n, S_COUNT - 1);
    bars_init();
}

/* The Switch buttons (badges) that give the GameCube button gc on the pausing controller. */
static int badges_for(int gc, int kind, int out[2]) {
    int n = 0;
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
    for (unsigned i = 0; i < sizeof BITS / sizeof BITS[0] && n < 2; i++)
        if (g_set.map[gc] & BITS[i].bit) out[n++] = BITS[i].badge;
    return n;
}

/* Bilinear copy of a cell x cell RGBA square (rows stride bytes apart) into a size x size square at
 * (x, y) of a w-wide RGBA canvas (premultiplied while filtering, so edges don't darken). */
static void put_cell(uint8_t *dst, int w, int x, int y, int size, const uint8_t *src, int stride, int cell) {
    for (int j = 0; j < size; j++)
        for (int i = 0; i < size; i++) {
            float fx = ((float)i + 0.5f) * cell / (float)size - 0.5f, fy = ((float)j + 0.5f) * cell / (float)size - 0.5f;
            int x0 = (int)fx, y0 = (int)fy;
            if (fx < 0) { fx = 0; x0 = 0; }
            if (fy < 0) { fy = 0; y0 = 0; }
            int x1 = x0 + 1 < cell ? x0 + 1 : x0, y1 = y0 + 1 < cell ? y0 + 1 : y0;
            float ax = fx - (float)x0, ay = fy - (float)y0, acc[4] = {0};
            const int xs[2] = {x0, x1}, ys[2] = {y0, y1};
            for (int k = 0; k < 4; k++) {
                const uint8_t *p = src + (size_t)ys[k >> 1] * stride + (size_t)xs[k & 1] * 4;
                float wt = ((k & 1) ? ax : 1 - ax) * ((k >> 1) ? ay : 1 - ay), a = p[3] / 255.0f;
                for (int c = 0; c < 3; c++) acc[c] += p[c] * a * wt;
                acc[3] += a * wt;
            }
            uint8_t *q = dst + ((size_t)(y + j) * w + (size_t)(x + i)) * 4;
            if (acc[3] <= 0.0f) continue;
            for (int c = 0; c < 3; c++) q[c] = (uint8_t)(acc[c] / acc[3] + 0.5f);
            q[3] = (uint8_t)(acc[3] * 255.0f + 0.5f);
        }
}
static void put_badge(uint8_t *dst, int w, int x, int y, int size, int b) {
    const uint8_t *src = pause_badges_rgba + ((size_t)(b / BADGE_COLS) * CELL * BADGE_COLS * CELL + (size_t)(b % BADGE_COLS) * CELL) * 4;
    put_cell(dst, w, x, y, size, src, BADGE_COLS * CELL * 4, CELL);
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
        if (kind == PAUSE_GC && id != S_STICK) continue; /* the game's own icons */
        uint8_t *px = calloc((size_t)w * h, 4);
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
            upload(id, px);
            free(px);
            continue;
        }
        int b[2], n = badges_for(SLOTS[id].gc, kind, b);
        if (n == 1) put_badge(px, w, (w - s) / 2, (h - s) / 2, s, b[0]);
        else if (n == 2) { /* two buttons: smaller, one top-left, one bottom-right */
            int hs = s * 5 / 8, ox = (w - s) / 2, oy = (h - s) / 2;
            put_badge(px, w, ox, oy, hs, b[0]);
            put_badge(px, w, ox + s - hs, oy + s - hs, hs, b[1]);
        }
        upload(id, px);
        free(px);
    }
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

/* One MarioFont letter (white, dark outline) of height size at (x, y), or its advance only. */
static float hint_letter(uint8_t *dst, char c, float x, int y, int size, bool draw) {
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
                o[0] = o[1] = o[2] = s[0];
                o[3] = s[1];
            }
        int px = (int)(x - g0 * sc + 0.5f);
        if (px >= 0 && px + size <= HINT_W && y >= 0 && y + size <= HINT_H) put_cell(dst, HINT_W, px, y, size, cell, 32 * 4, 32);
    }
    return (g1 - g0 - 2) * sc;
}

/* "PRESS (Y) OR (X) FOR MORE CUPS", or its width; badges are the Switch buttons for GameCube Y, X. */
static float hint_line(uint8_t *dst, int i, float x, int by, int bx, bool draw) {
    /* pad: moved down a little, clear of the bar above (the badge art has a margin) */
    const int size = 32, badge = 48, step = 40, pad = 6, y = (HINT_H - size) / 2 + pad;
    char tail[32];
    snprintf(tail, sizeof tail, " FOR MORE %s", BAR_MORE[i]);
    const char *parts[5] = {"PRESS ", NULL, " OR ", NULL, tail};
    const int badges[5] = {0, by, 0, bx, 0};
    float x0 = x;
    for (int k = 0; k < 5; k++) {
        if (!parts[k]) {
            if (draw) put_badge(dst, HINT_W, (int)(x + 0.5f) - (badge - step) / 2, (HINT_H - badge) / 2 + pad, badge, badges[k]);
            x += step;
            continue;
        }
        for (const char *s = parts[k]; *s; s++) x += hint_letter(dst, *s, x, y, size, draw);
    }
    return x - x0;
}

static unsigned hint_texture(int i) {
    int y_b[2], x_b[2];
    int by = badges_for(GC_Y, PAUSE_FULL, y_b) ? y_b[0] : B_Y, bx = badges_for(GC_X, PAUSE_FULL, x_b) ? x_b[0] : B_X;
    uint64_t key = (uint64_t)by << 8 | (uint64_t)bx;
    if (s_hint_tex[i] && key == s_hint_key[i]) return s_hint_tex[i];
    int aw, ah;
    if (!uifont_atlas(&aw, &ah)) return 0; /* no MarioFont on the disc: no hint */
    uint8_t *px = calloc((size_t)HINT_W * HINT_H, 4);
    if (!px) return s_hint_tex[i];
    /* centred under the bar's words */
    float w = hint_line(px, i, 0, by, bx, false), x = (float)(s_bar_l[i] + s_bar_r[i]) - w / 2;
    if (x < 0) x = 0;
    if (x + w > HINT_W) x = HINT_W - w;
    hint_line(px, i, x, by, bx, true);
    s_hint_tex[i] = upload_rgba(s_hint_tex[i], HINT_W, HINT_H, px);
    free(px);
    s_hint_key[i] = key;
    return s_hint_tex[i];
}

unsigned pause_art_below(unsigned id) {
    return id >= BAR_FIRST && id < BAR_FIRST + BAR_COUNT && s_bar[id - BAR_FIRST] ? hint_texture((int)(id - BAR_FIRST)) : 0;
}

unsigned pause_art_texture(unsigned id) {
    if (id >= BAR_FIRST && id < BAR_FIRST + BAR_COUNT) return s_bar[id - BAR_FIRST] ? bar_texture((int)(id - BAR_FIRST)) : 0;
    int kind = g_pause_kind;
    if (id == S_MENUPAD) {
        if (!s_tex[id]) build_menupad();
        return s_tex[id];
    }
    if (id == 0 || id >= S_COUNT || (kind == PAUSE_GC && id != S_STICK)) return 0;
    if (id == S_CONSOLE) {
        if (!s_tex[id]) upload(id, pause_console_rgba);
        return s_tex[id];
    }
    refresh_icons(kind);
    return s_tex[id];
}
