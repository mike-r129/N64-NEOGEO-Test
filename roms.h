#ifndef ROMS_H
#define ROMS_H

#include <stdint.h>
#include <stdbool.h>

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

// Same fact for sprite (C-ROM) tiles: all index-0 pixels can never touch
// the screen (alpha-compare kills them in every palette). Learns lazily on
// first fetch; reset by crom_set_bank.
bool crom_tile_empty(int spritenum);

void srom_set_bank(int bank);  // 0 = fixed (BIOS), 1 = game

void pbrom_cache_init(void);
uint8_t* pbrom_linear(void);
uint8_t* pbrom_cache_lookup(uint32_t addr);

void rom_next_frame(void);

#endif
