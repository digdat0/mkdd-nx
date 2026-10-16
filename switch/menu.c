/* The − menu (settings, button mapping, exit) and the on-screen overlay. Drawn after the game
 * image with one small instanced program: each instance is a rectangle, either solid or a 5x7
 * glyph whose bits come from a uniform, so there are no buffers or textures to manage. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>
#include <GLES3/gl32.h>

#include "menu.h"
#include "render_gl.h"
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
    g_set.show_fps = false;
    g_set.logging = 1;
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
        else if (!strcmp(k, "show_fps")) g_set.show_fps = atoi(v) != 0;
        else if (!strcmp(k, "logging")) g_set.logging = atoi(v) & 3;
        else
            for (int i = 0; i < GC_COUNT; i++)
                if (!strcmp(k, GC_KEYS[i])) g_set.map[i] = strtoull(v, NULL, 16) & mappable();
    }
    fclose(f);
    apply_logging();
    rt_log("settings: 16:9 %d, fps %d, logging %d", g_set.wide, g_set.show_fps, g_set.logging);
}

void settings_save(void) {
    FILE *f = fopen(s_path, "w");
    if (!f) return;
    fprintf(f, "display=%d\nshow_fps=%d\nlogging=%d\n", g_set.wide, g_set.show_fps, g_set.logging);
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
enum { PAGE_MAIN, PAGE_MAP, PAGE_EXIT, PAGE_COLOR };
enum { M_WIDE, M_FPS, M_COLOR, M_LOG, M_MAP, M_EXIT, M_COUNT };
enum { P_RESET = GC_COUNT, P_BACK, P_COUNT };
enum { K_MODE, K_BRIGHT, K_CONTRAST, K_SAT, K_RESET, K_BACK, K_COUNT };
static bool s_open;
static bool s_preview;          /* colour page with Y held: panel hidden to see the whole image */
static int s_page, s_sel[4];
static int s_capture = -1;      /* GameCube button waiting for a new Switch button */
static bool s_capture_armed;    /* all buttons were released since the capture started */
static bool s_release_wait;     /* menu closed: keep the game's pad neutral until buttons are released */

static void map_name(uint64_t m, char *out, size_t n) {
    out[0] = 0;
    for (unsigned i = 0; i < N_SW; i++)
        if (m & SW_BUTTONS[i].bit) {
            size_t l = strlen(out);
            snprintf(out + l, n - l, "%s%s", l ? "/" : "", SW_BUTTONS[i].name);
        }
    if (!out[0]) snprintf(out, n, "NONE");
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
    int count = s_page == PAGE_MAIN ? M_COUNT : s_page == PAGE_MAP ? P_COUNT : s_page == PAGE_COLOR ? K_COUNT : 1;
    int *sel = &s_sel[s_page];
    if (down & UP) *sel = (*sel + count - 1) % count;
    if (down & DOWN) *sel = (*sel + 1) % count;
    bool ok = down & HidNpadButton_A, side = down & SIDE;
    bool left = down & (HidNpadButton_Left | HidNpadButton_StickLLeft);
    s_preview = s_page == PAGE_COLOR && (held & HidNpadButton_Y);
    if (down & (HidNpadButton_Minus | HidNpadButton_B)) {
        if (s_page == PAGE_MAIN || (down & HidNpadButton_Minus)) { s_open = false; s_release_wait = true; s_preview = false; }
        else s_page = PAGE_MAIN;
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
            s_page = PAGE_MAIN;
            color_save();
        }
        return true;
    }
    if (s_page == PAGE_MAIN && (ok || side)) {
        switch (*sel) {
        case M_WIDE: g_set.wide = !g_set.wide; settings_save(); break;
        case M_FPS: g_set.show_fps = !g_set.show_fps; settings_save(); break;
        case M_COLOR:
            if (ok) { s_page = PAGE_COLOR; s_sel[PAGE_COLOR] = 0; }
            else { g_color.mode = (g_color.mode + (left ? COLOR_MODES - 1 : 1)) % COLOR_MODES; s_color_dirty = true; color_save(); }
            break;
        case M_LOG: {
            /* A / right: next mode, left: previous */
            g_set.logging = (g_set.logging + (left ? 3 : 1)) & 3;
            apply_logging();
            settings_save();
            break;
        }
        case M_MAP: if (ok) { s_page = PAGE_MAP; s_sel[PAGE_MAP] = 0; } break;
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

/* ---- drawing ---- */
#define BATCH 64
static GLuint s_prog;
static GLint u_rect, u_col, u_bits, u_screen;
static float s_rect[BATCH][4], s_col[BATCH][4];
static GLuint s_bits[BATCH][2];
static int s_n;

static const char *VS =
    "#version 320 es\n"
    "uniform vec4 u_rect[64];\n"
    "uniform vec4 u_col[64];\n"
    "uniform uvec2 u_bits[64];\n"
    "uniform vec2 u_screen;\n"
    "out vec2 v_uv;\n"
    "flat out vec4 v_col;\n"
    "flat out uvec2 v_bits;\n"
    "void main() {\n"
    "    vec2 c = vec2(float((0x32 >> gl_VertexID) & 1), float((0x2C >> gl_VertexID) & 1));\n"
    "    vec4 r = u_rect[gl_InstanceID];\n"
    "    vec2 p = r.xy + c * r.zw;\n"
    "    gl_Position = vec4(p.x / u_screen.x * 2.0 - 1.0, 1.0 - p.y / u_screen.y * 2.0, 0.0, 1.0);\n"
    "    v_uv = c;\n"
    "    v_col = u_col[gl_InstanceID];\n"
    "    v_bits = u_bits[gl_InstanceID];\n"
    "}\n";
static const char *FS =
    "#version 320 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "in vec2 v_uv;\n"
    "flat in vec4 v_col;\n"
    "flat in uvec2 v_bits;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "    ivec2 cell = clamp(ivec2(v_uv * vec2(5.0, 7.0)), ivec2(0), ivec2(4, 6));\n"
    "    int bit = cell.y * 5 + cell.x;\n"
    "    uint w = bit < 32 ? v_bits.x : v_bits.y;\n"
    "    if (((w >> uint(bit & 31)) & 1u) == 0u) discard;\n"
    "    o = v_col;\n"
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
    u_bits = glGetUniformLocation(p, "u_bits");
    u_screen = glGetUniformLocation(p, "u_screen");
    return true;
}

static void flush(void) {
    if (!s_n) return;
    glUniform4fv(u_rect, s_n, &s_rect[0][0]);
    glUniform4fv(u_col, s_n, &s_col[0][0]);
    glUniform2uiv(u_bits, s_n, &s_bits[0][0]);
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, s_n);
    s_n = 0;
}

static void quad(float x, float y, float w, float h, uint32_t rgba, GLuint b0, GLuint b1) {
    if (s_n == BATCH) flush();
    s_rect[s_n][0] = x; s_rect[s_n][1] = y; s_rect[s_n][2] = w; s_rect[s_n][3] = h;
    for (int i = 0; i < 4; i++) s_col[s_n][i] = (float)((rgba >> (24 - 8 * i)) & 0xFF) / 255.0f;
    s_bits[s_n][0] = b0;
    s_bits[s_n][1] = b1;
    s_n++;
}
static void rect(float x, float y, float w, float h, uint32_t rgba) { quad(x, y, w, h, rgba, 0xFFFFFFFFu, 7u); }

/* Text at (x, y) with glyph pixel size px; returns the width. */
static float text(float x, float y, float px, uint32_t rgba, const char *s) {
    float x0 = x;
    for (; *s; s++, x += 6 * px) {
        const char *g = font_glyph(*s);
        if (!g) continue;
        GLuint b[2] = {0, 0};
        for (int i = 0; i < 35; i++)
            if (g[i] == '1') b[i >> 5] |= 1u << (i & 31);
        quad(x, y, 5 * px, 7 * px, rgba, b[0], b[1]);
    }
    return x - x0;
}
static float text_w(const char *s, float px) { return (float)strlen(s) * 6 * px - px; }

#define C_PANEL 0x101018E0u
#define C_EDGE 0xF0C850FFu
#define C_TEXT 0xFFFFFFFFu
#define C_DIM 0xA0A0B0FFu
#define C_SEL 0x3A3A60FFu

static void row(float x, float y, float w, float rh, float px, bool sel, const char *label, const char *value) {
    if (sel) rect(x, y, w, rh, C_SEL);
    float ty = y + (rh - 7 * px) / 2;
    text(x + 3 * px, ty, px, sel ? C_EDGE : C_TEXT, label);
    if (value) text(x + w - 3 * px - text_w(value, px), ty, px, sel ? C_EDGE : C_TEXT, value);
}

void menu_draw(int w, int h, float fps) {
    if (!s_open && s_capture < 0 && !g_set.show_fps) return;
    if (!init_gl()) return;
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
    glBindVertexArray(0);
    float k = (float)h / 720.0f;

    if (g_set.show_fps) {
        char s[32];
        snprintf(s, sizeof s, "%.0f FPS", (double)fps);
        float px = 3 * k;
        rect(8 * k, 8 * k, text_w(s, px) + 6 * px, 7 * px + 4 * px, 0x000000A0u);
        text(8 * k + 3 * px, 8 * k + 2 * px, px, fps >= 55 ? 0x80FF80FFu : fps >= 40 ? 0xFFE060FFu : 0xFF7060FFu, s);
    }
    if ((s_open || s_capture >= 0) && !s_preview) {
        float px = 4 * k, rh = 13 * px, pw = 820 * k;
        int rows = s_page == PAGE_MAIN ? M_COUNT : s_page == PAGE_MAP ? P_COUNT : s_page == PAGE_COLOR ? K_COUNT : 2;
        float ph = rh * (float)(rows + 3), x = ((float)w - pw) / 2, y = ((float)h - ph) / 2;
        if (s_page != PAGE_COLOR) rect(0, 0, (float)w, (float)h, 0x00000070u); /* colour page: the image undimmed */
        rect(x - px, y - px, pw + 2 * px, ph + 2 * px, C_EDGE);
        rect(x, y, pw, ph, 0x101018F4u);
        const char *title = s_page == PAGE_MAP ? "BUTTON MAPPING" : s_page == PAGE_EXIT ? "EXIT THE GAME?" : s_page == PAGE_COLOR ? "COLOR" : "MKDD-NX OPTIONS";
        text(x + (pw - text_w(title, px)) / 2, y + (rh - 7 * px) / 2, px, C_EDGE, title);
        float ry = y + rh * 1.25f;
        if (s_page == PAGE_MAIN) {
            const char *labels[M_COUNT] = {"DISPLAY", "SHOW FPS", "COLOR", "LOGGING", "BUTTON MAPPING", "EXIT GAME"};
            const char *values[M_COUNT] = {g_set.wide ? "16:9" : "4:3", g_set.show_fps ? "ON" : "OFF", COLOR_NAMES[g_color.mode],
                                           LOG_NAMES[g_set.logging & 3], ">", NULL};
            for (int i = 0; i < M_COUNT; i++, ry += rh) row(x + px, ry, pw - 2 * px, rh, px, s_sel[PAGE_MAIN] == i, labels[i], values[i]);
            const char *hint = s_sel[PAGE_MAIN] == M_WIDE  ? "16:9: WIDER VIEW, RACE HUD AT THE SCREEN EDGES"
                               : s_sel[PAGE_MAIN] == M_LOG ? "DEBUG LOGS ON THE SD CARD, FOR BUG REPORTS"
                               : s_sel[PAGE_MAIN] == M_COLOR ? "A: ADJUST   LEFT/RIGHT: ACCURATE, VIVID, CUSTOM"
                                                           : "A: SELECT   B OR -: CLOSE   (+ PAUSES THE GAME)";
            text(x + (pw - text_w(hint, px * 0.6f)) / 2, ry + rh * 0.6f, px * 0.6f, C_DIM, hint);
            /* "Oct 15 2026 14:08:52" -> "BUILD 1408  OCT 15" (the name of the build's saved .elf) */
            char build[40];
            const char *b = g_build_stamp;
            snprintf(build, sizeof build, "BUILD %.2s%.2s  %.6s", b + 12, b + 15, b);
            text(x + pw - text_w(build, px * 0.5f) - 2 * px, y + ph - 6 * px, px * 0.5f, C_DIM, build);
        } else if (s_page == PAGE_COLOR) {
            int v[3];
            color_values(v);
            for (int i = 0; i < K_COUNT; i++, ry += rh) {
                static const char *labels[K_COUNT] = {"MODE", "BRIGHTNESS", "CONTRAST", "SATURATION", "RESET TO ACCURATE", "BACK"};
                char value[16];
                const char *vp = value;
                if (i == K_MODE) vp = COLOR_NAMES[g_color.mode];
                else if (i >= K_BRIGHT && i <= K_SAT) snprintf(value, sizeof value, "< %+d >", v[i == K_BRIGHT ? 0 : i == K_CONTRAST ? 1 : 2]);
                else vp = NULL;
                row(x + px, ry, pw - 2 * px, rh, px, s_sel[PAGE_COLOR] == i, labels[i], vp);
            }
            const char *hint = "LEFT/RIGHT: CHANGE   HOLD Y: PREVIEW   B: BACK";
            text(x + (pw - text_w(hint, px * 0.6f)) / 2, ry + rh * 0.6f, px * 0.6f, C_DIM, hint);
        } else if (s_page == PAGE_MAP) {
            for (int i = 0; i < P_COUNT; i++, ry += rh) {
                char label[32], value[64];
                if (i < GC_COUNT) {
                    snprintf(label, sizeof label, "GAMECUBE %s", GC_NAMES[i]);
                    if (s_capture == i) snprintf(value, sizeof value, "PRESS A BUTTON...");
                    else map_name(g_set.map[i], value, sizeof value);
                    row(x + px, ry, pw - 2 * px, rh, px, s_sel[PAGE_MAP] == i, label, value);
                } else {
                    row(x + px, ry, pw - 2 * px, rh, px, s_sel[PAGE_MAP] == i, i == P_RESET ? "RESET TO DEFAULTS" : "BACK", NULL);
                }
            }
            const char *hint = s_capture >= 0 ? "PRESS THE NEW BUTTON   -: CANCEL" : "A: CHANGE   B: BACK";
            text(x + (pw - text_w(hint, px * 0.6f)) / 2, ry + rh * 0.6f, px * 0.6f, C_DIM, hint);
        } else {
            const char *l1 = "UNSAVED RACE PROGRESS IS LOST", *l2 = "A: EXIT   B: BACK";
            text(x + (pw - text_w(l1, px)) / 2, ry + (rh - 7 * px) / 2, px, C_TEXT, l1);
            text(x + (pw - text_w(l2, px)) / 2, ry + rh + (rh - 7 * px) / 2, px, C_DIM, l2);
        }
    }
    flush();
    glDisable(GL_BLEND);
    glUseProgram(0);
}
