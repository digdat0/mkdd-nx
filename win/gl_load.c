/* The GL function pointers (gl.h) */
#include <stddef.h>
#include "gl.h"
#undef GLF
#undef GLO
#define GLF(type, name) type p_##name;
#define GLO GLF
#include "gl_funcs.h"
#undef GLF
#undef GLO
static const char *s_missing = "";
const char *gl_missing(void) { return s_missing; }
bool gl_load(void *(*get)(const char *name)) {
#define GLF(type, name) if (!(p_##name = (type)get(#name))) { s_missing = #name; return false; }
#define GLO(type, name) p_##name = (type)get(#name);
#include "gl_funcs.h"
#undef GLF
#undef GLO
    return true;
}
