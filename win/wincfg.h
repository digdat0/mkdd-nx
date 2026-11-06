/* The Windows build's own settings (config/windows.ini; the game's are in settings.ini, menu.c):
 * the launcher, the window, the sound device and the keyboard. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* keyboard rows: what each key presses on the (Switch-style) controller the game reads */
enum {
    KB_A, KB_B, KB_X, KB_Y, KB_L, KB_R, KB_ZL, KB_ZR, KB_PLUS, KB_MINUS, KB_LSTICK, KB_RSTICK,
    KB_LUP, KB_LDOWN, KB_LLEFT, KB_LRIGHT, KB_RUP, KB_RDOWN, KB_RLEFT, KB_RRIGHT,
    KB_DUP, KB_DDOWN, KB_DLEFT, KB_DRIGHT, KB_COUNT
};
#define KB_KEYS 2 /* keys per row */

typedef struct {
    bool launcher;      /* show the launcher at start (Shift held at start shows it anyway) */
    bool fullscreen;    /* start borderless full screen (F11 / Alt+Enter, remembered) */
    int win_w, win_h;   /* window size, client area */
    int volume;         /* 0-100 */
    int latency;        /* sound buffers: 0 low, 1 normal, 2 high */
    char audio_dev[64]; /* waveOut device name, "" the system's default */
    char disc[260];     /* disc image picked in the launcher, "" the one found next to the .exe */
    int keys[KB_COUNT][KB_KEYS]; /* virtual-key codes, 0 none */
    /* read only: players 2-4's driving buttons as 1.8.4's launcher saved them (p2_map= ..), moved to
     * settings.ini's p2_* at start (main_win.c) and no longer written here. WIN_MAPS = MAP_COUNT (menu.h). */
#define WIN_MAPS 12
    bool pmap_set[3];
    uint64_t pmap[3][WIN_MAPS];
} WinCfg;

extern WinCfg g_win;
extern const char *const KB_NAMES[KB_COUNT];
extern const int KB_DEFAULT[KB_COUNT][KB_KEYS];

void wincfg_load(void);
void wincfg_save(void);
void wincfg_default_keys(void);
/* a key's name for the launcher ("" for 0) */
void wincfg_key_name(int vk, char *out, int n);
