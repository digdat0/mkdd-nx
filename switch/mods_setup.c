/* The app folder's additions to the disc at start-up (mods_setup.h), the same on Switch and Windows. */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "../runtime/rt.h"
#include "../runtime/plat.h"
#include "../runtime/table.h"
#include "menu.h"
#include "pause_art.h"
#include "charpack_art.h"
#include "mods_setup.h"

bool find_disc(const char *dir, char *out, size_t n) {
    static const char *EXT[] = {".iso", ".gcm", ".ciso", ".rvz"};
    static const int RANK[] = {0, 0, 1, 2};
    DIR *d = opendir(dir);
    if (!d) return false;
    bool bare = !strcmp(dir, ".");
    struct dirent *e;
    int best = 99;
    while ((e = readdir(d))) {
        const char *dot = strrchr(e->d_name, '.');
        for (int i = 0; dot && i < 4; i++)
            if (!strcasecmp(dot, EXT[i]) && RANK[i] < best) {
                if (bare) snprintf(out, n, "%s", e->d_name);
                else snprintf(out, n, "%s/%s", dir, e->d_name);
                best = RANK[i];
            }
    }
    closedir(d);
    if (best < 99) rt_log("disc: using %s", out);
    return best < 99;
}

unsigned find_mods(const char *mods_dir) {
    unsigned found = 0;
    for (unsigned i = 0; i < MOD_COUNT && i < 32; i++) {
        char dir[512], p[560];
        struct stat st;
        snprintf(dir, sizeof dir, "%s/%s", mods_dir, MODS[i].key);
        if (stat(dir, &st) != 0) continue;
        bool ok = true;
        snprintf(p, sizeof p, "%s/sys/main.dol", dir), ok = ok && stat(p, &st) == 0;
        snprintf(p, sizeof p, "%s/sys/fst.bin", dir), ok = ok && stat(p, &st) == 0;
        snprintf(p, sizeof p, "%s/files", dir), ok = ok && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
        if (ok) found |= 1u << i, rt_log("mod: found %s (%s)", dir, MODS[i].name);
        else rt_log("mod: %s needs sys/main.dol, sys/fst.bin and files/", dir);
    }
    return found;
}

const char *find_audio(const char *mods_dir) {
    static const char *const SUB[] = {"audio/files/AudioRes/Stream", "audio/AudioRes/Stream", "audio"};
    static char path[512];
    for (unsigned i = 0; i < sizeof SUB / sizeof *SUB; i++) {
        snprintf(path, sizeof path, "%s/%s", mods_dir, SUB[i]);
        DIR *d = opendir(path);
        if (!d) continue;
        unsigned n = 0;
        struct dirent *e;
        while ((e = readdir(d))) {
            const char *dot = strrchr(e->d_name, '.');
            if (dot && !strcasecmp(dot, ".ast")) n++;
        }
        closedir(d);
        if (n) { rt_log("mod: stereo music: %u streams in %s", n, path); return path; }
    }
    return NULL;
}

static bool is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

const char *mods_setup(const ModsHost *h) {
    const char *M = h->mods_dir, *key = NULL;
    char err[512], dir[512], line[560];
    /* A mod in mods/<key>: played when it loads and the menu's MODS page picks it (from here on the disc
     * is the mod's); otherwise, or if it can't be used, the standard game (the disc is left as it was) */
    unsigned found = find_mods(M), pick = mods_pick(found);
    const char *audio = find_audio(M);
    if (found && !pick) rt_log("mod: none picked in the menu (MODS: %s); playing the standard game", g_set.mod);
    if (pick) {
        const ModInfo *mi = &MODS[pick - 1];
        snprintf(dir, sizeof dir, "%s/%s", M, mi->key);
        snprintf(line, sizeof line, "LOADING %s", mi->name);
        for (char *t = line; *t; t++) *t = (char)toupper((unsigned char)*t);
        h->screen(line, NULL, 0.1f);
        if (audio && g_set.audio && mod_is_plus(pick)) disc_overlay_set_streams(audio);
        if (disc_overlay(dir, err, sizeof err)) {
            rt_log("mod: playing %s", mi->name);
            key = mi->key;
            snprintf(dir, sizeof dir, "%s/%s", h->save_dir, mi->key);
            snprintf(line, sizeof line, "%s/slotA.bin", dir);
            plat_mkdirs_for(line);
            h->offer_save_copy(dir, mi->key);
            memcard_set_path(line);
        } else {
            rt_log("mod: %s; playing the standard game", err);
            found &= ~(1u << (pick - 1)); /* failed to load: not offered */
            pick = 0;
            if (h->mod_failed) h->mod_failed();
        }
    }
    /* the trick competition (the mode page's TRICKS), standard game only: mods/trick/tracks/Course/<arena>.arc
     * added as Course/Trick/ (swapped in for a trick competition's courses), mods/trick/files replacing
     * the image's (the mode page's menu.arc with its fourth row); both built by the trick tools */
    static char trick_dir[512], trick_files[512];
    snprintf(trick_dir, sizeof trick_dir, "%s/trick/tracks", M);
    snprintf(trick_files, sizeof trick_files, "%s/trick/files", M);
    snprintf(dir, sizeof dir, "%s/trick/tracks/Course", M);
    bool xtra = !pick && is_dir(dir);
    if (xtra) {
        disc_overlay_set_trick(trick_dir);
        if (is_dir(trick_files)) disc_overlay_set_extra(trick_files);
    }
    /* MODS > LANGUAGE: a PAL disc's language files from mods/lang, for the standard game only (a level
     * pack's menus and course names are its own, in English) */
    static char lang_dir[512];
    unsigned langs = langs_find(M);
    int lang = pick ? 0 : langs_pick(langs);
    if (pick && langs_pick(langs)) rt_log("lang: %s with a level pack: the game stays in English", LANG_NAMES[langs_pick(langs)]);
    if (lang) {
        snprintf(lang_dir, sizeof lang_dir, "%s/lang/%s", M, LANG_NAMES[lang]);
        disc_overlay_set_language(lang_dir, LANG_NAMES[lang]);
        if (!disc_overlay(NULL, err, sizeof err)) {
            rt_log("lang: %s; the game stays in English", err);
            disc_overlay_set_language(NULL, NULL); /* the trick files still go on without it */
            lang = 0;
        }
    }
    if (xtra && !disc_overlay_trick() && !disc_overlay(NULL, err, sizeof err)) rt_log("trick: %s", err);
    if (lang && !disc_overlay_language()) lang = 0;
    g_language = (uint32_t)lang;
    menu_set_langs(langs, lang);
    menu_set_mods(found, pick);
    static char karts[512], chars[512];
    snprintf(karts, sizeof karts, "%s/karts", M);
    bool kf = is_dir(karts);
    if (kf && g_set.karts) kartpack_scan(karts);
    menu_set_karts(kf, kf && g_set.karts);
    menu_set_audio(audio != NULL, disc_overlay_stream_count() != 0);
    pause_art_init(); /* the pause screen's controller picture */
    /* driver variants, unless EXTRA CHARACTERS is off (then nothing of them is read) */
    snprintf(chars, sizeof chars, "%s/characters", M);
    bool cf = is_dir(chars);
    if (cf && g_set.characters) charpack_scan(chars);
    menu_set_chars(cf, cf && g_set.characters);
    charpack_art_init();
    charvoice_preload();
    return key;
}
