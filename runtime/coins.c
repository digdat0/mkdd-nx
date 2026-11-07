/* Coins: a layout made from each race course's own data, so the stock courses and every level pack
 * get one without anyone placing them.
 *
 * The course's BOL (header: +0x19 course ID, +0x1a CL point count, +0x1e object count, +0x44 CL
 * points, +0x54 objects, +0x58 start points) holds the computer karts' line: CL points (0x20 each:
 * position, +0xe group ID, +0x10 scale, the line's half width). Rows of COIN_ROW coins go every
 * so often along that line (about COIN_ROWS a lap), each shifted a little to one side within the
 * width, then every coin is put on the ground with the game's own collision (CrsGround::search):
 * kept only on road (COIN_ROAD attributes), near the line's height (not a bridge above or below),
 * away from the start line and the item boxes; a steep or very long step of the line (a ramp, a
 * gap, a cannon) gets none. The rows come from a seed of the BOL's header hash: the same layout
 * every race, on every console.
 *
 * Built on the race's first kart frame, when the course's collision is up (the BOL alone is there at
 * loadCrsData). With LOGGING on, logs/coins_<ID>_<hash>.txt holds the line, the item boxes, the start
 * and the coins (tools/coins_view.py draws it).
 *
 * A course's own layout can replace that one: coins/<course ID>_<BOL hash>.txt in the app folder (the
 * key battle routes use: a mod's course has its own), from tools/coin_editor.py. One coin a line, "x y z"
 * (or the dump's "coin x y z ..." lines, so a logged layout can be copied there); '#' starts a comment.
 * Each is put on the ground under it as the made ones are (y: where the ground is searched from, about
 * the road's height); with none found it stays at y. A file with no coins: none on that course.
 *
 * Every kart picks coins up (at most COIN_HOLD), each held one a little more top speed; a hit drops some.
 * Coins over water (Peach Beach) ride the tide: the game's own water height, every TIDE_EVERY frames. */
#include "native_impl.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "charpack.h"
#include "coins.h"
#include "rt.h"

volatile uint32_t g_coins;

#define CL_MAX 1024
#define COIN_ROWS 16          /* rows a lap, about */
#define COIN_SPACE_MIN 2000.0f /* the rows' spacing along the line, at least ... */
#define COIN_SPACE_MAX 9000.0f /* ... and at most */
#define COIN_ROW 4            /* coins in a row */
#define COIN_GAP 170.0f       /* between a row's coins */
#define COIN_SIDE 0.45f       /* a row at most this much of the line's half width to a side ... */
#define COIN_SIDE_MAX 700.0f  /* ... and at most this far */
#define COIN_LIFT 70.0f       /* above the ground */
#define COIN_PROBE 150.0f     /* the ground searched from this far above the line */
#define COIN_BELOW 400.0f     /* the ground at most this far below the line ... */
#define COIN_ABOVE 150.0f     /* ... or above it */
#define COIN_START 2500.0f    /* none this near the first start point */
#define COIN_BOX 450.0f       /* none this near an item box */
#define COIN_STEEP 0.45f      /* none on a step of the line climbing or falling more than this a unit */
#define COIN_LONG 4.0f        /* none on a step this many times the line's usual step (a jump, a cannon) */
#define COIN_JOIN 3.0f        /* a group's last point joins the next point when this near (usual steps) */
#define COIN_ROW_MIN 3        /* a row keeps at least this many coins, else none of it */
#define OBJ_ITEMBOX 1         /* SObject +0x24 objID */
#define BOX_MAX 256

#define CRS_GROUND_CT 0x8017FBA4u     /* CrsGround::CrsGround(Course *) (the RaceMgr's course) */
#define CRS_GROUND_SEARCH 0x8017FC88u /* CrsGround::search(const TVec3f &) */
#define CRS_GROUND_SIZE 0x60          /* +0x4c attribute, +0x58 height */

#define CRS_GROUND_WATER 0x801818A4u /* CrsGround::getWaterHeight() (f1; -1000 off water) */

/* The kart's top speed on these (KartCtrl::GetMaxSpeed: 1, 4, 11 the full speed) and a few more
 * the courses drive on at speed (6, 7, 8); 0, 3, 12 and 19 slow the kart (off-road), 2 and 18 are
 * walls, 5, 10, 14, 15 and 0xff out of the course (GeoRabbitMark's), 9 a cannon, 13 a pipe, 16 a
 * puller, 17 water (kept: Peach Beach's shallows are driven on; the coin floats on the surface). */
#define ATTR_WATER 17
static bool coin_road(uint32_t a) { return a == 1 || a == 4 || a == 6 || a == 7 || a == 8 || a == 11 || a == ATTR_WATER; }

static Coin s_coin[COIN_MAX];
static int s_ncoin, s_nwater; /* coins, those over water */
static bool s_pending, s_built;
static uint32_t s_bol, s_hash, s_id, s_ground;
static uint32_t s_tick; /* race frames (coins_frame), for the spin */
static void collect_reset(void);
static void gxcall(Ctx *c, uint8_t *M, uint32_t fn, const uint32_t *a, unsigned n);

void coins_course(Ctx *c, uint8_t *M, uint32_t bol, uint32_t hash, uint32_t mode) {
    s_ncoin = s_nwater = 0, s_built = false;
    collect_reset();
    s_pending = g_coins && ram_ptr(bol) && (mode == 2 || mode == 3); /* Grand Prix and versus: not time trial (its records and ghosts stay fair) or the battles */
    s_bol = bol, s_hash = hash, s_id = s_pending ? (uint32_t)mld(c, M, bol + 0x19, 1) : 0;
}

const Coin *coins_layout(int *n) {
    *n = s_built ? s_ncoin : 0;
    return s_built ? s_coin : NULL;
}

static uint32_t rng_next(uint32_t *s) {
    *s ^= *s << 13, *s ^= *s >> 17, *s ^= *s << 5;
    return *s;
}
static float dist2(const float *a, const float *b) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

/* the ground under p: its height and attribute (false: none found, or no collision yet) */
static bool ground_at(Ctx *c, uint8_t *M, const float *p, float *h, uint32_t *attr, float *water) {
    if (!s_ground) s_ground = charpack_reserve(CRS_GROUND_SIZE + 0x20);
    if (!s_ground) return false;
    uint32_t g = s_ground, v = s_ground + CRS_GROUND_SIZE;
    const uint32_t ct[2] = {g, 0};
    gxcall(c, M, CRS_GROUND_CT, ct, 2);
    if (!ram_ptr(ld32(c, M, g))) return false; /* no course */
    stf(c, M, v, p[0]), stf(c, M, v + 4, p[1] + COIN_PROBE), stf(c, M, v + 8, p[2]);
    const uint32_t sa[2] = {g, v};
    gxcall(c, M, CRS_GROUND_SEARCH, sa, 2);
    *h = ldf(c, M, g + 0x58);
    *attr = ld32(c, M, g + 0x4c);
    *water = -1000.0f;
    if (*attr == ATTR_WATER) {
        const uint32_t wa[1] = {g};
        gxcall(c, M, CRS_GROUND_WATER, wa, 1);
        uint64_t u = (uint64_t)RD(c, 0x4008, 8); /* f1 */
        double d;
        memcpy(&d, &u, 8);
        if (isfinite(d)) *water = (float)d;
    }
    return isfinite(*h) && *h > p[1] - COIN_BELOW && *h < p[1] + COIN_ABOVE;
}

typedef struct {
    int a, b;  /* CL points */
    float len; /* its length */
} Step;

static void dump(int n, const float (*pt)[3], const int16_t *grp, const float *sc, const float (*box)[3], int nbox,
                 const float *start, const Step *st, int nst) {
    char path[64];
    snprintf(path, sizeof path, "logs/coins_%02x_%08x.txt", (unsigned)s_id, (unsigned)s_hash);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# coins.c layout: course %#x hash %08x\n", (unsigned)s_id, (unsigned)s_hash);
    for (int i = 0; i < n; i++) fprintf(f, "cl %d %.1f %.1f %.1f %d %.1f\n", i, pt[i][0], pt[i][1], pt[i][2], grp[i], sc[i]);
    for (int i = 0; i < nst; i++) fprintf(f, "step %d %d\n", st[i].a, st[i].b);
    for (int i = 0; i < nbox; i++) fprintf(f, "box %.1f %.1f %.1f\n", box[i][0], box[i][1], box[i][2]);
    if (start) fprintf(f, "start %.1f %.1f %.1f\n", start[0], start[1], start[2]);
    for (int i = 0; i < s_ncoin; i++)
        fprintf(f, "coin %.1f %.1f %.1f %u %u\n", s_coin[i].pos[0], s_coin[i].pos[1], s_coin[i].pos[2], s_coin[i].attr, s_coin[i].water);
    fclose(f);
}

#define COIN_DIR "coins"

/* coins/<key>.txt (see the top): true when there is one (the layout is then its coins) */
static bool load_layout(Ctx *c, uint8_t *M) {
    char path[48], line[128];
    snprintf(path, sizeof path, COIN_DIR "/%02x_%08x.txt", (unsigned)s_id, (unsigned)s_hash);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    int lines = 0, bad = 0, air = 0, over = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strchr(line, '\n') && !feof(f)) { /* too long: not a coin, and the rest of it skipped */
            int ch;
            while ((ch = fgetc(f)) != EOF && ch != '\n') {}
            bad++;
            continue;
        }
        const char *q = line;
        while (*q == ' ' || *q == '\t') q++;
        if (!*q || *q == '#' || *q == '\n' || *q == '\r') continue;
        if (!strncmp(q, "coin", 4)) q += 4;
        else if (*q != '-' && *q != '.' && *q != '+' && (*q < '0' || *q > '9')) continue; /* the dump's other lines */
        float p[3], h, wh;
        uint32_t at;
        lines++;
        if (sscanf(q, "%f %f %f", &p[0], &p[1], &p[2]) != 3 || !isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2]) ||
            fabsf(p[0]) > 1e6f || fabsf(p[1]) > 1e6f || fabsf(p[2]) > 1e6f) { bad++; continue; }
        if (s_ncoin >= COIN_MAX) { over++; continue; }
        Coin *k = &s_coin[s_ncoin++];
        memset(k, 0, sizeof *k);
        k->pos[0] = p[0], k->pos[2] = p[2];
        if (ground_at(c, M, p, &h, &at, &wh)) {
            k->attr = (uint8_t)at, k->water = at == ATTR_WATER, k->ground = h;
            k->pos[1] = (wh > h ? wh : h) + COIN_LIFT;
            s_nwater += k->water;
        } else {
            k->ground = p[1], k->pos[1] = p[1] + COIN_LIFT; /* no ground near: where it was put */
            air++;
        }
    }
    fclose(f);
    s_built = true;
    rt_log("coins: course %#x: %d coins from %s (%d lines; %d not coins, %d with no ground under, %d past %d)",
           (unsigned)s_id, s_ncoin, path, lines, bad, air, over, COIN_MAX);
    return true;
}

static void build(Ctx *c, uint8_t *M) {
    uint32_t bol = s_bol;
    if (load_layout(c, M)) return;
    int n = (int)ld16(c, M, bol + 0x1a);
    uint32_t clo = ld32(c, M, bol + 0x44);
    if (n < 2 || n > CL_MAX || clo < 0x7c || !IS_RAM(bol + clo + 0x20u * (uint32_t)n - 1)) {
        rt_log("coins: course %#x has no line to follow (%d CL points)", (unsigned)s_id, n);
        return;
    }
    static float pt[CL_MAX][3], sc[CL_MAX];
    static int16_t grp[CL_MAX];
    static Step st[CL_MAX + 1];
    for (int i = 0; i < n; i++) {
        uint32_t p = bol + clo + 0x20u * (uint32_t)i;
        ldfn(c, M, p, pt[i], 3);
        grp[i] = (int16_t)ld16(c, M, p + 0xe);
        sc[i] = ldf(c, M, p + 0x10);
        if (!isfinite(pt[i][0]) || !isfinite(pt[i][1]) || !isfinite(pt[i][2])) {
            rt_log("coins: course %#x CL point %d is not a position", (unsigned)s_id, i);
            return;
        }
        if (!isfinite(sc[i]) || sc[i] < 0) sc[i] = 0;
    }
    /* the line's steps: within a group (a group ID opens one, the next closes it, -1 between),
     * and from a group's last point on (to the next, the last to the first) when that is near */
    static float lens[CL_MAX];
    int nl = 0;
    for (int i = 0; i + 1 < n; i++) lens[nl++] = sqrtf(dist2(pt[i], pt[i + 1]));
    float usual = 0; /* the median step */
    for (int i = 0; i < nl; i++) {
        int below = 0;
        for (int j = 0; j < nl; j++) below += lens[j] < lens[i];
        if (below == nl / 2) { usual = lens[i]; break; }
    }
    if (usual < 1) usual = 1;
    int nst = 0;
    bool any = false, open = false;
    for (int i = 0; i < n; i++) any |= grp[i] >= 0;
    for (int i = 0; i < n; i++) {
        int j = i + 1 < n ? i + 1 : 0;
        bool closer = false;
        if (grp[i] >= 0) closer = open, open = !open;
        bool inside = !any || (open && !closer);
        float len = sqrtf(dist2(pt[i], pt[j]));
        if (j == 0 || closer || !inside) {
            if (len > COIN_JOIN * usual) continue;
        }
        if (len < 1) continue;
        st[nst++] = (Step){i, j, len};
    }
    /* the item boxes and the start */
    static float box[BOX_MAX][3];
    int nbox = 0, nobj = (int)ld16(c, M, bol + 0x1e);
    uint32_t obo = ld32(c, M, bol + 0x54);
    for (int i = 0; i < nobj && nbox < BOX_MAX; i++) {
        uint32_t o = bol + obo + 0x40u * (uint32_t)i;
        if (!IS_RAM(o + 0x40 - 1)) break;
        if (ld16(c, M, o + 0x24) == OBJ_ITEMBOX) ldfn(c, M, o, box[nbox++], 3);
    }
    float start[3];
    bool have_start = mld(c, M, bol + 0x3b, 1) > 0 && IS_RAM(bol + ld32(c, M, bol + 0x58) + 0xb);
    if (have_start) ldfn(c, M, bol + ld32(c, M, bol + 0x58), start, 3);

    float total = 0;
    for (int i = 0; i < nst; i++) total += st[i].len;
    float space = total / COIN_ROWS;
    space = space < COIN_SPACE_MIN ? COIN_SPACE_MIN : space > COIN_SPACE_MAX ? COIN_SPACE_MAX : space;
    uint32_t seed = s_hash ? s_hash : 0x9E3779B9u;
    int rows = 0, tried = 0, off_road = 0, no_ground = 0, near = 0, steep = 0;
    unsigned attrs[256] = {0};
    float carry = space * 0.5f;
    for (int s = 0; s < nst && s_ncoin + COIN_ROW <= COIN_MAX; s++) {
        const float *a = pt[st[s].a], *b = pt[st[s].b];
        float len = st[s].len, dy = b[1] - a[1], hz = sqrtf((b[0] - a[0]) * (b[0] - a[0]) + (b[2] - a[2]) * (b[2] - a[2]));
        bool bad = hz < 1 || fabsf(dy) > COIN_STEEP * hz || len > COIN_LONG * usual;
        for (float t = carry; t < len && s_ncoin + COIN_ROW <= COIN_MAX; t += space) { /* (room for a whole row) */
            tried++;
            if (bad) { steep++; continue; }
            float u = t / len, d[3] = {(b[0] - a[0]) / len, dy / len, (b[2] - a[2]) / len};
            float side[3] = {d[2] / (hz / len), 0, -d[0] / (hz / len)}; /* level, to the line's right */
            float w = sc[st[s].a] + (sc[st[s].b] - sc[st[s].a]) * u;
            float off = ((float)(rng_next(&seed) % 5) - 2) * 0.5f * COIN_SIDE * w;
            if (off > COIN_SIDE_MAX) off = COIN_SIDE_MAX;
            if (off < -COIN_SIDE_MAX) off = -COIN_SIDE_MAX;
            float mid[3];
            for (int i = 0; i < 3; i++) mid[i] = a[i] + (b[i] - a[i]) * u;
            if (have_start && dist2(mid, start) < COIN_START * COIN_START) { near++; continue; }
            Coin row[COIN_ROW];
            int got = 0;
            for (int pass = 0; pass < 2 && got < COIN_ROW_MIN; pass++, off = 0) { /* off the road: once more on the line */
                got = 0;
                for (int k = 0; k < COIN_ROW; k++) {
                    float p[3], h, wh;
                    uint32_t at;
                    float along = ((float)k - (COIN_ROW - 1) * 0.5f) * COIN_GAP;
                    for (int i = 0; i < 3; i++) p[i] = mid[i] + d[i] * along + side[i] * off;
                    bool by_box = false;
                    for (int q = 0; q < nbox && !by_box; q++) by_box = dist2(p, box[q]) < COIN_BOX * COIN_BOX;
                    if (by_box) { near++; continue; }
                    if (!ground_at(c, M, p, &h, &at, &wh)) { no_ground++; continue; }
                    attrs[at & 0xff]++;
                    if (!coin_road(at)) { off_road++; continue; }
                    row[got].water = at == ATTR_WATER;
                    row[got].ground = h;
                    row[got].pos[0] = p[0], row[got].pos[1] = (wh > h ? wh : h) + COIN_LIFT, row[got].pos[2] = p[2];
                    row[got++].attr = (uint8_t)at;
                }
            }
            if (got < COIN_ROW_MIN) continue;
            memcpy(&s_coin[s_ncoin], row, sizeof(Coin) * (size_t)got);
            for (int k = 0; k < got; k++) s_nwater += row[k].water;
            s_ncoin += got, rows++;
        }
        carry = fmodf(carry - len, space);
        if (carry < 0) carry += space;
    }
    s_built = true;
    char hist[256] = "";
    for (int i = 0, k = 0; i < 256 && k < (int)sizeof hist - 16; i++)
        if (attrs[i]) k += snprintf(hist + k, sizeof hist - (size_t)k, " %d:%u", i, attrs[i]);
    rt_log("coins: course %#x: %d coins in %d rows of %d tried (line %.0f, %d steps of usually %.0f, row every %.0f, "
           "%d boxes); skipped %d steep, %d near start/box, %d off road, %d no ground; ground%s",
           (unsigned)s_id, s_ncoin, rows, tried, total, nst, usual, space, nbox, steep, near, off_road, no_ground, hist);
    if (g_log_mask & LOG_GAME) dump(n, (const float(*)[3])pt, grp, sc, (const float(*)[3])box, nbox, have_start ? start : NULL, st, nst);
}


/* ---- pickup and the speed: a kart (any: the players' and the computer's) drives through a coin
 * and holds one more, at most COIN_HOLD; the coin comes back COIN_BACK frames later. Each coin
 * held raises the kart's top speed by COIN_SPEED: the four class speeds KartBody +0x3f0..+0x3fc
 * (road, +0x3f4 attribute 12, +0x3f8 off-road, +0x3fc 19: KartCtrl::GetMaxSpeed's), set once at
 * the kart's setup, scaled from the game's own value every frame (a value other than the one last
 * written is the game's new one). A hit (KartDamage +4 flags 0x10 spin, 0x20 / 0x40 a big hit,
 * 0x100 burn, just set) drops COIN_DROP. KartBody +0xcc its KartDamage (+0 back to the body). So does
 * a fall (off the course, deep water, lava): Lakitu's rescue, KartBody +0xb4 KartRescue, +0x74 flags
 * 0x20 while it lasts (KartCheck::CheckSpecialDyn's); no pickups while Lakitu carries the kart. ---- */
#define COIN_HOLD 10
#define COIN_BACK 600         /* frames: 10 s */
#define COIN_REACH 190.0f     /* a kart's centre this near the coin takes it */
#define COIN_SPEED 0.01f      /* top speed per coin held */
#define COIN_DROP 3
#define KART_MAX 8
#define DAMAGE_HIT 0x170
#define RESCUE_ON 0x20
/* the pickup's sound, for a person's kart (+0x578 game status: 2, not a ghost 0x10 or replay 0x80):
 * GameAudio::Main (r13 -0x5dc8) startSystemSe(id), as the menus call it (0x2xxxx their sounds); the id
 * is GCKart.bstn's JA_SE_TR_COIN, SE_KART (section 1) sound 18 */
#define G_startSystemSe 0x80115E74u
#define SE_COIN 0x10012u

static uint16_t s_gone[COIN_MAX]; /* frames until it is back (0: there) */
static uint8_t s_held[KART_MAX];
static uint32_t s_body[KART_MAX];
static bool s_hit[KART_MAX], s_fell[KART_MAX];
static float s_base[KART_MAX][4], s_wrote[KART_MAX][4];
static bool s_ding_logged; /* the pickup sound's result logged, once a race */
static int s_tide_log;     /* water heights still to log this race (the first, then big changes) */
static double s_tide_was;

bool coins_racing(void) { return s_built && s_ncoin > 0; }
int coins_held(int kart) { return s_built && kart >= 0 && kart < KART_MAX ? s_held[kart] : 0; }

static void collect_reset(void) {
    memset(s_gone, 0, sizeof s_gone), memset(s_held, 0, sizeof s_held), memset(s_body, 0, sizeof s_body);
    memset(s_hit, 0, sizeof s_hit), memset(s_fell, 0, sizeof s_fell);
    s_ding_logged = false;
    s_tide_log = 4;
}

static void kart_speed(Ctx *c, uint8_t *M, int k, uint32_t b) {
    float cur[4], mul = 1.0f + COIN_SPEED * (float)s_held[k];
    ldfn(c, M, b + 0x3f0, cur, 4);
    for (int i = 0; i < 4; i++) {
        if (memcmp(&cur[i], &s_wrote[k][i], 4)) s_base[k][i] = cur[i]; /* the game's own (set up or changed) */
        float base = s_base[k][i];
        if (!(base > 0 && base < 1000)) continue; /* NaN, 0 (reset) or junk: left as it is */
        s_wrote[k][i] = base * mul;
        stfn(c, M, b + 0x3f0 + 4 * i, &s_wrote[k][i], 1);
    }
}

/* a guest call on a frame of its own (the hooked function's stack left alone) */
static void gxcall(Ctx *c, uint8_t *M, uint32_t fn, const uint32_t *a, unsigned n) {
    uint32_t old = frame_push(c, M, 0x40);
    ctx_call_guest(c, fn, a, n);
    frame_pop(c, old);
}
static void coin_sound(Ctx *c, uint8_t *M) {
    uint32_t main = ld32(c, M, ctx_gpr(c, 13) - 0x5dc8);
    if (!ram_ptr(main) || !ram_ptr(ld32(c, M, main))) return; /* +0: its sound starter */
    const uint32_t a[] = {main, SE_COIN};
    uint32_t old = frame_push(c, M, 0x40), ok = ctx_call_guest(c, G_startSystemSe, a, 2);
    frame_pop(c, old);
    if (!s_ding_logged) s_ding_logged = true, rt_log("coins: pickup sound %s", ok & 0xFF ? "started" : "not started"); /* r3: the starter's result */
}

static void collect(Ctx *c, uint8_t *M, uint32_t ctrl) {
    bool ding = false;
    for (int i = 0; i < s_ncoin; i++)
        if (s_gone[i]) s_gone[i]--;
    if (!ram_ptr(ctrl) || (ld32(c, M, ctrl + 0x224) & 1)) return;
    uint32_t n = ld32(c, M, ctrl + 0x22c);
    if (n > KART_MAX) n = KART_MAX;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t b = ld32(c, M, ctrl + 0xa0 + 4 * k);
        if (!ram_ptr(b)) continue;
        if (s_body[k] != b) { /* a new kart: none held, the speeds its own */
            s_body[k] = b, s_held[k] = 0, s_hit[k] = s_fell[k] = false;
            memset(s_wrote[k], 0, sizeof s_wrote[k]);
        }
        uint32_t dmg = ld32(c, M, b + 0xcc);
        if (ram_ptr(dmg) && ld32(c, M, dmg) == b) {
            bool hit = ld16(c, M, dmg + 4) & DAMAGE_HIT;
            if (hit && !s_hit[k] && s_held[k]) {
                s_held[k] = s_held[k] > COIN_DROP ? (uint8_t)(s_held[k] - COIN_DROP) : 0;
                rt_log("coins: kart %u hit, %u left", (unsigned)k, (unsigned)s_held[k]);
            }
            s_hit[k] = hit;
        }
        uint32_t res = ld32(c, M, b + 0xb4);
        bool fell = ram_ptr(res) && (mld_s(M, res + 0x74, 1) & RESCUE_ON);
        if (fell && !s_fell[k] && s_held[k]) {
            s_held[k] = s_held[k] > COIN_DROP ? (uint8_t)(s_held[k] - COIN_DROP) : 0;
            rt_log("coins: kart %u fell, %u left", (unsigned)k, (unsigned)s_held[k]);
        }
        s_fell[k] = fell;
        float p[3];
        for (int i = 0; i < 3; i++) p[i] = ldf(c, M, b + 0x110 + 4 * (4 * i + 3));
        for (int i = 0; i < s_ncoin && !fell; i++) {
            if (s_gone[i] || dist2(p, s_coin[i].pos) > COIN_REACH * COIN_REACH) continue;
            s_gone[i] = COIN_BACK;
            if (s_held[k] < COIN_HOLD) s_held[k]++;
            uint32_t st = ld32(c, M, b + 0x578);
            ding |= (st & 2) && !(st & 0x90);
        }
        kart_speed(c, M, (int)k, b);
    }
    if (ding) coin_sound(c, M); /* once a frame */
}

/* the water coins on the surface as it moves (Peach Beach's tide): CrsGround::getWaterHeight on
 * ground_at's CrsGround with the water attribute (it reads only that, then the course's water
 * object), every TIDE_EVERY frames; never below their ground */
#define TIDE_EVERY 2
static void tide(Ctx *c, uint8_t *M) {
    if (!s_nwater || !s_ground || s_tick % TIDE_EVERY) return;
    mst(c, M, s_ground + 0x4c, 4, ATTR_WATER);
    const uint32_t wa[1] = {s_ground};
    gxcall(c, M, CRS_GROUND_WATER, wa, 1);
    uint64_t u = (uint64_t)RD(c, 0x4008, 8); /* f1 */
    double d;
    memcpy(&d, &u, 8);
    if (!isfinite(d) || d < -999.0 || d > 1e6) return; /* no water object: where they are */
    if (s_tide_log == 4 || (s_tide_log && fabs(d - s_tide_was) > 100))
        s_tide_log--, s_tide_was = d, rt_log("coins: water at %.1f (%d coins over it)", d, s_nwater);
    for (int i = 0; i < s_ncoin; i++)
        if (s_coin[i].water) s_coin[i].pos[1] = ((float)d > s_coin[i].ground ? (float)d : s_coin[i].ground) + COIN_LIFT;
}

/* the pause's RETRY (RaceDirector::checkPauseChoice, lanpause.c): the race back at its start
 * without a course load (the same karts and coins, the race's phase not seen to change). The
 * coins back and none held; the karts' speeds kept as they are (kart_speed writes their own back). */
void coins_restart(void) {
    if (!s_built) return;
    bool any = false;
    for (int k = 0; k < KART_MAX; k++) any |= s_held[k] != 0;
    memset(s_gone, 0, sizeof s_gone), memset(s_held, 0, sizeof s_held);
    memset(s_hit, 0, sizeof s_hit), memset(s_fell, 0, sizeof s_fell);
    rt_log("coins: the race restarted%s", any ? ": none held" : "");
}

void coins_frame(Ctx *c, uint8_t *M, uint32_t ctrl) {
    s_tick++;
    if (s_pending) {
        s_pending = false;
        collect_reset();
        build(c, M);
    }
    if (!s_built) return;
    tide(c, M);
    collect(c, M, ctrl);
}

/* ---- drawing: a spinning gold coin (COIN_SEG-sided, two faces and a rim, vertex colours: a
 * bright middle to a deeper edge) at every coin in front of the view's camera, all in one
 * GX_TRIANGLES draw through the game's own GX calls (their state kept in the game's GX data).
 * Drawn right after the race's item models (the item draw buffer's opaque list, once a view):
 * the view's camera is j3dSys's view matrix and its projection is loaded. ---- */
#define J3DSYS 0x803acf90u        /* j3dSys: +0 the view matrix */
#define J3D_MTX_CACHE 0x803afe54u /* J3D's position matrix load cache (10 halfwords, 0xffff none) */
#define G_GXClearVtxDesc 0x800BCA18u
#define G_GXSetVtxDesc 0x800BC100u
#define G_GXSetVtxAttrFmt 0x800BCA50u
#define G_GXSetNumChans 0x800BF040u
#define G_GXSetChanCtrl 0x800BF07Cu
#define G_GXSetNumTexGens 0x800BD5D8u
#define G_GXSetNumIndStages 0x800C0530u
#define G_GXSetTevDirect 0x800C0554u
#define G_GXSetNumTevStages 0x800C0D94u
#define G_GXSetTevOrder 0x800C0BF8u
#define G_GXSetTevOp 0x800C0658u
#define G_GXSetTevSwapMode 0x800C0A60u
#define G_GXSetAlphaCompare 0x800C0B28u
#define G_GXSetZCompLoc 0x800C11E4u
#define G_GXSetBlendMode 0x800C1104u
#define G_GXSetZMode 0x800C11B0u
#define G_GXSetCullMode 0x800BE0E4u
#define G_GXSetCurrentMtx 0x800C2178u
#define G_GXLoadPosMtxImm 0x800C20D8u
#define G_GXBegin 0x800BDEC4u
#define COIN_FMT 7                  /* the vertex format used (position f32 xyz, colour RGBA8) */
#define COIN_SEG 10                 /* sides */
#define COIN_R 55.0f                /* radius */
#define COIN_T 9.0f                 /* half thickness */
#define COIN_FAR 15000.0f           /* drawn this near the camera (a few pixels there), one player ... */
#define COIN_FAR_SPLIT 10000.0f     /* ... split screen (smaller views: as few pixels nearer) */
#define COIN_DRAW_MAX 128           /* coins a view, one player (each vertex goes through gx.c's decoder) ... */
#define COIN_DRAW_2P 80             /* ... two players (two views a frame) */
#define COIN_DRAW_4P 48             /* ... three and four (four views) */
#define COIN_VTX (COIN_SEG * 9)     /* the most a coin sends: a face (3 a side) and the whole rim (6 a side) */

static uint8_t s_vtx[COIN_DRAW_MAX * COIN_VTX * 16];

#define GX(fn, ...) do { const uint32_t a_[] = {__VA_ARGS__}; gxcall(c, M, fn, a_, sizeof a_ / sizeof a_[0]); } while (0)

static uint8_t *vtx(uint8_t *o, const float p[3], uint32_t rgba) {
    for (int i = 0; i < 3; i++) {
        uint32_t u;
        memcpy(&u, &p[i], 4);
        o[0] = (uint8_t)(u >> 24), o[1] = (uint8_t)(u >> 16), o[2] = (uint8_t)(u >> 8), o[3] = (uint8_t)u;
        o += 4;
    }
    o[0] = (uint8_t)(rgba >> 24), o[1] = (uint8_t)(rgba >> 16), o[2] = (uint8_t)(rgba >> 8), o[3] = (uint8_t)rgba;
    return o + 4;
}
static uint32_t shade(uint32_t rgb, float k) {
    float ch[3] = {(float)(rgb >> 16 & 0xff) * k, (float)(rgb >> 8 & 0xff) * k, (float)(rgb & 0xff) * k};
    uint32_t out = 0xff;
    for (int i = 0; i < 3; i++) out |= (uint32_t)(ch[i] > 255 ? 255 : ch[i] < 0 ? 0 : ch[i]) << (24 - 8 * i);
    return out;
}

/* The coin's shape, the same for all (one spin): made once a frame, near (COIN_SEG sides) and far
 * (COIN_SEG_FAR), relative to its middle. Each coin then sends only what faces the camera (a convex
 * opaque shape: the rest is behind it): one of its faces and the rim's sides turned to the camera,
 * about half the triangles, and no sin / cos of its own. */
#define COIN_SEG_FAR 6
#define COIN_NEAR 6000.0f /* the near shape this near the camera; the far one is its face alone (a rim a pixel or two there) */
typedef struct {
    int seg;
    bool rims;                       /* the rim drawn (far: the face alone, half the vertices) */
    float ring[COIN_SEG + 1][3];     /* the edge's points */
    float out[COIN_SEG][3];          /* each rim side's outward direction */
    uint32_t rim[COIN_SEG];          /* ... and colour */
} CoinShape;
static CoinShape s_shape[2];         /* near, far */
static float s_n[3];                 /* the face's normal times COIN_T */
static uint32_t s_mid, s_edge, s_back_mid, s_back_edge;

static void shape_make(CoinShape *sh, int seg, bool rims, float sa, float ca) {
    const float u[3] = {ca, 0, -sa}; /* the face's horizontal axis */
    sh->seg = seg, sh->rims = rims;
    for (int k = 0; k <= seg; k++) {
        float t = 6.2831853f * (float)k / (float)seg, ct = cosf(t), st = sinf(t);
        sh->ring[k][0] = u[0] * COIN_R * ct, sh->ring[k][1] = COIN_R * st, sh->ring[k][2] = u[2] * COIN_R * ct;
    }
    for (int k = 0; k < seg; k++) {
        float t = 6.2831853f * ((float)k + 0.5f) / (float)seg, ct = cosf(t), st = sinf(t);
        sh->out[k][0] = u[0] * ct, sh->out[k][1] = st, sh->out[k][2] = u[2] * ct;
        sh->rim[k] = shade(0xc07808, 0.8f + 0.3f * st); /* darker, lighter where it faces up */
    }
}
static void shapes_make(float sa, float ca) {
    s_n[0] = sa * COIN_T, s_n[1] = 0, s_n[2] = ca * COIN_T;
    float lit = 0.75f + 0.25f * (0.6f * sa + 0.8f * ca); /* the faces lit from one side as the coin turns */
    s_mid = shade(0xfff27a, lit), s_edge = shade(0xe8a010, lit);
    s_back_mid = shade(0xfff27a, 1.5f - lit), s_back_edge = shade(0xe8a010, 1.5f - lit);
    shape_make(&s_shape[0], COIN_SEG, true, sa, ca);
    shape_make(&s_shape[1], COIN_SEG_FAR, false, sa, ca);
}

/* one coin at p seen from e (the camera minus p): its vertices into o, the count into *nv */
static uint8_t *coin_tris(uint8_t *o, const float p[3], const float e[3], const CoinShape *sh, int *nv) {
    bool front = s_n[0] * e[0] + s_n[2] * e[2] > 0;
    float sg = front ? 1.0f : -1.0f, fc[3];
    for (int i = 0; i < 3; i++) fc[i] = p[i] + sg * s_n[i];
    uint32_t mid = front ? s_mid : s_back_mid, edge = front ? s_edge : s_back_edge;
    int n = 0;
    for (int k = 0; k < sh->seg; k++) {
        const float *ra = sh->ring[k], *rb = sh->ring[k + 1];
        float fa[3], fb[3];
        for (int i = 0; i < 3; i++) fa[i] = fc[i] + ra[i], fb[i] = fc[i] + rb[i];
        if (front) o = vtx(o, fc, mid), o = vtx(o, fa, edge), o = vtx(o, fb, edge);
        else o = vtx(o, fc, mid), o = vtx(o, fb, edge), o = vtx(o, fa, edge);
        n += 3;
        if (!sh->rims) continue;
        const float *d = sh->out[k];
        if (d[0] * e[0] + d[1] * e[1] + d[2] * e[2] <= 0) continue; /* this rim side turned away */
        float ba[3], bb[3]; /* the other face's edge */
        for (int i = 0; i < 3; i++) ba[i] = fa[i] - 2 * sg * s_n[i], bb[i] = fb[i] - 2 * sg * s_n[i];
        uint32_t rim = sh->rim[k];
        o = vtx(o, fa, rim), o = vtx(o, ba, rim), o = vtx(o, bb, rim);
        o = vtx(o, fa, rim), o = vtx(o, bb, rim), o = vtx(o, fb, rim);
        n += 6;
    }
    *nv += n;
    return o;
}

typedef struct {
    int i;   /* the coin */
    float d; /* its depth in front of the camera */
} ViewCoin;
static int view_coin_cmp(const void *a, const void *b) {
    float x = ((const ViewCoin *)a)->d, y = ((const ViewCoin *)b)->d;
    return (x > y) - (x < y);
}

void coins_draw(Ctx *c, uint8_t *M, uint32_t buf) {
    if (!s_built || !s_ncoin) return;
    uint32_t rm = ld32(c, M, ctx_gpr(c, 13) - 0x5c38);
    if (!ram_ptr(rm)) return;
    uint32_t rd = ld32(c, M, rm + 0x1c); /* RaceMgr's RaceDrawer */
    if (!ram_ptr(rd)) return;
    uint32_t items = ld32(c, M, rd + 0x30); /* its draw buffer of all karts' items */
    if (!ram_ptr(items) || buf != ld32(c, M, items + 8)) return; /* +8: the opaque J3DDrawBuffer */
    float v[12];
    ldfn(c, M, J3DSYS, v, 12);
    static uint32_t made = ~0u; /* the frame the shapes are for (several views share them) */
    if (made != s_tick) {
        made = s_tick;
        float spin = (float)s_tick * 0.07f;
        shapes_make(sinf(spin), cosf(spin));
    }
    float bob = 6.0f * sinf((float)s_tick * 0.05f);
    /* the camera in the world: the view matrix's rotation transposed, times its translation, negated */
    float cam[3];
    for (int j = 0; j < 3; j++) cam[j] = -(v[j] * v[3] + v[4 + j] * v[7] + v[8 + j] * v[11]);
    /* the coins in view: in the camera's space in front (-z), near, about inside the view's sides;
     * fewer and nearer with more views a frame (gRaceInfo +0x1e players) */
    uint32_t players = ld16(c, M, 0x803B145Cu + 0x1e);
    int most = players >= 3 ? COIN_DRAW_4P : players == 2 ? COIN_DRAW_2P : COIN_DRAW_MAX;
    float far = players >= 2 ? COIN_FAR_SPLIT : COIN_FAR;
    static ViewCoin seen[COIN_MAX];
    int ns = 0;
    for (int i = 0; i < s_ncoin; i++) {
        if (s_gone[i]) continue; /* taken, not back yet */
        const float *p = s_coin[i].pos;
        float z = v[8] * p[0] + v[9] * p[1] + v[10] * p[2] + v[11];
        if (!(z < COIN_R && z > -far)) continue;
        float x = v[0] * p[0] + v[1] * p[1] + v[2] * p[2] + v[3];
        if (fabsf(x) > 2.0f * -z + 4 * COIN_R) continue;
        float y = v[4] * p[0] + v[5] * p[1] + v[6] * p[2] + v[7];
        if (fabsf(y) > 1.5f * -z + 4 * COIN_R) continue;
        seen[ns++] = (ViewCoin){i, -z};
    }
    if (ns > most) qsort(seen, (size_t)ns, sizeof *seen, view_coin_cmp); /* more than it draws: the nearest */
    uint8_t *o = s_vtx;
    int drawn = 0, nv = 0;
    for (int s = 0; s < ns && drawn < most; s++, drawn++) {
        const float *p = s_coin[seen[s].i].pos;
        const float q[3] = {p[0], p[1] + bob, p[2]}, e[3] = {cam[0] - q[0], cam[1] - q[1], cam[2] - q[2]};
        o = coin_tris(o, q, e, &s_shape[seen[s].d > COIN_NEAR], &nv);
    }
    if (!drawn) return;
    GX(G_GXClearVtxDesc, 0);
    GX(G_GXSetVtxDesc, 9, 1);  /* GX_VA_POS, GX_DIRECT */
    GX(G_GXSetVtxDesc, 11, 1); /* GX_VA_CLR0 */
    GX(G_GXSetVtxAttrFmt, COIN_FMT, 9, 1, 4, 0);  /* XYZ, F32 */
    GX(G_GXSetVtxAttrFmt, COIN_FMT, 11, 1, 5, 0); /* RGBA, RGBA8 */
    GX(G_GXSetNumChans, 1);
    GX(G_GXSetChanCtrl, 4, 0, 0, 1, 0, 0, 2); /* COLOR0A0: no lighting, the vertex colour */
    GX(G_GXSetNumTexGens, 0);
    GX(G_GXSetNumIndStages, 0);
    GX(G_GXSetTevDirect, 0);
    GX(G_GXSetNumTevStages, 1);
    GX(G_GXSetTevOrder, 0, 0xff, 0xff, 4);
    GX(G_GXSetTevOp, 0, 4); /* GX_PASSCLR */
    GX(G_GXSetTevSwapMode, 0, 0, 0);
    GX(G_GXSetAlphaCompare, 7, 0, 0, 7, 0);
    GX(G_GXSetZCompLoc, 1);
    GX(G_GXSetBlendMode, 0, 1, 0, 0);
    GX(G_GXSetZMode, 1, 3, 1); /* LEQUAL, written */
    GX(G_GXSetCullMode, 0);
    GX(G_GXSetCurrentMtx, 0);
    GX(G_GXLoadPosMtxImm, J3DSYS, 0);
    GX(G_GXBegin, 0x90, COIN_FMT, (uint32_t)nv); /* GX_TRIANGLES */
    hw_gather_copy(s_vtx, (size_t)(o - s_vtx));
    /* J3D's caches: the next shape sends its vertex format list and position matrix again */
    mst(c, M, ctx_gpr(c, 13) - 0x6688, 4, 0);
    for (int i = 0; i < 10; i++) mst(c, M, J3D_MTX_CACHE + 2 * i, 2, 0xffff);
}
