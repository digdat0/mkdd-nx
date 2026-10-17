/* The - menu (options, colour, controllers, button mapping, debugging, exit) and the on-screen
 * overlay, styled after the game's own menus: its MarioFont letters (read from the disc, see
 * ui_font.c), a navy star panel with a red border and a checkered title band. Drawn after the
 * game image with one small instanced program (see "drawing"); the start-up screen
 * (menu_loading) uses the same pieces. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>
#include <GLES3/gl32.h>

#include "menu.h"
#include "render_gl.h"
#include "hdtex.h"
#include "ui_font.h"
#include "../runtime/rt.h"

Settings g_set;
static char s_path[512];

static const uint64_t DEFAULT_MAP[GC_COUNT] = {
    [GC_A] = HidNpadButton_A, [GC_B] = HidNpadButton_B, [GC_X] = HidNpadButton_X, [GC_Y] = HidNpadButton_Y,
    [GC_Z] = HidNpadButton_L | HidNpadButton_R, [GC_L] = HidNpadButton_ZL, [GC_R] = HidNpadButton_ZR,
    [GC_START] = HidNpadButton_Plus,
};
static const char *GC_NAMES[GC_COUNT] = {"A", "B", "X", "Y", "Z", "L", "R", "START"};
static const char *GC_KEYS[GC_COUNT] = {"map_a", "map_b", "map_x", "map_y", "map_z", "map_l", "map_r", "map_start"};

/* Switch buttons a GameCube button can be mapped to (− is reserved for the menu). */
static const struct { uint64_t bit; const char *name; } SW_BUTTONS[] = {
    {HidNpadButton_A, "A"}, {HidNpadButton_B, "B"}, {HidNpadButton_X, "X"}, {HidNpadButton_Y, "Y"},
    {HidNpadButton_L, "L"}, {HidNpadButton_R, "R"}, {HidNpadButton_ZL, "ZL"}, {HidNpadButton_ZR, "ZR"},
    {HidNpadButton_Plus, "+"}, {HidNpadButton_StickL, "LS"}, {HidNpadButton_StickR, "RS"},
    {HidNpadButton_Up, "UP"}, {HidNpadButton_Down, "DOWN"}, {HidNpadButton_Left, "LEFT"}, {HidNpadButton_Right, "RIGHT"},
};
#define N_SW (sizeof SW_BUTTONS / sizeof SW_BUTTONS[0])
static uint64_t mappable(void) {
    uint64_t m = 0;
    for (unsigned i = 0; i < N_SW; i++) m |= SW_BUTTONS[i].bit;
    return m;
}

/* ---- settings.ini ---- */
static void defaults(void) {
    g_set.wide = false;
    g_set.show_fps = 0;
    g_set.rumble = 2;
    g_set.unlock_all = false;
    g_set.logging = 1;
    g_set.hd_textures = true;
    g_set.fov[0] = 0; /* 4:3: the game's view */
    g_set.fov[1] = 2; /* 16:9: wider */
    g_set.aniso = 0;
    g_set.border = false;
    g_set.docked_res = 1;
    g_set.profiler = false;
    memcpy(g_set.map, DEFAULT_MAP, sizeof g_set.map);
}

static const uint32_t LOG_MASKS[4] = {0, LOG_GAME, LOG_GAME | LOG_AUDIO, LOG_AUDIO};
static const char *LOG_NAMES[4] = {"DISABLED", "GAME", "GAME + AUDIO", "AUDIO"};
static void apply_logging(void) {
    g_log_mask = LOG_MASKS[g_set.logging & 3];
    diag_flush();
}

void settings_load(const char *path) {
    snprintf(s_path, sizeof s_path, "%s", path);
    defaults();
    FILE *f = fopen(path, "r");
    if (!f) { apply_logging(); return; }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        /* display: 0 = 4:3, otherwise 16:9 (older builds: 1 stretched, 2 wide view; "widescreen" before 0814) */
        if (!strcmp(k, "display") || !strcmp(k, "widescreen")) g_set.wide = atoi(v) != 0;
        else if (!strcmp(k, "show_fps")) g_set.show_fps = atoi(v) < 0 ? 0 : atoi(v) > 2 ? 2 : atoi(v);
        else if (!strcmp(k, "rumble")) g_set.rumble = atoi(v) < 0 ? 0 : atoi(v) > 2 ? 2 : atoi(v);
        else if (!strcmp(k, "unlock_all")) g_set.unlock_all = atoi(v) != 0;
        else if (!strcmp(k, "logging")) g_set.logging = atoi(v) & 3;
        else if (!strcmp(k, "hd_textures")) g_set.hd_textures = atoi(v) != 0;
        else if (!strcmp(k, "fov_4_3")) g_set.fov[0] = atoi(v) < 0 ? 0 : atoi(v) > 2 ? 2 : atoi(v);
        else if (!strcmp(k, "fov_16_9")) g_set.fov[1] = atoi(v) < 0 ? 0 : atoi(v) > 2 ? 2 : atoi(v);
        else if (!strcmp(k, "aniso")) g_set.aniso = atoi(v) < 0 ? 0 : atoi(v) > 4 ? 4 : atoi(v);
        else if (!strcmp(k, "border")) g_set.border = atoi(v) != 0;
        else if (!strcmp(k, "docked_res")) g_set.docked_res = atoi(v) != 0;
        else if (!strcmp(k, "profiler")) g_set.profiler = atoi(v) != 0;
        else
            for (int i = 0; i < GC_COUNT; i++)
                if (!strcmp(k, GC_KEYS[i])) g_set.map[i] = strtoull(v, NULL, 16) & mappable();
    }
    fclose(f);
    apply_logging();
    rt_log("settings: 16:9 %d, fov %d / %d, fps %d, logging %d, rumble %d, unlock all %d", g_set.wide, g_set.fov[0], g_set.fov[1], g_set.show_fps,
           g_set.logging, g_set.rumble, g_set.unlock_all);
}

void settings_save(void) {
    FILE *f = fopen(s_path, "w");
    if (!f) return;
    fprintf(f, "display=%d\nshow_fps=%d\nlogging=%d\nrumble=%d\nunlock_all=%d\nhd_textures=%d\nfov_4_3=%d\nfov_16_9=%d\naniso=%d\nborder=%d\n"
            "docked_res=%d\nprofiler=%d\n", g_set.wide, g_set.show_fps, g_set.logging, g_set.rumble, g_set.unlock_all, g_set.hd_textures,
            g_set.fov[0], g_set.fov[1], g_set.aniso, g_set.border, g_set.docked_res, g_set.profiler);
    for (int i = 0; i < GC_COUNT; i++) fprintf(f, "%s=%llx\n", GC_KEYS[i], (unsigned long long)g_set.map[i]);
    fclose(f);
}

/* ---- color.ini ---- */
ColorSet g_color;
static char s_color_path[512];
static bool s_color_dirty;
static const char *COLOR_NAMES[COLOR_MODES] = {"ACCURATE", "VIVID", "CUSTOM"};
/* VIVID: a little brighter mid-tones, more saturation and contrast, for the handheld LCD. */
static const int VIVID[3] = {2, 2, 4};
static int clamp10(int v) { return v < -10 ? -10 : v > 10 ? 10 : v; }

void color_load(const char *path) {
    snprintf(s_color_path, sizeof s_color_path, "%s", path);
    memset(&g_color, 0, sizeof g_color);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        int v = atoi(eq + 1);
        if (!strcmp(line, "mode")) g_color.mode = v >= 0 && v < COLOR_MODES ? v : 0;
        else if (!strcmp(line, "brightness")) g_color.bright = clamp10(v);
        else if (!strcmp(line, "contrast")) g_color.contrast = clamp10(v);
        else if (!strcmp(line, "saturation")) g_color.sat = clamp10(v);
    }
    fclose(f);
    rt_log("color: %s (custom %d/%d/%d)", COLOR_NAMES[g_color.mode], g_color.bright, g_color.contrast, g_color.sat);
}

static void color_save(void) {
    if (!s_color_dirty || !s_color_path[0]) return;
    s_color_dirty = false;
    char buf[160];
    int n = snprintf(buf, sizeof buf, "mode=%d\nbrightness=%d\ncontrast=%d\nsaturation=%d\n", g_color.mode,
                     g_color.bright, g_color.contrast, g_color.sat);
    if (!plat_write_file_atomic(s_color_path, buf, (size_t)n)) rt_log("color: could not write %s", s_color_path);
}

/* The brightness/contrast/saturation the current mode uses. */
static void color_values(int v[3]) {
    if (g_color.mode == COLOR_VIVID) memcpy(v, VIVID, sizeof VIVID);
    else if (g_color.mode == COLOR_CUSTOM) { v[0] = g_color.bright; v[1] = g_color.contrast; v[2] = g_color.sat; }
    else v[0] = v[1] = v[2] = 0;
}

void color_adjust(float adj[3]) {
    int v[3];
    color_values(v);
    adj[0] = 1.0f / (1.0f + 0.04f * (float)v[0]); /* brightness: gamma, keeps black and white */
    adj[1] = 1.0f + 0.05f * (float)v[2];          /* saturation */
    adj[2] = 1.0f + 0.03f * (float)v[1];          /* contrast around mid-grey */
}

/* ---- menu state ---- */
/* Main page: what most players change. Advanced: picture and feel. Debugging: diagnostics. */
enum { PAGE_MAIN, PAGE_MAP, PAGE_EXIT, PAGE_COLOR, PAGE_DEBUG, PAGE_ADV, PAGE_COUNT };
enum { M_WIDE, M_HD, M_CTRL, M_MAP, M_ADV, M_DEBUG, M_EXIT, M_COUNT };
enum { A_COLOR, A_RUMBLE, A_FOV, A_ANISO, A_DOCKED, A_BORDER, A_BACK, A_COUNT };
static const char *FOV_NAMES[3] = {"NORMAL", "WIDE", "WIDER"};
static const char *ANISO_NAMES[5] = {"NORMAL", "2X", "4X", "8X", "16X"};
enum { D_FPS, D_UNLOCK, D_LOG, D_PROF, D_BACK, D_COUNT };
static const char *FPS_NAMES[3] = {"OFF", "FPS", "FPS + TIMES"};
static const char *RUMBLE_NAMES[3] = {"OFF", "LOW", "HIGH"};
static bool s_want_controllers;
enum { P_RESET = GC_COUNT, P_BACK, P_COUNT };
enum { K_MODE, K_BRIGHT, K_CONTRAST, K_SAT, K_RESET, K_BACK, K_COUNT };
static bool s_open;
static bool s_preview;          /* colour page with Y held: panel hidden to see the whole image */
static int s_page, s_sel[PAGE_COUNT];
static int s_capture = -1;      /* GameCube button waiting for a new Switch button */
static bool s_capture_armed;    /* all buttons were released since the capture started */
static bool s_release_wait;     /* menu closed: keep the game's pad neutral until buttons are released */
static char s_notice[64];       /* menu_open's reason, shown as the main page's hint until the cursor moves */

static void map_name(uint64_t m, char *out, size_t n) {
    out[0] = 0;
    for (unsigned i = 0; i < N_SW; i++)
        if (m & SW_BUTTONS[i].bit) {
            size_t l = strlen(out);
            snprintf(out + l, n - l, "%s%s", l ? "/" : "", SW_BUTTONS[i].name);
        }
    if (!out[0]) snprintf(out, n, "NONE");
}

static int page_rows(int page) {
    switch (page) {
    case PAGE_MAIN: return M_COUNT;
    case PAGE_MAP: return P_COUNT;
    case PAGE_COLOR: return K_COUNT;
    case PAGE_DEBUG: return D_COUNT;
    case PAGE_ADV: return A_COUNT;
    default: return 1;
    }
}

/* B goes back one page: colour to advanced, everything else to the main page. */
static int page_parent(int page) { return page == PAGE_COLOR ? PAGE_ADV : PAGE_MAIN; }

void menu_open(const char *notice) {
    if (s_capture >= 0) return;
    s_open = true;
    s_page = PAGE_MAIN;
    snprintf(s_notice, sizeof s_notice, "%s", notice ? notice : "");
}

bool menu_input(uint64_t down, uint64_t held, bool *quit) {
    if (s_capture >= 0) {
        uint64_t m = mappable();
        if (!s_capture_armed) {
            if (!(held & m)) s_capture_armed = true;
            return true;
        }
        if (down & HidNpadButton_Minus) { s_capture = -1; return true; }
        uint64_t b = down & m;
        if (b) {
            g_set.map[s_capture] = b & -b; /* lowest pressed button */
            s_capture = -1;
            settings_save();
        }
        return true;
    }
    if (!s_open) {
        if (s_release_wait) {
            if (held & (mappable() | HidNpadButton_Minus)) return true;
            s_release_wait = false;
        }
        if (down & HidNpadButton_Minus) { s_open = true; s_page = PAGE_MAIN; }
        return s_open;
    }
    const uint64_t UP = HidNpadButton_Up | HidNpadButton_StickLUp, DOWN = HidNpadButton_Down | HidNpadButton_StickLDown;
    const uint64_t SIDE = HidNpadButton_Left | HidNpadButton_Right | HidNpadButton_StickLLeft | HidNpadButton_StickLRight;
    int count = page_rows(s_page);
    int *sel = &s_sel[s_page];
    if (down & UP) *sel = (*sel + count - 1) % count;
    if (down & DOWN) *sel = (*sel + 1) % count;
    if (down & (UP | DOWN)) s_notice[0] = 0;
    bool ok = down & HidNpadButton_A, side = down & SIDE;
    bool left = down & (HidNpadButton_Left | HidNpadButton_StickLLeft);
    s_preview = s_page == PAGE_COLOR && (held & HidNpadButton_Y);
    if (down & (HidNpadButton_Minus | HidNpadButton_B)) {
        if (s_page == PAGE_MAIN || (down & HidNpadButton_Minus)) { s_open = false; s_release_wait = true; s_preview = false; s_notice[0] = 0; }
        else s_page = page_parent(s_page);
        color_save();
        return true;
    }
    if (s_page == PAGE_COLOR && (ok || side)) {
        int *val = *sel == K_BRIGHT ? &g_color.bright : *sel == K_CONTRAST ? &g_color.contrast : *sel == K_SAT ? &g_color.sat : NULL;
        if (*sel == K_MODE) {
            g_color.mode = (g_color.mode + (left ? COLOR_MODES - 1 : 1)) % COLOR_MODES;
            s_color_dirty = true;
        } else if (val && side) {
            /* Adjusting a value switches to CUSTOM, starting from what the current mode showed. */
            if (g_color.mode != COLOR_CUSTOM) {
                int v[3];
                color_values(v);
                g_color.bright = v[0]; g_color.contrast = v[1]; g_color.sat = v[2];
                g_color.mode = COLOR_CUSTOM;
            }
            *val = clamp10(*val + (left ? -1 : 1));
            s_color_dirty = true;
        } else if (*sel == K_RESET && ok) {
            g_color.mode = COLOR_ACCURATE;
            g_color.bright = g_color.contrast = g_color.sat = 0;
            s_color_dirty = true;
        } else if (*sel == K_BACK && ok) {
            s_page = PAGE_ADV;
            color_save();
        }
        return true;
    }
    if (s_page == PAGE_DEBUG && (ok || side)) {
        switch (*sel) {
        case D_FPS: g_set.show_fps = (g_set.show_fps + (left ? 2 : 1)) % 3; settings_save(); break;
        case D_UNLOCK: g_set.unlock_all = !g_set.unlock_all; cheats_set(g_set.unlock_all); settings_save(); break;
        case D_LOG:
            /* A / right: next mode, left: previous */
            g_set.logging = (g_set.logging + (left ? 3 : 1)) & 3;
            apply_logging();
            settings_save();
            break;
        case D_PROF: g_set.profiler = !g_set.profiler; settings_save(); break;
        case D_BACK: if (ok) s_page = PAGE_MAIN; break;
        }
        return true;
    }
    if (s_page == PAGE_ADV && (ok || side)) {
        switch (*sel) {
        case A_COLOR:
            if (ok) { s_page = PAGE_COLOR; s_sel[PAGE_COLOR] = 0; }
            else { g_color.mode = (g_color.mode + (left ? COLOR_MODES - 1 : 1)) % COLOR_MODES; s_color_dirty = true; color_save(); }
            break;
        case A_RUMBLE: g_set.rumble = (g_set.rumble + (left ? 2 : 1)) % 3; settings_save(); break;
        case A_FOV: g_set.fov[g_set.wide] = (g_set.fov[g_set.wide] + (left ? 2 : 1)) % 3; settings_save(); break;
        case A_ANISO: g_set.aniso = (g_set.aniso + (left ? 4 : 1)) % 5; settings_save(); break;
        case A_DOCKED: g_set.docked_res = !g_set.docked_res; settings_save(); break;
        case A_BORDER: g_set.border = !g_set.border; settings_save(); break;
        case A_BACK: if (ok) s_page = PAGE_MAIN; break;
        }
        return true;
    }
    if (s_page == PAGE_MAIN && (ok || side)) {
        switch (*sel) {
        case M_WIDE: g_set.wide = !g_set.wide; settings_save(); break;
        case M_HD: g_set.hd_textures = !g_set.hd_textures; settings_save(); break;
        case M_CTRL: if (ok) { s_want_controllers = true; s_open = false; s_release_wait = true; } break;
        case M_MAP: if (ok) { s_page = PAGE_MAP; s_sel[PAGE_MAP] = 0; } break;
        case M_ADV: if (ok) { s_page = PAGE_ADV; s_sel[PAGE_ADV] = 0; } break;
        case M_DEBUG: if (ok) { s_page = PAGE_DEBUG; s_sel[PAGE_DEBUG] = 0; } break;
        case M_EXIT: if (ok) s_page = PAGE_EXIT; break;
        }
    } else if (s_page == PAGE_MAP && ok) {
        if (*sel < GC_COUNT) { s_capture = *sel; s_capture_armed = false; }
        else if (*sel == P_RESET) { memcpy(g_set.map, DEFAULT_MAP, sizeof g_set.map); settings_save(); }
        else s_page = PAGE_MAIN;
    } else if (s_page == PAGE_EXIT && ok) {
        *quit = true;
    }
    return true;
}

/* ---- font ---- */
const char *font_glyph(char c) {
    static const char *az[26] = {
        "01110100011000111111100011000110001", "11110100011000111110100011000111110", "01110100011000010000100001000101110",
        "11110100011000110001100011000111110", "11111100001000011110100001000011111", "11111100001000011110100001000010000",
        "01110100011000010111100011000101111", "10001100011000111111100011000110001", "01110001000010000100001000010001110",
        "00111000100001000010000101001001100", "10001100101010011000101001001010001", "10000100001000010000100001000011111",
        "10001110111010110101100011000110001", "10001100011100110101100111000110001", "01110100011000110001100011000101110",
        "11110100011000111110100001000010000", "01110100011000110001101011001001101", "11110100011000111110101001001010001",
        "01111100001000001110000010000111110", "11111001000010000100001000010000100", "10001100011000110001100011000101110",
        "10001100011000110001100010101000100", "10001100011000110101101011010101010", "10001100010101000100010101000110001",
        "10001100010101000100001000010000100", "11111000010001000100010001000011111",
    };
    static const char *digits[10] = {
        "01110100011001110101110011000101110", "00100011000010000100001000010001110", "01110100010000100010001000100011111",
        "11111000100010000010000011000101110", "00010001100101010010111110001000010", "11111100001111000001000011000101110",
        "00110010001000011110100011000101110", "11111000010001000100010000100001000", "01110100011000101110100011000101110",
        "01110100011000101111000010001001100",
    };
    if (c >= 'A' && c <= 'Z') return az[c - 'A'];
    if (c >= 'a' && c <= 'z') return az[c - 'a'];
    if (c >= '0' && c <= '9') return digits[c - '0'];
    switch (c) {
    case '/': return "00001000010001000100010001000010000";
    case '.': return "00000000000000000000000000110001100";
    case '-': return "00000000000000011111000000000000000";
    case '(': return "00010001000100001000010000010000010";
    case ')': return "01000001000001000010000100010001000";
    case ':': return "00000011000110000000011000110000000";
    case '+': return "00000001000010011111001000010000000";
    case '>': return "01000001000001000001000100010001000";
    case '<': return "00010001000100010000010000010000010";
    case '%': return "11000110010001000100010001001100011";
    case '!': return "00100001000010000100001000000000100";
    case '?': return "01110100010000100010001000000000100";
    case ',': return "00000000000000000000011000010001000";
    case '=': return "00000000001111100000111110000000000";
    case '\'': return "00100001000100000000000000000000000";
    }
    return NULL;
}

/* ---- drawing ----
 * One instanced program; each instance is a rectangle with a top and a bottom colour and a
 * shape: a 5x7 bitmap glyph, a solid box, a MarioFont letter (atlas from the disc), a rounded
 * box, a star or a right-pointing arrow. No buffers to manage, ~100 instances a frame. */
#define BATCH 64
enum { SH_GLYPH, SH_BOX, SH_FONT, SH_ROUND, SH_STAR, SH_ARROW, SH_IMAGE };
static GLuint s_prog;
static GLint u_rect, u_col, u_col2, u_par, u_bits, u_screen, u_atlas;
static float s_rect[BATCH][4], s_col[BATCH][4], s_col2[BATCH][4], s_par[BATCH][4];
static GLuint s_bits[BATCH][2];
static int s_n;

static const char *VS =
    "#version 320 es\n"
    "uniform vec4 u_rect[64];\n"
    "uniform vec4 u_col[64];\n"
    "uniform vec4 u_col2[64];\n"
    "uniform vec4 u_par[64];\n"
    "uniform uvec2 u_bits[64];\n"
    "uniform vec2 u_screen;\n"
    "out vec2 v_uv;\n"
    "flat out vec4 v_col;\n"
    "flat out vec4 v_col2;\n"
    "flat out vec4 v_par;\n"
    "flat out vec2 v_size;\n"
    "flat out uvec2 v_bits;\n"
    "void main() {\n"
    "    vec2 c = vec2(float((0x32 >> gl_VertexID) & 1), float((0x2C >> gl_VertexID) & 1));\n"
    "    vec4 r = u_rect[gl_InstanceID];\n"
    "    vec2 p = r.xy + c * r.zw;\n"
    "    gl_Position = vec4(p.x / u_screen.x * 2.0 - 1.0, 1.0 - p.y / u_screen.y * 2.0, 0.0, 1.0);\n"
    "    v_uv = c;\n"
    "    v_size = r.zw;\n"
    "    v_col = u_col[gl_InstanceID];\n"
    "    v_col2 = u_col2[gl_InstanceID];\n"
    "    v_par = u_par[gl_InstanceID];\n"
    "    v_bits = u_bits[gl_InstanceID];\n"
    "}\n";
static const char *FS =
    "#version 320 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform sampler2D u_atlas;\n"
    "in vec2 v_uv;\n"
    "flat in vec4 v_col;\n"
    "flat in vec4 v_col2;\n"
    "flat in vec4 v_par;\n"
    "flat in vec2 v_size;\n"
    "flat in uvec2 v_bits;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "    uint shape = v_bits.y >> 28;\n"
    "    vec4 c = mix(v_col, v_col2, v_uv.y);\n"
    "    float a = 1.0;\n"
    "    if (shape == 0u) {\n" /* 5x7 bitmap glyph */
    "        ivec2 cell = clamp(ivec2(v_uv * vec2(5.0, 7.0)), ivec2(0), ivec2(4, 6));\n"
    "        int bit = cell.y * 5 + cell.x;\n"
    "        uint w = bit < 32 ? v_bits.x : v_bits.y;\n"
    "        if (((w >> uint(bit & 31)) & 1u) == 0u) discard;\n"
    "    } else if (shape == 2u) {\n" /* MarioFont letter: R intensity (outline), G alpha */
    "        vec2 la = texture(u_atlas, mix(v_par.xy, v_par.zw, v_uv)).rg;\n"
    "        c.rgb *= la.r;\n"
    "        a = la.g;\n"
    "    } else if (shape == 3u) {\n" /* rounded box, corner radius v_par.x px */
    "        vec2 h = v_size * 0.5, q = abs(v_uv * v_size - h) - (h - vec2(v_par.x));\n"
    "        float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - v_par.x;\n"
    "        a = clamp(0.5 - d, 0.0, 1.0);\n"
    "    } else if (shape == 4u) {\n" /* five-pointed star */
    "        vec2 p = (v_uv - 0.5) * 2.0;\n"
    "        float ang = atan(p.x, -p.y), k = 6.2831853 / 5.0;\n"
    "        float f = abs(mod(ang + k * 0.5, k) - k * 0.5) / (k * 0.5);\n"
    "        float r = mix(1.0, 0.45, f);\n"
    "        a = clamp((r - length(p)) * v_size.x * 0.5, 0.0, 1.0);\n"
    "    } else if (shape == 5u) {\n" /* arrow pointing right */
    "        float d = (1.0 - v_uv.x) - abs(v_uv.y - 0.5) * 2.0;\n"
        "        a = clamp(d * v_size.x, 0.0, 1.0);\n"
    "    } else if (shape == 6u) {\n" /* RGBA image (the logo), tinted */
    "        c *= texture(u_atlas, mix(v_par.xy, v_par.zw, v_uv));\n"
    "    }\n"
    "    if (a * c.a < 0.004) discard;\n"
    "    o = vec4(c.rgb, c.a * a);\n"
    "}\n";

static GLuint shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        rt_log("menu shader: %s", log);
    }
    return s;
}

static bool s_font;   /* MarioFont glyphs loaded from the disc */

static bool init_gl(void) {
    static bool tried;
    if (tried) return s_prog != 0;
    tried = true;
    GLuint p = glCreateProgram(), vs = shader(GL_VERTEX_SHADER, VS), fs = shader(GL_FRAGMENT_SHADER, FS);
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { rt_log("menu program failed to link"); glDeleteProgram(p); return false; }
    s_prog = p;
    u_rect = glGetUniformLocation(p, "u_rect");
    u_col = glGetUniformLocation(p, "u_col");
    u_col2 = glGetUniformLocation(p, "u_col2");
    u_par = glGetUniformLocation(p, "u_par");
    u_bits = glGetUniformLocation(p, "u_bits");
    u_screen = glGetUniformLocation(p, "u_screen");
    u_atlas = glGetUniformLocation(p, "u_atlas");
    return true;
}

void menu_font_load(void) {
    static bool tried;
    if (tried) return;
    tried = true;
    s_font = uifont_load();
}

static void flush(void) {
    if (!s_n) return;
    glUniform4fv(u_rect, s_n, &s_rect[0][0]);
    glUniform4fv(u_col, s_n, &s_col[0][0]);
    glUniform4fv(u_col2, s_n, &s_col2[0][0]);
    glUniform4fv(u_par, s_n, &s_par[0][0]);
    glUniform2uiv(u_bits, s_n, &s_bits[0][0]);
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, s_n);
    s_n = 0;
}

static void rgba(float out[4], uint32_t c) {
    for (int i = 0; i < 4; i++) out[i] = (float)((c >> (24 - 8 * i)) & 0xFF) / 255.0f;
}
static void quad(float x, float y, float w, float h, uint32_t top, uint32_t bot, unsigned shape, GLuint b0, GLuint b1,
                 const float par[4]) {
    if (s_n == BATCH) flush();
    s_rect[s_n][0] = x; s_rect[s_n][1] = y; s_rect[s_n][2] = w; s_rect[s_n][3] = h;
    rgba(s_col[s_n], top);
    rgba(s_col2[s_n], bot);
    for (int i = 0; i < 4; i++) s_par[s_n][i] = par ? par[i] : 0.0f;
    s_bits[s_n][0] = b0;
    s_bits[s_n][1] = (b1 & 0x0FFFFFFFu) | (GLuint)shape << 28;
    s_n++;
}
static void rect(float x, float y, float w, float h, uint32_t rgba_) { quad(x, y, w, h, rgba_, rgba_, SH_BOX, 0, 0, NULL); }
static void grad(float x, float y, float w, float h, uint32_t top, uint32_t bot) { quad(x, y, w, h, top, bot, SH_BOX, 0, 0, NULL); }
static void round_box(float x, float y, float w, float h, float r, uint32_t c) {
    const float par[4] = {r, 0, 0, 0};
    quad(x, y, w, h, c, c, SH_ROUND, 0, 0, par);
}
static void star(float cx, float cy, float r, uint32_t c) { quad(cx - r, cy - r, 2 * r, 2 * r, c, c, SH_STAR, 0, 0, NULL); }
static void arrow(float x, float y, float w, float h, uint32_t c) { quad(x, y, w, h, c, c, SH_ARROW, 0, 0, NULL); }

/* 5x7 bitmap text at (x, y) with glyph pixel size px; returns the width. */
static float text(float x, float y, float px, uint32_t rgba_, const char *s) {
    float x0 = x;
    for (; *s; s++, x += 6 * px) {
        const char *g = font_glyph(*s);
        if (!g) continue;
        GLuint b[2] = {0, 0};
        for (int i = 0; i < 35; i++)
            if (g[i] == '1') b[i >> 5] |= 1u << (i & 31);
        quad(x, y, 5 * px, 7 * px, rgba_, rgba_, SH_GLYPH, b[0], b[1], NULL);
    }
    return x - x0;
}
static float text_w(const char *s, float px) { return (float)strlen(s) * 6 * px - px; }

/* MarioFont text, size = cell height in px, top/bottom gradient (the game's titles: yellow to
 * orange). Characters the font lacks use the bitmap font. Returns the advance. */
static float mtext_draw(float x, float y, float size, uint32_t top, uint32_t bot, const char *s, bool draw) {
    if (!s_font) {
        float px = size / 9.0f;
        if (draw) text(x, y + size * 0.12f, px, top, s);
        return text_w(s, px);
    }
    float sc = size / 32.0f, x0 = x;
    for (; *s; s++) {
        float uv[4], g0, g1;
        if (*s == ' ') { x += 11 * sc; continue; }
        if (uifont_glyph(*s, uv, &g0, &g1)) {
            if (draw) quad(x - g0 * sc, y, 32 * sc, 32 * sc, top, bot, SH_FONT, 0, 0, uv);
            x += (g1 - g0 - 2) * sc;
        } else {
            const char *g = font_glyph(*s);
            float px = size / 9.0f;
            if (g && draw) {
                GLuint b[2] = {0, 0};
                for (int i = 0; i < 35; i++)
                    if (g[i] == '1') b[i >> 5] |= 1u << (i & 31);
                quad(x, y + size * 0.14f, 5 * px, 7 * px, 0x101010FFu, 0x101010FFu, SH_GLYPH, b[0], b[1], NULL); /* outline */
                quad(x + px * 0.35f, y + size * 0.14f + px * 0.35f, 5 * px * 0.86f, 7 * px * 0.86f, top, bot, SH_GLYPH, b[0], b[1], NULL);
            }
            x += 6 * px;
        }
    }
    return x - x0;
}
static float mtext(float x, float y, float size, uint32_t top, uint32_t bot, const char *s) { return mtext_draw(x, y, size, top, bot, s, true); }
static float mtext_w(const char *s, float size) { return mtext_draw(0, 0, size, 0, 0, s, false); }

/* the game's palette */
#define C_TITLE_T 0xFFEC40FFu   /* titles and labels: yellow ... */
#define C_TITLE_B 0xFF8800FFu   /* ... to orange */
#define C_SEL_T 0xFFFFFFFFu     /* selected label: white to yellow */
#define C_SEL_B 0xFFD640FFu
#define C_VAL_T 0xFFFFFFFFu     /* values: white to light blue */
#define C_VAL_B 0x96D2FFFFu
#define C_DIM 0xB8C8F0FFu

/* The panel: red border, white rim, navy with darker-blue stars, blue title band with a
 * checkered edge (the cup / character select screens). */
static void panel(float x, float y, float w, float h, float k, const char *title) {
    round_box(x - 8 * k, y - 8 * k, w + 16 * k, h + 16 * k, 22 * k, 0xD61C1CFFu);
    round_box(x - 3 * k, y - 3 * k, w + 6 * k, h + 6 * k, 18 * k, 0xFFFFFFFFu);
    round_box(x, y, w, h, 16 * k, 0x102268F8u);
    /* stars at fixed pseudo-random places */
    uint32_t seed = 12345;
    for (int i = 0; i < 26; i++) {
        seed = seed * 1103515245u + 12345u;
        float sx = x + 20 * k + (float)(seed >> 8 & 1023) / 1023.0f * (w - 40 * k);
        seed = seed * 1103515245u + 12345u;
        float sy = y + 100 * k + (float)(seed >> 8 & 1023) / 1023.0f * (h - 120 * k);
        seed = seed * 1103515245u + 12345u;
        star(sx, sy, (8 + (float)(seed >> 8 & 7)) * k, 0x223E96FFu);
    }
    float band = 70 * k;
    grad(x + 6 * k, y + 6 * k, w - 12 * k, band - 6 * k, 0x3C78E6FFu, 0x2457C8FFu);
    for (float i = 0, n = 0; i + 7 * k <= w - 12 * k; i += 7 * k, n++) {
        bool odd = (int)n & 1;
        rect(x + 6 * k + i, y + band, 7 * k, 7 * k, odd ? 0x2457C8FFu : 0xFFFFFFFFu);
        rect(x + 6 * k + i, y + band + 7 * k, 7 * k, 7 * k, odd ? 0xFFFFFFFFu : 0x2457C8FFu);
    }
    float ts = 46 * k;
    mtext(x + (w - mtext_w(title, ts)) / 2, y + 12 * k, ts, C_TITLE_T, C_TITLE_B, title);
}

/* A menu row: label left, value right; the selected one sits on a blue arrow bar. */
static void mrow(float x, float y, float w, float rh, float k, bool sel, const char *label, const char *value, bool more) {
    float ts = rh * 0.7f, ty = y + (rh - ts) * 0.5f;
    if (sel) {
        grad(x + 14 * k, y + rh * 0.06f, w - 72 * k, rh * 0.88f, 0x5AA0FFE8u, 0x3C7CF0E8u);
        arrow(x + w - 58 * k, y + rh * 0.06f, 30 * k, rh * 0.88f, 0x4890F8E8u);
        arrow(x + 22 * k, y + rh * 0.3f, 14 * k, rh * 0.4f, 0xFFFFFFFFu);
    }
    mtext(x + 50 * k, ty, ts, sel ? C_SEL_T : C_TITLE_T, sel ? C_SEL_B : C_TITLE_B, label);
    if (value) mtext(x + w - 70 * k - mtext_w(value, ts), ty, ts, C_VAL_T, C_VAL_B, value);
    else if (more) arrow(x + w - 84 * k, y + rh * 0.3f, 16 * k, rh * 0.4f, 0xFFFFFFFFu);
}

bool menu_is_open(void) { return s_open || s_capture >= 0; }
bool menu_take_controllers(void) {
    bool r = s_want_controllers;
    s_want_controllers = false;
    return r;
}

static void begin(int w, int h) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(s_prog);
    glUniform2f(u_screen, (float)w, (float)h);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_font ? uifont_texture() : 0);
    glBindSampler(0, 0);
    glUniform1i(u_atlas, 0);
    glBindVertexArray(0);
}
static void end(void) {
    flush();
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_BLEND);
    glUseProgram(0);
}

void menu_draw(int w, int h, float fps, const char *perf) {
    if (!s_open && s_capture < 0 && !g_set.show_fps) return;
    if (!init_gl()) return;
    menu_font_load();
    begin(w, h);
    float k = (float)h / 720.0f;

    if (g_set.show_fps) {
        char s[32];
        snprintf(s, sizeof s, "%.0f FPS", (double)fps);
        float px = 3 * k, lh = 7 * px + 4 * px;
        bool times = g_set.show_fps == 2 && perf && perf[0];
        float bw = text_w(s, px);
        if (times && text_w(perf, px * 0.6f) > bw) bw = text_w(perf, px * 0.6f);
        rect(8 * k, 8 * k, bw + 6 * px, times ? lh + 7 * px * 0.6f + 2 * px : lh, 0x000000A0u);
        text(8 * k + 3 * px, 8 * k + 2 * px, px, fps >= 55 ? 0x80FF80FFu : fps >= 40 ? 0xFFE060FFu : 0xFF7060FFu, s);
        if (times) text(8 * k + 3 * px, 8 * k + lh, px * 0.6f, 0xE0E0E0FFu, perf);
    }
    if ((s_open || s_capture >= 0) && !s_preview) {
        int rows = page_rows(s_page);
        if (s_page == PAGE_EXIT) rows = 2;
        float pw = 984 * k, top = 100 * k, foot = 56 * k;
        float rh = 58 * k, maxh = (float)h - 40 * k;
        if (top + rh * (float)rows + foot > maxh) rh = (maxh - top - foot) / (float)rows;
        float ph = top + rh * (float)rows + foot, x = ((float)w - pw) / 2, y = ((float)h - ph) / 2;
        if (s_page != PAGE_COLOR) rect(0, 0, (float)w, (float)h, 0x00000070u); /* colour page: the image undimmed */
        const char *title = s_page == PAGE_MAP ? "BUTTON MAPPING" : s_page == PAGE_EXIT ? "EXIT THE GAME?"
                            : s_page == PAGE_COLOR ? "COLOR" : s_page == PAGE_DEBUG ? "DEBUGGING"
                            : s_page == PAGE_ADV ? "ADVANCED" : "OPTIONS";
        panel(x, y, pw, ph, k, title);
        float ry = y + top;
        const char *hint = NULL;
        if (s_page == PAGE_MAIN) {
            static const char *labels[M_COUNT] = {"DISPLAY", "HD TEXTURES", "CONTROLLERS", "BUTTON MAPPING", "ADVANCED", "DEBUGGING", "EXIT GAME"};
            const char *values[M_COUNT] = {g_set.wide ? "16:9" : "4:3", !hdtex_available() ? "NO PACK" : g_set.hd_textures ? "ON" : "OFF",
                                           NULL, NULL, NULL, NULL, NULL};
            static const bool more[M_COUNT] = {false, false, true, true, true, true, false};
            char hd_hint[64];
            if (!hdtex_available()) snprintf(hd_hint, sizeof hd_hint, "PUT HD-TEXTURES.BIN IN THE MKDD-NX FOLDER");
            else if (!g_set.hd_textures) snprintf(hd_hint, sizeof hd_hint, "COMMUNITY HD TEXTURE PACK");
            else snprintf(hd_hint, sizeof hd_hint, "COMMUNITY HD TEXTURE PACK: %u IN USE", render_hd_in_use());
            for (int i = 0; i < M_COUNT; i++, ry += rh) mrow(x, ry, pw, rh, k, s_sel[PAGE_MAIN] == i, labels[i], values[i], more[i]);
            hint = s_notice[0]                   ? s_notice
                   : s_sel[PAGE_MAIN] == M_WIDE  ? "16:9: WIDER VIEW, RACE HUD AT THE SCREEN EDGES"
                   : s_sel[PAGE_MAIN] == M_HD    ? hd_hint
                   : s_sel[PAGE_MAIN] == M_CTRL  ? "PAIR CONTROLLERS, SET THE PLAYER ORDER, SINGLE JOY-CONS"
                   : s_sel[PAGE_MAIN] == M_ADV   ? "COLOR, RUMBLE, FIELD OF VIEW, TEXTURE FILTERING, BORDER"
                   : s_sel[PAGE_MAIN] == M_DEBUG ? "FPS AND FRAME TIMES, UNLOCK ALL, LOGS"
                                                 : "GAME PAUSED   A: SELECT   B OR -: CLOSE";
            /* "Oct 15 2026 14:08:52" -> "BUILD 1408" (the name of the build's saved .elf), in the
             * title band's top right */
            char build[16];
            const char *b = g_build_stamp;
            snprintf(build, sizeof build, "BUILD %.2s%.2s", b + 12, b + 15);
            float bs = 22 * k;
            mtext(x + pw - mtext_w(build, bs) - 24 * k, y + 10 * k + (60 * k - bs) / 2, bs, C_VAL_T, C_VAL_B, build);
        } else if (s_page == PAGE_ADV) {
            static const char *labels[A_COUNT] = {"COLOR", "RUMBLE", "FIELD OF VIEW", "TEXTURE FILTERING", "DOCKED RESOLUTION", "BORDER", "BACK"};
            const char *values[A_COUNT] = {COLOR_NAMES[g_color.mode], RUMBLE_NAMES[g_set.rumble % 3], FOV_NAMES[g_set.fov[g_set.wide] % 3],
                                           ANISO_NAMES[g_set.aniso % 5], g_set.docked_res ? "2X" : "NATIVE",
                                           g_set.border ? "ON" : "OFF", NULL};
            static const bool more[A_COUNT] = {true, false, false, false, false, false, false};
            for (int i = 0; i < A_COUNT; i++, ry += rh) mrow(x, ry, pw, rh, k, s_sel[PAGE_ADV] == i, labels[i], values[i], more[i]);
            hint = s_sel[PAGE_ADV] == A_COLOR    ? "A: ADJUST   LEFT/RIGHT: ACCURATE, VIVID, CUSTOM"
                   : s_sel[PAGE_ADV] == A_RUMBLE ? "CONTROLLER VIBRATION"
                   : s_sel[PAGE_ADV] == A_FOV    ? (g_set.wide ? "RACE CAMERA IN 16:9, DEFAULT WIDER" : "RACE CAMERA IN 4:3, DEFAULT NORMAL")
                   : s_sel[PAGE_ADV] == A_ANISO  ? "SHARPER TEXTURES AT ANGLES: ROADS AND WALLS IN THE DISTANCE"
                   : s_sel[PAGE_ADV] == A_DOCKED ? "ON THE TV: 2X RENDER, 1080P OUTPUT. HANDHELD STAYS NATIVE"
                   : s_sel[PAGE_ADV] == A_BORDER ? "4:3: CHECKERED BORDER BESIDE THE RACE"
                                                 : "B: BACK";
        } else if (s_page == PAGE_DEBUG) {
            static const char *labels[D_COUNT] = {"SHOW FPS", "UNLOCK ALL", "LOGGING", "PROFILER", "BACK"};
            const char *values[D_COUNT] = {FPS_NAMES[g_set.show_fps % 3], g_set.unlock_all ? "ON" : "OFF",
                                           LOG_NAMES[g_set.logging & 3], g_set.profiler ? "ON" : "OFF", NULL};
            for (int i = 0; i < D_COUNT; i++, ry += rh) mrow(x, ry, pw, rh, k, s_sel[PAGE_DEBUG] == i, labels[i], values[i], false);
            hint = s_sel[PAGE_DEBUG] == D_FPS      ? "TIMES: MS PER FRAME OF GAME, GRAPHICS AND RENDER THREADS"
                   : s_sel[PAGE_DEBUG] == D_UNLOCK ? "ALL CHARACTERS, KARTS, CUPS: NEVER WRITTEN TO YOUR SAVE"
                   : s_sel[PAGE_DEBUG] == D_LOG    ? "DEBUG LOGS ON THE SD CARD, FOR BUG REPORTS"
                   : s_sel[PAGE_DEBUG] == D_PROF   ? "PERFORMANCE SAMPLES FOR REPORTS: COSTS A LITTLE SPEED. NEXT START"
                                                   : "B: BACK";
        } else if (s_page == PAGE_COLOR) {
            int v[3];
            color_values(v);
            for (int i = 0; i < K_COUNT; i++, ry += rh) {
                static const char *labels[K_COUNT] = {"MODE", "BRIGHTNESS", "CONTRAST", "SATURATION", "RESET", "BACK"};
                char value[16];
                const char *vp = value;
                if (i == K_MODE) vp = COLOR_NAMES[g_color.mode];
                else if (i >= K_BRIGHT && i <= K_SAT) snprintf(value, sizeof value, "%+d", v[i == K_BRIGHT ? 0 : i == K_CONTRAST ? 1 : 2]);
                else vp = NULL;
                mrow(x, ry, pw, rh, k, s_sel[PAGE_COLOR] == i, labels[i], vp, false);
            }
            hint = "LEFT/RIGHT: CHANGE   HOLD Y: PREVIEW   B: BACK";
        } else if (s_page == PAGE_MAP) {
            for (int i = 0; i < P_COUNT; i++, ry += rh) {
                char label[32], value[64];
                if (i < GC_COUNT) {
                    snprintf(label, sizeof label, "GAMECUBE %s", GC_NAMES[i]);
                    if (s_capture == i) snprintf(value, sizeof value, "PRESS A BUTTON");
                    else map_name(g_set.map[i], value, sizeof value);
                    mrow(x, ry, pw, rh, k, s_sel[PAGE_MAP] == i, label, value, false);
                } else {
                    mrow(x, ry, pw, rh, k, s_sel[PAGE_MAP] == i, i == P_RESET ? "RESET TO DEFAULTS" : "BACK", NULL, false);
                }
            }
            hint = s_capture >= 0 ? "PRESS THE NEW BUTTON   -: CANCEL" : "A: CHANGE   B: BACK";
        } else {
            const char *l1 = "UNSAVED RACE PROGRESS IS LOST";
            float ts = rh * 0.6f;
            mtext(x + (pw - mtext_w(l1, ts)) / 2, ry + (rh - ts) / 2, ts, C_VAL_T, C_VAL_B, l1);
            ry += rh;
            const char *l2 = "A: EXIT   B: BACK";
            mtext(x + (pw - mtext_w(l2, ts)) / 2, ry + (rh - ts) / 2, ts, C_TITLE_T, C_TITLE_B, l2);
            ry += rh;
        }
        if (hint) {
            float px = 2.4f * k;
            text(x + (pw - text_w(hint, px)) / 2, y + ph - foot + 14 * k, px, 0xFFFFFFFFu, hint);
        }
    }
    end();
}

/* ---- start-up screen ---- */
/* Checkered columns two squares deep at the screen edge and beside the picture, navy and stars
 * between: the loading screen's look, in the side bars of a 4:3 race. */
void menu_border(int w, int h) {
    float pw = (float)h * 4.0f / 3.0f, bar = ((float)w - pw) / 2;
    if (bar < 8 || !init_gl()) return;
    begin(w, h);
    float k = (float)h / 720.0f, fh = (float)h;
    int rows = (int)(fh / (14 * k) + 0.5f);
    float ch = fh / (float)rows, cw = ch;
    for (int side = 0; side < 2; side++) {
        float x0 = side ? (float)w - bar : 0;
        grad(x0, 0, bar, fh, 0x0A1650FFu, 0x1C3A9AFFu);
        uint32_t seed = 777u + (uint32_t)side * 31u;
        for (int i = 0; i < 10; i++) {
            seed = seed * 1103515245u + 12345u;
            float sx = x0 + 2 * cw + (float)(seed >> 8 & 1023) / 1023.0f * (bar - 4 * cw);
            seed = seed * 1103515245u + 12345u;
            float sy = (float)(seed >> 8 & 1023) / 1023.0f * fh;
            seed = seed * 1103515245u + 12345u;
            star(sx, sy, (10 + (float)(seed >> 8 & 7)) * k, 0x2A4AA8FFu);
        }
        /* outer edge and inner edge (next to the picture) */
        float cols[2] = {side ? (float)w - 2 * cw : 0, side ? x0 : bar - 2 * cw};
        for (int c2 = 0; c2 < 2; c2++)
            for (int y = 0; y < rows; y++)
                for (int x = 0; x < 2; x++)
                    rect(cols[c2] + (float)x * cw, (float)y * ch, cw, ch, (x + y) & 1 ? 0x101010FFu : 0xFFFFFFFFu);
    }
    end();
}

#define TITLE_A "MARIO KART: "
#define TITLE_B "DOUBLE DASH!!"
#define TITLE_C "NX"

void menu_loading(int w, int h, const char *line1, const char *line2, float progress) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!init_gl()) return;
    begin(w, h);
    float k = (float)h / 720.0f, fw = (float)w, fh = (float)h;

    /* navy sky with the menus' darker-blue stars, framed by a checkered-flag border two squares
     * deep (squares sized to fit the screen exactly, so the pattern meets at every edge) */
    grad(0, 0, fw, fh, 0x0A1650FFu, 0x1C3A9AFFu);
    int cols = (int)(fw / (14 * k) + 0.5f), rows = (int)(fh / (14 * k) + 0.5f);
    float cw = fw / (float)cols, ch = fh / (float)rows;
    uint32_t seed = 777;
    for (int i = 0; i < 40; i++) {
        seed = seed * 1103515245u + 12345u;
        float sx = 2 * cw + (float)(seed >> 8 & 1023) / 1023.0f * (fw - 4 * cw);
        seed = seed * 1103515245u + 12345u;
        float sy = 2 * ch + (float)(seed >> 8 & 1023) / 1023.0f * (fh - 4 * ch);
        seed = seed * 1103515245u + 12345u;
        star(sx, sy, (10 + (float)(seed >> 8 & 7)) * k, 0x2A4AA8FFu);
    }
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++) {
            if (y >= 2 && y < rows - 2 && x >= 2 && x < cols - 2) continue;
            rect((float)x * cw, (float)y * ch, cw, ch, (x + y) & 1 ? 0x101010FFu : 0xFFFFFFFFu);
        }
    flush();
    glBindTexture(GL_TEXTURE_2D, s_font ? uifont_texture() : 0);

    /* the title in the game's lettering (yellow to orange, like its menu titles) */
    /* "MARIO KART:" yellow to orange, "DOUBLE DASH!!" white, "NX" red on its own line */
    float tsz = 76 * k, tw0 = mtext_w(TITLE_A TITLE_B, tsz), tmax = fw - 8 * cw - 40 * k;
    if (tw0 > tmax) { tsz *= tmax / tw0; tw0 = mtext_w(TITLE_A TITLE_B, tsz); }
    float nsz = tsz * 1.3f, ty0 = 215 * k - (2 * tsz + nsz) / 2, tx = (fw - tw0) / 2; /* a blank line before "NX" */
    tx += mtext(tx, ty0, tsz, C_TITLE_T, C_TITLE_B, TITLE_A);
    mtext(tx, ty0, tsz, 0xFFFFFFFFu, 0xD8DCF0FFu, TITLE_B);
    mtext((fw - mtext_w(TITLE_C, nsz)) / 2, ty0 + 2 * tsz, nsz, 0xFF5A4AFFu, 0xC81414FFu, TITLE_C);

    /* message box: red border, white rim, navy, like the - menu */
    float bw = 860 * k, bh = 190 * k, bx = (fw - bw) / 2, by = 420 * k;
    round_box(bx - 8 * k, by - 8 * k, bw + 16 * k, bh + 16 * k, 22 * k, 0xD61C1CFFu);
    round_box(bx - 3 * k, by - 3 * k, bw + 6 * k, bh + 6 * k, 18 * k, 0xFFFFFFFFu);
    round_box(bx, by, bw, bh, 16 * k, 0x102268F8u);
    float ts = 40 * k, ty = by + (progress >= 0 ? 22 * k : 40 * k);
    const char *lines[2] = {line1, line2};
    for (int i = 0; i < 2; i++, ty += ts + 14 * k) {
        if (!lines[i]) continue;
        float sz = i ? ts * 0.8f : ts, tw = mtext_w(lines[i], sz);
        if (tw > bw - 40 * k) { sz *= (bw - 40 * k) / tw; tw = mtext_w(lines[i], sz); }
        mtext((fw - tw) / 2, ty + (ts - sz) / 2, sz, i ? C_VAL_T : C_TITLE_T, i ? C_VAL_B : C_TITLE_B, lines[i]);
    }
    if (progress >= 0) {
        float pw = bw - 120 * k, ph = 26 * k, px = (fw - pw) / 2, py = by + bh - ph - 26 * k;
        float f = progress > 1 ? 1 : progress;
        round_box(px - 3 * k, py - 3 * k, pw + 6 * k, ph + 6 * k, ph / 2 + 3 * k, 0xFFFFFFFFu);
        round_box(px, py, pw, ph, ph / 2, 0x0A1238FFu);
        const float r[4] = {ph / 2, 0, 0, 0};
        if (f * pw >= ph) quad(px, py, f * pw, ph, C_TITLE_T, C_TITLE_B, SH_ROUND, 0, 0, r);
    }
    end();
}
