/* The − menu: settings (saved to settings.ini), button mapping and the on-screen overlay
 * (menu panel, FPS counter). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { GC_A, GC_B, GC_X, GC_Y, GC_Z, GC_L, GC_R, GC_START, GC_COUNT };
typedef struct {
    bool wide;         /* 16:9: wider game camera, race HUD at the screen edges, menus 4:3 in the middle
                          (else 4:3 pillarboxed) */
    int show_fps;      /* 0 off, 1 FPS, 2 FPS + frame times (GAME / GX / RENDER ms) */
    int rumble;        /* 0 off, 1 low, 2 high */
    bool unlock_all;   /* every character, kart, cup and mode, this session only (never saved) */
    int logging;       /* 0 disabled, 1 game, 2 game + audio, 3 audio */
    bool hd_textures;  /* use the HD texture file (HD_FILE) when there is one */
    int fov[2];        /* race camera field of view for 4:3 / 16:9: 0 normal (the game's), 1 wide, 2 wider */
    int aniso;         /* texture filtering: 0 normal, 1-4 anisotropic 2x / 4x / 8x / 16x */
    bool border;       /* 4:3: checkered border art in the side bars during races */
    bool profiler;     /* sampling profiler (logs/mkdd_prof.bin), from the next start */
    int docked_res;    /* docked (TV): 0 native (the game's 640x480-ish frame), 1 rendered at 2x, 1080p output */
    uint64_t map[GC_COUNT]; /* Switch buttons (HidNpadButton_*) that press each GameCube button */
} Settings;

extern Settings g_set;
/* HD textures converted by tools/hdtex_pack.py, in the app folder */
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
/* The menu (or a button capture) is on screen: the game is paused meanwhile. */
bool menu_is_open(void);
/* Player chose CONTROLLERS: returns true once, the caller shows the system controller screen. */
bool menu_take_controllers(void);
/* Draw the overlay (menu if open, FPS and perf line if enabled) over the default framebuffer. */
void menu_draw(int w, int h, float fps, const char *perf);
/* Read the MarioFont letters from the (opened) disc; later calls do nothing. */
void menu_font_load(void);
/* Start-up / message screen in the menu's style: logo, two lines, progress bar (< 0: none). */
void menu_loading(int w, int h, const char *line1, const char *line2, float progress);
/* The 4:3 border art in the side bars beside a w x h screen's 4:3 picture (after render_present). */
void menu_border(int w, int h);
/* Open the menu (game paused) on its main page, with notice as the hint until the cursor moves. */
void menu_open(const char *notice);
/* 5x7 bitmap ("0"/"1" x 35, rows top first) for c, or NULL. */
const char *font_glyph(char c);
