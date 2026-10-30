/* Character packs: extra drivers on top of the game's 20, as variants (Metal Mario on Mario's slot).
 *
 * In mods/characters, two layouts:
 *  - one folder per character with a character.ini (name, base, author, source; tools/charpack.py
 *    makes them): driver.bmd (required), driver_200.bmd and *.btp face animations, face.bti,
 *    map.bti, name.bti;
 *  - the files/ tree a pack ships for the MKDD Patcher, as mods/characters/files or
 *    mods/characters/<pack>/files: each driver folder with a model,
 *    files/MRAM.arc/mram/driver/<base>/driver.bmd, is one variant of that base, its pictures in
 *    race2d.arc and menu.arc. Names: names.txt next to files/ ("nokonoko=SHY GUY"), Crow8028's
 *    Character Texture Pack's own (recognised by its contents), else "KOOPA 2"; the author from the
 *    pack's modinfo.ini.
 * A variant has its base's stats and animations, its own model, face animations and pictures
 * (charpack_art.c makes the missing map icon and name tag), and its own voice when it ships one
 * (voice/, or a one-character pack's audio_waves/: charvoice.c), else its base's.
 *
 * The game never sees them. The character select screen (native.c SceneMenu::character) shows pages:
 * page 1 the game's 20, then the variants 20 a page in folder name order (layout; the last page's
 * empty cells checkered); Y and X turn them. A variant plays as its base: picked, the select
 * screen chooses the base's cell (native.c), so the race, its partner and the computer players are
 * the base's. Each pick is a character of its own: the picks are kept per team and driver (the
 * game's chosen cells, the base's), and only the very pick shown in a cell makes it taken there
 * (charpack_cell_taken), so Mario, Metal Mario and Mario's other variants can all race together.
 * While a DriverModel is built for a player's driver, its loads of "Driver/<base>/<file>" from
 * MRAM.arc come from that team and driver's pick when it is a variant. The pictures and the voice
 * follow the character: Mario's are the variant's when the Mario loaded last (or shown on the select
 * screen) is one.
 *
 * The files are read once each from the SD card into MEM1 above the game's 24 MiB (gc_core.h
 * RAM_SIZE): the models the game builds from them point into them. They stay until a select
 * screen opens anew (charpack_pool_reset), when no model built from them is left: the 8 MiB (3 of
 * them the select screen's models' heap) need hold only one visit's and the race's after it. */
#include <ctype.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "charpack.h"
#include "gx.h"

#define CP_DRIVERS "MRAM.arc/mram/driver"
#define CP_ANIMS "ARAM.arc/aram/driveranm"
#define POOL_LO 0x81800000u
#define POOL_HI 0x82000000u
#define VAR_MAX CP_VARIANTS
#define FILE_MAX 4096 /* each file asked for, there or not */

/* The game's driver folders (ResMgr::getDriverName), lower case, in the select screen's grid order
 * (column by column, top row first): SceneMenu's cell numbers are these indices. */
static const char *const BASES[CP_BASES] = {
    "mario", "luigi", "peach", "daisy", "yoshi", "catherine", "babymario", "babyluigi", "kinopio", "kinopico",
    "nokonoko", "patapata", "dk", "dkjr", "koopa", "koopajr", "wario", "waluigi", "bosspakkun", "kingteresa",
};
/* character.ini's base= values */
static const char *const BASE_IDS[CP_BASES] = {
    "mario", "luigi", "peach", "daisy", "yoshi", "birdo", "baby_mario", "baby_luigi", "toad", "toadette",
    "koopa", "paratroopa", "donkey_kong", "diddy_kong", "bowser", "bowser_jr", "wario", "waluigi", "petey", "king_boo",
};
static const char *const BASE_NAMES[CP_BASES] = {
    "MARIO", "LUIGI", "PEACH", "DAISY", "YOSHI", "BIRDO", "BABY MARIO", "BABY LUIGI", "TOAD", "TOADETTE",
    "KOOPA", "PARATROOPA", "DONKEY KONG", "DIDDY KONG", "BOWSER", "BOWSER JR.", "WARIO", "WALUIGI", "PETEY PIRANHA", "KING BOO",
};
/* Crow8028's Character Texture Pack (mkdd.org) has no names of its own. */
static const char *const CROW_NAMES[CP_BASES] = {
    "METAL MARIO", "GOOIGI", "BRIDE PEACH", "HOLIDAISY", "BLUE YOSHI", "YELLOW BIRDO", "DIAPER BABY MARIO", "BABY WALUIGI",
    "BLUE TOAD", "PURPLE TOADETTE", "GOLD KOOPA", "BLUE PARATROOPA", "WHITE DK", "BLACK DIDDY KONG", "DARK LAVA BOWSER",
    "SHADOW MARIO", "BULL WARIO", "BUS DRIVER WALUIGI", "PUTRID PETEY", "PINK BOO",
};
/* The Puyo Pack (loverosethefourth) names its characters only on their name tags. */
static const char *const PUYO_NAMES[CP_BASES] = {
    "ARLE", "RINGO", NULL, NULL, "SIG", "AMITIE", NULL, NULL, NULL, NULL, NULL, NULL, "FELI", "LEMRES", "SATAN", "DRACO",
};

typedef struct {
    char dir[160];   /* character.ini layout: its folder; else the pack's files/ folder */
    char folder[24]; /* MKDD Patcher layout: its driver folder's name as on the SD card ("" otherwise) */
    char name[32], author[32];
    uint8_t base, cell, page; /* where the select screen shows it (layout) */
    bool pinned;        /* character.ini slot= */
    int8_t anim;        /* the character whose animations move it (its skeleton's), -1 its base's */
    char voice;         /* its sounds (charvoice.c): 'v' in dir/voice, 'a' in the pack's audio_waves, 0 none */
} Variant;
static Variant s_var[VAR_MAX];
static unsigned s_nvar;

typedef struct { char path[224]; uint32_t addr, size; } Loaded; /* addr 0: no such file */
static Loaded s_file[FILE_MAX];
static unsigned s_nfile;
static uint32_t s_pool = POOL_LO, s_pool_top = POOL_HI; /* files from the bottom, reserved blocks from the top */

/* Per team and driver, the pick: the game's chosen cell (the base's; -1 none), the variant (-1 the
 * game's own) and the cell it was picked in. Per cell the one shown now (select screen; in a race per
 * character, the loaded driver's; CP_DIM the game's, dimmed); per character the one its model and
 * name tag show (charpack_model: native.c's choice of the select screen's model blocks); per player
 * a pick made in another character's cell, waiting for the game to choose the base (charpack_pending). */
typedef struct { int cell, v, at; } Pick;
#define NO_PICK {-1, -1, -1}
static Pick s_slot[4][2] = {{NO_PICK, NO_PICK}, {NO_PICK, NO_PICK}, {NO_PICK, NO_PICK}, {NO_PICK, NO_PICK}};
#define NO_PENDING (-9) /* s_pending: nothing (-1: the game's own, chosen on purpose) */
static int s_show[CP_BASES], s_model[CP_BASES], s_pending[4] = {NO_PENDING, NO_PENDING, NO_PENDING, NO_PENDING}, s_pending_cell[4];
static int s_cursor[4] = {-1, -1, -1, -1};
static int s_page, s_pages = 1;
static uint64_t s_select_gx; /* the game frame (gx_frame_count) of the select screen's last frame */
static bool s_select_seen;

volatile uint32_t g_char_cycle;

static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}
static bool is_file(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}
static void upper(char *s) {
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static unsigned rd16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }

int charpack_base(const char *folder, size_t n) {
    for (int i = 0; i < CP_BASES; i++)
        if (strlen(BASES[i]) == n && !strncasecmp(BASES[i], folder, n)) return i;
    return -1;
}
/* character.ini's base: its id ("koopa" is Koopa Troopa), the name on screen, or the game's folder */
static int base_from_ini(const char *s) {
    for (int i = 0; i < CP_BASES; i++)
        if (!strcasecmp(BASE_IDS[i], s) || !strcasecmp(BASE_NAMES[i], s)) return i;
    return charpack_base(s, strlen(s));
}

/* key=value (or "key = value") from an ini-style file; false when absent */
static bool ini_get(const char *path, const char *key, char *out, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[256];
    bool found = false;
    size_t kl = strlen(key);
    while (!found && fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (strncasecmp(s, key, kl)) continue;
        char *v = s + kl;
        while (*v == ' ' || *v == '\t') v++;
        if (*v++ != '=') continue;
        while (*v == ' ' || *v == '\t') v++;
        char *e = v + strlen(v);
        while (e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        if (*v) snprintf(out, n, "%s", v), found = true;
    }
    fclose(f);
    return found;
}

static Variant *add_variant(int b, const char *dir, const char *folder) {
    if (s_nvar == VAR_MAX) return NULL;
    Variant *v = &s_var[s_nvar];
    memset(v, 0, sizeof *v);
    snprintf(v->dir, sizeof v->dir, "%s", dir);
    snprintf(v->folder, sizeof v->folder, "%s", folder);
    v->base = v->cell = (uint8_t)b;
    v->anim = -1;
    return v;
}

/* ---- skeletons: a variant with another character's ----
 * The packs seen so far (the Puyo Pack, Crow8028's Shadow Mario on Bowser Jr.) put a model with
 * Mario's skeleton on another character, with Mario's animations copied into that character's
 * folders (byte for byte the game's; only the .bas sound timings, which name the character's
 * voice, stay the character's). So a variant whose skeleton is another character's moves with that
 * character's animations (native.c: the select screen's and the race's), whether its pack's files
 * are there or not (tools/charpack.py doesn't export them); one shipping animations for a skeleton
 * no character has is left out. Skeletons compare by joint names, the first (the model's root,
 * "mario_all_root") aside. */
static char *s_skel[CP_BASES]; /* each character's joint names, "skl_root|leg_l1|...", once read */

/* the joint names of a JNT1 section (in memory) after the first, '|'-separated, malloc'd; NULL none */
static char *jnt1_names(const uint8_t *j, uint32_t n) {
    if (n < 0x18) return NULL;
    uint32_t t = rd32(j + 0x14);
    if (t + 4 > n) return NULL;
    unsigned cnt = rd16(j + t);
    char *out = malloc(cnt * 32 + 1);
    if (!out) return NULL;
    size_t len = 0;
    out[0] = 0;
    for (unsigned i = 1; i < cnt && t + 8 + 4 * i <= n; i++) {
        uint32_t o = t + rd16(j + t + 4 + 4 * i + 2);
        for (unsigned k = 0; o + k < n && j[o + k] && k < 30; k++) out[len++] = (char)j[o + k];
        out[len++] = '|';
        out[len] = 0;
    }
    return out;
}
static char *bmd_skeleton(const uint8_t *b, uint32_t n) {
    if (n < 0x20 || memcmp(b, "J3D2", 4)) return NULL;
    for (uint32_t o = 0x20; o + 8 <= n;) {
        uint32_t size = rd32(b + o + 4);
        if (!memcmp(b + o, "JNT1", 4)) return jnt1_names(b + o, size <= n - o ? size : n - o);
        if (size < 8) break;
        o += size;
    }
    return NULL;
}
/* the same from a file on the SD card, reading only the section headers and JNT1 */
static char *file_skeleton(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t h[8];
    char *out = NULL;
    if (fread(h, 1, 4, f) == 4 && !memcmp(h, "J3D2", 4))
        for (long o = 0x20; !out && !fseek(f, o, SEEK_SET) && fread(h, 1, 8, f) == 8;) {
            uint32_t size = rd32(h + 4);
            if (size < 8 || size > (4u << 20)) break;
            if (!memcmp(h, "JNT1", 4)) {
                uint8_t *j = malloc(size);
                if (j && !fseek(f, o, SEEK_SET) && fread(j, 1, size, f) == size) out = jnt1_names(j, size);
                free(j);
                break;
            }
            o += size;
        }
    fclose(f);
    return out;
}
static const char *base_skeleton(int b) {
    if (!s_skel[b]) {
        uint32_t n = 0;
        uint8_t *bmd = charpack_orig(b, "driver.bmd", &n);
        s_skel[b] = bmd ? bmd_skeleton(bmd, n) : NULL;
        free(bmd);
        if (!s_skel[b]) s_skel[b] = calloc(1, 1); /* none: matches nothing */
    }
    return s_skel[b];
}
/* Which character's animations move a variant of b with the model at bmd: -1 b's (its skeleton is
 * b's, or no character's and it has none of its own: as before, the joints follow by place), the
 * character whose skeleton it is (the first), -2 none (its own animations, a skeleton no character
 * has: left out). */
static int variant_anim(const char *bmd, int b, bool has_anims, const char *where) {
    char *sk = file_skeleton(bmd);
    int k = -2;
    if (sk && sk[0]) {
        if (!strcmp(base_skeleton(b), sk)) k = b;
        for (int i = 0; i < CP_BASES && k == -2; i++)
            if (!strcmp(base_skeleton(i), sk)) k = i;
    }
    free(sk);
    if (k == -2 && has_anims) rt_log("charpack: %s has its own animations and a skeleton no character has: left out", where);
    else if (k >= 0 && k != b) rt_log("charpack: %s has %s's skeleton: it moves as %s", where, BASE_NAMES[k], BASE_NAMES[k]);
    return k == -2 ? (has_anims ? -2 : -1) : k == b ? -1 : k;
}
static void added(Variant *v, const char *where) {
    upper(v->name), upper(v->author);
    s_nvar++;
    rt_log("charpack: variant %u: %s (on %s)%s%s, %s", s_nvar, v->name, BASE_NAMES[v->base], v->author[0] ? " by " : "", v->author, where);
}
static void default_name(Variant *v) {
    unsigned same = 2;
    for (unsigned i = 0; i < s_nvar; i++) same += s_var[i].base == v->base;
    snprintf(v->name, sizeof v->name, "%s %u", BASE_NAMES[v->base], same);
}

/* a character folder: character.ini and driver.bmd */
static void scan_character(const char *dir) {
    char ini[200], p[224], base[32] = "";
    snprintf(ini, sizeof ini, "%s/character.ini", dir);
    int b = ini_get(ini, "base", base, sizeof base) ? base_from_ini(base) : -1;
    snprintf(p, sizeof p, "%s/driver.bmd", dir);
    if (b < 0 || !is_file(p)) {
        rt_log("charpack: %s: %s", dir, b < 0 ? "no base= (or an unknown one) in character.ini" : "no driver.bmd");
        return;
    }
    char anims[200];
    snprintf(anims, sizeof anims, "%s/anims", dir);
    int anim = variant_anim(p, b, is_dir(anims), dir);
    if (anim == -2) return;
    Variant *v = add_variant(b, dir, "");
    if (!v) return;
    v->anim = (int8_t)anim;
    snprintf(p, sizeof p, "%s/voice", dir);
    if (is_dir(p)) v->voice = 'v';
    if (!ini_get(ini, "name", v->name, sizeof v->name)) default_name(v);
    ini_get(ini, "author", v->author, sizeof v->author);
    char slot[32];
    int cell = ini_get(ini, "slot", slot, sizeof slot) ? base_from_ini(slot) : -1;
    if (cell >= 0) v->cell = (uint8_t)cell, v->pinned = true;
    added(v, dir);
}

/* pack: the folder holding files/ (MKDD Patcher layout) */
static void scan_pack(const char *pack) {
    char root[160], drivers[192], p[256], author[32] = "", modname[48] = "";
    snprintf(root, sizeof root, "%s/files", pack);
    snprintf(drivers, sizeof drivers, "%s/" CP_DRIVERS, root);
    DIR *d = opendir(drivers);
    if (!d) return;
    /* the driver folders, read once (libnx can't rewind a directory) */
    char folders[CP_BASES][24];
    unsigned nf = 0;
    struct dirent *e;
    while ((e = readdir(d)) && nf < CP_BASES)
        if (charpack_base(e->d_name, strlen(e->d_name)) >= 0) snprintf(folders[nf++], sizeof folders[0], "%s", e->d_name);
    closedir(d);
    rt_log("charpack: %s: %u driver folders", drivers, nf);
    snprintf(p, sizeof p, "%s/modinfo.ini", pack);
    ini_get(p, "author", author, sizeof author);
    ini_get(p, "modname", modname, sizeof modname);
    bool puyo = !strcasecmp(modname, "Puyo Pack");
    /* Crow8028's pack: all 20 drivers, Toadette's hair, Shadow Mario's animations */
    snprintf(p, sizeof p, "%s/kinopico/kinoko_hair.bmd", drivers);
    char anims_jr[224];
    snprintf(anims_jr, sizeof anims_jr, "%s/" CP_ANIMS "/koopajr", root);
    bool crow = nf == CP_BASES && is_file(p) && is_dir(anims_jr);
    for (unsigned f = 0; f < nf; f++) {
        const char *folder = folders[f];
        int b = charpack_base(folder, strlen(folder));
        snprintf(p, sizeof p, "%s/%s/driver.bmd", drivers, folder);
        if (!is_file(p)) {
            rt_log("charpack: %s has no driver.bmd", p);
            continue;
        }
        /* its own driving animations (Shadow Mario's skeleton): it moves as its skeleton's character */
        char anims[256];
        snprintf(anims, sizeof anims, "%s/" CP_ANIMS "/%s", root, folder);
        int anim = variant_anim(p, b, is_dir(anims), p);
        if (anim == -2) continue;
        Variant *v = add_variant(b, root, folder);
        if (!v) return;
        v->anim = (int8_t)anim;
        char waves[200];
        snprintf(waves, sizeof waves, "%s/audio_waves", pack);
        if (nf == 1 && is_dir(waves)) v->voice = 'a'; /* a pack's sounds are its one character's */
        char names[200];
        snprintf(names, sizeof names, "%s/names.txt", pack);
        if (!ini_get(names, folder, v->name, sizeof v->name)) {
            if (crow) snprintf(v->name, sizeof v->name, "%s", CROW_NAMES[b]);
            else if (puyo && PUYO_NAMES[b]) snprintf(v->name, sizeof v->name, "%s", PUYO_NAMES[b]);
            else default_name(v);
        }
        snprintf(v->author, sizeof v->author, "%s", author);
        snprintf(p, sizeof p, "%s/%s", drivers, folder);
        added(v, p);
    }
}

/* Cells for the variants: in folder name order, the first page's third row (cells 20-29: the game's
 * 20 above), then page after page all 30 cells, whatever character they play as: a cell is only a
 * place to pick one (the hint names its base). Only the last page has empty cells. character.ini's
 * slot= is not used. */
static void layout(void) {
    for (unsigned i = 0; i < s_nvar; i++) {
        unsigned j = i < CP_ROW ? i + CP_BASES : i - CP_ROW;
        s_var[i].page = (uint8_t)(i < CP_ROW ? 0 : 1 + j / CP_CELLS), s_var[i].cell = (uint8_t)(i < CP_ROW ? j : j % CP_CELLS);
    }
}

void charpack_scan(const char *dir) {
    s_nvar = 0;
    for (int b = 0; b < CP_BASES; b++)
        s_show[b] = s_model[b] = -1;
    scan_pack(dir); /* mods/characters/files */
    DIR *d = opendir(dir);
    static char subs[VAR_MAX][64];
    unsigned n = 0;
    struct dirent *e;
    while (d && (e = readdir(d)) && n < VAR_MAX)
        if (e->d_name[0] != '.' && strcasecmp(e->d_name, "files")) snprintf(subs[n++], sizeof subs[0], "%s", e->d_name);
    if (d) closedir(d);
    /* folder name order, so the pages keep their order */
    for (unsigned i = 1; i < n; i++)
        for (unsigned j = i; j > 0 && strcasecmp(subs[j - 1], subs[j]) > 0; j--) {
            char t[64];
            memcpy(t, subs[j], sizeof t), memcpy(subs[j], subs[j - 1], sizeof t), memcpy(subs[j - 1], t, sizeof t);
        }
    for (unsigned i = 0; i < n; i++) {
        char sub[160], p[200];
        snprintf(sub, sizeof sub, "%s/%s", dir, subs[i]);
        snprintf(p, sizeof p, "%s/character.ini", sub);
        if (is_file(p)) scan_character(sub);
        else scan_pack(sub);
    }
    layout();
    s_pages = s_nvar ? s_var[s_nvar - 1].page + 1 : 1;
    rt_log("charpack: %u variants in %s, %d pages", s_nvar, dir, s_pages);
}

unsigned charpack_count(void) { return s_nvar; }
int charpack_variant_base(int v) { return v >= 0 && (unsigned)v < s_nvar ? s_var[v].base : -1; }
const char *charpack_name(int v) { return v >= 0 && (unsigned)v < s_nvar ? s_var[v].name : NULL; }
const char *charpack_author(int v) { return v >= 0 && (unsigned)v < s_nvar ? s_var[v].author : NULL; }
const char *charpack_base_name(int b) { return b >= 0 && b < CP_BASES ? BASE_NAMES[b] : ""; }
int charpack_anim(int v) { return v >= 0 && (unsigned)v < s_nvar ? s_var[v].anim : -1; }
int charpack_show(int b) { return b >= 0 && b < CP_BASES ? s_show[b] : -1; }
int charpack_model(int b) { return b >= 0 && b < CP_BASES ? s_model[b] : -1; }
bool charpack_has(int b) {
    for (unsigned i = 0; i < s_nvar; i++)
        if (s_var[i].base == b) return true;
    return false;
}

/* Where a variant's picture or driver file is: kind 'd' a driver file (driver.bmd, *.btp), 'f' the
 * face, 'm' the map icon, 'n' the name tag; game names the MKDD Patcher layout's file name for the
 * pictures ("mario", "Mario", "mario": charpack_art.c). */
bool charpack_path(int v, char kind, const char *game, char *out, size_t n) {
    if (v < 0 || (unsigned)v >= s_nvar) return false;
    const Variant *x = &s_var[v];
    if (!x->folder[0]) { /* character.ini layout */
        const char *f = kind == 'f' ? "face.bti" : kind == 'm' ? "map.bti" : kind == 'n' ? "name.bti" : game;
        snprintf(out, n, "%s/%s", x->dir, f);
    } else if (kind == 'd') snprintf(out, n, "%s/" CP_DRIVERS "/%s/%s", x->dir, x->folder, game);
    else if (kind == 'f') snprintf(out, n, "%s/race2d.arc/mram_race2d/timg/%s.bti", x->dir, game);
    else if (kind == 'm') snprintf(out, n, "%s/race2d.arc/mram_race2d/timg/%s_mapicon32.bti", x->dir, game);
    else snprintf(out, n, "%s/SceneData/English/menu.arc/menu/timg/chaname_%s.bti", x->dir, game);
    return true;
}

int charpack_cell_variant(int c) {
    for (unsigned i = 0; c >= 0 && c < CP_CELLS && i < s_nvar; i++)
        if (s_var[i].page == s_page && s_var[i].cell == c) return (int)i;
    return -1;
}
int charpack_real_base(int c) {
    int v = charpack_cell_variant(c);
    return v >= 0 ? s_var[v].base : c < CP_BASES ? c : -1;
}
void charpack_pending(int player, int v, int cell) {
    if (player >= 0 && player < 4) s_pending[player] = v, s_pending_cell[player] = cell;
}
void charpack_borrow(int b, int v) {
    if (b >= 0 && b < CP_BASES) s_model[b] = v;
}
int charpack_slot(int t, int k, int *v) {
    if (t < 0 || t >= 4 || k < 0 || k >= 2 || s_slot[t][k].cell < 0) return *v = -1, -1;
    return *v = s_slot[t][k].v, s_slot[t][k].cell;
}
int charpack_slot_shown(int t, int k) {
    if (t < 0 || t >= 4 || k < 0 || k >= 2) return -1;
    const Pick *x = &s_slot[t][k];
    return x->cell >= 0 && x->v >= 0 && x->at != x->cell && x->at >= 0 && x->at < CP_BASES && charpack_cell_variant(x->at) == x->v ? x->at : -1;
}
bool charpack_variant_taken(int v) {
    for (int t = 0; t < 4; t++)
        for (int k = 0; k < 2; k++)
            if (s_slot[t][k].cell >= 0 && s_slot[t][k].v == v) return true;
    return false;
}
bool charpack_cell_taken(int c) {
    if (c < 0 || c >= CP_CELLS) return false;
    int v = charpack_cell_variant(c);
    for (int t = 0; t < 4; t++)
        for (int k = 0; k < 2; k++) {
            const Pick *x = &s_slot[t][k];
            if (x->cell >= 0 && (v >= 0 ? x->v == v : x->v < 0 && x->cell == c)) return true;
        }
    return false;
}

/* The variant kart k's driver d (0 or 1) drives in place of base character b, or -1: that team
 * and driver's pick (the players' karts are the teams, in order), else the other driver's of b. */
int charpack_pick(int kart, int drv, int b, bool human) {
    if (b < 0 || b >= CP_BASES || !human || kart < 0 || kart >= 4 || drv < 0 || drv >= 2) return -1;
    if (s_slot[kart][drv].cell == b) return s_slot[kart][drv].v;
    return s_slot[kart][!drv].cell == b ? s_slot[kart][!drv].v : -1;
}

int charpack_pages(void) { return s_pages; }
int charpack_page(void) { return s_page; }
void charpack_page_first(void) { s_page = 0; }
void charpack_page_step(int dir) {
    if (s_pages < 2) return;
    s_page = (s_page + (dir < 0 ? s_pages - 1 : 1)) % s_pages;
}

/* Each frame of the character select screen: the players' cursor cells (-1 none, or not choosing
 * drivers), each team's chosen cells (the characters' own: a pick in another character's cell
 * chooses the base's, native.c) and who chose them. A newly chosen cell picks what its player chose
 * in another cell for it (by A or the random pick, native.c), else the variant this page shows there
 * when it is that character's, else the game's own. A pick made in another character's cell shows
 * there (its picture, native.c the highlight) while the page shows it there. The models are
 * native.c's to choose each frame (charpack_borrow), then charvoice_update. */
/* A race's drivers, per kart and driver: the character (-1 not built) and the variant it is (-1 the
 * game's own); the HUD's faces and map icons follow them per kart (charpack_race_face). */
static int s_race_b[8][2], s_race_v[8][2];
/* per kart, its driver in front now (the one driving), and the one in front when its HUD first drew
 * it (the race order's first head stays that driver's: the heads trade places on Z; -1 not yet) */
static int s_front[8], s_front0[8];
/* per character, 2 + the variant of its driver who spoke last in the race (1 the game's own), 0 none
 * yet; s_race_on: s_race_b / s_race_v were set (a race began, or the select screen cleared them) */
static int s_speak[CP_BASES];
static bool s_race_on;
void charpack_select_frame(const int cursor[4], const int cells[4][2], const int owner[4][2]) {
    for (int p = 0; p < 4; p++) s_cursor[p] = cursor[p];
    for (int b = 0; b < CP_BASES; b++) s_speak[b] = 0;
    s_race_on = true;
    for (int k = 0; k < 8; k++) s_race_b[k][0] = s_race_b[k][1] = s_race_v[k][0] = s_race_v[k][1] = -1;
    for (int t = 0; t < 4; t++)
        for (int k = 0; k < 2; k++) {
            Pick *x = &s_slot[t][k];
            int cell = cells[t][k], p = owner[t][k];
            if (cell == x->cell) continue;
            if (cell < 0 || cell >= CP_BASES) { /* (the game's: one of the 20) */
                *x = (Pick)NO_PICK;
                continue;
            }
            int on_page = charpack_cell_variant(cell);
            if (p >= 0 && p < 4 && s_pending[p] != NO_PENDING && (s_pending[p] < 0 || s_var[s_pending[p]].base == cell))
                *x = (Pick){cell, s_pending[p], s_pending[p] < 0 ? cell : s_pending_cell[p]};
            else *x = (Pick){cell, on_page >= 0 && s_var[on_page].base == cell ? on_page : -1, cell};
            rt_log("charpack: team %d driver %d: %s as %s", t + 1, k + 1, BASE_NAMES[cell], x->v < 0 ? "the game's own" : s_var[x->v].name);
        }
    for (int p = 0; p < 4; p++) s_pending[p] = NO_PENDING;
    for (int c = 0; c < CP_BASES; c++) {
        int on_page = charpack_cell_variant(c);
        s_show[c] = s_page && on_page < 0 ? CP_DIM : on_page;
        s_model[c] = -1;
    }
    for (int t = 0; t < 4; t++)
        for (int k = 0; k < 2; k++) {
            int vc = charpack_slot_shown(t, k);
            if (vc >= 0) s_show[vc] = s_slot[t][k].v;
        }
    s_select_gx = gx_frame_count();
    s_select_seen = true;
}
/* counted in game frames, not time: one slow frame is still the same screen */
bool charpack_select_active(void) { return s_select_seen && gx_frame_count() - s_select_gx <= 3; }
/* the end of a select screen frame: a slow one (a page's first models, read from the SD card)
 * must not look like the screen opened anew next frame (which turns back to the first page) */
void charpack_select_keep(void) {
    if (s_select_seen) s_select_gx = gx_frame_count();
}

/* Player p's line for the select screen while the cell under the cursor shows a variant
 * ("P1  METAL MARIO", "P1  ROBO MARIO  (AS MARIO)"). */
bool charpack_hint(int p, char *out, size_t n) {
    int b = p >= 0 && p < 4 ? s_cursor[p] : -1;
    int v = b >= 0 && b < CP_BASES ? s_show[b] : charpack_cell_variant(b);
    if (v < 0) return false;
    char as[40] = "";
    if (s_var[v].base != b) snprintf(as, sizeof as, "  (AS %s)", BASE_NAMES[s_var[v].base]);
    snprintf(out, n, "P%d  %s%s", p + 1, s_var[v].name, as); /* the authors: the pack's credits and the README */
    return true;
}

/* the HUD pictures' own copies of a face (native.c hud_face): per kind, kart and slot one place in
 * the pool, with the character it holds now */
typedef struct { uint32_t addr, size; int kind, kart, slot, b; } HudCopy;
static HudCopy s_hud[64];
static unsigned s_nhud;

/* A race's drivers are about to be built: nothing is shown as a variant until one is loaded. */
void charpack_race_begin(void) {
    for (int k = 0; k < 8; k++) s_race_b[k][0] = s_race_b[k][1] = s_race_v[k][0] = s_race_v[k][1] = -1;
    for (int k = 0; k < 8; k++) s_front[k] = 1, s_front0[k] = -1;
    for (int b = 0; b < CP_BASES; b++) s_speak[b] = 0;
    s_race_on = true;
    for (int b = 0; b < CP_BASES; b++) s_show[b] = s_model[b] = -1;
    charvoice_update();
}
/* The variant's own driver.bmd was loaded: its faces show for b. Not its model textures: the model
 * has its own, and another driver of b (the game's own) keeps the game's. */
void charpack_loaded(int b, int v) {
    if (b >= 0 && b < CP_BASES) s_show[b] = v;
    charvoice_update();
}

void charpack_race_driver(int kart, int drv, int b, int v) {
    if (kart < 0 || kart >= 8 || drv < 0 || drv > 1 || b < 0 || b >= CP_BASES) return;
    s_race_b[kart][drv] = b, s_race_v[kart][drv] = v;
    charvoice_update();
}
void charpack_voice_speak(int kart, int drv) {
    if (kart < 0 || kart >= 8 || drv < 0 || drv > 1) return;
    int b = s_race_b[kart][drv];
    if (b >= 0 && b < CP_BASES) s_speak[b] = s_race_v[kart][drv] + 2;
}
/* the variant whose voice plays for b now: the race's driver of b who spoke last (before any did,
 * the first one racing as a variant), else the select screen's (charpack_model) */
static int voice_want(int b) {
    if (s_speak[b]) return s_speak[b] - 2;
    for (int k = 0; k < 8 && s_race_on; k++)
        for (int d = 0; d < 2; d++)
            if (s_race_b[k][d] == b && s_race_v[k][d] >= 0) return s_race_v[k][d];
    return s_model[b];
}
bool charpack_voice_on(int v) {
    for (int b = 0; b < CP_BASES; b++)
        if (v >= 0 && voice_want(b) == v) return true;
    return false;
}
unsigned charpack_voice_variants(int *out, unsigned max) {
    unsigned n = 0;
    for (int i = 0; i < CP_BASES + (s_race_on ? 16 : 0); i++) {
        int v = i < CP_BASES ? s_model[i] : s_race_v[(i - CP_BASES) / 2][(i - CP_BASES) % 2];
        bool dup = v < 0;
        for (unsigned j = 0; j < n && !dup; j++) dup = out[j] == v;
        if (!dup && n < max) out[n++] = v;
    }
    return n;
}
void charpack_race_front(int kart, int front, bool sure) {
    if (kart < 0 || kart >= 8) return;
    s_front[kart] = front & 1;
    if (sure && s_front0[kart] < 0) s_front0[kart] = front & 1;
}
int charpack_race_face(int kart, int slot, int b) {
    if (kart < 0 || kart >= 8 || b < 0 || b >= CP_BASES) return CP_RACE_NONE;
    bool d0 = s_race_b[kart][0] == b, d1 = s_race_b[kart][1] == b;
    if (!d0 && !d1) return CP_RACE_NONE;
    int f0 = s_front0[kart] >= 0 ? s_front0[kart] : s_front[kart];
    return s_race_v[kart][!(d0 && d1) ? d1 : slot < 0 ? s_front[kart] : slot == 0 ? f0 : !f0];
}
int charpack_hud_copy(int kind, int kart, int slot, int b, uint32_t size, uint32_t *addr) {
    unsigned i = 0;
    for (; i < s_nhud; i++)
        if (s_hud[i].kind == kind && s_hud[i].kart == kart && s_hud[i].slot == slot) break;
    if (i < s_nhud && s_hud[i].size < size) return -1; /* a bigger picture than before: never (faces are one size) */
    if (i == s_nhud) {
        uint32_t a = i < sizeof s_hud / sizeof s_hud[0] ? charpack_reserve(size) : 0;
        if (!a) return -1;
        s_hud[s_nhud++] = (HudCopy){a, size, kind, kart, slot, b};
    }
    s_hud[i].b = b;
    *addr = s_hud[i].addr;
    return (int)i;
}
bool charpack_hud_copy_info(int i, int *kind, int *kart, int *slot, int *b) {
    if (i < 0 || (unsigned)i >= s_nhud) return false;
    *kind = s_hud[i].kind, *kart = s_hud[i].kart, *slot = s_hud[i].slot, *b = s_hud[i].b;
    return true;
}

bool charpack_voice_dir(int v, char *out, size_t n) {
    if (v < 0 || (unsigned)v >= s_nvar || !s_var[v].voice) return false;
    if (!out) return true;
    const char *d = s_var[v].dir;
    if (s_var[v].voice == 'v') snprintf(out, n, "%s/voice", d);
    else snprintf(out, n, "%.*s/audio_waves", (int)(strlen(d) - strlen("/files")), d); /* dir: the pack's files/ */
    return true;
}

void charpack_pool_reset(void) {
    if (s_nfile) rt_log("charpack: %u files (%u KiB) let go, the pool starts over", s_nfile, (s_pool - POOL_LO) >> 10);
    s_pool = POOL_LO, s_nfile = 0;
}

/* n bytes of the pool for good (native.c: the select screen's heap for the variants' models), 0 when
 * it no longer has them. */
uint32_t charpack_reserve(uint32_t n) {
    n = (n + 31) & ~31u;
    if (s_pool + n > s_pool_top) return 0;
    s_pool_top -= n;
    return s_pool_top;
}

/* A variant's driver file in MEM1 (read from the SD card the first time), or 0 when it has none. */
static uint32_t load_file(const char *path, uint32_t *size);
uint32_t charpack_file(int v, const char *file, uint32_t *size) {
    char path[224];
    if (strchr(file, '/') || strlen(file) > 48 || !charpack_path(v, 'd', file, path, sizeof path)) return 0;
    return load_file(path, size);
}
/* the same from another driver folder of the variant's pack (MKDD Patcher layout: the shared
 * animations' "cmn_Liz"), 0 none */
uint32_t charpack_folder_file(int v, const char *folder, const char *file, uint32_t *size) {
    char path[224];
    if (v < 0 || (unsigned)v >= s_nvar || !s_var[v].folder[0] || strchr(file, '/') || strchr(folder, '/') ||
        strlen(file) > 48 || strlen(folder) > 16)
        return 0;
    char low[17];
    unsigned n = 0;
    for (; folder[n]; n++) low[n] = (char)tolower((unsigned char)folder[n]); /* the packs' folders are lower case */
    low[n] = 0;
    snprintf(path, sizeof path, "%s/" CP_DRIVERS "/%s/%s", s_var[v].dir, low, file);
    return load_file(path, size);
}
/* a variant's own select screen pose (selectAnm.arc's enter.bck, wait.bck, gut.bck, gut.btp): its
 * selectanm/ (tools/charpack.py), or the MKDD Patcher layout's SceneData/selectAnm.arc/selectanm/<its
 * driver folder>/; 0 none */
uint32_t charpack_select_file(int v, const char *file, uint32_t *size) {
    char path[224];
    if (v < 0 || (unsigned)v >= s_nvar || strchr(file, '/') || strlen(file) > 48) return 0;
    if (!s_var[v].folder[0]) snprintf(path, sizeof path, "%s/selectanm/%s", s_var[v].dir, file);
    else {
        char low[sizeof s_var[v].folder];
        unsigned n = 0;
        for (; s_var[v].folder[n] && n + 1 < sizeof low; n++) low[n] = (char)tolower((unsigned char)s_var[v].folder[n]);
        low[n] = 0;
        snprintf(path, sizeof path, "%s/SceneData/selectAnm.arc/selectanm/%s/%s", s_var[v].dir, low, file);
    }
    return load_file(path, size);
}
static uint32_t load_file(const char *path, uint32_t *size) {
    for (unsigned i = 0; i < s_nfile; i++)
        if (!strcasecmp(s_file[i].path, path)) return *size = s_file[i].size, s_file[i].addr;
    if (s_nfile == FILE_MAX) return 0;
    Loaded *l = &s_file[s_nfile++];
    snprintf(l->path, sizeof l->path, "%s", path);
    l->addr = l->size = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0 && n < (8 << 20) && s_pool + (uint32_t)n <= s_pool_top) {
        uint8_t *dst = g_ram + (s_pool & 0x01FFFFFFu);
        if (fread(dst, 1, (size_t)n, f) == (size_t)n && memcmp(dst, "Yaz0", 4)) {
            l->addr = s_pool, l->size = (uint32_t)n;
            s_pool = (s_pool + (uint32_t)n + 31) & ~31u;
            rt_log("charpack: %s at %#x, %ld bytes (%u KiB of the pool left)", path, l->addr, n, (s_pool_top - s_pool) >> 10);
        } else rt_log("charpack: %s could not be read (or is Yaz0 compressed)", path);
    } else if (n > 0) rt_log("charpack: no room for %s (%ld bytes)", path, n);
    fclose(f);
    *size = l->size;
    return l->addr;
}

/* The game's own Driver/<base>/<file> from MRAM.arc on the disc (malloc'd), or NULL. RARC: info
 * block at 0x20 (node count, node offset, entry count, entry offset, string table size, offset; all
 * relative to 0x20), data at 0x20 + header 0x0C; nodes 16 bytes (name offset +4, entry count +10,
 * first entry +12), entries 20 bytes (flags +4: 0x0100 a file, name offset +6, data +8, size +12). */
uint8_t *charpack_orig(int b, const char *file, uint32_t *size) {
    uint64_t arc;
    uint32_t asize;
    if (b < 0 || b >= CP_BASES || !disc_find("MRAM.arc", &arc, &asize) || asize < 0x40) return NULL;
    uint8_t h[0x40];
    disc_read(arc, h, sizeof h);
    if (memcmp(h, "RARC", 4)) return NULL;
    uint32_t nn = rd32(h + 0x20), no = rd32(h + 0x24) + 0x20, ne = rd32(h + 0x28), eo = rd32(h + 0x2C) + 0x20;
    uint32_t ss = rd32(h + 0x30), so = rd32(h + 0x34) + 0x20, data = rd32(h + 0x0C) + 0x20;
    if (nn > 4096 || ne > 65536 || ss > (1u << 20)) return NULL;
    uint8_t *nodes = malloc(nn * 16), *ents = malloc(ne * 20), *str = malloc(ss + 1), *out = NULL;
    if (!nodes || !ents || !str) goto done;
    disc_read(arc + no, nodes, nn * 16);
    disc_read(arc + eo, ents, ne * 20);
    disc_read(arc + so, str, ss);
    str[ss] = 0;
    for (uint32_t i = 0; i < nn && !out; i++) {
        const uint8_t *nd = nodes + 16 * i;
        uint32_t nm = rd32(nd + 4), cnt = rd16(nd + 10), first = rd32(nd + 12);
        if (nm >= ss || strcasecmp((const char *)str + nm, BASES[b]) || first + cnt > ne) continue;
        for (uint32_t j = first; j < first + cnt; j++) {
            const uint8_t *en = ents + 20 * j;
            unsigned fl = rd16(en + 4), en_nm = rd16(en + 6);
            if (!(fl & 0x0100) || en_nm >= ss || strcasecmp((const char *)str + en_nm, file)) continue;
            uint32_t sz = rd32(en + 12);
            if (sz && sz < (4u << 20) && (out = malloc(sz))) disc_read(arc + data + rd32(en + 8), out, sz), *size = sz;
            break;
        }
    }
done:
    free(nodes), free(ents), free(str);
    return out;
}
