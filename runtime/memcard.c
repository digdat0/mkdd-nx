/* Memory card emulation at the CARD API level (slot A present, slot B empty). C port of
 * gc-runtime's memcard.rs. Saves are standard .gci files (Dolphin, save editors); the older
 * single-file "MKDDCARD" v1 save is still read once and converted.
 *
 * The SDK's EXI-level driver can't be used as-is (unlocking a card runs a challenge on the DSP),
 * so hle.c routes the public CARD* functions here. Files live in memory and each one is written
 * to its .gci after every change (see load()). Card: 4 Mbit (59 user blocks of 8 KiB), like the
 * standard "Memory Card 59". */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#define SECTOR_SIZE 8192u
#define USER_BLOCKS 59u
#define MAX_FILES 127
#define MEM_SIZE_MBIT 4

#define READY 0
#define NOCARD -3
#define NOFILE -4
#define EXIST -7
#define INSSPACE -9
#define NOPERM -10
#define LIMIT -11
#define NAMETOOLONG -12

typedef struct {
    bool present;
    bool backed_up; /* this session's first change already copied the old file to backup/ */
    uint8_t name[32], game[4], company[2];
    uint32_t time;
    uint8_t banner_format;
    uint32_t icon_addr;
    uint16_t icon_format, icon_speed;
    uint32_t comment_addr;
    uint8_t perm, copies; /* kept from imported .gci files */
    uint8_t *data;
    uint32_t len;
} Entry;

static Entry s_files[MAX_FILES];
static bool s_loaded, s_mounted;
/* The card is a folder of .gci files (Dolphin's "GCI folder" layout: one card file each, named
 * "<maker>-<game>-<name>.gci"), next to the old single-file MKDDCARD save:
 *   <dir>/slotA.bin           MKDDCARD v1, converted once, then renamed slotA.bin.migrated
 *   <dir>/card_a/<file>.gci    the card
 *   <dir>/backup/<file>.gci    each file as it was before this session first changed or deleted it
 * Every write goes to a .tmp file first and replaces the .gci only once complete. */
static char s_path[512] = "memcard/slotA.bin";
static char s_dir[512] = "memcard";

void memcard_set_path(const char *path) {
    snprintf(s_path, sizeof s_path, "%s", path);
    snprintf(s_dir, sizeof s_dir, "%s", path);
    char *slash = strrchr(s_dir, '/');
    if (slash) *slash = 0;
    else snprintf(s_dir, sizeof s_dir, ".");
}

static uint32_t rd32le(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16le(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32be(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t rd16be(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void wr32be(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void wr16be(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static void this_game(uint8_t g[4], uint8_t co[2]);
static bool blank_code(const Entry *e) {
    static const uint8_t z[6];
    return !memcmp(e->game, z, 4) && !memcmp(e->company, z, 2);
}

/* "<dir>/<sub>/01-GM4E-<name>.gci"; characters a FAT/Windows file name can't hold are written
 * as __XX__ (Dolphin's escaping). The name inside the .gci header is what counts on load. */
static void gci_path(char *out, size_t cap, const char *sub, const Entry *e) {
    char fn[200];
    size_t k = 0;
    for (int i = 0; i < 2; i++) fn[k++] = (char)(e->company[i] ? e->company[i] : '0');
    fn[k++] = '-';
    for (int i = 0; i < 4; i++) fn[k++] = (char)(e->game[i] ? e->game[i] : '0');
    fn[k++] = '-';
    for (int i = 0; i < 32 && e->name[i]; i++) {
        uint8_t ch = e->name[i];
        if (ch < 0x20 || ch >= 0x7F || strchr("\"*/:<>?\\|%", ch)) k += (size_t)snprintf(fn + k, sizeof fn - k, "__%02x__", ch);
        else fn[k++] = (char)ch;
    }
    fn[k] = 0;
    snprintf(out, cap, "%s/%s/%s.gci", s_dir, sub, fn);
}

/* The 64-byte directory entry (big-endian) followed by the file's blocks. */
static uint8_t *gci_image(const Entry *e, size_t *n) {
    uint8_t *b = malloc(64 + e->len), *h = b;
    memcpy(h, e->game, 4);
    memcpy(h + 4, e->company, 2);
    h[6] = 0xFF;
    h[7] = e->banner_format;
    memcpy(h + 8, e->name, 32);
    wr32be(h + 0x28, e->time);
    wr32be(h + 0x2C, e->icon_addr);
    wr16be(h + 0x30, e->icon_format);
    wr16be(h + 0x32, e->icon_speed);
    h[0x34] = e->perm;
    h[0x35] = e->copies;
    wr16be(h + 0x36, (uint16_t)(5 + (e - s_files)));
    wr16be(h + 0x38, (uint16_t)(e->len / SECTOR_SIZE));
    wr16be(h + 0x3A, 0xFFFF);
    wr32be(h + 0x3C, e->comment_addr);
    memcpy(b + 64, e->data, e->len);
    *n = 64 + e->len;
    return b;
}

static bool parse_gci(const uint8_t *b, size_t n, Entry *e) {
    if (n < 64 || (n - 64) % SECTOR_SIZE || rd16be(b + 0x38) != (n - 64) / SECTOR_SIZE) return false;
    memset(e, 0, sizeof *e);
    memcpy(e->game, b, 4);
    memcpy(e->company, b + 4, 2);
    e->banner_format = b[7];
    memcpy(e->name, b + 8, 32);
    e->time = rd32be(b + 0x28);
    e->icon_addr = rd32be(b + 0x2C);
    e->icon_format = rd16be(b + 0x30);
    e->icon_speed = rd16be(b + 0x32);
    e->perm = b[0x34];
    e->copies = b[0x35];
    e->comment_addr = rd32be(b + 0x3C);
    e->len = (uint32_t)(n - 64);
    e->data = malloc(e->len ? e->len : 1);
    memcpy(e->data, b + 64, e->len);
    e->present = true;
    return true;
}

/* Copy the file at path (if any) into backup/, atomically. */
static void backup_file(const char *path, const Entry *e) {
    size_t n;
    uint8_t *b = plat_read_file(path, &n);
    if (!b) return;
    char bp[600];
    gci_path(bp, sizeof bp, "backup", e);
    if (!plat_write_file_atomic(bp, b, n)) rt_log("memcard: could not write backup %s", bp);
    free(b);
}

static void write_entry(Entry *e) {
    char p[600];
    gci_path(p, sizeof p, "card_a", e);
    if (!e->backed_up) { backup_file(p, e); e->backed_up = true; }
    size_t n;
    uint8_t *b = gci_image(e, &n);
    if (!plat_write_file_atomic(p, b, n)) rt_log("memcard: could not write %s", p);
    free(b);
}

/* Remove a file from the card folder; its last version stays in backup/. */
static void remove_entry_file(Entry *e) {
    char p[600];
    gci_path(p, sizeof p, "card_a", e);
    if (!e->backed_up) backup_file(p, e);
    remove(p);
}

/* MKDDCARD v1 (one file for the whole card, little-endian fields). */
static int load_mkddcard(const char *path) {
    size_t n;
    uint8_t *b = plat_read_file(path, &n);
    if (!b) return -1;
    if (n < 12 || memcmp(b, "MKDDCARD", 8)) { free(b); return -1; }
    size_t p = 12;
    int count = 0;
#define TAKE(k) (p + (k) <= n ? (p += (k), b + p - (k)) : NULL)
    for (int i = 0; i < MAX_FILES; i++) {
        const uint8_t *t = TAKE(1);
        if (!t) break;
        if (!t[0]) continue;
        Entry e = {0};
        const uint8_t *q;
        if (!(q = TAKE(32))) break;
        memcpy(e.name, q, 32);
        if (!(q = TAKE(4))) break;
        memcpy(e.game, q, 4);
        if (!(q = TAKE(2))) break;
        memcpy(e.company, q, 2);
        if (!(q = TAKE(4))) break;
        e.time = rd32le(q);
        if (!(q = TAKE(1))) break;
        e.banner_format = q[0];
        if (!(q = TAKE(4))) break;
        e.icon_addr = rd32le(q);
        if (!(q = TAKE(2))) break;
        e.icon_format = rd16le(q);
        if (!(q = TAKE(2))) break;
        e.icon_speed = rd16le(q);
        if (!(q = TAKE(4))) break;
        e.comment_addr = rd32le(q);
        if (!(q = TAKE(4))) break;
        e.len = rd32le(q);
        if (!(q = TAKE(e.len))) break;
        e.data = malloc(e.len ? e.len : 1);
        memcpy(e.data, q, e.len);
        e.perm = 0x04; /* public, as the SDK creates files */
        e.present = true;
        s_files[i] = e;
        count++;
    }
#undef TAKE
    free(b);
    return count;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* Names in dir ending in suffix, sorted (malloc'd; caller frees). */
static int list_dir(const char *dir, const char *suffix, char **names, int cap) {
    int nn = 0;
    size_t k = strlen(suffix);
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d)) && nn < cap) {
        size_t l = strlen(de->d_name);
        if (l > k && !strcmp(de->d_name + l - k, suffix)) names[nn++] = strdup(de->d_name);
    }
    closedir(d);
    qsort(names, (size_t)nn, sizeof *names, cmp_str);
    return nn;
}

static void load(void) {
    s_loaded = true;
    char cd[560];
    snprintf(cd, sizeof cd, "%s/card_a", s_dir);
    char *names[512];
    /* A .tmp is complete once it exists without its .gci (plat_write_file_atomic removes the old
     * file only after closing the new one): finish that rename; otherwise drop the leftover. */
    int nn = list_dir(cd, ".gci.tmp", names, 512);
    for (int i = 0; i < nn; i++) {
        char tp[1200], gp[1200];
        snprintf(tp, sizeof tp, "%s/%s", cd, names[i]);
        snprintf(gp, sizeof gp, "%s/%.*s", cd, (int)(strlen(names[i]) - 4), names[i]);
        FILE *f = fopen(gp, "rb");
        if (f) { fclose(f); remove(tp); rt_log("memcard: dropped unfinished %s", tp); }
        else if (rename(tp, gp) == 0) rt_log("memcard: completed interrupted save %s", gp);
        free(names[i]);
    }
    nn = list_dir(cd, ".gci", names, 512);
    int slot = 0, used = 0;
    for (int i = 0; i < nn; i++) {
        if (!names[i]) continue;
        char p[1200];
        snprintf(p, sizeof p, "%s/%s", cd, names[i]);
        size_t n;
        uint8_t *b = plat_read_file(p, &n);
        Entry e;
        if (b && slot < MAX_FILES && parse_gci(b, n, &e)) {
            s_files[slot++] = e;
            used += (int)(e.len / SECTOR_SIZE);
        } else rt_log("memcard: skipped %s (not a card file, or the card is full)", p);
        free(b);
        free(names[i]);
    }
    if (slot) {
        rt_log("memcard: %d files (%d blocks) from %s", slot, used, cd);
        if (used > (int)USER_BLOCKS) rt_log("memcard: more than %u blocks in use; new saves will report a full card", USER_BLOCKS);
        return;
    }
    /* No .gci files yet: convert the old single-file save once. */
    int k = load_mkddcard(s_path);
    if (k < 0) return;
    uint8_t g[4], co[2];
    this_game(g, co);
    if (!g[0]) { memcpy(g, "GM4E", 4); memcpy(co, "01", 2); }
    for (int i = 0; i < MAX_FILES; i++) {
        Entry *e = &s_files[i];
        if (!e->present) continue;
        if (blank_code(e)) { memcpy(e->game, g, 4); memcpy(e->company, co, 2); } /* saves from before 0939 */
        e->backed_up = true; /* the old file stays as slotA.bin.migrated */
        write_entry(e);
    }
    char mp[600];
    snprintf(mp, sizeof mp, "%s.migrated", s_path);
    remove(mp);
    if (rename(s_path, mp) == 0) rt_log("memcard: converted %s (%d files) to %s; old file kept as %s", s_path, k, cd, mp);
    else rt_log("memcard: converted %s (%d files) to %s; could not rename the old file", s_path, k, cd);
}

static void read_name(uint32_t addr, uint8_t n[32]) {
    memset(n, 0, 32);
    for (int i = 0; i < 32; i++) {
        n[i] = (uint8_t)m_read_be(addr + i, 1);
        if (!n[i]) break;
    }
}
static unsigned name_len(uint32_t addr) {
    unsigned i = 0;
    while (i < 64 && m_read_be(addr + i, 1)) i++;
    return i;
}
static void this_game(uint8_t g[4], uint8_t co[2]) {
    for (int i = 0; i < 4; i++) g[i] = (uint8_t)m_read_be(0x80000000u + i, 1);
    for (int i = 0; i < 2; i++) co[i] = (uint8_t)m_read_be(0x80000004u + i, 1);
}
static uint32_t now2000(void) {
    uint64_t s = plat_unix_seconds();
    return s > 946684800u ? (uint32_t)(s - 946684800u) : 0;
}
/* CARDFileInfo: chan, fileNo, offset, length, iBlock. */
static void write_fileinfo(uint32_t fi, uint32_t chan, uint32_t no, uint32_t len) {
    m_write_be(fi, 4, chan);
    m_write_be(fi + 4, 4, no);
    m_write_be(fi + 8, 4, 0);
    m_write_be(fi + 12, 4, len);
    m_write_be(fi + 16, 2, 5 + no);
}
/* Saves made before 0939 may carry a blank game code: the SDK's SetEffectivePriority sometimes
 * wrote zeros over the disc ID that this_game reads. They count as this game's (after an exact
 * match, so a card holding both kinds opens the same file as before). */
static int find(const uint8_t name[32], const uint8_t g[4], const uint8_t co[2]) {
    static const uint8_t blank[6];
    int fallback = -1;
    for (int i = 0; i < MAX_FILES; i++) {
        Entry *e = &s_files[i];
        if (!e->present || memcmp(e->name, name, 32)) continue;
        if (!memcmp(e->game, g, 4) && !memcmp(e->company, co, 2)) return i;
        if (fallback < 0 && !memcmp(e->game, blank, 4) && !memcmp(e->company, blank, 2)) fallback = i;
    }
    return fallback;
}
static uint32_t blocks_of(const Entry *e) { return (e->len + SECTOR_SIZE - 1) / SECTOR_SIZE; }
static uint32_t free_blocks(void) {
    uint32_t used = 0;
    for (int i = 0; i < MAX_FILES; i++)
        if (s_files[i].present) used += blocks_of(&s_files[i]);
    return used >= USER_BLOCKS ? 0 : USER_BLOCKS - used;
}
static void free_entry(Entry *e) { free(e->data); memset(e, 0, sizeof *e); }

/* Fill a CARDStat (0x6C bytes), including the derived icon/banner offsets. */
static void write_stat(uint32_t st, const Entry *e) {
    for (int i = 0; i < 32; i++) m_write_be(st + i, 1, e->name[i]);
    m_write_be(st + 0x20, 4, e->len);
    m_write_be(st + 0x24, 4, e->time);
    for (int i = 0; i < 4; i++) m_write_be(st + 0x28 + i, 1, e->game[i]);
    for (int i = 0; i < 2; i++) m_write_be(st + 0x2C + i, 1, e->company[i]);
    m_write_be(st + 0x2E, 1, e->banner_format);
    m_write_be(st + 0x30, 4, e->icon_addr);
    m_write_be(st + 0x34, 2, e->icon_format);
    m_write_be(st + 0x36, 2, e->icon_speed);
    m_write_be(st + 0x38, 4, e->comment_addr);
    /* Same layout rules as the SDK's __CARDUpdateIconOffsets. */
    const uint32_t NONE = 0xFFFFFFFFu;
    uint8_t banner_format = e->banner_format;
    uint16_t icon_format = e->icon_format;
    uint32_t off;
    if (e->icon_addr == NONE) { banner_format = 0; icon_format = 0; off = 0; }
    else off = e->icon_addr;
    uint32_t banner = NONE, banner_tlut = NONE;
    if ((banner_format & 3) == 1) { banner = off; banner_tlut = off + 96 * 32; off += 96 * 32 + 512; }
    else if ((banner_format & 3) == 2) { banner = off; off += 96 * 32 * 2; }
    m_write_be(st + 0x3C, 4, banner);
    m_write_be(st + 0x40, 4, banner_tlut);
    bool tlut = false;
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t icon = NONE;
        switch ((icon_format >> (2 * i)) & 3) {
        case 1: tlut = true; icon = off; off += 32 * 32; break;
        case 2: icon = off; off += 32 * 32 * 2; break;
        }
        m_write_be(st + 0x44 + 4 * i, 4, icon);
    }
    uint32_t icon_tlut = NONE;
    if (tlut) { icon_tlut = off; off += 512; }
    m_write_be(st + 0x64, 4, icon_tlut);
    m_write_be(st + 0x68, 4, off);
}

/* Run an async callback (chan, result) if present, and return the result. */
static int32_t done(Ctx *c, uint32_t cb, uint32_t chan, int32_t result) {
    if (cb) {
        uint32_t args[2] = {chan, (uint32_t)result};
        ctx_call_guest(c, cb, args, 2);
    }
    return result;
}

static bool ends_with(const char *s, const char *suffix) {
    size_t n = strlen(s), k = strlen(suffix);
    return n >= k && !strcmp(s + n - k, suffix);
}

bool memcard_call(Ctx *c, const char *name, int32_t *result) {
    if (!s_loaded) load();
    uint32_t a[6];
    for (int i = 0; i < 6; i++) a[i] = ctx_gpr(c, 3 + i);
    bool async = ends_with(name, "Async");
    int32_t r;
    uint8_t g[4], co[2], nm[32];
    if (!strcmp(name, "CARDInit")) r = READY;
    else if (!strcmp(name, "CARDProbeEx")) {
        if (a[0] == 0) {
            if (a[1]) m_write_be(a[1], 4, MEM_SIZE_MBIT);
            if (a[2]) m_write_be(a[2], 4, SECTOR_SIZE);
            r = READY;
        } else r = NOCARD;
    } else if (!strcmp(name, "CARDMount") || !strcmp(name, "CARDMountAsync")) {
        r = a[0] == 0 ? (s_mounted = true, READY) : NOCARD;
        r = done(c, async ? a[3] : 0, a[0], r);
    } else if (!strcmp(name, "CARDUnmount")) {
        if (a[0] == 0) s_mounted = false;
        r = READY;
    } else if (!strcmp(name, "CARDCheck")) r = a[0] == 0 ? READY : NOCARD;
    else if (!strcmp(name, "CARDCheckExAsync")) {
        if (a[1]) m_write_be(a[1], 4, 0);
        r = done(c, a[2], a[0], a[0] == 0 ? READY : NOCARD);
    } else if (!strcmp(name, "CARDFormat")) {
        if (a[0] != 0) r = NOCARD;
        else {
            for (int i = 0; i < MAX_FILES; i++)
                if (s_files[i].present) { remove_entry_file(&s_files[i]); free_entry(&s_files[i]); }
            r = READY;
        }
    } else if (!strcmp(name, "CARDFreeBlocks")) {
        if (a[0] != 0) r = NOCARD;
        else {
            if (a[1]) m_write_be(a[1], 4, free_blocks() * SECTOR_SIZE);
            if (a[2]) {
                int used = 0;
                for (int i = 0; i < MAX_FILES; i++) used += s_files[i].present;
                m_write_be(a[2], 4, (uint32_t)(MAX_FILES - used));
            }
            r = READY;
        }
    } else if (!strcmp(name, "CARDOpen")) {
        if (a[0] != 0) r = NOCARD;
        else {
            this_game(g, co);
            read_name(a[1], nm);
            int no = find(nm, g, co);
            if (no >= 0) { write_fileinfo(a[2], a[0], (uint32_t)no, s_files[no].len); r = READY; }
            else r = NOFILE;
        }
    } else if (!strcmp(name, "CARDFastOpen")) {
        if (a[0] != 0) r = NOCARD;
        else if (a[1] < MAX_FILES && s_files[a[1]].present) { write_fileinfo(a[2], a[0], a[1], s_files[a[1]].len); r = READY; }
        else r = NOFILE;
    } else if (!strcmp(name, "CARDClose")) r = READY;
    else if (!strcmp(name, "CARDCreate") || !strcmp(name, "CARDCreateAsync")) {
        uint32_t chan = a[0], size = a[2], fi = a[3];
        if (chan != 0) r = NOCARD;
        else if (name_len(a[1]) > 32) r = NAMETOOLONG;
        else {
            this_game(g, co);
            read_name(a[1], nm);
            uint32_t blocks = (size + SECTOR_SIZE - 1) / SECTOR_SIZE;
            int slot = -1;
            for (int i = 0; i < MAX_FILES && slot < 0; i++) if (!s_files[i].present) slot = i;
            if (find(nm, g, co) >= 0) r = EXIST;
            else if (blocks > free_blocks()) r = INSSPACE;
            else if (slot < 0) r = LIMIT;
            else {
                Entry *e = &s_files[slot];
                memset(e, 0, sizeof *e);
                e->present = true;
                memcpy(e->name, nm, 32); memcpy(e->game, g, 4); memcpy(e->company, co, 2);
                e->time = now2000();
                e->icon_addr = 0xFFFFFFFFu;
                e->comment_addr = 0xFFFFFFFFu;
                e->len = blocks * SECTOR_SIZE;
                e->data = calloc(1, e->len ? e->len : 1);
                e->perm = 0x04;
                write_entry(e);
                write_fileinfo(fi, chan, (uint32_t)slot, e->len);
                r = READY;
            }
        }
        r = done(c, async ? a[4] : 0, chan, r);
    } else if (!strncmp(name, "CARDRead", 8) || !strncmp(name, "CARDWrite", 9)) {
        uint32_t fi = a[0], buf = a[1], len = a[2], off = a[3];
        uint32_t chan = (uint32_t)m_read_be(fi, 4), no = (uint32_t)m_read_be(fi + 4, 4);
        bool write = !strncmp(name, "CARDWrite", 9);
        if (no >= MAX_FILES || !s_files[no].present) r = NOFILE;
        else {
            Entry *e = &s_files[no];
            if ((uint64_t)off + len > e->len) {
                rt_log("CARD %s out of range %#x+%#x", name, off, len);
                r = NOPERM;
            } else {
                for (uint32_t i = 0; i < len; i++) {
                    if (write) e->data[off + i] = (uint8_t)m_read_be(buf + i, 1);
                    else m_write_be(buf + i, 1, e->data[off + i]);
                }
                if (write) { e->time = now2000(); write_entry(e); }
                m_write_be(fi + 8, 4, off + len);
                r = READY;
            }
        }
        r = done(c, async ? a[4] : 0, chan, r);
    } else if (!strncmp(name, "CARDDelete", 10) || !strncmp(name, "CARDFastDelete", 14)) {
        uint32_t chan = a[0];
        if (chan != 0) r = NOCARD;
        else {
            int no = -1;
            if (strstr(name, "Fast")) { if (a[1] < MAX_FILES && s_files[a[1]].present) no = (int)a[1]; }
            else { this_game(g, co); read_name(a[1], nm); no = find(nm, g, co); }
            if (no >= 0) { remove_entry_file(&s_files[no]); free_entry(&s_files[no]); r = READY; }
            else r = NOFILE;
        }
        r = done(c, async ? a[2] : 0, chan, r);
    } else if (!strcmp(name, "CARDGetStatus")) {
        if (a[0] != 0) r = NOCARD;
        else if (a[1] < MAX_FILES && s_files[a[1]].present) { write_stat(a[2], &s_files[a[1]]); r = READY; }
        else r = NOFILE;
    } else if (!strcmp(name, "CARDSetStatus") || !strcmp(name, "CARDSetStatusAsync")) {
        uint32_t chan = a[0], no = a[1], st = a[2];
        if (chan != 0) r = NOCARD;
        else if (no < MAX_FILES && s_files[no].present) {
            Entry *e = &s_files[no];
            e->banner_format = (uint8_t)m_read_be(st + 0x2E, 1);
            e->icon_addr = (uint32_t)m_read_be(st + 0x30, 4);
            e->icon_format = (uint16_t)m_read_be(st + 0x34, 2);
            e->icon_speed = (uint16_t)m_read_be(st + 0x36, 2);
            e->comment_addr = (uint32_t)m_read_be(st + 0x38, 4);
            e->time = now2000();
            write_entry(e);
            write_stat(st, e);
            r = READY;
        } else r = NOFILE;
        r = done(c, async ? a[3] : 0, chan, r);
    } else if (!strcmp(name, "CARDRename") || !strcmp(name, "CARDRenameAsync")) {
        uint32_t chan = a[0];
        if (chan != 0) r = NOCARD;
        else if (name_len(a[2]) > 32) r = NAMETOOLONG;
        else {
            uint8_t o[32];
            this_game(g, co);
            read_name(a[1], o);
            read_name(a[2], nm);
            int i;
            if (find(nm, g, co) >= 0) r = EXIST;
            else if ((i = find(o, g, co)) >= 0) {
                remove_entry_file(&s_files[i]); /* the old name's file goes to backup/ */
                memcpy(s_files[i].name, nm, 32);
                s_files[i].backed_up = true;
                write_entry(&s_files[i]);
                r = READY;
            }
            else r = NOFILE;
        }
        r = done(c, async ? a[3] : 0, chan, r);
    } else {
        return false;
    }
    *result = r;
    return true;
}
