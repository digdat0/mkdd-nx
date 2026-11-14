/* The drivers (drivers.c): character packs' models and animations in a race, and the character select
 * screen's grid (rows, models, flags, moves). Natives registered in native.c's NATIVE_LIST. */
#pragma once
#include "native_impl.h"

uint64_t i___ct__11DriverModelFP10KartLoaderiUsbi(Ctx *c, uint8_t *M, ARGS);
uint64_t i___ct__Q29GameAudio17CharacterSoundMgrFP3VecP7JKRHeapUc7ECharID(Ctx *c, uint8_t *M, ARGS);
uint64_t i_setVoice__Q29GameAudio17CharacterSoundMgrFUc(Ctx *c, uint8_t *M, ARGS);
uint64_t i___ct__13DriverManagerFv(Ctx *c, uint8_t *M, ARGS);
uint64_t i_getPtr__6ResMgrFQ26ResMgr9ArchiveIdPCc(Ctx *c, uint8_t *M, ARGS);
uint64_t i_loadAnimation__16DriverAnimBufMgrFii(Ctx *c, uint8_t *M, ARGS);
uint64_t i_characterAnm__9SceneMenuFv(Ctx *c, uint8_t *M, ARGS);
uint64_t i_setCharacterFlag__9SceneMenuFv(Ctx *c, uint8_t *M, ARGS);
uint64_t i_calc__13MenuTitleLineFv(Ctx *c, uint8_t *M, ARGS);
uint64_t i_FUN_8015639c(Ctx *c, uint8_t *M, ARGS);
uint64_t i_selectUp__9SceneMenuFi(Ctx *c, uint8_t *M, ARGS);
uint64_t i_selectDown__9SceneMenuFi(Ctx *c, uint8_t *M, ARGS);
uint64_t i_selectLeft__9SceneMenuFi(Ctx *c, uint8_t *M, ARGS);
uint64_t i_selectRight__9SceneMenuFi(Ctx *c, uint8_t *M, ARGS);
uint64_t i_character__9SceneMenuFv(Ctx *c, uint8_t *M, ARGS);

/* shared with native.c: a guest call with up to five arguments (drivers.c), the select screen's kart
 * models freed (native.c) */
uint32_t sel_call(Ctx *c, uint32_t fn, uint32_t a, uint32_t b, uint32_t c3, uint32_t d, uint32_t e);
void kart_wipe(Ctx *c, uint8_t *M, uint32_t cs);

/* for the HUD: a DriverModel in the back seat, with the karts' two driver models */
extern uint32_t g_cp_dm[8][2];
bool dm_back(Ctx *c, uint8_t *M, uint32_t dm);
