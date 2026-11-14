/* The - menu (graphics, colour, controls, button mapping, game, mods, debugging, update, exit) and the on-screen
 * overlay, styled after the game's own menus: its MarioFont letters (read from the disc, see
 * ui_font.c), a navy star panel with a red border and a checkered title band. Drawn after the
 * game image with one small instanced program (see "drawing"); the start-up screen
 * (menu_loading) uses the same pieces. */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#ifdef _WIN32
#include "../win/nx_compat.h" /* the Windows build (win/) */
#include "../win/wincfg.h"
#define SW_PC(sw, pc) (pc) /* the Switch's text, the PC's */
#else
#include <switch.h>
#define SW_PC(sw, pc) (sw)
#endif

#include "menu.h"
#include "gfx.h"
#include "render.h"
#include "hdtex.h"
#include "ui_font.h"
#include "route.h"
#include "pause_art.h"
#include "update.h"
#include "../runtime/rt.h"
#include "../runtime/coins.h"
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
/* BUTTON MAPPING's default (driving): as g_map_default, but L is rear view (GameCube Y, with Y),
 * only R switches (GameCube Z), ZL is JUMP (the tricks' button; map_version 3) and GameCube L has
 * no button (ZR / GameCube R drifts; a player can give ZL back to DRIFT / SLIDE LEFT). */
const uint64_t MAP_DRIVE_DEFAULT[MAP_COUNT] = {
    [GC_A] = HidNpadButton_A, [GC_B] = HidNpadButton_B, [GC_X] = HidNpadButton_X, [GC_Y] = HidNpadButton_Y | HidNpadButton_L,
    [GC_Z] = HidNpadButton_R, [GC_R] = HidNpadButton_ZR,
    [GC_START] = HidNpadButton_Plus, [MAP_KFP] = HidNpadButton_StickR, [MAP_JUMP] = HidNpadButton_ZL,
    [MAP_CAM_RESET] = HidNpadButton_StickL,
};
static const char *GC_KEYS[MAP_COUNT] = {"map_a", "map_b", "map_x", "map_y", "map_z", "map_l", "map_r", "map_start", "map_fp", "map_jump", "map_cam_reset", "map_kfp"};
static const char *JOY_KEYS[MAP_COUNT] = {"joycon_a", "joycon_b", "joycon_x", "joycon_y", "joycon_z", "joycon_l", "joycon_r", "joycon_start",
                                          "joycon_fp", "joycon_jump", "joycon_cam_reset", "joycon_kfp"};
/* JOY-CON SIDEWAYS's default (driving): main_switch.c's fixed layout, SL GameCube L and SR GameCube R
 * (both drift; in co-op the rider slides left / right with them) */
static const uint64_t JOY_DRIVE_DEFAULT[MAP_COUNT] = {
    [GC_A] = HidNpadButton_A, [GC_B] = HidNpadButton_B, [GC_X] = HidNpadButton_X, [GC_Y] = HidNpadButton_Y,
    [GC_Z] = HidNpadButton_L | HidNpadButton_StickL, [GC_L] = HidNpadButton_LeftSL, [GC_R] = HidNpadButton_LeftSR,
    [GC_START] = HidNpadButton_Plus | HidNpadButton_ZL,
};

/* Switch buttons the BUTTON MAPPING page assigns (− is reserved for the menu; the sticks and the
 * d-pad always steer). */
typedef struct { uint64_t bit; const char *name; } SwButton;
static const SwButton FULL_BUTTONS[] = {
    {HidNpadButton_A, SW_PC("A", "A / CROSS")}, {HidNpadButton_B, SW_PC("B", "B / CIRCLE")}, {HidNpadButton_X, SW_PC("X", "X / SQUARE")},
    {HidNpadButton_Y, SW_PC("Y", "Y / TRIANGLE")}, {HidNpadButton_L, SW_PC("L", "L / LB / L1")}, {HidNpadButton_R, SW_PC("R", "R / RB / R1")},
    {HidNpadButton_ZL, SW_PC("ZL", "ZL / LT / L2")}, {HidNpadButton_ZR, SW_PC("ZR", "ZR / RT / R2")}, {HidNpadButton_Plus, SW_PC("+", "+ / START")},
    {HidNpadButton_StickL, "LEFT STICK PRESS"}, {HidNpadButton_StickR, "RIGHT STICK PRESS"},
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
/* whose CAMERA and BUTTON MAPPING the menu shows and edits: 0 player 1 (g_set's own fields), 1-3
 * players 2-4 (g_set.pl[s_mpl - 1]), the one who opened it */
static int s_mpl;
#define ED(f) (*(s_mpl ? &g_set.pl[s_mpl - 1].f : &g_set.f))
#define SW_BUTTONS (s_mset ? JOY_BUTTONS : FULL_BUTTONS)
#define N_SW (s_mset ? (int)(sizeof JOY_BUTTONS / sizeof JOY_BUTTONS[0]) : (int)(sizeof FULL_BUTTONS / sizeof FULL_BUTTONS[0]))
#define CUR_MAP (s_mset ? ED(jmap) : ED(map))
#define CUR_DEFAULT (s_mset ? JOY_DRIVE_DEFAULT : MAP_DRIVE_DEFAULT)
static uint64_t mappable(void) {
    uint64_t m = 0;
    for (int i = 0; i < N_SW; i++) m |= SW_BUTTONS[i].bit;
    return m;
}
/* What each Switch button does while driving: an action is the GameCube button it presses
 * (g_set.map[gc] holds the Switch buttons pressing it), or one of the port's own (MAP_FP driver
 * high first person, MAP_KFP kart first person, JUMP, CAMERA RESET). Menus, the pause screen and everything outside driving always use
 * g_map_default (main_switch.c map_full). GameCube L and R both drift; in co-op (two in one kart)
 * the rider slides left with L and right with R, so each needs a button.
 * Rear view is GameCube Y: in races that player's camera turns around instead (main_switch.c). */
static const struct { int gc; const char *name, *hint; } ACTIONS[] = {
    {GC_A, "ACCELERATE", "ACCELERATE"},
    {GC_B, "BRAKE / REVERSE", "BRAKE AND REVERSE"},
    {GC_R, "DRIFT / SLIDE RIGHT", "HOP AND DRIFT. CO-OP RIDER: SLIDE RIGHT"},
    {GC_L, "DRIFT / SLIDE LEFT", "HOP AND DRIFT. CO-OP RIDER: SLIDE LEFT"},
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
#define KART_UP 0 /* added to the kart's own eye */
#define KART_FWD 0
#define KART_TILT 0
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
    g_set.draft = 0;
    g_set.draft_cpu = 2;
    g_set.draft_power = 1;
    g_set.draft_range = 1;
    g_set.battle_cpus = true;
    g_set.battle_karts = 4;
    g_set.bomb_hits = 4;
    g_set.vs_karts = 0;
    g_set.jump = 2; /* MID, on ZL (g_map_default) */
    g_set.tricks = 0;
    g_set.laps = 0;
    g_set.free_cam = 1; /* SNAP BACK */
    g_set.cam_height = 2; /* REGULAR */
    g_set.cam_dist = 2;   /* REGULAR */
    for (int i = 0; i < 20; i++) g_set.special[i] = i / 2; /* each pair's own special; Petey and King Boo any */
    g_set.logging = 0;
    g_set.hd_textures = false;
    g_set.characters = true;
    g_set.audio = true;
    g_set.karts = true;
    snprintf(g_set.mod, sizeof g_set.mod, "auto");
    g_set.language = 0;
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
    memset(g_set.kart_add, 0, sizeof g_set.kart_add);
    g_set.docked_res = DOCKED_AUTO;
    g_set.handheld_res = HANDHELD_AUTO;
    g_set.profiler = false;
    g_set.gpu_test = false;
    g_set.menu_hold = 0;
    g_set.game_clocks = false;
    g_set.lan = false;
    g_set.lan_peer[0] = 0;
    memcpy(g_set.map, MAP_DRIVE_DEFAULT, sizeof g_set.map);
    memcpy(g_set.jmap, JOY_DRIVE_DEFAULT, sizeof g_set.jmap);
}

#define LOG_MODES 5
static const uint32_t LOG_MASKS[LOG_MODES] = {0, LOG_GAME, LOG_GAME | LOG_AUDIO, LOG_AUDIO, LOG_GAME | LOG_BATTLE};
static const char *LOG_NAMES[LOG_MODES] = {"DISABLED", "GAME", "GAME + AUDIO", "AUDIO", "GAME + BATTLE AI"};
static const char *DOCKED_NAMES[6] = {"NATIVE", "2X", "1.5X", "AUTO", "3X", "4X"};
static const char *HANDHELD_NAMES[6] = {"NATIVE", "1.25X", "1.5X", "AUTO", "2X", "3X (OC)"};
static void apply_logging(void) {
    g_log_mask = LOG_MASKS[(unsigned)g_set.logging % LOG_MODES];
    diag_flush();
}


/* what a report needs to know (from loading and each save; an unchanged line is dropped by rt_log) */
static void settings_log(void) {
    rt_log("settings: 16:9 %d, fov %d / %d, fps %d, logging %d, rumble %d, unlock all %d, res docked %d handheld %d, aniso %d, hd %d, "
           "mod %s, characters %d, stereo music %d, extra karts %d, clocks %d, camera %d, any kart %d, laps %d, multi-thread cpu %d, single battle %d, battle karts %d, bomb hits %d, vs karts %d, split %d, lan %d",
           g_set.wide, g_set.fov[0], g_set.fov[1], g_set.show_fps, g_set.logging, g_set.rumble, g_set.unlock_all, g_set.docked_res, g_set.handheld_res,
           g_set.aniso, g_set.hd_textures, g_set.mod, g_set.characters, g_set.audio, g_set.karts, g_set.game_clocks, g_set.camera, g_set.any_kart, g_set.laps, g_set.mt_cpu,
           g_set.battle_cpus, g_set.battle_karts, g_set.bomb_hits, g_set.vs_karts, g_set.vsplit, g_set.lan);
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
    int map_version = 0; /* 2: GameCube L has driving buttons (ZL, a Joy-Con's SL) */
    /* players 2-4 (p2_* .. p4_*): what a player's file names; the rest from player 1's after reading */
    enum { PS_VIEW = 1, PS_HEIGHT = 2, PS_DIST = 4, PS_FREE = 8, PS_MAP = 16, PS_JMAP = 32 };
    unsigned pseen[3] = {0, 0, 0};
    for (int i = 0; i < 3; i++) memset(&g_set.pl[i], 0, sizeof g_set.pl[i]);
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *v = eq + 1;
        v[strcspn(v, "\r\n")] = 0; /* text values (mod) must not keep the line end */
        const char *k = line;
        if (k[0] == 'p' && k[1] >= '2' && k[1] <= '4' && k[2] == '_') {
            int i = k[1] - '2', n = atoi(v);
            PlayerSet *ps = &g_set.pl[i];
            const char *pk = k + 3;
            if (!strcmp(pk, "camera_view")) ps->camera = n < 0 || n > 2 ? 0 : n, pseen[i] |= PS_VIEW;
            else if (!strcmp(pk, "camera_height")) ps->cam_height = n < 0 || n > 3 ? 2 : n, pseen[i] |= PS_HEIGHT;
            else if (!strcmp(pk, "camera_distance")) ps->cam_dist = n < 0 || n > 4 ? 2 : n, pseen[i] |= PS_DIST;
            else if (!strcmp(pk, "free_camera")) ps->free_cam = n < 0 || n > 3 ? 1 : n, pseen[i] |= PS_FREE;
            else
                for (int j = 0; j < MAP_COUNT; j++)
                    if (!strcmp(pk, GC_KEYS[j])) ps->map[j] = strtoull(v, NULL, 16), pseen[i] |= PS_MAP;
                    else if (!strcmp(pk, JOY_KEYS[j])) ps->jmap[j] = strtoull(v, NULL, 16), pseen[i] |= PS_JMAP;
            continue;
        }
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
        else if (!strcmp(k, "drafting")) g_set.draft = atoi(v) > 0; /* earlier builds' 1 light, 2 strong: on */
        else if (!strcmp(k, "cpu_draft")) g_set.draft_cpu = atoi(v) < 0 || atoi(v) > 2 ? 2 : atoi(v);
        else if (!strcmp(k, "draft_strength")) g_set.draft_power = atoi(v) < 0 || atoi(v) > 2 ? 1 : atoi(v);
        else if (!strcmp(k, "draft_distance")) g_set.draft_range = atoi(v) < 0 || atoi(v) > 2 ? 1 : atoi(v);
        else if (!strcmp(k, "coins")) g_set.coins = atoi(v) != 0;
        else if (!strcmp(k, "single_battle")) g_set.battle_cpus = atoi(v) != 0;
        else if (!strcmp(k, "battle_karts")) g_set.battle_karts = atoi(v) < 4 ? 4 : atoi(v) > 8 ? 8 : atoi(v);
        else if (!strcmp(k, "vs_karts")) g_set.vs_karts = atoi(v) < 4 ? 0 : atoi(v) > 8 ? 8 : atoi(v);
        else if (!strcmp(k, "bomb_hits")) g_set.bomb_hits = atoi(v) < 4 ? 4 : atoi(v) > 20 ? 20 : atoi(v) & ~1;
        else if (!strcmp(k, "mt_length")) g_set.mt_length = atoi(v) < 0 ? 0 : atoi(v) >= MT_N_LENGTHS ? MT_N_LENGTHS - 1 : atoi(v);
        else if (!strcmp(k, "tricks")) g_set.tricks = atoi(v) < 0 || atoi(v) > 2 ? 0 : atoi(v);
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
        else if (!strcmp(k, "audio")) g_set.audio = atoi(v) != 0;
        else if (!strcmp(k, "karts")) g_set.karts = atoi(v) != 0;
        else if (!strcmp(k, "mod")) snprintf(g_set.mod, sizeof g_set.mod, "%s", v);
        else if (!strcmp(k, "language")) {
            g_set.language = 0;
            for (int i = 1; i < LANG_COUNT; i++)
                if (!strcasecmp(v, LANG_NAMES[i])) g_set.language = i;
        }
        else if (!strcmp(k, "plus")) snprintf(g_set.mod, sizeof g_set.mod, atoi(v) ? "auto" : "off"); /* before 1.4.0 */
        else if (!strcmp(k, "fov_4_3")) g_set.fov[0] = atoi(v) < 0 ? 0 : atoi(v) > 4 ? 4 : atoi(v);
        else if (!strcmp(k, "fov_16_9")) g_set.fov[1] = atoi(v) < 0 ? 0 : atoi(v) > 4 ? 4 : atoi(v);
        else if (!strcmp(k, "aniso")) g_set.aniso = atoi(v) < 0 ? 0 : atoi(v) > 4 ? 4 : atoi(v);
        else if (!strcmp(k, "border")) g_set.border = atoi(v) != 0;
        else if (!strcmp(k, "hud_hide")) g_set.hud_hide = (unsigned)atoi(v) & (HUD_ALL | HUD_MIRROR | HUD_COINS);
        else if (!strcmp(k, "hud_style")) g_set.hud_style = atoi(v) == 3 ? 2 : atoi(v) >= 0 && atoi(v) <= 2 ? atoi(v) : 1; /* 3: PILLS + MIRROR before, the mirror now an asset */
        else if (!strcmp(k, "speed_kmh")) g_set.speed_kmh = atoi(v) != 0;
        else if (!strcmp(k, "split_side_by_side")) g_set.vsplit = atoi(v) != 0;
        else if (!strcmp(k, "camera_view")) g_set.camera = atoi(v) < 0 || atoi(v) > 2 ? 0 : atoi(v), view_seen = true;
        else if (!strcmp(k, "camera")) old_view = atoi(v);
        else if (!strcmp(k, "driver_eye_up")) g_set.drv_up = atoi(v);
        else if (!strcmp(k, "driver_eye_forward")) g_set.drv_forward = atoi(v);
        else if (!strcmp(k, "driver_tilt")) g_set.drv_tilt = atoi(v);
        else if (!strcmp(k, "kart_add_up")) g_set.kart_up = atoi(v);
        else if (!strcmp(k, "kart_add_forward")) g_set.kart_forward = atoi(v);
        else if (!strcmp(k, "kart_add_tilt")) g_set.kart_tilt = atoi(v);
        else if (!strncmp(k, "kart_add_", 9) && k[9] >= '0' && k[9] <= '9') { /* one kart: up,forward,tilt */
            int id = atoi(k + 9), u, f, t;
            if (id < 21 && sscanf(v, "%d,%d,%d", &u, &f, &t) == 3) {
                g_set.kart_add[id][0] = u < -100 ? -100 : u > 200 ? 200 : u;
                g_set.kart_add[id][1] = f < -150 ? -150 : f > 150 ? 150 : f;
                g_set.kart_add[id][2] = t < -45 ? -45 : t > 45 ? 45 : t;
            }
        }
        else if (!strcmp(k, "profiler")) g_set.profiler = atoi(v) != 0;
        else if (!strcmp(k, "gpu_test")) g_set.gpu_test = atoi(v) != 0;
        else if (!strcmp(k, "menu_hold")) g_set.menu_hold = atoi(v) == 3 || atoi(v) == 5 ? atoi(v) : 0;
        else if (!strcmp(k, "game_clocks")) g_set.game_clocks = atoi(v) != 0;
        else if (!strcmp(k, "map_version")) map_version = atoi(v);
        else if (!strcmp(k, "lan")) g_set.lan = atoi(v) != 0;
        else if (!strcmp(k, "lan_peer")) snprintf(g_set.lan_peer, sizeof g_set.lan_peer, "%s", v);
        else if (!strcmp(k, "handheld_res")) g_set.handheld_res = atoi(v) >= 0 && atoi(v) <= HANDHELD_3X ? atoi(v) : HANDHELD_AUTO;
        else if (!strcmp(k, "docked_res")) g_set.docked_res = atoi(v) >= 0 && atoi(v) <= DOCKED_4X ? atoi(v) : DOCKED_AUTO;
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
    /* settings from before GameCube L had a driving button: the co-op rider could not slide left.
     * ZL goes from JUMP (its old default; GAME > JUMP HEIGHT then asks for a button) to L, a Joy-Con's
     * SL from DRIFT to L */
    if (map_version < 2) {
        if (!g_set.map[GC_L] && g_set.map[MAP_JUMP] == HidNpadButton_ZL)
            g_set.map[GC_L] = HidNpadButton_ZL, g_set.map[MAP_JUMP] = 0, rt_log("settings: ZL moved from JUMP to DRIFT / SLIDE LEFT");
        if (!g_set.jmap[GC_L] && (g_set.jmap[GC_R] & HidNpadButton_LeftSL) && (g_set.jmap[GC_R] & ~HidNpadButton_LeftSL))
            g_set.jmap[GC_L] = HidNpadButton_LeftSL, g_set.jmap[GC_R] &= ~HidNpadButton_LeftSL;
    }
    /* settings from before 3: ZL back to JUMP (the tricks), when it is still the default L and JUMP has none */
    if (map_version < 3 && g_set.map[GC_L] == HidNpadButton_ZL && !g_set.map[MAP_JUMP])
        g_set.map[MAP_JUMP] = HidNpadButton_ZL, g_set.map[GC_L] = 0, rt_log("settings: ZL moved from DRIFT / SLIDE LEFT to JUMP");
    if (!kfp_seen && g_set.map[MAP_FP]) g_set.map[MAP_KFP] = g_set.map[MAP_FP], g_set.map[MAP_FP] = 0;
    /* a player with keys of their own has their own settings; what their keys leave out is player 1's */
    for (int i = 0; i < 3; i++) {
        PlayerSet *ps = &g_set.pl[i];
        if (!pseen[i]) continue;
        ps->own = true;
        if (!(pseen[i] & PS_VIEW)) ps->camera = g_set.camera;
        if (!(pseen[i] & PS_HEIGHT)) ps->cam_height = g_set.cam_height;
        if (!(pseen[i] & PS_DIST)) ps->cam_dist = g_set.cam_dist;
        if (!(pseen[i] & PS_FREE)) ps->free_cam = g_set.free_cam;
        if (!(pseen[i] & PS_MAP)) memcpy(ps->map, g_set.map, sizeof ps->map);
        if (!(pseen[i] & PS_JMAP)) memcpy(ps->jmap, g_set.jmap, sizeof ps->jmap);
    }
    /* one action per Switch button (older builds could give one button several), the first in
     * ACTIONS order kept; a required action left without one gets its default buttons back */
    for (s_mpl = 0; s_mpl < 4; s_mpl++) {
        if (s_mpl && !g_set.pl[s_mpl - 1].own) continue;
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
                rt_log("settings: player %d %s mapping missing %d required action(s) (%s), their default buttons back", s_mpl + 1,
                       s_mset ? "Joy-Con" : "button", n, name);
            }
        }
    }
    s_mset = 0, s_mpl = 0;
    apply_logging();
    if (map_version < 3) settings_save(); /* the moved buttons, once (settings_save logs them) */
    else settings_log();
}

void settings_save(void) {
    settings_log();
    FILE *f = fopen(s_path, "w");
    if (!f) { rt_log("settings: could not write %s", s_path); return; }
    fprintf(f, "display=%d\nshow_fps=%d\nlogging=%d\nrumble=%d\nunlock_all=%d\nhd_textures=%d\nfov_4_3=%d\nfov_16_9=%d\naniso=%d\nborder=%d\n"
            "docked_res=%d\nprofiler=%d\nmod=%s\nhandheld_res=%d\nhud_hide=%u\nhud_style=%d\nspeed_kmh=%d\ncamera_view=%d\ndriver_eye_up=%d\ndriver_eye_forward=%d\ndriver_tilt=%d\nkart_add_up=%d\nkart_add_forward=%d\nkart_add_tilt=%d\nany_kart=%d\n", g_set.wide, g_set.show_fps,
            g_set.logging, g_set.rumble, g_set.unlock_all, g_set.hd_textures, g_set.fov[0], g_set.fov[1], g_set.aniso, g_set.border, g_set.docked_res,
            g_set.profiler, g_set.mod, g_set.handheld_res, g_set.hud_hide, g_set.hud_style, g_set.speed_kmh, g_set.camera, g_set.drv_up, g_set.drv_forward, g_set.drv_tilt, g_set.kart_up, g_set.kart_forward, g_set.kart_tilt, g_set.any_kart);
    fprintf(f, "gpu_test=%d\n", g_set.gpu_test);
    fprintf(f, "language=%s\n", LANG_NAMES[g_set.language > 0 && g_set.language < LANG_COUNT ? g_set.language : 0]);
    fprintf(f, "menu_hold=%d\n", g_set.menu_hold);
    fprintf(f, "map_version=3\n");
    fprintf(f, "drafting=%d\ncpu_draft=%d\ndraft_strength=%d\ndraft_distance=%d\n", g_set.draft, g_set.draft_cpu,
            g_set.draft_power, g_set.draft_range);
    fprintf(f, "coins=%d\n", g_set.coins);
    fprintf(f, "vs_karts=%d\n", g_set.vs_karts);
    fprintf(f, "mt_cpu=%d\nsingle_battle=%d\nbattle_karts=%d\nbomb_hits=%d\nperfect_start=%d\ntricks=%d\n", g_set.mt_cpu, g_set.battle_cpus, g_set.battle_karts, g_set.bomb_hits, g_set.perfect_start, g_set.tricks);
    fprintf(f, "mt_sweeps=%d\nmt_speed=%d\nmt_length=%d\njump=%d\nlaps=%d\nfree_camera=%d\ncamera_height=%d\ncamera_distance=%d\n", g_set.mt_sweeps, g_set.mt_speed,
            g_set.mt_length, g_set.jump, g_set.laps, g_set.free_cam, g_set.cam_height, g_set.cam_dist);
    for (int i = 0; i < 21; i++)
        if (g_set.kart_add[i][0] || g_set.kart_add[i][1] || g_set.kart_add[i][2])
            fprintf(f, "kart_add_%d=%d,%d,%d\n", i, g_set.kart_add[i][0], g_set.kart_add[i][1], g_set.kart_add[i][2]);
    fprintf(f, "characters=%d\n", g_set.characters);
    fprintf(f, "audio=%d\n", g_set.audio);
    fprintf(f, "karts=%d\n", g_set.karts);
    fprintf(f, "game_clocks=%d\n", g_set.game_clocks);
    fprintf(f, "split_side_by_side=%d\n", g_set.vsplit);
    fprintf(f, "lan=%d\nlan_peer=%s\n", g_set.lan, g_set.lan_peer);
    fprintf(f, "special_items=%d\nspecial=", g_set.special_mode);
    for (int i = 0; i < 20; i++) fputc('0' + g_set.special[i] % 10, f);
    fputc('\n', f);
    for (int i = 0; i < MAP_COUNT; i++) fprintf(f, "%s=%llx\n", GC_KEYS[i], (unsigned long long)g_set.map[i]);
    for (int i = 0; i < MAP_COUNT; i++) fprintf(f, "%s=%llx\n", JOY_KEYS[i], (unsigned long long)g_set.jmap[i]);
    for (int pl = 0; pl < 3; pl++) {
        const PlayerSet *ps = &g_set.pl[pl];
        if (!ps->own) continue;
        fprintf(f, "p%d_camera_view=%d\np%d_camera_height=%d\np%d_camera_distance=%d\np%d_free_camera=%d\n", pl + 2, ps->camera, pl + 2,
                ps->cam_height, pl + 2, ps->cam_dist, pl + 2, ps->free_cam);
        for (int i = 0; i < MAP_COUNT; i++) fprintf(f, "p%d_%s=%llx\n", pl + 2, GC_KEYS[i], (unsigned long long)ps->map[i]);
        for (int i = 0; i < MAP_COUNT; i++) fprintf(f, "p%d_%s=%llx\n", pl + 2, JOY_KEYS[i], (unsigned long long)ps->jmap[i]);
    }
    fclose(f);
}

void player_set(int p, PlayerSet *out) {
    if (p > 0 && p < 4 && g_set.pl[p - 1].own) { *out = g_set.pl[p - 1]; return; }
    memset(out, 0, sizeof *out);
    out->camera = g_set.camera, out->cam_height = g_set.cam_height, out->cam_dist = g_set.cam_dist, out->free_cam = g_set.free_cam;
    memcpy(out->map, g_set.map, sizeof out->map);
    memcpy(out->jmap, g_set.jmap, sizeof out->jmap);
}
static bool pset_same(const PlayerSet *a, const PlayerSet *b) {
    return a->camera == b->camera && a->cam_height == b->cam_height && a->cam_dist == b->cam_dist && a->free_cam == b->free_cam &&
           !memcmp(a->map, b->map, sizeof a->map) && !memcmp(a->jmap, b->jmap, sizeof a->jmap);
}

void camera_apply(void) {
    static const float HEIGHTS[4] = {0.45f, 0.7f, 1.0f, 1.5f};
    static const float DISTS[5] = {0.55f, 0.75f, 1.0f, 1.3f, 1.6f};
    static int last_view[4] = {-1, -1, -1, -1};
    for (int p = 0; p < 4; p++) {
        PlayerSet ps;
        player_set(p, &ps);
        g_cam_height[p] = HEIGHTS[ps.cam_height & 3];
        g_cam_dist[p] = DISTS[(unsigned)ps.cam_dist % 5];
        g_cam_back[p] = ps.free_cam == 2 ? CAM_EASE : ps.free_cam == 3 ? CAM_STAY : CAM_SNAP;
        /* CAMERA VIEW: NORMAL, DRIVER FPV, KART FPV, applied when it changes (menu, player 1's FPV
         * buttons): players 2-4's FPV buttons switch their own bit between, for the session */
        int cv = (int)((unsigned)ps.camera % 3);
        if (cv == last_view[p]) continue;
        last_view[p] = cv;
        uint32_t bit = 1u << p;
        if (cv) __atomic_or_fetch(&g_first_person, bit, __ATOMIC_RELAXED);
        else __atomic_and_fetch(&g_first_person, ~bit, __ATOMIC_RELAXED);
        if (cv == 2) __atomic_or_fetch(&g_fp_kart, bit, __ATOMIC_RELAXED);
        else __atomic_and_fetch(&g_fp_kart, ~bit, __ATOMIC_RELAXED);
    }
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
       PAGE_MULTI, PAGE_LAN, PAGE_BATTLE, PAGE_DRAFT, PAGE_PLAYER, PAGE_COUNT };
/* the main page's rows; UPDATE and DEBUG sit on EXIT GAME's row, at its right (LEFT/RIGHT there) */
enum { M_GAME, M_GFX, M_CAM, M_CTRL, M_MULTI, M_MODS, M_CHEATS, M_EXIT, M_UPDATE, M_DEBUG, M_COUNT };
enum { MD_MOD, MD_LANG, MD_AUDIO, MD_HD, MD_CHARS, MD_KARTS, MD_RESTART, MD_BACK, MD_COUNT };
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
/* mods/lang/<name>: the languages there at start-up (a PAL or Japanese disc's files: its title screen and
 * memory card texts at least), and the one played */
const char *const LANG_NAMES[LANG_COUNT] = {"English", "French", "German", "Italian", "Spanish", "Japanese"};
static unsigned s_langs_found;
static int s_lang_running;
unsigned langs_find(const char *mods_dir) {
    unsigned found = 0;
    for (int i = 1; i < LANG_COUNT; i++) {
        char a[320], b[320];
        struct stat st;
        snprintf(a, sizeof a, "%s/lang/%s/SceneData/title.arc", mods_dir, LANG_NAMES[i]);
        snprintf(b, sizeof b, "%s/lang/%s/MRAM_Locale/MRAMLoc.arc", mods_dir, LANG_NAMES[i]);
        if (stat(a, &st) == 0 && stat(b, &st) == 0) found |= 1u << i;
    }
    return found;
}
int langs_pick(unsigned found) {
    int l = g_set.language;
    return l > 0 && l < LANG_COUNT && (found >> l & 1) ? l : 0;
}
void menu_set_langs(unsigned found, int running) { s_langs_found = found, s_lang_running = running; }
/* the language the next start plays: English with a level pack */
static bool lang_pending(void) { return (mods_pick(s_mods_found) ? 0 : langs_pick(s_langs_found)) != s_lang_running; }
/* LEFT/RIGHT on LANGUAGE: English, then each one installed */
static void lang_step(int dir) {
    int l = langs_pick(s_langs_found);
    do l = (l + LANG_COUNT + dir) % LANG_COUNT;
    while (l && !(s_langs_found >> l & 1));
    g_set.language = l;
}
/* mods/characters: there at start-up, and loaded then (EXTRA CHARACTERS was on) */
static bool s_chars_found, s_chars_running;
void menu_set_chars(bool found, bool running) { s_chars_found = found, s_chars_running = running; }
static bool chars_pending(void) { return s_chars_found && g_set.characters != s_chars_running; }
/* mods/karts: there at start-up, and loaded then */
static bool s_karts_found, s_karts_running;
void menu_set_karts(bool found, bool running) { s_karts_found = found, s_karts_running = running; }
static bool karts_pending(void) { return s_karts_found && g_set.karts != s_karts_running; }
/* mods/audio: there at start-up, and played then */
static bool s_audio_found, s_audio_running;
void menu_set_audio(bool found, bool running) { s_audio_found = found, s_audio_running = running; }
bool mod_is_plus(unsigned n) { return !strcmp(mod_key(n), "plus"); }
static bool audio_pending(void) { return s_audio_found && mod_is_plus(mods_pick(s_mods_found)) && g_set.audio != s_audio_running; }
/* LAN PLAY and LAN PEER: eth.c reads g_lan_on when the game looks for the adapter at start-up, and
 * g_lan_peer when the tunnel opens, so a change waits for a restart too */
static bool lan_pending(void) { return g_set.lan != (g_lan_on != 0) || (g_set.lan && strcmp(g_set.lan_peer, g_lan_peer)); }
static int lan_rows(void) { return lan_pending() ? LN_COUNT : LN_COUNT - 1; }
static int lan_item(int row) { return !lan_pending() && row >= LN_RESTART ? row + 1 : row; } /* row -> LN_ */
/* MODS' own changes; mod_pending: anything RESTART NOW (on either page) applies */
static bool mods_changed(void) { return mods_pick(s_mods_found) != s_mod_running || lang_pending() || chars_pending() || karts_pending() || audio_pending(); }
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
        snprintf(hint, hn, g_lan_on ? SW_PC("STARTS WITH WI-FI MODE ON THE TITLE SCREEN", "STARTS WITH LAN MODE ON THE TITLE SCREEN") : "LAN PLAY IS OFF");
    } else if (!st.consoles) {
        snprintf(val, vn, "LOOKING...");
        snprintf(hint, hn, st.seconds < 10 ? SW_PC("LOOKING FOR OTHER CONSOLES ON THIS NETWORK", "LOOKING FOR OTHER PCS AND SWITCHES ON THIS NETWORK")
                                           : "NONE FOUND: NOT ON A GUEST NETWORK? ELSE SET LAN PEER");
    } else if (st.silent == st.consoles) {
        snprintf(val, vn, "%u FOUND", st.consoles);
        snprintf(hint, hn, SW_PC("NO TIMES: THE OTHER CONSOLE HAS AN OLDER BUILD", "NO TIMES: THE OTHER ONE HAS AN OLDER BUILD"));
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
enum { C_MAP, C_JOYMAP, C_SYNC, C_HOLD, C_BACK, C_COUNT };
enum { GM_UNITS, GM_JUMP, GM_TRICKS, GM_DRAFT, GM_COINS, GM_RUMBLE, GM_BATTLE, GM_VERSUS, GM_BACK, GM_COUNT };
/* VERSUS CPUS: OFF, then the karts a 2-4 player Versus is filled to (g_set.vs_karts 0, 4-8) */
static const char *VS_KARTS_NAMES[6] = {"OFF", "4 KARTS", "5 KARTS", "6 KARTS", "7 KARTS", "8 KARTS"};
/* DRAFTING's page (A on GAME's row while it is on) */
enum { DR_CPU, DR_POWER, DR_RANGE, DR_BACK, DR_COUNT };
static const char *DRAFT_CPU_NAMES[3] = {"OFF", "WEAKER", "SAME AS YOU"};
static const char *DRAFT_POWER_NAMES[3] = {"WEAK", "MEDIUM", "STRONG"};
static const char *DRAFT_RANGE_NAMES[3] = {"CLOSE", "REGULAR", "FAR"};
/* SINGLE PLAYER BATTLE MODE's page (A on GAME's row while it is on) */
enum { BT_KARTS, BT_HITS, BT_BACK, BT_COUNT };
enum { CA_VIEW, CA_HEIGHT, CA_DIST, CA_FOV, CA_FREE, CA_STYLE, CA_HUD, CA_BACK, CA_COUNT }; /* RACE HUD, HUD ASSETS: everyone's (player 1's page only) */
/* players 2-4's menu (PAGE_PLAYER): their camera and buttons only; the game's settings stay player 1's */
enum { PL_CAM, PL_MAP, PL_JOYMAP, PL_CLOSE, PL_COUNT };
/* the FPV page (A on CAMERA VIEW): its eye (KA_UP, KA_FWD: steps of 5; KA_TILT: degrees down). KART FPV keeps
 * one for each kart: in a race the one being driven, otherwise KA_KART chooses. DRIVER FPV's covers every kart. */
enum { KA_KART, KA_UP, KA_FWD, KA_TILT, KA_RESET, KA_BACK, KA_COUNT };
static const char *KART_NAMES[21] = {"RED FIRE", "DK JUMBO", "TURBO YOSHI", "KOOPA DASHER", "HEART COACH", "GOO-GOO BUGGY", "WARIO CAR",
                                     "KOOPA KING", "GREEN FIRE", "BARREL TRAIN", "TURBO BIRDO", "PARA-WING", "BLOOM COACH", "RATTLE BUGGY",
                                     "WALUIGI RACER", "BULLET BLASTER", "TOAD KART", "TOADETTE KART", "BOO PIPES", "PIRANHA PIPES", "PARADE KART"};
static int s_kart_sel;
static bool s_in_race; /* the race camera was running when the menu opened */
static bool fpv_each_kart(void) { return ED(camera) == 2 && g_kart_eye_on; }
/* the page's three settings: DRIVER FPV's or KART FPV's (CAMERA VIEW 1 / 2), and their defaults */
static int *fpv_eye(int i) {
    bool kart = ED(camera) == 2;
    if (fpv_each_kart()) return &g_set.kart_add[s_kart_sel][i - KA_UP];
    return i == KA_UP ? (kart ? &g_set.kart_up : &g_set.drv_up) : i == KA_FWD ? (kart ? &g_set.kart_forward : &g_set.drv_forward)
                      : (kart ? &g_set.kart_tilt : &g_set.drv_tilt);
}
static int fpv_default(int i) {
    bool kart = ED(camera) == 2;
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
        g_draft = 0;
        g_coins = 0;
        g_battle_cpus = g_set.battle_cpus; /* battle.c keeps Wi-Fi races the game's own */
        g_battle_karts = (uint32_t)g_set.battle_karts;
        g_bomb_hits = (uint32_t)g_set.bomb_hits;
        g_vs_karts = 0;
        g_jump_power = 0;
        g_tricks = 0;
        memset((void *)g_special, 0, sizeof g_special);
        return;
    }
    g_mt_sweeps = (uint32_t)g_set.mt_sweeps;
    g_mt_speed = MT_SPEEDS[g_set.mt_speed % MT_N_SPEEDS].v;
    g_mt_length = MT_LENGTHS[g_set.mt_length % MT_N_LENGTHS].v;
    g_mt_cpu = g_set.mt_cpu;
    g_perfect_start = g_set.perfect_start;
    g_draft = g_set.draft ? (uint32_t)(g_set.draft_power % 3) + 1 : 0;
    g_draft_cpu = (uint32_t)(g_set.draft_cpu % 3);
    g_draft_range = (uint32_t)(g_set.draft_range % 3);
    g_coins = (uint32_t)g_set.coins;
    g_battle_cpus = g_set.battle_cpus;
    g_battle_karts = (uint32_t)g_set.battle_karts;
    g_bomb_hits = (uint32_t)g_set.bomb_hits;
    g_vs_karts = (uint32_t)g_set.vs_karts;
    g_jump_power = JUMP_POWER[g_set.jump & 3];
    g_tricks = (uint32_t)g_set.tricks;
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
enum { G_WIDE, G_FULL, G_BORDER, G_DOCKED, G_HANDHELD, G_ANISO, G_COLOR, G_BACK, G_COUNT }; /* G_FULL: PC only */
/* HUD ASSETS page: one row per element (HUD_* bit 1 << row); the mirror single player only, the coins 1 to 4 players */
enum { HU_PLACE, HU_ITEMS, HU_LAPS, HU_MAP, HU_SPEED, HU_ORDER, HU_TIME, HU_MIRROR, HU_COINS, HU_BACK, HU_COUNT };
static const char *HUD_LABELS[HU_COUNT] = {"POSITION", "ITEMS", "LAPS", "MAP", "SPEEDOMETER", "RACE ORDER", "RACE TIMER", "REAR-VIEW MIRROR", "COINS", "BACK"};
static const char *HUD_STYLES[3] = {"CLASSIC", "MODERN", "DASHBOARD"};
/* the mirror's row only under DASHBOARD (its setting kept under the others); a shown row's HU_ */
static uint32_t hud_mask(void) { return HUD_ALL | HUD_COINS | (g_set.hud_style == 2 ? HUD_MIRROR : 0); }
static int hud_rows(void) { return g_set.hud_style == 2 ? HU_COUNT : HU_COUNT - 1; }
static int hud_row(int i) { return g_set.hud_style != 2 && i >= HU_MIRROR ? i + 1 : i; }
static const char *hud_value(void) {
    uint32_t h = g_set.hud_hide & hud_mask();
    return !h ? "ON" : h == hud_mask() ? "OFF" : "CUSTOM";
}
static const char *FOV_NAMES[5] = {"NORMAL", "WIDE", "WIDER", "WIDEST", "WIDEEE"};
static const char *ANISO_NAMES[5] = {"NORMAL", "2X", "4X", "8X", "16X"};
enum { D_FPS, D_LOG, D_PROF, D_GPUTEST, D_CLOCKS, D_BACK, D_COUNT };
static const char *FPS_NAMES[3] = {"OFF", "FPS", "FPS + TIMES"};
static const char *RUMBLE_NAMES[3] = {"OFF", "LOW", "HIGH"};
static bool s_want_controllers;
static bool s_want_fullscreen; /* FULL SCREEN (PC): main_win.c switches it */
#ifdef _WIN32
#define FULLSCREEN_ON g_win.fullscreen
#else
#define FULLSCREEN_ON false
#endif

/* Rows one build leaves out: the PC has no handheld mode, controller pairing, Joy-Con sideways
 * mapping, clocks, GPU test or updater; the Switch has no FULL SCREEN. On those pages a row
 * number (s_sel) is not the item: row_item turns it into one. */
static bool item_hidden(int page, int item);
static int item_count(int page);
static int shown_rows(int page) {
    int n = 0;
    for (int i = 0; i < item_count(page); i++) n += !item_hidden(page, i);
    return n;
}
static int row_item(int page, int row) {
    int c = item_count(page);
    for (int i = 0, n = 0; i < c; i++)
        if (!item_hidden(page, i) && n++ == row) return i;
    return c - 1; /* past the end: BACK */
}
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

static bool item_hidden(int page, int item) {
    if (page == PAGE_CAM) return (item == CA_FOV || item == CA_STYLE || item == CA_HUD) && s_mpl; /* the field of view and the HUD are the whole picture's: player 1's */
#ifdef _WIN32
    return (page == PAGE_GFX && item == G_HANDHELD) || (page == PAGE_CTRL && (item == C_JOYMAP || item == C_SYNC)) ||
           (page == PAGE_DEBUG && (item == D_GPUTEST || item == D_CLOCKS)) ||
           (page == PAGE_PLAYER && item == PL_JOYMAP);
#else
    return page == PAGE_GFX && item == G_FULL;
#endif
}
static int item_count(int page) {
    return page == PAGE_GFX ? G_COUNT : page == PAGE_CTRL ? C_COUNT : page == PAGE_DEBUG ? D_COUNT : page == PAGE_CAM ? CA_COUNT : page == PAGE_PLAYER ? PL_COUNT : 0;
}

static int page_rows(int page) {
    switch (page) {
    case PAGE_MAIN: return M_EXIT + 1;
    case PAGE_MAP: return P_COUNT;
    case PAGE_COLOR: return K_COUNT;
    case PAGE_DEBUG: return shown_rows(PAGE_DEBUG);
    case PAGE_GFX: return shown_rows(PAGE_GFX);
    case PAGE_GAME: return GM_COUNT;
    case PAGE_BATTLE: return BT_COUNT;
    case PAGE_DRAFT: return DR_COUNT;
    case PAGE_CAM: return shown_rows(PAGE_CAM);
    case PAGE_PLAYER: return shown_rows(PAGE_PLAYER);
    case PAGE_KART: return KA_COUNT;
    case PAGE_CHEATS: return CH_COUNT;
    case PAGE_SPECIAL: return SP_COUNT;
    case PAGE_MODS: return MD_COUNT;
    case PAGE_MULTI: return MP_COUNT;
    case PAGE_LAN: return LN_COUNT; /* the most it shows */
    case PAGE_UPDATE: return U_COUNT;
    case PAGE_CTRL: return shown_rows(PAGE_CTRL);
    case PAGE_HUD: return hud_rows();
    default: return 1;
    }
}

/* B goes back one page: colour to graphics, button mapping to controls, race HUD to game,
 * everything else to the main page. */
static int page_parent(int page) {
    if (s_mpl && (page == PAGE_MAP || page == PAGE_CAM)) return PAGE_PLAYER;
    return page == PAGE_COLOR ? PAGE_GFX : page == PAGE_MAP ? PAGE_CTRL : page == PAGE_HUD ? PAGE_CAM : page == PAGE_BATTLE || page == PAGE_DRAFT ? PAGE_GAME
           : page == PAGE_SPECIAL ? PAGE_CHEATS : page == PAGE_KART ? PAGE_CAM : page == PAGE_LAN ? PAGE_MULTI : PAGE_MAIN;
}

static bool s_close_only; /* opened by menu_open with OPEN MENU on hold: a dropped controller must not hand over the options */
static int s_block_pl; /* whose buttons s_block holds */

/* Player p (0-3) opens the menu: players 2-4 get their own page, starting from player 1's settings
 * when they have none yet. Closed: if they changed nothing, they keep following player 1's. */
static bool s_mpl_was_own;
static void menu_begin(int p) {
    s_mpl = p;
    s_page = p ? PAGE_PLAYER : PAGE_MAIN;
    s_sel[PAGE_PLAYER] = 0;
    if (!p) return;
    PlayerSet *g = &g_set.pl[p - 1];
    s_mpl_was_own = g->own;
    if (!g->own) player_set(0, g), g->own = true;
    rt_log("menu: player %d's options", p + 1);
}
static void menu_end(void) {
    if (s_mpl && !s_mpl_was_own) {
        PlayerSet a;
        player_set(0, &a);
        PlayerSet *g = &g_set.pl[s_mpl - 1];
        if (pset_same(g, &a)) g->own = false, settings_save();
    }
    s_mpl = 0;
}

void menu_open(const char *notice) {
    if (s_open) menu_end();
    menu_begin(0);
    s_open = true;
    s_close_only = g_set.menu_hold != 0;
    snprintf(s_notice, sizeof s_notice, "%s", notice ? notice : "");
}

bool menu_input(const uint64_t down_[4], const uint64_t held_[4], bool *quit) {
    if (!s_open) {
        s_block &= held_[s_block_pl & 3];
        static uint32_t cam_last;
        static int cam_idle = 100;
        uint32_t cam = g_cam_frames[0];
        cam_idle = cam != cam_last ? 0 : cam_idle < 100 ? cam_idle + 1 : cam_idle;
        cam_last = cam;
        s_in_race = cam_idle < 4;
        /* any player's -: their menu (players 2-4: their camera and buttons). OPEN MENU on hold: from a
         * fresh press, so holding on after closing the menu doesn't reopen it */
        static uint64_t hold_from[4];
        for (int p = 0; p < 4 && !s_open; p++) {
            bool minus = down_[p] & HidNpadButton_Minus;
            if (!(held_[p] & HidNpadButton_Minus)) hold_from[p] = 0;
            else if (minus) hold_from[p] = plat_now_ns();
            if (g_set.menu_hold) minus = hold_from[p] && plat_now_ns() - hold_from[p] >= (uint64_t)g_set.menu_hold * 1000000000ull;
            if (minus) { menu_begin(p); s_open = true; s_pressed = HidNpadButton_Minus; s_close_only = false; hold_from[p] = 0; }
        }
        return s_open;
    }
    uint64_t down = down_[s_mpl & 3], held = held_[s_mpl & 3];
    if (s_close_only) down &= HidNpadButton_Minus | HidNpadButton_B;
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
        if (s_page == PAGE_MAIN || s_page == PAGE_PLAYER || (down & HidNpadButton_Minus)) {
            if (s_page == PAGE_MAP) map_leave(true);
            s_open = false; s_block = s_pressed & held; s_block_pl = s_mpl; s_preview = false; s_notice[0] = 0;
            menu_end();
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
        switch (row_item(PAGE_DEBUG, *sel)) {
        case D_FPS: g_set.show_fps = (g_set.show_fps + (left ? 2 : 1)) % 3; settings_save(); break;
        case D_LOG:
            /* A / right: next mode, left: previous */
            g_set.logging = (g_set.logging + (left ? LOG_MODES - 1 : 1)) % LOG_MODES;
            apply_logging();
            settings_save();
            break;
        case D_PROF: g_set.profiler = !g_set.profiler; settings_save(); break;
        case D_GPUTEST: g_set.gpu_test = !g_set.gpu_test; settings_save(); break;
        case D_CLOCKS: g_set.game_clocks = !g_set.game_clocks; settings_save(); break;
        case D_BACK: if (ok) s_page = PAGE_MAIN; break;
        }
        return true;
    }
    if (s_page == PAGE_GFX && (ok || side)) {
        switch (row_item(PAGE_GFX, *sel)) {
        case G_WIDE: g_set.wide = !g_set.wide; settings_save(); break;
        case G_FULL: s_want_fullscreen = true; break;
        case G_COLOR:
            if (ok) { s_page = PAGE_COLOR; s_sel[PAGE_COLOR] = 0; }
            else { g_color.mode = (g_color.mode + (left ? COLOR_MODES - 1 : 1)) % COLOR_MODES; s_color_dirty = true; color_save(); }
            break;
        case G_ANISO: g_set.aniso = (g_set.aniso + (left ? 4 : 1)) % 5; settings_save(); break;
        case G_DOCKED: {
            /* in the menu's order */
            static const int ORDER[6] = {DOCKED_NATIVE, DOCKED_1_5X, DOCKED_2X, DOCKED_3X, DOCKED_4X, DOCKED_AUTO};
            int i = 0;
            while (i < 5 && ORDER[i] != g_set.docked_res) i++;
            g_set.docked_res = ORDER[(i + (left ? 5 : 1)) % 6];
            settings_save();
            break;
        }
        case G_HANDHELD: {
            static const int ORDER[6] = {HANDHELD_NATIVE, HANDHELD_1_25X, HANDHELD_1_5X, HANDHELD_2X, HANDHELD_3X, HANDHELD_AUTO};
            int i = 0;
            while (i < 5 && ORDER[i] != g_set.handheld_res) i++;
            g_set.handheld_res = ORDER[(i + (left ? 5 : 1)) % 6];
            settings_save();
            break;
        }
        case G_BORDER: g_set.border = !g_set.border; settings_save(); break;
        case G_BACK: if (ok) s_page = PAGE_MAIN; break;
        }
        return true;
    }
    if (s_page == PAGE_HUD && (ok || side)) {
        if (hud_row(*sel) == HU_BACK) { if (ok) s_page = PAGE_GAME; }
        else { g_set.hud_hide ^= 1u << hud_row(*sel); settings_save(); }
        return true;
    }
    if (s_page == PAGE_CTRL && (ok || side)) {
        switch (row_item(PAGE_CTRL, *sel)) {
        case C_SYNC: if (ok) { s_want_controllers = true; s_open = false; s_block = s_pressed & held; s_block_pl = 0; menu_end(); } break;
        case C_MAP:
        case C_JOYMAP: if (ok) { s_mset = row_item(PAGE_CTRL, *sel) == C_JOYMAP; s_page = PAGE_MAP; s_sel[PAGE_MAP] = 0; } break;
        case C_HOLD: g_set.menu_hold = left ? (g_set.menu_hold == 0 ? 5 : g_set.menu_hold == 5 ? 3 : 0) : (g_set.menu_hold == 0 ? 3 : g_set.menu_hold == 3 ? 5 : 0); settings_save(); break;
        case C_BACK: if (ok) s_page = PAGE_MAIN; break;
        }
        return true;
    }
    if (s_page == PAGE_MODS && (ok || side)) {
        switch (*sel) {
        case MD_MOD: if (s_mods_found) { mod_step(left ? -1 : 1); settings_save(); } break;
        case MD_LANG: if (s_langs_found) { lang_step(left ? -1 : 1); settings_save(); } break;
        case MD_HD: g_set.hd_textures = !g_set.hd_textures; settings_save(); break;
        case MD_AUDIO: if (s_audio_found && mod_is_plus(mods_pick(s_mods_found))) { g_set.audio = !g_set.audio; settings_save(); } break;
        case MD_CHARS: if (s_chars_found) { g_set.characters = !g_set.characters; settings_save(); } break;
        case MD_KARTS: if (s_karts_found) { g_set.karts = !g_set.karts; settings_save(); } break;
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
        case GM_UNITS: g_set.speed_kmh = !g_set.speed_kmh; settings_save(); break;
        case GM_RUMBLE: g_set.rumble = (g_set.rumble + (left ? 2 : 1)) % 3; settings_save(); break;
        case GM_JUMP: g_set.jump = (g_set.jump + (left ? 3 : 1)) & 3; cheats_apply(); settings_save(); break;
        case GM_TRICKS: g_set.tricks = (g_set.tricks + (left ? 2 : 1)) % 3; cheats_apply(); settings_save(); break;
        case GM_BATTLE:
            if (ok && g_set.battle_cpus) { s_page = PAGE_BATTLE; s_sel[PAGE_BATTLE] = 0; break; }
            g_set.battle_cpus = !g_set.battle_cpus; cheats_apply(); settings_save();
            break;
        case GM_DRAFT:
            if (ok && g_set.draft) { s_page = PAGE_DRAFT; s_sel[PAGE_DRAFT] = 0; break; }
            g_set.draft = !g_set.draft; cheats_apply(); settings_save();
            break;
        case GM_COINS: g_set.coins = !g_set.coins; cheats_apply(); settings_save(); break;
        case GM_VERSUS: { /* OFF, 4 ... 8 KARTS, round */
            int i = g_set.vs_karts >= 4 ? g_set.vs_karts - 3 : 0;
            i = (i + (left ? 5 : 1)) % 6;
            g_set.vs_karts = i ? i + 3 : 0;
            cheats_apply(); settings_save();
            break;
        }
        case GM_BACK: if (ok) s_page = PAGE_MAIN; break;
        }
        return true;
    }
    if (s_page == PAGE_DRAFT && (ok || side)) {
        switch (*sel) {
        case DR_CPU: g_set.draft_cpu = (g_set.draft_cpu + (left ? 2 : 1)) % 3; cheats_apply(); settings_save(); break;
        case DR_POWER: g_set.draft_power = (g_set.draft_power + (left ? 2 : 1)) % 3; cheats_apply(); settings_save(); break;
        case DR_RANGE: g_set.draft_range = (g_set.draft_range + (left ? 2 : 1)) % 3; cheats_apply(); settings_save(); break;
        case DR_BACK: if (ok) { s_page = PAGE_GAME; s_sel[PAGE_GAME] = GM_DRAFT; } break;
        }
        return true;
    }
    if (s_page == PAGE_BATTLE && (ok || side)) {
        switch (*sel) {
        case BT_KARTS: g_set.battle_karts = (g_set.battle_karts - 4 + (left ? 4 : 1)) % 5 + 4; cheats_apply(); settings_save(); break;
        case BT_HITS: g_set.bomb_hits = (g_set.bomb_hits - 4 + (left ? 16 : 2)) % 18 + 4; cheats_apply(); settings_save(); break;
        case BT_BACK: if (ok) { s_page = PAGE_GAME; s_sel[PAGE_GAME] = GM_BATTLE; } break;
        }
        return true;
    }
    if (s_page == PAGE_PLAYER && (ok || side)) {
        switch (row_item(PAGE_PLAYER, *sel)) {
        case PL_CAM: if (ok) { s_page = PAGE_CAM; s_sel[PAGE_CAM] = 0; } break;
        case PL_MAP:
        case PL_JOYMAP: if (ok) { s_mset = row_item(PAGE_PLAYER, *sel) == PL_JOYMAP; s_page = PAGE_MAP; s_sel[PAGE_MAP] = 0; } break;
        case PL_CLOSE: if (ok) { s_open = false; s_block = s_pressed & held; s_block_pl = s_mpl; s_notice[0] = 0; menu_end(); } break;
        }
        return true;
    }
    if (s_page == PAGE_CAM && (ok || side)) {
        switch (row_item(PAGE_CAM, *sel)) {
        case CA_VIEW: /* A on DRIVER FPV or KART FPV: its eye's page (player 1's: the eyes are everyone's) */
            if (ok && ED(camera) && !s_mpl) { s_page = PAGE_KART; s_sel[PAGE_KART] = 0; s_kart_sel = g_kart_last >= 0 && g_kart_last < 21 ? g_kart_last : 0; break; }
            ED(camera) = (ED(camera) + (left ? 2 : 1)) % 3; settings_save(); break;
        case CA_HEIGHT: ED(cam_height) = (ED(cam_height) + (left ? 3 : 1)) & 3; settings_save(); break;
        case CA_DIST: ED(cam_dist) = (ED(cam_dist) + (left ? 4 : 1)) % 5; settings_save(); break;
        case CA_FOV: g_set.fov[g_set.wide] = (g_set.fov[g_set.wide] + (left ? 4 : 1)) % 5; settings_save(); break;
        case CA_FREE: ED(free_cam) = (ED(free_cam) + (left ? 3 : 1)) & 3; settings_save(); break;
        case CA_STYLE: g_set.hud_style = (g_set.hud_style + (left ? 2 : 1)) % 3; settings_save(); break;
        case CA_HUD:
            if (ok) { s_page = PAGE_HUD; s_sel[PAGE_HUD] = 0; }
            else { g_set.hud_hide = g_set.hud_hide & hud_mask() ? g_set.hud_hide & ~hud_mask() : g_set.hud_hide | hud_mask(); settings_save(); } /* LEFT/RIGHT: all on / all off */
            break;
        case CA_BACK: if (ok) s_page = page_parent(PAGE_CAM); break;
        }
        return true;
    }
    if (s_page == PAGE_KART && (ok || side)) {
        switch (*sel) {
        case KA_KART: if (fpv_each_kart() && !s_in_race) s_kart_sel = (s_kart_sel + (side && left ? 20 : 1)) % 21; break;
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
        bool upd = !item_hidden(PAGE_MAIN, M_UPDATE);
        *sel = left ? (*sel == M_DEBUG && upd ? M_UPDATE : M_EXIT) : (*sel == M_EXIT && upd ? M_UPDATE : M_DEBUG);
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
        else if (*sel == P_RESET) { memcpy(CUR_MAP, CUR_DEFAULT, sizeof ED(map)); s_notice[0] = 0; settings_save(); }
        else if (map_leave(false)) s_page = page_parent(PAGE_MAP);
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
#define BATCH GFX_UI_BATCH
enum { SH_GLYPH, SH_BOX, SH_FONT, SH_ROUND, SH_STAR, SH_ARROW, SH_IMAGE, SH_LINE };
static GfxUiBatch s_b;
static int s_n;
static GfxTex s_tex; /* the batch's texture: the font atlas or a picture */

static bool s_font;   /* MarioFont glyphs loaded from the disc */

void menu_font_load(void) { /* again later if the disc wasn't up yet (a few tries) */
    static int tries;
    if (s_font || tries >= 4) return;
    tries++;
    s_font = uifont_load();
}

static void flush(void) {
    if (!s_n) return;
    gfx_ui_draw(&s_b, (unsigned)s_n, s_tex);
    s_n = 0;
}

static void rgba(float out[4], uint32_t c) {
    for (int i = 0; i < 4; i++) out[i] = (float)((c >> (24 - 8 * i)) & 0xFF) / 255.0f;
}
static void quad(float x, float y, float w, float h, uint32_t top, uint32_t bot, unsigned shape, uint32_t b0, uint32_t b1,
                 const float par[4]) {
    if (s_n == BATCH) flush();
    s_b.rect[s_n][0] = x; s_b.rect[s_n][1] = y; s_b.rect[s_n][2] = w; s_b.rect[s_n][3] = h;
    rgba(s_b.col[s_n], top);
    rgba(s_b.col2[s_n], bot);
    for (int i = 0; i < 4; i++) s_b.par[s_n][i] = par ? par[i] : 0.0f;
    s_b.bits[s_n][0] = b0;
    s_b.bits[s_n][1] = (b1 & 0x0FFFFFFFu) | (uint32_t)shape << 28;
    s_n++;
}
static void rect(float x, float y, float w, float h, uint32_t rgba_) { quad(x, y, w, h, rgba_, rgba_, SH_BOX, 0, 0, NULL); }
static void grad(float x, float y, float w, float h, uint32_t top, uint32_t bot) { quad(x, y, w, h, top, bot, SH_BOX, 0, 0, NULL); }
static void round_box(float x, float y, float w, float h, float r, uint32_t c) {
    const float par[4] = {r, 0, 0, 0};
    quad(x, y, w, h, c, c, SH_ROUND, 0, 0, par);
}
static void star(float cx, float cy, float r, uint32_t c) { quad(cx - r, cy - r, 2 * r, 2 * r, c, c, SH_STAR, 0, 0, NULL); }
static void star_sharp(float cx, float cy, float r, uint32_t c) { quad(cx - r, cy - r, 2 * r, 2 * r, c, c, SH_STAR, 0, 0, (const float[4]){1, 0, 0, 0}); }
/* PC: a hint's buttons with player 1's keys beside them: "A: SELECT" -> "A (X): SELECT" (a button
 * is a word of its own followed by ":", " OR " or " TO "); the Switch's as they are */
static const char *key_hint(const char *h) {
#ifdef _WIN32
    static char out[2][192];
    static int which;
    char *o = out[which ^= 1];
    size_t n = 0;
    for (const char *p = h; *p && n + 1 < sizeof out[0]; p++) {
        o[n++] = *p;
        int kb = *p == 'A' ? KB_A : *p == 'B' ? KB_B : *p == 'Y' ? KB_Y : *p == '-' ? KB_MINUS : -1;
        if (kb < 0 || (p != h && p[-1] != ' ') || !(p[1] == ':' || !strncmp(p + 1, " OR ", 4) || !strncmp(p + 1, " TO ", 4))) continue;
        char name[32];
        wincfg_key_name(g_win.keys[kb][0], name, sizeof name);
        if (!name[0]) continue;
        for (char *c = name; *c; c++) *c = (char)toupper((unsigned char)*c);
        int w = snprintf(o + n, sizeof out[0] - n, " (%s)", name);
        if (w > 0) n += (size_t)w < sizeof out[0] - n ? (size_t)w : sizeof out[0] - n - 1;
    }
    o[n] = 0;
    return o;
#else
    return h;
#endif
}
static void arrow(float x, float y, float w, float h, uint32_t c) { quad(x, y, w, h, c, c, SH_ARROW, 0, 0, NULL); }
/* a line (x0, y0) to (x1, y1), wd px wide, colour c0 at its start to c1 at its end */
static void line(float x0, float y0, float x1, float y1, float wd, uint32_t c0, uint32_t c1) {
    float m = wd + 1, lx = (x0 < x1 ? x0 : x1) - m, ly = (y0 < y1 ? y0 : y1) - m;
    const float par[4] = {x0 - lx, y0 - ly, x1 - lx, y1 - ly};
    quad(lx, ly, fabsf(x1 - x0) + 2 * m, fabsf(y1 - y0) + 2 * m, c0, c1, SH_LINE, (uint32_t)(wd * 16), 0, par);
}

/* DRAFTING's wind streaks (native.c sets g_draft_show): a slipstream, thin white lines hugging the
 * kart's sides as they stream past it toward the camera, along the road's perspective: each runs on
 * a ray from the vanishing point (the screen's middle) through a point beside the kart's middle
 * (where the chase camera puts it: the middle, about three quarters down), starting there (none
 * ahead of the kart) and fanning at half the perspective's rate (held close to the body). 16 to 40
 * of them, more and brighter as the meter fills, brightest during the boost. Screens: one player whole, two top and bottom, three or four in quarters. Gone
 * once the karts stop updating (pause, the race over) for a fifth of a second. */
static bool draft_streaks_on(void) {
    static uint32_t seen;
    static uint64_t at;
    uint32_t q = g_draft_seq;
    uint64_t now = plat_now_ns();
    if (q != seen) seen = q, at = now;
    if (!g_draft || !at || now - at > 200000000ull) return false;
    for (unsigned v = 0; v < 4 && v < g_draft_views; v++)
        if (g_draft_show[v] > 0.01f) return true;
    return false;
}
static void draft_streaks(int w, int h) {
    unsigned views = g_draft_views > 4 ? 4 : g_draft_views;
    float t = (float)(plat_now_ns() % 100000000000ull) / 1e9f, k = (float)h / 720.0f;
    for (unsigned v = 0; v < views; v++) {
        float s = g_draft_show[v];
        if (!(s > 0.01f)) continue; /* NaN too */
        if (s > 1.5f) s = 1.5f;
        float vx = 0, vy = 0, vw = (float)w, vh = (float)h;
        if (views == 2) vh /= 2, vy = v * vh;
        else if (views > 2) vw /= 2, vh /= 2, vx = (v & 1) * vw, vy = (v >> 1) * vh;
        float kx = vx + vw / 2, ky = vy + vh * 0.73f, rx = vw * 0.05f, ry = vh * 0.12f; /* the kart's outline */
        float hx = kx, hy = vy + vh * 0.45f, bottom = vy + vh;                         /* the vanishing point */
        float len = 0.2f + 0.1f * (s > 1 ? 1 : s); /* along the ray, the base point at 1 */
        unsigned lines = 16 + (unsigned)(24 * (s > 1 ? 1 : s));
        float wd = (s > 1 ? 2.0f : 1.5f) * k * (views > 1 ? 0.7f : 1.0f);
        float a = s > 1 ? 150 : 50 + 70 * s;
        for (unsigned i = 0; i < lines; i++) {
            float f = (float)i * 0.618034f, g = (float)i * 0.754878f + 0.31f;
            f -= floorf(f), g -= floorf(g);
            float side = (i & 1) ? 1.0f : -1.0f, umax = 1.8f;
            float bx = kx + side * rx * (0.85f + 0.25f * f), by = ky + ry * 0.3f * (g - 0.5f); /* the body's middle */
            float ph = t * (1.1f + 0.15f * (float)(i % 4)) + (float)i * 0.29f;
            ph -= floorf(ph);
            float u0 = 1.0f + (umax - 1.0f) * ph, u1 = u0 + len;
            if (u1 > umax) u1 = umax;
            if (by > hy && hy + (by - hy) * u1 > bottom) u1 = (bottom - hy) / (by - hy); /* inside its screen */
            float x0 = bx + (bx - hx) * (u0 - 1) * 0.5f, y0 = hy + (by - hy) * u0, x1 = bx + (bx - hx) * (u1 - 1) * 0.5f, y1 = hy + (by - hy) * u1;
            if ((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0) < 9 * k * k) continue;
            uint32_t fade = (uint32_t)(a * (ph < 0.15f ? ph / 0.15f : ph > 0.7f ? (1 - ph) / 0.3f : 1.0f));
            line(x0, y0, x1, y1, wd, 0xFFFFFF00u | fade / 3, 0xFFFFFF00u | fade);
        }
    }
}

/* 5x7 bitmap text at (x, y) with glyph pixel size px; returns the width. */
static float text(float x, float y, float px, uint32_t rgba_, const char *s) {
    float x0 = x;
    for (; *s; s++, x += 6 * px) {
        const char *g = font_glyph(*s);
        if (!g) continue;
        uint32_t b[2] = {0, 0};
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
                uint32_t b[2] = {0, 0};
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
uint64_t menu_blocked(int p) { return p == s_block_pl ? s_block : 0; }
bool menu_take_fullscreen(void) {
    bool w = s_want_fullscreen;
    s_want_fullscreen = false;
    return w;
}
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

/* The overlay on the screen (clear: black first); false if the backend has none. */
static bool begin(int w, int h, bool clear) {
    if (!gfx_ui_begin(w, h, clear)) return false;
    s_b.screen[0] = (float)w, s_b.screen[1] = (float)h;
    s_tex = s_font ? uifont_texture() : 0;
    return true;
}
static void end(void) {
    flush();
    gfx_ui_end();
}

void menu_draw(int w, int h, float fps, const char *perf) {
    /* an extra kart's name for a moment: the game's own text is the stock kart's */
    bool label[4], any_label = false;
    int labels = 0, label_rows = 1;
    for (int i = 0; i < 4; i++) {
        uint64_t at = g_kart_label_at[i];
        label[i] = at && plat_now_ns() - at < 3000000000ull;
        any_label = any_label || label[i];
        if (label[i]) { labels++; if (i >= 2) label_rows = 2; }
    }
    bool streaks = !s_open && draft_streaks_on();
    if (!s_open && !g_set.show_fps && !any_label && !streaks) return;
    menu_font_load();
    if (!begin(w, h, false)) return;
    float k = (float)h / 720.0f;
    if (streaks) draft_streaks(w, h);

    for (int i = 0; i < 4; i++) {
        if (!label[i]) continue;
        char up[56];
        upper(up, sizeof up, g_kart_label[i]);
        float px = 3 * k, tw = text_w(up, px);
        /* in the strip under the preview boxes: alone centred, else one player's corner */
        float cx = labels == 1 ? (float)w / 2 : (float)w * (i % 2 ? 0.75f : 0.25f);
        float x0 = cx - tw / 2, y0 = (label_rows == 1 ? 664 : 652 + 36 * (i / 2)) * k;
        rect(x0 - 3 * px, y0 - 2 * px, tw + 6 * px, 11 * px, 0x000000A0u);
        text(x0, y0, px, 0xFFFFFFFFu, up);
    }

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
        if (st.up && !st.consoles) snprintf(net, sizeof net, SW_PC("WI-FI: LOOKING", "LAN: LOOKING"));
        else if (st.up && st.silent == st.consoles) snprintf(net, sizeof net, SW_PC("WI-FI: %u FOUND", "LAN: %u FOUND"), st.consoles);
        else if (st.up && st.loss_pct >= 0) snprintf(net, sizeof net, SW_PC("WI-FI %u MS (UP TO %u) %d%% LOST", "LAN %u MS (UP TO %u) %d%% LOST"), st.rtt_ms, st.rtt_max_ms, st.loss_pct);
        else if (st.up) snprintf(net, sizeof net, SW_PC("WI-FI %u MS", "LAN %u MS"), st.rtt_ms);
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
                            : s_page == PAGE_CTRL ? "CONTROLS" : s_page == PAGE_GAME ? "GAME" : s_page == PAGE_BATTLE ? "SINGLE PLAYER BATTLE" : s_page == PAGE_DRAFT ? "DRAFTING" : s_page == PAGE_CAM ? "CAMERA" : s_page == PAGE_KART ? (ED(camera) == 2 ? "KART FPV" : "DRIVER FPV")
                            : s_page == PAGE_MODS ? "MODS" : s_page == PAGE_MULTI ? "MULTIPLAYER" : s_page == PAGE_LAN ? "LAN PLAY" : s_page == PAGE_HUD ? "HUD ASSETS" : s_page == PAGE_CHEATS ? "CHEATS" : s_page == PAGE_SPECIAL ? "SPECIAL ITEMS" : "OPTIONS";
        char ptitle[48];
        if (s_mpl) { /* players 2-4: their name on every page */
            snprintf(ptitle, sizeof ptitle, s_page == PAGE_PLAYER ? "PLAYER %d" : "PLAYER %d %s", s_mpl + 1, title);
            title = ptitle;
        }
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
                const int b0 = item_hidden(PAGE_MAIN, M_UPDATE) ? 1 : 0; /* hidden: DEBUG only */
                for (int i = 1; i >= b0; i--) {
                    bws[i] = mtext_w(blabel[i], bs) + 40 * k;
                    bx -= bws[i];
                    bxs[i] = bx;
                    bx -= 16 * k;
                }
                mrow(x, yb, bxs[b0] - x + 12 * k, rh, k, msel == M_EXIT, "EXIT GAME", NULL, false);
                for (int i = b0; i < 2; i++) {
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
                   : s_sel[PAGE_MAIN] == M_MULTI  ? SW_PC("LAN PLAY OVER WI-FI WITH OTHER CONSOLES", "LAN PLAY WITH OTHER PCS AND SWITCHES")
                   : s_sel[PAGE_MAIN] == M_CTRL   ? SW_PC("BUTTON MAPPING, SYNC CONTROLLERS", "BUTTON MAPPING, OPEN MENU")
                   : s_sel[PAGE_MAIN] == M_GAME   ? "RACE HUD, SPEED, HUD ASSETS, JUMP, COINS, RUMBLE, SINGLE PLAYER BATTLE"
                   : s_sel[PAGE_MAIN] == M_CHEATS ? "UNLOCK ALL, NO WEIGHT CLASSES, SPECIAL ITEMS, MINI-TURBO, LAPS"
                   : s_sel[PAGE_MAIN] == M_DEBUG  ? "FPS AND FRAME TIMES, LOGS, PROFILER"
                   : s_sel[PAGE_MAIN] == M_UPDATE ? uhint
                   : s_sel[PAGE_MAIN] == M_EXIT   ? "A: EXIT THE GAME   RIGHT: UPDATE, DEBUG"
                                                  : "GAME PAUSED   A: SELECT   B OR -: CLOSE";
        } else if (s_page == PAGE_CTRL) {
            static const char *labels[C_COUNT] = {"BUTTON MAPPING", "JOY-CON SIDEWAYS", "SYNC CONTROLLERS", "OPEN MENU", "BACK"};
            const char *values[C_COUNT] = {NULL, NULL, NULL, g_set.menu_hold == 3 ? "HOLD 3 SEC" : g_set.menu_hold == 5 ? "HOLD 5 SEC" : "PRESS", NULL};
            static const bool more[C_COUNT] = {false, false, true, false, false};
            const int n = shown_rows(PAGE_CTRL), cur = row_item(PAGE_CTRL, s_sel[PAGE_CTRL]);
            for (int r = 0; r < n; r++, ry += rh) {
                int i = row_item(PAGE_CTRL, r);
                mrow(x, ROW_Y(r, n), pw, rh, k, s_sel[PAGE_CTRL] == r, labels[i], values[i], more[i]);
            }
            hint = cur == C_SYNC     ? "PAIR CONTROLLERS, SET THE PLAYER ORDER, SINGLE JOY-CONS"
                   : cur == C_MAP    ? SW_PC("CHOOSE WHAT EACH SWITCH BUTTON DOES", "CHOOSE WHAT EACH CONTROLLER BUTTON DOES WHILE DRIVING")
                   : cur == C_JOYMAP ? "BUTTONS OF ONE JOY-CON HELD SIDEWAYS"
                   : cur == C_HOLD   ? "HOLD - TO OPEN THIS MENU, TO KEEP CHEATS FROM CHILDREN"
                                                  : "B: BACK";
        } else if (s_page == PAGE_GFX) {
            static const char *labels[G_COUNT] = {"WIDESCREEN", "FULL SCREEN", "4:3 EDGE BORDER", SW_PC("DOCKED RESOLUTION", "RENDER RESOLUTION"),
                                                  "HANDHELD RESOLUTION", "TEXTURE FILTERING", "COLOR", "BACK"};
            const char *values[G_COUNT] = {g_set.wide ? "16:9" : "4:3", FULLSCREEN_ON ? "ON" : "OFF", g_set.border ? "ON" : "OFF",
                                           DOCKED_NAMES[g_set.docked_res], HANDHELD_NAMES[g_set.handheld_res], ANISO_NAMES[g_set.aniso % 5],
                                           COLOR_NAMES[g_color.mode], NULL};
            static const bool more[G_COUNT] = {false, false, false, false, false, false, true, false};
            const int n = shown_rows(PAGE_GFX), cur = row_item(PAGE_GFX, s_sel[PAGE_GFX]);
            for (int r = 0; r < n; r++, ry += rh) {
                int i = row_item(PAGE_GFX, r);
                mrow(x, ROW_Y(r, n), pw, rh, k, s_sel[PAGE_GFX] == r, labels[i], values[i], more[i]);
            }
            hint = cur == G_WIDE     ? "16:9: WIDER VIEW, RACE HUD AT THE SCREEN EDGES"
                   : cur == G_FULL   ? "THE WHOLE SCREEN, NO WINDOW BORDER. ALSO F11 OR ALT+ENTER"
                   : cur == G_BORDER ? "4:3: CHECKERED BORDER BESIDE THE RACE"
                   : cur == G_DOCKED && SW_PC(0, 1) ? (g_set.docked_res >= DOCKED_3X ? "VERY SHARP: NEEDS A FAST GRAPHICS CARD"
                                                       : "AUTO: FITS THE WINDOW. HIGHER IS SHARPER, BUT SLOWER")
                   : cur == G_DOCKED ? g_set.docked_res == DOCKED_4X ? "ABOUT 60 FPS WITH 1 PLAYER; 2 TO 4 PLAYERS RUN SLOWER" : g_set.docked_res == DOCKED_3X ? "VERY SHARP, BUT THE FRAME RATE CAN DROP" : "ON THE TV, 1080P OUTPUT. AUTO: 2X, LOWER WHEN THE FRAME RATE DROPS"
                   : cur == G_HANDHELD ? g_set.handheld_res == HANDHELD_3X ? "OVERCLOCK RECOMMENDED: ABOUT 30 FPS AT STOCK CLOCKS" : g_set.handheld_res == HANDHELD_2X ? "VERY SHARP, BUT THE FRAME RATE CAN DROP" : "SHARPER IN HANDHELD. AUTO: 1.5X, LOWER WHEN THE FRAME RATE DROPS"
                   : cur == G_ANISO  ? "SHARPER TEXTURES AT ANGLES: ROADS AND WALLS IN THE DISTANCE"
                   : cur == G_COLOR  ? "A: ADJUST   LEFT/RIGHT: ACCURATE, VIVID, CUSTOM"
                                                 : "B: BACK";
        } else if (s_page == PAGE_MODS) {
            static const char *labels[MD_COUNT] = {"LEVEL PACK", "LANGUAGE", "STEREO MUSIC", "HD TEXTURES", "EXTRA CHARACTERS", "EXTRA KARTS", "RESTART NOW", "BACK"};
            unsigned pick = mods_pick(s_mods_found);
            char mod_value[16], mod_hint[96];
            upper(mod_value, sizeof mod_value, pick ? MODS[pick - 1].key : s_mods_found ? "off" : "none found");
            upper(mod_hint, sizeof mod_hint, pick ? MODS[pick - 1].name : "the standard game");
            snprintf(mod_hint + strlen(mod_hint), sizeof mod_hint - strlen(mod_hint), pick ? ": ITS OWN SAVE" : "");
            char lang_value[16];
            upper(lang_value, sizeof lang_value, LANG_NAMES[langs_pick(s_langs_found)]);
            const char *values[MD_COUNT] = {mod_value, lang_value,
                                            !s_audio_found ? "NONE FOUND" : !mod_is_plus(pick) ? "PLUS ONLY" : g_set.audio ? "ON" : "OFF",
                                            !hdtex_available() ? "NO PACK" : g_set.hd_textures ? "ON" : "OFF",
                                            !s_chars_found ? "NONE FOUND" : g_set.characters ? "ON" : "OFF",
                                            !s_karts_found ? "NONE FOUND" : g_set.karts ? "ON" : "OFF",
                                            NULL, NULL};
            for (int i = 0; i < MD_COUNT; i++, ry += rh) mrow(x, ROW_Y(i, MD_COUNT), pw, rh, k, s_sel[PAGE_MODS] == i, labels[i], values[i], false);
            char hd_hint[64];
            if (!hdtex_available()) snprintf(hd_hint, sizeof hd_hint, SW_PC("PUT HD-TEXTURES.BIN IN THE MKDD-NX FOLDER", "PUT HD-TEXTURES.BIN IN MODS/HD"));
            else if (!g_set.hd_textures) snprintf(hd_hint, sizeof hd_hint, "COMMUNITY HD TEXTURE PACK");
            else snprintf(hd_hint, sizeof hd_hint, "COMMUNITY HD TEXTURE PACK: %u IN USE", render_hd_in_use());
            const int s = s_sel[PAGE_MODS];
            hint = s == MD_MOD && !s_mods_found     ? "PUT A MOD IN THE MODS FOLDER (SEE README)"
                   : s == MD_MOD && mods_changed()   ? "CHANGES AFTER A RESTART: SEE RESTART NOW"
                   : s == MD_MOD                     ? mod_hint
                   : s == MD_LANG && !s_langs_found  ? "MORE: A PAL OR JAPANESE DISC'S, BY MKDD-NX TOOLS, IN MODS/LANG"
                   : s == MD_LANG && pick && langs_pick(s_langs_found) ? "THE STANDARD GAME ONLY: LEVEL PACKS ARE IN ENGLISH"
                   : s == MD_LANG && lang_pending()  ? "CHANGES AFTER A RESTART: SEE RESTART NOW"
                   : s == MD_LANG                    ? "THE GAME'S MENUS AND TEXTS, FROM YOUR OWN DISC'S FILES"
                   : s == MD_AUDIO && !s_audio_found ? "PUT THE EXPANSION PACK'S FILES FOLDER IN MODS/AUDIO (SEE README)"
                   : s == MD_AUDIO && !mod_is_plus(pick) ? "THE MKDD PLUS EXPANSION PACK: PICK PLUS AS THE LEVEL PACK"
                   : s == MD_AUDIO && audio_pending() ? "CHANGES AFTER A RESTART: SEE RESTART NOW"
                   : s == MD_AUDIO                   ? "MKDD PLUS'S TRACK MUSIC IN STEREO"
                   : s == MD_HD                      ? hd_hint
                   : s == MD_CHARS && !s_chars_found ? "PUT CHARACTERS IN MODS/CHARACTERS (SEE README)"
                   : s == MD_CHARS && chars_pending() ? "CHANGES AFTER A RESTART: SEE RESTART NOW"
                   : s == MD_CHARS                   ? "MORE CHARACTERS ON THE CHARACTER SELECT SCREEN"
                   : s == MD_KARTS && !s_karts_found ? "PUT KARTS IN MODS/KARTS (SEE README)"
                   : s == MD_KARTS && karts_pending() ? "CHANGES AFTER A RESTART: SEE RESTART NOW"
                   : s == MD_KARTS                   ? "MORE KARTS ON THE KART SCREEN, EACH NEXT TO THE KART IT DRIVES AS"
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
            snprintf(lan_hint, sizeof lan_hint, SW_PC("THIS CONSOLE: %s. A: TYPE THE OTHER'S. LEFT/RIGHT: ANY", "THIS PC: %s. SET THE OTHER'S IN THE LAUNCHER. LEFT/RIGHT: ANY"), own_ip());
            unsigned pick = mods_pick(s_mods_found);
            bool rtgp = MOD_IS_RTGP(pick); /* and its Baby / Classic */
            hint = s == LN_LAN && lan_pending()  ? "CHANGES AFTER A RESTART: SEE RESTART NOW"
                   : s == LN_LAN && rtgp           ? "NOT WITH RETRO TRACK GRAND PRIX: ITS LAN MODE DOESN'T RUN"
                   : s == LN_LAN                   ? SW_PC("LAN MODE ON THE TITLE SCREEN, OVER WI-FI. SAME MOD ON EVERY CONSOLE", "LAN MODE ON THE TITLE SCREEN. SAME MOD ON EVERY PC AND SWITCH")
                   : s == LN_PEER && lan_pending() ? "CHANGES AFTER A RESTART: SEE RESTART NOW"
                   : s == LN_PEER                  ? lan_hint
                   : s == LN_LINK                  ? link_hint
                   : s == LN_RESTART               ? restart_hint()
                                                   : "B: BACK";
        } else if (s_page == PAGE_GAME) {
            static const char *labels[GM_COUNT] = {"SPEED", "JUMP HEIGHT", "TRICKS", "DRAFTING", "COINS", "RUMBLE STRENGTH", "SINGLE PLAYER BATTLE MODE", "VERSUS CPUS", "BACK"};
            const char *values[GM_COUNT] = {g_set.speed_kmh ? "KMH" : "MPH",
                                            JUMP_NAMES[g_set.jump & 3], g_set.tricks == 2 ? "ON + WIPEOUTS" : g_set.tricks ? "ON" : "OFF", g_set.draft ? "ON" : "OFF", g_set.coins ? "ON" : "OFF", RUMBLE_NAMES[g_set.rumble % 3],
                                            g_set.battle_cpus ? "ON" : "OFF", VS_KARTS_NAMES[g_set.vs_karts >= 4 && g_set.vs_karts <= 8 ? g_set.vs_karts - 3 : 0], NULL};
            const bool more[GM_COUNT] = {false, false, false, g_set.draft != 0, false, false, g_set.battle_cpus, false, false};
            for (int i = 0; i < GM_COUNT; i++, ry += rh) mrow(x, ROW_Y(i, GM_COUNT), pw, rh, k, s_sel[PAGE_GAME] == i, labels[i], values[i], more[i]);
            hint = s_sel[PAGE_GAME] == GM_UNITS ? "SPEEDOMETER IN MILES OR KILOMETRES PER HOUR"
                   : s_sel[PAGE_GAME] == GM_BATTLE ? (g_set.battle_cpus ? "A: BATTLE KARTS, BOB-OMB HITS   LEFT/RIGHT: OFF"
                                                                        : "CPUS FILL BATTLES TO 4 KARTS. 1 PLAYER: BATTLE ON THE MODE PAGE")
                   : s_sel[PAGE_GAME] == GM_VERSUS ? (g_lan_play ? "OFF IN LAN PLAY" : "2-4 PLAYER VERSUS: CPUS FILL THE RACE TO THIS MANY KARTS. FROM THE NEXT RACE")
                   : s_sel[PAGE_GAME] == GM_TRICKS ? (g_lan_play ? "OFF IN LAN PLAY" : g_set.tricks == 2 ? "AS ON, BUT A BAD LANDING SPINS YOU OUT" : "IN THE AIR: HOLD D-PAD TO TURN, JUMP: RIDER TRICKS. LAND STRAIGHT: BOOST")
                   : s_sel[PAGE_GAME] == GM_RUMBLE ? "VIBRATION STRENGTH. THE GAME'S OWN RUMBLE OPTION MUST BE ON TOO"
                   : s_sel[PAGE_GAME] == GM_DRAFT ? (g_lan_play ? "OFF IN LAN PLAY"
                                                    : g_set.draft ? "A: CPU DRAFTING, STRENGTH, DISTANCE   LEFT/RIGHT: OFF"
                                                                  : "STAY BEHIND A KART AT SPEED TO FILL UP, THEN BOOST. GP AND VS")
                   : s_sel[PAGE_GAME] == GM_COINS ? (g_lan_play ? "OFF IN LAN PLAY" : "COINS ON THE TRACK: EACH ONE HELD ADDS TOP SPEED, HITS DROP THEM. GP AND VS, FROM THE NEXT RACE")
                   : s_sel[PAGE_GAME] == GM_JUMP ? (ED(map)[MAP_JUMP] ? "ONE HOP PER PRESS OF THE JUMP BUTTON"
                                                                       : "GIVE JUMP A BUTTON: CONTROLS > BUTTON MAPPING")
                                                 : "B: BACK";
        } else if (s_page == PAGE_DRAFT) {
            static const char *labels[DR_COUNT] = {"CPU DRAFTING", "DRAFT STRENGTH", "DRAFT DISTANCE", "BACK"};
            const char *values[DR_COUNT] = {DRAFT_CPU_NAMES[g_set.draft_cpu % 3], DRAFT_POWER_NAMES[g_set.draft_power % 3],
                                            DRAFT_RANGE_NAMES[g_set.draft_range % 3], NULL};
            for (int i = 0; i < DR_COUNT; i++, ry += rh) mrow(x, ROW_Y(i, DR_COUNT), pw, rh, k, s_sel[PAGE_DRAFT] == i, labels[i], values[i], false);
            hint = s_sel[PAGE_DRAFT] == DR_CPU     ? "COMPUTER KARTS DRAFTING TOO. WEAKER: SLOWER TO FILL, SHORTER BOOST"
                   : s_sel[PAGE_DRAFT] == DR_POWER ? "HOW LONG THE DRAFT BOOST LASTS"
                   : s_sel[PAGE_DRAFT] == DR_RANGE ? "HOW FAR BEHIND A KART A DRAFT STARTS"
                                                   : "B: BACK";
        } else if (s_page == PAGE_BATTLE) {
            static const char *labels[BT_COUNT] = {"BATTLE KARTS", "BOB-OMB HITS TO WIN", "BACK"};
            char karts_value[8], hits_value[8];
            snprintf(karts_value, sizeof karts_value, "%d", g_set.battle_karts);
            snprintf(hits_value, sizeof hits_value, "%d", g_set.bomb_hits);
            const char *values[BT_COUNT] = {karts_value, hits_value, NULL};
            for (int i = 0; i < BT_COUNT; i++, ry += rh) mrow(x, ROW_Y(i, BT_COUNT), pw, rh, k, s_sel[PAGE_BATTLE] == i, labels[i], values[i], false);
            hint = s_sel[PAGE_BATTLE] == BT_KARTS  ? "KARTS IN A 1 PLAYER BATTLE, YOU AND THE CPUS. 2-3 PLAYERS: ALWAYS 4"
                   : s_sel[PAGE_BATTLE] == BT_HITS ? "1 PLAYER BOB-OMB BLAST: HITS TO FILL THE 4 STARS. 4 IS THE GAME'S OWN"
                                                   : "B: BACK";
        } else if (s_page == PAGE_CAM) {
            static const char *labels[CA_COUNT] = {"CAMERA VIEW", "CAMERA HEIGHT", "CAMERA DISTANCE", "FIELD OF VIEW", "FREE CAMERA", "RACE HUD", "HUD ASSETS", "BACK"};
            static const char *HEIGHT_NAMES[4] = {"LOW", "MEDIUM", "REGULAR", "HIGH"};
            static const char *DIST_NAMES[5] = {"VERY CLOSE", "CLOSE", "REGULAR", "FAR", "VERY FAR"};
            static const char *FREECAM_NAMES[4] = {"OFF", "SNAP BACK", "EASE BACK", "STAY"};
            const char *values[CA_COUNT] = {CAMERA_NAMES[ED(camera) % 3], HEIGHT_NAMES[ED(cam_height) & 3], DIST_NAMES[ED(cam_dist) % 5],
                                            FOV_NAMES[g_set.fov[g_set.wide] % 5], FREECAM_NAMES[ED(free_cam) & 3], HUD_STYLES[g_set.hud_style], hud_value(), NULL};
            const int n = shown_rows(PAGE_CAM), cur = row_item(PAGE_CAM, s_sel[PAGE_CAM]);
            for (int r = 0; r < n; r++, ry += rh) {
                int i = row_item(PAGE_CAM, r);
                mrow(x, ROW_Y(r, n), pw, rh, k, s_sel[PAGE_CAM] == r, labels[i], values[i], (i == CA_VIEW && ED(camera) && !s_mpl) || i == CA_HUD);
            }
            hint = cur == CA_VIEW     ? (s_mpl ? "FIRST PERSON: DRIVER FPV OR KART FPV" : ED(camera) == 2 ? "A: ADJUST KART FPV" : ED(camera) ? "A: ADJUST DRIVER FPV" : "FIRST PERSON: DRIVER FPV OR KART FPV")
                   : cur == CA_HEIGHT ? "THE CAMERA BEHIND THE KART. REGULAR: THE GAME'S"
                   : cur == CA_DIST   ? "HOW FAR BEHIND THE KART. REGULAR: THE GAME'S"
                   : cur == CA_STYLE  ? (g_set.hud_style == 2 ? "ONE BAR ACROSS THE TOP, REAR-VIEW MIRROR, RACE ORDER BELOW. 1 PLAYER" : g_set.hud_style == 1 ? "MODERN: SMALLER, IN THE CORNERS. 1 TO 4 PLAYERS" : "THE GAME'S OWN")
                   : cur == CA_HUD    ? "A: CHOOSE   LEFT/RIGHT: ALL ON, ALL OFF"
                   : cur == CA_FOV    ? (g_set.wide ? "RACE CAMERA IN 16:9, DEFAULT WIDER" : "RACE CAMERA IN 4:3, DEFAULT NORMAL")
                   : cur == CA_FREE   ? (ED(free_cam) == 0 ? "RIGHT STICK LEFT/RIGHT: TURN THE CAMERA ROUND THE KART"
                                                     : ED(free_cam) == 1 ? "RIGHT STICK TURNS THE CAMERA. LET GO: STRAIGHT BACK"
                                                     : ED(free_cam) == 2 ? "RIGHT STICK TURNS THE CAMERA. LET GO: GLIDES BACK"
                                                     : ED(map)[MAP_CAM_RESET] ? "RIGHT STICK TURNS THE CAMERA. CAMERA RESET: BACK"
                                                                                : "GIVE CAMERA RESET A BUTTON: CONTROLS > BUTTON MAPPING")
                                                  : "B: BACK";
        } else if (s_page == PAGE_PLAYER) {
            static const char *labels[PL_COUNT] = {"CAMERA", "BUTTON MAPPING", "JOY-CON SIDEWAYS", "CLOSE"};
            const int n = shown_rows(PAGE_PLAYER), cur = row_item(PAGE_PLAYER, s_sel[PAGE_PLAYER]);
            for (int r = 0; r < n; r++, ry += rh) {
                int i = row_item(PAGE_PLAYER, r);
                mrow(x, ROW_Y(r, n), pw, rh, k, s_sel[PAGE_PLAYER] == r, labels[i], NULL, i != PL_CLOSE);
            }
            hint = cur == PL_CAM      ? "YOUR VIEW, HEIGHT, DISTANCE, FREE CAMERA"
                   : cur == PL_MAP    ? SW_PC("CHOOSE WHAT EACH OF YOUR BUTTONS DOES", "CHOOSE WHAT EACH OF YOUR BUTTONS DOES WHILE DRIVING")
                   : cur == PL_JOYMAP ? "YOUR BUTTONS ON ONE JOY-CON HELD SIDEWAYS"
                                      : "GAME PAUSED   B OR -: CLOSE. THE GAME'S OPTIONS: PLAYER 1";
        } else if (s_page == PAGE_KART) {
            static const char *labels[KA_COUNT] = {"KART", "EYE HEIGHT", "EYE FORWARD", "TILT", "DEFAULTS", "BACK"};
            static char kup[16], kfwd[16], ktilt[16], khint[48];
            snprintf(kup, sizeof kup, "%d", *fpv_eye(KA_UP));
            snprintf(kfwd, sizeof kfwd, "%d", *fpv_eye(KA_FWD));
            snprintf(ktilt, sizeof ktilt, "%d DEG", *fpv_eye(KA_TILT));
            const char *values[KA_COUNT] = {fpv_each_kart() ? KART_NAMES[s_kart_sel] : "ALL", kup, kfwd, ktilt, NULL, NULL};
            for (int i = 0; i < KA_COUNT; i++, ry += rh) mrow(x, ROW_Y(i, KA_COUNT), pw, rh, k, s_sel[PAGE_KART] == i, labels[i], values[i], false);
            int ks = s_sel[PAGE_KART];
            if (ks >= KA_UP && ks <= KA_TILT)
                snprintf(khint, sizeof khint, "%s. DEFAULT %d", ED(camera) == 2 ? (ks == KA_UP ? "HIGHER THAN THE KART'S VIEW" : ks == KA_FWD ? "AHEAD OF THE KART'S VIEW" : "LOOKS DOWN MORE")
                         : ks == KA_UP ? "ABOVE THE DRIVER'S HEAD" : ks == KA_FWD ? "AHEAD OF THE DRIVER'S HEAD"
                         : "HOW FAR IT LOOKS DOWN", fpv_default(ks));
            hint = ks == KA_KART                ? (!fpv_each_kart() ? "THESE COVER EVERY KART" : s_in_race ? "THE KART YOU ARE DRIVING. SAVED FOR THIS KART ONLY" : "LEFT AND RIGHT CHOOSE THE KART. SAVED FOR EACH KART")
                   : ks <= KA_TILT              ? khint
                   : s_sel[PAGE_KART] == KA_RESET ? (fpv_each_kart() ? "A: THIS KART BACK TO ITS DEFAULTS" : "A: BACK TO THE DEFAULTS")
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
                       ? SW_PC("OFF AFTER LAN MODE UNTIL A RESTART: EVERY CONSOLE MUST PLAY THE SAME GAME", "OFF AFTER LAN MODE UNTIL A RESTART: EVERY PLAYER MUST PLAY THE SAME GAME")
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
                   : us.state == UPD_AVAILABLE   ? SW_PC("A: DOWNLOAD AND INSTALL (THE OLD ONE IS KEPT AS .NRO.OLD)", "A: DOWNLOAD AND INSTALL")
                   : us.state == UPD_DOWNLOADING ? "YOU CAN KEEP PLAYING: CLOSE THE MENU WITH B"
                   : us.state == UPD_READY       ? (s_restart_manual ? "INSTALLED: CLOSE THE GAME AND START IT AGAIN"
                                                                     : "INSTALLED: A RESTARTS THE GAME (SAVE YOUR PROGRESS FIRST)")
                   : us.state == UPD_CHECKING    ? "CONNECTING TO GITHUB..."
                                                 : "NEEDS AN INTERNET CONNECTION";
        } else if (s_page == PAGE_HUD) {
            int n = hud_rows(), at = hud_row(s_sel[PAGE_HUD]);
            for (int i = 0; i < n; i++, ry += rh)
                mrow(x, ROW_Y(i, n), pw, rh, k, s_sel[PAGE_HUD] == i, HUD_LABELS[hud_row(i)],
                     hud_row(i) == HU_BACK ? NULL : g_set.hud_hide >> hud_row(i) & 1 ? "OFF" : "ON", false);
            hint = at == HU_BACK      ? "B: BACK"
                   : at == HU_TIME    ? "HIDDEN WHILE YOU RACE, SHOWN AGAIN AT THE FINISH"
                   : at == HU_MIRROR  ? "A REAR VIEW AT THE TOP LEFT: LOWERS THE FRAME RATE"
                   : at == HU_COINS   ? "THE COINS YOU HOLD, WHEN COINS ARE ON (GAME > COINS)"
                                      : "ON OR OFF";
        } else if (s_page == PAGE_DEBUG) {
            static const char *labels[D_COUNT] = {"SHOW FPS", "LOGGING", "PROFILER", "GPU TEST", "CLOCKS", "BACK"};
            const char *values[D_COUNT] = {FPS_NAMES[g_set.show_fps % 3], LOG_NAMES[(unsigned)g_set.logging % LOG_MODES], g_set.profiler ? "ON" : "OFF", g_set.gpu_test ? "ON" : "OFF",
                                           g_set.game_clocks ? "GAME" : "SYSTEM", NULL};
            const int n = shown_rows(PAGE_DEBUG), cur = row_item(PAGE_DEBUG, s_sel[PAGE_DEBUG]);
            for (int r = 0; r < n; r++, ry += rh) {
                int i = row_item(PAGE_DEBUG, r);
                mrow(x, ROW_Y(r, n), pw, rh, k, s_sel[PAGE_DEBUG] == r, labels[i], values[i], false);
            }
            hint = cur == D_FPS      ? "TIMES: MS PER FRAME OF GAME, GRAPHICS AND RENDER THREADS"
                   : cur == D_LOG    ? SW_PC("DEBUG LOGS ON THE SD CARD, FOR BUG REPORTS", "DEBUG LOGS IN THE LOGS FOLDER, FOR BUG REPORTS")
                   : cur == D_PROF   ? "PERFORMANCE PROFILE FOR BUG REPORTS, FROM THE NEXT START"
                   : cur == D_GPUTEST ? "COMPARES GPU SETTINGS IN RACES. NEEDS LOGGING. NEXT START"
                   : cur == D_CLOCKS ? (g_set.game_clocks ? "HANDHELD GPU 460 MHZ, FASTER LOADING. FROM THE NEXT START"
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
            const char *l2 = key_hint("A: EXIT   B: BACK");
            mtext(x + (pw - mtext_w(l2, ts)) / 2, ry + (rh - ts) / 2, ts, C_TITLE_T, C_TITLE_B, l2);
            ry += rh;
        }
#undef ROW_Y
        if (hint) {
            hint = key_hint(hint);
            /* MarioFont, as the build number; a long one is made smaller to fit the box */
            float hs = 26 * k, hw = mtext_w(hint, hs), most = pw - 48 * k;
            if (hw > most) { hs *= most / hw; hw = most; }
            mtext(x + (pw - hw) / 2, y + ph - foot + (foot - hs) / 2, hs, 0xFFFFFFFFu, 0xFFFFFFFFu, hint);
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
    if (pages < 2 || !begin(w, h, false)) return;
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
        s_tex = t;
        quad(x, BANNER_MID * k - ih / 2, iw, ih, 0xFFFFFFFFu, 0xFFFFFFFFu, SH_IMAGE, 0, 0, par);
        flush();
    }
    s_tex = s_font ? uifont_texture() : 0;
    end();
}

/* ---- the trick guide (runtime/trick.c g_trick_guide) ----
 * A page of its own over a trick battle's pause screen: how a trick jump goes (take off, trick,
 * land, combo; the signature and the scoring) and every trick with its input and points, the
 * numbers native.c's TRICKS uses. Buttons by player 1's BUTTON MAPPING. While it is closed the
 * pause screen shows "Y TRICK GUIDE" at its foot. */
#define G_CARD 0x0C1A52F0u
#define G_EDGE 0x3C78E6FFu
#define G_TEXT 0xFFFFFFFFu
#define G_SOFT 0xC8D8FFFFu
#define G_ON 0xFFD640FFu
#define G_OFF 0x2C3C78FFu
/* MarioFont text wrapped into wmax, lines lh apart ('\n' breaks); returns the height used */
static float mwrap(float x, float y, float wmax, float size, float lh, uint32_t t, uint32_t b, const char *s) {
    char line[192] = "", trial[192];
    float y0 = y;
    while (*s) {
        const char *e = s;
        while (*e && *e != ' ' && *e != '\n') e++;
        snprintf(trial, sizeof trial, "%s%s%.*s", line, line[0] ? " " : "", (int)(e - s), s);
        if (line[0] && mtext_w(trial, size) > wmax) {
            mtext(x, y, size, t, b, line), y += lh, line[0] = 0;
            continue;
        }
        memcpy(line, trial, sizeof line);
        s = e;
        if (*s == '\n') mtext(x, y, size, t, b, line), y += lh, line[0] = 0;
        if (*s) s++;
    }
    if (line[0]) mtext(x, y, size, t, b, line), y += lh;
    return y - y0;
}
/* the first Switch button (its short name) of player 1's mapping for action a (the sideways Joy-Con
 * map when player 1 holds one; a GameCube pad's own L and R), else dflt */
static const char *guide_button(int a, const char *dflt) {
    static char out[4][16];
    static int n;
    if (g_p1_gamecube && (a == GC_L || a == GC_R || a == GC_Z)) return a == GC_L ? "L" : a == GC_R ? "R" : "Z";
    bool side = g_p1_sideways;
    const uint64_t *map = side ? g_set.jmap : g_set.map;
    const SwButton *tb = side ? JOY_BUTTONS : FULL_BUTTONS;
    size_t nb = side ? sizeof JOY_BUTTONS / sizeof JOY_BUTTONS[0] : sizeof FULL_BUTTONS / sizeof FULL_BUTTONS[0];
    for (size_t i = 0; i < nb; i++)
        if (map[a] & tb[i].bit) {
            char *o = out[n = (n + 1) & 3];
            snprintf(o, sizeof out[0], "%s", tb[i].name);
            char *cut = strstr(o, " /");
            if (cut) *cut = 0;
            return o;
        }
    return dflt;
}
/* the signature's two buttons as player 1's controller has them (main_switch.c / main_win.c sig2):
 * a GameCube pad's L + R, a sideways Joy-Con's SL + SR, else ZL + ZR */
static const char *const *sig_buttons(void) {
    static const char *const GC[2] = {"L", "R"}, *const SIDE[2] = {"SL", "SR"}, *const FULL[2] = {"ZL", "ZR"};
    return g_p1_gamecube ? GC : g_p1_sideways ? SIDE : FULL;
}
/* a button: a rounded chip with its name; returns its width */
static float chip(float x, float cy, float k, const char *name, uint32_t bg) {
    float ts = 22 * k, w = mtext_w(name, ts) + 22 * k, h = 32 * k;
    round_box(x, cy - h / 2, w, h, h / 2, 0x0A0A20FFu);
    round_box(x + 2 * k, cy - h / 2 + 2 * k, w - 4 * k, h - 4 * k, h / 2 - 2 * k, bg);
    mtext(x + 11 * k, cy - ts * 0.55f, ts, G_TEXT, G_TEXT, name);
    return w;
}
/* a D-pad, s across, its pressed arms lit: bits 1 up, 2 down, 4 left, 8 right */
static float dpad(float x, float cy, float s, unsigned arms) {
    float a = s / 3, y = cy - s / 2;
    round_box(x + a - 2, y - 2, a + 4, s + 4, a * 0.35f, 0x0A0A20FFu);
    round_box(x - 2, y + a - 2, s + 4, a + 4, a * 0.35f, 0x0A0A20FFu);
    round_box(x + a, y, a, s, a * 0.3f, G_OFF);
    round_box(x, y + a, s, a, a * 0.3f, G_OFF);
    if (arms & 1) round_box(x + a, y, a, a, a * 0.3f, G_ON);
    if (arms & 2) round_box(x + a, y + 2 * a, a, a, a * 0.3f, G_ON);
    if (arms & 4) round_box(x, y + a, a, a, a * 0.3f, G_ON);
    if (arms & 8) round_box(x + 2 * a, y + a, a, a, a * 0.3f, G_ON);
    return s;
}
static void card(float x, float y, float w, float h, float k) {
    round_box(x - 2 * k, y - 2 * k, w + 4 * k, h + 4 * k, 14 * k, G_EDGE);
    round_box(x, y, w, h, 12 * k, G_CARD);
}
/* a card's heading, with a tag on the right (how the inputs go) */
static void card_head(float x, float y, float w, float k, const char *title, const char *tag) {
    mtext(x + 16 * k, y + 10 * k, 30 * k, C_TITLE_T, C_TITLE_B, title);
    if (tag) {
        float ts = 20 * k, tw = mtext_w(tag, ts) + 20 * k;
        round_box(x + w - tw - 14 * k, y + 13 * k, tw, 28 * k, 14 * k, 0x3C78E6FFu);
        mtext(x + w - tw - 4 * k, y + 16 * k, ts, G_TEXT, G_TEXT, tag);
    }
    rect(x + 14 * k, y + 50 * k, w - 28 * k, 2 * k, 0x3C78E680u);
}
/* a trick's row: its input (drawn by the caller at x), name, points on the right */
static void trick_row(float x, float y, float w, float k, float name_x, const char *name, int pts) {
    char p[16];
    snprintf(p, sizeof p, "%d", pts);
    float ts = 23 * k;
    mtext(name_x, y - ts * 0.55f, ts, G_TEXT, G_SOFT, name);
    float pw = mtext_w(p, ts) + 18 * k;
    round_box(x + w - pw - 16 * k, y - 14 * k, pw, 28 * k, 14 * k, 0xFFD64033u);
    mtext(x + w - pw - 7 * k, y - ts * 0.55f, ts, C_TITLE_T, C_TITLE_B, p);
}
/* the inputs of a row, "TAP TAP" twice the D-pad */
static float in_dpad(float x, float y, float k, unsigned arms, int times) {
    float s = 30 * k, at = x;
    for (int i = 0; i < times; i++) at += dpad(at, y, s, arms) + 6 * k;
    return at - x;
}

static void guide_steps(float x, float y, float w, float h, float k) {
    const char *jump = guide_button(MAP_JUMP, "JUMP");
    char t2[192], t3[192];
    snprintf(t2, sizeof t2, "HOLD THE D-PAD TO FLIP AND SPIN. TAP A DIRECTION TWICE FOR A SPECIAL. %s FOR RIDER TRICKS", jump);
    const char *cl = guide_button(GC_L, NULL), *cr = guide_button(GC_R, NULL); /* the default layout has no L */
    char catch_b[40];
    snprintf(catch_b, sizeof catch_b, "%s%s%s", cl ? cl : "", cl && cr ? " OR " : "", cr ? cr : "");
    snprintf(t3, sizeof t3, "LEVEL WITHIN 25 DEGREES IS PERFECT. HOLD %s JUST BEFORE TOUCHDOWN TO CATCH A SKETCHY ONE",
             catch_b[0] ? catch_b : "L OR R");
    static const char *const title[4] = {"TAKE OFF", "TRICK", "LAND IT", "COMBO"};
    const char *body[4] = {"HIT A RAMP OR A BIG DROP. TRICKS START ONCE YOU ARE UP IN THE AIR", t2, t3,
                           "LAND CLEAN, THEN TAKE OFF AGAIN WITHIN 15 SECONDS. MORE DIFFERENT TRICKS: A BIGGER MULTIPLIER"};
    float gap = 12 * k, cw = (w - 3 * gap) / 4, ch = h * 0.6f;
    for (int i = 0; i < 4; i++) {
        float cx = x + i * (cw + gap);
        card(cx, y, cw, ch, k);
        round_box(cx + 14 * k, y + 12 * k, 40 * k, 40 * k, 20 * k, G_ON);
        char n[2] = {(char)('1' + i), 0};
        mtext(cx + 34 * k - mtext_w(n, 30 * k) / 2, y + 16 * k, 30 * k, 0x102268FFu, 0x102268FFu, n);
        mtext(cx + 64 * k, y + 16 * k, 28 * k, C_TITLE_T, C_TITLE_B, title[i]);
        float by = y + 66 * k;
        if (i == 1) { /* the D-pad's turns */
            static const char *const nm[3] = {"FRONT FLIP", "BACKFLIP", "SPIN"};
            static const unsigned arms[3] = {1, 2, 4 | 8};
            for (int d = 0; d < 3; d++) {
                float rx = cx + 16 * k, ry = by + 14 * k + d * 32 * k;
                dpad(rx, ry, 28 * k, arms[d]);
                mtext(rx + 38 * k, ry - 10 * k, 20 * k, G_SOFT, G_SOFT, nm[d]);
            }
            by += 104 * k;
        } else if (i == 2) { /* the grades */
            static const char *const g[3] = {"PERFECT", "SKETCHY", "BAIL"};
            static const char *const what[3] = {"BOOST", "SLOWER", "SPIN OUT"};
            static const uint32_t col[3] = {0x2EB85CFFu, 0xE8A317FFu, 0xD8342CFFu};
            for (int d = 0; d < 3; d++) {
                float ry = by + 4 * k + d * 32 * k;
                round_box(cx + 16 * k, ry, 112 * k, 26 * k, 13 * k, col[d]);
                mtext(cx + 72 * k - mtext_w(g[d], 18 * k) / 2, ry + 3 * k, 18 * k, G_TEXT, G_TEXT, g[d]);
                mtext(cx + 140 * k, ry + 3 * k, 18 * k, G_SOFT, G_SOFT, what[d]);
            }
            by += 104 * k;
        } else if (i == 3) { /* x1 .. x10 */
            for (int d = 0; d < 5; d++) {
                char m[8];
                snprintf(m, sizeof m, "X%d", d < 4 ? d + 1 : 10);
                float bw = (cw - 32 * k - 4 * 6 * k) / 5, bx = cx + 16 * k + d * (bw + 6 * k), bh = (22 + 10 * d) * k;
                grad(bx, by + 70 * k - bh, bw, bh, 0xFFEC40FFu, 0xFF8800FFu);
                mtext(bx + bw / 2 - mtext_w(m, 18 * k) / 2, by + 74 * k, 18 * k, G_TEXT, G_TEXT, m);
            }
            by += 104 * k;
        } else { /* a ramp and the arc off it */
            float rx = cx + 20 * k, ry = by + 70 * k;
            line(rx, ry, rx + 70 * k, ry - 34 * k, 8 * k, 0x6A86D0FFu, 0x9AB6FFFFu);
            for (int d = 0; d < 9; d++) {
                float t0 = d / 9.0f, t1 = (d + 0.6f) / 9.0f, ax = rx + 70 * k, aw = cw - 120 * k;
                line(ax + t0 * aw, ry - 34 * k - 120 * k * t0 * (1 - t0), ax + t1 * aw, ry - 34 * k - 120 * k * t1 * (1 - t1), 3 * k, G_ON, G_ON);
            }
            star(rx + 70 * k + (cw - 120 * k) * 0.5f, ry - 34 * k - 30 * k - 16 * k, 14 * k, G_ON);
            by += 90 * k;
        }
        mwrap(cx + 16 * k, by, cw - 32 * k, 18 * k, 23 * k, G_TEXT, G_SOFT, body[i]);
    }
    /* the signature and the scoring, below */
    float y2 = y + ch + 12 * k, h2 = h - ch - 12 * k, w2 = (w - gap) / 2;
    const char *const *sb = sig_buttons();
    char sig[224];
    snprintf(sig, sizeof sig, "LANDED JUMPS FILL THE METER UNDER YOUR SCORE. AT READY PRESS %s + %s IN THE AIR "
             "(HOLD A DIRECTION FOR ANOTHER): A STAR, A CLEAN LANDING AND A LONG BOOST", sb[0], sb[1]);
    card(x, y2, w2, h2, k);
    card_head(x, y2, w2, k, "SIGNATURE TRICK", "METER FULL");
    float mx = x + 16 * k, my = y2 + 64 * k, mw = 150 * k;
    round_box(mx, my, mw, 24 * k, 12 * k, 0x0A0A20FFu);
    round_box(mx + 3 * k, my + 3 * k, mw - 6 * k, 18 * k, 9 * k, G_ON);
    mtext(mx + mw / 2 - mtext_w("READY", 18 * k) / 2, my + 3 * k, 18 * k, 0x102268FFu, 0x102268FFu, "READY");
    mwrap(mx, my + 36 * k, w2 - 32 * k, 19 * k, 25 * k, G_TEXT, G_SOFT, sig); /* under the pill, the card's width */
    card(x + w2 + gap, y2, w2, h2, k);
    card_head(x + w2 + gap, y2, w2, k, "SCORING", NULL);
    mwrap(x + w2 + gap + 16 * k, y2 + 64 * k, w2 - 32 * k, 19 * k, 25 * k, G_TEXT, G_SOFT,
          "A LANDING SCORES ITS TRICKS AT ONCE: MORE TURNS AND A HIGHER JUMP SCORE MORE. A TRICK AMONG YOUR LAST 5 IS WORTH LESS. "
          "THE COMBO BONUS COMES WHEN IT ENDS. A BAIL LOSES IT");
}

/* one line of text in width w: smaller letters rather than a second line */
static void fit_line(float x, float y, float w, float size, uint32_t col, const char *t) {
    float s = size;
    while (s > size * 0.6f && mtext_w(t, s) > w) s -= size * 0.05f;
    mtext(x, y + (size - s) / 2, s, col, col, t);
}

static void guide_tricks(float x, float y, float w, float h, float k) {
    float gap = 12 * k, cw = (w - gap) / 2, rh = 46 * k;
    float h1 = 52 * k + 4 * rh + 40 * k, h2 = h - h1 - 12 * k;
    /* kart turns (held) */
    card(x, y, cw, h1, k);
    card_head(x, y, cw, k, "FLIPS AND SPINS", "HOLD");
    static const struct { unsigned arms; const char *name; int pts; } TURN[4] = {
        {1, "FRONT FLIP", 100}, {2, "BACKFLIP", 100}, {4 | 8, "SPIN", 100}, {1 | 8, "FLIP AND SPIN AT ONCE", 200}};
    for (int i = 0; i < 4; i++) {
        float ry = y + 52 * k + rh * (i + 0.5f) + 4 * k;
        if (i == 2) dpad(x + 18 * k, ry, 30 * k, 4), dpad(x + 54 * k, ry, 30 * k, 8);
        else in_dpad(x + 18 * k, ry, k, TURN[i].arms, 1);
        if (i < 3) trick_row(x, ry, cw, k, x + 100 * k, TURN[i].name, TURN[i].pts);
        else mtext(x + 100 * k, ry - 12 * k, 22 * k, G_TEXT, G_SOFT, "A DIAGONAL: BOTH AT ONCE");
    }
    fit_line(x + 18 * k, y + h1 - 34 * k, cw - 36 * k, 18 * k, G_SOFT, "MORE TURNS IN ONE JUMP SCORE MORE");
    /* specials (tap, tap) */
    float sx = x + cw + gap;
    card(sx, y, cw, h1, k);
    card_head(sx, y, cw, k, "SPECIALS", "TAP TWICE");
    static const struct { unsigned arms; const char *name; int pts; } SPEC[4] = {
        {8, "BARREL ROLL", 150}, {4, "WHIP", 120}, {1, "TWISTER", 220}, {2, "BACK TWISTER", 220}};
    for (int i = 0; i < 4; i++) {
        float ry = y + 52 * k + rh * (i + 0.5f) + 4 * k;
        in_dpad(sx + 18 * k, ry, k, SPEC[i].arms, 2);
        trick_row(sx, ry, cw, k, sx + 100 * k, SPEC[i].name, SPEC[i].pts);
    }
    fit_line(sx + 18 * k, y + h1 - 34 * k, cw - 36 * k, 18 * k, G_SOFT, "TAP, THEN THE SAME WAY AGAIN");
    /* the signature, the card's width in two columns: the two signature buttons together (native.c TRICK_SIG),
     * a direction held picks one of the other four */
    float y2 = y + h1 + 12 * k;
    card(x, y2, w, h2, k);
    card_head(x, y2, w, k, "SIGNATURE", "METER FULL");
    static const struct { unsigned arms; const char *name; } SIG[5] = {
        {0, "HURRICANE"}, {1, "METEOR"}, {2, "TEMPEST"}, {4, "CYCLONE"}, {8, "TYPHOON"}};
    const char *const *sb = sig_buttons();
    float rr = (h2 - 60 * k) / 3;
    for (int i = 0; i < 6; i++) {
        float colx = i < 3 ? x : sx, ry = y2 + 56 * k + rr * (i % 3 + 0.5f), at = colx + 18 * k;
        if (i == 5) { mtext(at, ry - 10 * k, 19 * k, G_TEXT, G_SOFT, "A STAR, A CLEAN LANDING, A LONG BOOST"); break; }
        at += chip(at, ry, k, sb[0], 0xD61C1CFFu) + 4 * k;
        at += chip(at, ry, k, sb[1], 0xD61C1CFFu) + 4 * k;
        if (SIG[i].arms) dpad(at, ry, 26 * k, SIG[i].arms);
        trick_row(colx, ry, cw, k, colx + 186 * k, SIG[i].name, 600);
    }
}

/* the rider's tricks, two columns: the jump button then a direction (trick.c POSE), ZR in the air then a direction
 * (ZRK); below each, the same direction again with the button held (ZLBIG, BIG), lined up across */
static void guide_more(float x, float y, float w, float h, float k) {
    const char *jump = guide_button(MAP_JUMP, "JUMP"), *zr = guide_button(GC_R, "R");
    float gap = 12 * k, cw = (w - gap) / 2, rr = (h - 60 * k) / 11;
    typedef struct { unsigned arms; int times, again; const char *name; int pts; } Row;
    static const Row JR[10] = {
        {0, 0, 0, "SUPERMAN", 120}, {0, 0, 1, "CLIFFHANGER", 120}, {1, 1, 0, "HANDSTAND", 150}, {2, 1, 0, "LAZY BOY", 150},
        {4, 1, 0, "REVERSE SIDEWINDER", 150}, {8, 1, 0, "SIDEWINDER", 150},
        {1, 2, 0, "HELICOPTER", 280}, {2, 2, 0, "SOMERSAULT", 230}, {4, 2, 0, "CARTWHEEL", 240}, {8, 2, 0, "SURFER", 210}};
    static const Row ZRR[10] = {
        {0, 0, 0, "RODEO", 170}, {0, 0, 1, "CORKSCREW", 270},
        {1, 1, 0, "KISS OF DEATH", 180}, {2, 1, 0, "CORDOVA", 170}, {4, 1, 0, "NAC-NAC", 160}, {8, 1, 0, "CAN-CAN", 160},
        {1, 2, 0, "FLAIR", 260}, {2, 2, 0, "SWITCHEROO", 200}, {4, 2, 0, "BODY VARIAL", 250}, {8, 2, 0, "ROCK SOLID", 220}};
    for (int col = 0; col < 2; col++) {
        float cx = x + col * (cw + gap);
        const char *btn = col ? zr : jump;
        uint32_t bc = col ? 0x1C8C4AFFu : 0x7A3CD6FFu;
        card(cx, y, cw, h, k);
        card_head(cx, y, cw, k, col ? "MORE TRICKS" : "RIDER TRICKS", "PRESS IN THE AIR");
        const Row *rows = col ? ZRR : JR;
        for (int i = 0; i < 10; i++) {
            int slot = i < 6 ? i : i + 1; /* the doubles from slot 7, under a heading in slot 6 */
            float ry = y + 56 * k + rr * (slot + 0.5f), at = cx + 18 * k;
            at += chip(at, ry, k, btn, bc) + 6 * k;
            if (rows[i].again) mtext(at, ry - 10 * k, 18 * k, G_SOFT, G_SOFT, "AGAIN");
            if (rows[i].times) in_dpad(at, ry, k, rows[i].arms, rows[i].times);
            trick_row(cx, ry, cw, k, cx + 176 * k, rows[i].name, rows[i].pts);
        }
        /* the bigger ones' heading */
        float hy = y + 56 * k + rr * 6.5f;
        rect(cx + 14 * k, hy - rr * 0.5f + 2 * k, cw - 28 * k, 2 * k, 0x3C78E680u);
        mtext(cx + 18 * k, hy - 13 * k, 26 * k, C_TITLE_T, C_TITLE_B, "BIGGER ONES");
    }
}

void menu_trick_guide(int w, int h) {
    unsigned page = g_trick_guide;
    if ((!page && (!g_trick_guide_hint || g_tp_confirm)) || menu_is_open() || !begin(w, h, false)) return;
    float k = (float)h / 720.0f;
    if ((float)w / 1280.0f < k) k = (float)w / 1280.0f;
    if (!page) { /* the pause screen's foot: Y TRICK GUIDE */
        const char *hint = "TRICK GUIDE";
        float ts = 26 * k, bw = 42 * k + mtext_w(hint, ts) + 30 * k, bx = ((float)w - bw) / 2, by = (float)h - 66 * k;
        if (g_trick_cup) bx = (float)w - bw - 40 * k; /* the Grand Prix's pause screen: QUIT at the foot */
        round_box(bx, by, bw, 44 * k, 22 * k, 0x000000A0u);
        round_box(bx + 6 * k, by + 6 * k, 32 * k, 32 * k, 16 * k, 0xE8E8E8FFu);
        mtext(bx + 22 * k - mtext_w("Y", 22 * k) / 2, by + 9 * k, 22 * k, 0x202020FFu, 0x202020FFu, "Y");
        mtext(bx + 46 * k, by + 9 * k, ts, C_TITLE_T, C_TITLE_B, hint);
        end();
        return;
    }
    rect(0, 0, (float)w, (float)h, 0x000010C8u);
    float pw = 1200 * k, ph = 680 * k, px = ((float)w - pw) / 2, py = ((float)h - ph) / 2;
    panel(px, py, pw, ph, k, ""); /* the title beside the tabs, below */
    /* the pages' tabs on the title band */
    static const char *const TABS[3] = {"HOW IT WORKS", "KART TRICKS", "RIDER TRICKS"};
    float tx = px + 24 * k;
    for (int i = 0; i < 3; i++) {
        float ts = 22 * k, tw = mtext_w(TABS[i], ts) + 28 * k;
        bool on = page == (unsigned)i + 1;
        round_box(tx, py + 20 * k, tw, 34 * k, 17 * k, on ? 0xFFD640FFu : 0x1A3A90FFu);
        mtext(tx + 14 * k, py + 25 * k, ts, on ? 0x102268FFu : G_SOFT, on ? 0x102268FFu : G_SOFT, TABS[i]);
        tx += tw + 10 * k;
    }
    /* left / right arrows: PAGE, B: BACK on the right; the title centred between the tabs and those */
    const char *nav = "PAGE   B: BACK";
    float nw = mtext_w(nav, 20 * k), nx = px + pw - 24 * k - nw, ay = py + 37 * k, al = 22 * k;
    mtext(nx, py + 27 * k, 20 * k, G_SOFT, G_SOFT, nav);
    for (int d = 0; d < 2; d++) { /* <- then -> */
        float x0 = nx - 12 * k - (2 - d) * (al + 10 * k), tip = d ? x0 + al : x0, back = d ? -8 * k : 8 * k;
        line(x0, ay, x0 + al, ay, 3 * k, G_SOFT, G_SOFT);
        line(tip, ay, tip + back, ay - 7 * k, 3 * k, G_SOFT, G_SOFT);
        line(tip, ay, tip + back, ay + 7 * k, 3 * k, G_SOFT, G_SOFT);
    }
    float left = tx, right = nx - 12 * k - 2 * (al + 10 * k), ts = 40 * k, tw = mtext_w("TRICK GUIDE", ts);
    if (tw > right - left - 16 * k) ts *= (right - left - 16 * k) / tw, tw = mtext_w("TRICK GUIDE", ts);
    mtext(left + (right - left - tw) / 2, py + 35 * k - ts * 0.5f, ts, C_TITLE_T, C_TITLE_B, "TRICK GUIDE");
    float cx = px + 24 * k, cy = py + 100 * k, cw = pw - 48 * k, chh = ph - 120 * k;
    if (page == 1) guide_steps(cx, cy, cw, chh, k);
    else if (page == 2) guide_tricks(cx, cy, cw, chh, k);
    else guide_more(cx, cy, cw, chh, k);
    end();
}

/* ---- the trick track picker (runtime/trick.c trick_pick_pad: g_tp_page) ----
 * Over a trick battle's stage page: SINGLE TRACK or TRICK CUP; the four cups (their icons, the tracks in the one
 * picked); a cup's four tracks (their pictures); between a cup's rounds the next one. The pictures are
 * <trick dir>/Art/ui_cupN.rgba and ui_Tnn.rgba (tools/art8.py: width and height as two little-endian u16, then
 * RGBA8 rows), loaded the first time the picker shows. */
#define TP_TROPHY (TRICK_CUPS + TRICK_TRACKS) /* + 0..2: gold, silver, bronze (ui_trophyN.rgba) */
static GfxTex s_tp_tex[TP_TROPHY + 3];
static bool s_tp_tried;
static GfxTex tp_load(const char *dir, const char *name) {
    char path[512];
    snprintf(path, sizeof path, "%s/Art/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t h[4];
    GfxTex t = 0;
    if (fread(h, 1, 4, f) == 4) {
        int w = h[0] | h[1] << 8, hh = h[2] | h[3] << 8;
        size_t n = (size_t)w * (size_t)hh * 4;
        uint8_t *px = w > 0 && hh > 0 && w <= 1024 && hh <= 1024 ? malloc(n) : NULL;
        if (px && fread(px, 1, n, f) == n) t = gfx_tex_upload(0, GFX_RGBA8, w, hh, px);
        free(px);
    }
    fclose(f);
    return t;
}
static void tp_pics(void) {
    const char *dir = disc_overlay_trick_dir();
    if (s_tp_tried || !dir) return;
    s_tp_tried = true;
    int got = 0;
    for (int i = 0; i < TP_TROPHY + 3; i++) {
        char nm[32];
        if (i >= TP_TROPHY) snprintf(nm, sizeof nm, "ui_trophy%d.rgba", i - TP_TROPHY + 1);
        else if (i < TRICK_CUPS) snprintf(nm, sizeof nm, "ui_cup%d.rgba", i + 1);
        else snprintf(nm, sizeof nm, "ui_T%02d.rgba", i - TRICK_CUPS + 1);
        got += (s_tp_tex[i] = tp_load(dir, nm)) != 0;
    }
    printf("trick picker: %d of %d pictures\n", got, TP_TROPHY + 3);
}
/* a picture (none: a plain box), tint multiplies it */
static void tp_pic(GfxTex t, float x, float y, float w, float h, float k, uint32_t tint) {
    if (!t) { round_box(x, y, w, h, 10 * k, 0x1A3A90FFu & (0xFFFFFF00u | (tint & 0xFF))); return; }
    flush();
    s_tex = t;
    const float par[4] = {0, 0, 1, 1};
    quad(x, y, w, h, tint, tint, SH_IMAGE, 0, 0, par);
    flush();
    s_tex = s_font ? uifont_texture() : 0;
}
static void tp_text_c(float cx, float y, float size, float maxw, uint32_t t, uint32_t b, const char *s) {
    float w = mtext_w(s, size);
    if (w > maxw) size *= maxw / w, w = mtext_w(s, size);
    mtext(cx - w / 2, y, size, t, b, s);
}
/* the game's menu look: the dark blue star field, the checkered band with the title in yellow letters */
#define TP_TITLE_T 0xFFFFC8FFu
#define TP_TITLE_B 0xFFD820FFu
#define TP_NAME_T 0xFFFFFFFFu  /* a list's lines: white to pale blue; the picked one yellow to orange */
#define TP_NAME_B 0xB4CCFFFFu
static void tp_screen(int w, int h, float k, const char *title) {
    float W = (float)w, H = (float)h;
    grad(0, 0, W, H, 0x0C2470FFu, 0x061650FFu);
    float sx = 132 * k, sy = 118 * k, off = fmodf((float)g_tp_t * 0.25f * k, sx);
    for (int r = 0; (float)r * sy < H + sy; r++)
        for (int i = -1; (float)i * sx < W + sx; i++)
            star_sharp((float)i * sx + (r & 1 ? sx / 2 : 0) + off, 150 * k + (float)r * sy, 50 * k, 0x16348AFFu);
    float by = 26 * k, bh = 84 * k, cs = 12 * k;
    rect(0, by, W, bh, 0x2A48D0FFu);
    for (int r = 0; (float)r * cs < bh; r++)
        for (int i = 0; (float)i * cs < W; i++)
            if ((r + i) & 1) rect((float)i * cs, by + (float)r * cs, cs, fminf(cs, bh - (float)r * cs), 0x5C84F4FFu);
    rect(0, by + bh, W, 4 * k, 0x0A1440FFu);
    float ts = 66 * k;
    tp_text_c(W / 2, by + (bh - ts) / 2 + 2 * k, ts, W - 80 * k, TP_TITLE_T, TP_TITLE_B, title);
}
/* a panel: dark glass with a light rim (picked: the rim yellow, pulsing) */
static void tp_glass(float x, float y, float w, float h, float k, bool sel) {
    if (sel) {
        float p = 0.5f + 0.5f * sinf((float)g_tp_t * 0.12f);
        round_box(x - 7 * k, y - 7 * k, w + 14 * k, h + 14 * k, 20 * k, p > 0.5f ? 0xFFF080FFu : 0xFFC020FFu);
    }
    round_box(x - 3 * k, y - 3 * k, w + 6 * k, h + 6 * k, 16 * k, sel ? 0xFFFFFFFFu : 0x7C9CE8FFu);
    round_box(x, y, w, h, 14 * k, 0x0A1A58E8u);
}
static void tp_hint(int w, int h, float k, const char *s) {
    tp_text_c((float)w / 2, (float)h - 56 * k, 30 * k, (float)w - 80 * k, TP_NAME_T, TP_NAME_B, s);
}
/* the Grand Prix's cup page (2026-11-13: laid out as the game's own): the cups in a row, the picked one bigger in a
 * gold ring with sparkles round it; under them one framed panel in the cup's colour: the
 * cup's name top left, its four tracks listed under it, the right a picture of each track in turn (pick: a line of
 * the list picked, single track, and its picture) */
static const uint32_t TP_PANEL[TRICK_CUPS][2] = {{0x2A50D8FFu, 0x10287CFFu}, {0x2E96C4FFu, 0x1C78A8FFu},
                                                 {0x8A5CC4FFu, 0x6A40A4FFu}, {0xD27424FFu, 0xB05614FFu}};
#define TP_LIST_T 0xFFFFFFFFu /* the list's names: white (2026-11-13: yellow on the panels was hard on the eyes) */
#define TP_LIST_B 0xE6F0FFFFu
#define TP_CUPN_T 0xFFFFFFFFu /* the cup's name: white */
#define TP_CUPN_B 0xE6F0FFFFu
static void tp_sparkle(float cx, float cy, float r, uint32_t c) {
    uint32_t c0 = c & 0xFFFFFF00u;
    line(cx - r, cy, cx, cy, r * 0.12f, c0, c), line(cx, cy, cx + r, cy, r * 0.12f, c, c0);
    line(cx, cy - r * 1.4f, cx, cy, r * 0.12f, c0, c), line(cx, cy, cx, cy + r * 1.4f, r * 0.12f, c, c0);
    round_box(cx - r * 0.22f, cy - r * 0.22f, r * 0.44f, r * 0.44f, r * 0.22f, 0xFFFFFFFFu);
}
static void tp_cups(int w, float k, unsigned cup, int pick) {
    float W = (float)w, mid = W / 2, t = (float)g_tp_t;
    /* the cups */
    float sp = 228 * k, cy = 230 * k, iz = 96 * k, sz = 112 * k;
    for (int i = 0; i < TRICK_CUPS; i++) {
        bool sel = i == (int)cup;
        float cx = mid + ((float)i - (TRICK_CUPS - 1) / 2.0f) * sp;
        if (sel) {
            float R = 76 * k;
            round_box(cx - R - 3 * k, cy - R - 3 * k, 2 * R + 6 * k, 2 * R + 6 * k, R + 3 * k, 0x5A3A08FFu);
            quad(cx - R, cy - R, 2 * R, 2 * R, 0xFFE68CFFu, 0xC88A1CFFu, SH_ROUND, 0, 0, (const float[4]){R, 0, 0, 0});
            float r2 = R - 9 * k;
            quad(cx - r2, cy - r2, 2 * r2, 2 * r2, 0xB07A18FFu, 0xFFE68CFFu, SH_ROUND, 0, 0, (const float[4]){r2, 0, 0, 0});
            float r3 = R - 13 * k;
            quad(cx - r3, cy - r3, 2 * r3, 2 * r3, 0x2A0806FFu, 0x5A1A0EFFu, SH_ROUND, 0, 0, (const float[4]){r3, 0, 0, 0});
            tp_pic(s_tp_tex[i], cx - sz / 2, cy - sz / 2, sz, sz, k, 0xFFFFFFFFu);
            for (int s = 0; s < 4; s++) { /* sparkles round the ring, each twinkling in turn */
                float a = 2.4f + (float)s * 1.57f + t * 0.004f, tw = 0.35f + 0.65f * fabsf(sinf(t * 0.06f + (float)s * 1.7f));
                float rr = (s & 1 ? 40 : 28) * k * tw;
                tp_sparkle(cx + cosf(a) * (R + 4 * k), cy + sinf(a) * (R + 4 * k), rr, 0xFFF4C8FFu);
            }
        } else
            tp_pic(s_tp_tex[i], cx - iz / 2, cy - iz / 2, iz, iz, k, 0xFFFFFFFFu);
    }
    /* the panel: a dark rim, a gold line, the cup's colour */
    float px0 = 185 * k, py0 = 332 * k, pw0 = 912 * k, ph0 = 348 * k;
    round_box(px0 - 5 * k, py0 - 5 * k, pw0 + 10 * k, ph0 + 10 * k, 12 * k, 0x2A1C08FFu);
    round_box(px0 - 3 * k, py0 - 3 * k, pw0 + 6 * k, ph0 + 6 * k, 10 * k, 0xE0BC64FFu);
    round_box(px0 - 1 * k, py0 - 1 * k, pw0 + 2 * k, ph0 + 2 * k, 9 * k, 0x3A2A10FFu);
    quad(px0, py0, pw0, ph0, TP_PANEL[cup][0], TP_PANEL[cup][1], SH_ROUND, 0, 0, (const float[4]){8 * k, 0, 0, 0});
    /* the cup's name, a light line under it */
    float lx = px0 + 30 * k, lw = 410 * k, lmid = lx + lw / 2;
    tp_text_c(lmid, py0 + 16 * k, 54 * k, lw - 20 * k, TP_CUPN_T, TP_CUPN_B, TRICK_CUP_NAME[cup]);
    line(lx - 10 * k, py0 + 76 * k, lmid, py0 + 76 * k, 3 * k, 0xFFFFFF00u, 0xFFFFFFE0u);
    line(lmid, py0 + 76 * k, lx + lw + 10 * k, py0 + 76 * k, 3 * k, 0xFFFFFFE0u, 0xFFFFFF00u);
    /* the list */
    unsigned shown = pick >= 0 ? (unsigned)pick : (g_tp_t / 180) % 4;
    float ly = py0 + 96 * k, row = 60 * k;
    float fs = 42 * k; /* one size for the four (the longest fits) */
    for (int j = 0; j < 4; j++) { float nw = mtext_w(TRICK_TRACK_NAME[cup * 4 + j], 42 * k); if (nw > lw - 10 * k) fs = fminf(fs, 42 * k * (lw - 10 * k) / nw); }
    for (int j = 0; j < 4; j++) {
        float y = ly + (float)j * row;
        if (j == pick) { /* the picked line: a light bar with an arrow at its left */
            float p = 0.75f + 0.25f * sinf(t * 0.12f);
            round_box(lx - 6 * k, y + 2 * k, lw + 12 * k, row - 4 * k, 12 * k, 0xFFFFFF00u | (uint32_t)(p * 90));
            arrow(lx - 26 * k, y + 12 * k, 24 * k, row - 24 * k, 0xFFF070FFu);
        }
        tp_text_c(lmid, y + 10 * k + (42 * k - fs) / 2, fs, lw - 10 * k, TP_LIST_T, TP_LIST_B, TRICK_TRACK_NAME[cup * 4 + j]);
    }
    /* the picture: each in turn, faded across, in a cream frame */
    float pw = 406 * k, ph = pw * 204 / 288, px = px0 + pw0 - pw - 30 * k, py = py0 + (ph0 - ph) / 2;
    round_box(px - 6 * k, py - 6 * k, pw + 12 * k, ph + 12 * k, 10 * k, 0x3A2A10FFu);
    round_box(px - 4 * k, py - 4 * k, pw + 8 * k, ph + 8 * k, 8 * k, 0xF4E4AEFFu);
    tp_pic(s_tp_tex[TRICK_CUPS + cup * 4 + shown], px, py, pw, ph, k, 0xFFFFFFFFu);
    if (pick < 0) {
        unsigned ph_t = g_tp_t % 180;
        if (ph_t >= 160) { /* the next one fades in */
            uint32_t a = (uint32_t)((float)(ph_t - 160) / 20.0f * 255.0f);
            tp_pic(s_tp_tex[TRICK_CUPS + cup * 4 + (shown + 1) % 4], px, py, pw, ph, k, 0xFFFFFF00u | a);
        }
    }
}
/* a cup's QUIT? over the pause menu (trick.c cup_pause_pad) */
static void tp_confirm(int w, int h) {
    if (!begin(w, h, false)) return;
    float k = (float)h / 720.0f;
    if ((float)w / 1280.0f < k) k = (float)w / 1280.0f;
    rect(0, 0, (float)w, (float)h, 0x00000090u);
    float bw = 560 * k, bh = 300 * k, bx = ((float)w - bw) / 2, by = ((float)h - bh) / 2;
    round_box(bx - 8 * k, by - 8 * k, bw + 16 * k, bh + 16 * k, 24 * k, 0xFFFFFFFFu);
    round_box(bx - 3 * k, by - 3 * k, bw + 6 * k, bh + 6 * k, 20 * k, 0x0A1440FFu);
    grad(bx, by, bw, bh, 0x2A50D8F8u, 0x10287CF8u);
    tp_text_c(bx + bw / 2, by + 30 * k, 60 * k, bw - 40 * k, TP_TITLE_T, TP_TITLE_B, "QUIT?");
    tp_text_c(bx + bw / 2, by + 104 * k, 34 * k, bw - 40 * k, TP_NAME_T, TP_NAME_B, "ARE YOU SURE?");
    static const char *const OPT[2] = {"YES", "NO"};
    for (int i = 0; i < 2; i++) {
        bool sel = (unsigned)i == g_tp_csel;
        float ox = bx + bw / 2 + (i ? 40 : -200) * k, oy = by + 184 * k, ow = 160 * k, oh = 70 * k;
        if (sel) {
            float p = 0.5f + 0.5f * sinf((float)g_tp_t * 0.15f);
            round_box(ox - 5 * k, oy - 5 * k, ow + 10 * k, oh + 10 * k, 16 * k, p > 0.5f ? 0xFFF080FFu : 0xFFC020FFu);
        }
        round_box(ox, oy, ow, oh, 12 * k, sel ? 0x3C7CF0FFu : 0x0A1A58FFu);
        tp_text_c(ox + ow / 2, oy + 14 * k, 44 * k, ow - 20 * k, sel ? C_TITLE_T : TP_NAME_T, sel ? C_TITLE_B : TP_NAME_B, OPT[i]);
    }
    end();
}
void menu_trick_pick(int w, int h) {
    unsigned page = g_tp_page;
    if (g_tp_confirm && !menu_is_open()) { menu_font_load(); tp_confirm(w, h); return; }
    if (!page || menu_is_open()) return;
    menu_font_load();
    if (!begin(w, h, false)) return;
    tp_pics();
    float k = (float)h / 720.0f;
    if ((float)w / 1280.0f < k) k = (float)w / 1280.0f;
    float W = (float)w, H = (float)h;
    unsigned cup = g_tp_cup % TRICK_CUPS, tc = g_trick_cup ? (g_trick_cup - 1) % TRICK_CUPS : cup;
    if (page == 1) {
        tp_screen(w, h, k, "TRICK STYLE");
        static const char *const NAME[2] = {"SINGLE TRACK", "TRICK CUP"};
        static const char *const SUB[2] = {"ANY OF THE 16 TRACKS", "4 TRACKS IN A ROW, MOST POINTS WINS"};
        float cw = 520 * k, chh = 450 * k, gap = 60 * k, x0 = W / 2 - cw - gap / 2, y0 = 150 * k;
        for (int i = 0; i < 2; i++) {
            float bx = x0 + (float)i * (cw + gap);
            bool sel = g_tp_mode == (unsigned)i;
            tp_glass(bx, y0, cw, chh, k, sel);
            uint32_t tint = sel ? 0xFFFFFFFFu : 0x9098B0FFu;
            if (i == 0) { /* four tracks, two by two */
                float tw = 190 * k, th = tw * 204 / 288, tx = bx + (cw - 2 * tw - 20 * k) / 2;
                for (int j = 0; j < 4; j++)
                    tp_pic(s_tp_tex[TRICK_CUPS + j * 4 + 1], tx + (float)(j & 1) * (tw + 20 * k), y0 + 24 * k + (float)(j >> 1) * (th + 16 * k), tw, th, k, tint);
            } else { /* the four cups, two by two */
                float iz = 120 * k;
                for (int j = 0; j < 4; j++)
                    tp_pic(s_tp_tex[j], bx + cw / 2 - iz - 20 * k + (float)(j & 1) * (iz + 40 * k), y0 + 20 * k + (float)(j >> 1) * (iz + 8 * k), iz, iz, k, tint);
            }
            tp_text_c(bx + cw / 2, y0 + chh - 112 * k, 50 * k, cw - 40 * k, sel ? C_TITLE_T : TP_NAME_T, sel ? C_TITLE_B : TP_NAME_B, NAME[i]);
            tp_text_c(bx + cw / 2, y0 + chh - 52 * k, 24 * k, cw - 40 * k, TP_NAME_T, TP_NAME_B, SUB[i]);
        }
        tp_hint(w, h, k, "A: OK   B: BACK");
    } else if (page == 2 || page == 3) {
        tp_screen(w, h, k, page == 3 ? "SELECT TRACK" : "SELECT CUP");
        tp_cups(w, k, cup, page == 3 ? (int)(g_tp_track % 4) : -1);
        /* (no hint line: the Grand Prix's page has none) */
    } else if (page == 5) { /* the podium: first to third */
        unsigned pl = g_tp_place < 3 ? g_tp_place : 2;
        tp_screen(w, h, k, "CONGRATULATIONS!");
        /* confetti */
        static const uint32_t CONF[5] = {0xFFD820FFu, 0xFF5A46FFu, 0x5AE06EFFu, 0x5AA0FFFFu, 0xFFFFFFFFu};
        for (int i = 0; i < 60; i++) {
            uint32_t sd = (uint32_t)i * 2654435761u;
            float sp = (1.2f + (float)(sd >> 8 & 255) / 255.0f) * k, fx = (float)(sd >> 16 & 1023) / 1023.0f * W;
            float fy = fmodf((float)g_tp_t * sp + (float)(sd & 1023) / 1023.0f * H, H + 40 * k) - 20 * k;
            fx += sinf((float)g_tp_t * 0.05f + (float)i) * 14 * k;
            float cz = (6 + (float)(sd >> 4 & 7)) * k, sq = fabsf(sinf((float)g_tp_t * 0.08f + (float)i));
            rect(fx, fy, cz, cz * (0.3f + 0.7f * sq), CONF[i % 5]);
        }
        /* the trophy, glowing, popping in */
        float pop = g_tp_t < 30 ? (float)g_tp_t / 30.0f : 1;
        pop = 1 - (1 - pop) * (1 - pop) * (1 - pop);
        float tz = 300 * k * (0.4f + 0.6f * pop) * (1 + 0.03f * sinf((float)g_tp_t * 0.08f)), tx = W * 0.3f, ty = 150 * k + 150 * k + 54 * k; /* the glow's top level with the place's line */
        static const uint32_t GLOW[3] = {0xFFE06040u, 0xE0ECFF40u, 0xFFA06040u};
        for (int r = 3; r >= 1; r--) round_box(tx - tz / 2 - (float)r * 18 * k, ty - tz / 2 - (float)r * 18 * k, tz + (float)r * 36 * k, tz + (float)r * 36 * k, tz / 2 + (float)r * 18 * k, GLOW[pl]);
        tp_pic(s_tp_tex[TP_TROPHY + pl], tx - tz / 2, ty - tz / 2, tz, tz, k, 0xFFFFFFFFu);
        static const char *const PLACE[3] = {"1ST PLACE", "2ND PLACE", "3RD PLACE"};
        static const uint32_t PT[3] = {0xFFF4A0FFu, 0xF4F8FFFFu, 0xFFD0A0FFu}, PB[3] = {0xF0A000FFu, 0x9AA4B8FFu, 0xB0602CFFu};
        float rx = W * 0.64f;
        tp_text_c(rx, 150 * k, 72 * k, W * 0.5f, PT[pl], PB[pl], PLACE[pl]);
        tp_text_c(rx, 236 * k, 40 * k, W * 0.5f, TP_NAME_T, TP_NAME_B, TRICK_CUP_NAME[tc]);
        char s[48];
        snprintf(s, sizeof s, "POINTS %u", (unsigned)g_trick_total[0]);
        tp_text_c(rx, 288 * k, 36 * k, W * 0.5f, C_TITLE_T, C_TITLE_B, s);
        static const char *const OPT[3] = {"RACE AGAIN", "SELECT CUP", "QUIT"};
        for (int i = 0; i < 3; i++) {
            bool sel = (unsigned)i == g_tp_csel;
            float ow = 380 * k, oh = 62 * k, ox = rx - ow / 2, oy = 360 * k + (float)i * (oh + 18 * k);
            tp_glass(ox, oy, ow, oh, k, sel);
            if (sel) grad(ox + 6 * k, oy + 6 * k, ow - 12 * k, oh - 12 * k, 0x5AA0FFE8u, 0x3C7CF0E8u);
            tp_text_c(rx, oy + 12 * k, 38 * k, ow - 30 * k, sel ? C_TITLE_T : TP_NAME_T, sel ? C_TITLE_B : TP_NAME_B, OPT[i]);
        }
        tp_hint(w, h, k, "A: OK");
    } else { /* a cup's next round, a moment before it loads */
        unsigned r = g_trick_round < 4 ? g_trick_round : 3;
        tp_screen(w, h, k, TRICK_CUP_NAME[tc]);
        char s[48];
        snprintf(s, sizeof s, "ROUND %u", r + 1);
        tp_text_c(W / 2, 128 * k, 50 * k, W, C_TITLE_T, C_TITLE_B, s);
        float pw = 380 * k, ph = pw * 204 / 288, px = W / 2 - pw / 2, py = 190 * k; /* the name and points clear of the small pictures */
        round_box(px - 8 * k, py - 8 * k, pw + 16 * k, ph + 16 * k, 14 * k, 0xFFFFFFFFu);
        tp_pic(s_tp_tex[TRICK_CUPS + tc * 4 + r], px, py, pw, ph, k, 0xFFFFFFFFu);
        tp_text_c(W / 2, py + ph + 22 * k, 48 * k, W - 80 * k, TP_NAME_T, TP_NAME_B, TRICK_TRACK_NAME[tc * 4 + r]);
        /* the four rounds as small pictures */
        float tw = 120 * k, th = tw * 204 / 288, gx = W / 2 - (4 * tw + 3 * 16 * k) / 2, ty = H - 100 * k;
        for (int j = 0; j < 4; j++) {
            float bx = gx + (float)j * (tw + 16 * k);
            if (j == (int)r) round_box(bx - 5 * k, ty - 5 * k, tw + 10 * k, th + 10 * k, 8 * k, 0xFFD820FFu);
            tp_pic(s_tp_tex[TRICK_CUPS + tc * 4 + j], bx, ty, tw, th, k, j < (int)r ? 0x606070FFu : j == (int)r ? 0xFFFFFFFFu : 0xA0A0B0FFu);
        }
        snprintf(s, sizeof s, "YOUR POINTS %u", (unsigned)g_trick_total[0]);
        tp_text_c(W / 2, py + ph + 76 * k, 32 * k, W, C_TITLE_T, C_TITLE_B, s);
    }
    end();
}

/* ---- start-up screen ---- */
/* Checkered columns two squares deep at the screen edge and beside the picture, navy and stars
 * between: the loading screen's look, in the side bars of a 4:3 race. */
void menu_border(int w, int h) {
    float pw = (float)h * 4.0f / 3.0f, bar = ((float)w - pw) / 2;
    if (bar < 8 || !begin(w, h, false)) return;
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

/* The loading screen's background and title; false if the renderer isn't up. */
static bool loading_back(int w, int h) {
    if (!begin(w, h, true)) return false;
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
    s_tex = s_font ? uifont_texture() : 0;

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
