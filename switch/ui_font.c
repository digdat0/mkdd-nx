/* The game's own lettering for the - menu: MKDD draws words like "GRAND PRIX" from one 32x32
 * IA4 texture per letter (MarioFont_*.bti in race2d.arc, inside MRAM.arc). They are read from
 * the player's disc at start-up (nothing from the game ships with the port) and packed into one
 * GL texture: R = intensity (the dark outline), G = alpha. The menu tints them with a
 * top-to-bottom gradient, like the game's yellow-to-orange titles. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <GLES3/gl32.h>

#include "ui_font.h"
#include "../runtime/rt.h"

#define CELL 32
#define COLS 8
#define ROWS 6

typedef struct { char c; const char *file; } GlyphFile;
static const GlyphFile FILES[] = {
    {'0', "mariofont_number_0.bti"}, {'1', "mariofont_number_1.bti"}, {'2', "mariofont_number_2.bti"},
    {'3', "mariofont_number_3.bti"}, {'4', "mariofont_number_4.bti"}, {'5', "mariofont_number_5.bti"},
    {'6', "mariofont_number_6.bti"}, {'7', "mariofont_number_7.bti"}, {'8', "mariofont_number_8.bti"},
    {'9', "mariofont_number_9.bti"}, {'+', "mariofont_number_plus.bti"}, {'!', "mariofontexclamation.bti"},
    {'?', "mariofontquestion.bti"}, {'.', "mariofontperiod.bti"}, {':', "mariofontcolon.bti"},
    {'/', "mariofontslash.bti"},
    {'(', NULL}, {')', NULL}, /* the disc has none: drawn in the same style (paren_glyph) */
};

static GLuint s_tex;
static uint8_t *s_atlas; /* the same RG8 atlas on the CPU, COLS*CELL x ROWS*CELL */
static bool s_have[128];
static float s_uv[128][4], s_x0[128], s_x1[128];

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

/* A RARC archive at disc offset arc: its tables, read once. */
typedef struct { uint64_t base, data; uint8_t *ent, *str; uint32_t nent, strsz; } Rarc;
static bool rarc_open(Rarc *r, uint64_t off) {
    uint8_t h[0x40];
    disc_read(off, h, sizeof h);
    if (memcmp(h, "RARC", 4)) return false;
    r->base = off;
    r->data = off + 0x20 + be32(h + 0x0C);
    r->nent = be32(h + 0x28);
    uint32_t eoff = be32(h + 0x2C), ssz = be32(h + 0x30), soff = be32(h + 0x34);
    if (r->nent > 100000 || ssz > (4u << 20)) return false;
    r->ent = malloc((size_t)r->nent * 20);
    r->str = malloc(ssz + 1);
    disc_read(off + 0x20 + eoff, r->ent, (size_t)r->nent * 20);
    disc_read(off + 0x20 + soff, r->str, ssz);
    r->str[ssz] = 0;
    r->strsz = ssz;
    return true;
}
static void rarc_close(Rarc *r) { free(r->ent); free(r->str); }
/* A file anywhere in the archive, by name (case-insensitive). */
static bool rarc_find(const Rarc *r, const char *name, uint64_t *off, uint32_t *size) {
    for (uint32_t i = 0; i < r->nent; i++) {
        const uint8_t *e = r->ent + 20 * i;
        uint16_t type = be16(e + 4), no = be16(e + 6);
        if (!(type & 0x0100) || no >= r->strsz) continue;
        if (!strcasecmp((const char *)r->str + no, name)) {
            *off = r->data + be32(e + 8);
            *size = be32(e + 12);
            return true;
        }
    }
    return false;
}

bool arc_locate(const char *arc, const char *name, uint64_t *off, uint32_t *size) {
    uint64_t a;
    uint32_t asize;
    Rarc r;
    if (!disc_find(arc, &a, &asize) || !rarc_open(&r, a)) return false;
    bool ok = rarc_find(&r, name, off, size);
    rarc_close(&r);
    return ok;
}

uint8_t *race2d_read(const char *name, uint32_t *size) {
    uint64_t mram, r2off, off;
    uint32_t msize, r2size;
    Rarc outer, r2d;
    if (!disc_find("MRAM.arc", &mram, &msize) || !rarc_open(&outer, mram)) return NULL;
    bool have = rarc_find(&outer, "race2d.arc", &r2off, &r2size) && rarc_open(&r2d, r2off);
    rarc_close(&outer);
    if (!have) return NULL;
    uint8_t *b = NULL;
    if (rarc_find(&r2d, name, &off, size) && *size < (16u << 20) && (b = malloc(*size))) disc_read(off, b, *size);
    rarc_close(&r2d);
    return b;
}

/* IA4 (GX format 2: 8x4 blocks, high nibble alpha, low nibble intensity) -> RG8 cell. */
static bool load_glyph(const Rarc *r, const char *file, uint8_t *cell, int stride) {
    uint64_t off;
    uint32_t size;
    if (!rarc_find(r, file, &off, &size) || size < 0x20 + CELL * CELL / 2) return false;
    uint8_t *b = malloc(size);
    disc_read(off, b, size);
    bool ok = b[0] == 2 && be16(b + 2) == CELL && be16(b + 4) == CELL && be32(b + 0x1C) + CELL * CELL / 2 <= size;
    if (ok) {
        const uint8_t *d = b + be32(b + 0x1C);
        int k = 0;
        for (int by = 0; by < CELL; by += 4)
            for (int bx = 0; bx < CELL; bx += 8)
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 8; x++, k++) {
                        uint8_t v = d[k];
                        uint8_t *px = cell + (size_t)(by + y) * stride + (size_t)(bx + x) * 2;
                        px[0] = (uint8_t)((v & 15) * 17);
                        px[1] = (uint8_t)((v >> 4) * 17);
                    }
    }
    free(b);
    return ok;
}

/* A bracket in MarioFont's look (the slash's: a white stroke about 5 texels wide, a black edge 2
 * wide, rounded ends, rows 3-28): an arc of a circle of radius 16, its middle at x 8 (')' mirrored). */
static bool paren_glyph(uint8_t *cell, int pitch, bool close) {
    const float R = 16, cx = 24, cy = 15.5f, half = asinf(10.0f / R), core = 2.6f, edge = 4.6f;
    for (int y = 0; y < CELL; y++)
        for (int x = 0; x < CELL; x++) {
            float px = (close ? CELL - 1 - x : x) + 0.5f - cx, py = y + 0.5f - cy, d;
            float a = atan2f(py, -px); /* 0 at the arc's middle (left of the centre) */
            if (fabsf(a) <= half) d = fabsf(sqrtf(px * px + py * py) - R);
            else {
                float ea = a < 0 ? -half : half, ex = -R * cosf(ea) - px, ey = R * sinf(ea) - py;
                d = sqrtf(ex * ex + ey * ey);
            }
            float al = edge + 0.5f - d, in = core + 0.5f - d;
            uint8_t *o = cell + (size_t)y * pitch + (size_t)x * 2;
            o[0] = (uint8_t)(255 * (in < 0 ? 0 : in > 1 ? 1 : in));
            o[1] = (uint8_t)(255 * (al < 0 ? 0 : al > 1 ? 1 : al));
        }
    return true;
}

bool uifont_load(void) {
    uint64_t mram;
    uint32_t msize;
    Rarc outer, r2d;
    if (!disc_find("MRAM.arc", &mram, &msize) || !rarc_open(&outer, mram)) {
        rt_log("ui font: MRAM.arc not found");
        return false;
    }
    uint64_t r2off;
    uint32_t r2size;
    bool have = rarc_find(&outer, "race2d.arc", &r2off, &r2size) && rarc_open(&r2d, r2off);
    rarc_close(&outer);
    if (!have) { rt_log("ui font: race2d.arc not found"); return false; }

    const int W = COLS * CELL, H = ROWS * CELL;
    uint8_t *atlas = calloc((size_t)W * H, 2);
    int slot = 0, loaded = 0;
    char file[40];
    for (int i = 0; i < 26 + (int)(sizeof FILES / sizeof FILES[0]) && slot < COLS * ROWS; i++) {
        char c;
        if (i < 26) { c = (char)('A' + i); snprintf(file, sizeof file, "mariofont_%c.bti", 'a' + i); }
        else { c = FILES[i - 26].c; snprintf(file, sizeof file, "%s", FILES[i - 26].file ? FILES[i - 26].file : ""); }
        int cx = slot % COLS * CELL, cy = slot / COLS * CELL;
        uint8_t *cell = atlas + ((size_t)cy * W + (size_t)cx) * 2;
        if (!(*file ? load_glyph(&r2d, file, cell, W * 2) : paren_glyph(cell, W * 2, c == ')'))) continue;
        /* the letter's horizontal extent (its alpha), for proportional spacing */
        int x0 = CELL, x1 = 0;
        for (int y = 0; y < CELL; y++)
            for (int x = 0; x < CELL; x++)
                if (cell[(size_t)y * W * 2 + (size_t)x * 2 + 1] > 40) { if (x < x0) x0 = x; if (x + 1 > x1) x1 = x + 1; }
        if (x1 <= x0) { x0 = 8; x1 = 24; }
        unsigned uc = (unsigned char)c;
        s_have[uc] = true;
        s_uv[uc][0] = (float)cx / W; s_uv[uc][1] = (float)cy / H;
        s_uv[uc][2] = (float)(cx + CELL) / W; s_uv[uc][3] = (float)(cy + CELL) / H;
        s_x0[uc] = (float)x0; s_x1[uc] = (float)x1;
        slot++;
        loaded++;
    }
    rarc_close(&r2d);
    if (loaded < 26) {
        rt_log("ui font: only %d letters found, using the built-in font", loaded);
        free(atlas);
        memset(s_have, 0, sizeof s_have);
        return false;
    }
    glGenTextures(1, &s_tex);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, W, H, 0, GL_RG, GL_UNSIGNED_BYTE, atlas);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    s_atlas = atlas;
    rt_log("ui font: %d MarioFont glyphs from the disc", loaded);
    return true;
}

unsigned uifont_texture(void) { return s_tex; }

const uint8_t *uifont_atlas(int *w, int *h) {
    *w = COLS * CELL;
    *h = ROWS * CELL;
    return s_atlas;
}

bool uifont_glyph(char c, float uv[4], float *x0, float *x1) {
    unsigned uc = (unsigned char)c;
    if (c >= 'a' && c <= 'z') uc = (unsigned char)(c - 32);
    if (uc >= 128 || !s_have[uc] || !s_tex) return false;
    memcpy(uv, s_uv[uc], sizeof s_uv[uc]);
    *x0 = s_x0[uc];
    *x1 = s_x1[uc];
    return true;
}
