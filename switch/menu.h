/* The − menu: settings (saved to settings.ini), button mapping and the on-screen overlay
 * (menu panel, FPS counter). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* The GameCube buttons, then the port's own button actions (MAP_FP: first person on / off) */
enum { GC_A, GC_B, GC_X, GC_Y, GC_Z, GC_L, GC_R, GC_START, GC_COUNT, MAP_FP = GC_COUNT, MAP_JUMP, MAP_CAM_RESET, MAP_COUNT };
typedef struct {
    bool wide;         /* 16:9: wider game camera, race HUD at the screen edges, menus 4:3 in the middle
                          (else 4:3 pillarboxed) */
    int show_fps;      /* 0 off, 1 FPS, 2 FPS + frame times (GAME / GX / RENDER ms) */
    int rumble;        /* 0 off, 1 low, 2 high */
    bool unlock_all;   /* every character, kart, cup and mode, this session only (never saved) */
    bool any_kart;     /* kart screen: no weight classes, every unlocked kart for any drivers */
    int char_test;     /* DEBUG CHARACTER TEST: character pack variants, 0 off, 1 players, 2 everyone (never saved) */
    int special_mode;  /* SPECIAL ITEMS: 0 normal, 1 any (every character as Petey / King Boo), 2 custom */
    int special[20];   /* custom, by character ID - 1: SPECIAL_NAMES index (9 = any) */
    int mt_sweeps;     /* MINI-TURBO SWEEPS: drift stages to blue sparks, 3 (the game's), 2 or 1 */
    int mt_speed;      /* MINI-TURBO SPEED: MT_SPEEDS index, 0 the game's */
    int mt_length;     /* MINI-TURBO LENGTH: MT_LENGTHS index, 0 the game's */
    bool mt_cpu;       /* MINI-TURBO FOR CPUS: the three above for the computer's karts too */
    int jump;          /* JUMP: 0 off, 1-3 low / mid / high hop on the JUMP button (map[MAP_JUMP]) */
    int laps;          /* LAPS: Grand Prix and Versus races, 0 the course's own, else 1-7 */
    int free_cam;      /* FREE CAMERA: 0 off, the right stick turns the race camera round the kart and
                        * when let go 1 snaps back, 2 eases back, 3 stays (CAMERA RESET: back) */
    int cam_height;    /* CAMERA HEIGHT: the chase camera's, 0-3 low / medium / regular (the game's) / high */
    int cam_dist;      /* CAMERA DISTANCE: the chase camera's, 0-4 very close / close / regular (the game's) / far / very far */
    int logging;       /* 0 disabled, 1 game, 2 game + audio, 3 audio */
    bool hd_textures;  /* use the HD texture file (HD_FILE) when there is one */
    bool characters;   /* EXTRA CHARACTERS: load mods/characters (character packs), from the next start */
    char mod[16];      /* the mod to play, from the next start: "auto" (the first found), "off" or a MODS key */
    int fov[2];        /* race camera field of view for 4:3 / 16:9: 0 normal (the game's), 1 wide, 2 wider, 3 widest, 4 wideee */
    int aniso;         /* texture filtering: 0 normal, 1-4 anisotropic 2x / 4x / 8x / 16x */
    bool border;       /* 4:3: checkered border art in the side bars during races */
    unsigned hud_hide; /* race HUD elements turned off, single player: HUD_* bits (runtime/rt.h) */
    int hud_style;     /* race HUD, single player: 0 classic (the game's), 1 modern (runtime native.c MODERN, the default) */
    bool speed_kmh;    /* speedometer in km/h; mph (the US game's) when false */
    int camera;        /* race camera, 1 and 2 player: 0 the game's, 1-3 first person, eye low / mid / high */
    int fp_forward;    /* first person: eye this far ahead of the kart's centre (settings.ini only) */
    bool profiler;     /* sampling profiler (logs/mkdd_prof.bin), from the next start */
    int docked_res;    /* docked (TV), 1080p output: DOCKED_NATIVE (the game's 640x480-ish frame), _2X, _1_5X or
                        * _AUTO (2x, stepping down while the GPU can't keep up). Values kept from when it was on/off. */
    int handheld_res;  /* handheld, 1280x720 screen: HANDHELD_NATIVE (640x480-ish), _1_25X, _1_5X (960x720, the
                        * screen's own lines) or _AUTO (1.5x, stepping down while the GPU can't keep up) */
    uint64_t map[MAP_COUNT]; /* Switch buttons (HidNpadButton_*) that press each GameCube button: the
                             * BUTTON MAPPING page's actions (menu.c ACTIONS), one per Switch button */
} Settings;
enum { DOCKED_NATIVE, DOCKED_2X, DOCKED_1_5X, DOCKED_AUTO };
enum { HANDHELD_NATIVE, HANDHELD_1_25X, HANDHELD_1_5X, HANDHELD_AUTO };

extern Settings g_set;
/* The standard layout: what every screen outside driving uses, whatever BUTTON MAPPING says */
extern const uint64_t g_map_default[MAP_COUNT];
/* HD textures converted by tools/hdtex_pack.py, in the app folder's mods/hd/ */
#define HD_FILE "hd-textures.bin"

/* Colour option, kept in its own file (save/color.ini) apart from settings.ini and the game save.
 * ACCURATE shows the image as rendered; VIVID and CUSTOM adjust it in the final blit. */
enum { COLOR_ACCURATE, COLOR_VIVID, COLOR_CUSTOM, COLOR_MODES };
typedef struct {
    int mode;
    int bright, contrast, sat; /* CUSTOM values, -10..10 */
} ColorSet;
extern ColorSet g_color;
void color_load(const char *path);
/* Gamma exponent, saturation and contrast for render_present. */
void color_adjust(float adj[3]);
/* __DATE__ " " __TIME__ of main_switch.c (rebuilt every build), shown in the menu as BUILD HHMM */
extern const char g_build_stamp[];

void settings_load(const char *path);
void settings_save(void);
/* Feed one frame of controller state. Returns true while the menu is open (the game then gets
 * a neutral pad). *quit is set when the player chose Exit. */
bool menu_input(uint64_t down, uint64_t held, bool *quit);
/* Player 1's buttons the game must not see yet: pressed in the menu (A, B, -) and still held
 * after it closed. Buttons held from before the menu opened (gas) reach the game at once. */
uint64_t menu_blocked(void);
/* The menu (or a button capture) is on screen: the game is paused meanwhile. */
bool menu_is_open(void);
/* Player chose CONTROLLERS: returns true once, the caller shows the system controller screen. */
bool menu_take_controllers(void);
/* Draw the overlay (menu if open, FPS and perf line if enabled) over the default framebuffer. */
void menu_draw(int w, int h, float fps, const char *perf);
/* Read the MarioFont letters from the (opened) disc; later calls do nothing. */
void menu_font_load(void);
/* Start-up choice in the loading screen's style: title, n options (sel highlighted), hint line. */
void menu_choice(int w, int h, const char *title, const char *const *opts, int n, int sel, const char *hint);
/* Start-up / message screen in the menu's style: logo, two lines, progress bar (< 0: none). */
void menu_loading(int w, int h, const char *line1, const char *line2, float progress);
/* The 4:3 border art in the side bars beside a w x h screen's 4:3 picture (after render_present). */
void menu_border(int w, int h);
/* Character select with character packs: the page above the grid, each player's variant bottom right. */
void menu_char_hint(int w, int h);
/* What the start-up found: the mods usable in mods/ (bit n - 1 for MODS[n - 1]) and the one this
 * session plays (0: none). */
void menu_set_mods(unsigned found, unsigned running);
/* At start-up: whether mods/characters exists and whether its characters were loaded. */
void menu_set_chars(bool found, bool running);
/* The mod the MOD setting picks among the found ones (bit mask as above): 0 none, n = MODS[n - 1]. */
unsigned mods_pick(unsigned found);
/* Open the menu (game paused) on its main page, with notice as the hint until the cursor moves. */
void menu_open(const char *notice);
void special_apply(void); /* SPECIAL ITEMS settings -> the runtime's g_special */
void cheats_apply(void);  /* the CHEATS page's game settings (special items, mini-turbo) -> the runtime */
/* 5x7 bitmap ("0"/"1" x 35, rows top first) for c, or NULL. */
const char *font_glyph(char c);
