#pragma once
/* Character pack pictures (charpack_art.c). */
void charpack_art_init(void);                 /* after charpack_scan, before the game runs */
unsigned charpack_art_texture(unsigned id);   /* render thread: GL texture for a GX_SUBST_CHARPACK id, 0 the game's */
