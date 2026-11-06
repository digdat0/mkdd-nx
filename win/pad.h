/* Controllers through SDL2's game controller API (pad_sdl.c): Xbox, PlayStation, Switch Pro /
 * Joy-Con, GameCube adapters (WinUSB driver) and anything in SDL's or config/gamecontrollerdb.txt's
 * mappings. Buttons go by their label as Switch buttons (HidNpadButton_*): A = A, Cross = A ... */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define PAD_SLOTS 4

typedef struct {
    bool on;
    bool gc;           /* a GameCube controller: its own layout (Z = ZR, analog L / R), as on the Switch */
    bool xface;        /* face buttons laid out as Xbox / PlayStation (Y top, X left, A bottom, B right), not Nintendo's */
    uint64_t b, prev;  /* HidNpadButton_* bits held now / at the previous poll */
    int32_t lx, ly, rx, ry; /* sticks, +-32767, up positive, dead zone applied */
    uint8_t lt, rt;    /* analog triggers 0-255 */
    char name[64];
    int player;        /* 0-3: the player it is: the first free one when it connects; the launcher can change it */
} Pad;

extern Pad g_pads[PAD_SLOTS]; /* in connection order; a slot is kept free when its pad leaves */

bool pad_init(void); /* false: no SDL controller support (the keyboard still works) */
void pad_poll(void);
void pad_rumble(int slot, uint16_t strength); /* 0 stops */
void pad_quit(void);
