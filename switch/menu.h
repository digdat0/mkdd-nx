/* The − menu: settings (saved to settings.ini), button mapping and the on-screen overlay
 * (menu panel, FPS counter). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { GC_A, GC_B, GC_X, GC_Y, GC_Z, GC_L, GC_R, GC_START, GC_COUNT };
typedef struct {
    bool wide;         /* 16:9: wider game camera, race HUD at the screen edges, menus 4:3 in the middle
                          (else 4:3 pillarboxed) */
    bool show_fps;
    int logging;       /* 0 disabled, 1 game, 2 game + audio, 3 audio */
    uint64_t map[GC_COUNT]; /* Switch buttons (HidNpadButton_*) that press each GameCube button */
} Settings;

extern Settings g_set;

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
/* Draw the overlay (menu if open, FPS if enabled) over the default framebuffer w x h. */
void menu_draw(int w, int h, float fps);
/* 5x7 bitmap ("0"/"1" x 35, rows top first) for c, or NULL. */
const char *font_glyph(char c);
