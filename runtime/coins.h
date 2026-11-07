/* Coins (coins.c): a layout made from each race course's own data, for the stock courses and any
 * level pack's. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "gc_core.h"

extern volatile uint32_t g_coins; /* 0 off, else on (the - menu's GAME > COINS) */

#define COIN_MAX 384
typedef struct {
    float pos[3]; /* on the ground plus COIN_LIFT */
    uint8_t attr; /* the ground's attribute under it (CrsGround +0x4c) */
    uint8_t water; /* over water (attribute 17, Peach Beach's tide): pos[1] follows the surface, never below the ground */
    float ground;  /* the ground's height under it */
} Coin;

/* the course a race loads (battle.c, loadCrsData: the BOL and its header hash); mode gRaceInfo +8 */
void coins_course(Ctx *c, uint8_t *M, uint32_t bol, uint32_t hash, uint32_t mode);
/* every race frame (native.c, after KartCtrl ctrl's per-frame FUN_8029fd40): builds the layout on the first, then the pickups */
void coins_frame(Ctx *c, uint8_t *M, uint32_t ctrl);
/* this race has coins (the HUD counter shows) */
bool coins_racing(void);
/* the pause's RETRY: coins back, none held */
void coins_restart(void);
/* the coins kart k holds now (KartCtrl order) */
int coins_held(int kart);
/* J3DDrawBuffer::drawHead (native.c), after drawing buf: the coins, after the race's opaque item list */
void coins_draw(Ctx *c, uint8_t *M, uint32_t buf);
/* the layout built for this race (NULL until then) and its size */
const Coin *coins_layout(int *n);
