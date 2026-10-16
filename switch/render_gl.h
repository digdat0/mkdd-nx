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
