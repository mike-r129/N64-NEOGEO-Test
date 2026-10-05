
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
