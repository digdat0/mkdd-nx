/* In-game updater (− menu > UPDATE): asks GitHub for the latest release (its version, and
 * MKDD-NX.nro's size and SHA-256; else the release's update.txt), downloads the .nro next to the
 * running one, checks it, swaps it in (the old one kept as .nro.old) and relaunches. Runs on its own thread; the menu
 * polls update_status. Release side: tools/release.sh. */
#pragma once
#include <stdbool.h>

enum {
    UPD_IDLE,        /* nothing done yet */
    UPD_CHECKING,
    UPD_CURRENT,     /* the release is this build */
    UPD_AVAILABLE,   /* a newer (or rebuilt) release: update_start installs it */
    UPD_DOWNLOADING,
    UPD_READY,       /* installed: relaunch to run it */
    UPD_ERROR,
};

typedef struct {
    int state;
    float progress;    /* UPD_DOWNLOADING: 0..1 */
    char version[24];  /* the release's version (UPD_AVAILABLE / DOWNLOADING / READY / CURRENT) */
    char msg[64];      /* UPD_ERROR: what went wrong, menu font letters */
} UpdateStatus;

/* The running .nro (argv[0] from the loader, else the app folder's MKDD-NX.nro). */
void update_init(const char *nro_path);
/* The version this build carries (the NACP's). */
const char *update_version(void);
void update_status(UpdateStatus *out);
/* Check for a release (UPD_IDLE / CURRENT / AVAILABLE / ERROR), or install the available one. */
void update_start(void);
/* After UPD_READY: ask the loader to start the new .nro when this one exits. False when the
 * loader can't (then the player closes and starts the game again). */
bool update_relaunch(void);
