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
uint32_t perf_dr_wwait;   /* RSP sprite walk: CPU time blocked in rspq_wait */
uint32_t perf_walk_spr;   /* sprites that reach the tile loop (past all culls) */
uint32_t perf_walk_iter;  /* tile-loop iterations, visible or not */
// PLAN-DRAW-RDP Phase 0 decision counters ([PERF3] in emu.c):
uint32_t perf_dr_recs;    /* records seen by consume (incl. empty-skipped) */
uint32_t perf_dr_adjrep;  /* drawn records whose tnum == previous drawn */
uint32_t perf_dr_maxrun;  /* longest consecutive same-tnum drawn run */
uint32_t perf_dr_uniqx;   /* ~unique tnums (gen-stamped hash, collisions
                             undercount uniques slightly) */
uint32_t perf_dr_psw;     /* palette switches between consecutive draws */
uint32_t perf_dr_modal;   /* drawn records matching the COPY-mode predicate
                             (sw==16 && sh==16 && no flip) */
uint32_t perf_dr_miss;    /* sprite-cache misses (PI DMA loads) */
uint32_t perf_dr_missticks; /* ticks spent in the miss/DMA path */
// Uniq table: 2048 gen-stamped entries accessed UNCACHED — heavy frames
// touch most slots, and an 8KB cached resident table would evict the whole
// dcache (PLAN-DRAW-RDP §9 law 1). Diagnostic builds only.
static uint32_t perf_uniq_tab[2048] __attribute__((aligned(16)));
static uint32_t perf_uniq_gen;
#define DRAW_PERF 1
#endif

// --- sprite walk: produce/consume split -----------------------------------
// The SCB walk produces a flat list of visible-tile records; the consume
// pass turns records into draw calls. The split is what lets the walk move
// to the RSP on N64 (cmd_sprite_walk in rsp_video.S produces the same list):
// the C producer below is the bit-exact reference, and the ucode is gated
// against it record-by-record (MVS64_WALKDBG dual-compute). Record fields
// carry exactly what draw_sprite needs; positions keep only the low 12 bits,
// which is lossless: both draw paths reduce positions mod 512 (PC) or to a
// 12-bit signed field (RSP), and sx/ssy never carry information above that.
typedef struct {
	uint32_t w0;   // tnum[0..19] | palnum[20..27] | flipx[28] | flipy[29]
	uint32_t w1;   // sx[0..11] | ssy[12..23] | (sw-1)[24..27] | (ssh-1)[28..31]
} SprWalkRec;

// ~7x the observed in-fight maximum (~600 tiles). Overflowing content is
// walked correctly but its excess records are dropped (logged), so pixels
// would differ from the old direct-draw path only in that case.
#define SPRWALK_MAX_RECS  4096
// +2 records: the RSP walk writes its {nrec, ovfl} trailer at list+maxrecs*8
// (sprite_walk_produce_rsp) — keep it inside the object for -Warray-bounds.
static SprWalkRec sprwalk_recs[SPRWALK_MAX_RECS + 2] __attribute__((aligned(16)));
static int sprwalk_overflow;   // records dropped this frame (diagnostic)
#ifdef N64
static int sprwalk_rsp_ovfl;   // overflow count reported by the RSP walk
#endif

#ifdef N64
	#if 1
	#include "video_n64.c"
	#else
	#include "video_cpu.c"
	#endif
#else
#include "video_cpu.c"
#endif

static void render_fix(void) {
	uint16_t *fix = VIDEO_RAM + 0x7000;

	render_begin_fix();

	for (int i=0;i<40;i++) {
		fix += 2; // skip two lines
		for (int j=0;j<28;j++) {
			uint16_t v = *fix++;
			// Skip tiles known to decode to all-transparent pixels: the map
			// is full of nonzero "blank" codes, so without this we issue
			// ~1120 draws/frame that can never touch the screen (see
			// srom_tile_empty).
			if (v && !srom_tile_empty(v & 0xFFF))
				draw_sprite_fix(v & 0xFFF, (v >> 12) & 0xF, i*8, j*8);
		}
		fix += 2;
	}

	render_end_fix();
}


static inline void sprite_consume_begin(void);
static inline void sprite_consume_one(uint32_t w0, uint32_t w1);

// Bit-exact reference walk: same SCB reads, same vshrink math, same culls,
// same order as the historical direct-draw loop.
// recs == NULL is the FUSED mode (default path): each record is consumed
// (empty-skip + draw) the moment it is produced instead of being stored —
// the list only exists for the RSP-walk / batch paths that need it. Same
// records, same order, same maxrecs drop rule => the draw stream is
// identical by construction; what goes away is the cached record-list
// write+readback (~5KB/frame modal, up to 32KB dense: a streaming sweep
// through the 8KB dcache, PLAN-DRAW-RDP §9 law 1).
static int sprite_walk_produce(SprWalkRec *recs, int maxrecs) {
	int sx = 0, sy = 0, sh = 0, sw = 0, vshrink = 0;
	bool repeat_tiles = false;
	int nrec = 0;

	uint8_t aa;
	bool aa_enabled = lspc_get_auto_animation(&aa);

	sprwalk_overflow = 0;
	if (!recs) sprite_consume_begin();

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

					// Emit the record the consume pass will draw.
					if (nrec < maxrecs) {
						uint32_t w0 = tnum | (palnum << 20)
						            | ((tc & 1) << 28) | ((tc & 2) << 28);
						uint32_t w1 = (sx & 0xFFF) | ((ssy & 0xFFF) << 12)
						            | ((sw-1) << 24) | ((ssh-1) << 28);
						if (recs) {
							recs[nrec].w0 = w0;
							recs[nrec].w1 = w1;
						} else
							sprite_consume_one(w0, w1);
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
}

	if (sprwalk_overflow)
		debugf("[VIDEO] sprite walk overflow: %d records dropped\n", sprwalk_overflow);
	return nrec;
}

#ifdef DRAW_PERF
// Phase 0 run/repeat/palette stats over the DRAWN stream (post empty-skip:
// that is the stream Phase 1's memo and Phase 3's mode runs would see).
// Reset per consume pass (sprite_consume_begin); gen bump ages the uniq
// table without clearing it.
static uint32_t p0_last_tnum, p0_last_pal, p0_run;
#endif

#ifdef N64
// CROM direct-table knob (runtime twin; .data-pinned like mvs64_walk_fuse).
#ifdef MVS64_CDT_OFF
int mvs64_cdt_enable __attribute__((section(".data"))) = 0;
#else
int mvs64_cdt_enable __attribute__((section(".data"))) = 1;
#endif
#endif

static inline void sprite_consume_begin(void) {
#ifdef DRAW_PERF
	p0_last_tnum = ~0u; p0_last_pal = ~0u; p0_run = 0;
	perf_uniq_gen++;
#endif
}

#ifdef DRAW_PERF
static inline void sprite_consume_p0(uint32_t tnum, uint32_t w0, uint32_t w1) {
		volatile uint32_t *p0_uniq =
			(volatile uint32_t *)UncachedAddr(perf_uniq_tab);
		uint32_t pal = (w0 >> 20) & 0xFF;
		uint32_t sw = ((w1 >> 24) & 0xF) + 1, sh = ((w1 >> 28) & 0xF) + 1;
		if (tnum == p0_last_tnum) {
			perf_dr_adjrep++;
			if (++p0_run > perf_dr_maxrun) perf_dr_maxrun = p0_run;
		} else
			p0_run = 0;
		if (pal != p0_last_pal) perf_dr_psw++;
		if (sw == 16 && sh == 16 && !(w0 & (3u << 28))) perf_dr_modal++;
		p0_last_tnum = tnum; p0_last_pal = pal;
		uint32_t h = (tnum * 2654435761u) >> 21;
		uint32_t key = (perf_uniq_gen << 20) | tnum;
		if (p0_uniq[h] != key) { perf_dr_uniqx++; p0_uniq[h] = key; }
}
#endif

// Consume one record: identical tail of the historical loop — empty-tile
// skip, then draw_sprite (cache side effects unchanged).
static inline void sprite_consume_one(uint32_t w0, uint32_t w1) {
	uint32_t tnum = w0 & 0xFFFFF;

#ifdef DRAW_PERF
	perf_dr_recs++;
#endif
#ifdef N64
	if (mvs64_cdt_enable) {
		// Fused empty-test + resolve through the CROM direct table
		// (roms.c crom_resolve): one sparse read per record. Same
		// records, same order, same skip set => same draw stream.
#ifdef DRAW_PERF
		uint32_t _c0 = TICKS_READ();
#endif
		uint8_t *src = crom_resolve(tnum);
#ifdef DRAW_PERF
		perf_dr_cache += TICKS_DISTANCE(_c0, TICKS_READ());
#endif
		if (!src) {
#ifdef DRAW_PERF
			perf_dr_empty++;
#endif
			return;
		}
#ifdef DRAW_PERF
		perf_dr_tiles++;
		sprite_consume_p0(tnum, w0, w1);
#endif
		draw_sprite_src(src, (w0 >> 20) & 0xFF,
		                w1 & 0xFFF, (w1 >> 12) & 0xFFF,
		                ((w1 >> 24) & 0xF) + 1, ((w1 >> 28) & 0xF) + 1,
		                w0 & (1 << 28), w0 & (1 << 29));
		return;
	}
#endif
	// Skip tiles known to decode to all-transparent
	// pixels — the sprite-layer analogue of the fix
	// skip above (ROM-stable fact, learned on first
	// fetch; pixel-identical by construction: index-0
	// pixels never pass the alpha compare).
	if (crom_tile_empty(tnum)) {
#ifdef DRAW_PERF
		perf_dr_empty++;
#endif
		return;
	}

#ifdef DRAW_PERF
	sprite_consume_p0(tnum, w0, w1);
#endif
	draw_sprite(tnum, (w0 >> 20) & 0xFF,
	            w1 & 0xFFF, (w1 >> 12) & 0xFFF,
	            ((w1 >> 24) & 0xF) + 1, ((w1 >> 28) & 0xF) + 1,
	            w0 & (1 << 28), w0 & (1 << 29));
}

// Consume pass over a produced record list, in record order.
static void sprite_walk_consume(const SprWalkRec *recs, int nrec) {
#if defined(N64) && defined(MVS64_SPRBATCH)
	// Phase 2 batch path (video_n64.c): resolve pointers once, one RSP
	// command per chunk. PC build always keeps the C path (plan §9 law 8).
	if (mvs64_batch_enable) {
		sprite_walk_consume_batch(recs, nrec);
		return;
	}
#endif
	sprite_consume_begin();
	for (int i=0;i<nrec;i++)
		sprite_consume_one(recs[i].w0, recs[i].w1);
}

#if defined(N64) && defined(MVS64_WALK_RSP)
// Runtime walk gate (twin-binary A/B law: ON and OFF builds differ by one
// initializer constant, keeping layout identical — the old "-1.6 modal"
// walk verdict was a cross-binary measurement and is layout-confounded).
// The kick site latches the decision per frame so collect always matches.
#ifdef MVS64_WALK_DISABLE
int mvs64_walk_enable = 0;
#else
int mvs64_walk_enable = 1;
#endif
static int walk_kicked_this_frame;
#endif

// Fused walk knob (runtime twin, layout-identical A/B: the OFF twin differs
// by this one initializer; pinned to .data so a 0 initializer cannot move
// it into .bss and shift the layout). The batch path consumes a list, so
// it always takes the split walk.
#ifdef MVS64_FUSE_OFF
int mvs64_walk_fuse __attribute__((section(".data"))) = 0;
#else
int mvs64_walk_fuse __attribute__((section(".data"))) = 1;
#endif

static void sprite_walk_default(void) {
#if defined(N64) && defined(MVS64_SPRBATCH)
	if (mvs64_batch_enable) {
		int nrec = sprite_walk_produce(sprwalk_recs, SPRWALK_MAX_RECS);
		sprite_walk_consume(sprwalk_recs, nrec);
		return;
	}
#endif
	if (mvs64_walk_fuse) {
		sprite_walk_produce(NULL, SPRWALK_MAX_RECS);
		return;
	}
	int nrec = sprite_walk_produce(sprwalk_recs, SPRWALK_MAX_RECS);
	sprite_walk_consume(sprwalk_recs, nrec);
}

static void render_sprites(void) {
	render_begin_sprites();
#if defined(N64) && defined(MVS64_WALK_RSP)
	if (!walk_kicked_this_frame) {
		sprite_walk_default();
		render_end_sprites();
		return;
	}
	// OPT-IN (default OFF). The walk runs on the RSP (cmd_sprite_walk,
	// rsp_video.S); the CPU only consumes the record list. Bit-exact
	// (MVS64_WALKDBG dual-compute, 11.4k frames, 0 mismatches). The old
	// walk+audio deadlock was the rspq lost-wakeup race, fixed closed-loop
	// in the vendored libdragon (patches/libdragon-rspq-closed-loop-flush.
	// patch, 2026-08-07). The kick happened at video_render entry so the
	// RSP walked during render_begin; here we only collect (sentinel
	// trailer poll — no full-queue rspq_wait).
	int nrec = sprite_walk_collect_rsp(sprwalk_recs, SPRWALK_MAX_RECS);
	#ifdef MVS64_WALKDBG
	// Dual-compute gate: the C walk is authoritative; compare record lists
	// per frame and log any divergence (see rsp_audio's VERIFY pattern).
	{
		static SprWalkRec recs_c[SPRWALK_MAX_RECS];
		static uint32_t walkdbg_frames;
		int rsp_ovfl = sprwalk_rsp_ovfl;
		int nc = sprite_walk_produce(recs_c, SPRWALK_MAX_RECS);
		if (nc != nrec || memcmp(recs_c, sprwalk_recs, nc * sizeof(SprWalkRec)) != 0
		    || rsp_ovfl != sprwalk_overflow) {
			debugf("[WALKDBG] MISMATCH frame=%lu nrec=%d nc=%d ovfl=%d/%d\n",
				(unsigned long)walkdbg_frames, nrec, nc, rsp_ovfl, sprwalk_overflow);
			for (int i=0; i<nc && i<nrec; i++) {
				if (recs_c[i].w0 != sprwalk_recs[i].w0 || recs_c[i].w1 != sprwalk_recs[i].w1) {
					debugf("[WALKDBG] first diff @%d: rsp %08lx/%08lx vs c %08lx/%08lx\n", i,
						(unsigned long)sprwalk_recs[i].w0, (unsigned long)sprwalk_recs[i].w1,
						(unsigned long)recs_c[i].w0, (unsigned long)recs_c[i].w1);
					break;
				}
			}
		}
		if (++walkdbg_frames % 600 == 0)
			debugf("[WALKDBG] %lu frames checked OK\n", (unsigned long)walkdbg_frames);
		sprite_walk_consume(recs_c, nc);
	}
	#else
	sprite_walk_consume(sprwalk_recs, nrec);
	#endif
#else
	sprite_walk_default();
#endif
	render_end_sprites();
}



void video_render(void) {
#ifdef MVS64_NORENDER
	// Diagnostic: skip all sprite/fix drawing (RSP/RDP) to isolate whether the
	// ~frame-537 crash is in the N64 render path. Frames still flip (blank screen).
	return;
#endif
#if defined(N64) && defined(MVS64_WALK_RSP)
	// Kick the RSP sprite walk FIRST: VRAM is stable for the whole render
	// (the 68k is not running), so the walk overlaps render_begin's CPU
	// work and render_sprites only has to collect the finished list.
	walk_kicked_this_frame = mvs64_walk_enable;
	if (walk_kicked_this_frame) {
		uint8_t aa_k;
		bool aa_en_k = lspc_get_auto_animation(&aa_k);
		sprite_walk_kick_rsp(sprwalk_recs, SPRWALK_MAX_RECS, aa_k, aa_en_k);
	}
#endif
#ifdef DRAW_PERF
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
