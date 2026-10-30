/* Character packs' voices: a variant's sounds in place of its base's.
 *
 * A pack for the MKDD Patcher ships them as audio_waves/<archive>/<wave id>.wav (tools/charpack.py
 * copies that folder to the character's voice/): Voice (in the race), SelectVoice (the select
 * screen), CommendationVoice (the awards), and the other three of GCKart.baa's wave systems. The
 * Patcher rebuilds the game's wave archive and wave table with them; here nothing of the game's
 * changes. Each sound is matched where it plays: when the DSP (dsp.c) starts a channel on a wave in
 * ARAM, the wave's first and last 32 bytes are compared with each replaced wave's bytes from the
 * disc's .aw, and a match plays the variant's samples instead (at their own rate: the pitch scaled by
 * its rate over the original's). A stereo one is mixed to mono, and it never loops. As the MKDD
 * Patcher and MKDD Plus do, a sound longer than the one it replaces is cut to that one's length
 * (the game's sound tables are made for the original lengths). tools/charpack.py already makes
 * them so on the PC (mono, no higher rate, cut); this is for files copied by hand.
 *
 * The game keeps 36 clips twice or three times (a line in Voice_0.aw and SelectVoice_0.aw, same
 * bytes and rate): replacing one replaces its copies too, the same line wherever it plays.
 *
 * The replacements apply while their variant stands in for its base: the select screen's previewed
 * or chosen one (charpack_model), in a race the driver of that character who last started a voice
 * line (charpack_voice_on: two drivers of one character, as a variant and the game's own or two
 * variants, each speak as their own). A variant's files are read from the SD card
 * the first time it is active and kept (host memory, VOICE_BUDGET at most in all).
 *
 * Wave tables: GCKart.baa holds a WSYS per wave archive: +0x10 WINF, +0x14 WBCT (offsets from the
 * WSYS). WINF: count, group offsets; a group: the .aw's name (0x70 bytes), count, wave offsets; a
 * wave: +1 format, +4 sample rate (float), +8 offset in the .aw, +0xc bytes, +0x1c samples. WBCT:
 * +8 count, scene offsets; a scene: +0xc its C-DF list (count, offsets of {s16 group, s16 wave id}),
 * entry i naming group wave i. The wave id is the number in the file name. */
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "charpack.h"

#define MATCH 32
#define VOICE_BUDGET (48u << 20)
#define TABLE_MAX 512

/* the Patcher's folder names, each its .aw */
static const char *const ARC_DIRS[] = {"SelectVoice", "Voice", "CommendationVoice", "SoundEffects", "NintendoLogo", "BGMSamples"};
static const char *const ARC_FILES[] = {"SelectVoice_0.aw", "Voice_0.aw", "CommendationVoice_0.aw", "se00_0.aw", "NintendoLogoMario_0.aw", "bgm_0.aw"};
#define ARCS (sizeof ARC_DIRS / sizeof ARC_DIRS[0])

typedef struct {
    int id;
    uint32_t off, bytes, samples;
    float rate;
} Wave;
typedef struct {
    Wave *w;
    unsigned n;
    uint64_t aw; /* the .aw on the disc */
    uint32_t aw_size;
} Arc;
static Arc s_arc[ARCS];
static int s_tables = -1; /* -1 not read yet, 0 unreadable, 1 read */

typedef struct {
    uint8_t head[MATCH], tail[MATCH];
    uint32_t bytes;
    CharVoiceSound snd;
    const char *what; /* for the log: "Voice 264" */
    bool said;
} Rep;
typedef struct {
    bool loaded;
    Rep *r;
    unsigned n;
} VarVoice;
static VarVoice *s_var;
static unsigned s_nvar;
static size_t s_used;

/* what plays now: two tables, the DSP reading the one s_cur points at while the other is rebuilt */
typedef struct {
    unsigned n;
    Rep *r[TABLE_MAX];
    int v[TABLE_MAX]; /* the variant r[i] is of */
} Table;
static Table s_tab[2];
static Table *volatile s_cur;
volatile unsigned g_charvoice_on;
#define ACTIVE_MAX 36
static int s_active[ACTIVE_MAX]; /* the variants s_cur was made for */
static unsigned s_nactive;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
static unsigned le16(const uint8_t *p) { return (unsigned)p[1] << 8 | p[0]; }

/* one WSYS at b (n bytes left in the file) into its archive's entry */
static void read_wsys(const uint8_t *d, size_t n) {
#define IN(o, len) ((uint64_t)(o) + (len) <= n)
    if (!IN(0, 0x18)) return;
    uint32_t winf = be32(d + 0x10), wbct = be32(d + 0x14);
    if (!IN(winf, 8) || !IN(wbct, 12) || memcmp(d + winf, "WINF", 4) || memcmp(d + wbct, "WBCT", 4)) return;
    uint32_t ng = be32(d + winf + 4), ns = be32(d + wbct + 8);
    for (uint32_t g = 0; g < ng && g < ns && IN(winf + 8 + 4 * g, 4) && IN(wbct + 12 + 4 * g, 4); g++) {
        uint32_t go = be32(d + winf + 8 + 4 * g), so = be32(d + wbct + 12 + 4 * g);
        if (!IN(go, 0x74) || !IN(so, 0x10)) continue;
        char name[0x71];
        memcpy(name, d + go, 0x70), name[0x70] = 0;
        unsigned a = 0;
        while (a < ARCS && strcasecmp(name, ARC_FILES[a])) a++;
        uint32_t cdf = be32(d + so + 0xc), nw = be32(d + go + 0x70);
        if (a == ARCS || s_arc[a].w || !IN(cdf, 8) || memcmp(d + cdf, "C-DF", 4) || nw > 4096 || be32(d + cdf + 4) < nw) continue;
        Wave *w = calloc(nw ? nw : 1, sizeof *w);
        if (!w) continue;
        unsigned k = 0;
        for (uint32_t i = 0; i < nw; i++) {
            if (!IN(go + 0x74 + 4 * i, 4) || !IN(cdf + 8 + 4 * i, 4)) break;
            uint32_t wo = be32(d + go + 0x74 + 4 * i), eo = be32(d + cdf + 8 + 4 * i);
            if (!IN(wo, 0x20) || !IN(eo, 4)) continue;
            uint32_t r = be32(d + wo + 4);
            float rate;
            memcpy(&rate, &r, 4);
            w[k++] = (Wave){(int16_t)(d[eo + 2] << 8 | d[eo + 3]), be32(d + wo + 8), be32(d + wo + 0xc), be32(d + wo + 0x1c), rate};
        }
        s_arc[a].w = w, s_arc[a].n = k;
        char path[96];
        snprintf(path, sizeof path, "AudioRes/Waves/%s", ARC_FILES[a]);
        if (!disc_find(path, &s_arc[a].aw, &s_arc[a].aw_size)) s_arc[a].aw_size = 0;
        rt_log("charvoice: %s: %u waves", ARC_FILES[a], k);
    }
#undef IN
}

static bool tables(void) {
    if (s_tables >= 0) return s_tables;
    s_tables = 0;
    uint64_t off;
    uint32_t size;
    if (!disc_find("AudioRes/GCKart.baa", &off, &size) || size < 0x20 || size > (16u << 20)) {
        rt_log("charvoice: no AudioRes/GCKart.baa on the disc: character voices off");
        return false;
    }
    uint8_t *d = malloc(size);
    if (!d) return false;
    disc_read(off, d, size);
    for (uint32_t i = 0; i + 0x18 <= size; i += 4)
        if (!memcmp(d + i, "WSYS", 4)) read_wsys(d + i, size - i);
    free(d);
    s_tables = 1;
    return true;
}

/* a .wav (16- or 8-bit PCM, any channels) as mono 16-bit, malloc'd; NULL when it is none */
static int16_t *read_wav(const char *path, uint32_t *n, uint32_t *rate) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *d = NULL;
    int16_t *out = NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 44 || size > (32 << 20) || !(d = malloc((size_t)size)) || fread(d, 1, (size_t)size, f) != (size_t)size) goto done;
    if (memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) goto done;
    unsigned ch = 0, bits = 0, fmt = 0;
    const uint8_t *data = NULL;
    uint32_t data_n = 0;
    for (long o = 12; o + 8 <= size;) {
        uint32_t len = le32(d + o + 4);
        if ((uint64_t)o + 8 + len > (uint64_t)size) len = (uint32_t)(size - o - 8);
        if (!memcmp(d + o, "fmt ", 4) && len >= 16) {
            fmt = le16(d + o + 8), ch = le16(d + o + 10), *rate = le32(d + o + 12), bits = le16(d + o + 22);
            if (fmt == 0xFFFE && len >= 26) fmt = le16(d + o + 32); /* WAVE_FORMAT_EXTENSIBLE: its subformat */
        } else if (!memcmp(d + o, "data", 4)) data = d + o + 8, data_n = len;
        o += 8 + len + (len & 1);
    }
    if (fmt != 1 || !ch || ch > 8 || (bits != 16 && bits != 8) || !data || *rate < 1000 || *rate > 192000) goto done;
    unsigned frame = ch * bits / 8;
    *n = data_n / frame;
    if (!*n || !(out = malloc(*n * 2))) goto done;
    for (uint32_t i = 0; i < *n; i++) {
        int32_t s = 0;
        for (unsigned c = 0; c < ch; c++) {
            const uint8_t *p = data + i * frame + c * bits / 8;
            s += bits == 16 ? (int16_t)le16(p) : ((int32_t)p[0] - 128) << 8;
        }
        out[i] = (int16_t)(s / (int32_t)ch);
    }
done:
    free(d);
    fclose(f);
    return out;
}

/* variant v's sounds, read once */
static VarVoice *load(int v) {
    if (v < 0 || (unsigned)v >= charpack_count()) return NULL;
    if (!s_var) {
        s_nvar = charpack_count();
        s_var = calloc(s_nvar, sizeof *s_var);
        if (!s_var) return NULL;
    }
    VarVoice *vv = &s_var[v];
    if (vv->loaded) return vv;
    vv->loaded = true;
    char dir[224];
    if (!charpack_voice_dir(v, dir, sizeof dir) || !tables()) return vv;
    unsigned cap = 0;
    for (unsigned a = 0; a < ARCS; a++) {
        if (!s_arc[a].n || !s_arc[a].aw_size) continue;
        char sub[256];
        snprintf(sub, sizeof sub, "%s/%s", dir, ARC_DIRS[a]);
        DIR *dd = opendir(sub);
        if (!dd) continue;
        struct dirent *e;
        char names[TABLE_MAX][16];
        unsigned nn = 0;
        while ((e = readdir(dd)) && nn < TABLE_MAX) {
            size_t l = strlen(e->d_name);
            if (l > 4 && l < sizeof names[0] && !strcasecmp(e->d_name + l - 4, ".wav")) snprintf(names[nn++], sizeof names[0], "%.15s", e->d_name);
        }
        closedir(dd);
        for (unsigned i = 0; i < nn; i++) {
            char *end;
            long id = strtol(names[i], &end, 10);
            const Wave *w = NULL;
            for (unsigned k = 0; *end == '.' && k < s_arc[a].n && !w; k++)
                if (s_arc[a].w[k].id == id) w = &s_arc[a].w[k];
            char path[288];
            snprintf(path, sizeof path, "%s/%s", sub, names[i]);
            if (!w || w->bytes < 2 * MATCH || (uint64_t)w->off + w->bytes > s_arc[a].aw_size || w->rate <= 0) {
                rt_log("charvoice: %s: no wave %s in %s", path, names[i], ARC_FILES[a]);
                continue;
            }
            if (s_used > VOICE_BUDGET) {
                rt_log("charvoice: %s: over %u MiB of voices, left out", path, VOICE_BUDGET >> 20);
                continue;
            }
            uint32_t n = 0, rate = 0;
            int16_t *pcm = read_wav(path, &n, &rate);
            if (!pcm) {
                rt_log("charvoice: %s: not a 16- or 8-bit PCM .wav", path);
                continue;
            }
            uint32_t most = (uint32_t)((double)w->samples * rate / w->rate); /* the original's length */
            if (most && n > most) n = most;
            if (vv->n == cap) {
                Rep *r = realloc(vv->r, (cap = cap ? 2 * cap : 16) * sizeof *r);
                if (!r) {
                    free(pcm);
                    break;
                }
                vv->r = r;
            }
            Rep *r = &vv->r[vv->n++];
            memset(r, 0, sizeof *r);
            disc_read(s_arc[a].aw + w->off, r->head, MATCH);
            disc_read(s_arc[a].aw + w->off + w->bytes - MATCH, r->tail, MATCH);
            r->bytes = w->bytes;
            r->snd = (CharVoiceSound){pcm, n, (float)(rate / w->rate)};
            char what[40];
            snprintf(what, sizeof what, "%s %ld", ARC_DIRS[a], id);
            r->what = strdup(what);
            s_used += (size_t)n * 2;
        }
    }
    rt_log("charvoice: %s: %u sounds from %s", charpack_name(v), vv->n, dir);
    return vv;
}

void charvoice_update(void) {
    if (!charpack_count()) return;
    int m[ACTIVE_MAX], all[ACTIVE_MAX];
    unsigned na = charpack_voice_variants(all, ACTIVE_MAX), n = 0;
    for (unsigned i = 0; i < na; i++)
        if (charpack_voice_dir(all[i], NULL, 0)) m[n++] = all[i];
    bool same = s_cur != NULL && n == s_nactive;
    for (unsigned i = 0; same && i < n; i++) same = m[i] == s_active[i];
    if (same) return;
    Table *t = s_cur == &s_tab[0] ? &s_tab[1] : &s_tab[0];
    t->n = 0;
    s_nactive = n;
    for (unsigned j = 0; j < n; j++) {
        s_active[j] = m[j];
        VarVoice *vv = load(m[j]);
        for (unsigned i = 0; vv && i < vv->n && t->n < TABLE_MAX; i++) t->v[t->n] = m[j], t->r[t->n++] = &vv->r[i];
        if (vv && vv->n) rt_log("charvoice: %s's voice ready", charpack_name(m[j]));
    }
    __atomic_store_n(&s_cur, t, __ATOMIC_RELEASE);
    g_charvoice_on = t->n;
}

bool charvoice_match(uint32_t wave, CharVoiceSound *out) {
    Table *t = __atomic_load_n(&s_cur, __ATOMIC_ACQUIRE);
    if (!t || (uint64_t)wave + MATCH > ARAM_SIZE) return false;
    const uint8_t *p = g_aram + wave;
    for (unsigned i = 0; i < t->n; i++) {
        Rep *r = t->r[i];
        if (memcmp(p, r->head, MATCH) || (uint64_t)wave + r->bytes > ARAM_SIZE || memcmp(p + r->bytes - MATCH, r->tail, MATCH)) continue;
        if (!charpack_voice_on(t->v[i])) continue; /* another driver of the character speaks */
        if (!r->said) r->said = true, rt_log("charvoice: %s plays the character's own", r->what);
        *out = r->snd;
        return true;
    }
    return false;
}
