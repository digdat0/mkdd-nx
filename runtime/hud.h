/* The race HUD (hud.c): its natives, and what native.c shares with it. */
#pragma once
#include "native_impl.h"
#include "camera.h"
#include "drivers.h"

uint64_t i_draw__6Race2DFv(Ctx *c, uint8_t *M, ARGS);
uint64_t i_drawCourse__6Race2DFv(Ctx *c, uint8_t *M, ARGS);
uint64_t i_draw__11WipeManagerFv(Ctx *c, uint8_t *M, ARGS);
uint64_t i_draw__5MarioFfQ28JUtility6TColor(Ctx *c, uint8_t *M, ARGS);
uint64_t i_drawK2D__10K2DPictureFffffb(Ctx *c, uint8_t *M, ARGS);

/* native.c's, for the HUD: the mirror's last ask and GXLoadTexObj (the split screen's tables, the
 * camera aspect and the number of screens are camera.h's; a DriverModel in the back seat drivers.h's) */
extern uint64_t g_mirror_ns;
uint64_t i_GXLoadTexObj(Ctx *c, uint8_t *M, ARGS);
