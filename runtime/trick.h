/* The kart's tricks (trick.c, rt.h TRICKS): what native.c's trick HUD reads of them. */
#pragma once
#include "native_impl.h"

#define COMBO_WINDOW 900    /* frames on the ground after a clean landing before the combo banks (the arenas'
                             * jumps are often more than 6 s apart; 15 s, 2026-11-11) */
#define COMBO_MULT_MAX 10
#define STALE_N 5           /* a trick among the kart's last STALE_N is worth STALE_KEEP as much for each time */
#define STALE_KEEP 0.7f
#define METER_FULL 1000u
enum { T_NONE, T_FRONT, T_BACK, T_SPIN, T_ROLL, T_WHIP, T_TWIST, T_BTWIST, T_SUPERMAN, T_CLIFF, T_SIDEW, T_RSIDEW,
       T_HANDSTAND, T_LAZY, T_HURRICANE, T_TEMPEST, T_METEOR, T_CYCLONE, T_TYPHOON,
       T_KISS, T_CORDOVA, T_NACNAC, T_CANCAN, T_VARIAL, T_ROCKSOLID, T_FLAIR, T_SWITCH,
       T_HELI, T_SOMER, T_CART, T_SURF, T_RODEO, T_CORK, T_COUNT }; /* ZR + a direction (and again), JUMP + a direction twice, ZR alone (and again) */
static const uint16_t T_PTS[T_COUNT] = {0, 100, 100, 100, 150, 120, 220, 220, 120, 120, 150, 150, 150, 150, 600, 600, 600, 600, 600,
                                             180, 170, 160, 160, 250, 220, 260, 200, 280, 230, 240, 210, 170, 270};
enum { G_NONE, G_PERFECT, G_SKETCHY, G_BAIL, G_CATCH };
typedef struct {
    uint32_t body, pad_was, presses;
    int air, upjump;                                /* frames in the air, tricks possible this jump */
    float qc[4], peak;                              /* the drawn kart's turn (its own frame), the jump's height */
    float w[2], acc[2];                             /* free turning: speed and this jump's turn round x (flip), y (spin) */
    int nturn[2][2];                                /* full turns of each counted, either way */
    uint32_t mask;                                  /* directions taken by a special or pose: no turning until let go */
    int tap_down[4], tap_since[4];                  /* per direction: frames held, frames since a short tap */
    float tap_q[4][4], tap_acc[4][2];               /* ... the turn when it was pressed */
    int ck, cf, cl, sig, sigdone;                   /* a canned trick: kind, frame, length; the signature running, done */
    float cbase[4], cblend[4];
    int rstate, rkind, rnext, rframe, rlen, rcount, jump_since; /* the back rider's trick */
    int zr_win, zr_last, zr_up, rswap;              /* frames left for a direction after ZR pressed in the air; the last ZR trick's direction, frames left to press it again; the SWITCHEROO's swap asked for */
    int zl_last, zl_up;                             /* the last JUMP pose's direction, frames left to press it again (JUMP held) */
    int zr_count;                                   /* ZR presses: RODEO, CORKSCREW in turn (rcount: JUMP's) */
    uint32_t rd;                                    /* its DriverModel, 0 until tk_rider measures it */
    float rup, rfwd;                                /* its head then, in the model's frame */
    float rh;                                       /* its amount, 0-1 */
    uint32_t rdc;                                   /* the last measure taken upright (tk_rider_rest), 0 none */
    float rupc, rfwdc;
    int calm;                                       /* frames since the kart or rider last turned */
    float hprev, fall, grav, y0, rmax;              /* kart height last frame, falling speed and its gain a frame, at takeoff, most above it */
    float x0, z0, v0;                               /* where it took off (x, z) and its speed there: the flight's length in the log */
    int since_land;                                 /* frames since a jump's touchdown (999: long ago): the bounce after it damped */
    int air_up;                                     /* the air frame tricks became possible, 0 not yet */
    uint8_t jt[8], jc[8];                           /* this jump's tricks and how many of each */
    int jn;
    float combo;                                    /* the combo's points so far (landed jumps) */
    uint32_t cids;                                  /* ... its different tricks (bits) */
    int window;                                     /* frames left to take off again */
    uint8_t hist[STALE_N];                          /* the last tricks scored, oldest first */
    int histn;
    uint32_t meter;
    int lr_since;                                   /* frames since L or R was pressed */
    float sq;                                       /* a perfect landing's dip, 1 down to 0 */
    char pop[120];                                  /* HUD: this jump's tricks ("A + DOUBLE B"), frames left */
    int pop_t, grade, grade_t, bank_t;              /* ... the landing's grade; a banked combo */
    uint32_t bank;
    uint32_t gain; int gain_t;                      /* HUD: the last landing's points (on the score at once) */
    int cp_plan, cp_t, cp_dir, cp_catch, cp_stop;   /* a computer's plan for this jump */
    float cp_goal;
    uint32_t gbuf;                                  /* presses on the ground in the last PRE_BUF frames (TRICK_JUMP, directions) */
    int gbuf_age, pend;                             /* ... frames since the last; a special tapped before tricks start (direction + 1) */
    int resc;                                       /* Lakitu has carried it since it last touched the ground: no tricks */
} Trick;
extern Trick g_tk[8]; /* per kart number */
/* frames in a trick battle's round (2:00; MKDD_TRICK_SECS changes it, tests) */
uint32_t tk_len(void);
/* the signature's star each frame (from KartStar's step, native.c) */
void tk_star_frame(Ctx *c, uint8_t *M, uint32_t ctrl);
/* order[0..n): the karts, most points first (the earlier kart on a tie): the battle's end (trick_battle_tick) and the results */
void trick_rank(int order[8], int n);
/* the natives: KartDisp::MakeKartDisp and MakeTireDispMatrix */
uint64_t i_FUN_802d3c60(Ctx *c, uint8_t *M, ARGS);
uint64_t i_FUN_802d320c(Ctx *c, uint8_t *M, ARGS);
