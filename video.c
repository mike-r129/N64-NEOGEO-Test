#include <stdint.h>
#include <assert.h>
#include <stdlib.h>
#include <memory.h>
#include "video.h"
#include "roms.h"
#include "hw.h"
#include "platform.h"

// Magic table to calculate pixel-perfect vertical shrinking.
// This table can be thought of a condensed version of the original
// NeoGeo L0 ROM, but we just need 16 bytes to achieve the same results.
// The table is then duplicated (and mirrored) to 32 bytes to simplify
// lookup for tiles 16-31 (where the L0 ROM is read backwards).
// To see how it's calculated, see l0.py.
static const uint8_t VSHRINK_MAGIC[32] = {
	1, 9, 5, 13, 3, 11, 7, 15, 0, 8, 4, 12, 2, 10, 6, 14,
	14, 6, 10, 2, 12, 4, 8, 0, 15, 7, 11, 3, 13, 5, 9, 1
};


// Given a vshrink code and the tile number, compute the tile
// height in pixels. For instance:
// vshrink_tile_height(0xBC, 4) == 12 means that when using
// vshrink code 0xBC, the fifth tile of a sprite will be exactly
// 12 pixel tall (shrunk from 16).
static inline int vshrink_tile_height(int vshrink, int num_tile) {
	vshrink += 1;
	return vshrink/16 + (vshrink%16 > VSHRINK_MAGIC[num_tile]);
}

// Given a tile of the specified height and y in [0,15],
// return True if the given line is drawn, or False if
// it should be skipped.
static inline bool vshrink_line_drawn(int height, int y) {
	return height > VSHRINK_MAGIC[y];
}

static uint16_t color_convert(uint16_t val) {
	uint16_t c16 = 0;

	c16 |= ((val & 0x0F00) << 4) | ((val & 0x4000) >> 3);
	c16 |= ((val & 0x00F0) << 3) | ((val & 0x2000) >> 7);
	c16 |= ((val & 0x000F) << 2) | ((val & 0x1000) >> 11);

	return c16;
}

static uint16_t PALETTE_RAM_EMU[4*1024];

#if defined(N64) && defined(MVS64_PERFCOUNT)
// DRAW-1 fine split (diagnostic builds): where does the draw bucket go?
// Ticks: render_begin / sprite walk / fix layer, and inside the sprite walk
// the sprite-cache lookups vs the rspq command issue. Counts: tiles, fix
// cells. Printed as [PERF2] in emu.c, reset per frame.
uint32_t perf_dr_begin, perf_dr_sprites, perf_dr_fix;
uint32_t perf_dr_cache, perf_dr_rspq;
uint32_t perf_dr_tiles, perf_dr_cells;
uint32_t perf_dr_empty;   /* sprite tiles skipped as known-empty */
uint32_t perf_walk_spr;   /* sprites that reach the tile loop (past all culls) */
uint32_t perf_walk_iter;  /* tile-loop iterations, visible or not */
uint32_t perf_dr_miss;    /* sprite-cache misses (PI DMA loads, [PERF3]) */
uint32_t perf_dr_missticks; /* ticks spent in the miss/DMA path */
#define DRAW_PERF 1
#endif
#if defined(N64) && (defined(MVS64_PERFCOUNT) || defined(MVS64_PERFOSD))
// Coarse per-frame draw split, cheap enough for the PERFOSD hardware overlay
// (one TICKS pair per section, per walk and per C-ROM miss; the per-record
// DRAW_PERF timers stay PERFCOUNT-only). PERFOSD consumes and zeroes these
// in plat_perf_frame; PERFCOUNT prints and zeroes them in [PERF2]/[PERF3].
// Inside the sprite pass, per drawn record: C-ROM lookup ticks (Q, incl.
// misses), RSP command issue ticks (E, incl. flushes and buffer switches)
// and the drawn-tile count (N); the walk itself is R minus Q and E.
#ifndef MVS64_PERFCOUNT
uint32_t perf_dr_begin, perf_dr_sprites, perf_dr_fix, perf_dr_missticks;
uint32_t perf_dr_cache, perf_dr_rspq, perf_dr_tiles;
#endif
// Drawn tiles by RDP path (the ucode modal test, rsp_video.S): G = COPY
// mode (full 16x16, no flip, no x-clip), H = would be COPY but flipped.
uint32_t perf_dr_copyt, perf_dr_flipt;
#define DRAW_PERF_COARSE 1
#endif

// --- sprite walk -------------------------------------------------------------
// The SCB walk turns each visible tile into a record and draws it on the
// spot. A record is two words, exactly what the draw needs:
//   w0 = tnum[0..19] | palnum[20..27] | flipx[28] | flipy[29]
//   w1 = sx[0..11] | ssy[12..23] | (sw-1)[24..27] | (ssh-1)[28..31]
// Positions keep only the low 12 bits, which is lossless: both draw paths
// reduce positions mod 512 (PC) or to a 12-bit signed field (RSP), and
// sx/ssy never carry information above that. On N64, w0/w1 go to the RSP
// as the 2-word cmd_sprite_draw2 almost unchanged.

// ~4x the in-fight maximum (~1000 drawn tiles measured in Round 4). Excess
// tiles are walked correctly but dropped (logged).
#define SPRWALK_MAX_RECS  4096
static int sprwalk_overflow;   // records dropped this frame (diagnostic)

#ifdef N64
#include "video_n64.c"
#else
#include "video_cpu.c"
#endif

// Fix layer. The per-cell empty test is inlined (srom_tile_empty_fast) with a
// 1-entry memo of the last blank tile code: the map repeats the same blank
// codes, and ~950 of ~1120 cells are blank in fights. The memo is per call:
// tile numbers only change meaning on srom_set_bank, which the 68k does
// between renders.
static void render_fix(void) {
	uint16_t *fix = VIDEO_RAM + 0x7000;
	int last_blank = -1;

	render_begin_fix();

	for (int i=0;i<40;i++) {
		fix += 2; // skip two lines
		for (int j=0;j<28;j++) {
			uint16_t v = *fix++;
			// Skip tiles known to decode to all-transparent pixels: the map
			// is full of nonzero "blank" codes, so without this we issue
			// ~1120 draws/frame that can never touch the screen (see
			// srom_tile_empty).
			if (!v) continue;
			int t = v & 0xFFF;
			if (t == last_blank) continue;
			if (srom_tile_empty_fast(t)) { last_blank = t; continue; }
			draw_sprite_fix(t, (v >> 12) & 0xF, i*8, j*8);
		}
		fix += 2;
	}

	render_end_fix();
}


// Draw one record. Forced inline: as a call (GCC kept it out of line) every
// drawn tile paid ~35 instructions of spills/reloads and prologue, and
// re-read the C-ROM context from memory.
static inline __attribute__((always_inline))
void sprite_consume_one(const CromResolveCtx *cx, uint32_t w0, uint32_t w1) {
	uint32_t tnum = w0 & 0xFFFFF;
#ifdef N64
	// Fused empty-test + resolve through the C-ROM direct table (roms.c):
	// one sparse read per record, NULL for an all-transparent tile.
#ifdef DRAW_PERF_COARSE
	uint32_t _c0 = TICKS_READ();
#endif
	uint8_t *src = crom_resolve_fast(cx, tnum);
#ifdef DRAW_PERF_COARSE
	perf_dr_cache += TICKS_DISTANCE(_c0, TICKS_READ());
#endif
	if (!src) {
#ifdef DRAW_PERF
		perf_dr_empty++;
#endif
		return;
	}
#ifdef DRAW_PERF_COARSE
	perf_dr_tiles++;
	{   // same predicate as the ucode's modal test (cmd_sprite_draw)
		int x = ((int32_t)(w1 << 20)) >> 20, y = ((int32_t)(w1 << 8)) >> 20;
		if (x >= 512-16) x -= 512;
		if (y >= 512-16) y -= 512;
		if ((w1 >> 24) == 0xFF && x >= 0 && x <= 304 && y >= -15 && y <= 223) {
			if (w0 & (3u << 28)) perf_dr_flipt++;
			else perf_dr_copyt++;
		}
	}
	uint32_t _r0 = TICKS_READ();
#endif
	rsp_sprite_draw2((uint32_t)(src - cx->sprites) >> 7, w0, w1);
#ifdef DRAW_PERF_COARSE
	perf_dr_rspq += TICKS_DISTANCE(_r0, TICKS_READ());
#endif
#else
	// PC: skip tiles known to decode to all-transparent pixels (learned on
	// first fetch; index-0 pixels never pass the alpha compare), then draw
	// on the CPU.
	(void)cx;
	if (crom_tile_empty(tnum))
		return;
	draw_sprite(tnum, (w0 >> 20) & 0xFF,
	            w1 & 0xFFF, (w1 >> 12) & 0xFFF,
	            ((w1 >> 24) & 0xF) + 1, ((w1 >> 28) & 0xF) + 1,
	            w0 & (1 << 28), w0 & (1 << 29));
#endif
}

// Same SCB reads, vshrink math, culls and order as the historical direct-draw
// loop. Each record is drawn the moment it is produced (no record list: that
// was a cached write+readback of ~5KB/frame modal, up to 32KB dense, i.e. a
// streaming sweep through the 8KB dcache, PLAN-DRAW-RDP §9 law 1).
static void sprite_walk(void) {
	int sx = 0, sy = 0, sh = 0, sw = 0, vshrink = 0;
	bool repeat_tiles = false;
	int nrec = 0;

	uint8_t aa;
	bool aa_enabled = lspc_get_auto_animation(&aa);

	sprwalk_overflow = 0;
	// Per-render CDT context, local so the walk keeps it in registers: the
	// copy's address never escapes the inlined consume path (cx0's does,
	// into crom_resolve_ctx, which made GCC reload it after every store).
	CromResolveCtx cx0;
	crom_resolve_ctx(&cx0);
	const CromResolveCtx cx = cx0;

	for (int snum=0;snum<381;snum++) {
		uint16_t zc = VIDEO_RAM[0x8000 + snum];
		uint16_t yc = VIDEO_RAM[0x8200 + snum];
		uint16_t xc = VIDEO_RAM[0x8400 + snum];
		uint16_t *tmap = VIDEO_RAM + snum*64;

		if (!(yc & 0x40)) {
			sx = xc >> 7;
			sy = 496 - (yc >> 7);
			sh = (yc & 0x3F) * 16;
			repeat_tiles = false;
			if (sh > 32*16) { sh = 32*16; repeat_tiles = true; };
			vshrink = zc & 0xFF;
		} else {
			sx += sw;
		}

		sw = ((zc>>8)&0xF) + 1;

		if (sh == 0) continue;
		if (sx >= 320 && sx+sw <= 512) continue;
		// Coarse Y-cull (PLAN rank 11): every tile drawn below has
		// ssy in [sy, sy+sh) and passes the per-tile visibility test
		// "ssy < 224 || ssy+ssh > 512". If the whole sprite span lies in
		// the hidden band [224, 512], no tile can pass — skip the tile
		// walk entirely. Exactly equivalent to the per-tile checks (pure
		// speedup, pixel-identical by construction); chain bookkeeping
		// (sx += sw) already happened above.
		if (sy >= 224 && sy + sh <= 512) continue;
#ifdef DRAW_PERF
		perf_walk_spr++;
#endif

		// debugf("[VIDEO] sprite snum:%d xc:%04x yc:%04x zc:%04x pos:%d,%d sh:%d chain:%d repeat:%d tmap:%04x:%04x\n", snum, xc, yc, zc, sx, sy, sh, (yc & 0x40), repeat_tiles, tmap[0], tmap[1]);

		int nt, y, maxy;
		int halfy = sh < 256 ? sh : 256;
		// Early-out: when the sprite does not
		// wrap (sy+sh <= 512), ssy+ssh <= 512 for every tile (ssh is clipped
		// to sh), so a tile is visible iff ssy < 224. y never decreases in
		// the top half, and the bottom half starts at y >= 241 (512 minus a
		// top-half end <= 271) with sy >= -15, i.e. ssy >= 226: once a
		// top-half tile reaches ssy >= 224 nothing later in this sprite can
		// be visible. Skips the culled lower tiles of full-height strips
		// (~437 culled iterations/frame in fights). Same records, same order.
		const bool nowrap = (sy + sh <= 512);

		// Iterate on the two halves of the vertical sprite. This for loop
		// is mainly useful to reuse the core drawing loop. The setup
		// of the two halves is different (see below).
		for (int half = 0; half < 2; half++) {
			if (half == 0) {
				// Top half of the sprite (first 256 pixels). This part shrinks
				// to the top of the sprite position. In case of overfill, this
				// is exactly 256 pixels, repeating all tiles as required.
				maxy = halfy;
				nt = y = 0;
			} else {
				if (sh <= 256) break;

				// Bottom half of the sprite (pixels after 256). This part shrinks
				// to the bottom of the sprite (because it accesses the line ROM
				// backward, reversing also its contents).
				maxy = sh;
				if (repeat_tiles) {
					// In repeat mode, we need to find a starting Y where the next
					// tile begins, which is basically symmetric across the 256 pixel
					// line compared to where we ended up with the top half.
					// FIXME: overdraw here, we should instead clip.
					y -= (y-256)*2;
					nt = (32-nt)&31;
				} else {
					// In non-repeat mode, skip overfill area. We basically want
					// to reach the symmetric y coordinate in the bottom area.
					// FIXME: the top half overfill area should be filled with
					// the last line of tile #15, while the bottom half overfill
					// should be filled with the first line of tile #16. This
					// is currently not implemented.
					y = 32*16 - y;
				}
			}

			// Loop through the vertical sprite, tile by tile
			while (y < maxy) {
#ifdef DRAW_PERF
				perf_walk_iter++;
#endif
				// Calculate the vertical size of this tile. This is
				// a pixel-perfect formula using the magic table derived
				// from the original NeoGeo L0 ROM.
				// int ssh = vshrink/16 + (vshrink%16 > VSHRINK_MAGIC[nt]);
				int ssh = vshrink_tile_height(vshrink, nt);

				if (ssh > 0) {
					// vertical clip of the tile to the total sprite height
					// FIXME: this is wrong because ssh also affects the
					// shrinking size of the sprite. We should separate the
					// two matters.
					if (y + ssh > sh)
						ssh = sh - y;

					// See if this tile is visible, given its Y coordinate and size
					int ssy = sy + y;
					if (nowrap && ssy >= 224) goto sprite_done;
					if (ssy < 224 || (ssy+ssh) > 512) {
						uint32_t tnum = tmap[nt*2+0];
						uint32_t tc = tmap[nt*2+1];

						tnum |= (tc << 12) & 0xF0000;
						int palnum = ((tc >> 8) & 0xFF);

						// debugf("[VIDEO]   %s: nt:%d y:%d ssy:%d ssh:%d tnum:%x\n", half?"bot":"top", nt, y, ssy, ssh, tnum);

					// Auto animation
					if (aa_enabled) {
						if (tc & 8)      { tnum &= ~7; tnum |= aa & 7; }
						else if (tc & 4) { tnum &= ~3; tnum |= aa & 3; }
					}

					// Build the record and draw it.
					if (nrec < SPRWALK_MAX_RECS) {
						uint32_t w0 = tnum | (palnum << 20)
						            | ((tc & 1) << 28) | ((tc & 2) << 28);
						uint32_t w1 = (sx & 0xFFF) | ((ssy & 0xFFF) << 12)
						            | ((sw-1) << 24) | ((ssh-1) << 28);
						sprite_consume_one(&cx, w0, w1);
						nrec++;
					} else {
						sprwalk_overflow++;
					}
				}
			}

			y += ssh;
			nt++; nt &= 31;

			// In non-repeat mode (standard), the top half
			// finishes when/if we reach tile #16 (or before, if
			// the vertical sprite size is reached).
			if (!repeat_tiles && nt == 16) break;  // FIXME: draw overfill when not repeating
		}
	}
	sprite_done: ;
}

	if (sprwalk_overflow)
		debugf("[VIDEO] sprite walk overflow: %d records dropped\n", sprwalk_overflow);
}

static void render_sprites(void) {
	render_begin_sprites();
	sprite_walk();
	render_end_sprites();
}

void video_render(void) {
#ifdef DRAW_PERF_COARSE
	uint32_t t0 = TICKS_READ();
	render_begin();
	uint32_t t1 = TICKS_READ();
	render_sprites();
	uint32_t t2 = TICKS_READ();
	render_fix();
	render_end();
	uint32_t t3 = TICKS_READ();
	perf_dr_begin   += TICKS_DISTANCE(t0, t1);
	perf_dr_sprites += TICKS_DISTANCE(t1, t2);
	perf_dr_fix     += TICKS_DISTANCE(t2, t3);
#else
	render_begin();
	render_sprites();
	render_fix();
	render_end();
#endif
}

// Set on every palette write / bank switch; consumed by the N64 render_begin
// to skip the per-frame 8KB writeback + RSP color conversion when the palette
// is unchanged. Starts dirty so the first frame always converts.
uint8_t mvs64_palette_dirty = 1;

void video_palette_w(uint32_t address, uint32_t val, int sz) {
	if (sz == 4) {
		video_palette_w(address+0, val >> 16, 2);
		video_palette_w(address+2, val & 0xFFFF, 2);
		return;
	}

	if (sz == 1) val |= val << 8;  // FIXME: this is used by unibios in-game menu, verify
	address &= 0x1FFF;
	address /= 2;
	address += PALETTE_RAM_BANK;
	PALETTE_RAM[address] = val;
	mvs64_palette_dirty = 1;
}

uint32_t video_palette_r(uint32_t address, int sz) {
	if (sz==4)
		return (video_palette_r(address+0, 2) << 16) | video_palette_r(address+2, 2);

	assertf(sz == 2, "video_palette_r access size %d", sz);
	address &= 0x1FFF;
	address /= 2;
	address += PALETTE_RAM_BANK;
	return PALETTE_RAM[address];
}
