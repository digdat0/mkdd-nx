/* Character pack pictures (runtime/charpack.c variants): while a character is shown as one of its
 * variants (the select screen's page or choice there, the loaded driver's in a race), the renderer
 * draws the variant's pictures in place of the game's: the face (select grid, race HUD, results), the
 * map icon, the select screen's name tag, and the textures of the select screen's 3D model (the
 * game's model, the variant's textures: the shape follows in the race, where the variant's own model
 * is loaded). On a page with no variant for a character its face is the game's, dimmed.
 *
 * A variant without a map icon gets its face in a white ring; without a name tag, its name in the
 * game's MarioFont (ui_font.c), white, as the game's tags are.
 *
 * The game's pictures are recognised by format, size and hash (gx_subst_register, ids from
 * GX_SUBST_CHARPACK), read from the disc at start-up. A texture both the game's model of one
 * character and another's use is left alone. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <switch.h>
#include <GLES3/gl32.h>

#include "charpack_art.h"
#include "ui_font.h"
#include "../runtime/gx.h"
#include "../runtime/charpack.h"

enum { K_FACE, K_MAP, K_NAME, K_KINDS, K_MODEL = K_KINDS, K_DIM, K_HUD, K_ROW3 };
#define MODEL_TEX 40 /* textures per model at most */
#define ID_MODEL (GX_SUBST_CHARPACK + K_KINDS * CP_BASES)
_Static_assert(CP_HUD_SUBST == ID_MODEL + CP_BASES * MODEL_TEX, "charpack.h's HUD copies come after the model textures");
#define MAP_SIZE 32

/* the MKDD Patcher layout's names for each character's pictures (charpack.c BASES order) */
static const char *const FACE[CP_BASES] = {
    "mario", "luigi", "peach", "daisy", "yoshi", "catherine", "baby_mario", "baby_luigi", "kinopio", "kinopico",
    "noko", "pata", "donky", "diddy", "koopa", "koopa_jr", "wario", "waluigi", "pakkun", "teresa",
};
static const char *const MAP[CP_BASES] = {
    "Mario", "Luigi", "Peach", "Daisy", "yoshi", "Catherine", "BabyMario", "BabyLuigi", "Kinopio", "Kinopico",
    "Nokonoko", "Patapata", "DonkeyKong", "DiddyKong", "Koopa", "Koopajr", "Wario", "Waluigi", "Pakkun", "Teresa",
};
static const char *const NAME[CP_BASES] = {
    "mario", "luigi", "peach", "daisy", "yoshi", "birdo", "baby_mario", "baby_luigi", "kinopio", "kinopico",
    "koopa", "paratoopa", "donkey_kong", "diddy_kong", "bowser", "bowser_jr", "wario", "waluigi", "pakkun", "teresa",
};
#define MENU_ARC "SceneData/English/menu.arc"

static char s_tex_name[CP_BASES][MODEL_TEX][24]; /* the game's model's texture names */
static uint8_t *s_face[CP_BASES];                 /* the game's faces (BTI), for the dimmed ones */
static uint32_t s_face_size[CP_BASES];
static int s_name_w = 152, s_name_h = 32;         /* the game's name tags' size */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }

/* A BTI header at h (offsets from h), avail bytes from h on: its level 0's hash. */
static bool bti_hash(const uint8_t *h, size_t avail, uint32_t *fmt, uint32_t *w, uint32_t *ht, uint64_t *hash) {
    if (avail < 0x20) return false;
    *fmt = h[0], *w = be16(h + 2), *ht = be16(h + 4);
    size_t off = be32(h + 0x1C), bytes = gx_tex_size(*fmt, *w, *ht);
    if (!bytes || off + bytes > avail) return false;
    *hash = fnv_bytes(FNV_INIT, h + off, bytes);
    return true;
}

/* A BMD's TEX1 section: texture count, the BTI headers (0x20 each) and the name table. */
typedef struct { const uint8_t *hdr, *names; unsigned n; size_t avail; } Tex1;
static bool tex1_find(const uint8_t *b, size_t size, Tex1 *t) {
    if (size < 0x20 || memcmp(b, "J3D2bmd", 7)) return false;
    for (size_t o = 0x20; o + 0x14 <= size;) {
        uint32_t len = be32(b + o + 4);
        if (len < 8 || o + len > size) return false;
        if (!memcmp(b + o, "TEX1", 4)) {
            t->n = be16(b + o + 8);
            t->hdr = b + o + be32(b + o + 0xC);
            t->names = b + o + be32(b + o + 0x10);
            t->avail = size - (size_t)(t->hdr - b);
            return t->hdr < b + size && t->names < b + size;
        }
        o += len;
    }
    return false;
}
static const char *tex1_name(const Tex1 *t, unsigned i, const uint8_t *end) {
    if (t->names + 4 + 4 * (i + 1) > end || i >= be16(t->names)) return "";
    const uint8_t *s = t->names + be16(t->names + 4 + 4 * i + 2);
    return s < end ? (const char *)s : "";
}

static uint8_t *read_all(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = len > 0 && len < (8 << 20) ? malloc((size_t)len) : NULL;
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) free(b), b = NULL;
    fclose(f);
    if (b) *n = (size_t)len;
    return b;
}

static void reg_bti(unsigned id, const uint8_t *b, size_t size) {
    uint32_t fmt, w, h;
    uint64_t hash;
    if (b && bti_hash(b, size, &fmt, &w, &h, &hash)) gx_subst_register(id, fmt, w, h, hash);
}

void charpack_art_init(void) {
    if (!charpack_count()) return;
    /* the models' textures: hashes seen in more than one character are left out */
    typedef struct { uint64_t hash; uint32_t fmt, w, h; uint16_t id; int8_t base; } Cand;
    Cand *cand = calloc(CP_BASES * MODEL_TEX, sizeof *cand);
    unsigned nc = 0, nreg = 0;
    for (int b = 0; b < CP_BASES && cand; b++) {
        uint32_t size = 0;
        uint8_t *bmd = charpack_orig(b, "driver.bmd", &size);
        Tex1 t;
        if (bmd && tex1_find(bmd, size, &t))
            for (unsigned i = 0; i < t.n && i < MODEL_TEX; i++) {
                Cand *c = &cand[nc];
                if (!bti_hash(t.hdr + 0x20 * i, t.avail - 0x20 * i, &c->fmt, &c->w, &c->h, &c->hash)) continue;
                snprintf(s_tex_name[b][i], sizeof s_tex_name[b][i], "%s", tex1_name(&t, i, bmd + size));
                c->id = (uint16_t)(ID_MODEL + b * MODEL_TEX + i), c->base = (int8_t)b;
                nc++;
            }
        free(bmd);
    }
    for (unsigned i = 0; i < nc; i++) {
        if (!charpack_has(cand[i].base)) continue;
        bool shared = false;
        for (unsigned j = 0; j < nc && !shared; j++) shared = cand[j].hash == cand[i].hash && cand[j].base != cand[i].base;
        if (!shared) gx_subst_register(cand[i].id, cand[i].fmt, cand[i].w, cand[i].h, cand[i].hash), nreg++;
    }
    free(cand);
    /* faces (every character's: a page dims the ones without a variant), map icons, name tags */
    for (int b = 0; b < CP_BASES; b++) {
        char name[48];
        uint32_t size;
        snprintf(name, sizeof name, "%s.bti", FACE[b]);
        if ((s_face[b] = race2d_read(name, &size))) s_face_size[b] = size;
        reg_bti(GX_SUBST_CHARPACK + K_FACE * CP_BASES + b, s_face[b], s_face_size[b]);
        if (!charpack_has(b)) continue;
        snprintf(name, sizeof name, "%s_MapIcon32.bti", MAP[b]);
        uint8_t *p = race2d_read(name, &size);
        reg_bti(GX_SUBST_CHARPACK + K_MAP * CP_BASES + b, p, size), free(p);
        uint64_t off;
        snprintf(name, sizeof name, "chaname_%s.bti", NAME[b]);
        if (arc_locate(MENU_ARC, name, &off, &size) && size >= 0x20 && size < (1u << 20) && (p = malloc(size))) {
            disc_read(off, p, size);
            s_name_w = (int)be16(p + 2), s_name_h = (int)be16(p + 4);
            reg_bti(GX_SUBST_CHARPACK + K_NAME * CP_BASES + b, p, size), free(p);
        }
    }
    rt_log("charpack art: %u model textures recognised", nreg);
}

/* ---- the variants' pictures, made on first use (render thread) ---- */
/* by key, open addressing: a few hundred lookups a frame (every bind of a stand-in texture) */
typedef struct { uint32_t key; GLuint tex; bool used; } Made; /* tex 0: the variant has none */
#define MADE_SLOTS 16384 /* a power of 2, over CP_VARIANTS x (3 pictures + MODEL_TEX) */
static Made s_made[MADE_SLOTS];
static unsigned s_nmade;
static uint8_t *s_bmd[CP_VARIANTS];
static size_t s_bmd_size[CP_VARIANTS];
static bool s_bmd_tried[CP_VARIANTS];

/* A BTI at h decoded to RGBA (malloc'd), w x h out. */
static uint8_t *decode_bti(const uint8_t *h, size_t avail, int *w, int *ht) {
    if (avail < 0x20) return NULL;
    GxTexture t = {0};
    t.fmt = h[0], t.width = be16(h + 2), t.height = be16(h + 4);
    size_t off = be32(h + 0x1C), bytes = gx_tex_size(t.fmt, t.width, t.height);
    if (!bytes || off + bytes > avail || t.width > 1024 || t.height > 1024) return NULL;
    t.raw = (uint8_t *)h + off, t.raw_len = bytes;
    uint8_t tlut[512] = {0};
    if (t.fmt >= 8 && t.fmt <= 10) { /* C4, C8, C14X2: the palette */
        size_t po = be32(h + 0xC), pn = be16(h + 0xA) * 2u;
        if (pn > sizeof tlut) pn = sizeof tlut;
        if (po + pn <= avail) memcpy(tlut, h + po, pn);
        t.tlut = tlut, t.tlut_fmt = h[9];
    }
    uint8_t *rgba = malloc((size_t)t.width * t.height * 4);
    if (rgba && !gx_texture_decode(&t, rgba)) free(rgba), rgba = NULL;
    *w = (int)t.width, *ht = (int)t.height;
    return rgba;
}
static GLuint upload(const uint8_t *rgba, int w, int h) {
    if (!rgba) return 0;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    return tex;
}
static GLuint upload_bti(const uint8_t *h, size_t avail) {
    int w, ht;
    uint8_t *rgba = decode_bti(h, avail, &w, &ht);
    GLuint tex = upload(rgba, w, ht);
    free(rgba);
    return tex;
}
static uint8_t *read_picture(int v, char kind, const char *game, int *w, int *h) {
    char path[256];
    size_t n = 0;
    if (!charpack_path(v, kind, game, path, sizeof path)) return NULL;
    uint8_t *f = read_all(path, &n), *rgba = f ? decode_bti(f, n, w, h) : NULL;
    free(f);
    return rgba;
}

/* the map icon from a face: a disc of it in a white ring, MAP_SIZE square */
static GLuint map_from_face(const uint8_t *face, int fw, int fh) {
    uint8_t out[MAP_SIZE * MAP_SIZE * 4];
    const float c = (MAP_SIZE - 1) / 2.0f, r_out = MAP_SIZE / 2.0f - 0.5f, ring = 2.5f;
    for (int y = 0; y < MAP_SIZE; y++)
        for (int x = 0; x < MAP_SIZE; x++) {
            uint8_t *o = out + 4 * (y * MAP_SIZE + x);
            float d = sqrtf((x - c) * (x - c) + (y - c) * (y - c));
            if (d > r_out) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
            if (d > r_out - ring) { o[0] = o[1] = o[2] = o[3] = 255; continue; }
            /* the face, box-filtered into this pixel */
            int sx0 = x * fw / MAP_SIZE, sx1 = (x + 1) * fw / MAP_SIZE, sy0 = y * fh / MAP_SIZE, sy1 = (y + 1) * fh / MAP_SIZE;
            unsigned acc[4] = {0}, cnt = 0;
            for (int sy = sy0; sy < (sy1 > sy0 ? sy1 : sy0 + 1); sy++)
                for (int sx = sx0; sx < (sx1 > sx0 ? sx1 : sx0 + 1); sx++, cnt++)
                    for (int k = 0; k < 4; k++) acc[k] += face[4 * (sy * fw + sx) + k];
            /* over white, as the ring's inside */
            unsigned a = acc[3] / cnt;
            for (int k = 0; k < 3; k++) o[k] = (uint8_t)((acc[k] / cnt * a + 255 * (255 - a)) / 255);
            o[3] = 255;
        }
    return upload(out, MAP_SIZE, MAP_SIZE);
}

/* the name tag: s in MarioFont, white, centred in the game's tag size (squeezed when too wide) */
static GLuint name_tag(const char *s) {
    int aw, ah;
    const uint8_t *atlas = uifont_atlas(&aw, &ah);
    if (!atlas && uifont_load()) atlas = uifont_atlas(&aw, &ah);
    if (!atlas) return 0;
    int w = s_name_w, h = s_name_h;
    uint8_t *out = calloc((size_t)w * h, 4);
    if (!out) return 0;
    const float cell = 32, scale = (float)h / cell;
    float total = 0;
    for (const char *c = s; *c; c++) {
        float uv[4], x0, x1;
        total += *c == ' ' ? cell * 0.4f : uifont_glyph(*c, uv, &x0, &x1) ? (x1 - x0) + 1 : 0;
    }
    float sx = total * scale > w - 4 ? (w - 4) / total : scale, pen = (w - total * sx) / 2;
    for (const char *c = s; *c; c++) {
        float uv[4], x0, x1;
        if (*c == ' ' || !uifont_glyph(*c, uv, &x0, &x1)) { pen += *c == ' ' ? cell * 0.4f * sx : 0; continue; }
        float gw = x1 - x0 + 1;
        for (int y = 0; y < h; y++)
            for (int x = (int)pen; x < (int)(pen + gw * sx) && x < w; x++) {
                if (x < 0) continue;
                /* the glyph's cell texel under this pixel */
                float gx = x0 + (x - pen) / sx, gy = y / scale;
                int ax = (int)(uv[0] * aw + gx), ay = (int)(uv[1] * ah + gy);
                if (ax < 0 || ay < 0 || ax >= aw || ay >= ah) continue;
                const uint8_t *t = atlas + 2 * ((size_t)ay * aw + ax);
                uint8_t *o = out + 4 * ((size_t)y * w + x);
                if (t[1] > o[3]) o[0] = o[1] = o[2] = t[0], o[3] = t[1];
            }
        pen += gw * sx;
    }
    GLuint tex = upload(out, w, h);
    free(out);
    return tex;
}

static GLuint make(int v, int b, unsigned kind, unsigned i) {
    int w = 0, h = 0;
    /* the variant's own pictures are named after its base (MKDD Patcher layout): a face shown in
     * another character's cell too */
    int vb = kind == K_DIM ? b : charpack_variant_base(v);
    if (vb < 0) vb = b;
    if (kind == K_DIM) { /* no character here: an empty slot, a faint checkered flag in the face's shape */
        uint8_t *rgba = s_face[b] ? decode_bti(s_face[b], s_face_size[b], &w, &h) : NULL;
        for (int y = 0; rgba && y < h; y++)
            for (int x = 0; x < w; x++) {
                uint8_t *px = rgba + 4 * (y * w + x);
                /* the flag waves: its squares (w / 8 across) bend with a sine down the tile */
                int sq = w / 8 > 0 ? w / 8 : 1, wave = (int)(sinf((float)y * 6.2832f / (float)h + (float)x * 0.08f) * sq * 0.35f);
                bool dark = ((x / sq) + ((y + wave) / sq + 64)) & 1;
                float dx = (x - w * 0.5f) / (w * 0.5f), dy = (y - h * 0.5f) / (h * 0.5f);
                float shade = 1.0f - 0.35f * (dx * dx + dy * dy); /* darker towards the edges */
                int g = (int)((dark ? 38 : 72) * shade);
                px[0] = (uint8_t)g, px[1] = (uint8_t)g, px[2] = (uint8_t)(g + 10); /* a slate grey */
                px[3] = (uint8_t)(px[3] * 85 / 100);
            }
        GLuint tex = upload(rgba, w, h);
        free(rgba);
        return tex;
    }
    if (kind == K_FACE || kind == K_NAME) {
        uint8_t *rgba = read_picture(v, kind == K_FACE ? 'f' : 'n', kind == K_FACE ? FACE[vb] : NAME[vb], &w, &h);
        /* no face of its own: its base's (not the game's picture of the cell it shows in) */
        if (!rgba && kind == K_FACE && vb < CP_BASES && s_face[vb]) rgba = decode_bti(s_face[vb], s_face_size[vb], &w, &h);
        GLuint tex = rgba ? upload(rgba, w, h) : kind == K_NAME ? name_tag(charpack_name(v)) : 0;
        free(rgba);
        return tex;
    }
    if (kind == K_MAP) {
        uint8_t *rgba = read_picture(v, 'm', MAP[vb], &w, &h);
        GLuint tex;
        if (rgba) tex = upload(rgba, w, h);
        else tex = (rgba = read_picture(v, 'f', FACE[vb], &w, &h)) ? map_from_face(rgba, w, h) : 0;
        free(rgba);
        return tex;
    }
    /* a model texture: the variant's texture of the same name, else the same place */
    if (v >= CP_VARIANTS) return 0;
    if (!s_bmd_tried[v]) {
        char path[256];
        s_bmd_tried[v] = true;
        if (charpack_path(v, 'd', "driver.bmd", path, sizeof path)) s_bmd[v] = read_all(path, &s_bmd_size[v]);
    }
    Tex1 t;
    if (!s_bmd[v] || !tex1_find(s_bmd[v], s_bmd_size[v], &t)) return 0;
    unsigned k = i;
    for (unsigned j = 0; j < t.n; j++)
        if (s_tex_name[b][i][0] && !strcmp(tex1_name(&t, j, s_bmd[v] + s_bmd_size[v]), s_tex_name[b][i])) { k = j; break; }
    return k < t.n ? upload_bti(t.hdr + 0x20 * k, t.avail - 0x20 * k) : 0;
}

unsigned charpack_art_texture(unsigned id) {
    if (id < GX_SUBST_CHARPACK) return 0;
    unsigned rel = id - GX_SUBST_CHARPACK, kind, b, i = 0;
    if (rel < K_KINDS * CP_BASES) kind = rel / CP_BASES, b = rel % CP_BASES;
    else if ((rel -= K_KINDS * CP_BASES) < CP_BASES * MODEL_TEX) kind = K_MODEL, b = rel / MODEL_TEX, i = rel % MODEL_TEX;
    else if (id >= CP_ROW3_SUBST && id < CP_ROW3_SUBST + CP_ROW) kind = K_ROW3;
    else if (id >= CP_HUD_SUBST && id < CP_ROW3_SUBST) kind = K_HUD;
    else return 0;
    /* faces: what each cell shows (the grid; in a race, each character's); model textures and name
     * tags: the variant standing in for that character (the one previewed or chosen); the race HUD's
     * copies (native.c hud_face): their kart's driver of that character */
    int v;
    if (kind == K_ROW3) { /* the third row's cell: its variant on this page, else empty */
        v = charpack_cell_variant(CP_BASES + (int)(id - CP_ROW3_SUBST));
        kind = K_FACE, b = v >= 0 ? (unsigned)charpack_variant_base(v) : 0;
        if (v < 0) v = CP_DIM;
    } else if (kind == K_HUD) {
        int hk, kart, slot, hb;
        if (!charpack_hud_copy_info((int)(id - CP_HUD_SUBST), &hk, &kart, &slot, &hb)) return 0;
        v = charpack_race_face(kart, slot, hb);
        kind = hk == CP_HUD_MAP ? K_MAP : K_FACE, b = (unsigned)hb;
        if (v == CP_RACE_NONE) v = charpack_show(hb);
    } else v = kind == K_FACE || kind == K_MAP ? charpack_show((int)b) : charpack_model((int)b);
    if (v == CP_DIM && kind == K_FACE) kind = K_DIM, v = 0;
    else if (v == CP_DIM) return 0;
    else if (v < 0) return 0;
    uint32_t key = (uint32_t)v << 16 | kind << 12 | (kind == K_DIM ? b : i);
    unsigned j = (key * 2654435761u) & (MADE_SLOTS - 1);
    for (; s_made[j].used; j = (j + 1) & (MADE_SLOTS - 1))
        if (s_made[j].key == key) return s_made[j].tex;
    GLint bound = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    GLuint tex = make(v, (int)b, kind, i);
    glBindTexture(GL_TEXTURE_2D, (GLuint)bound);
    if (s_nmade < MADE_SLOTS - 1) s_made[j] = (Made){key, tex, true}, s_nmade++; /* else (never: see MADE_SLOTS) made each time */
    if (!tex && kind != K_DIM)
        rt_log("charpack art: %s has no %s picture %u", charpack_name(v), kind == K_FACE ? "face" : kind == K_MAP ? "map" : kind == K_NAME ? "name" : "model", i);
    return tex;
}
