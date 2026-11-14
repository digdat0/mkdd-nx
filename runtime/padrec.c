/* Controller recording and replay for tests (PC: environment variables; nothing happens without them).
 *
 *   MKDD_PADREC=<name>   records player 1's input, one entry per game frame (PADRead), to padrec/<name>.txt
 *   MKDD_PADPLAY=<name>  feeds padrec/<name>.txt to player 1 instead of the controller and keyboard
 *
 * Frame-locked: the entry for each PADRead, so the game sees the same input on the same frame however
 * fast the PC runs. Loads take a varying number of frames, so the file also holds anchors: a line
 * "@ <key>" each time the scene changes or a race starts or ends (SequenceApp::msScene, the race
 * director's presence). Replay re-syncs on them: reaching an anchor in the file before the game gets
 * there, it waits (neutral input); the game getting there first, it skips ahead to that anchor.
 *
 * The game's randomness follows the guest clock (the host's), so computer karts and items differ
 * between runs. In a race each entry also holds player 1's kart position, and replay follows that
 * line: it takes the entry recorded nearest to where the kart is (around the frame it would be at),
 * and steers toward the recorded position a little further on. A bump or another item does not
 * spoil the rest of the run.
 *
 * File: text, '#' comments, entries "<frames> <buttons hex> <stick x> <stick y> <c x> <c y> <L> <R> <extra hex> [<x> <z>]"
 * (a run of identical frames on one line outside races; extra: bit 0 jump, bits 1-4 the trick D-pad nibble, bit 5 the signature's ZL + ZR). */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#endif

#include "rt.h"

typedef struct {
    uint16_t button;
    int8_t sx, sy, cx, cy;
    uint8_t tl, tr, extra;
} PrIn;
typedef struct {
    uint32_t n;      /* frames (0: an anchor) */
    uint32_t key;    /* the anchor */
    PrIn in;
    bool hasp;       /* a race frame: the kart's position */
    float x, z;
} PrRec;

#define PR_BACK 30       /* entries: how far back / ahead of the expected one replay looks for the nearest */
#define PR_AHEAD 60
#define PR_LOOK 20       /* entries ahead of the nearest: the point it steers toward */
#define PR_GAIN 2.5f     /* stick units per degree off that point */
#define PR_SIGN 1.0f     /* sign of stick per degree of atan2(cross, dot) off the target: measured with the route trace (-1 turned away) */

static int s_mode = -1;           /* -1 not looked yet, 0 off, 1 recording, 2 replaying */
static FILE *s_out;
static PrIn s_last;
static uint32_t s_run, s_frame;
static PrRec *s_rec;
static size_t s_nrec, s_pos;
static uint32_t s_left;           /* frames left in s_rec[s_pos] */
static bool s_waiting, s_done;
static uint32_t s_key;            /* the game's anchor key now */
static float s_px, s_pz;          /* replay: the kart's position last frame */
static bool s_phave;
static uint32_t s_follow_log;
volatile bool g_padplay;

static bool name_ok(const char *n) {
    if (!n || !*n || strlen(n) > 64) return false;
    for (const char *p = n; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return false;
    return true;
}

static void put_in(const PrIn *in, uint32_t n) {
    fprintf(s_out, "%u %x %d %d %d %d %u %u %x", n, in->button, in->sx, in->sy, in->cx, in->cy, in->tl, in->tr, in->extra);
}
static void flush_run(void) {
    if (!s_out || !s_run) return;
    put_in(&s_last, s_run);
    fputc('\n', s_out);
    s_run = 0;
}

static bool load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { rt_log("padplay: cannot open %s (%s)", path, strerror(errno)); return false; }
    size_t cap = 4096;
    s_rec = malloc(cap * sizeof *s_rec);
    char line[192];
    unsigned ln = 0;
    while (s_rec && fgets(line, sizeof line, f)) {
        ln++;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        PrRec r = {0};
        if (line[0] == '@') {
            if (sscanf(line + 1, "%x", &r.key) != 1) { rt_log("padplay: %s line %u: bad anchor", path, ln); continue; }
        } else {
            unsigned b, tl, tr, ex;
            int sx, sy, cx, cy;
            float x, z;
            int got = sscanf(line, "%u %x %d %d %d %d %u %u %x %f %f", &r.n, &b, &sx, &sy, &cx, &cy, &tl, &tr, &ex, &x, &z);
            if ((got != 9 && got != 11) || !r.n) { rt_log("padplay: %s line %u: bad entry", path, ln); continue; }
#define CL8(v) (int8_t)((v) < -128 ? -128 : (v) > 127 ? 127 : (v))
            r.in = (PrIn){(uint16_t)b, CL8(sx), CL8(sy), CL8(cx), CL8(cy), (uint8_t)(tl > 255 ? 255 : tl), (uint8_t)(tr > 255 ? 255 : tr), (uint8_t)ex};
#undef CL8
            if (got == 11 && isfinite(x) && isfinite(z)) r.hasp = true, r.x = x, r.z = z, r.n = 1;
        }
        if (s_nrec == cap) {
            PrRec *g = realloc(s_rec, (cap *= 2) * sizeof *s_rec);
            if (!g) break;
            s_rec = g;
        }
        s_rec[s_nrec++] = r;
    }
    fclose(f);
    if (!s_rec || !s_nrec) { rt_log("padplay: %s holds nothing", path); free(s_rec); s_rec = NULL; s_nrec = 0; return false; }
    uint64_t frames = 0;
    unsigned anchors = 0, pos = 0;
    for (size_t i = 0; i < s_nrec; i++) frames += s_rec[i].n, anchors += !s_rec[i].n, pos += s_rec[i].hasp;
    rt_log("padplay: %s: %llu frames (%u following the recorded line), %u anchors", path, (unsigned long long)frames, pos, anchors);
    return true;
}

static void init(void) {
    s_mode = 0;
    const char *rec = getenv("MKDD_PADREC"), *play = getenv("MKDD_PADPLAY");
    char path[96];
    if (play && *play) {
        if (!name_ok(play)) { rt_log("padplay: MKDD_PADPLAY=%s: letters, digits, - and _ only", play); return; }
        snprintf(path, sizeof path, "padrec/%s.txt", play);
        if (load(path)) s_mode = 2, g_padplay = true, s_left = 0;
    } else if (rec && *rec) {
        if (!name_ok(rec)) { rt_log("padrec: MKDD_PADREC=%s: letters, digits, - and _ only", rec); return; }
#ifdef _WIN32
        mkdir("padrec");
#else
        mkdir("padrec", 0777);
#endif
        snprintf(path, sizeof path, "padrec/%s.txt", rec);
        s_out = fopen(path, "w");
        if (!s_out) { rt_log("padrec: cannot write %s (%s)", path, strerror(errno)); return; }
        fprintf(s_out, "# MKDD-NX controller recording (runtime/padrec.c), player 1, one entry per game frame\n");
        s_mode = 1;
        rt_log("padrec: recording player 1 to %s", path);
    }
}

/* Player 1's kart (kart 0) position: KartCtrl (r13 - 0x4e98) +0xa0 the body, its matrix's w column */
static bool kart0_pos(uint32_t r13, float *x, float *z, float *fx, float *fz) {
    uint32_t kc = (uint32_t)m_read_be(r13 - 0x4e98, 4);
    if ((kc & 0xFE000000u) != 0x80000000u || m_read_be(kc + 0x22c, 4) == 0) return false;
    uint32_t body = (uint32_t)m_read_be(kc + 0xa0, 4);
    if ((body & 0xFE000000u) != 0x80000000u) return false;
    uint32_t bx = (uint32_t)m_read_be(body + 0x11c, 4), bz = (uint32_t)m_read_be(body + 0x13c, 4);
    uint32_t ux = (uint32_t)m_read_be(body + 0x118, 4), uz = (uint32_t)m_read_be(body + 0x138, 4); /* forward: the z column */
    memcpy(x, &bx, 4), memcpy(z, &bz, 4), memcpy(fx, &ux, 4), memcpy(fz, &uz, 4);
    if (!isfinite(*fx) || !isfinite(*fz)) *fx = *fz = 0;
    return isfinite(*x) && isfinite(*z) && fabsf(*x) < 1e6f && fabsf(*z) < 1e6f;
}

/* ---- route driver (replay, in races): player 1 drives the course's CL points, the line the
 * computer's karts follow, instead of the recorded line (a recording drifts off after the first
 * bump: the game's randomness follows the host clock). MKDD_PADROUTE=0 turns it off. The BOL's CL
 * points (+0x1a count, +0x44 offset, 0x20 each: position, +0xe link) come in groups: a link opens a
 * group and the next closes it; a group ending with link L goes on to the groups starting with L.
 * At a fork it takes the longest group (shortcuts are the short ones). Stuck (hardly moving for
 * 0.75 s once the race is going), it backs up for 0.8 s steering the other way. */
#define RD_MAX 512
#define RD_LOOK 900.0f     /* units ahead of the nearest point: the point it steers toward */
static float s_rp[RD_MAX][2];  /* x, z */
static int16_t s_rnext[RD_MAX]; /* the point after each (-1: none) */
static int s_rn, s_rcur = -1, s_rstill, s_rback, s_rlog;
static bool s_rmoved;

void padrec_course(uint32_t bol) {
    s_rn = 0, s_rcur = -1, s_rstill = s_rback = 0, s_rmoved = false;
    const char *e = getenv("MKDD_PADROUTE");
    if ((e && e[0] == '0') || (bol & 0xFE000000u) != 0x80000000u) return;
    uint32_t n = (uint32_t)m_read_be(bol + 0x1a, 2), at = bol + (uint32_t)m_read_be(bol + 0x44, 4);
    if (n < 2 || n > RD_MAX || (at & 0xFE000000u) != 0x80000000u) { rt_log("padplay route: %u CL points, not driven", n); return; }
    static int16_t link[RD_MAX];
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = at + 0x20 * i, bx = (uint32_t)m_read_be(p, 4), bz = (uint32_t)m_read_be(p + 8, 4);
        memcpy(&s_rp[i][0], &bx, 4), memcpy(&s_rp[i][1], &bz, 4);
        if (!isfinite(s_rp[i][0]) || !isfinite(s_rp[i][1])) { rt_log("padplay route: point %u is not a number, not driven", i); return; }
        link[i] = (int16_t)m_read_be(p + 0xe, 2);
        s_rnext[i] = -1;
    }
    /* groups [s, e]: s opens (link >= 0), e the next point with a link */
    static int16_t gs[RD_MAX], ge[RD_MAX];
    static float glen[RD_MAX];
    int ng = 0;
    for (int i = 0; i < (int)n && ng < RD_MAX;) {
        if (link[i] < 0) { i++; continue; }
        int j = i + 1;
        while (j < (int)n && link[j] < 0) j++;
        if (j >= (int)n) break;
        float len = 0;
        for (int q = i; q < j; q++) {
            s_rnext[q] = (int16_t)(q + 1);
            len += hypotf(s_rp[q + 1][0] - s_rp[q][0], s_rp[q + 1][1] - s_rp[q][1]);
        }
        gs[ng] = (int16_t)i, ge[ng] = (int16_t)j, glen[ng++] = len;
        i = j + 1;
    }
    int joined = 0;
    for (int g = 0; g < ng; g++) { /* the end point goes on to the longest group starting with its link */
        int best = -1;
        for (int h = 0; h < ng; h++)
            if (link[gs[h]] == link[ge[g]] && (best < 0 || glen[h] > glen[best])) best = h;
        if (best >= 0) s_rnext[ge[g]] = gs[best], joined++;
    }
    if (!ng) for (uint32_t i = 0; i < n; i++) s_rnext[i] = (int16_t)((i + 1) % n); /* no links: one loop in order */
    s_rn = (int)n;
    rt_log("padplay route: %u CL points, %d groups (%d joined)", n, ng, joined);
}

static float rd_d2(int i, float x, float z) { return (s_rp[i][0] - x) * (s_rp[i][0] - x) + (s_rp[i][1] - z) * (s_rp[i][1] - z); }

/* Steer player 1 along the route: in (buttons, stick) from the kart's position and motion. */
static bool route_drive(PrIn *in, float x, float z, float mx, float mz, float fx, float fz) {
    if (s_rn < 2) return false;
    if (s_rcur < 0 || rd_d2(s_rcur, x, z) > 4000.0f * 4000.0f) { /* start, or a respawn: the nearest point */
        int best = 0;
        for (int i = 1; i < s_rn; i++) if (rd_d2(i, x, z) < rd_d2(best, x, z)) best = i;
        s_rcur = best;
    }
    /* move on while the next point is nearer (a few each frame at most) */
    for (int k = 0; k < 4; k++) {
        int nx = s_rnext[s_rcur];
        if (nx < 0 || rd_d2(nx, x, z) > rd_d2(s_rcur, x, z)) break;
        s_rcur = nx;
    }
    /* target: RD_LOOK along the route past the nearest point (past it if the kart is beyond it) */
    int t = s_rcur;
    float left = RD_LOOK;
    for (int k = 0; k < 16 && s_rnext[t] >= 0; k++) {
        int nx = s_rnext[t];
        left -= sqrtf(rd_d2(nx, s_rp[t][0], s_rp[t][1]));
        t = nx;
        if (left <= 0) break;
    }
    float ml = sqrtf(mx * mx + mz * mz), speed = ml;
    if (s_rback) s_rback--;
    else if (ml < 10.0f) { /* full speed is ~60 a frame: scraping along a wall is ~6 */ if (s_rmoved && ++s_rstill > 45) s_rback = 50, s_rstill = 0; }
    else s_rmoved = true, s_rstill = 0;
    if (ml < 10.0f) mx = fx, mz = fz, ml = sqrtf(mx * mx + mz * mz);
    float dx = s_rp[t][0] - x, dz = s_rp[t][1] - z;
    float a = ml > 1e-3f ? atan2f(mx * dz - mz * dx, mx * dx + mz * dz) * 57.29578f : 0;
    static float s_rturn = 1; /* target behind (|a| near 180): keep turning one way, no flip-flop */
    if (fabsf(a) > 150.0f) a = 180.0f * s_rturn; else if (fabsf(a) > 1.0f) s_rturn = a > 0 ? 1.0f : -1.0f;
    float s = PR_SIGN * PR_GAIN * a;
    if (s_rback) s = -s; /* reversing: the stick turns the other way */
    in->sx = (int8_t)(s > 100 ? 100 : s < -100 ? -100 : s);
    in->sy = 0;
    in->button = (uint16_t)((in->button & ~(PAD_A | PAD_B | 0x0060)) | (s_rback ? PAD_B : PAD_A)); /* no L/R (hops, drifts) */
    in->tl = in->tr = 0;
    in->extra = 0;
    static int s_rtrace = -1; /* MKDD_PADROUTE_TRACE=1: a line every 30 frames (steering checks) */
    if (s_rtrace < 0) s_rtrace = getenv("MKDD_PADROUTE_TRACE") != NULL;
    if (s_rtrace && s_frame % 30 == 0)
        rt_log("route trace %u: pos %.0f %.0f mv %.1f %.1f face %.2f %.2f tgt %d (%.0f %.0f) a %.1f sx %d back %d", s_frame, x, z, mx, mz, fx, fz, t, s_rp[t][0], s_rp[t][1], a, in->sx, s_rback);
    if (s_frame % 300 == 0 && s_rlog++ < 200)
        rt_log("padplay route: frame %u: point %d, target %d (%.0f units), speed %.0f%s", s_frame, s_rcur, t, sqrtf(dx * dx + dz * dz), speed, s_rback ? ", backing up" : "");
    return true;
}

/* The anchor key: scene, and whether a race director exists (the race loaded) */
static uint32_t anchor_key(uint32_t scene, bool race) { return (scene & 0xFFFF) << 4 | (race ? 1u : 0u); }

/* Replay in a race: the entry nearest the kart around s_pos (within this stretch of race entries) */
static void follow(float x, float z) {
    size_t lo = s_pos > PR_BACK ? s_pos - PR_BACK : 0, hi = s_pos + PR_AHEAD;
    while (lo < s_pos && !s_rec[lo].hasp) lo++;
    size_t best = s_pos;
    float bd = (s_rec[s_pos].x - x) * (s_rec[s_pos].x - x) + (s_rec[s_pos].z - z) * (s_rec[s_pos].z - z);
    for (size_t i = lo; i < hi && i < s_nrec && s_rec[i].hasp; i++) {
        float d = (s_rec[i].x - x) * (s_rec[i].x - x) + (s_rec[i].z - z) * (s_rec[i].z - z);
        if (d + 2500.0f < bd) bd = d, best = i; /* 50 units nearer: standing still (the countdown) keeps time */
    }
    if (best != s_pos && s_follow_log < 40 && (best > s_pos + 8 || best + 8 < s_pos)) {
        s_follow_log++;
        rt_log("padplay: frame %u: off the recorded time by %d frames (%.0f units from the line)", s_frame, (int)best - (int)s_pos, sqrtf(bd));
    }
    s_pos = best;
    static float worst;
    if (sqrtf(bd) > worst) worst = sqrtf(bd);
    if (s_frame % 300 == 0) rt_log("padplay: frame %u: %.0f units from the recorded line (furthest in 5 s %.0f)", s_frame, sqrtf(bd), worst), worst = 0;
}

void padrec_frame(PadStatus *p0, uint32_t scene, bool race, uint32_t r13) {
    if (s_mode < 0) init();
    if (!s_mode) return;
    s_frame++;
    uint32_t key = anchor_key(scene, race);
    bool changed = key != s_key;
    s_key = key;
    float kx = 0, kz = 0, kfx = 0, kfz = 0;
    bool kpos = race && kart0_pos(r13, &kx, &kz, &kfx, &kfz);
    if (s_mode == 1) {
        if (changed) { flush_run(); fprintf(s_out, "@ %x\n", key); fflush(s_out); }
        PrIn in = {p0->button, p0->stick_x, p0->stick_y, p0->substick_x, p0->substick_y, p0->trigger_l, p0->trigger_r,
                   (uint8_t)((g_jump_held & 1) | (g_trick_pad & 15) << 1 | (g_trick_pad >> TRICK_SIG_BIT & 1) << 5)};
        if (kpos) { /* a race frame: one line each, with the position */
            flush_run();
            put_in(&in, 1);
            fprintf(s_out, " %.1f %.1f\n", kx, kz);
        } else {
            if (s_run && (memcmp(&in, &s_last, sizeof in) != 0 || s_run >= 100000)) flush_run();
            s_last = in;
            s_run++;
        }
        if ((s_frame & 1023) == 0) { flush_run(); fflush(s_out); }
        return;
    }
    /* replay */
    PrIn in = {0};
    if (changed) {
        /* the game reached an anchor: the file's next one with this key (from where it stands) */
        size_t i = s_pos;
        while (i < s_nrec && !(s_rec[i].n == 0 && s_rec[i].key == key)) i++;
        if (i < s_nrec) {
            if (i != s_pos || s_left) rt_log("padplay: anchor %x at frame %u: skipped %s", key, s_frame, s_waiting ? "nothing (it waited)" : "ahead");
            s_pos = i + 1, s_left = 0, s_waiting = false;
        } else rt_log("padplay: anchor %x at frame %u: not in the file from here (kept going)", key, s_frame);
        s_phave = false;
    }
    if (!s_done) {
        if (!s_left && s_pos < s_nrec) s_left = s_rec[s_pos].n;
        if (s_pos >= s_nrec) {
            s_done = true;
            rt_log("padplay: done at frame %u", s_frame);
        } else if (!s_left) { /* the file's next line is an anchor the game has not reached: wait */
            if (!s_waiting) rt_log("padplay: frame %u: waiting for anchor %x", s_frame, s_rec[s_pos].key);
            s_waiting = true;
        } else {
            if (s_rec[s_pos].hasp && kpos) follow(kx, kz);
            const PrRec *r = &s_rec[s_pos];
            in = r->in;
            if (r->hasp && kpos && s_phave) {
                /* steer toward the recorded line PR_LOOK entries on: the angle between the kart's motion
                 * (its facing when slow: stopped against something) and the way there */
                size_t t = s_pos;
                for (int k = 0; k < PR_LOOK && t + 1 < s_nrec && s_rec[t + 1].hasp; k++) t++;
                float mx = kx - s_px, mz = kz - s_pz, dx = s_rec[t].x - kx, dz = s_rec[t].z - kz;
                float ml = sqrtf(mx * mx + mz * mz), dl = sqrtf(dx * dx + dz * dz);
                if (ml < 10.0f) mx = kfx, mz = kfz, ml = sqrtf(mx * mx + mz * mz);
                if (ml > 1e-3f && dl > 50.0f) {
                    float a = atan2f(mx * dz - mz * dx, mx * dx + mz * dz) * 57.29578f;
                    float s = (float)in.sx + PR_SIGN * PR_GAIN * a;
                    in.sx = (int8_t)(s > 100 ? 100 : s < -100 ? -100 : s);
                }
            }
            if (r->hasp) s_pos++, s_left = 0;
            else if (--s_left == 0) s_pos++;
        }
    }
    if (kpos && s_phave) route_drive(&in, kx, kz, kx - s_px, kz - s_pz, kfx, kfz);
    if (kpos) s_px = kx, s_pz = kz, s_phave = true;
    p0->button = in.button;
    p0->stick_x = in.sx, p0->stick_y = in.sy, p0->substick_x = in.cx, p0->substick_y = in.cy;
    p0->trigger_l = in.tl, p0->trigger_r = in.tr;
    p0->analog_a = p0->analog_b = 0;
    p0->connected = true;
    g_jump_held = (g_jump_held & ~1u) | (in.extra & 1);
    g_trick_pad = (g_trick_pad & ~(15u | 1u << TRICK_SIG_BIT)) | (uint32_t)(in.extra >> 1 & 15) | (uint32_t)(in.extra >> 5 & 1) << TRICK_SIG_BIT;
}

void padrec_close(void) {
    if (s_mode == 1 && s_out) { flush_run(); fclose(s_out); s_out = NULL; }
}
