/* C view of the vendored uam (third_party/uam, uam_api.h): GLSL in, DKSH out. One thread only, with
 * a large stack (8 MB): render_dk.c's compile worker. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
void uamc_init(void);
/* Compiles one stage (fragment or vertex). On success *dksh is a malloc'd DKSH file of *size bytes.
 * log (may be NULL) receives uam's messages, truncated to logn. */
bool uamc_compile(bool fragment, const char *glsl, uint8_t **dksh, size_t *size, char *log, size_t logn);
#ifdef __cplusplus
}
#endif
