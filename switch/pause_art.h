/* The pause screen's controller picture and button icons drawn as a Switch (pause_art.c). */
#pragma once

/* The controller that last pressed Start (set by main_switch.c's pad code). */
enum { PAUSE_FULL, PAUSE_LEFT, PAUSE_RIGHT, PAUSE_GC };
extern volatile int g_pause_kind;

/* Recognise the game's pictures on the disc (disc open, before the game runs). */
void pause_art_init(void);
/* render_set_subst callback: our GL texture for a recognised picture (0 = the game's). */
unsigned pause_art_texture(unsigned id);
/* render_set_below callback: the line drawn under a recognised picture (MKDD Plus's title bars). */
unsigned pause_art_below(unsigned id);
