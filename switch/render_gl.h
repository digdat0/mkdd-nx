/* GLES 3.2 renderer for decoded GX frames (port of MKDD-Recomp's mkdd-bevy/src/gxrender.rs). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

bool render_init(void);
/* Replay the latest presented GX frame (if new) into the EFB / copy targets. True if rendered. */
bool render_update(int scale);
/* Draw the XFB to the default framebuffer (w x h), letterboxed 4:3 (or stretched to 16:9 when
 * wide: the game camera is widened to match, see g_aspect_scale). */
/* 16:9: draw the 2D layer at 4:3, the race HUD at the screen edges and the rest in the middle. */
void render_set_hud43(bool on);
/* HD texture replacements (hdtex.h) on or off: takes effect next frame, every texture reloads. Stays
 * off without a texture file or BC7 support. */
void render_set_hd(bool on);
/* Anisotropic filtering level (1 = off, up to 16; clamped to what the GPU supports). */
void render_set_aniso(float level);
/* Textures currently showing an HD replacement. */
unsigned render_hd_in_use(void);
/* GPU memory of those (BC7 data). */
uint64_t render_hd_bytes(void);
/* Textures registered with gx_subst_register: the GL texture to draw instead (0 = the game's own). */
void render_set_subst(unsigned (*texture)(unsigned id));
/* Sampled GPU time of a whole loop (render_update .. present), about once a second: begin before
 * render_update, end after the menu; take returns the samples since the last call (avg / max ms). */
void render_gpu_timer_begin(void);
void render_gpu_timer_mid(void); /* between render_update and present */
void render_gpu_timer_end(void);
unsigned render_gpu_timer_take(double *avg_ms, double *max_ms, double *image_ms, double *output_ms);
uint64_t render_take_gpu_wait(void); /* ns spent waiting for the GPU since the last call */
/* adj: gamma exponent, saturation, contrast applied to the game image (1,1,1 = unchanged). */
void render_present(int w, int h, const float adj[3], bool wide);
/* Statistics of the last rendered frame. */
void render_stats(uint32_t *batches, uint32_t *verts, uint32_t *textures);
/* Compile the specialised TEV programs recorded in the cache file at path (progress called as
 * it goes), then keep appending new ones to it. */
void render_shader_cache(const char *path, void (*progress)(unsigned done, unsigned total));
/* Specialised TEV programs compiled so far and the total time spent compiling them. */
void render_shader_stats(unsigned *programs, double *compile_ms);
