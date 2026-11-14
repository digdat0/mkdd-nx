/* Trick Style track effects (trickfx.c): spring pads, geysers, tornadoes from mods/trick/fx/<key>.txt */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "gc_core.h"

/* the course a race loads (battle.c, loadCrsData: the BOL's course ID and header hash, as route_course) */
void fx_course(uint32_t id, uint32_t hash);
void fx_restart(void); /* the pause's RETRY: no push, tornado hold or Lakitu carry kept from before */
/* every kart every race frame (trick.c tk_step): true when an effect threw it up this frame */
bool fx_kart(Ctx *c, uint8_t *M, uint32_t body, uint32_t kart, bool ground);
/* J3DDrawBuffer::drawHead (native.c), after the coins */
void fx_draw(Ctx *c, uint8_t *M, uint32_t buf);
