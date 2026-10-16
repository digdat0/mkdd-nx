/* The game's MarioFont letters (read from the disc) for the - menu. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Read the glyphs from the disc and upload them (main thread, GL context current). False if the
 * disc has no usable font: the menu then keeps its built-in bitmap font. */
bool uifont_load(void);
unsigned uifont_texture(void);
/* Atlas rectangle (u0, v0, u1, v1) and the letter's horizontal extent within its 32 px cell. */
bool uifont_glyph(char c, float uv[4], float *x0, float *x1);
/* A file from race2d.arc (inside MRAM.arc) on the disc, malloc'd, or NULL. */
uint8_t *race2d_read(const char *name, uint32_t *size);
/* Disc offset and size of file name inside the (uncompressed) archive at disc path arc. */
bool arc_locate(const char *arc, const char *name, uint64_t *off, uint32_t *size);
