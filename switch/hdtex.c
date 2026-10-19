/* HD texture replacements (hdtex.h). File layout: tools/hdtex_pack.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hdtex.h"
#include "../runtime/plat.h"
#include "../runtime/rt.h"

/* ---- XXH64 (Dolphin names a replacement by the XXH64 of the texture's level-0 bytes, seed 0) ---- */
#define P1 11400714785074694791ull
#define P2 14029467366897019727ull
#define P3 1609587929392839161ull
#define P4 9650029242287828579ull
#define P5 2870177450012600261ull
static inline uint64_t rotl64(uint64_t x, int r) { return x << r | x >> (64 - r); }
static inline uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint64_t xround(uint64_t acc, uint64_t in) { return rotl64(acc + in * P2, 31) * P1; }
static inline uint64_t xmerge(uint64_t h, uint64_t v) { return (h ^ xround(0, v)) * P1 + P4; }

uint64_t xxh64(const void *data, size_t len, uint64_t seed) {
    const uint8_t *p = data, *end = p + len;
    uint64_t h;
    if (len >= 32) {
        uint64_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        const uint8_t *lim = end - 32;
        do {
            v1 = xround(v1, rd64(p)); v2 = xround(v2, rd64(p + 8));
            v3 = xround(v3, rd64(p + 16)); v4 = xround(v4, rd64(p + 24));
            p += 32;
        } while (p <= lim);
        h = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
        h = xmerge(h, v1); h = xmerge(h, v2); h = xmerge(h, v3); h = xmerge(h, v4);
    } else {
        h = seed + P5;
    }
    h += len;
    for (; p + 8 <= end; p += 8) h = rotl64(h ^ xround(0, rd64(p)), 27) * P1 + P4;
    if (p + 4 <= end) { h = rotl64(h ^ (uint64_t)rd32(p) * P1, 23) * P2 + P3; p += 4; }
    for (; p < end; p++) h = rotl64(h ^ *p * P5, 11) * P1;
    h ^= h >> 33; h *= P2; h ^= h >> 29; h *= P3; h ^= h >> 32;
    return h;
}

/* ---- index ---- */
typedef struct __attribute__((packed)) {
    uint64_t hash, tlut;
    uint16_t w, h, W, H;
    uint8_t fmt, flags, levels, pad;
    uint32_t size;
    uint64_t off;
} Entry;
_Static_assert(sizeof(Entry) == 40, "index entry");

static Entry *s_idx;
static unsigned s_n;
static char s_path[256];

/* ---- loader thread: requests in, finished loads out ---- */
#define QCAP 256
typedef struct { int idx; uint64_t tag; uint8_t *data; } Job;
static Job s_req[QCAP], s_done[QCAP];
static unsigned s_req_head, s_req_n, s_done_head, s_done_n;
static unsigned s_loads, s_fails;
static uint64_t s_bytes;
static PMutex s_mu;
static PCond s_cv;

static void loader(void *arg) {
    (void)arg;
    FILE *f = fopen(s_path, "rb");
    for (;;) {
        pmutex_lock(&s_mu);
        while (!s_req_n || s_done_n == QCAP) pcond_wait(&s_cv, &s_mu);
        Job j = s_req[s_req_head];
        s_req_head = (s_req_head + 1) % QCAP;
        s_req_n--;
        pmutex_unlock(&s_mu);

        const Entry *e = &s_idx[j.idx];
        j.data = f ? malloc(e->size) : NULL;
        if (j.data && (fseeko(f, (off_t)e->off, SEEK_SET) || fread(j.data, 1, e->size, f) != e->size)) {
            free(j.data);
            j.data = NULL;
        }
        pmutex_lock(&s_mu);
        s_loads++;
        if (j.data) s_bytes += e->size;
        else s_fails++;
        s_done[(s_done_head + s_done_n) % QCAP] = j;
        s_done_n++;
        pmutex_unlock(&s_mu);
    }
}

bool hdtex_open(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { rt_log("hd textures: no %s", path); return false; }
    struct __attribute__((packed)) { char magic[8]; uint32_t version, count, enc, zero; uint64_t index; } hd;
    if (fread(&hd, sizeof hd, 1, f) != 1 || memcmp(hd.magic, "MKDDHDT1", 8) || hd.version != 1 || hd.enc != 1 || !hd.count) {
        rt_log("hd textures: %s is not a texture file from tools/hdtex_pack.py (version 1)", path);
        fclose(f);
        return false;
    }
    s_idx = malloc((size_t)hd.count * sizeof *s_idx);
    if (!s_idx || fseeko(f, (off_t)hd.index, SEEK_SET) || fread(s_idx, sizeof *s_idx, hd.count, f) != hd.count) {
        rt_log("hd textures: %s: index unreadable", path);
        free(s_idx);
        s_idx = NULL;
        fclose(f);
        return false;
    }
    fclose(f);
    s_n = hd.count;
    snprintf(s_path, sizeof s_path, "%s", path);
    pmutex_init(&s_mu);
    pcond_init(&s_cv);
    /* core 0 (the render thread's, which mostly waits for the GPU), below it in priority: core 2
     * belongs to the GX decoder */
    pthread_start("mkdd-hdtex", loader, NULL, 64 << 10, 0, 1);
    rt_log("hd textures: %u replacements in %s", s_n, path);
    return true;
}

bool hdtex_available(void) { return s_n != 0; }

static int find_hash(uint64_t hash, uint32_t fmt, uint32_t w, uint32_t h) {
    unsigned lo = 0, hi = s_n;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (s_idx[mid].hash < hash) lo = mid + 1;
        else hi = mid;
    }
    for (unsigned i = lo; i < s_n && s_idx[i].hash == hash; i++)
        if (s_idx[i].w == w && s_idx[i].h == h && s_idx[i].fmt == fmt) return (int)i;
    return -1;
}

/* MKDD Plus 3.3 shrinks the course select pictures to 97x70 (CMPR) to fit one per course for each
 * of its five track sets. Its pictures of the 16 original courses (set 0) are the game's 256x184
 * ones scaled down, so they get the HD pack's replacement of the original. Pairs of XXH64 hashes
 * (Plus picture, original picture), matched by comparing the images. */
static const uint64_t PLUS_PICTURES[16][2] = {
    {0x4808e9e486c81173ull, 0xf9cba123e9b85332ull}, /* Baby Park */
    {0xd68b1f386f232047ull, 0x96a90d4eaf298fcbull}, /* Bowser's Castle */
    {0x44986d8abd793588ull, 0x68bb299bc94e7634ull}, /* Daisy Cruiser */
    {0x37026c89a14e77cdull, 0x339e6359acd014aaull}, /* Dino Dino Jungle */
    {0xa0faf1be851d0ffbull, 0xd655a9a4e6f6db30ull}, /* DK Mountain */
    {0x2d18a6198fa30246ull, 0x8d3d28dedcad2918ull}, /* Dry Dry Desert */
    {0x90e7ac567ff548bdull, 0xbac2911127470008ull}, /* Mushroom Bridge */
    {0x3a07e7970614b4ecull, 0x0e73a95666d15996ull}, /* Mushroom City */
    {0x3c7357e7e060c2baull, 0xceba35d8c0bb588dull}, /* Luigi Circuit */
    {0x152ffbfca45548d4ull, 0xcd42e5c6e03811bfull}, /* Mario Circuit */
    {0xf9695a7cb46e85eeull, 0x2c3e5929d385690eull}, /* Peach Beach */
    {0x74341ec02acd3897ull, 0xae0be49ce04bca08ull}, /* Rainbow Road */
    {0xa9401bd1cf26dfc0ull, 0xec570e75a4783eb2ull}, /* Sherbet Land */
    {0x09b7ea753c27ee3dull, 0x5e41fe116c35678cull}, /* Waluigi Stadium */
    {0x12df48cd6ab9f6feull, 0x4f0539b36f86a987ull}, /* Wario Colosseum */
    {0x348d73d3fb5da663ull, 0xd503a93c7e3691a4ull}, /* Yoshi Circuit */
};

int hdtex_find(uint32_t fmt, uint32_t w, uint32_t h, const uint8_t *raw, size_t len) {
    if (!s_n || fmt == 8 || fmt == 9 || fmt == 10) return -1; /* paletted: not in the MKDD pack */
    uint64_t hash = xxh64(raw, len, 0);
    int i = find_hash(hash, fmt, w, h);
    if (i < 0 && fmt == 14 && w == 97 && h == 70)
        for (unsigned k = 0; k < 16; k++)
            if (PLUS_PICTURES[k][0] == hash) return find_hash(PLUS_PICTURES[k][1], 14, 256, 184);
    return i;
}

void hdtex_info(int idx, unsigned *w, unsigned *h, unsigned *levels) {
    *w = s_idx[idx].W;
    *h = s_idx[idx].H;
    *levels = s_idx[idx].levels;
}

bool hdtex_request(int idx, uint64_t tag) {
    pmutex_lock(&s_mu);
    bool ok = s_req_n < QCAP;
    if (ok) {
        s_req[(s_req_head + s_req_n) % QCAP] = (Job){idx, tag, NULL};
        s_req_n++;
        pcond_signal(&s_cv);
    }
    pmutex_unlock(&s_mu);
    return ok;
}

void hdtex_stats(unsigned *loads, unsigned *fails, uint64_t *bytes, unsigned *queued) {
    pmutex_lock(&s_mu);
    *loads = s_loads; *fails = s_fails; *bytes = s_bytes; *queued = s_req_n;
    pmutex_unlock(&s_mu);
}

bool hdtex_take(int *idx, uint64_t *tag, uint8_t **data) {
    pmutex_lock(&s_mu);
    bool ok = s_done_n != 0;
    if (ok) {
        Job j = s_done[s_done_head];
        s_done_head = (s_done_head + 1) % QCAP;
        s_done_n--;
        *idx = j.idx; *tag = j.tag; *data = j.data;
        pcond_signal(&s_cv);
    }
    pmutex_unlock(&s_mu);
    return ok;
}
