/* What render_gl.c needs from the Windows front end (main_win.c): the window with its OpenGL 4.5
 * core context, the buffer swap and the message pump. */
#pragma once
#include <stdbool.h>

bool win_gl_create(int w, int h); /* window + context, GL functions loaded (gl_load.c); false on failure */
void win_gl_destroy(void);
void win_client_size(int *w, int *h); /* the window's drawable size now */
void win_swap(void);                  /* SwapBuffers (vsync on) */
void win_pump(void);                  /* window messages, during loops that don't run the game */

/* the folders next to the .exe (the Switch build's layout) */
#define LOG_DIR "logs"
#define CFG_DIR "config"
#define SAVE_DIR "save"
#define MODS_DIR "mods"
#define APP_TITLE "MKDD-NX"

/* launcher.c: the settings window shown before the game; false: the player closed it (quit) */
bool launcher_run(void);
