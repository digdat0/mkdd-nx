/* Specialised TEV fragment shaders: GLSL ES 3.20 source with the stage configuration baked in. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../runtime/gx.h"

/* Key of everything tev_gen_fs bakes in (stage setup, swap tables, alpha test, fog mode). */
uint64_t tev_shader_key(const TevState *t);
/* Writes the fragment shader for t's configuration; returns its length (0 if cap is too small). */
size_t tev_gen_fs(const TevState *t, char *dst, size_t cap);
