#pragma once
/* What the app folder adds to the disc at start-up, shared by main_switch.c and main_win.c: the disc
 * image, the level packs in mods/<key>, mods/audio, mods/lang, mods/trick, mods/karts and mods/characters. */
#include <stdbool.h>
#include <stddef.h>

/* the disc image in dir ("." = the current folder: out is the bare name). With several, the fastest to
 * read wins: .iso / .gcm, then .ciso, then .rvz (each is checked against the supported main.dol later) */
bool find_disc(const char *dir, char *out, size_t n);
/* the mods this build carries code for, extracted to mods_dir/<key> (sys/main.dol, sys/fst.bin, files/):
 * bit n - 1 for MODS[n - 1] */
unsigned find_mods(const char *mods_dir);
/* the folder of mods_dir/audio with .ast streams (as a download's files/ folder, its contents or the
 * streams alone), NULL none */
const char *find_audio(const char *mods_dir);

typedef struct {
    const char *mods_dir, *save_dir;
    void (*screen)(const char *line1, const char *line2, float progress); /* the loading screen */
    void (*offer_save_copy)(const char *save_dir, const char *key);       /* a mod's first start */
    void (*mod_failed)(void);                                              /* NULL: only logged */
} ModsHost;

/* after disc_open: the mod the menu picked (its disc overlay and save folder), the language, the trick
 * files, mods/audio, kart and character packs, the pause art; the menu told what was found. Returns the
 * mod's key, NULL for the standard game. */
const char *mods_setup(const ModsHost *h);
