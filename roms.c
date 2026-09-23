#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "platform.h"
#include "hw.h"
#include "roms.h"
#include "sprite_cache.h"

#ifdef N64
#include <malloc.h>
#define ALIGN_256K __attribute__((aligned(256*1024)))
#else
#define memalign(a, b) malloc(b)
#define ALIGN_256K
#endif

uint8_t *P_ROM;
#define P_ROM_SIZE (1024*1024)
uint8_t *PB_ROM;
#define PB_ROM_CACHE_SIZE  (1024*1024)

// M-ROM (Z80 sound program) is small (128KB for samsho2) and loaded fully
// resident in RDRAM. V-ROM (YM2610 ADPCM samples, up to 7MB) cannot fit in
// RDRAM and is streamed from cart on demand (see vrom_read). Consumed by the
// sound subsystem (WS2/WS3).
uint8_t *M_ROM;
unsigned int m_rom_size;
unsigned int v_rom_size;

// Address to trigger idle-skipping
unsigned int rom_pc_idle_skip = 0;
extern uint32_t profile_dma_load;

static SpriteCache srom_cache;
static SpriteCache crom_cache;

static const char* srom_fn[2] = {NULL, NULL};
static const char* crom_fn[1] = {NULL};
static int srom_bank = -1;

#ifdef N64
static int crom_file = -1;
static int srom_file = -1;
static int pbrom_file = -1;
static int vrom_file = -1;
#else
static FILE *crom_file = NULL;
static FILE *srom_file = NULL;
static FILE *pbrom_file = NULL;
static FILE *vrom_file = NULL;
#endif

static unsigned int crom_mask;
static unsigned int crom_num_tiles;
static unsigned int srom_num_tiles;

static void rom_cache_init(void) {
	sprite_cache_init(&srom_cache, 4*8, 256);
	sprite_cache_init(&crom_cache, 8*16, 1280);
}

// fread whose short-read is tolerated (EOF-bounded lookahead, cache fills
// from known-good files). Exists to satisfy -Werror=unused-result on the
// PC build without changing behavior.
static inline void fread_ok(void *dst, size_t sz, FILE *f) {
	size_t got = fread(dst, 1, sz, f);
	(void)got;
}

// Empty-tile knowledge for the fix layer. Most of the 40x28 fix map is
// "blank" cells whose tile decodes to all index-0 pixels (color 0 has alpha
// forced to 0 in every palette, so such a draw can never touch the screen).
// The map is full of nonzero tile codes though, so the `if (v)` check in
// render_fix passes for every cell and we used to issue ~1120 draws/frame,
// ~15% of the frame budget, almost all fully transparent. Tile pixel data is
// ROM (immutable per SROM bank), so emptiness is a stable per-tile fact:
// learn it on first sight, then skip empty tiles forever. Pixel-identical by
// construction. Reset on srom_set_bank (tile numbers change meaning).
#define SROM_MAX_TILES 8192
static uint8_t srom_known[SROM_MAX_TILES/8];
static uint8_t srom_empty[SROM_MAX_TILES/8];

bool srom_tile_empty(int spritenum) {
	if ((unsigned)spritenum >= srom_num_tiles) spritenum = srom_num_tiles-1;
	int byte = spritenum >> 3, bit = 1 << (spritenum & 7);
	if (!(srom_known[byte] & bit)) {
		const uint8_t *pix = srom_get_sprite(spritenum);
		uint32_t acc = 0;
		for (int i=0; i<4*8; i+=4)
			acc |= *(const uint32_t*)(pix + i);
		srom_known[byte] |= bit;
		if (acc == 0) srom_empty[byte] |= bit;
	}
	return (srom_empty[byte] & bit) != 0;
}

uint8_t* srom_get_sprite(int spritenum) {
	if (spritenum >= srom_num_tiles) spritenum = srom_num_tiles-1;
	uint8_t *pix = sprite_cache_lookup(&srom_cache, spritenum);
	if (pix) return pix;

	pix = sprite_cache_insert(&srom_cache, spritenum);
	assertf(pix, "SROM cache is full");

	#ifdef N64
	profile_dma_load -= TICKS_READ();
	dfs_seek(srom_file, spritenum*4*8, SEEK_SET);
	dfs_read(pix, 1, 4*8, srom_file);
	data_cache_hit_writeback_invalidate(pix, 4*8);    // FIXME: should not be required
	profile_dma_load += TICKS_READ();
	#else
	fseek(srom_file, spritenum*4*8, SEEK_SET);
	fread_ok(pix, 4*8, srom_file);
	#endif

	return pix;
}

// Empty-tile knowledge for the sprite (C) ROM — the render_sprites analogue
// of srom_tile_empty above. An all-index-0 tile is fully transparent in
// every palette (pal_convert forces color 0 alpha to 0 and the sprite path
// draws with alpha-compare on), and tile pixel data is immutable ROM, so
// emptiness is a stable per-tile fact: learn it on the first fetch, then
// skip the cache lookup AND the RSP/RDP draw forever. samsho2's dense
// sprite-background stages issue 8-10k tiles/frame at up to ~100% of the
// frame budget; [PERF2] empty= reports how many draws this removes.
#define CROM_MAX_TILES (1u << 18)   // 32MB of C-ROM; samsho2 uses 2^17
static uint8_t crom_known[CROM_MAX_TILES/8];
static uint8_t crom_emptyb[CROM_MAX_TILES/8];

bool crom_tile_empty(int spritenum) {
	unsigned sn = (unsigned)spritenum & crom_mask;
	if (sn >= crom_num_tiles) sn = crom_num_tiles-1;
	int byte = sn >> 3, bit = 1 << (sn & 7);
	if (!(crom_known[byte] & bit)) {
		const uint8_t *pix = crom_get_sprite((int)sn);
		uint32_t acc = 0;
		for (int i=0; i<8*16; i+=4)
			acc |= *(const uint32_t*)(pix + i);
		crom_known[byte] |= bit;
		if (acc == 0) crom_emptyb[byte] |= bit;
	}
	return (crom_emptyb[byte] & bit) != 0;
}

uint8_t* crom_get_sprite(int spritenum) {
	spritenum &= crom_mask;
	if (spritenum >= crom_num_tiles) spritenum = crom_num_tiles-1;

	uint8_t *pix = sprite_cache_lookup(&crom_cache, spritenum);
	if (pix) return pix;

	pix = sprite_cache_insert(&crom_cache, spritenum);
	assertf(pix, "CROM cache is full");

	#ifdef N64
	profile_dma_load -= TICKS_READ();
	#ifdef MVS64_PERFCOUNT
	{
		// PLAN-DRAW-RDP Phase 0: per-frame miss count + DMA ticks
		// ([PERF3], emu.c) — sizes the unique-tile floor Phase 1
		// compresses the cache bucket toward.
		extern uint32_t perf_dr_miss, perf_dr_missticks;
		perf_dr_miss++;
		perf_dr_missticks -= TICKS_READ();
	}
	#endif
	dfs_seek(crom_file, spritenum*8*16, SEEK_SET);
	dfs_read(pix, 1, 8*16, crom_file);
	data_cache_hit_writeback_invalidate(pix, 8*16);  // FIXME: should not be required
	#ifdef MVS64_PERFCOUNT
	{
		extern uint32_t perf_dr_missticks;
		perf_dr_missticks += TICKS_READ();
	}
	#endif
	profile_dma_load += TICKS_READ();
	#else
	fseek(crom_file, spritenum*8*16, SEEK_SET);
	fread_ok(pix, 8*16, crom_file);
	#endif

	return pix;
}

// CROM direct table (CDT): one u16 per tile fusing the empty-tile fact and
// the cache-resident pointer, so the per-record hot path is a single
// sparse read instead of known-bitmap + empty-bitmap + hash-bucket probe
// (+ a dirtying bucket tick write) — three independent dcache lines per
// drawn record. Encoding:
//   0 = unknown (never fetched)       1 = known empty (never drawn)
//   2 = known non-empty, not resident >=3 = resident at pixel slot e-3
// Invariant: e >= 3 => the tile is resident at that slot. It is set only
// right after crom_get_sprite returned the slot, and sprite_cache_pop
// demotes it to 2 in the same step that frees the slot (crom_cache.dt
// hook); crom_set_bank clears the table with the cache. Current-tick
// entries are never evicted (sprite_cache.c), so a resolved pointer is
// valid for the rest of the frame — same lifetime as crom_get_sprite's.
#define CDT_UNKNOWN 0
#define CDT_EMPTY   1
#define CDT_SOLID   2
#define CDT_SLOT0   3
static uint16_t *crom_dt;

static uint8_t *crom_resolve_miss(unsigned sn, uint32_t e) {
	uint8_t *pix = crom_get_sprite((int)sn);
	if (e == CDT_UNKNOWN) {
		uint32_t acc = 0;
		for (int i=0; i<8*16; i+=4)
			acc |= *(const uint32_t*)(pix + i);
		if (acc == 0) {
			crom_dt[sn] = CDT_EMPTY;
			return NULL;
		}
	}
	crom_dt[sn] = CDT_SLOT0 + sprite_cache_slot(&crom_cache, pix);
	return pix;
}

uint8_t* crom_resolve(int spritenum) {
	unsigned sn = (unsigned)spritenum & crom_mask;
	if (sn >= crom_num_tiles) sn = crom_num_tiles-1;
	uint32_t e = crom_dt[sn];
	if (e >= CDT_SLOT0) {
		e -= CDT_SLOT0;
		crom_cache.slot_tick[e] = crom_cache.cur_tick;
		return crom_cache.sprites + (e << 7);
	}
	if (e == CDT_EMPTY)
		return NULL;
	return crom_resolve_miss(sn, e);
}

void srom_set_bank(int bank) {
	assert(bank == 0 || bank == 1);
	unsigned len;

	if (srom_bank != bank) {
		srom_bank = bank;

		#ifdef N64
		if (srom_file != -1) dfs_close(srom_file);
		srom_file = dfs_open(srom_fn[srom_bank]);
		assertf(srom_file >= 0, "cannot open: %s", srom_fn[srom_bank]);
		len = dfs_size(srom_file);
		#else
		if (srom_file) fclose(srom_file);
		srom_file = fopen(srom_fn[bank], "rb");
		assertf(srom_file, "cannot open: %s", srom_fn[bank]);
		fseek(srom_file, 0, SEEK_END);
		len = ftell(srom_file);
		#endif

		sprite_cache_reset(&srom_cache);
		srom_num_tiles = len / (4*8);
		assertf(srom_num_tiles <= SROM_MAX_TILES, "SROM too large: %d tiles", srom_num_tiles);

		// Tile numbers refer to the new bank now: relearn emptiness.
		memset(srom_known, 0, sizeof(srom_known));
		memset(srom_empty, 0, sizeof(srom_empty));
	}
}

void crom_set_bank(int bank) {
	assert(bank == 0);
	unsigned len;

	#ifdef N64
	if (crom_file != -1) dfs_close(crom_file);
	crom_file = dfs_open(crom_fn[bank]);
	assertf(crom_file >= 0, "cannot open: %s", crom_fn[bank]);
	len = dfs_size(crom_file);
	#else
	if (crom_file) fclose(crom_file);
	crom_file = fopen(crom_fn[bank], "rb");
	assertf(crom_file, "cannot open: %s", crom_fn[bank]);
	fseek(crom_file, 0, SEEK_END);
	len = ftell(crom_file);
	#endif

	sprite_cache_reset(&crom_cache);
	crom_num_tiles = len / (8*16);
	assertf(crom_num_tiles <= CROM_MAX_TILES, "CROM too large: %d tiles",
		crom_num_tiles);

	// Tile numbers refer to the new bank now: relearn emptiness.
	memset(crom_known, 0, sizeof(crom_known));
	memset(crom_emptyb, 0, sizeof(crom_emptyb));
	free(crom_dt);
	crom_dt = calloc(crom_num_tiles, sizeof(uint16_t));
	assertf(crom_dt, "CROM direct table: out of memory (%u tiles)", crom_num_tiles);
	crom_cache.dt = crom_dt;

	// Calculate mask based on next power of two
	len /= 8*16;
	len -= 1;
	len |= len >> 1;
	len |= len >> 2;
	len |= len >> 4;
	len |= len >> 8;
	len |= len >> 16;
	crom_mask = len;
}

// PBROM handling.
//
// PBROM is a made-up name for the banked part of the PROM, which is
// mapped at 0x2xxxxx in the 68K address space. The ROM is saved in the
// B.ROM file, separated from P.ROM which contains only the first Mb
// (mapped at 0x0xxxxx).
//
// mvs64 allocates 1 Mb of total RDRAM to handle PBROM (PB_ROM array).
// There are two possible options, depending on the game: if the PBROM
// file is 1 Mb or less, we can simply load it in RDRAM in full. We call
// this "linear mapping of PBROM", and it's the easiest and fastest option.
//
// If the file is larger than the PBROM area (it can be up to 4Mb),
// then we switch to a cache-based implementation, where chunks of the
// B.ROM file are loaded and used on demand. The cache is organized
// around small chunks of data ("banks"). Banks are very small (eg: 64
// bytes) because games do many random accesses in the PBROM area so it
// doesn't make sense to waste time loading large banks when just a few
// bytes are accessed.
//
// Banks are kept in a hash table, that is sorted using the remaining bits
// of the bank address (PBROM_LOOKUP_BITS). The hashtable itself is stored
// in the PB_ROM array, so to reuse the same buffer of memory.
//
// Notice that in case of linear mapping, we simply use TLB to map the PBROM
// to the 68K; instead in case of cache, we cannot use TLB because we would
// be forced to use 4K banks, which would be too big.
#define PBROM_BANK_BITS    6
#define PBROM_LOOKUP_BITS  12
#define PBROM_BANK_MASK    ((1<<PBROM_BANK_BITS)-1)

// Single entry of the PBROM cache. We store two different memory banks
// for each entry, so to do very simple hash function conflicts. This is
// very important because there will always be conflicts and we absolutely
// need to avoid race loops where two entries push each other off the cache
// multiple times per frame.
//
// Notice also that we load 2 bytes more for each bank, to allow for
// a 32-bit memory read that crosses the bank boundary.
//
// Memory areas are kept aligned to 8 bytes to allow for direct DMA.
typedef struct {
	uint8_t mem1[(1<<PBROM_BANK_BITS)+2] __attribute__((aligned(8)));
	uint32_t bank1;
 	uint8_t mem2[(1<<PBROM_BANK_BITS)+2] __attribute__((aligned(8)));
 	uint32_t bank2;
} PBROMCacheEntry;

// PBROM cache is limited to 1Mb to make it work on N64 without expansion pack
_Static_assert(sizeof(PBROMCacheEntry)*(1<<PBROM_LOOKUP_BITS) <= PB_ROM_CACHE_SIZE, "PBROM cache too big");

static bool pbrom_is_linear = false;
uint32_t pbrom_last_bank = 0xFFFFFFFF;
uint8_t *pbrom_last_mem = NULL;

void pbrom_init(const char *fn) {
	unsigned len;
	#ifdef N64
	if (pbrom_file >= 0) dfs_close(pbrom_file);
	pbrom_file = dfs_open(fn);
	if (pbrom_file < 0) {
		debugf("[PBROM] no PBROM detected\n");
		pbrom_is_linear = true;
		return;
	}
	len = dfs_size(pbrom_file);
	#else
	if (pbrom_file) fclose(pbrom_file);
	pbrom_file = fopen(fn, "rb");
	if (pbrom_file == NULL) {
		pbrom_is_linear = true;
		return;
	}
	fseek(pbrom_file, 0, SEEK_END);
	len = ftell(pbrom_file);
	fseek(pbrom_file, 0, SEEK_SET);
	#endif

	#ifdef N64
	int mem_avail = get_memory_size();
	#else
	int mem_avail = 1024*1024; // on PC, simulate a 4Mb RDRAM so that we test the cache
	#endif
	int pbrom_avail = mem_avail - 3*1024*1024;

	if (len > pbrom_avail) {
		// The available memory isn't sufficient for PBROM. Switch to cache mode.
		debugf("[PBROM] swapping activated, performance will be impacted (%s, req:%d avail:%d)\n", fn, len, pbrom_avail);
		pbrom_is_linear = false;
		pbrom_cache_init();
		return;
	}

	// Allocate a buffer to hold the PBROM file with 1 MiB alignment so that
	// can later TLB-map it into the 1 MiB area in the m68k space.
	PB_ROM = memalign(1024*1024, len);
	assertf(PB_ROM, "cannot allocate PBROM buffer");

	// We have enough RDRAM to fully load the PBROM file into RDRAM
	// (aka linear mapping)
	#ifdef N64
	dfs_read(PB_ROM, 1, len, pbrom_file);
	dfs_close(pbrom_file); pbrom_file = -1;
	#else
	fread_ok(PB_ROM, len, pbrom_file);
	fclose(pbrom_file); pbrom_file = NULL;
	#endif
	pbrom_is_linear = true;
	debugf("[PBROM] using linear mode\n");
}

static bool fastrand_bool(void) {
	#ifdef N64
	return C0_COUNT() & 2;
	#else
	return rand() & 1;
	#endif
}

// Return the PBROM linear mapping area, or NULL in case PBROM is banked.
uint8_t* pbrom_linear(void) {
	return pbrom_is_linear ? PB_ROM : NULL;
}

void pbrom_cache_init(void) {
	// Initialize pbrom cache
	PB_ROM = malloc(PB_ROM_CACHE_SIZE);
	assertf(PB_ROM, "cannot allocate PBROM cache");

	PBROMCacheEntry *cache = (PBROMCacheEntry *)PB_ROM;
	for (int i=0; i < 1<<PBROM_LOOKUP_BITS; i++) {
		cache[i].bank1 = 0xFFFFFFFF;
		cache[i].bank2 = 0xFFFFFFFF;
	}
}

// Lookup PBROM cache, loading data on demand.
uint8_t *pbrom_cache_lookup(uint32_t address) {
	assert(!pbrom_is_linear);
	PBROMCacheEntry *prom_cache = (PBROMCacheEntry*)PB_ROM;

	// See if this is the same bank that was last accessed.
	// This is a simple speedup for the common case of multiple
	// subsequent accesses to nearby addresses.
	uint32_t bank = address >> PBROM_BANK_BITS;
	if (bank == pbrom_last_bank) return pbrom_last_mem + (address & PBROM_BANK_MASK);

	// Calculate hash function for the requested address. Experimentally
	// this works reasonably well with the kind of addresses that we see
	// in games, and is fast enough.
	uint32_t entry = bank;
	entry ^= entry >> 16;
  	entry *= 2654435761;
	entry ^= entry >> 16;
	entry *= 2654435761;
	entry &= ((1<<PBROM_LOOKUP_BITS)-1);

	// Lookup the cache to check whether this address have been already
	// loaded.
	PBROMCacheEntry *c = &prom_cache[entry];
	if (c->bank1 == bank) return c->mem1 + (address & PBROM_BANK_MASK);
	if (c->bank2 == bank) return c->mem2 + (address & PBROM_BANK_MASK);

	// Populate the cache, loading from N64 ROM
	uint32_t base = bank << PBROM_BANK_BITS;
	debugf("[PBROM] loading %06x (bank:%x entry:%x)\n", (unsigned)base, (unsigned)bank, (unsigned)entry);
	uint8_t *mem;
	if (fastrand_bool()) { c->bank1 = bank; mem = c->mem1; }
	else                 { c->bank2 = bank; mem = c->mem2; }

	#ifdef N64
	dfs_seek(pbrom_file, base, SEEK_SET);
	dfs_read(mem, 1, (1<<PBROM_BANK_BITS)+2, pbrom_file);
	#else
	fseek(pbrom_file, base, SEEK_SET);
	fread_ok(mem, (1<<PBROM_BANK_BITS)+2, pbrom_file);
	#endif

	pbrom_last_mem = mem;
	pbrom_last_bank = bank;

	return mem + (address & PBROM_BANK_MASK);
}

static void rom(const char *dir, const char* name, int off, int sz, uint8_t *buf, int bufsize, bool bswap) {
	char fullname[1024];
	strlcpy(fullname, dir, sizeof(fullname));
	strlcat(fullname, name, sizeof(fullname));

	FILE *f = fopen(fullname, "rb");
	assertf(f, "file not found: %s", fullname);
	if (!sz) {
		fseek(f, 0, SEEK_END);
		sz = ftell(f);
	}
	assertf(sz <= bufsize, "ROM too big: %s", name);
	fseek(f, off, SEEK_SET);
	int read = fread(buf, 1, sz, f);
	fclose(f);
	assertf(read == sz, "rom:%s off:%d sz:%d read:%d", fullname, off, sz, read);

	if (bswap) {
		for (int i=0;i<sz;i+=2) {
			uint8_t v = buf[i];
			buf[i] = buf[i+1];
			buf[i+1] = v;
		}
	}
}

#define strcatalloc(a, b) ({ char v[strlen(a)+strlen(b)+1]; strcpy(v, a); strcat(v, b); strdup(v); })

static uint32_t ini_get_integer(const char *ini, const char *key, bool *ok) {
	int klen = strlen(key); char *kv;
	if ((kv = strstr(ini, key)) && kv[klen] == '=') {
		kv += klen+1;
		if (ok) *ok = true;
		if (kv[0] == '0' && kv[1] == 'x')
			return strtoul(kv, NULL, 16);
		else
			return strtoul(kv, NULL, 10);
	}
	if (ok) *ok=false;
	return 0;
}

void rom_next_frame(void) {
	sprite_cache_tick(&srom_cache);
	sprite_cache_tick(&crom_cache);
}

// M-ROM (Z80 sound program): small, loaded fully resident in RDRAM.
// Tolerant of absence (older builds / soundless games): just disables sound.
void rom_load_mrom(const char *dir) {
	char fullname[1024];
	strlcpy(fullname, dir, sizeof(fullname));
	strlcat(fullname, "m.rom", sizeof(fullname));

	FILE *f = fopen(fullname, "rb");
	if (!f) {
		debugf("[ROM] no m.rom found (sound disabled)\n");
		M_ROM = NULL; m_rom_size = 0;
		return;
	}
	fseek(f, 0, SEEK_END);
	m_rom_size = ftell(f);
	fseek(f, 0, SEEK_SET);
	M_ROM = malloc(m_rom_size);
	assertf(M_ROM, "cannot allocate M_ROM (%u bytes)", m_rom_size);
	int rd = fread(M_ROM, 1, m_rom_size, f);
	fclose(f);
	assertf(rd == (int)m_rom_size, "short read on m.rom: %d/%u", rd, m_rom_size);
	debugf("[ROM] loaded m.rom: %u bytes\n", m_rom_size);
}

// V-ROM (YM2610 ADPCM-A/B samples, up to 7MB): too big for RDRAM, streamed
// from cart on demand. Mirrors the crom streaming idiom (dfs DMA + cache flush).
void vrom_open(const char *fn) {
	#ifdef N64
	if (vrom_file >= 0) dfs_close(vrom_file);
	vrom_file = dfs_open(fn);
	if (vrom_file < 0) { debugf("[ROM] no v.rom found\n"); v_rom_size = 0; return; }
	v_rom_size = dfs_size(vrom_file);
	#else
	if (vrom_file) fclose(vrom_file);
	vrom_file = fopen(fn, "rb");
	if (!vrom_file) { debugf("[ROM] no v.rom found\n"); v_rom_size = 0; return; }
	fseek(vrom_file, 0, SEEK_END);
	v_rom_size = ftell(vrom_file);
	fseek(vrom_file, 0, SEEK_SET);
	#endif
	debugf("[ROM] opened v.rom: %u bytes (streamed)\n", v_rom_size);
}

// Read `len` bytes of ADPCM source data from V-ROM at byte `offset` into `buf`.
// Used by the YM2610 ADPCM engine (WS3). Zero-fills if V-ROM is absent.
void vrom_read(uint32_t offset, uint8_t *buf, int len) {
	#ifdef N64
	if (vrom_file < 0) { memset(buf, 0, len); return; }
	profile_dma_load -= TICKS_READ();
	dfs_seek(vrom_file, offset, SEEK_SET);
	dfs_read(buf, 1, len, vrom_file);
	data_cache_hit_writeback_invalidate(buf, len);  // FIXME: should not be required
	profile_dma_load += TICKS_READ();
	#else
	if (!vrom_file) { memset(buf, 0, len); return; }
	fseek(vrom_file, offset, SEEK_SET);
	fread_ok(buf, len, vrom_file);
	#endif
}

void rom_load_prom(const char *dir) {
	if (!P_ROM) P_ROM = memalign(256*1024, 1024*1024);
	assertf(P_ROM, "cannot allocate P_ROM buffer");
	rom(dir, "p.rom", 0, 0, P_ROM, P_ROM_SIZE, false);
}

void rom_load(const char *dir) {
	rom_load_prom(dir);
	rom(dir, "p.bios", 0, 0, BIOS, sizeof(BIOS), false);
	rom_load_mrom(dir);  // Z80 program (resident); loaded while dir is still "rom:/" on N64

	char ini[1024];
	strcpy(ini, dir);
	strcat(ini, "game.ini");
	FILE *f = fopen(ini, "rb");
	if (f) {
		size_t n = fread(ini, 1, sizeof(ini) - 1, f);
		ini[n] = 0;   // also fixes the unterminated-buffer parse
		fclose(f);

		bool ok;

		rom_pc_idle_skip = ini_get_integer(ini, "idle_skip", &ok);
		if (ok) debugf("[ROM] configure idle_skip: %x\n", rom_pc_idle_skip);
	}
	rom_pc_idle_skip = 0;

	#ifdef N64
	dir = "";
	#endif

	srom_fn[0] = strcatalloc(dir, "s.bios");
	srom_fn[1] = strcatalloc(dir, "s.rom");
	crom_fn[0] = strcatalloc(dir, "c.rom");

	rom_cache_init();
	srom_set_bank(0);  // Set SFIX as current
	crom_set_bank(0);
	pbrom_init(strcatalloc(dir, "b.rom"));
	vrom_open(strcatalloc(dir, "v.rom"));  // ADPCM samples (streamed)
}
