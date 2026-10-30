/* A pause screen for LAN races. The game has none there: every console simulates every kart from
 * the inputs alone, so one that stopped would stop them all (and the others drop the link after
 * ~20 s). PauseManager is built with its byte +0 set when NetGameMgr is up, and that byte skips
 * the whole pause logic in exec.
 *
 * Here the pause screen opens on START as in a one-player Grand Prix (CONTINUE / QUIT, the
 * controller picture), but only that screen sees the pause: tstPause answers "paused" to
 * PauseManager::exec and Pause2D::draw only, so the race, the HUD and the sound go on behind it
 * (the kart still gets this console's inputs, as the pad is shared with the menu). QUIT is the
 * game's own soft reset (START + X + B, ResetAgent): it ends the LAN session (NetGameMgr::end)
 * and goes back to the title screen; the other consoles see this one leave. The hooks are
 * natives (native.h NATIVE_LIST). GM4E01 addresses; MKDD Plus and RTGP have the same ones. */
#include "native_impl.h"
#include "rt.h"

#define PM_THIS 0x803CB6D8u     /* PauseManager::mThis: +0 net (no pause), +0x11 game over, +0x18 pause end */
#define PM_IS_PAUSE 0x803CB6DCu /* PauseManager::mIsPause */
#define PM_CHOICE 0x803CB6E4u   /* mPauseChoice (0xd: none) */
#define PM_PAD 0x803CB6ECu      /* mPausePad */
#define RACE_MGR 0x803CB7E8u    /* RaceMgr::sRaceManager: +0x38 RaceInfo (+1 LAN mode, +8 race mode) */
#define RESET_STATE 0x803CBE80u /* ResetAgent::msState; msForceMenu +8, msInvalid +9 */
#define PM_EXEC 0x80126804u     /* PauseManager::exec, 2372 bytes */
#define PM_EXEC_TST 0x80126A44u /* ... its first tstPause call returns here, before the net byte's checks */
#define P2D_DRAW 0x80128050u    /* Pause2D::draw, 108 bytes */
#define CHOICE_NONE 0xd
#define CHOICE_QUIT 8
#define MODE_GP 2

static bool lr_in(uint32_t lr, uint32_t f, uint32_t size) { return lr - f < size; }

/* the race's RaceInfo when it is a LAN race, else 0 */
static uint32_t lan_race(Ctx *c, uint8_t *M) {
    if (!g_lan_play) return 0;
    uint32_t mgr = ld32(c, M, RACE_MGR);
    uint32_t info = ram_ptr(mgr) ? ld32(c, M, mgr + 0x38) : 0;
    return ram_ptr(info) && mld(c, M, info + 1, 1) ? info : 0;
}

/* Before exec's pause checks: the net byte clear while the race runs, so START opens the pause
 * screen; set again once the race is over (game over / pause end), where the LAN results take
 * over, closing a pause screen still open then. */
static void gate(Ctx *c, uint8_t *M) {
    uint32_t pm = ld32(c, M, PM_THIS);
    if (!ram_ptr(pm)) return;
    bool over = mld(c, M, pm + 0x11, 1) || mld(c, M, pm + 0x18, 1);
    if (over && mld(c, M, PM_IS_PAUSE, 1)) {
        mst(c, M, PM_IS_PAUSE, 1, 0);
        mst(c, M, PM_PAD, 4, 0);
        mst(c, M, PM_CHOICE, 4, CHOICE_NONE);
        rt_log("lan pause: the race ended with the pause screen open: closed");
    }
    mst(c, M, pm, 1, over ? 1 : 0);
}

uint64_t i_tstPause__12PauseManagerFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t lr = (uint32_t)RD(c, 0x1020, 4);
    if (lr == PM_EXEC_TST && lan_race(c, M)) gate(c, M);
    ctx_call_orig(c, g_native_addr[NAT_tstPause__12PauseManagerFv]);
    uint32_t r = (uint32_t)ctx_gpr(c, 3);
    if (r && lan_race(c, M) && !lr_in(lr, PM_EXEC, 2372) && !lr_in(lr, P2D_DRAW, 108)) r = 0;
    return r;
}

/* QUIT: the soft reset instead of the race's own exit to the menus (which would leave the LAN
 * session up); the race sees no choice meanwhile. */
uint64_t i_getPauseChoice__12PauseManagerFv(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    ctx_call_orig(c, g_native_addr[NAT_getPauseChoice__12PauseManagerFv]);
    uint32_t r = (uint32_t)ctx_gpr(c, 3);
    if (r != CHOICE_QUIT || !lan_race(c, M)) return r;
    if (!ld32(c, M, RESET_STATE)) {
        if (mld(c, M, RESET_STATE + 9, 1)) {
            rt_log("lan pause: QUIT: the soft reset is blocked (msInvalid)");
        } else {
            mst(c, M, RESET_STATE + 8, 1, 0); /* ResetAgent::reset(false) */
            mst(c, M, RESET_STATE, 4, 1);
            rt_log("lan pause: QUIT: soft reset, back to the title screen");
        }
    }
    return CHOICE_NONE;
}

/* Pause2D builds its screen for the race mode: a LAN race (versus or battle) gets the Grand Prix
 * one, CONTINUE and QUIT, not versus' RETRY / CHANGE COURSE / CHANGE CHARACTER. */
uint64_t i___ct__7Pause2DFP7JKRHeap(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t info = lan_race(c, M);
    uint32_t mode = info ? ld32(c, M, info + 8) : MODE_GP;
    if (mode != MODE_GP) {
        mst(c, M, info + 8, 4, MODE_GP);
        rt_log("lan pause: the pause screen of a Grand Prix (race mode %u)", (unsigned)mode);
    }
    ctx_call_orig(c, g_native_addr[NAT___ct__7Pause2DFP7JKRHeap]);
    if (mode != MODE_GP) mst(c, M, info + 8, 4, mode);
    return ctx_gpr(c, 3);
}

/* GameAudioMain::setPause (0 resume, 1 pause, 2 leaving): the race's sound goes on behind
 * PauseManager's pause screen in a LAN race. */
uint64_t i_FUN_80115040(Ctx *c, uint8_t *M, ARGS) {
    UNUSED_ARGS;
    uint32_t lr = (uint32_t)RD(c, 0x1020, 4);
    if ((uint32_t)a1 <= 1 && lr_in(lr, PM_EXEC, 2372) && lan_race(c, M)) return 0;
    ctx_call_orig(c, g_native_addr[NAT_FUN_80115040]);
    return 0;
}
