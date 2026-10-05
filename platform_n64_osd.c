// Diagnostic overlays and the pixel gate for N64 builds: the PERFOSD
// hardware perf overlay, the text renderer it shares with SNDOSD (whose
// body stays in platform_n64.c next to the audio ring it reads), and the
// FBCRC framebuffer hash. Every block here is compiled only in those
// diagnostic builds, so the release image does not change.
#include <libdragon.h>
#include <stdio.h>
#include "platform.h"
#include "platform_n64_osd.h"

#if defined(MVS64_SNDOSD) || defined(MVS64_PERFOSD)
// On-screen text for the hardware overlays (ares/paraLLEl-RDP does not model
// the DPC counters, so RDP numbers come from a real console). osd_text
// draws a 4x6 bitmap font at 2x into a 16-bit buffer: SNDOSD draws onto the
// finished frame through the uncached segment (detach_wait first); PERFOSD
// draws into its own texture (see below).
static const uint8_t osd_font[32][6] = {
	{0x6,0x9,0x9,0x9,0x9,0x6}, {0x2,0x6,0x2,0x2,0x2,0x7}, // 0 1
	{0x6,0x9,0x1,0x2,0x4,0xF}, {0xE,0x1,0x6,0x1,0x1,0xE}, // 2 3
	{0x2,0x6,0xA,0xF,0x2,0x2}, {0xF,0x8,0xE,0x1,0x1,0xE}, // 4 5
	{0x6,0x8,0xE,0x9,0x9,0x6}, {0xF,0x1,0x2,0x2,0x4,0x4}, // 6 7
	{0x6,0x9,0x6,0x9,0x9,0x6}, {0x6,0x9,0x9,0x7,0x1,0x6}, // 8 9
	{0x0,0x0,0x0,0x0,0x0,0x2},                            // .
	{0xE,0x9,0xE,0x8,0x8,0x8}, {0xF,0x2,0x2,0x2,0x2,0x2}, // P T
	{0xE,0x9,0xE,0x9,0x9,0xE}, {0xF,0x8,0xE,0x8,0x8,0x8}, // B F
	{0x0,0x0,0x0,0x0,0x0,0x0},                            // space
	{0xE,0x9,0x9,0x9,0x9,0xE}, {0x9,0xA,0xC,0xC,0xA,0x9}, // D K
	{0xE,0x9,0x9,0xE,0xA,0x9}, {0x7,0x8,0x6,0x1,0x1,0xE}, // R S
	{0x8,0x8,0x8,0x8,0x8,0xF}, {0x6,0x9,0x8,0x8,0x9,0x6}, // L C
	{0x9,0x9,0x9,0xF,0xF,0x9},                            // W
	{0x9,0xF,0xF,0x9,0x9,0x9}, {0x9,0x9,0x9,0x9,0x6,0x6}, // M V
	{0x6,0x9,0x9,0xF,0x9,0x9}, {0x9,0x9,0x6,0x6,0x9,0x9}, // A X
	{0xF,0x8,0xE,0x8,0x8,0xF}, {0x9,0xD,0xD,0xB,0xB,0x9}, // E N
	{0x6,0x9,0x9,0x9,0xA,0x5}, {0x6,0x9,0x8,0xB,0x9,0x7}, // Q G
	{0x9,0x9,0xF,0x9,0x9,0x9},                            // H
};
void osd_text(uint16_t *fb, int stride_px, int x, int y, const char *s) {
	for (; *s; s++, x += 10) {
		int g;
		if (*s >= '0' && *s <= '9') g = *s - '0';
		else if (*s == '.') g = 10;
		else if (*s == 'P') g = 11;
		else if (*s == 'T') g = 12;
		else if (*s == 'B') g = 13;
		else if (*s == 'F') g = 14;
		else if (*s == 'D') g = 16;
		else if (*s == 'K') g = 17;
		else if (*s == 'R') g = 18;
		else if (*s == 'S') g = 19;
		else if (*s == 'L') g = 20;
		else if (*s == 'C') g = 21;
		else if (*s == 'W') g = 22;
		else if (*s == 'M') g = 23;
		else if (*s == 'V') g = 24;
		else if (*s == 'A') g = 25;
		else if (*s == 'X') g = 26;
		else if (*s == 'E') g = 27;
		else if (*s == 'N') g = 28;
		else if (*s == 'Q') g = 29;
		else if (*s == 'G') g = 30;
		else if (*s == 'H') g = 31;
		else continue;
		for (int r = 0; r < 6; r++) {
			uint8_t bits = osd_font[g][r];
			for (int c = 0; c < 4; c++) {
				if (!(bits & (8 >> c))) continue;
				uint16_t *p = fb + (y + r*2) * stride_px + x + c*2;
				p[0] = p[1] = p[stride_px] = p[stride_px+1] = 0xFFFF;
			}
		}
	}
}
#endif

#if defined(MVS64_FBCRC) || defined(MVS64_FBCRC_PIPE)
// FNV-1a over the visible 320x224 region of a finished frame, read uncached
// (the RDP wrote RDRAM behind the CPU cache).
uint32_t osd_fb_crc(const surface_t *disp) {
	uint32_t crc = 0x811C9DC5u;
	const uint8_t *row = (const uint8_t *)UncachedAddr(disp->buffer);
	for (int y = 0; y < 224; y++) {
		const uint32_t *p = (const uint32_t *)row;
		for (int x = 0; x < 320*2/4; x++) {
			crc ^= p[x];
			crc *= 16777619u;
		}
		row += disp->stride;
	}
	return crc;
}
#endif

#ifdef MVS64_PERFOSD
// Hardware perf overlay that does NOT stall the pipeline: unlike SNDOSD
// (detach_wait, then CPU-draw onto the finished frame, which
// serializes CPU and RDP every frame), the text is rendered into a small
// RGBA16 texture only when the numbers change (every 60 drawn frames) and
// blitted each frame with one copy-mode rectangle queued at the end of the
// frame's own RDP work. Two textures alternate, so the one being rewritten
// was last read 60 frames ago. Per guest frame, averaged over the window:
//   F dd.d gg.g   drawn fps, game-speed (emulated) fps
//   M mm.m S ss.s 68k ms (incl. MMIO and Z80 catch-up in sound commands),
//                 sound ms (Z80 + YM2610 pump)
//   V vv.v W ww.w draw CPU ms (video_render issue + frame end, without the
//                 wait), and ms spent in display_get waiting for a free
//                 buffer (= the RSP/RDP still finishing older frames: the
//                 frame is RDP-bound when W is large)
//   B bb.b L ll.l inside V: render_begin (palette snapshot/convert issue),
//                 fix layer
//   R rr.r        inside V: sprites total
//   Q qq.q E ee.e inside R: C-ROM tile lookups (incl. C), RSP sprite
//                 command issue (incl. flushes); the walk is R - Q - E
//   C cc.c N nnn  inside Q: C-ROM tile-cache miss reads from the cart;
//                 sprite tiles drawn per frame
//   G ggg H hhh   drawn tiles per frame in RDP COPY mode, and tiles that
//                 would be COPY but are flipped (the rest of N is 1-cycle:
//                 shrunk, x-clipped or flipped partial tiles)
//   A aa.a X xx.x whole guest frame ms (CPU wall time incl. all waits), and
//                 A minus M+S+V+W: the part no other line accounts for
//   P pp.p T tt.t RDP pipe-busy / TMEM-busy ms per drawn frame (DPC
//                 counters; real hardware only, ares reads 0)
#define POSD_W 160
#define POSD_H 146
static surface_t posd_surf[2];
static int posd_cur = -1;
uint32_t posd_wait;                 // ticks in display_get (plat_beginframe), current guest frame
static uint32_t posd_acc_all, posd_acc_m68k, posd_acc_snd, posd_acc_draw, posd_acc_wait;
static uint32_t posd_acc_n;
// Draw split (video.c DRAW_PERF_COARSE): render_begin, sprites (incl. the
// C-ROM miss reads), fix layer.
static uint32_t posd_acc_beg, posd_acc_spr, posd_acc_fix, posd_acc_miss;
static uint32_t posd_acc_look, posd_acc_emit, posd_acc_tiles, posd_acc_copyt, posd_acc_flipt;
// emu.c main loop, once per guest frame.
void plat_perf_frame(uint32_t all, uint32_t m68k, uint32_t snd, uint32_t draw) {
	extern uint32_t perf_dr_begin, perf_dr_sprites, perf_dr_fix, perf_dr_missticks;
	extern uint32_t perf_dr_cache, perf_dr_rspq, perf_dr_tiles, perf_dr_copyt, perf_dr_flipt;
	posd_acc_all += all; posd_acc_m68k += m68k; posd_acc_snd += snd;
	posd_acc_draw += draw; posd_acc_wait += posd_wait;
	posd_acc_beg += perf_dr_begin; posd_acc_spr += perf_dr_sprites; posd_acc_fix += perf_dr_fix;
	posd_acc_miss += perf_dr_missticks;
	posd_acc_look += perf_dr_cache; posd_acc_emit += perf_dr_rspq; posd_acc_tiles += perf_dr_tiles;
	posd_acc_copyt += perf_dr_copyt; posd_acc_flipt += perf_dr_flipt;
	perf_dr_begin = perf_dr_sprites = perf_dr_fix = perf_dr_missticks = 0;
	perf_dr_cache = perf_dr_rspq = perf_dr_tiles = perf_dr_copyt = perf_dr_flipt = 0;
	posd_wait = 0;
	posd_acc_n++;
}
static uint32_t posd_ms10(uint32_t ticks, uint32_t n) {   // ms*10 per frame
	return n ? (uint32_t)((uint64_t)ticks * 10000 / ((uint64_t)TICKS_PER_SECOND * n)) : 0;
}
static void posd_render(char lines[][32], int nlines) {
	int nxt = posd_cur < 0 ? 0 : posd_cur ^ 1;
	surface_t *s = &posd_surf[nxt];
	if (!s->buffer) *s = surface_alloc(FMT_RGBA16, POSD_W, POSD_H);
	{   // opaque black backdrop (RGBA5551 0x0001: alpha bit set) for legibility
		uint16_t *px = (uint16_t *)s->buffer;
		for (int i = 0; i < s->stride / 2 * POSD_H; i++) px[i] = 0x0001;
	}
	for (int i = 0; i < nlines; i++)
		osd_text((uint16_t *)s->buffer, s->stride / 2, 4, 4 + i * 14, lines[i]);
	data_cache_hit_writeback(s->buffer, s->stride * POSD_H);
	posd_cur = nxt;
}

// plat_endframe, once per drawn frame: accumulate the DPC counters, refresh
// the overlay texture every 60 frames, and queue its blit.
void perfosd_endframe(void) {
	extern int g_frame;
	static uint32_t tick0, pipe0, tmem0, acc_pipe, acc_tmem;
	static int accn, gf0;
	static char lines[10][32];
	// DPC counters are 24-bit at 62.5 MHz (wrap every ~0.27 s):
	// accumulate per-frame deltas. Read without draining, so a delta
	// covers whatever the RDP finished since the last read.
	uint32_t pipe = *(volatile uint32_t*)0xA4100018 & 0xFFFFFF;
	uint32_t tmem = *(volatile uint32_t*)0xA410001C & 0xFFFFFF;
	if (tick0 == 0) {   // bootstrap
		tick0 = TICKS_READ(); gf0 = g_frame;
	} else {
		acc_pipe += (pipe - pipe0) & 0xFFFFFF;
		acc_tmem += (tmem - tmem0) & 0xFFFFFF;
	}
	pipe0 = pipe; tmem0 = tmem;
	if (++accn >= 60) {
		uint32_t now = TICKS_READ();
		uint32_t dt = TICKS_DISTANCE(tick0, now);
		uint32_t n = posd_acc_n;
		if (dt && n) {
			uint32_t f10 = (uint32_t)((uint64_t)TICKS_PER_SECOND * accn * 10 / dt);
			uint32_t g10 = (uint32_t)((uint64_t)TICKS_PER_SECOND * (uint32_t)(g_frame - gf0) * 10 / dt);
			uint32_t m = posd_ms10(posd_acc_m68k, n), s = posd_ms10(posd_acc_snd, n);
			uint32_t w = posd_ms10(posd_acc_wait, n);
			uint32_t v = posd_ms10(posd_acc_draw - posd_acc_wait, n);
			uint32_t a = posd_ms10(posd_acc_all, n);
			// RDP clock 62.5 MHz = 62500 cycles/ms; per drawn frame
			uint32_t p = (uint32_t)((uint64_t)acc_pipe * 10 / ((uint64_t)accn * 62500u));
			uint32_t t = (uint32_t)((uint64_t)acc_tmem * 10 / ((uint64_t)accn * 62500u));
			snprintf(lines[0], 32, "F %lu.%lu %lu.%lu", (unsigned long)(f10/10), (unsigned long)(f10%10),
			         (unsigned long)(g10/10), (unsigned long)(g10%10));
			snprintf(lines[1], 32, "M %lu.%lu S %lu.%lu", (unsigned long)(m/10), (unsigned long)(m%10),
			         (unsigned long)(s/10), (unsigned long)(s%10));
			snprintf(lines[2], 32, "V %lu.%lu W %lu.%lu", (unsigned long)(v/10), (unsigned long)(v%10),
			         (unsigned long)(w/10), (unsigned long)(w%10));
			// X = frame time not covered by M/S/V/W (events, input poll,
			// interrupt handlers, anything else in the loop)
			uint32_t mx = m + s + v + w, x = a > mx ? a - mx : 0;
			uint32_t b = posd_ms10(posd_acc_beg, n), r = posd_ms10(posd_acc_spr, n);
			uint32_t l = posd_ms10(posd_acc_fix, n);
			uint32_t c = posd_ms10(posd_acc_miss, n);
			#define POSD_MS(q) (unsigned long)((q)/10), (unsigned long)((q)%10)
			snprintf(lines[3], 32, "B %lu.%lu L %lu.%lu", POSD_MS(b), POSD_MS(l));
			snprintf(lines[4], 32, "R %lu.%lu", POSD_MS(r));
			uint32_t q = posd_ms10(posd_acc_look, n), e = posd_ms10(posd_acc_emit, n);
			snprintf(lines[5], 32, "Q %lu.%lu E %lu.%lu", POSD_MS(q), POSD_MS(e));
			snprintf(lines[6], 32, "C %lu.%lu N %lu", POSD_MS(c), (unsigned long)(posd_acc_tiles / n));
			snprintf(lines[7], 32, "G %lu H %lu", (unsigned long)(posd_acc_copyt / n), (unsigned long)(posd_acc_flipt / n));
			snprintf(lines[8], 32, "A %lu.%lu X %lu.%lu", POSD_MS(a), POSD_MS(x));
			snprintf(lines[9], 32, "P %lu.%lu T %lu.%lu", POSD_MS(p), POSD_MS(t));
			#undef POSD_MS
			posd_render(lines, 10);
			static bool mem_logged;
			if (!mem_logged) {   // RDRAM left above the heap (sizes the caches)
				extern void *sbrk(intptr_t);
				char *top = (char *)sbrk(0);
				plat_log("[PERFOSD-MEM] ram=%d heaptop=%p free=%d\n", get_memory_size(), top,
				         (int)((char *)0x80000000 + get_memory_size() - top));
				mem_logged = true;
			}
			plat_log("[PERFOSD] f=%d f10=%lu g10=%lu m=%lu s=%lu v=%lu w=%lu a=%lu x=%lu p=%lu t=%lu b=%lu r=%lu l=%lu c=%lu q=%lu e=%lu n=%lu g=%lu h=%lu\n",
			         g_frame, (unsigned long)f10, (unsigned long)g10, (unsigned long)m,
			         (unsigned long)s, (unsigned long)v, (unsigned long)w, (unsigned long)a,
			         (unsigned long)x, (unsigned long)p, (unsigned long)t,
			         (unsigned long)b, (unsigned long)r, (unsigned long)l,
			         (unsigned long)c, (unsigned long)q,
			         (unsigned long)e, (unsigned long)(posd_acc_tiles / n),
			         (unsigned long)(posd_acc_copyt / n), (unsigned long)(posd_acc_flipt / n));
		}
		tick0 = now; gf0 = g_frame; accn = 0;
		acc_pipe = acc_tmem = 0;
		posd_acc_all = posd_acc_m68k = posd_acc_snd = posd_acc_draw = posd_acc_wait = 0;
		posd_acc_beg = posd_acc_spr = posd_acc_fix = posd_acc_miss = 0;
		posd_acc_look = posd_acc_emit = posd_acc_tiles = posd_acc_copyt = posd_acc_flipt = 0;
		posd_acc_n = 0;
	}
	if (posd_cur >= 0) {
		rdpq_set_mode_copy(true);
		rdpq_tex_blit(&posd_surf[posd_cur], 8, 64, NULL);   // below the HUD
	}
}
#endif
