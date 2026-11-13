/* Versus with CPUs (the − menu's GAME > VERSUS CPUS): a Versus race of 2 to 4 players' karts gets
 * computer karts up to g_vs_karts (4 to 8; 0 off, the game's own: one kart per player).
 *
 * Versus (gRaceInfo +8 race mode 3) is set up by SceneMenu::setRaceData with
 * RaceInfo::setRace(3, karts, players, consoles, status), its karts the players' karts. Its loop
 * after the players' karts fills every other kart (below gRaceInfo +0x1c) with a computer's, as
 * for a Grand Prix: two free cells each from getCharSel (never a player's team, never twice), the
 * heavier one's kart. The rest of the race already handles it, as the game's own attract demo (a
 * Versus of computer karts only) does:
 * - the starts: Versus has its own for 2-4 karts, more get the Grand Prix's (RaceMgr::getStartPoint);
 * - the end: with karts that are not players', the race ends when every player's has finished
 *   (RaceDirector::checkRaceEndForRace), the computers' places then given by their progress;
 * - the items: past 4 karts Versus takes the Grand Prix's table for its engine class
 *   (ItemShuffleMgr), the computers' karts the same table as the players'.
 * The hook is battle.c's i_FUN_8018c878 (setRace), which asks versus_karts for mode 3. A Wi-Fi
 * (LAN) race (net) stays the game's own on every console, and so does the attract demo (no players). */
#include "rt.h"

volatile uint32_t g_vs_karts;

int versus_karts(int karts, int players, bool net) {
    int want = (int)g_vs_karts;
    if (net || want < 4 || want > 8 || players < 2 || players > 4 || karts < 2 || karts > 4 || karts >= want) return 0;
    rt_log("versus cpus: %d players in %d karts, %d computer karts", players, karts, want - karts);
    return want;
}
