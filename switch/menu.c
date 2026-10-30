/* The - menu (graphics, colour, controls, button mapping, game, mods, debugging, update, exit) and the on-screen
 * overlay, styled after the game's own menus: its MarioFont letters (read from the disc, see
 * ui_font.c), a navy star panel with a red border and a checkered title band. Drawn after the
 * game image with one small instanced program (see "drawing"); the start-up screen
 * (menu_loading) uses the same pieces. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>
#include <GLES3/gl32.h>

#include "menu.h"
#include "render_gl.h"
#include "hdtex.h"
#include "ui_font.h"
#include "route.h"
#include "pause_art.h"
#include "update.h"
#include "../runtime/rt.h"
#include "../runtime/gx.h"
#include "../runtime/table.h"

#define MT_N_SPEEDS 6  /* MT_SPEEDS entries */
#define MT_N_LENGTHS 5 /* MT_LENGTHS entries */

Settings g_set;
static char s_path[512];

const uint64_t g_map_default[MAP_COUNT] = {
    [GC_A] = HidNpadButton_A, [GC_B] = HidNpadButton_B, [GC_X] = HidNpadButton_X, [GC_Y] = HidNpadButton_Y,
    [GC_Z] = HidNpadButton_L | HidNpadButton_R, [GC_R] = HidNpadButton_ZR,
    [GC_START] = HidNpadButton_Plus, [MAP_KFP] = HidNpadButton_StickR, [MAP_JUMP] = HidNpadButton_ZL,
    [MAP_CAM_RESET] = HidNpadButton_StickL,
};
/* BUTTON MAPPING's default (driving): as g_map_default, but L is rear view (GameCube Y, with Y) and
 * only R switches (GameCube Z) */
static const uint64_t MAP_DRIVE_DEFAULT[MAP_COUNT] = {
    [GC_A] = HidNpadButton_A, [GC_B] = HidNpadButton_B, [GC_X] = HidNpadButton_X, [GC_Y] = HidNpadButton_Y | HidNpadButton_L,
    [GC_Z] = HidNpadButton_R, [GC_R] = HidNpadButton_ZR,
    [GC_START] = HidNpadButton_Plus, [MAP_KFP] = HidNpadButton_StickR, [MAP_JUMP] = HidNpadButton_ZL,
    [MAP_CAM_RESET] = HidNpadButton_StickL,
};
static const char *GC_KEYS[MAP_COUNT] = {"map_a", "map_b", "map_x", "map_y", "map_z", "map_l", "map_r", "map_start", "map_fp", "map_jump", "map_cam_reset", "map_kfp"};
static const char *JOY_KEYS[MAP_COUNT] = {"joycon_a", "joycon_b", "joycon_x", "joycon_y", "joycon_z", "joycon_l", "joycon_r", "joycon_start",
                                          "joycon_fp", "joycon_jump", "joycon_cam_reset", "joycon_kfp"};
/* JOY-CON SIDEWAYS's default (driving): main_switch.c's fixed layout, SL drifting too (GameCube L does) */
static const uint64_t JOY_DRIVE_DEFAULT[MAP_COUNT] = {
    [GC_A] = HidNpadButton_A, [GC_B] = HidNpadButton_B, [GC_X] = HidNpadButton_X, [GC_Y] = HidNpadButton_Y,
    [GC_Z] = HidNpadButton_L | HidNpadButton_StickL, [GC_R] = HidNpadButton_LeftSR | HidNpadButton_LeftSL,
    [GC_START] = HidNpadButton_Plus | HidNpadButton_ZL,
};

/* Switch buttons the BUTTON MAPPING page assigns (− is reserved for the menu; the sticks and the
 * d-pad always steer). */
typedef struct { uint64_t bit; const char *name; } SwButton;
static const SwButton FULL_BUTTONS[] = {
    {HidNpadButton_A, "A"}, {HidNpadButton_B, "B"}, {HidNpadButton_X, "X"}, {HidNpadButton_Y, "Y"},
    {HidNpadButton_L, "L"}, {HidNpadButton_R, "R"}, {HidNpadButton_ZL, "ZL"}, {HidNpadButton_ZR, "ZR"},
    {HidNpadButton_Plus, "+"}, {HidNpadButton_StickL, "LEFT STICK PRESS"}, {HidNpadButton_StickR, "RIGHT STICK PRESS"},
};
/* a single Joy-Con held sideways, rail on top: face buttons by where they are, the shoulder,
 * trigger and stick press whichever Joy-Con it is (main_switch.c sideways_buttons) */
static const SwButton JOY_BUTTONS[] = {
    {HidNpadButton_A, "RIGHT BUTTON"}, {HidNpadButton_B, "BOTTOM BUTTON"}, {HidNpadButton_X, "TOP BUTTON"},
    {HidNpadButton_Y, "LEFT BUTTON"}, {HidNpadButton_LeftSL, "SL"}, {HidNpadButton_LeftSR, "SR"},
    {HidNpadButton_L, "L / R"}, {HidNpadButton_ZL, "ZL / ZR"}, {HidNpadButton_Plus, "+ / -"}, {HidNpadButton_StickL, "STICK PRESS"},
};
/* the set BUTTON MAPPING shows and edits: 0 full controllers, 1 a single Joy-Con sideways */
static int s_mset;
#define SW_BUTTONS (s_mset ? JOY_BUTTONS : FULL_BUTTONS)
#define N_SW (s_mset ? (int)(sizeof JOY_BUTTONS / sizeof JOY_BUTTONS[0]) : (int)(sizeof FULL_BUTTONS / sizeof FULL_BUTTONS[0]))
#define CUR_MAP (s_mset ? g_set.jmap : g_set.map)
#define CUR_DEFAULT (s_mset ? JOY_DRIVE_DEFAULT : MAP_DRIVE_DEFAULT)
static uint64_t mappable(void) {
    uint64_t m = 0;
    for (int i = 0; i < N_SW; i++) m |= SW_BUTTONS[i].bit;
    return m;
}
/* What each Switch button does while driving: an action is the GameCube button it presses
 * (g_set.map[gc] holds the Switch buttons pressing it), or one of the port's own (MAP_FP driver
 * high first person, MAP_KFP kart first person, JUMP, CAMERA RESET). Menus, the pause screen and everything outside driving always use
 * g_map_default (main_switch.c map_full). GameCube L is no action: nothing needs it while driving.
 * Rear view is GameCube Y: in races that player's camera turns around instead (main_switch.c). */
static const struct { int gc; const char *name, *hint; } ACTIONS[] = {
    {GC_A, "ACCELERATE", "ACCELERATE"},
    {GC_B, "BRAKE / REVERSE", "BRAKE AND REVERSE"},
    {GC_R, "DRIFT", "HOP AND DRIFT"},
    {GC_X, "USE ITEM", "USE YOUR ITEM"},
    {GC_Z, "SWITCH", "SWAP DRIVER AND RIDER"},
    {GC_Y, "REAR VIEW", "LOOK BEHIND IN RACES"},
    {GC_START, "PAUSE", "PAUSE"},
    {MAP_FP, "DRIVER FPV", "FIRST PERSON FROM THE DRIVER'S EYES, ON OR OFF"},
    {MAP_KFP, "KART FPV", "FIRST PERSON WITH THE KART IN VIEW, ON OR OFF"},
    {MAP_JUMP, "JUMP", "HOP IN RACES WHEN GAME > JUMP HEIGHT IS ON"},
    {MAP_CAM_RESET, "CAMERA RESET", "FREE CAMERA BACK TO THE NORMAL VIEW"},
};
#define N_ACT (int)(sizeof ACTIONS / sizeof ACTIONS[0]) /* also "NONE" */
/* the actions a race can't do without: the others may have no button */
static bool action_required(int a) {
    if (a >= N_ACT) return false;
    int gc = ACTIONS[a].gc;
    return gc == GC_A || gc == GC_B || gc == GC_R || gc == GC_X || gc == GC_START;
}
static int button_action(int sw) {
    for (int a = 0; a < N_ACT; a++)
        if (CUR_MAP[ACTIONS[a].gc] & SW_BUTTONS[sw].bit) return a;
    return N_ACT;
}
/* every required action has a button (settings from older builds or edited by hand may not) */
static bool map_valid(void) {
    for (int a = 0; a < N_ACT; a++)
        if (action_required(a) && !(CUR_MAP[ACTIONS[a].gc] & mappable())) return false;
    return true;
}
/* each required action without a button gets its default ones back (taken from whatever has
 * them); the number fixed, the first one's name in *name */
static int map_fix(const char **name) {
    int n = 0;
    for (int a = 0; a < N_ACT; a++) {
        int gc = ACTIONS[a].gc;
        if (!action_required(a) || (CUR_MAP[gc] & mappable())) continue;
        uint64_t bits = CUR_DEFAULT[gc] & mappable();
        for (int i = 0; i < MAP_COUNT; i++) CUR_MAP[i] &= ~bits;
        CUR_MAP[gc] |= bits;
        if (!n++ && name) *name = ACTIONS[a].name;
    }
    return n;
}

/* ---- settings.ini ---- */
/* first person's eye (CAMERA VIEW's page): above and ahead of the driver's head, degrees down */
#define DRV_UP 0
#define DRV_FWD 20
#define DRV_TILT 0
#define KART_UP 25
#define KART_FWD 10
#define KART_TILT 10
static void defaults(void) {
    g_set.wide = true;
    g_set.show_fps = 0;
    g_set.rumble = 1; /* low: the game's own Rumble option then works as players expect */
    g_set.unlock_all = false;
    g_set.any_kart = false;
    g_set.special_mode = 0;
    g_set.mt_sweeps = 3;
    g_set.mt_speed = g_set.mt_length = 0;
    g_set.mt_cpu = false;
    g_set.perfect_start = false;
    g_set.battle_cpus = true;
    g_set.jump = 2; /* MID, on ZL (g_map_default) */
    g_set.laps = 0;
    g_set.free_cam = 1; /* SNAP BACK */
    g_set.cam_height = 2; /* REGULAR */
    g_set.cam_dist = 2;   /* REGULAR */
    for (int i = 0; i < 20; i++) g_set.special[i] = i / 2; /* each pair's own special; Petey and King Boo any */
    g_set.logging = 0;
    g_set.hd_textures = false;
    g_set.characters = true;
    snprintf(g_set.mod, sizeof g_set.mod, "auto");
    g_set.fov[0] = 0; /* 4:3: the game's view */
    g_set.fov[1] = 2; /* 16:9: wider */
    g_set.aniso = 0;
    g_set.border = false;
    g_set.hud_hide = 0;
    g_set.hud_style = 1; /* modern */
    g_set.speed_kmh = false;
    g_set.vsplit = false;
    g_set.camera = 0;
    g_set.drv_up = DRV_UP, g_set.drv_forward = DRV_FWD, g_set.drv_tilt = DRV_TILT;
    g_set.kart_up = KART_UP, g_set.kart_forward = KART_FWD, g_set.kart_tilt = KART_TILT;
    g_set.docked_res = DOCKED_AUTO;
    g_set.handheld_res = HANDHELD_AUTO;
    g_set.profiler = false;
    g_set.game_clocks = false;
    g_set.lan = false;
    g_set.lan_peer[0] = 0;
    memcpy(g_set.map, MAP_DRIVE_DEFAULT, sizeof g_set.map);
    memcpy(g_set.jmap, JOY_DRIVE_DEFAULT, sizeof g_set.jmap);
}

#define LOG_MODES 5
static const uint32_t LOG_MASKS[LOG_MODES] = {0, LOG_GAME, LOG_GAME | LOG_AUDIO, LOG_AUDIO, LOG_GAME | LOG_BATTLE};
static const char *LOG_NAMES[LOG_MODES] = {"DISABLED", "GAME", "GAME + AUDIO", "AUDIO", "GAME + BATTLE AI"};
static const char *DOCKED_NAMES[4] = {"NATIVE", "2X", "1.5X", "AUTO"};
static const char *HANDHELD_NAMES[4] = {"NATIVE", "1.25X", "1.5X", "AUTO"};
static void apply_logging(void) {
    g_log_mask = LOG_MASKS[(unsigned)g_set.logging % LOG_MODES];
    diag_flush();
}

void settings_load(const char *path) {
    snprintf(s_path, sizeof s_path, "%s", path);
    defaults();
    FILE *f = fopen(path, "r");
    if (!f) { apply_logging(); return; }
    char line[256];
    bool kfp_seen = false; /* settings from before KART FPV had a button: FIRST PERSON's goes to it */
    bool view_seen = false; /* settings from before DRIVER FPV: their camera (and its eye height) moved over */
    int old_view = 0;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *v = eq + 1;
        v[strcspn(v, "\r\n")] = 0; /* text values (mod) must not keep the line end */
        const char *k = line;
        /* display: 0 = 4:3, otherwise 16:9 (older builds: 1 stretched, 2 wide view; "widescreen" before 0814) */
        if (!strcmp(k, "display") || !strcmp(k, "widescreen")) g_set.wide = atoi(v) != 0;
        else if (!strcmp(k, "show_fps")) g_set.show_fps = atoi(v) < 0 ? 0 : atoi(v) > 2 ? 2 : atoi(v);
        else if (!strcmp(k, "rumble")) g_set.rumble = atoi(v) < 0 ? 0 : atoi(v) > 2 ? 2 : atoi(v);
        else if (!strcmp(k, "unlock_all")) g_set.unlock_all = atoi(v) != 0;
        else if (!strcmp(k, "any_kart")) g_set.any_kart = atoi(v) != 0;
        else if (!strcmp(k, "mt_sweeps")) g_set.mt_sweeps = atoi(v) < 1 || atoi(v) > 3 ? 3 : atoi(v);
        else if (!strcmp(k, "mt_speed")) g_set.mt_speed = atoi(v) < 0 ? 0 : atoi(v) >= MT_N_SPEEDS ? MT_N_SPEEDS - 1 : atoi(v);
        else if (!strcmp(k, "mt_cpu")) g_set.mt_cpu = atoi(v) != 0;
        else if (!strcmp(k, "perfect_start")) g_set.perfect_start = atoi(v) != 0;
        else if (!strcmp(k, "single_battle")) g_set.battle_cpus = atoi(v) != 0;
        else if (!strcmp(k, "mt_length")) g_set.mt_length = atoi(v) < 0 ? 0 : atoi(v) >= MT_N_LENGTHS ? MT_N_LENGTHS - 1 : atoi(v);
        else if (!strcmp(k, "jump")) g_set.jump = atoi(v) < 0 || atoi(v) > 3 ? 0 : atoi(v);
        else if (!strcmp(k, "laps")) g_set.laps = atoi(v) < 0 || atoi(v) > 7 ? 0 : atoi(v);
        else if (!strcmp(k, "free_camera")) g_set.free_cam = atoi(v) < 0 || atoi(v) > 3 ? 1 : atoi(v);
        else if (!strcmp(k, "camera_height")) g_set.cam_height = atoi(v) < 0 || atoi(v) > 3 ? 2 : atoi(v);
        else if (!strcmp(k, "camera_distance")) g_set.cam_dist = atoi(v) < 0 || atoi(v) > 4 ? 2 : atoi(v);
        else if (!strcmp(k, "special_items")) g_set.special_mode = atoi(v) < 0 || atoi(v) > 2 ? 0 : atoi(v);
        else if (!strcmp(k, "special")) /* one digit per character, SPECIAL_NAMES index */
            for (int i = 0; i < 20 && v[i] >= '0' && v[i] <= '9'; i++) g_set.special[i] = v[i] - '0';
        else if (!strcmp(k, "logging")) g_set.logging = (int)((unsigned)atoi(v) % LOG_MODES);
        else if (!strcmp(k, "hd_textures")) g_set.hd_textures = atoi(v) != 0;
        else if (!strcmp(k, "characters")) g_set.characters = atoi(v) != 0;
        else if (!strcmp(k, "mod")) snprintf(g_set.mod, sizeof g_set.mod, "%s", v);
        else if (!strcmp(k, "plus")) snprintf(g_set.mod, sizeof g_set.mod, atoi(v) ? "auto" : "off"); /* before 1.4.0 */
        else if (!strcmp(k, "fov_4_3")) g_set.fov[0] = atoi(v) < 0 ? 0 : atoi(v) > 4 ? 4 : atoi(v);
        else if (!strcmp(k, "fov_16_9")) g_set.fov[1] = atoi(v) < 0 ? 0 : atoi(v) > 4 ? 4 : atoi(v);
        else if (!strcmp(k, "aniso")) g_set.aniso = atoi(v) < 0 ? 0 : atoi(v) > 4 ? 4 : atoi(v);
        else if (!strcmp(k, "border")) g_set.border = atoi(v) != 0;
        else if (!strcmp(k, "hud_hide")) g_set.hud_hide = (unsigned)atoi(v) & HUD_ALL;
        else if (!strcmp(k, "hud_style")) g_set.hud_style = atoi(v) == 1;
        else if (!strcmp(k, "speed_kmh")) g_set.speed_kmh = atoi(v) != 0;
        else if (!strcmp(k, "split_side_by_side")) g_set.vsplit = atoi(v) != 0;
        else if (!strcmp(k, "camera_view")) g_set.camera = atoi(v) < 0 || atoi(v) > 2 ? 0 : atoi(v), view_seen = true;
        else if (!strcmp(k, "camera")) old_view = atoi(v);
        else if (!strcmp(k, "driver_eye_up")) g_set.drv_up = atoi(v);
        else if (!strcmp(k, "driver_eye_forward")) g_set.drv_forward = atoi(v);
        else if (!strcmp(k, "driver_tilt")) g_set.drv_tilt = atoi(v);
        else if (!strcmp(k, "kart_eye_up")) g_set.kart_up = atoi(v);
        else if (!strcmp(k, "kart_eye_forward")) g_set.kart_forward = atoi(v);
        else if (!strcmp(k, "kart_tilt")) g_set.kart_tilt = atoi(v);
        else if (!strcmp(k, "profiler")) g_set.profiler = atoi(v) != 0;
        else if (!strcmp(k, "game_clocks")) g_set.game_clocks = atoi(v) != 0;
        else if (!strcmp(k, "lan")) g_set.lan = atoi(v) != 0;
        else if (!strcmp(k, "lan_peer")) snprintf(g_set.lan_peer, sizeof g_set.lan_peer, "%s", v);
        else if (!strcmp(k, "handheld_res")) g_set.handheld_res = atoi(v) >= 0 && atoi(v) <= HANDHELD_AUTO ? atoi(v) : HANDHELD_AUTO;
        else if (!strcmp(k, "docked_res")) g_set.docked_res = atoi(v) >= 0 && atoi(v) <= DOCKED_AUTO ? atoi(v) : DOCKED_AUTO;
        else
            for (int i = 0; i < MAP_COUNT; i++)
                if (!strcmp(k, GC_KEYS[i])) g_set.map[i] = strtoull(v, NULL, 16), kfp_seen |= i == MAP_KFP;
                else if (!strcmp(k, JOY_KEYS[i])) g_set.jmap[i] = strtoull(v, NULL, 16);
    }
    fclose(f);
    if (!view_seen && old_view >= 1 && old_view <= 4) {
        static const int OLD_UP[3] = {-40, -20, DRV_UP}; /* DRIVER LOW, MID, HIGH */
        g_set.camera = old_view == 4 ? 2 : 1;
        if (old_view < 4) g_set.drv_up = OLD_UP[old_view - 1];
    }
    if (!kfp_seen && g_set.map[MAP_FP]) g_set.map[MAP_KFP] = g_set.map[MAP_FP], g_set.map[MAP_FP] = 0;
    /* one action per Switch button (older builds could give one button several), the first in
     * ACTIONS order kept; a required action left without one gets its default buttons back */
    for (s_mset = 0; s_mset < 2; s_mset++) {
        for (int i = 0; i < MAP_COUNT; i++) CUR_MAP[i] &= mappable();
        for (int sw = 0; sw < N_SW; sw++) {
            int a = button_action(sw);
            for (int i = 0; i < MAP_COUNT; i++)
                if (a == N_ACT || i != ACTIONS[a].gc) CUR_MAP[i] &= ~SW_BUTTONS[sw].bit;
        }
        if (!map_valid()) {
            const char *name = "";
            int n = map_fix(&name);
            rt_log("settings: %s mapping missing %d required action(s) (%s), their default buttons back", s_mset ? "Joy-Con" : "button", n, name);
        }
    }
    s_mset = 0;
    apply_logging();
    rt_log("settings: 16:9 %d, fov %d / %d, fps %d, logging %d, rumble %d, unlock all %d", g_set.wide, g_set.fov[0], g_set.fov[1], g_set.show_fps,
           g_set.logging, g_set.rumble, g_set.unlock_all);
}

void settings_save(void) {
    FILE *f = fopen(s_path, "w");
    if (!f) return;
    fprintf(f, "display=%d\nshow_fps=%d\nlogging=%d\nrumble=%d\nunlock_all=%d\nhd_textures=%d\nfov_4_3=%d\nfov_16_9=%d\naniso=%d\nborder=%d\n"
            "docked_res=%d\nprofiler=%d\nmod=%s\nhandheld_res=%d\nhud_hide=%u\nhud_style=%d\nspeed_kmh=%d\ncamera_view=%d\ndriver_eye_up=%d\ndriver_eye_forward=%d\ndriver_tilt=%d\nkart_eye_up=%d\nkart_eye_forward=%d\nkart_tilt=%d\nany_kart=%d\n", g_set.wide, g_set.show_fps,
            g_set.logging, g_set.rumble, g_set.unlock_all, g_set.hd_textures, g_set.fov[0], g_set.fov[1], g_set.aniso, g_set.border, g_set.docked_res,
            g_set.profiler, g_set.mod, g_set.handheld_res, g_set.hud_hide, g_set.hud_style, g_set.speed_kmh, g_set.camera, g_set.drv_up, g_set.drv_forward, g_set.drv_tilt, g_set.kart_up, g_set.kart_forward, g_set.kart_tilt, g_set.any_kart);
    fprintf(f, "mt_cpu=%d\nsingle_battle=%d\nperfect_start=%d\n", g_set.mt_cpu, g_set.battle_cpus, g_set.perfect_start);
    fprintf(f, "mt_sweeps=%d\nmt_speed=%d\nmt_length=%d\njump=%d\nlaps=%d\nfree_camera=%d\ncamera_height=%d\ncamera_distance=%d\n", g_set.mt_sweeps, g_set.mt_speed,
            g_set.mt_length, g_set.jump, g_set.laps, g_set.free_cam, g_set.cam_height, g_set.cam_dist);
    fprintf(f, "characters=%d\n", g_set.characters);
    fprintf(f, "game_clocks=%d\n", g_set.game_clocks);
    fprintf(f, "split_side_by_side=%d\n", g_set.vsplit);
    fprintf(f, "lan=%d\nlan_peer=%s\n", g_set.lan, g_set.lan_peer);
    fprintf(f, "special_items=%d\nspecial=", g_set.special_mode);
    for (int i = 0; i < 20; i++) fputc('0' + g_set.special[i] % 10, f);
    fputc('\n', f);
    for (int i = 0; i < MAP_COUNT; i++) fprintf(f, "%s=%llx\n", GC_KEYS[i], (unsigned long long)g_set.map[i]);
    for (int i = 0; i < MAP_COUNT; i++) fprintf(f, "%s=%llx\n", JOY_KEYS[i], (unsigned long long)g_set.jmap[i]);
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
/* Pages by what they change: graphics (the picture), controls (pads, buttons, rumble), game
 * (unlock all), multiplayer (LAN play), mods (MKDD Plus, HD textures), debugging (diagnostics), update. */
enum { PAGE_MAIN, PAGE_MAP, PAGE_EXIT, PAGE_COLOR, PAGE_DEBUG, PAGE_GFX, PAGE_UPDATE, PAGE_CTRL, PAGE_GAME, PAGE_MODS, PAGE_HUD, PAGE_CHEATS, PAGE_SPECIAL, PAGE_CAM, PAGE_KART,
       PAGE_MULTI, PAGE_LAN, PAGE_COUNT };
/* the main page's rows; UPDATE and DEBUG sit on EXIT GAME's row, at its right (LEFT/RIGHT there) */
enum { M_GAME, M_GFX, M_CAM, M_CTRL, M_MULTI, M_MODS, M_CHEATS, M_EXIT, M_UPDATE, M_DEBUG, M_COUNT };
enum { MD_MOD, MD_HD, MD_CHARS, MD_BATTLE, MD_RESTART, MD_BACK, MD_COUNT };
enum { MP_SPLIT, MP_LAN, MP_BACK, MP_COUNT };
/* LAN PLAY's page (A on MULTIPLAYER's LAN PLAY): RESTART NOW only while a change waits for one (lan_rows) */
enum { LN_LAN, LN_PEER, LN_LINK, LN_RESTART, LN_BACK, LN_COUNT };
static unsigned s_mods_found, s_mod_running;
void menu_set_mods(unsigned found, unsigned running) { s_mods_found = found; s_mod_running = running; }
unsigned mods_pick(unsigned found) {
    if (!strcmp(g_set.mod, "off")) return 0;
    for (unsigned n = 1; n <= MOD_COUNT && n <= 32; n++)
        if ((found >> (n - 1) & 1) && (!strcmp(g_set.mod, "auto") || !strcmp(g_set.mod, MODS[n - 1].key))) return n;
    return 0;
}
/* The MOD setting picks another mod than this session plays: a restart applies it. */
/* mods/characters: there at start-up, and loaded then (EXTRA CHARACTERS was on) */
static bool s_chars_found, s_chars_running;
void menu_set_chars(bool found, bool running) { s_chars_found = found, s_chars_running = running; }
static bool chars_pending(void) { return s_chars_found && g_set.characters != s_chars_running; }
/* LAN PLAY and LAN PEER: eth.c reads g_lan_on when the game looks for the adapter at start-up, and
 * g_lan_peer when the tunnel opens, so a change waits for a restart too */
static bool lan_pending(void) { return g_set.lan != (g_lan_on != 0) || (g_set.lan && strcmp(g_set.lan_peer, g_lan_peer)); }
static int lan_rows(void) { return lan_pending() ? LN_COUNT : LN_COUNT - 1; }
static int lan_item(int row) { return !lan_pending() && row >= LN_RESTART ? row + 1 : row; } /* row -> LN_ */
/* MODS' own changes; mod_pending: anything RESTART NOW (on either page) applies */
static bool mods_changed(void) { return mods_pick(s_mods_found) != s_mod_running || chars_pending(); }
static bool mod_pending(void) { return mods_changed() || lan_pending(); }
static bool s_restart_manual; /* installed, but the loader can't relaunch: the player restarts */
/* RESTART NOW (MODS and MULTIPLAYER): the loader starts this .nro again as soon as it exits */
static void restart_now(bool *quit) {
    if (!mod_pending()) return;
    if (update_relaunch()) *quit = true;
    else s_restart_manual = true;
}
/* MULTIPLAYER's CONNECTION row (value) and its hint, from eth_status. */
static void link_text(char *val, size_t vn, char *hint, size_t hn) {
    EthStatus st;
    eth_status(&st);
    if (!st.up || !g_lan_play) { /* the adapter opens at start-up; LAN mode starts with its first send */
        snprintf(val, vn, g_lan_on ? "NOT STARTED" : "OFF");
        snprintf(hint, hn, g_lan_on ? "STARTS WITH WI-FI MODE ON THE TITLE SCREEN" : "LAN PLAY IS OFF");
    } else if (!st.consoles) {
        snprintf(val, vn, "LOOKING...");
        snprintf(hint, hn, st.seconds < 10 ? "LOOKING FOR OTHER CONSOLES ON THIS NETWORK"
                                           : "NONE FOUND: NOT ON A GUEST NETWORK? ELSE SET LAN PEER");
    } else if (st.silent == st.consoles) {
        snprintf(val, vn, "%u FOUND", st.consoles);
        snprintf(hint, hn, "NO TIMES: THE OTHER CONSOLE HAS AN OLDER BUILD");
    } else {
        snprintf(val, vn, "%u FOUND, %u MS", st.consoles, st.rtt_ms);
        if (st.loss_pct < 0) snprintf(hint, hn, "ROUND TRIP %u MS, UP TO %u. MEASURING LOSS...", st.rtt_ms, st.rtt_max_ms);
        else snprintf(hint, hn, "ROUND TRIP %u MS, UP TO %u. %d%% LOST%s", st.rtt_ms, st.rtt_max_ms, st.loss_pct,
                      st.rtt_max_ms > 50 || st.loss_pct > 5 ? ": TRY 5 GHZ, NEARER THE ROUTER" : "");
    }
}
static const char *restart_hint(void) {
    return !mod_pending() ? "NOTHING TO APPLY" : s_restart_manual ? "CLOSE THE GAME AND START IT AGAIN"
                                                                : "A: RESTART THE GAME (UNSAVED RACE PROGRESS IS LOST)";
}
/* This console's address on the network (nifm), for the other console's LAN PEER; looked up every 2 s */
static const char *own_ip(void) {
    static char s[20] = "NOT CONNECTED";
    static u64 next;
    static bool nifm;
    u64 now = armGetSystemTick();
    if (now < next) return s;
    next = now + armNsToTicks(2000000000ull);
    if (!nifm) nifm = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
    u32 ip = 0;
    if (!nifm || R_FAILED(nifmGetCurrentIpAddress(&ip)) || !ip) snprintf(s, sizeof s, "NOT CONNECTED");
    else snprintf(s, sizeof s, "%u.%u.%u.%u", ip & 0xff, ip >> 8 & 0xff, ip >> 16 & 0xff, ip >> 24); /* network order */
    return s;
}
/* LEFT/RIGHT on LEVEL PACK: OFF, then each mod found, in MODS order. */
static void mod_step(int dir) {
    unsigned cur = mods_pick(s_mods_found), n = cur;
    do n = (n + MOD_COUNT + 1 + dir) % (MOD_COUNT + 1);
    while (n && !(s_mods_found >> (n - 1) & 1));
    snprintf(g_set.mod, sizeof g_set.mod, "%s", n ? MODS[n - 1].key : "off");
}
static void upper(char *dst, size_t size, const char *s) {
    size_t i = 0;
    for (; s[i] && i + 1 < size; i++) dst[i] = (char)toupper((unsigned char)s[i]);
    dst[i] = 0;
}
enum { C_MAP, C_JOYMAP, C_SYNC, C_BACK, C_COUNT };
enum { GM_STYLE, GM_UNITS, GM_HUD, GM_JUMP, GM_RUMBLE, GM_BACK, GM_COUNT };
enum { CA_VIEW, CA_HEIGHT, CA_DIST, CA_FOV, CA_FREE, CA_BACK, CA_COUNT };
/* the KART view's page (A on CAMERA VIEW while it is KART): its eye (KA_UP, KA_FWD: steps of 5 from
 * the driver's head; KA_TILT: degrees down) */
enum { KA_UP, KA_FWD, KA_TILT, KA_RESET, KA_BACK, KA_COUNT };
/* the page's three settings: DRIVER FPV's or KART FPV's (CAMERA VIEW 1 / 2), and their defaults */
static int *fpv_eye(int i) {
    bool kart = g_set.camera == 2;
    return i == KA_UP ? (kart ? &g_set.kart_up : &g_set.drv_up) : i == KA_FWD ? (kart ? &g_set.kart_forward : &g_set.drv_forward)
                      : (kart ? &g_set.kart_tilt : &g_set.drv_tilt);
}
static int fpv_default(int i) {
    bool kart = g_set.camera == 2;
    return i == KA_UP ? (kart ? KART_UP : DRV_UP) : i == KA_FWD ? (kart ? KART_FWD : DRV_FWD) : (kart ? KART_TILT : DRV_TILT);
}
static int step_clamp(int v, int d, int lo, int hi) { v += d; return v < lo ? lo : v > hi ? hi : v; }
enum { CH_UNLOCK, CH_KARTS, CH_SPECIAL, CH_MT_SWEEPS, CH_MT_SPEED, CH_MT_LENGTH, CH_MT_CPU, CH_START, CH_LAPS, CH_BACK, CH_COUNT };
static const char *SPECIAL_MODES[3] = {"NORMAL", "ANY", "CUSTOM"};
/* MINI-TURBO SPEED (times the kart's top speed) and LENGTH (times the kart's own), the game's first */
static const struct { const char *name; float v; } MT_SPEEDS[] = {
    {"NORMAL", 1.3f}, {"+10%", 1.43f}, {"+20%", 1.56f}, {"+30%", 1.69f}, {"+50%", 1.95f}, {"+100%", 2.6f}};
static const struct { const char *name; float v; } MT_LENGTHS[] = {
    {"NORMAL", 1.0f}, {"1.5X", 1.5f}, {"2X", 2.0f}, {"3X", 3.0f}, {"4X", 4.0f}};
static const char *MT_SWEEP_NAMES[3] = {"1", "2", "3 (NORMAL)"};
/* JUMP: off, then the hop's upward speed (gravity takes about 12-18 a frame) */
static const char *JUMP_NAMES[4] = {"OFF", "LOW", "MID", "HIGH"};
static const float JUMP_POWER[4] = {0, 45.0f, 75.0f, 110.0f};
void special_apply(void);
/* SPECIAL ITEMS page (CUSTOM): one row per character, by ID, then BACK */
enum { SP_BACK = 20, SP_COUNT };
static const char *CHAR_NAMES[20] = {"MARIO", "LUIGI", "PEACH", "DAISY", "YOSHI", "BIRDO", "BABY MARIO", "BABY LUIGI", "TOAD", "TOADETTE",
                                     "KOOPA", "PARATROOPA", "DONKEY KONG", "DIDDY KONG", "BOWSER", "BOWSER JR", "WARIO", "WALUIGI",
                                     "PETEY PIRANHA", "KING BOO"};
static const char *SPECIAL_NAMES[10] = {"FIREBALLS", "HEART", "EGG", "CHAIN CHOMP", "GOLDEN MUSHROOM", "TRIPLE SHELLS",
                                        "GIANT BANANA", "BOWSER SHELL", "BOB-OMB", "ANY"};
/* the item kinds of SPECIAL_NAMES 0-8 (runtime g_special) */
static const uint8_t SPECIAL_KIND[9] = {7, 0x11, 0xe, 0x15, 8, 0xb, 4, 1, 0xc};
void cheats_apply(void) {
    if (g_lan_play) { /* the game's own on every console (rt.h) */
        g_mt_sweeps = 3, g_mt_speed = 1.3f, g_mt_length = 1.0f, g_mt_cpu = 0;
        g_perfect_start = 0;
        g_battle_cpus = g_set.battle_cpus; /* battle.c keeps Wi-Fi races the game's own */
        g_jump_power = 0;
        memset((void *)g_special, 0, sizeof g_special);
        return;
    }
    g_mt_sweeps = (uint32_t)g_set.mt_sweeps;
    g_mt_speed = MT_SPEEDS[g_set.mt_speed % MT_N_SPEEDS].v;
    g_mt_length = MT_LENGTHS[g_set.mt_length % MT_N_LENGTHS].v;
    g_mt_cpu = g_set.mt_cpu;
    g_perfect_start = g_set.perfect_start;
    g_battle_cpus = g_set.battle_cpus;
    g_jump_power = JUMP_POWER[g_set.jump & 3];
    special_apply();
}
void special_apply(void) {
    if (g_lan_play) return; /* cheats_apply's stock values */
    for (int i = 0; i < 20; i++) {
        int s = g_set.special[i] % 10;
        g_special[i + 1] = g_set.special_mode == 0 ? 0 : g_set.special_mode == 1 || s == 9 ? SPECIAL_ANY : SPECIAL_KIND[s];
    }
}
static const char *CAMERA_NAMES[3] = {"NORMAL", "DRIVER FPV", "KART FPV"};
enum { U_THIS, U_LATEST, U_ACTION, U_BACK, U_COUNT };
enum { G_WIDE, G_BORDER, G_DOCKED, G_HANDHELD, G_ANISO, G_COLOR, G_BACK, G_COUNT };
/* HUD ASSETS page: one row per element (HUD_* bit 1 << row), single player races */
enum { HU_PLACE, HU_ITEMS, HU_LAPS, HU_MAP, HU_SPEED, HU_ORDER, HU_TIME, HU_BACK, HU_COUNT };
static const char *HUD_LABELS[HU_COUNT] = {"POSITION", "ITEMS", "LAPS", "MAP", "SPEEDOMETER", "RACE ORDER", "RACE TIMER", "BACK"};
static const char *HUD_STYLES[2] = {"CLASSIC", "MODERN"};
static const char *hud_value(void) { return !g_set.hud_hide ? "ON" : g_set.hud_hide == HUD_ALL ? "OFF" : "CUSTOM"; }
static const char *FOV_NAMES[5] = {"NORMAL", "WIDE", "WIDER", "WIDEST", "WIDEEE"};
static const char *ANISO_NAMES[5] = {"NORMAL", "2X", "4X", "8X", "16X"};
enum { D_FPS, D_LOG, D_PROF, D_CLOCKS, D_BACK, D_COUNT };
static const char *FPS_NAMES[3] = {"OFF", "FPS", "FPS + TIMES"};
static const char *RUMBLE_NAMES[3] = {"OFF", "LOW", "HIGH"};
static bool s_want_controllers;
static bool s_want_peer; /* LAN PEER: the keyboard (main_switch.c) */
#define P_RESET N_SW
#define P_BACK (N_SW + 1)
#define P_COUNT (N_SW + 2)
enum { K_MODE, K_BRIGHT, K_CONTRAST, K_SAT, K_RESET, K_BACK, K_COUNT };
static bool s_open;
static bool s_preview;          /* colour page with Y held: panel hidden to see the whole image */
static int s_page, s_sel[PAGE_COUNT];
static uint64_t s_pressed;      /* buttons pressed while the menu was open */
static uint64_t s_block;        /* menu closed: those of them still held, kept from the game until released */
static char s_notice[64];       /* menu_open's reason, shown as the main page's hint until the cursor moves */

/* BUTTON MAPPING: give Switch button sw action a (N_ACT: none). That may leave a required action
 * without a button for a while (the hint says so): leaving the page gives it its default back
 * (map_leave). */
static bool s_map_warned;
static void set_action(int sw, int a) {
    int was = button_action(sw);
    uint64_t bit = SW_BUTTONS[sw].bit;
    if (was == a) return;
    s_notice[0] = 0;
    s_map_warned = false;
    for (int i = 0; i < MAP_COUNT; i++) CUR_MAP[i] &= ~bit;
    if (a < N_ACT) CUR_MAP[ACTIONS[a].gc] |= bit;
    if (action_required(was) && !(CUR_MAP[ACTIONS[was].gc] & mappable()))
        snprintf(s_notice, sizeof s_notice, "%s HAS NO BUTTON: GIVE IT ONE", ACTIONS[was].name);
    settings_save();
}
/* leaving BUTTON MAPPING (B, BACK; close: force): with a required action left without a button,
 * the first try stays and says so, the next (or closing) gives it its default buttons */
static bool map_leave(bool force) {
    if (map_valid()) return true;
    const char *name = "";
    if (!force && !s_map_warned) {
        for (int a = 0; a < N_ACT; a++)
            if (action_required(a) && !(CUR_MAP[ACTIONS[a].gc] & mappable())) { name = ACTIONS[a].name; break; }
        snprintf(s_notice, sizeof s_notice, "%s HAS NO BUTTON. B AGAIN: ITS DEFAULT", name);
        s_map_warned = true;
        return false;
    }
    map_fix(&name);
    s_map_warned = false;
    s_notice[0] = 0;
    settings_save();
    return true;
}

static int page_rows(int page) {
    switch (page) {
    case PAGE_MAIN: return M_EXIT + 1;
    case PAGE_MAP: return P_COUNT;
    case PAGE_COLOR: return K_COUNT;
    case PAGE_DEBUG: return D_COUNT;
    case PAGE_GFX: return G_COUNT;
    case PAGE_GAME: return GM_COUNT;
    case PAGE_CAM: return CA_COUNT;
    case PAGE_KART: return KA_COUNT;
    case PAGE_CHEATS: return CH_COUNT;
    case PAGE_SPECIAL: return SP_COUNT;
    case PAGE_MODS: return MD_COUNT;
    case PAGE_MULTI: return MP_COUNT;
    case PAGE_LAN: return LN_COUNT; /* the most it shows */
    case PAGE_UPDATE: return U_COUNT;
    case PAGE_CTRL: return C_COUNT;
    case PAGE_HUD: return HU_COUNT;
    default: return 1;
    }
}

/* B goes back one page: colour to graphics, button mapping to controls, race HUD to game,
 * everything else to the main page. */
static int page_parent(int page) {
    return page == PAGE_COLOR ? PAGE_GFX : page == PAGE_MAP ? PAGE_CTRL : page == PAGE_HUD ? PAGE_GAME
           : page == PAGE_SPECIAL ? PAGE_CHEATS : page == PAGE_KART ? PAGE_CAM : page == PAGE_LAN ? PAGE_MULTI : PAGE_MAIN;
}

void menu_open(const char *notice) {
    s_open = true;
    s_page = PAGE_MAIN;
    snprintf(s_notice, sizeof s_notice, "%s", notice ? notice : "");
}

bool menu_input(uint64_t down, uint64_t held, bool *quit) {
    if (!s_open) {
        s_block &= held;
        if (down & HidNpadButton_Minus) { s_open = true; s_page = PAGE_MAIN; s_pressed = HidNpadButton_Minus; }
        return s_open;
    }
    s_pressed |= down;
    const uint64_t UP = HidNpadButton_Up | HidNpadButton_StickLUp, DOWN = HidNpadButton_Down | HidNpadButton_StickLDown;
    const uint64_t SIDE = HidNpadButton_Left | HidNpadButton_Right | HidNpadButton_StickLLeft | HidNpadButton_StickLRight;
    int count = s_page == PAGE_LAN ? lan_rows() : page_rows(s_page);
    int *sel = &s_sel[s_page];
    if (s_page == PAGE_LAN && *sel >= count) *sel = count - 1; /* RESTART NOW gone */
    if (s_page == PAGE_MAIN && *sel > M_EXIT && (down & (UP | DOWN))) *sel = M_EXIT; /* UPDATE, DEBUG: on EXIT GAME's row */
    if (down & UP) *sel = (*sel + count - 1) % count;
    if (down & DOWN) *sel = (*sel + 1) % count;
    if (down & (UP | DOWN)) s_notice[0] = 0;
    bool ok = down & HidNpadButton_A, side = down & SIDE;
    bool left = down & (HidNpadButton_Left | HidNpadButton_StickLLeft);
    s_preview = s_page == PAGE_COLOR && (held & HidNpadButton_Y);
    if (down & (HidNpadButton_Minus | HidNpadButton_B)) {
        if (s_page == PAGE_MAIN || (down & HidNpadButton_Minus)) {
            if (s_page == PAGE_MAP) map_leave(true);
            s_open = false; s_block = s_pressed & held; s_preview = false; s_notice[0] = 0;
        } else if (s_page != PAGE_MAP || map_leave(false)) s_page = page_parent(s_page);
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
            s_page = PAGE_GFX;
            color_save();
        }
        return true;
    }
    if (s_page == PAGE_DEBUG && (ok || side)) {
        switch (*sel) {
        case D_FPS: g_set.show_fps = (g_set.show_fps + (left ? 2 : 1)) % 3; settings_save(); break;
        case D_LOG:
            /* A / right: next mode, left: previous */
            g_set.logging = (g_set.logging + (left ? LOG_MODES - 1 : 1)) % LOG_MODES;
            apply_logging();
            settings_save();
            break;
        case D_PROF: g_set.profiler = !g_set.profiler; settings_save(); break;
        case D_CLOCKS: g_set.game_clocks = !g_set.game_clocks; settings_save(); break;
        case D_BACK: if (ok) s_page = PAGE_MAIN; break;
        }
        return true;
    }
    if (s_page == PAGE_GFX && (ok || side)) {
        switch (*sel) {
        case G_WIDE: g_set.wide = !g_set.wide; settings_save(); break;
        case G_COLOR:
            if (ok) { s_page = PAGE_COLOR; s_sel[PAGE_COLOR] = 0; }
            else { g_color.mode = (g_color.mode + (left ? COLOR_MODES - 1 : 1)) % COLOR_MODES; s_color_dirty = true; color_save(); }
            break;
        case G_ANISO: g_set.aniso = (g_set.aniso + (left ? 4 : 1)) % 5; settings_save(); break;
        case G_DOCKED: {
            /* in the menu's order: NATIVE, 1.5X, 2X, AUTO */
            static const int ORDER[4] = {DOCKED_NATIVE, DOCKED_1_5X, DOCKED_2X, DOCKED_AUTO};
            int i = 0;
            while (i < 3 && ORDER[i] != g_set.docked_res) i++;
            g_set.docked_res = ORDER[(i + (left ? 3 : 1)) % 4];
            settings_save();
            break;
        }
        case G_HANDHELD: g_set.handheld_res = (g_set.handheld_res + (left ? 3 : 1)) % 4; settings_save(); break;
        case G_BORDER: g_set.border = !g_set.border; settings_save(); break;
        case G_BACK: if (ok) s_page = PAGE_MAIN; break;
        }
        return true;
    }
    if (s_page == PAGE_HUD && (ok || side)) {
        if (*sel == HU_BACK) { if (ok) s_page = PAGE_GAME; }
        else { g_set.hud_hide ^= 1u << *sel; settings_save(); }
        return true;
    }
    if (s_page == PAGE_CTRL && (ok || side)) {
        switch (*sel) {
        case C_SYNC: if (ok) { s_want_controllers = true; s_open = false; s_block = s_pressed & held; } break;
        case C_MAP:
        case C_JOYMAP: if (ok) { s_mset = *sel == C_JOYMAP; s_page = PAGE_MAP; s_sel[PAGE_MAP] = 0; } break;
        case C_BACK: if (ok) s_page = PAGE_MAIN; break;
        }
        return true;
    }
    if (s_page == PAGE_MODS && (ok || side)) {
        switch (*sel) {
        case MD_MOD: if (s_mods_found) { mod_step(left ? -1 : 1); settings_save(); } break;
        case MD_HD: g_set.hd_textures = !g_set.hd_textures; settings_save(); break;
        case MD_CHARS: if (s_chars_found) { g_set.characters = !g_set.characters; settings_save(); } break;
        case MD_BATTLE: g_set.battle_cpus = !g_set.battle_cpus; cheats_apply(); settings_save(); break;
        case MD_RESTART: if (ok) restart_now(quit); break;
        case MD_BACK: if (ok) s_page = PAGE_MAIN; break;
        }
        return true;
    }
    if (s_page == PAGE_MULTI && (ok || side)) {
        switch (*sel) {
        case MP_SPLIT: g_set.vsplit = !g_set.vsplit; settings_save(); break;
        case MP_LAN:
            if (ok) { s_page = PAGE_LAN; s_sel[PAGE_LAN] = 0; }
            else { g_set.lan = !g_set.lan; settings_save(); }
            break;
        case MP_BACK: if (ok) s_page = PAGE_MAIN; break;
        }
        return true;
    }
    if (s_page == PAGE_LAN && (ok || side)) {
        switch (lan_item(*sel)) {
        case LN_LAN: g_set.lan = !g_set.lan; settings_save(); break;
        case LN_PEER:
            if (ok) { s_want_peer = true; s_block = s_pressed & held; }
            else { g_set.lan_peer[0] = 0; settings_save(); } /* LEFT/RIGHT: none */
            break;
        case LN_RESTART: if (ok) restart_now(quit); break;
        case LN_BACK: if (ok) s_page = PAGE_MULTI; break;
        }
        return true;
    }
    if (s_page == PAGE_MAP && side && *sel < N_SW) {
        set_action(*sel, (button_action(*sel) + (left ? N_ACT : 1)) % (N_ACT + 1));
        return true;
    }
    if (s_page == PAGE_CHEATS && (ok || side)) {
        switch (*sel) {
        case CH_UNLOCK: g_set.unlock_all = !g_set.unlock_all; cheats_set(g_set.unlock_all); settings_save(); break;
        case CH_KARTS: g_set.any_kart = !g_set.any_kart; settings_save(); break;
        case CH_SPECIAL: /* LEFT/RIGHT: NORMAL, ANY, CUSTOM; A on CUSTOM: the characters' page */
            if (ok && g_set.special_mode == 2) { s_page = PAGE_SPECIAL; s_sel[PAGE_SPECIAL] = 0; break; }
            g_set.special_mode = (g_set.special_mode + (left ? 2 : 1)) % 3;
            special_apply(); settings_save(); break;
        case CH_MT_SWEEPS: g_set.mt_sweeps = (g_set.mt_sweeps - 1 + (left ? 2 : 1)) % 3 + 1; cheats_apply(); settings_save(); break;
        case CH_MT_SPEED: g_set.mt_speed = (g_set.mt_speed + (left ? MT_N_SPEEDS - 1 : 1)) % MT_N_SPEEDS; cheats_apply(); settings_save(); break;
        case CH_MT_LENGTH: g_set.mt_length = (g_set.mt_length + (left ? MT_N_LENGTHS - 1 : 1)) % MT_N_LENGTHS; cheats_apply(); settings_save(); break;
        case CH_MT_CPU: g_set.mt_cpu = !g_set.mt_cpu; cheats_apply(); settings_save(); break;
        case CH_START: g_set.perfect_start = !g_set.perfect_start; cheats_apply(); settings_save(); break;
        case CH_LAPS: g_set.laps = (g_set.laps + (left ? 7 : 1)) % 8; settings_save(); break;
        case CH_BACK: if (ok) s_page = PAGE_MAIN; break;
        }
        return true;
    }
    if (s_page == PAGE_SPECIAL && (ok || side)) {
        if (*sel == SP_BACK) { if (ok) s_page = PAGE_CHEATS; }
        else {
            g_set.special[*sel] = (g_set.special[*sel] % 10 + (left ? 9 : 1)) % 10;
            special_apply(); settings_save();
        }
        return true;
    }
    if (s_page == PAGE_GAME && (ok || side)) {
        switch (*sel) {
        case GM_STYLE: g_set.hud_style = !g_set.hud_style; settings_save(); break;
        case GM_UNITS: g_set.speed_kmh = !g_set.speed_kmh; settings_save(); break;
        case GM_HUD:
            if (ok) { s_page = PAGE_HUD; s_sel[PAGE_HUD] = 0; }
            else { g_set.hud_hide = g_set.hud_hide ? 0 : HUD_ALL; settings_save(); } /* LEFT/RIGHT: all on / all off */
            break;
        case GM_RUMBLE: g_set.rumble = (g_set.rumble + (left ? 2 : 1)) % 3; settings_save(); break;
        case GM_JUMP: g_set.jump = (g_set.jump + (left ? 3 : 1)) & 3; cheats_apply(); settings_save(); break;
        case GM_BACK: if (ok) s_page = PAGE_MAIN; break;
        }
        return true;
    }
    if (s_page == PAGE_CAM && (ok || side)) {
        switch (*sel) {
        case CA_VIEW: /* A on DRIVER FPV or KART FPV: its eye's page */
            if (ok && g_set.camera) { s_page = PAGE_KART; s_sel[PAGE_KART] = 0; break; }
            g_set.camera = (g_set.camera + (left ? 2 : 1)) % 3; settings_save(); break;
        case CA_HEIGHT: g_set.cam_height = (g_set.cam_height + (left ? 3 : 1)) & 3; settings_save(); break;
        case CA_DIST: g_set.cam_dist = (g_set.cam_dist + (left ? 4 : 1)) % 5; settings_save(); break;
        case CA_FOV: g_set.fov[g_set.wide] = (g_set.fov[g_set.wide] + (left ? 4 : 1)) % 5; settings_save(); break;
        case CA_FREE: g_set.free_cam = (g_set.free_cam + (left ? 3 : 1)) & 3; settings_save(); break;
        case CA_BACK: if (ok) s_page = PAGE_MAIN; break;
        }
        return true;
    }
    if (s_page == PAGE_KART && (ok || side)) {
        switch (*sel) {
        case KA_UP: *fpv_eye(KA_UP) = step_clamp(*fpv_eye(KA_UP), left ? -5 : 5, -100, 200); settings_save(); break;
        case KA_FWD: *fpv_eye(KA_FWD) = step_clamp(*fpv_eye(KA_FWD), left ? -5 : 5, -150, 150); settings_save(); break;
        case KA_TILT: *fpv_eye(KA_TILT) = step_clamp(*fpv_eye(KA_TILT), left ? -1 : 1, -45, 45); settings_save(); break;
        case KA_RESET:
            if (ok) { for (int i = KA_UP; i <= KA_TILT; i++) *fpv_eye(i) = fpv_default(i); settings_save(); }
            break;
        case KA_BACK: if (ok) { s_page = PAGE_CAM; s_sel[PAGE_CAM] = CA_VIEW; } break;
        }
        return true;
    }
    if (s_page == PAGE_MAIN && side && *sel >= M_EXIT) {
        /* EXIT GAME, UPDATE, DEBUG from left to right, no wrapping */
        *sel = left ? (*sel == M_DEBUG ? M_UPDATE : M_EXIT) : (*sel == M_EXIT ? M_UPDATE : M_DEBUG);
        return true;
    }
    if (s_page == PAGE_MAIN && ok) {
        switch (*sel) {
        case M_GFX: s_page = PAGE_GFX; s_sel[PAGE_GFX] = 0; break;
        case M_CTRL: s_page = PAGE_CTRL; s_sel[PAGE_CTRL] = 0; break;
        case M_CAM: s_page = PAGE_CAM; s_sel[PAGE_CAM] = 0; break;
        case M_GAME: s_page = PAGE_GAME; s_sel[PAGE_GAME] = 0; break;
        case M_CHEATS: s_page = PAGE_CHEATS; s_sel[PAGE_CHEATS] = 0; break;
        case M_MODS: s_page = PAGE_MODS; s_sel[PAGE_MODS] = 0; break;
        case M_MULTI: s_page = PAGE_MULTI; s_sel[PAGE_MULTI] = 0; break;
        case M_DEBUG: s_page = PAGE_DEBUG; s_sel[PAGE_DEBUG] = 0; break;
        case M_UPDATE: s_page = PAGE_UPDATE; s_sel[PAGE_UPDATE] = U_ACTION; break;
        case M_EXIT: s_page = PAGE_EXIT; break;
        }
    } else if (s_page == PAGE_MAP && ok) {
        if (*sel < N_SW) set_action(*sel, (button_action(*sel) + 1) % (N_ACT + 1));
        else if (*sel == P_RESET) { memcpy(CUR_MAP, CUR_DEFAULT, sizeof g_set.map); s_notice[0] = 0; settings_save(); }
        else if (map_leave(false)) s_page = PAGE_CTRL;
    } else if (s_page == PAGE_UPDATE && ok) {
        if (*sel == U_BACK) s_page = PAGE_MAIN;
        else if (*sel == U_ACTION) {
            UpdateStatus st;
            update_status(&st);
            if (st.state == UPD_READY) {
                /* the loader starts the new .nro as soon as this one exits */
                if (update_relaunch()) *quit = true;
                else s_restart_manual = true;
            } else update_start();
        }
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
/* A row with its label in the given colours (top, bottom). */
static void mrow_c(float x, float y, float w, float rh, float k, bool sel, const char *label, uint32_t ct, uint32_t cb) {
    float ts = 34 * k;
    if (ts > rh * 0.7f) ts = rh * 0.7f;
    if (sel) {
        grad(x + 14 * k, y + rh * 0.06f, w - 72 * k, rh * 0.88f, 0x5AA0FFE8u, 0x3C7CF0E8u);
        arrow(x + w - 58 * k, y + rh * 0.06f, 30 * k, rh * 0.88f, 0x4890F8E8u);
        arrow(x + 22 * k, y + rh * 0.3f, 14 * k, rh * 0.4f, 0xFFFFFFFFu);
    }
    mtext(x + 50 * k, y + (rh - ts) * 0.5f, ts, ct, cb, label);
}

static void mrow(float x, float y, float w, float rh, float k, bool sel, const char *label, const char *value, bool more) {
    float ts = 34 * k, ty;
    if (ts > rh * 0.7f) ts = rh * 0.7f;
    ty = y + (rh - ts) * 0.5f;
    if (sel) {
        grad(x + 14 * k, y + rh * 0.06f, w - 72 * k, rh * 0.88f, 0x5AA0FFE8u, 0x3C7CF0E8u);
        arrow(x + w - 58 * k, y + rh * 0.06f, 30 * k, rh * 0.88f, 0x4890F8E8u);
        arrow(x + 22 * k, y + rh * 0.3f, 14 * k, rh * 0.4f, 0xFFFFFFFFu);
    }
    mtext(x + 50 * k, ty, ts, sel ? C_SEL_T : C_TITLE_T, sel ? C_SEL_B : C_TITLE_B, label);
    if (value) mtext(x + w - 70 * k - mtext_w(value, ts), ty, ts, C_VAL_T, C_VAL_B, value);
    else if (more) arrow(x + w - 84 * k, y + rh * 0.3f, 16 * k, rh * 0.4f, 0xFFFFFFFFu);
}

bool menu_is_open(void) { return s_open; }
uint64_t menu_blocked(void) { return s_block; }
bool menu_take_controllers(void) {
    bool r = s_want_controllers;
    s_want_controllers = false;
    return r;
}
bool menu_take_peer(void) {
    bool r = s_want_peer;
    s_want_peer = false;
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
    if (!s_open && !g_set.show_fps) return;
    if (!init_gl()) return;
    menu_font_load();
    begin(w, h);
    float k = (float)h / 720.0f;

    if (g_set.show_fps) {
        char s[32];
        snprintf(s, sizeof s, "%.0f FPS", (double)fps);
        float px = 3 * k, lh = 7 * px + 4 * px;
        bool times = g_set.show_fps == 2 && perf && perf[0];
        /* Wi-Fi mode: the link under it (eth_status) */
        EthStatus st;
        eth_status(&st);
        st.up = st.up && g_lan_play; /* the game opens the adapter at start-up: only once LAN mode sends */
        char net[48] = "";
        if (st.up && !st.consoles) snprintf(net, sizeof net, "WI-FI: LOOKING");
        else if (st.up && st.silent == st.consoles) snprintf(net, sizeof net, "WI-FI: %u FOUND", st.consoles);
        else if (st.up && st.loss_pct >= 0) snprintf(net, sizeof net, "WI-FI %u MS (UP TO %u) %d%% LOST", st.rtt_ms, st.rtt_max_ms, st.loss_pct);
        else if (st.up) snprintf(net, sizeof net, "WI-FI %u MS", st.rtt_ms);
        float sp = px * 0.6f, lines = (times ? 1.0f : 0.0f) + (net[0] ? 1.0f : 0.0f);
        float bw = text_w(s, px);
        if (times && text_w(perf, sp) > bw) bw = text_w(perf, sp);
        if (net[0] && text_w(net, sp) > bw) bw = text_w(net, sp);
        rect(8 * k, 8 * k, bw + 6 * px, lh + lines * (7 * sp + 2 * px), 0x000000A0u);
        text(8 * k + 3 * px, 8 * k + 2 * px, px, fps >= 55 ? 0x80FF80FFu : fps >= 40 ? 0xFFE060FFu : 0xFF7060FFu, s);
        float ly = 8 * k + lh;
        if (times) text(8 * k + 3 * px, ly, sp, 0xE0E0E0FFu, perf), ly += 7 * sp + 2 * px;
        if (net[0]) text(8 * k + 3 * px, ly, sp, st.loss_pct > 5 || st.rtt_max_ms > 50 ? 0xFFE060FFu : 0xE0E0E0FFu, net);
    }
    if (s_open && !s_preview) {
        /* every page in the same box: sized for the longest page (at least 10 rows), except button
         * mapping, whose rows are made narrower to fit */
        int rows = 10;
        for (int p = 0; p < PAGE_COUNT; p++)
            if (p != PAGE_MAP && p != PAGE_SPECIAL && page_rows(p) > rows) rows = page_rows(p);
        float pw = 984 * k, top = 100 * k, foot = 56 * k;
        float rh = 58 * k, maxh = (float)h - 40 * k;
        if (top + rh * (float)rows + foot > maxh) rh = (maxh - top - foot) / (float)rows;
        float ph = top + rh * (float)rows + foot, x = ((float)w - pw) / 2, y = ((float)h - ph) / 2;
        /* the image is never dimmed behind the menu: colour and picture changes show as they look in play */
        const char *title = s_page == PAGE_MAP ? (s_mset ? "JOY-CON SIDEWAYS" : "BUTTON MAPPING") : s_page == PAGE_EXIT ? "EXIT THE GAME?"
                            : s_page == PAGE_COLOR ? "COLOR" : s_page == PAGE_DEBUG ? "DEBUG"
                            : s_page == PAGE_GFX ? "GRAPHICS" : s_page == PAGE_UPDATE ? "UPDATE"
                            : s_page == PAGE_CTRL ? "CONTROLS" : s_page == PAGE_GAME ? "GAME" : s_page == PAGE_CAM ? "CAMERA" : s_page == PAGE_KART ? (g_set.camera == 2 ? "KART FPV" : "DRIVER FPV")
                            : s_page == PAGE_MODS ? "MODS" : s_page == PAGE_MULTI ? "MULTIPLAYER" : s_page == PAGE_LAN ? "LAN PLAY" : s_page == PAGE_HUD ? "HUD ASSETS" : s_page == PAGE_CHEATS ? "CHEATS" : s_page == PAGE_SPECIAL ? "SPECIAL ITEMS" : "OPTIONS";
        panel(x, y, pw, ph, k, title);
        {
            /* "Oct 15 2026 14:08:52" -> "BUILD 1408" (the name of the build's saved .elf), in the
             * title band's top right of every page */
            char build[16];
            const char *b = g_build_stamp;
            snprintf(build, sizeof build, "BUILD %.2s%.2s", b + 12, b + 15);
            float bs = 22 * k;
            mtext(x + pw - mtext_w(build, bs) - 24 * k, y + 10 * k + (60 * k - bs) / 2, bs, C_VAL_T, C_VAL_B, build);
        }
        float ry = y + top;
        /* the last row of every page (BACK, EXIT GAME) sits at the bottom of the box */
        const float yb = y + top + rh * (float)(rows - 1);
#define ROW_Y(i, n) ((i) == (n) - 1 ? yb : ry)
        const char *hint = NULL;
        if (s_page == PAGE_MAIN) {
            static const char *labels[M_EXIT] = {"GAME", "GRAPHICS", "CAMERA", "CONTROLS", "MULTIPLAYER", "MODS", "CHEATS"};
            UpdateStatus us;
            update_status(&us);
            const int msel = s_sel[PAGE_MAIN];
            for (int i = 0; i < M_EXIT; i++, ry += rh)
                mrow(x, ry, pw, rh, k, msel == i, labels[i],
                     (i == M_MODS && mods_changed()) || (i == M_MULTI && lan_pending()) ? "RESTART" : NULL, true);
            {
                /* the bottom row: EXIT GAME, then small UPDATE and DEBUG buttons at the right (an
                 * available or downloaded update shows on its button) */
                const char *ulabel = us.state == UPD_AVAILABLE ? "UPDATE: NEW" : us.state == UPD_READY ? "UPDATE: RESTART" : "UPDATE";
                const char *blabel[2] = {ulabel, "DEBUG"};
                float bs = 28 * k;
                if (bs > rh * 0.6f) bs = rh * 0.6f;
                float bx = x + pw - 30 * k, bxs[2], bws[2];
                for (int i = 1; i >= 0; i--) {
                    bws[i] = mtext_w(blabel[i], bs) + 40 * k;
                    bx -= bws[i];
                    bxs[i] = bx;
                    bx -= 16 * k;
                }
                mrow(x, yb, bxs[0] - x + 12 * k, rh, k, msel == M_EXIT, "EXIT GAME", NULL, false);
                for (int i = 0; i < 2; i++) {
                    bool sel = msel == M_UPDATE + i, lit = i == 0 && (us.state == UPD_AVAILABLE || us.state == UPD_READY);
                    round_box(bxs[i], yb + rh * 0.1f, bws[i], rh * 0.8f, 10 * k, sel ? 0x5AA0FFE8u : 0x2457C8FFu);
                    mtext(bxs[i] + 20 * k, yb + (rh - bs) * 0.5f, bs, sel ? C_SEL_T : lit ? C_TITLE_T : C_VAL_T,
                          sel ? C_SEL_B : lit ? C_TITLE_B : C_DIM, blabel[i]);
                }
            }
            static char uhint[96];
            snprintf(uhint, sizeof uhint, "VERSION %s. DOWNLOAD AND INSTALL THE LATEST RELEASE", update_version());
            hint = s_notice[0]                    ? s_notice
                   : s_sel[PAGE_MAIN] == M_GFX    ? "WIDESCREEN, RESOLUTION, FILTERING, COLOR"
                   : s_sel[PAGE_MAIN] == M_CAM    ? "VIEW, HEIGHT, DISTANCE, FIELD OF VIEW, FREE CAMERA"
                   : s_sel[PAGE_MAIN] == M_MODS   ? "GAME MODS, HD TEXTURES"
                   : s_sel[PAGE_MAIN] == M_MULTI  ? "LAN PLAY OVER WI-FI WITH OTHER CONSOLES"
                   : s_sel[PAGE_MAIN] == M_CTRL   ? "BUTTON MAPPING, SYNC CONTROLLERS"
                   : s_sel[PAGE_MAIN] == M_GAME   ? "RACE HUD, SPEED, HUD ASSETS, JUMP, RUMBLE"
                   : s_sel[PAGE_MAIN] == M_CHEATS ? "UNLOCK ALL, NO WEIGHT CLASSES, SPECIAL ITEMS, MINI-TURBO, LAPS"
                   : s_sel[PAGE_MAIN] == M_DEBUG  ? "FPS AND FRAME TIMES, LOGS, PROFILER"
                   : s_sel[PAGE_MAIN] == M_UPDATE ? uhint
                   : s_sel[PAGE_MAIN] == M_EXIT   ? "A: EXIT THE GAME   RIGHT: UPDATE, DEBUG"
                                                  : "GAME PAUSED   A: SELECT   B OR -: CLOSE";
        } else if (s_page == PAGE_CTRL) {
            static const char *labels[C_COUNT] = {"BUTTON MAPPING", "JOY-CON SIDEWAYS", "SYNC CONTROLLERS", "BACK"};
            const char *values[C_COUNT] = {NULL, NULL, NULL, NULL};
            static const bool more[C_COUNT] = {false, false, true, false};
            for (int i = 0; i < C_COUNT; i++, ry += rh) mrow(x, ROW_Y(i, C_COUNT), pw, rh, k, s_sel[PAGE_CTRL] == i, labels[i], values[i], more[i]);
            hint = s_sel[PAGE_CTRL] == C_SYNC     ? "PAIR CONTROLLERS, SET THE PLAYER ORDER, SINGLE JOY-CONS"
                   : s_sel[PAGE_CTRL] == C_MAP    ? "CHOOSE WHAT EACH SWITCH BUTTON DOES"
                   : s_sel[PAGE_CTRL] == C_JOYMAP ? "BUTTONS OF ONE JOY-CON HELD SIDEWAYS"
                                                  : "B: BACK";
        } else if (s_page == PAGE_GFX) {
            static const char *labels[G_COUNT] = {"WIDESCREEN", "4:3 EDGE BORDER", "DOCKED RESOLUTION", "HANDHELD RESOLUTION",
                                                  "TEXTURE FILTERING", "COLOR", "BACK"};
            const char *values[G_COUNT] = {g_set.wide ? "16:9" : "4:3", g_set.border ? "ON" : "OFF", DOCKED_NAMES[g_set.docked_res & 3],
                                           HANDHELD_NAMES[g_set.handheld_res & 3], ANISO_NAMES[g_set.aniso % 5], COLOR_NAMES[g_color.mode], NULL};
            static const bool more[G_COUNT] = {false, false, false, false, false, true, false};
            for (int i = 0; i < G_COUNT; i++, ry += rh) mrow(x, ROW_Y(i, G_COUNT), pw, rh, k, s_sel[PAGE_GFX] == i, labels[i], values[i], more[i]);
            hint = s_sel[PAGE_GFX] == G_WIDE     ? "16:9: WIDER VIEW, RACE HUD AT THE SCREEN EDGES"
                   : s_sel[PAGE_GFX] == G_BORDER ? "4:3: CHECKERED BORDER BESIDE THE RACE"
                   : s_sel[PAGE_GFX] == G_DOCKED ? "ON THE TV, 1080P OUTPUT. AUTO: 2X, LOWER WHEN THE FRAME RATE DROPS"
                   : s_sel[PAGE_GFX] == G_HANDHELD ? "SHARPER IN HANDHELD. AUTO: 1.5X, LOWER WHEN THE FRAME RATE DROPS"
                   : s_sel[PAGE_GFX] == G_ANISO  ? "SHARPER TEXTURES AT ANGLES: ROADS AND WALLS IN THE DISTANCE"
                   : s_sel[PAGE_GFX] == G_COLOR  ? "A: ADJUST   LEFT/RIGHT: ACCURATE, VIVID, CUSTOM"
                                                 : "B: BACK";
        } else if (s_page == PAGE_MODS) {
            static const char *labels[MD_COUNT] = {"LEVEL PACK", "HD TEXTURES", "EXTRA CHARACTERS", "SINGLE PLAYER BATTLE MODE", "RESTART NOW", "BACK"};
            unsigned pick = mods_pick(s_mods_found);
            char mod_value[16], mod_hint[96];
            upper(mod_value, sizeof mod_value, pick ? MODS[pick - 1].key : s_mods_found ? "off" : "none found");
            upper(mod_hint, sizeof mod_hint, pick ? MODS[pick - 1].name : "the standard game");
            snprintf(mod_hint + strlen(mod_hint), sizeof mod_hint - strlen(mod_hint), pick ? ": ITS OWN SAVE" : "");
            const char *values[MD_COUNT] = {mod_value,
                                            !hdtex_available() ? "NO PACK" : g_set.hd_textures ? "ON" : "OFF",
                                            !s_chars_found ? "NONE FOUND" : g_set.characters ? "ON" : "OFF",
                                            g_set.battle_cpus ? "ON" : "OFF", NULL, NULL};
            for (int i = 0; i < MD_COUNT; i++, ry += rh) mrow(x, ROW_Y(i, MD_COUNT), pw, rh, k, s_sel[PAGE_MODS] == i, labels[i], values[i], false);
            char hd_hint[64];
            if (!hdtex_available()) snprintf(hd_hint, sizeof hd_hint, "PUT HD-TEXTURES.BIN IN THE MKDD-NX FOLDER");
            else if (!g_set.hd_textures) snprintf(hd_hint, sizeof hd_hint, "COMMUNITY HD TEXTURE PACK");
            else snprintf(hd_hint, sizeof hd_hint, "COMMUNITY HD TEXTURE PACK: %u IN USE", render_hd_in_use());
            const int s = s_sel[PAGE_MODS];
            hint = s == MD_MOD && !s_mods_found     ? "PUT A MOD IN THE MODS FOLDER (SEE README)"
                   : s == MD_MOD && mods_changed()   ? "CHANGES AFTER A RESTART: SEE RESTART NOW"
                   : s == MD_MOD                     ? mod_hint
                   : s == MD_HD                      ? hd_hint
                   : s == MD_CHARS && !s_chars_found ? "PUT CHARACTERS IN MODS/CHARACTERS (SEE README)"
                   : s == MD_CHARS && chars_pending() ? "CHANGES AFTER A RESTART: SEE RESTART NOW"
                   : s == MD_CHARS                   ? "MORE CHARACTERS ON THE CHARACTER SELECT SCREEN"
                   : s == MD_BATTLE                  ? "CPUS FILL BATTLES TO 4 KARTS. 1 PLAYER: BATTLE ON THE MODE PAGE"
                   : s == MD_RESTART                 ? restart_hint()
                                                     : "B: BACK";
        } else if (s_page == PAGE_MULTI) {
            static const char *labels[MP_COUNT] = {"SPLIT SCREEN", "LAN PLAY", "BACK"};
            const char *values[MP_COUNT] = {g_set.vsplit ? "SIDE BY SIDE" : "TOP AND BOTTOM", g_set.lan ? "ON" : "OFF", NULL};
            static const bool more[MP_COUNT] = {false, true, false};
            for (int i = 0; i < MP_COUNT; i++, ry += rh) mrow(x, ROW_Y(i, MP_COUNT), pw, rh, k, s_sel[PAGE_MULTI] == i, labels[i], values[i], more[i]);
            const int s = s_sel[PAGE_MULTI];
            hint = s == MP_SPLIT                  ? "2 PLAYERS: THE SCREENS LEFT AND RIGHT OR TOP AND BOTTOM"
                   : s == MP_LAN && lan_pending() ? "A: LAN SETTINGS. CHANGES AFTER A RESTART: SEE RESTART NOW THERE"
                   : s == MP_LAN                  ? "A: LAN SETTINGS AND CONNECTION   LEFT/RIGHT: ON, OFF"
                                                  : "B: BACK";
        } else if (s_page == PAGE_LAN) {
            static const char *labels[LN_COUNT] = {"LAN PLAY", "LAN PEER", "CONNECTION", "RESTART NOW", "BACK"};
            static char link_val[32], link_hint[96];
            link_text(link_val, sizeof link_val, link_hint, sizeof link_hint);
            const char *values[LN_COUNT] = {g_set.lan ? "ON" : "OFF", g_set.lan_peer[0] ? g_set.lan_peer : "ANY", link_val, NULL, NULL};
            const int n = lan_rows();
            for (int i = 0; i < n; i++, ry += rh) mrow(x, ROW_Y(i, n), pw, rh, k, s_sel[PAGE_LAN] == i, labels[lan_item(i)], values[lan_item(i)], false);
            const int s = lan_item(s_sel[PAGE_LAN] < n ? s_sel[PAGE_LAN] : n - 1);
            static char lan_hint[96];
            snprintf(lan_hint, sizeof lan_hint, "THIS CONSOLE: %s. A: TYPE THE OTHER'S. LEFT/RIGHT: ANY", own_ip());
            unsigned pick = mods_pick(s_mods_found);
            bool rtgp = MOD_IS_RTGP(pick); /* and its Baby / Classic */
            hint = s == LN_LAN && lan_pending()  ? "CHANGES AFTER A RESTART: SEE RESTART NOW"
                   : s == LN_LAN && rtgp           ? "NOT WITH RETRO TRACK GRAND PRIX: ITS LAN MODE DOESN'T RUN"
                   : s == LN_LAN                   ? "LAN MODE ON THE TITLE SCREEN, OVER WI-FI. SAME MOD ON EVERY CONSOLE"
                   : s == LN_PEER && lan_pending() ? "CHANGES AFTER A RESTART: SEE RESTART NOW"
                   : s == LN_PEER                  ? lan_hint
                   : s == LN_LINK                  ? link_hint
                   : s == LN_RESTART               ? restart_hint()
                                                   : "B: BACK";
        } else if (s_page == PAGE_GAME) {
            static const char *labels[GM_COUNT] = {"RACE HUD", "SPEED", "HUD ASSETS", "JUMP HEIGHT", "RUMBLE STRENGTH", "BACK"};
            const char *values[GM_COUNT] = {HUD_STYLES[g_set.hud_style & 1], g_set.speed_kmh ? "KM/H" : "MPH", hud_value(),
                                            JUMP_NAMES[g_set.jump & 3], RUMBLE_NAMES[g_set.rumble % 3], NULL};
            static const bool more[GM_COUNT] = {false, false, true, false, false, false};
            for (int i = 0; i < GM_COUNT; i++, ry += rh) mrow(x, ROW_Y(i, GM_COUNT), pw, rh, k, s_sel[PAGE_GAME] == i, labels[i], values[i], more[i]);
            hint = s_sel[PAGE_GAME] == GM_STYLE ? "MODERN: SMALLER, IN THE CORNERS. 1 TO 4 PLAYERS"
                   : s_sel[PAGE_GAME] == GM_UNITS ? "SPEEDOMETER IN MILES OR KILOMETRES PER HOUR"
                   : s_sel[PAGE_GAME] == GM_HUD  ? "A: CHOOSE   LEFT/RIGHT: ALL ON, ALL OFF"
                   : s_sel[PAGE_GAME] == GM_RUMBLE ? "VIBRATION STRENGTH. THE GAME'S OWN RUMBLE OPTION MUST BE ON TOO"
                   : s_sel[PAGE_GAME] == GM_JUMP ? (g_set.map[MAP_JUMP] ? "ONE HOP PER PRESS OF THE JUMP BUTTON"
                                                                       : "GIVE JUMP A BUTTON: CONTROLS > BUTTON MAPPING")
                                                 : "B: BACK";
        } else if (s_page == PAGE_CAM) {
            static const char *labels[CA_COUNT] = {"CAMERA VIEW", "CAMERA HEIGHT", "CAMERA DISTANCE", "FIELD OF VIEW", "FREE CAMERA", "BACK"};
            static const char *HEIGHT_NAMES[4] = {"LOW", "MEDIUM", "REGULAR", "HIGH"};
            static const char *DIST_NAMES[5] = {"VERY CLOSE", "CLOSE", "REGULAR", "FAR", "VERY FAR"};
            static const char *FREECAM_NAMES[4] = {"OFF", "SNAP BACK", "EASE BACK", "STAY"};
            const char *values[CA_COUNT] = {CAMERA_NAMES[g_set.camera % 3], HEIGHT_NAMES[g_set.cam_height & 3], DIST_NAMES[g_set.cam_dist % 5],
                                            FOV_NAMES[g_set.fov[g_set.wide] % 5], FREECAM_NAMES[g_set.free_cam & 3], NULL};
            for (int i = 0; i < CA_COUNT; i++, ry += rh) mrow(x, ROW_Y(i, CA_COUNT), pw, rh, k, s_sel[PAGE_CAM] == i, labels[i], values[i], i == CA_VIEW && g_set.camera);
            hint = s_sel[PAGE_CAM] == CA_VIEW     ? (g_set.camera == 2 ? "A: ADJUST KART FPV" : g_set.camera ? "A: ADJUST DRIVER FPV" : "FIRST PERSON: DRIVER FPV OR KART FPV")
                   : s_sel[PAGE_CAM] == CA_HEIGHT ? "THE CAMERA BEHIND THE KART. REGULAR: THE GAME'S"
                   : s_sel[PAGE_CAM] == CA_DIST   ? "HOW FAR BEHIND THE KART. REGULAR: THE GAME'S"
                   : s_sel[PAGE_CAM] == CA_FOV    ? (g_set.wide ? "RACE CAMERA IN 16:9, DEFAULT WIDER" : "RACE CAMERA IN 4:3, DEFAULT NORMAL")
                   : s_sel[PAGE_CAM] == CA_FREE   ? (g_set.free_cam == 0 ? "RIGHT STICK LEFT/RIGHT: TURN THE CAMERA ROUND THE KART"
                                                     : g_set.free_cam == 1 ? "RIGHT STICK TURNS THE CAMERA. LET GO: STRAIGHT BACK"
                                                     : g_set.free_cam == 2 ? "RIGHT STICK TURNS THE CAMERA. LET GO: GLIDES BACK"
                                                     : g_set.map[MAP_CAM_RESET] ? "RIGHT STICK TURNS THE CAMERA. CAMERA RESET: BACK"
                                                                                : "GIVE CAMERA RESET A BUTTON: CONTROLS > BUTTON MAPPING")
                                                  : "B: BACK";
        } else if (s_page == PAGE_KART) {
            static const char *labels[KA_COUNT] = {"EYE HEIGHT", "EYE FORWARD", "TILT", "DEFAULTS", "BACK"};
            static char kup[16], kfwd[16], ktilt[16], khint[48];
            snprintf(kup, sizeof kup, "%d", *fpv_eye(KA_UP));
            snprintf(kfwd, sizeof kfwd, "%d", *fpv_eye(KA_FWD));
            snprintf(ktilt, sizeof ktilt, "%d DEG", *fpv_eye(KA_TILT));
            const char *values[KA_COUNT] = {kup, kfwd, ktilt, NULL, NULL};
            for (int i = 0; i < KA_COUNT; i++, ry += rh) mrow(x, ROW_Y(i, KA_COUNT), pw, rh, k, s_sel[PAGE_KART] == i, labels[i], values[i], false);
            int ks = s_sel[PAGE_KART];
            if (ks <= KA_TILT)
                snprintf(khint, sizeof khint, "%s. DEFAULT %d", ks == KA_UP ? "ABOVE THE DRIVER'S HEAD" : ks == KA_FWD ? "AHEAD OF THE DRIVER'S HEAD"
                         : "HOW FAR IT LOOKS DOWN", fpv_default(ks));
            hint = ks <= KA_TILT                ? khint
                   : s_sel[PAGE_KART] == KA_RESET ? "A: BACK TO THE DEFAULTS"
                                                  : "B: BACK";
        } else if (s_page == PAGE_CHEATS) {
            static const char *labels[CH_COUNT] = {"UNLOCK ALL", "NO WEIGHT CLASSES", "SPECIAL ITEMS", "MINI-TURBO SWEEPS", "MINI-TURBO SPEED",
                                                   "MINI-TURBO LENGTH", "MINI-TURBO FOR CPUS", "PERFECT STARTS", "LAPS", "BACK"};
            static const char *LAP_NAMES[8] = {"NORMAL", "1", "2", "3", "4", "5", "6", "7"};
            const char *values[CH_COUNT] = {g_set.unlock_all ? "ON" : "OFF", g_set.any_kart ? "ON" : "OFF", SPECIAL_MODES[g_set.special_mode % 3],
                                            MT_SWEEP_NAMES[(g_set.mt_sweeps + 2) % 3], MT_SPEEDS[g_set.mt_speed % MT_N_SPEEDS].name,
                                            MT_LENGTHS[g_set.mt_length % MT_N_LENGTHS].name, g_set.mt_cpu ? "ON" : "OFF",
                                            g_set.perfect_start ? "ON" : "OFF", LAP_NAMES[g_set.laps & 7], NULL};
            for (int i = 0; i < CH_COUNT; i++, ry += rh)
                mrow(x, ROW_Y(i, CH_COUNT), pw, rh, k, s_sel[PAGE_CHEATS] == i, labels[i], values[i], i == CH_SPECIAL && g_set.special_mode == 2);
            hint = g_lan_play && s_sel[PAGE_CHEATS] != CH_UNLOCK && s_sel[PAGE_CHEATS] != CH_BACK
                       ? "OFF AFTER LAN MODE UNTIL A RESTART: EVERY CONSOLE MUST PLAY THE SAME GAME"
                   : s_sel[PAGE_CHEATS] == CH_UNLOCK  ? "ALL CHARACTERS, KARTS, CUPS: NEVER WRITTEN TO YOUR SAVE"
                   : s_sel[PAGE_CHEATS] == CH_KARTS ? "ANY UNLOCKED KART FOR ANY DRIVERS, WHATEVER THEIR WEIGHT"
                   : s_sel[PAGE_CHEATS] == CH_SPECIAL ? (g_set.special_mode == 2 ? "A: PICK A SPECIAL PER CHARACTER. FROM THE NEXT RACE"
                                                       : "NORMAL, ANY (ALL OF THEM, LIKE PETEY), CUSTOM. NEXT RACE")
                   : s_sel[PAGE_CHEATS] == CH_MT_SWEEPS ? "DRIFT STAGES TO BLUE SPARKS. 3: DRIFT + 2 SWEEPS, 1: AT ONCE"
                   : s_sel[PAGE_CHEATS] == CH_MT_SPEED ? "BOOST SPEED. NORMAL: 1.3X THE KART'S TOP SPEED"
                   : s_sel[PAGE_CHEATS] == CH_MT_LENGTH ? "BOOST TIME, TIMES THE KART'S OWN"
                   : s_sel[PAGE_CHEATS] == CH_MT_CPU ? "THE COMPUTER'S KARTS GET THE MINI-TURBO SETTINGS ABOVE TOO"
                   : s_sel[PAGE_CHEATS] == CH_START ? "EVERY PLAYER GETS THE START BOOST AT GO, NO TIMING NEEDED"
                   : s_sel[PAGE_CHEATS] == CH_LAPS ? "GRAND PRIX AND VERSUS, FROM THE NEXT RACE. NORMAL: THE COURSE'S"
                                                    : "B: BACK";
        } else if (s_page == PAGE_SPECIAL) {
            /* 20 characters and BACK: rows made narrower to fit, as button mapping */
            float mh = rh * (float)rows / (float)SP_COUNT;
            for (int i = 0; i < SP_COUNT; i++, ry += mh)
                mrow(x, ry, pw, mh, k, s_sel[PAGE_SPECIAL] == i, i < 20 ? CHAR_NAMES[i] : "BACK", i < 20 ? SPECIAL_NAMES[g_set.special[i] % 10] : NULL, false);
            hint = s_sel[PAGE_SPECIAL] == SP_BACK ? "B: BACK"
                   : g_set.special[s_sel[PAGE_SPECIAL]] % 10 == 9 ? "ANY: EVERY SPECIAL, LIKE PETEY AND KING BOO. LEFT/RIGHT: CHANGE"
                                                                  : "LEFT/RIGHT: CHANGE. FROM THE NEXT RACE";
        } else if (s_page == PAGE_UPDATE) {
            UpdateStatus us;
            update_status(&us);
            char action[48], latest[32];
            const char *lv = "?";
            if (us.version[0] && us.state != UPD_IDLE && us.state != UPD_CHECKING) lv = us.version;
            snprintf(latest, sizeof latest, "%s", us.state == UPD_CHECKING ? "CHECKING..." : lv);
            switch (us.state) {
            case UPD_AVAILABLE: snprintf(action, sizeof action, "INSTALL %s", us.version); break;
            case UPD_DOWNLOADING: snprintf(action, sizeof action, "DOWNLOADING %d%%", (int)(us.progress * 100 + 0.5f)); break;
            case UPD_READY: snprintf(action, sizeof action, "RESTART NOW"); break;
            case UPD_CHECKING: snprintf(action, sizeof action, "CHECKING..."); break;
            default: snprintf(action, sizeof action, "CHECK FOR UPDATES"); break;
            }
            const char *labels[U_COUNT] = {"THIS VERSION", "LATEST RELEASE", action, "BACK"};
            const char *values[U_COUNT] = {update_version(), latest, NULL, NULL};
            for (int i = 0; i < U_COUNT; i++, ry += rh) mrow(x, ROW_Y(i, U_COUNT), pw, rh, k, s_sel[PAGE_UPDATE] == i, labels[i], values[i], false);
            hint = us.state == UPD_ERROR         ? us.msg
                   : us.state == UPD_CURRENT     ? "YOU HAVE THE LATEST RELEASE"
                   : us.state == UPD_AVAILABLE   ? "A: DOWNLOAD AND INSTALL (THE OLD ONE IS KEPT AS .NRO.OLD)"
                   : us.state == UPD_DOWNLOADING ? "YOU CAN KEEP PLAYING: CLOSE THE MENU WITH B"
                   : us.state == UPD_READY       ? (s_restart_manual ? "INSTALLED: CLOSE THE GAME AND START IT AGAIN"
                                                                     : "INSTALLED: A RESTARTS THE GAME (SAVE YOUR PROGRESS FIRST)")
                   : us.state == UPD_CHECKING    ? "CONNECTING TO GITHUB..."
                                                 : "NEEDS AN INTERNET CONNECTION";
        } else if (s_page == PAGE_HUD) {
            for (int i = 0; i < HU_COUNT; i++, ry += rh)
                mrow(x, ROW_Y(i, HU_COUNT), pw, rh, k, s_sel[PAGE_HUD] == i, HUD_LABELS[i],
                     i == HU_BACK ? NULL : g_set.hud_hide >> i & 1 ? "OFF" : "ON", false);
            hint = s_sel[PAGE_HUD] == HU_BACK    ? "B: BACK"
                   : s_sel[PAGE_HUD] == HU_TIME  ? "HIDDEN WHILE YOU RACE, SHOWN AGAIN AT THE FINISH"
                                                 : "ON OR OFF";
        } else if (s_page == PAGE_DEBUG) {
            static const char *labels[D_COUNT] = {"SHOW FPS", "LOGGING", "PROFILER", "CLOCKS", "BACK"};
            const char *values[D_COUNT] = {FPS_NAMES[g_set.show_fps % 3], LOG_NAMES[(unsigned)g_set.logging % LOG_MODES], g_set.profiler ? "ON" : "OFF",
                                           g_set.game_clocks ? "GAME" : "SYSTEM", NULL};
            for (int i = 0; i < D_COUNT; i++, ry += rh) mrow(x, ROW_Y(i, D_COUNT), pw, rh, k, s_sel[PAGE_DEBUG] == i, labels[i], values[i], false);
            hint = s_sel[PAGE_DEBUG] == D_FPS      ? "TIMES: MS PER FRAME OF GAME, GRAPHICS AND RENDER THREADS"
                   : s_sel[PAGE_DEBUG] == D_LOG    ? "DEBUG LOGS ON THE SD CARD, FOR BUG REPORTS"
                   : s_sel[PAGE_DEBUG] == D_PROF   ? "PERFORMANCE PROFILE FOR BUG REPORTS, FROM THE NEXT START"
                   : s_sel[PAGE_DEBUG] == D_CLOCKS ? (g_set.game_clocks ? "HANDHELD GPU 460 MHZ, FASTER LOADING. FROM THE NEXT START"
                                                                        : "LEFT TO THE SYSTEM OR AN OVERCLOCK TOOL. FROM THE NEXT START")
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
                mrow(x, ROW_Y(i, K_COUNT), pw, rh, k, s_sel[PAGE_COLOR] == i, labels[i], vp, false);
            }
            hint = "LEFT/RIGHT: CHANGE (SAVED)   HOLD Y: PREVIEW   B: BACK";
        } else if (s_page == PAGE_MAP) {
            float mh = rh * (float)rows / (float)P_COUNT;
            for (int i = 0; i < P_COUNT; i++, ry += mh) {
                if (i < N_SW) {
                    int a = button_action(i);
                    mrow(x, ry, pw, mh, k, s_sel[PAGE_MAP] == i, SW_BUTTONS[i].name, a < N_ACT ? ACTIONS[a].name : "NONE", false);
                } else {
                    mrow(x, ry, pw, mh, k, s_sel[PAGE_MAP] == i, i == P_RESET ? "RESET TO DEFAULTS" : "BACK", NULL, false);
                }
            }
            int sel = s_sel[PAGE_MAP], a = sel < N_SW ? button_action(sel) : N_ACT;
            hint = s_notice[0]     ? s_notice
                   : sel == P_BACK ? "B: BACK"
                   : sel == P_RESET ? "A: THE STANDARD LAYOUT"
                   : a < N_ACT     ? ACTIONS[a].hint
                                   : "NOTHING.   LEFT/RIGHT: CHOOSE AN ACTION";
        } else {
            const char *l1 = "UNSAVED RACE PROGRESS IS LOST";
            float ts = 34 * k;
            ry = y + top + rh * (float)(rows - 2) / 2; /* the two lines in the middle of the box */
            mtext(x + (pw - mtext_w(l1, ts)) / 2, ry + (rh - ts) / 2, ts, C_VAL_T, C_VAL_B, l1);
            ry += rh;
            const char *l2 = "A: EXIT   B: BACK";
            mtext(x + (pw - mtext_w(l2, ts)) / 2, ry + (rh - ts) / 2, ts, C_TITLE_T, C_TITLE_B, l2);
            ry += rh;
        }
#undef ROW_Y
        if (hint) {
            float px = 2.4f * k;
            text(x + (pw - text_w(hint, px)) / 2, y + ph - foot + 14 * k, px, 0xFFFFFFFFu, hint);
        }
    }
    end();
}

/* Character select with character packs: in the title banner's corners, "PAGE 1/3" on the left
 * and "(Y) / (X) MORE CHARACTERS" on the right (pause_art_char_label), white, at the screen's
 * edges; the banner's middle at BANNER_MID (the game raises it there, native.c). */
#define BANNER_MID 40.0f
void menu_char_hint(int w, int h) {
    int pages = charpack_pages();
    if (pages < 2 || !init_gl()) return;
    begin(w, h);
    /* in 4:3 inside the picture (its side bars beside it) */
    float k = (float)h / 720.0f, s = (float)h / 960.0f * 0.8f, bar = g_set.wide ? 0 : ((float)w - (float)h * 4.0f / 3.0f) / 2;
    float edge = 14 * k + (bar > 0 ? bar : 0);
    const float par[4] = {0, 0, 1, 1};
    for (int side = 0; side < 2; side++) {
        int tw, th;
        float used;
        unsigned t = pause_art_char_label(side, charpack_page() + 1, pages, &tw, &th, &used);
        if (!t) continue;
        float iw = (float)tw * s, ih = (float)th * s, x = side ? (float)w - edge - used * s : edge;
        flush();
        glBindTexture(GL_TEXTURE_2D, t);
        quad(x, BANNER_MID * k - ih / 2, iw, ih, 0xFFFFFFFFu, 0xFFFFFFFFu, SH_IMAGE, 0, 0, par);
        flush();
    }
    glBindTexture(GL_TEXTURE_2D, s_font ? uifont_texture() : 0);
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

#define TITLE_A "MARIO KART"
#define TITLE_B "DOUBLE DASH!! "
#define TITLE_C "NX"

/* The loading screen's background and title; false if GL isn't up. */
static bool loading_back(int w, int h) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!init_gl()) return false;
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
    /* "MARIO KART" yellow to orange on top, "DOUBLE DASH!!" white and "NX" red below it */
    float tsz = 84 * k, tmax = fw - 8 * cw - 40 * k;
    float tw1 = mtext_w(TITLE_A, tsz), tw2 = mtext_w(TITLE_B TITLE_C, tsz), twm = tw1 > tw2 ? tw1 : tw2;
    if (twm > tmax) { tsz *= tmax / twm; tw1 = mtext_w(TITLE_A, tsz); tw2 = mtext_w(TITLE_B TITLE_C, tsz); }
    float ty0 = 215 * k - tsz, tx = (fw - tw2) / 2;
    mtext((fw - tw1) / 2, ty0, tsz, C_TITLE_T, C_TITLE_B, TITLE_A);
    tx += mtext(tx, ty0 + tsz, tsz, 0xFFFFFFFFu, 0xD8DCF0FFu, TITLE_B);
    mtext(tx, ty0 + tsz, tsz, 0xFF5A4AFFu, 0xC81414FFu, TITLE_C);
    return true;
}

/* A choice on the loading screen (the Plus save copy): a title line and the options, sel
 * highlighted, in the message box grown to fit them. */
void menu_choice(int w, int h, const char *title, const char *const *opts, int n, int sel, const char *hint) {
    if (!loading_back(w, h)) return;
    float k = (float)h / 720.0f, fw = (float)w;
    /* about 30% taller than a plain list: more room around the title and between the rows */
    float rh = 74 * k, bw = 860 * k, bh = (100 + (float)n * 74 + (hint ? 60 : 20)) * k, by = 340 * k;
    /* wide enough for the longest option: label inset 50, highlight end 72 past it, row margins 2 x 40 */
    for (int i = 0; i < n; i++) {
        float need = mtext_w(opts[i], 34 * k) + 50 * k + 72 * k + 24 * k + 80 * k;
        if (need > bw) bw = need;
    }
    /* and for the title and hint, with 50 each side ("INSTALL RETRO TRACK GRAND PRIX 2.1?") */
    float ts = 40 * k, tw = mtext_w(title, ts), hs = 26 * k, hw = hint ? mtext_w(hint, hs) : 0;
    if (tw + 100 * k > bw) bw = tw + 100 * k;
    if (hw + 100 * k > bw) bw = hw + 100 * k;
    if (bw > fw - 40 * k) bw = fw - 40 * k;
    if (tw > bw - 100 * k) { ts *= (bw - 100 * k) / tw; tw = mtext_w(title, ts); } /* still too long: smaller letters */
    if (hw > bw - 100 * k) { hs *= (bw - 100 * k) / hw; hw = mtext_w(hint, hs); }
    float bx = (fw - bw) / 2;
    round_box(bx - 8 * k, by - 8 * k, bw + 16 * k, bh + 16 * k, 22 * k, 0xD61C1CFFu);
    round_box(bx - 3 * k, by - 3 * k, bw + 6 * k, bh + 6 * k, 18 * k, 0xFFFFFFFFu);
    round_box(bx, by, bw, bh, 16 * k, 0x102268F8u);
    mtext((fw - tw) / 2, by + 28 * k + (40 * k - ts) / 2, ts, C_TITLE_T, C_TITLE_B, title);
    for (int i = 0; i < n; i++)  /* white options */
        mrow_c(bx + 40 * k, by + 96 * k + (float)i * rh + 5 * k, bw - 80 * k, rh - 10 * k, k, i == sel, opts[i], 0xFFFFFFFFu, 0xFFFFFFFFu);
    if (hint) mtext((fw - hw) / 2, by + bh - 46 * k + (26 * k - hs) / 2, hs, C_DIM, C_DIM, hint);
    end();
}

void menu_loading(int w, int h, const char *line1, const char *line2, float progress) {
    if (!loading_back(w, h)) return;
    float k = (float)h / 720.0f, fw = (float)w;

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
