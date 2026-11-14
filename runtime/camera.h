/* The cameras (camera.c): widescreen aspect, two players side by side, field of view and first person.
 * Natives registered in native.c's NATIVE_LIST. */
#pragma once
#include "native_impl.h"

uint64_t i_getPerspAspectDiv2__6SystemFv(Ctx *c, uint8_t *M, ARGS);
uint64_t i_getPerspAspectDiv4__6SystemFv(Ctx *c, uint8_t *M, ARGS);
uint64_t i_getPerspAspectSub__6SystemFv(Ctx *c, uint8_t *M, ARGS);
uint64_t i_C_MTXPerspective(Ctx *c, uint8_t *M, ARGS);
uint64_t i_FUN_8029e0a8(Ctx *c, uint8_t *M, ARGS);
uint64_t i_FUN_80027ebc(Ctx *c, uint8_t *M, ARGS);
uint64_t i_FUN_801a77e8(Ctx *c, uint8_t *M, ARGS);
uint64_t i_FUN_8000a260(Ctx *c, uint8_t *M, ARGS);
uint64_t i_FUN_802b6bd4(Ctx *c, uint8_t *M, ARGS);
uint64_t i_FUN_801908b4(Ctx *c, uint8_t *M, ARGS);
uint64_t i_FUN_80190a88(Ctx *c, uint8_t *M, ARGS);

/* native.c's: frames the mirror's view isn't culled for (split screen has no mirror) */
extern uint32_t g_mirror_off;

/* for the HUD and native.c: two players side by side (the tables hold the columns; the live check; the
 * finish), the camera aspect read again, the number of screens */
extern bool g_vsplit_on;
void vsplit_live(Ctx *c, uint8_t *M);
void vs_goal(Ctx *c, uint8_t *M);
void aspect_refresh(Ctx *c, uint8_t *M);
int screens(Ctx *c, uint8_t *M);
