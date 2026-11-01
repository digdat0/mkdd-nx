/* The deko3d build links SDL2 for audio only, but SDL2's Switch video driver references EGL, and
 * Mesa's EGL would clash with uam's Mesa compiler. These stand-ins fail every call; nothing calls
 * them (SDL's video subsystem is never initialised). Signatures are loose on purpose: unused args. */
#include <stdint.h>

#define EGL_NOT_INITIALIZED 0x3001
#define FAIL_PTR(name) void *name(void) { return 0; }
#define FAIL_INT(name) unsigned name(void) { return 0; }

FAIL_INT(eglBindAPI)
FAIL_INT(eglChooseConfig)
FAIL_PTR(eglCreateContext)
FAIL_PTR(eglCreatePbufferSurface)
FAIL_PTR(eglCreateWindowSurface)
FAIL_INT(eglDestroyContext)
FAIL_INT(eglDestroySurface)
FAIL_INT(eglGetConfigAttrib)
FAIL_PTR(eglGetDisplay)
FAIL_PTR(eglGetPlatformDisplay)
FAIL_PTR(eglGetProcAddress)
FAIL_INT(eglInitialize)
FAIL_INT(eglMakeCurrent)
FAIL_INT(eglQueryAPI)
FAIL_PTR(eglQueryString)
FAIL_INT(eglSwapBuffers)
FAIL_INT(eglSwapInterval)
FAIL_INT(eglTerminate)
FAIL_INT(eglWaitGL)
FAIL_INT(eglWaitNative)
int eglGetError(void) { return EGL_NOT_INITIALIZED; }
