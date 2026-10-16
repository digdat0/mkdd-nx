/* HD texture replacements: a Dolphin HD texture pack converted by tools/hdtex_pack.py (BC7, full
 * mipmap chains) in hd-textures.bin next to the .nro. Game textures are matched as Dolphin matches them
 * (format, size and XXH64 of the level-0 bytes); a loader thread reads replacements from the SD
 * card while the game texture stays on screen. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Read the file's index and start the loader thread. False (and logged) when the file is missing
 * or not a texture file of this version. */
bool hdtex_open(const char *path);
bool hdtex_available(void);
/* Replacement for a game texture, or -1. Hashes the texture: call once per new texture. */
int hdtex_find(uint32_t fmt, uint32_t w, uint32_t h, const uint8_t *raw, size_t len);
/* Level-0 size and mipmap levels of replacement idx. */
void hdtex_info(int idx, unsigned *w, unsigned *h, unsigned *levels);
/* Queue replacement idx for loading; tag comes back with it from hdtex_take. False if the queue is full. */
bool hdtex_request(int idx, uint64_t tag);
/* A finished load: its levels, largest first, each in 4x4 BC7 blocks (free() it). */
bool hdtex_take(int *idx, uint64_t *tag, uint8_t **data);

/* Loads finished so far, how many failed (no memory / read error), bytes read, requests waiting. */
void hdtex_stats(unsigned *loads, unsigned *fails, uint64_t *bytes, unsigned *queued);

uint64_t xxh64(const void *data, size_t len, uint64_t seed);
