/* The pause screen's controller picture and button icons drawn as a Switch (pause_art.c). */
#pragma once
#include <stdbool.h>

/* The controller that last pressed Start (set by main_switch.c's pad code). */
enum { PAUSE_FULL, PAUSE_LEFT, PAUSE_RIGHT, PAUSE_GC };
extern volatile int g_pause_kind;

/* Recognise the game's pictures on the disc (disc open, before the game runs). */
void pause_art_init(void);
/* render_set_subst callback: our GL texture for a recognised picture (0 = the game's). */
unsigned pause_art_texture(unsigned id);
/* render_set_below callback: the line drawn under a recognised picture (MKDD Plus's title bars). */
unsigned pause_art_below(unsigned id);
/* render_set_shift callback: the cup and course pages' "L + R: RANDOM" moved down, below the cups,
 * clear of the hint line (pause_art_below). */
float pause_art_shift(unsigned id);
/* The cup / course page's "L + R: RANDOM" drawn within the last 0.2 s (renderer, any thread). */
bool pause_art_random_shown(void);
/* The character select screen's "PRESS (Y) OR (X) FOR MORE CHARACTERS (page/pages)" (page from
 * 1; 0 without the game's font); w x h: the picture's size, twice the game's 640x480 units, as the
 * cup page draws it. */
unsigned pause_art_char_label(int side, int page, int pages, int *w, int *h, float *used);
