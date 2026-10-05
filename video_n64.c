
#include <libdragon.h>
#include <string.h>

extern uint32_t RSP_OVL_ID;

static void rsp_fix_init(void) {
	rspq_write(RSP_OVL_ID, 0x0);
}
static void rsp_fix_draw(uint8_t *src, int palnum, int x, int y) {
	rspq_write(RSP_OVL_ID, 0x1, PhysicalAddr(src),
		(palnum << 20) | (x << 10) | y);
}
// Flush the queue every N sprite commands (0 = never) so the RSP/RDP start
// drawing while the CPU is still issuing the frame. rspq only hands commands
// to the RSP on a flush; with libdragon's 2 KB lowpri buffers every buffer
// switch flushed implicitly (~every 128 tiles), but with larger buffers
// (MVS64_RSPQ_LOWPRI_WORDS) nothing flushed until render_end, serializing
// CPU issue and RSP/RDP execution. Runtime twin knob pinned to .data.
#ifndef MVS64_DRAW_FLUSH_EVERY
#define MVS64_DRAW_FLUSH_EVERY 64
#endif
int mvs64_draw_flush_every __attribute__((section(".data"))) = MVS64_DRAW_FLUSH_EVERY;

static int draw_since_flush;
static void rsp_sprite_draw(uint8_t *src, int palnum, int x0, int y0, int sw, int sh, bool flipx, bool flipy) {
	assertf(sw <= 16 && sh <= 16, "sprite too large: %dx%d", sw, sh);
	assertf(sw > 0 && sh > 0, "sprite too small: %dx%d", sw, sh);
	rspq_write(RSP_OVL_ID, 0x2, PhysicalAddr(src),
		(palnum << 24) | ((x0 & 0xFFF) << 12) | (y0 & 0xFFF),
		(sw-1) | ((sh-1) << 4) | (flipx ? 0x100 : 0) | (flipy ? 0x200 : 0));
	if (mvs64_draw_flush_every && ++draw_since_flush >= mvs64_draw_flush_every) {
		draw_since_flush = 0;
		rspq_flush();
	}
}
// 2-word sprite command (cmd_sprite_draw2): the C-ROM pixel slot instead
// of its address, plus the walk record fields as they are. w0's bits
// 20..29 are already pal | flipx<<8 | flipy<<9, and w1 goes verbatim; the
// RSP rebuilds cmd_sprite_draw's three words. One uncached store less per
// tile and no unpack/repack. Runtime twin knob pinned to .data; the OFF
// twin (-DMVS64_SPR2W_OFF) issues the 3-word cmd_sprite_draw.
#ifdef MVS64_SPR2W_OFF
int mvs64_spr2w __attribute__((section(".data"))) = 0;
#else
int mvs64_spr2w __attribute__((section(".data"))) = 1;
#endif
// Each 2-word command goes out as ONE uncached 64-bit store when the queue
// pointer is 8-byte aligned: on hardware every uncached store is its own
// RDRAM transaction (PERFOSD E was ~1 us/tile on a real console vs ~0.4 in
// ares). Same bytes and the same order guarantee as rspq_write (the header
// word can never be visible without its argument). render_begin_sprites
// pads the queue to 8 bytes with cmd_nop; a misaligned pointer still takes
// the two-store path. Runtime twin knob (.data; OFF twin -DMVS64_SPR64_OFF).
#ifdef MVS64_SPR64_OFF
int mvs64_spr64 __attribute__((section(".data"))) = 0;
#else
int mvs64_spr64 __attribute__((section(".data"))) = 1;
#endif
// mode: the caller's knob snapshot (video.c WALK_KNOB_2W), held in a register
// across the walk instead of reloading the knobs after every uncached store.
#define SPR_MODE_2W           1
#define SPR_MODE_64           2
#define SPR_MODE_FLUSH_SHIFT  2
static inline void rsp_sprite_draw2(uint32_t slot, uint32_t w0, uint32_t w1, int mode) {
	const int flush_every = mode >> SPR_MODE_FLUSH_SHIFT;
	uint32_t word0 = (RSP_OVL_ID + (0x7 << 24)) | (slot << 10) | ((w0 >> 20) & 0x3FF);
	volatile uint32_t *p = rspq_cur_pointer;
	if ((mode & SPR_MODE_64) && !((uint32_t)p & 7)) {
		*(volatile uint64_t *)p = ((uint64_t)word0 << 32) | w1;
	} else {
		p[1] = w1;
		p[0] = word0;
	}
	rspq_cur_pointer = p + 2;
	if (__builtin_expect(rspq_cur_pointer > rspq_cur_sentinel, 0))
		rspq_next_buffer();
	if (flush_every && ++draw_since_flush >= flush_every) {
		draw_since_flush = 0;
		rspq_flush();
	}
}
static void rsp_pal_convert(uint16_t *src, uint16_t *dst) {
	rspq_write(RSP_OVL_ID, 0x3, PhysicalAddr(src), PhysicalAddr(dst));
}
static void rsp_sprite_begin(uint16_t *palette_ram) {
	CromResolveCtx cx;
	crom_resolve_ctx(&cx);
	rspq_write(RSP_OVL_ID, 0x4, PhysicalAddr(palette_ram), PhysicalAddr(cx.sprites));
}

// Produce the visible-tile record list on the RSP (cmd_sprite_walk), split
// into kick + collect so the RSP walks while the CPU runs render_begin (the
// palette writeback/convert) instead of stalling in a full rspq_wait. The
// ucode DMAs the SCB + sprite tilemaps out of the emulated VRAM, so those
// regions are written back first. Completion is detected by polling the
// {nrec, ovfl} trailer the ucode DMAs LAST: the CPU pre-writes a sentinel
// through the uncached segment (the trailer line is never cached here — the
// collect path invalidates before any cached read), so the first non-sentinel
// value means the whole command, DMAs included, is done. A bounded timeout
// falls back to the old full rspq_wait.
#define SPRWALK_SENTINEL 0xFFFFFFFFu
static void sprite_walk_kick_rsp(SprWalkRec *list, int maxrecs, uint8_t aa, bool aa_en) {
#ifdef MVS64_WALKDBG
	debugf("[W] kick\n");
#endif
	volatile uint32_t *utrailer =
		(volatile uint32_t *)UncachedAddr((uint8_t *)list + maxrecs*8);
	utrailer[0] = SPRWALK_SENTINEL;
	data_cache_hit_writeback(VIDEO_RAM, 0xBE80);                     // sprite tilemaps
	data_cache_hit_writeback((uint8_t*)VIDEO_RAM + 0x10000, 0xC00);  // SCB
	rspq_write(RSP_OVL_ID, 0x5, PhysicalAddr(VIDEO_RAM), PhysicalAddr(list),
	           (maxrecs << 16) | (aa_en ? 0x100 : 0) | aa);
	rspq_flush();
}
static int sprite_walk_collect_rsp(SprWalkRec *list, int maxrecs) {
	volatile uint32_t *utrailer =
		(volatile uint32_t *)UncachedAddr((uint8_t *)list + maxrecs*8);
#ifdef DRAW_PERF_COARSE
	uint32_t _w0 = TICKS_READ();
#endif
	uint32_t t0 = TICKS_READ();
	while (utrailer[0] == SPRWALK_SENTINEL) {
		if (TICKS_DISTANCE(t0, TICKS_READ()) > (int32_t)TICKS_FROM_MS(20)) {
			// Should not happen (the walk is ~1ms): fall back to the
			// old drain once and log. If even that leaves the sentinel,
			// the RSP is wedged — treat as an empty frame.
			debugf("[VIDEO] sprite walk trailer timeout, draining queue\n");
			rspq_flush();
			rspq_wait();
			break;
		}
	}
#ifdef DRAW_PERF_COARSE
	perf_dr_wwait += TICKS_DISTANCE(_w0, TICKS_READ());
#endif
	uint32_t nrec = utrailer[0], ovfl = utrailer[1];
	if (nrec == SPRWALK_SENTINEL)
		nrec = ovfl = 0;
	sprwalk_rsp_ovfl = (int)ovfl;
	if (ovfl)
		debugf("[VIDEO] RSP sprite walk overflow: %lu dropped\n", (unsigned long)ovfl);
#ifdef MVS64_WALKDBG
	debugf("[W] done nrec=%lu ovfl=%lu\n", (unsigned long)nrec, (unsigned long)ovfl);
#endif
	// n64sys cache ops require 16-byte multiples: round up (the +2 record
	// padding keeps the tail inside the object).
	data_cache_hit_invalidate(list, (nrec * sizeof(SprWalkRec) + 15) & ~15);
	return (int)nrec;
}

#ifdef MVS64_SPRBATCH
// PLAN-DRAW-RDP Phase 2: batch consume — resolve tile pointers once on the
// CPU, then one cmd_sprite_batch per <=64-record chunk; the RSP loops the
// records and reuses the cmd_sprite_draw body per non-empty record.
// Compile-gated (feature absent without MVS64_SPRBATCH) and UNGATED until
// the BATCHDBG rig passes; runtime twin knob for layout-identical A/Bs.
#ifdef MVS64_BATCH_DISABLE
int mvs64_batch_enable __attribute__((section(".data"))) = 0;
#else
int mvs64_batch_enable __attribute__((section(".data"))) = 1;
#endif
// Chunk size caps the audio-latency window: a chunk is one uninterruptible
// RSP command, and whole-pump audio commands queue behind it (snd% pays
// the wait). Overridable for A/B (-DSPRBATCH_CHUNK=16); must stay <= 64
// (WALK_LIST staging is 512B) and a multiple of 2 (8-byte DMA records).
#ifndef SPRBATCH_CHUNK
#define SPRBATCH_CHUNK 64
#endif
static uint32_t sprbatch_ptrs[SPRWALK_MAX_RECS] __attribute__((aligned(16)));

#ifdef MVS64_BATCHDBG
// Phase 2 bit-exactness gate (plan §6): the ucode journals one 16-byte
// entry {a0, a1, a2, w0} per record (skips included) into this ring via
// per-record DMAOut; after draining the chunks we compare every entry
// against expected triples derived INDEPENDENTLY here from the record
// words, following the C consume/draw-path semantics. Sentinel prefill
// catches dropped/short journals (a record the ucode never reached).
// Rig build only — the extra rspq_wait per frame disqualifies it from
// any fps reading.
#define BATCHDBG_SENTINEL 0xDEADDEADu
static uint32_t batchdbg_ring[SPRWALK_MAX_RECS * 4] __attribute__((aligned(16)));
static uint32_t batchdbg_frames, batchdbg_bad;
#endif

static void sprite_walk_consume_batch(const SprWalkRec *recs, int nrec) {
	// Pointer-lifetime invariant (PLAN-DRAW-RDP §9 law 5) — VERIFIED in
	// sprite_cache.c: both eviction paths (tick-scatter pop and the
	// forced pop inside sprite_cache_insert) only remove entries whose
	// tick delta exceeds a cutoff >= 1, so entries touched THIS tick —
	// every pointer this pass resolves — cannot be evicted. Consumption
	// is same-frame: chunks are kicked below, before the next tick.
	// Pointers are written through the uncached segment (§9 law 1: no
	// cached streaming writes in the frame loop; the batch-arena killer).
	volatile uint32_t *up = (volatile uint32_t *)UncachedAddr(sprbatch_ptrs);
	// Records may be cached (C-produced walk): write back before the RSP
	// DMAs them. RSP-produced lists were already invalidated at collect;
	// writeback of uncached-clean lines is a no-op.
	data_cache_hit_writeback((void *)recs,
	                         ((unsigned)nrec * sizeof(SprWalkRec) + 15) & ~15u);
#ifdef MVS64_BATCHDBG
	volatile uint32_t *jr = (volatile uint32_t *)UncachedAddr(batchdbg_ring);
	for (int i = 0; i < nrec * 4; i++)
		jr[i] = BATCHDBG_SENTINEL;
#endif
	// Pipelined chunk kick: each chunk is issued (and flushed) the moment
	// its own pointers are resolved, so the RSP draws chunk k while the
	// CPU resolves chunk k+1. The first cut resolved ALL pointers before
	// issuing anything — measured -1.5..-2.2 fps content-matched despite
	// a ~13% faster CPU pass: the RSP idled through the resolve, the draw
	// stream finished later, and the audio whole-pump behind it in the
	// FIFO paid the delay as snd% wait (2026-08-08 twins).
	for (int off = 0; off < nrec; off += SPRBATCH_CHUNK) {
		int n = nrec - off;
		if (n > SPRBATCH_CHUNK) n = SPRBATCH_CHUNK;
		for (int i = off; i < off + n; i++) {
			uint32_t tnum = recs[i].w0 & 0xFFFFF;
			if (crom_tile_empty(tnum)) {
#ifdef DRAW_PERF
				perf_dr_empty++;
#endif
				up[i] = 0;
				continue;
			}
#ifdef DRAW_PERF
			uint32_t _c0 = TICKS_READ();
#endif
			uint8_t *src = crom_get_sprite(tnum);
#ifdef DRAW_PERF
			perf_dr_cache += TICKS_DISTANCE(_c0, TICKS_READ());
			perf_dr_tiles++;
#endif
			up[i] = PhysicalAddr(src);
		}
#ifdef MVS64_BATCHDBG
		rspq_write(RSP_OVL_ID, 0x6,
		           PhysicalAddr((void *)(recs + off)),
		           PhysicalAddr((uint8_t *)sprbatch_ptrs + off * 4), n,
		           PhysicalAddr((uint8_t *)batchdbg_ring + off * 16));
#else
		rspq_write(RSP_OVL_ID, 0x6,
		           PhysicalAddr((void *)(recs + off)),
		           PhysicalAddr((uint8_t *)sprbatch_ptrs + off * 4), n);
#endif
		rspq_flush();
	}
#ifdef MVS64_BATCHDBG
	rspq_wait();
	int bad = 0;
	for (int i = 0; i < nrec; i++) {
		uint32_t w0 = recs[i].w0, w1 = recs[i].w1;
		uint32_t e0 = up[i];
		uint32_t pal = (w0 >> 20) & 0xFF;
		uint32_t x0 = w1 & 0xFFF, y0 = (w1 >> 12) & 0xFFF;
		uint32_t sw = ((w1 >> 24) & 0xF) + 1, sh = ((w1 >> 28) & 0xF) + 1;
		uint32_t e1 = (pal << 24) | ((x0 & 0xFFF) << 12) | (y0 & 0xFFF);
		uint32_t e2 = (sw - 1) | ((sh - 1) << 4)
		            | ((w0 & (1u << 28)) ? 0x100 : 0)
		            | ((w0 & (1u << 29)) ? 0x200 : 0);
		if (jr[i*4+0] != e0 || jr[i*4+1] != e1
		    || jr[i*4+2] != e2 || jr[i*4+3] != w0) {
			if (bad++ < 4)
				debugf("[BATCHDBG] MISMATCH f=%lu i=%d got %08lx/%08lx/%08lx/%08lx exp %08lx/%08lx/%08lx/%08lx\n",
					(unsigned long)batchdbg_frames, i,
					(unsigned long)jr[i*4+0], (unsigned long)jr[i*4+1],
					(unsigned long)jr[i*4+2], (unsigned long)jr[i*4+3],
					(unsigned long)e0, (unsigned long)e1,
					(unsigned long)e2, (unsigned long)w0);
		}
	}
	batchdbg_bad += bad;
	if (bad)
		debugf("[BATCHDBG] frame %lu: %d bad entries of %d recs\n",
			(unsigned long)batchdbg_frames, bad, nrec);
	if (++batchdbg_frames % 600 == 0)
		debugf("[BATCHDBG] %lu frames checked, %lu bad total\n",
			(unsigned long)batchdbg_frames, (unsigned long)batchdbg_bad);
#endif
}
#endif // MVS64_SPRBATCH

static int fix_last_spritnum = 0;

static void draw_sprite_src(uint8_t *src, int palnum, int x0, int y0, int sw, int sh, bool flipx, bool flipy);

static void draw_sprite(int spritenum, int palnum, int x0, int y0, int sw, int sh, bool flipx, bool flipy) {
#ifdef DRAW_PERF
	// Fine split of the sprite pass (walk = spr - cache - rspq): the cache
	// bucket is crom_get_sprite (hash walk + cart DFS on miss), the rspq
	// bucket is the command write INCLUDING any queue back-pressure stall.
	uint32_t _c0 = TICKS_READ();
#endif
	uint8_t *src = crom_get_sprite(spritenum);
#ifdef DRAW_PERF
	perf_dr_cache += TICKS_DISTANCE(_c0, TICKS_READ());
	perf_dr_tiles++;
#endif
	draw_sprite_src(src, palnum, x0, y0, sw, sh, flipx, flipy);
}

static void draw_sprite_src(uint8_t *src, int palnum, int x0, int y0, int sw, int sh, bool flipx, bool flipy) {
#ifdef DRAW_PERF_COARSE
	uint32_t _r0 = TICKS_READ();
	rsp_sprite_draw(src, palnum, x0, y0, sw, sh, flipx, flipy);
	perf_dr_rspq += TICKS_DISTANCE(_r0, TICKS_READ());
#else
	rsp_sprite_draw(src, palnum, x0, y0, sw, sh, flipx, flipy);
#endif
}

static void render_begin_sprites(void) {
	rdpq_debug_log_msg("render_begin_sprites");
	rsp_sprite_begin(PALETTE_RAM_EMU);
	rdpq_mode_begin();
		rdpq_set_mode_standard();
		rdpq_mode_tlut(TLUT_RGBA16);
		rdpq_mode_alphacompare(1);
	rdpq_mode_end();
	// 8-byte align the queue for the 64-bit sprite command stores
	// (sprite commands are 8 bytes, buffers start aligned).
	if (mvs64_spr64 && ((uint32_t)rspq_cur_pointer & 7))
		rspq_write(RSP_OVL_ID, 0x8);
}

static void render_end_sprites(void) {}

#define FIX_TMEM_ADDR 	0
#define FIX_TMEM_PITCH  8

static void draw_sprite_fix(int spritenum, int palnum, int x, int y) {
	uint8_t *src = NULL;
	if (spritenum != fix_last_spritnum) {
		fix_last_spritnum = spritenum;
		src = srom_get_sprite(spritenum);
	}
	rsp_fix_draw(src, palnum, x, y);
}

static void render_begin_fix(void) {
	rdpq_sync_pipe();
	rdpq_sync_tile();
	rdpq_set_mode_copy(true);
	rdpq_mode_tlut(TLUT_RGBA16);

	// Load all 16 palettes right away. They fit TMEM, so that we don't need
	// to load them while we process
	rdpq_tex_load_tlut(PALETTE_RAM_EMU, 0, 256);

	// Configure tiles once
	rdpq_set_tile(TILE0, FMT_CI4, FIX_TMEM_ADDR, FIX_TMEM_PITCH, 0);  // used for drawing
	rdpq_set_tile(TILE1, FMT_CI8, FIX_TMEM_ADDR, FIX_TMEM_PITCH, 0);  // used for loading
	rdpq_set_tile_size(TILE0, 0, 0, 8, 8);

	fix_last_spritnum = -1;

	rsp_fix_init();
}

static void render_end_fix(void) {}

static void render_begin(void) {
	// Reconvert the palette only when it changed since the last frame
	// (writes via the asm/C MMIO handlers or a bank switch set the flag).
	// PALETTE_RAM_EMU persists in RDRAM between frames otherwise.
	//
	// The RSP converts from a SNAPSHOT, not from the live PALETTE_RAM: the
	// pal_convert commands can sit behind the previous pump's highpri audio
	// burst while the 68k already runs the next frame and writes the live
	// palette (dirty lines can reach RDRAM before the RSP DMAs them), which
	// would show a palette one frame early on fades. Only the small default
	// rspq buffer used to hide this, by forcing the CPU to wait. pal_snap is
	// rewritten only here, after display_get proved the previous frame's
	// commands complete (2 display buffers).
	extern uint8_t mvs64_palette_dirty;
	static uint16_t pal_snap[4096] __attribute__((aligned(16)));
	if (mvs64_palette_dirty) {
		mvs64_palette_dirty = 0;
		memcpy(pal_snap, PALETTE_RAM + PALETTE_RAM_BANK, sizeof(pal_snap));
		data_cache_hit_writeback(pal_snap, sizeof(pal_snap));
		for (int i=0; i<4096 / 0x400; i++) {
			rsp_pal_convert(pal_snap + i*0x400, PALETTE_RAM_EMU + i*0x400);
		}
	}

	uint16_t bkg = color_convert(PALETTE_RAM[PALETTE_RAM_BANK+0xFFF]) | 1;

	// Clear the screen
	// rdpq_debug_log(true);
	rdpq_set_mode_fill(color_from_packed16(bkg));
	rdpq_fill_rectangle(0, 0, 320, 240);
	rspq_flush();
}

static void render_end(void) {
	rspq_flush();
	// rdpq_debug_log(false);
}
