#ifndef ROMS_H
#define ROMS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

extern uint8_t *P_ROM;
extern unsigned int rom_pc_idle_skip;

// Sound ROMs (consumed by the Z80/YM2610 subsystem, WS2/WS3).
extern uint8_t *M_ROM;            // Z80 program, resident
extern unsigned int m_rom_size;
extern unsigned int v_rom_size;   // YM2610 ADPCM source ROM (streamed from cart)
void vrom_read(uint32_t offset, uint8_t *buf, int len);

void rom_load(const char *dir);
void rom_load_prom(const char *dir);

uint8_t* crom_get_sprite(int spritenum);
uint8_t* srom_get_sprite(int spritenum);

// True if the fix-layer tile decodes to all index-0 (fully transparent)
// pixels — drawing it can never touch the screen, so callers skip it.
// Learns lazily on first sight of each tile; reset by srom_set_bank.
bool srom_tile_empty(int spritenum);

// Inline fast path for render_fix's per-cell test: answers from the learned
// bitmaps without a call; only a not-yet-learned or out-of-range tile falls
// back to srom_tile_empty (which learns it / applies its clamp). Same answer
// as srom_tile_empty for every input.
extern unsigned int srom_num_tiles;
extern uint8_t srom_known[], srom_empty[];
static inline bool srom_tile_empty_fast(int spritenum) {
	if ((unsigned)spritenum < srom_num_tiles) {
		int byte = spritenum >> 3, bit = 1 << (spritenum & 7);
		if (srom_known[byte] & bit)
			return (srom_empty[byte] & bit) != 0;
	}
	return srom_tile_empty(spritenum);
}

#ifndef N64
// Same fact for sprite (C-ROM) tiles on the PC build: all index-0 pixels can
// never touch the screen. Learns lazily on first fetch; reset by
// crom_set_bank. (N64 uses the direct table below.)
bool crom_tile_empty(int spritenum);
#endif

// Fused empty-test + lookup through the per-tile C-ROM direct table, inlined,
// reading a per-render context (crom_resolve_ctx) that the caller keeps in
// registers across the sprite walk: NULL if the tile is all-transparent
// (skip it), else the cached pixel pointer (valid for the rest of the frame).
// One sparse table read on the hot path, plus the LRU tick store; misses go
// out of line.
typedef struct {
	const uint16_t *dt;     // CROM direct table (0 unknown, 1 empty, 2 solid, >=3 slot+3)
	uint8_t *slot_tick;     // per-slot LRU tick (uncached alias on N64)
	uint8_t *sprites;       // pixel slots, 128 bytes each
	unsigned mask, ntiles;
	uint8_t tick;           // (uint8_t)cur_tick
} CromResolveCtx;
void crom_resolve_ctx(CromResolveCtx *c);
uint8_t *crom_resolve_slowpath(unsigned sn);
static inline uint8_t *crom_resolve_fast(const CromResolveCtx *c, int spritenum) {
	unsigned sn = (unsigned)spritenum & c->mask;
	if (sn >= c->ntiles) sn = c->ntiles - 1;
	uint32_t e = c->dt[sn];
	if (e >= 3) {
		e -= 3;
		c->slot_tick[e] = c->tick;
		return c->sprites + (e << 7);
	}
	if (e == 1)
		return NULL;
	return crom_resolve_slowpath(sn);
}

void srom_set_bank(int bank);  // 0 = fixed (BIOS), 1 = game

void pbrom_cache_init(void);
uint8_t* pbrom_linear(void);
uint8_t* pbrom_cache_lookup(uint32_t addr);

void rom_next_frame(void);

#endif
