/* OpenGL 4.5 core for render_gl.c and main_win.c: Khronos' glcorearb.h prototypes as function
 * pointers, loaded once the context exists (gl_load.c; the list is gl_funcs.h). */
#pragma once
#include <stdbool.h>
#ifndef APIENTRY
#define APIENTRY __stdcall
#endif
#include <GL/glcorearb.h>
#define GLF(type, name) extern type p_##name;
#define GLO GLF
#include "gl_funcs.h"
#undef GLF
#undef GLO
#include "gl_names.h" /* #define glX p_glX */
bool gl_load(void *(*get)(const char *name));
const char *gl_missing(void); /* the first function gl_load did not find */
