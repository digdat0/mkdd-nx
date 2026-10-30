/* charpack.c's select screen placement (the rest is in rt.h): a variant sits in any cell of the
 * grid (its own character's when there is room, see charpack.c layout), and plays as its base. Picks
 * are per team and driver, so a variant and its base (or two of its variants) can race together. */
#pragma once
#include "rt.h"

/* The select screen's cells: the game's 20 (cell / 2 its column, cell % 2 its row) and the third
 * row's 10 under them (native.c: CP_BASES + column), a page of CP_CELLS. */
#define CP_ROW 10
#define CP_CELLS (CP_BASES + CP_ROW)
/* The variant cell shows on the page shown now, -1 none (the game's own on the first page's 20, or
 * nothing there). */
int charpack_cell_variant(int cell);
/* The character cell stands for on this page: its variant's base, else the cell's own. */
int charpack_real_base(int cell);
/* Player p picks variant v in another character's cell (A, or the random pick landing there): the
 * select screen chooses its base (native.c SceneMenu::character), and the team's driver that choice
 * makes becomes v, picked in cell (v -1: the game's own, even on a page showing a variant there). */
void charpack_pending(int player, int variant, int cell);
/* Team t's driver k (0, 1): the game's chosen cell (its base's), -1 none; *variant its pick, -1 the
 * game's own. */
int charpack_slot(int t, int k, int *variant);
/* The cell team t's driver k shows in on this page when it was picked in another character's cell
 * (that page shows it there), else -1: native.c moves its highlight there. */
int charpack_slot_shown(int t, int k);
/* What cell c offers on this page (its variant, else the game's own) is picked already: cursors
 * skip it. A variant of a character taken, or that character itself, is still free. */
bool charpack_cell_taken(int cell);
/* Variant v is some team's driver's pick already. */
bool charpack_variant_taken(int variant);

/* The race's drivers (native.c, as each DriverModel's driver.bmd is read): kart's driver drv is
 * character b, as variant v (-1 the game's own). */
void charpack_race_driver(int kart, int drv, int b, int v);
/* The variant kart's driver of character b shows as in its HUD pictures (slot 0 / 1: the race
 * order's first or second head, when both its drivers are b; -1 unknown), -1 the game's own,
 * CP_RACE_NONE when no driver of kart is b. */
#define CP_RACE_NONE (-3)
int charpack_race_face(int kart, int slot, int b);
/* kart's driver in front now (native.c, each HUD frame; sure: one of the two is in the back seat) */
void charpack_race_front(int kart, int front, bool sure);
/* kart's driver drv starts a voice line (native.c, CharacterSoundMgr::setVoice): its character's
 * lines are its variant's (or the game's own) until another driver of that character speaks. */
void charpack_voice_speak(int kart, int drv);
/* Variant v's voice plays now (charvoice.c, as a channel starts). */
bool charpack_voice_on(int variant);
/* The variants whose voices may play now (the select screen's, the race's drivers'), at most max. */
unsigned charpack_voice_variants(int *out, unsigned max);
/* The HUD's own copy of a face or map icon (kind CP_HUD_FACE / CP_HUD_MAP) for kart's slot, size
 * bytes, now holding character b's: its index (the substitute id CP_HUD_SUBST + index), -1 none
 * (the pool is full). The renderer draws it as charpack_race_face says (charpack_art.c). */
#define CP_HUD_FACE 0
#define CP_HUD_MAP 1
#define CP_HUD_SUBST (GX_SUBST_CHARPACK + 3 * CP_BASES + CP_BASES * 40) /* after charpack_art.c's ids */
int charpack_hud_copy(int kind, int kart, int slot, int b, uint32_t size, uint32_t *addr);
/* the third row's faces (native.c: each column's own copy of its face texture), CP_ROW of them */
#define CP_ROW3_SUBST (CP_HUD_SUBST + 64)
bool charpack_hud_copy_info(int index, int *kind, int *kart, int *slot, int *b);
/* This frame base's block of the select screen's 3D models shows variant v (-1 the game's own),
 * native.c's choice for each block, made each frame after charpack_select_frame. */
void charpack_borrow(int base, int variant);
/* The variant whose model textures, name tag and voice (charvoice.c) stand in for base's now (select screen: the one
 * previewed or chosen; race: the one loaded), -1 the game's own. */
int charpack_model(int base);
/* n bytes of MEM1 above the game's 24 MiB kept for good (32-byte aligned), 0 when there is no room. */
uint32_t charpack_reserve(uint32_t n);
/* The character whose animations move variant v (its skeleton is that character's, it ships
 * animations of its own), -1 its base's. */
int charpack_anim(int variant);
/* The select screen back on its first page (the game's 20). */
void charpack_page_first(void);
/* Variant v's file in another driver folder of its pack ("cmn_Liz"), like charpack_file. */
uint32_t charpack_folder_file(int variant, const char *folder, const char *file, uint32_t *size);
uint32_t charpack_select_file(int variant, const char *file, uint32_t *size);
