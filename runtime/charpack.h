/* charpack.c's select screen placement (the rest is in rt.h): a variant sits in any cell of the
 * grid (its own character's when there is room, see charpack.c layout), and plays as its base. */
#pragma once
#include "rt.h"

/* The variant cell shows on the page shown now, -1 none (page 1, or nothing there). */
int charpack_cell_variant(int cell);
/* The character cell stands for on this page: its variant's base, else the cell's own. */
int charpack_real_base(int cell);
/* A player picks variant v in another character's cell (A, or the random pick landing there): the
 * select screen chooses its base (native.c SceneMenu::character), and base's pick becomes v, chosen
 * in cell, when that shows next frame (v -1: the game's own, even on a page showing a variant there). */
void charpack_pending(int base, int variant, int cell);
/* The random pick's version of base: the game's own (-1) or one of its variants, on any page, each
 * as likely; cell is where it is shown (its page's cell, base's for the game's own). */
int charpack_random(int base, int *cell);
/* The cell base's choice shows in now (chosen in another character's cell, which this page shows it
 * in), else -1: native.c moves its highlight there. */
int charpack_moved(int base);
/* This frame base's block of the select screen's 3D models shows variant v (a second preview of
 * another character, native.c). */
void charpack_borrow(int base, int variant);
/* The variant whose model textures and name tag stand in for base's now (select screen: the one
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
