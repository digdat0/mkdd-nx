/* What the rest of the port needs from the renderer besides the game's frames (render.h): the
 * window, textures for the front end's pictures, and the 2D overlay the menus and loading screens
 * draw with. In render_dk.c. Main thread only. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* The window (deko3d device and swapchain) at the output size; false on failure. */
bool gfx_open(int w, int h);
void gfx_close(void);
const char *gfx_describe(void); /* backend, vendor and version, for the log */
void gfx_size(int *w, int *h);  /* output size now */
void gfx_resize(int w, int h);  /* docking: follow the output size */
void gfx_swap(void);            /* show what was drawn this frame */
void gfx_pump(void);            /* system events (appletMainLoop) during loops that don't run the game */

/* Front end pictures: a texture (0 = none) is a render_dk.c texture id, so it
 * can be passed to render_set_subst / render_set_below as is. Linear filtering, clamped, one level. */
typedef unsigned GfxTex;
enum { GFX_RGBA8, GFX_RG8 };
/* Creates (tex 0) or replaces the contents of tex with w x h texels of fmt. 0 on failure. */
GfxTex gfx_tex_upload(GfxTex tex, int fmt, int w, int h, const void *px);

/* The overlay (menu.c): instanced quads, BATCH per draw, alpha blended on the screen. Layout is the
 * UI shader's uniform block (std140): bits[i] uses x, y (glyph bits / shape << 28). */
#define GFX_UI_BATCH 64
typedef struct {
    float rect[GFX_UI_BATCH][4], col[GFX_UI_BATCH][4], col2[GFX_UI_BATCH][4], par[GFX_UI_BATCH][4];
    uint32_t bits[GFX_UI_BATCH][4];
    float screen[4];
} GfxUiBatch;
bool gfx_ui_begin(int w, int h, bool clear); /* false: no overlay (its shaders failed) */
void gfx_ui_draw(const GfxUiBatch *b, unsigned n, GfxTex tex);
void gfx_ui_end(void);
