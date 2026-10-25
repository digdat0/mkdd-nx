/* Native versions of hot guest functions, called directly from generated code.
 *
 * tools/gen_c.py reads NATIVE_LIST below: a call to one of these guest functions becomes a plain
 * C call n_<name>(c, M, a0..a7) with the arguments taken from the caller's register locals,
 * instead of storing the register file, entering the recompiled function (or the HLE dispatcher)
 * and reloading. Other ways in (indirect calls, the runtime) still reach the recompiled function.
 *
 * X(name, args, ret, guest):
 *   name   the guest function's name in sheets/functions.csv
 *   args   one letter per argument: r = next GPR (r3, r4, ...), f = next FPR (f1, f2, ...; passed
 *          as the 64-bit register contents)
 *   ret    'r' result in r3, 'f' in f1 (ps0 and ps1), '-' none
 *   guest  1 if it may call guest code: the caller then stores its registers first (r1 etc.);
 *          also needed by any native that reads r1 (e.g. to use the original's stack frame as
 *          scratch): with 0 the register file's r1 can be stale and the scratch lands on the
 *          caller's live stack (calcMPos once corrupted the limb IK, stretched arms);
 *          2 if it writes MSR: callers that keep MSR in a local make a normal call instead;
 *          3 if it writes the GX gather pipe (verified by comparing the FIFO bytes);
 *          4 both 1 and 3
 *
 * Semantics must match the recompiled code exactly (same memory writes, same float rounding).
 * MKDD_NATIVE_VERIFY=N (PC builds from gen_c.py --native-verify) runs every Nth call of each
 * function both ways from the same RAM snapshot and reports differences. */
#pragma once
#include "gc_core.h"

#define NATIVE_LIST(X) \
    X(changeKart__9SceneMenuFiiii, "rrrrr", 'r', 1) \
    X(FUN_8018c878, "rrrrrr", '-', 1) \
    X(loadCrsData__6CourseFb, "rr", '-', 1) \
    X(FUN_802b1f60, "", 'r', 1) \
    X(FUN_80226f1c, "rrr", 'r', 1) \
    X(FUN_802b2174, "rr", 'r', 0) \
    X(FUN_802be4cc, "r", '-', 1) \
    X(FUN_802be8d4, "rr", '-', 1) \
    X(comebackRescure__13RivalBodyCtrlFP7CLPointb, "rrr", '-', 1) \
    X(calc__10GeoItemBoxFv, "r", '-', 1) \
    X(doItemCtrl__13RivalItemCtrlFv, "r", '-', 1) \
    X(FUN_802d5e28, "r", '-', 1) \
    X(__ct__11BombStart2DFP7JKRHeap, "rr", 'r', 1) \
    X(doSterrCtrl__13RivalBodyCtrlFv, "r", '-', 1) \
    X(mode_init__9SceneMenuFv, "r", '-', 1) \
    X(mode__9SceneMenuFv, "r", '-', 1) \
    X(mode_end__9SceneMenuFv, "r", '-', 1) \
    X(setRaceData__9SceneMenuFv, "r", '-', 1) \
    X(getCharSel__9SceneMenuFRiRi, "rrr", '-', 1) \
    X(FUN_801f1cf4, "r", '-', 1) \
    X(getSpecialKind__7ItemObjFRCiRCUc, "rr", 'r', 0) \
    X(IsSpecialCharacter__10ItemObjMgrFl, "r", 'r', 0) \
    X(PSMTXCopy, "rr", '-', 0) \
    X(PSMTXConcat, "rrr", '-', 0) \
    X(PSMTXMultVec, "rrr", '-', 0) \
    X(PSVECCrossProduct, "rrr", '-', 0) \
    X(PSVECMag, "r", 'f', 0) \
    X(PSMTXInverse, "rr", 'r', 0) \
    X(OSDisableInterrupts, "", 'r', 2) \
    X(OSEnableInterrupts, "", 'r', 2) \
    X(OSRestoreInterrupts, "r", 'r', 2) \
    X(FUN_8005b564, "rrrrr", '-', 0) \
    X(getTransform__19J3DAnmTransformFullCFUsP16J3DTransformInfo, "rrr", '-', 0) \
    X(J3DGetKeyFrameInterpolation_s___FfP18J3DAnmKeyTableBasePs, "rrf", 'f', 0) \
    X(J3DMtxProjConcat__FPA4_fPA4_fPA4_f, "rrr", '-', 0) \
    X(FUN_80026a1c, "rrrf", '-', 0) \
    X(FUN_80026a74, "rrfff", '-', 0) \
    X(FUN_80026880, "rrrr", '-', 0) \
    X(FUN_80026944, "rrrf", '-', 0) \
    X(FUN_801912c4, "rr", '-', 0) \
    X(FUN_80182fa4, "rr", '-', 0) \
    X(GXLoadPosMtxImm, "rr", '-', 3) \
    X(GXLoadTexMtxImm, "rrr", '-', 3) \
    X(GXSetTevColor, "rr", '-', 3) \
    X(GXLoadLightObjImm, "rr", '-', 3) \
    X(GXSetArray, "rrr", '-', 3) \
    X(recursiveCalc__8J3DJointFv, "r", '-', 1) \
    X(PSMTXQuat, "rr", '-', 0) \
    X(FUN_800089e4, "rrrr", '-', 1) \
    X(FUN_8017b8bc, "rrr", '-', 0) \
    X(FUN_8017b96c, "rrrffff", '-', 0) \
    X(FUN_8017ddd4, "rrrrr", 'r', 0) \
    X(getPerspAspectDiv2__6SystemFv, "", 'f', 0) \
    X(getPerspAspectDiv4__6SystemFv, "", 'f', 0) \
    X(getPerspAspectSub__6SystemFv, "", 'f', 0) \
    X(C_MTXPerspective, "rffff", '-', 1) \
    X(FUN_8029e0a8, "rr", '-', 1) \
    X(FUN_801a77e8, "rffffr", '-', 1) \
    X(FUN_8000a260, "r", '-', 1) \
    X(FUN_802b6bd4, "r", '-', 1) \
    X(FUN_801908b4, "rrr", '-', 4) \
    X(FUN_80190a88, "rrr", '-', 4) \
    X(PSVECNormalize, "rr", '-', 0) \
    X(J3DPSCalcInverseTranspose__FPA4_fPA3_f, "rr", 'r', 0) \
    X(J3DGetTranslateRotateMtx__FRC16J3DTransformInfoPA4_f, "rr", '-', 0) \
    X(J3DGetTextureMtx__FRC17J3DTextureSRTInfoRC3VecPA4_f, "rrr", '-', 0) \
    X(J3DGetTextureMtxOld__FRC17J3DTextureSRTInfoRC3VecPA4_f, "rrr", '-', 0) \
    X(J3DGetKeyFrameInterpolation_f___FfP18J3DAnmKeyTableBasePf, "rrf", 'f', 0) \
    X(calcCurrentMtx__11J3DMaterialFv, "r", '-', 1) \
    X(FUN_800546a4, "rr", '-', 0) \
    X(stMakePlaneParam__FR12stPlaneParamRQ29JGeometry8TVec3_f_RCQ29JGeometry8TVec3_f_, "rrr", 'r', 0) \
    X(FUN_8017b7ac, "rrr", '-', 0) \
    X(FUN_80054080, "rrr", '-', 0) \
    X(GDPadCurr32, "", '-', 0) \
    X(FUN_800671c8, "rr", '-', 1) \
    X(FUN_80067338, "rr", '-', 1) \
    X(FUN_80067a64, "rr", '-', 3) \
    X(FUN_80067ae8, "rr", '-', 3) \
    X(FUN_8000a3c8, "rrrf", 'r', 0) \
    X(FUN_80183afc, "rrrr", '-', 0) \
    X(FUN_80183934, "rrrrf", '-', 0) \
    X(FUN_8017cea8, "rrr", 'r', 0) \
    X(FUN_8017cffc, "rr", 'f', 0) \
    X(FUN_8018af28, "rrr", '-', 0) \
    X(FUN_8005826c, "rrrf", '-', 0) \
    X(DCInvalidateRange, "rr", '-', 0) \
    X(DCFlushRange, "rr", '-', 0) \
    X(DCStoreRange, "rr", '-', 0) \
    X(DCFlushRangeNoSync, "rr", '-', 0) \
    X(DCStoreRangeNoSync, "rr", '-', 0) \
    X(GDFlushCurrToMem, "", '-', 0) \
    X(FUN_80009398, "rr", '-', 0) \
    X(FUN_8005c08c, "r", '-', 0) \
    X(FUN_8005be04, "r", '-', 0) \
    X(FUN_8000a1b4, "r", '-', 1) \
    X(FUN_8017fd0c, "rrrrr", '-', 1) \
    X(FUN_80067bc8, "rr", '-', 3) \
    X(GXLoadTexObjPreLoaded, "rrr", '-', 4) \
    X(calcDiffTexMtx__8J3DModelFv, "r", '-', 1) \
    X(FUN_801826d4, "rrr", '-', 1) \
    X(FUN_80183c08, "rr", '-', 0) \
    X(FUN_80183d2c, "rrr", '-', 1) \
    X(setPos__15JAISoundHandlesFRCQ29JGeometry8TVec3_f_, "rr", '-', 0) \
    X(diff__11J3DMaterialFUl, "rr", '-', 1) \
    X(calc__14J3DMaterialAnmCFP11J3DMaterial, "rr", '-', 1) \
    X(FUN_80026ae0, "rff", 'f', 0) \
    X(update__12J3DFrameCtrlFv, "r", '-', 0) \
    X(PSMTXScale, "rfff", '-', 0) \
    X(calcJointScaleMtx__6IKCtrlFPQ29JGeometry64TPosition3_Q29JGeometry38TMatrix34_Q29JGeometry13SMatrix34C_f___PA4_f, "rr", '-', 0) \
    X(calcScaleMtx__10LumbarCtrlFPA4_f, "rr", '-', 0) \
    X(stVecNormalize__FRQ29JGeometry8TVec3_f_, "r", 'r', 0) \
    X(calcMPos__11DriverModelFRQ29JGeometry8TVec3_f_RQ29JGeometry8TVec3_f_RQ29JGeometry8TVec3_f_ffPQ29JGeometry8TVec3_f_, "rrrrrff", '-', 1) \
    X(GXCallDisplayList, "rr", '-', 4) \
    X(FUN_80050ed0, "r", '-', 4) \
    X(FUN_80050f70, "r", '-', 4) \
    X(FUN_800507a0, "r", '-', 4) \
    X(draw__12J3DMatPacketFv, "r", '-', 4) \
    X(drawHead__13J3DDrawBufferCFv, "r", '-', 4) \
    X(FUN_80067b58, "rr", '-', 3) \
    X(FUN_80067c64, "rr", '-', 3) \
    X(J3DScaleNrmMtx33__FPA3_fRC3Vec, "rr", '-', 0) \
    X(loadNrmMtx__21J3DShapeMtxConcatViewCFiUsPA4_f, "rrrr", '-', 4) \
    X(loadNrmMtx__26J3DShapeMtxMultiConcatViewCFiUsPA4_f, "rrrr", '-', 4) \
    X(loadMtxConcatView_PNGP__21J3DShapeMtxConcatViewCFiUs, "rrr", '-', 4) \
    X(load__21J3DShapeMtxConcatViewCFv, "r", '-', 4) \
    X(load__26J3DShapeMtxMultiConcatViewCFv, "r", '-', 4) \
    X(getNewColor__10K2DPictureFPQ28JUtility6TColor, "rr", '-', 0) \
    X(sin, "f", 'f', 1) \
    X(cos, "f", 'f', 1) \
    X(getTexNo__16J3DAnmTexPatternCFUsPUs, "rrr", '-', 0) \
    X(FUN_80183820, "r", '-', 0) \
    X(GXSetTevOrder, "rrrr", '-', 3) \
    X(getTevColorReg__15J3DAnmTevRegKeyCFUsP11_GXColorS10, "rrr", '-', 1) \
    X(__cvt_fp2unsigned, "f", 'r', 0) \
    X(diff__8J3DModelFv, "r", '-', 1) \
    X(draw__6Race2DFv, "r", '-', 4) \
    X(draw__11WipeManagerFv, "r", '-', 4) \
    X(__ct__11DriverModelFP10KartLoaderiUsbi, "rrrrrr", 'r', 1) \
    X(__ct__13DriverManagerFv, "r", 'r', 1) \
    X(loadAnimation__16DriverAnimBufMgrFii, "rrr", 'r', 1) \
    X(character__9SceneMenuFv, "r", '-', 1) \
    X(characterAnm__9SceneMenuFv, "r", '-', 1) \
    X(getPtr__6ResMgrFQ26ResMgr9ArchiveIdPCc, "rr", 'r', 1) \
    X(drawCourse__6Race2DFv, "r", '-', 4)     X(__GXSetDirtyState, "", '-', 4)     X(GXLoadTexObj, "rr", '-', 4)     X(FUN_80023828, "rr", '-', 4)     X(FUN_8007ce10, "rr", '-', 4)     X(FUN_8007c198, "rr", '-', 4)     X(FUN_8007c260, "rr", '-', 4)     X(FUN_8007de24, "rr", '-', 4)     X(FUN_8007d304, "rr", '-', 4)     X(FUN_8007d3e8, "rr", '-', 4)     X(GXSetTevDirect, "r", '-', 3)     X(GXSetTevColorOp, "rrrrrr", '-', 3)     X(GXSetTevAlphaOp, "rrrrrr", '-', 3)     X(__GXSetMatrixIndex, "r", '-', 3)     X(GXSetTexCoordGen2, "rrrrrr", '-', 3)     X(GXSetChanCtrl, "rrrrrrr", '-', 3)     X(GXSetVtxDesc, "rr", '-', 0)     X(GXBegin, "rrr", '-', 4)     X(IsHitSphere__12ObjColSphereFQ29JGeometry8TVec3_f_Q29JGeometry8TVec3_f_f, "rrrf", 'r', 0)     X(IsHitKart__12GeographyObjFRCQ29JGeometry8TVec3_f_fPQ29JGeometry8TVec3_f_, "rrfr", 'r', 1)     X(calc__13ShadowManagerFv, "r", '-', 1)     X(frameWorkAll__Q29GameAudio38SoundMgr_Q29GameAudio14ObjectSoundMgr_Fv, "", '-', 1)     X(setCurrentViewNo__19GeographyObjManagerFUl, "rr", '-', 1)     X(calc__11J3DMaterialFPA4_Cf, "rr", '-', 1)     X(calcMaterial__8J3DModelFv, "r", '-', 1)     X(beginDiff__12J3DMatPacketFv, "r", '-', 2)     X(viewCalc__19GeographyObjManagerFUl, "rr", '-', 1)     X(calc__8J3DModelFv, "r", '-', 1)     X(Collide_norm__19GeographyObjManagerFRC31JSUListIterator_12GeographyObj_RCiRCQ29JGeometry8TVec3_f_RCf, "rrrrr", '-', 1)     X(drawEachParticle__14stParticleDrawFR29JSUListIterator_10stParticle_, "rr", '-', 1)     X(FUN_800724d0, "rr", '-', 1)     X(drawK2D__10K2DPictureFffffb, "rffffr", '-', 4)     X(FUN_80093294, "rrrr", '-', 1)     X(FUN_800505bc, "r", '-', 4)     X(FUN_80182d74, "rr", '-', 1)     X(drawParentBillBoardCommon__14stParticleDrawFR29JSUListIterator_10stParticle_PA4_f, "rrr", '-', 4)     X(draw__13ShadowManagerFUlRQ27JGadget60TList_P11ShadowModel_Q27JGadget26TAllocator_P11ShadowModel__, "rrr", '-', 4)     X(FUN_80085f48, "rr", 'r', 1)     X(Calc_norm__19GeographyObjManagerFRC31JSUListIterator_12GeographyObj_, "rr", '-', 1)     X(viewCalc__13ShadowManagerFUl, "rr", '-', 1)     X(prepareShapePackets__8J3DModelFv, "r", '-', 0)     X(calc__12J3DJointTreeFP12J3DMtxBufferRC3VecRA3_A4_Cf, "rrrr", '-', 1)

#define NATIVE_DECL(name, args, ret, guest) \
    uint64_t n_##name(Ctx *c, uint8_t *M, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
NATIVE_LIST(NATIVE_DECL)
#undef NATIVE_DECL

#define NATIVE_ENUM(name, args, ret, guest) NAT_##name,
enum { NATIVE_LIST(NATIVE_ENUM) NATIVE_COUNT };
#undef NATIVE_ENUM

extern int g_native_verify; /* 0 = off, else verify every Nth call */
void native_init(void);
/* Widescreen: game camera aspect multiplier (1 = original, 4/3 = 16:9 output) */
extern float g_aspect_scale;
/* Field of view: tan(fovy / 2) of the race cameras times this (1 = the game's own). */
extern float g_fov_scale;
void native_report(void);
