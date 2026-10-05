#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#ifndef N64
#include <stdlib.h>
#endif
#include "emu.h"
#ifdef USE_M64K
#include "m64k/m64k.h"
#else
#include "m68k.h"
#endif
#include "hw.h"
#include "video.h"
#include "roms.h"
#include "platform.h"
#include "sound.h"

static int cpu_trace_count = 0;
void cpu_trace(unsigned int pc) {
	(void)cpu_trace_count;
	#ifndef N64
	if (cpu_trace_count == 0) {
		m68k_set_instr_hook_callback(NULL);
		return;
	}

	char inst[1024];
	m68k_disassemble(inst, pc, M68K_CPU_TYPE_68000);
	debugf("trace: %06x %-30s", pc, inst);

	if (strstr(inst, "A0")) debugf("A0=%08x ", m68k_get_reg(NULL, M68K_REG_A0));
	if (strstr(inst, "A1")) debugf("A1=%08x ", m68k_get_reg(NULL, M68K_REG_A1));
	if (strstr(inst, "A2")) debugf("A2=%08x ", m68k_get_reg(NULL, M68K_REG_A2));
	if (strstr(inst, "A3")) debugf("A3=%08x ", m68k_get_reg(NULL, M68K_REG_A3));
	if (strstr(inst, "A4")) debugf("A4=%08x ", m68k_get_reg(NULL, M68K_REG_A4));
	if (strstr(inst, "A5")) debugf("A5=%08x ", m68k_get_reg(NULL, M68K_REG_A5));
	if (strstr(inst, "A6")) debugf("A6=%08x ", m68k_get_reg(NULL, M68K_REG_A6));
	if (strstr(inst, "A7")) debugf("A7=%08x ", m68k_get_reg(NULL, M68K_REG_A7));
	if (strstr(inst, "D0")) debugf("D0=%08x ", m68k_get_reg(NULL, M68K_REG_D0));
	if (strstr(inst, "D1")) debugf("D1=%08x ", m68k_get_reg(NULL, M68K_REG_D1));
	if (strstr(inst, "D2")) debugf("D2=%08x ", m68k_get_reg(NULL, M68K_REG_D2));
	if (strstr(inst, "D3")) debugf("D3=%08x ", m68k_get_reg(NULL, M68K_REG_D3));
	if (strstr(inst, "D4")) debugf("D4=%08x ", m68k_get_reg(NULL, M68K_REG_D4));
	if (strstr(inst, "D5")) debugf("D5=%08x ", m68k_get_reg(NULL, M68K_REG_D5));
	if (strstr(inst, "D6")) debugf("D6=%08x ", m68k_get_reg(NULL, M68K_REG_D6));
	if (strstr(inst, "D7")) debugf("D7=%08x ", m68k_get_reg(NULL, M68K_REG_D7));

	debugf("\n");

	cpu_trace_count--;
	#endif
}

void cpu_start_trace(int cnt) {
	#ifndef N64
	m68k_set_instr_hook_callback(cpu_trace);
	#endif
	cpu_trace_count = cnt;
}

int g_frame;   // guest frame counter (non-static: the IOLOG gate in hw.c
               // needs a GUEST-aligned key; N64_FRAME is the host VI count
               // and skews with wall speed)

#ifndef N64
// --- Headless scripted input ---------------------------------------------
// Lets the PC emu drive menus/gameplay with the human out of the loop. The
// script is a text file (env MVS64_INPUT) of lines: "<f0> <f1> <key>", meaning
// hold <key> from frame f0 to f1 inclusive. <key> is one of:
//   coin start select a b c d up down left right
// keystate is reassigned to point at hl_keys so input.c reads our buffer.
extern const uint8_t *keystate;
static uint8_t hl_keys[512];
#define HL_MAX_EVENTS 256
static struct { int f0, f1, sc; } hl_script[HL_MAX_EVENTS];
static int hl_nevents;

static int hl_keyname_to_sc(const char *n) {
	if (!strcmp(n, "coin"))   return PLAT_KEY_COIN_1;
	if (!strcmp(n, "start"))  return PLAT_KEY_P1_START;
	if (!strcmp(n, "select")) return PLAT_KEY_P1_SELECT;
	if (!strcmp(n, "a"))      return PLAT_KEY_P1_A;
	if (!strcmp(n, "b"))      return PLAT_KEY_P1_B;
	if (!strcmp(n, "c"))      return PLAT_KEY_P1_C;
	if (!strcmp(n, "d"))      return PLAT_KEY_P1_D;
	if (!strcmp(n, "up"))     return PLAT_KEY_P1_UP;
	if (!strcmp(n, "down"))   return PLAT_KEY_P1_DOWN;
	if (!strcmp(n, "left"))   return PLAT_KEY_P1_LEFT;
	if (!strcmp(n, "right"))  return PLAT_KEY_P1_RIGHT;
	return -1;
}

static void hl_load_script(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) { fprintf(stderr, "[INPUT] cannot open %s\n", path); return; }
	char line[128], key[32];
	int f0, f1;
	while (fgets(line, sizeof(line), f)) {
		if (line[0] == '#' || line[0] == '\n') continue;
		if (sscanf(line, "%d %d %31s", &f0, &f1, key) == 3) {
			int sc = hl_keyname_to_sc(key);
			if (sc < 0) { fprintf(stderr, "[INPUT] bad key '%s'\n", key); continue; }
			if (hl_nevents < HL_MAX_EVENTS)
				hl_script[hl_nevents++] = (typeof(hl_script[0])){ f0, f1, sc };
		}
	}
	fclose(f);
	fprintf(stderr, "[INPUT] loaded %d events from %s\n", hl_nevents, path);
}

static void hl_apply_input(int frame) {
	memset(hl_keys, 0, sizeof(hl_keys));
	for (int i = 0; i < hl_nevents; i++)
		if (frame >= hl_script[i].f0 && frame <= hl_script[i].f1)
			hl_keys[hl_script[i].sc] = 1;
}

// --- Headless WAV capture (16-bit signed stereo, little-endian host) ---------
// Lets me validate generated audio (RMS/FFT on the .wav) with no speakers and
// no human in the loop. Enabled by env MVS64_WAV=<path>.
static FILE *wav_fp;
static uint32_t wav_data_bytes;
static int wav_freq;

static void wav_open(const char *path, int freq) {
	wav_fp = fopen(path, "wb");
	if (!wav_fp) { fprintf(stderr, "[WAV] cannot open %s\n", path); return; }
	wav_freq = freq;
	wav_data_bytes = 0;
	uint8_t hdr[44] = {0};
	fwrite(hdr, 1, sizeof(hdr), wav_fp);   // placeholder, patched in wav_close()
	fprintf(stderr, "[WAV] capturing to %s (%d Hz, 16-bit stereo)\n", path, freq);
}

static void wav_write(const int16_t *stereo, int nframes) {
	if (!wav_fp) return;
	fwrite(stereo, sizeof(int16_t) * 2, (size_t)nframes, wav_fp);
	wav_data_bytes += (uint32_t)nframes * 2 * sizeof(int16_t);
}

static void wav_close(void) {
	if (!wav_fp) return;
	uint32_t riff = 36 + wav_data_bytes, fmtlen = 16, byterate = (uint32_t)wav_freq * 4;
	uint16_t fmt = 1, ch = 2, bits = 16, blockalign = 4;
	uint32_t freq = (uint32_t)wav_freq;
	fseek(wav_fp, 0, SEEK_SET);
	fwrite("RIFF", 1, 4, wav_fp); fwrite(&riff, 4, 1, wav_fp); fwrite("WAVE", 1, 4, wav_fp);
	fwrite("fmt ", 1, 4, wav_fp); fwrite(&fmtlen, 4, 1, wav_fp);
	fwrite(&fmt, 2, 1, wav_fp); fwrite(&ch, 2, 1, wav_fp); fwrite(&freq, 4, 1, wav_fp);
	fwrite(&byterate, 4, 1, wav_fp); fwrite(&blockalign, 2, 1, wav_fp); fwrite(&bits, 2, 1, wav_fp);
	fwrite("data", 1, 4, wav_fp); fwrite(&wav_data_bytes, 4, 1, wav_fp);
	fclose(wav_fp); wav_fp = NULL;
}

#define AUDIO_FREQ MVS64_AUDIO_RATE
// Sized for MVS64_SIMFPS as low as 5fps (AUDIO_FREQ/5 samples/frame); see emu loop.
static int16_t audio_frame[(AUDIO_FREQ / 5 + 16) * 2];
#endif
#ifdef USE_M64K
extern m64k_t m64k;   // allocated in m64k_asm.S, next to the dispatch tables
#endif
#ifdef MVS64_LAYOUT_PAD
// Layout-sensitivity rig: shifts .rodata and everything linked after it
// (.data, .sdata, .sbss) by MVS64_LAYOUT_PAD bytes, to check that a speed
// result does not depend on where the data happens to land in the dcache.
__attribute__((used)) const char mvs64_layout_pad[MVS64_LAYOUT_PAD] = {1};
#endif
#if defined(N64) && defined(USE_M64K) && defined(M64K_DYNREC)
// Boot-time proof that hw_n64.S's EPC-range checks accept dynarec-arena
// addresses (a non-negotiable dynarec law: MMIO from translated code must
// keep mid-slice clock accuracy and honor slice-break clamps). Publishes a
// tiny stub into the arena ("lbu v0, 0(a2); jr ra") and calls it via the
// hw_n64.S thunk with a1 = sentinel and a2 = an MMIO address (REG_P1CNT,
// a pure input-port read). The stub's load TLB-faults with EPC inside the
// arena; on success the handler must have (1) refreshed ts_cur from a1 and
// (2) consumed slice_break: banked a1 into forced_remaining and clamped the
// saved a1 to 0.
static void m64k_dyntest(m64k_t *ctx)
{
	extern uint8_t __m64k_dyn_arena[];
	extern void __m64k_dyn_publish(void *dst, const void *src, int len);
	extern uint32_t __m64k_dyntest_thunk(uint32_t unused, uint32_t a1_sentinel, uint32_t mmio_addr);
	extern m64k_t *__m64k_live;

	// The load MUST target t0: the handler's SAFE_MODE check (tlb_readhwio)
	// enforces the interpreter's canonical loads->t0 convention and bails to
	// the crash screen for any other RT — emitted code is bound by the same
	// law (first [DYNTEST] run proved the check fires: an lbu into v0 died).
	static const uint32_t stub[4] = {
		0x90C80000,  // lbu t0, 0(a2)   <- TLB-faults: EPC is arena-resident
		0x03E00008,  // jr ra
		0x00000000,  // nop (delay slot)
		0x00000000,
	};
	__m64k_dyn_publish(__m64k_dyn_arena, stub, sizeof(stub));

	const uint32_t sent = 0x00123456;
	ctx->forced_remaining = 0;
	ctx->ts_cur = 0xDEAD0001;
	ctx->slice_break = 1;
	__m64k_live = ctx;
	uint32_t a1_after = __m64k_dyntest_thunk(0, sent, 0xFF300000);
	__m64k_live = NULL;

	bool ok_tscur = (ctx->ts_cur == sent);
	bool ok_clamp = (ctx->forced_remaining == (int32_t)sent) && (a1_after == 0);
	bool ok_break = (ctx->slice_break == 0);
	debugf("[DYNTEST] arena EPC accept: ts_cur=%s clamp=%s break=%s -> %s\n",
		ok_tscur ? "ok" : "FAIL", ok_clamp ? "ok" : "FAIL",
		ok_break ? "ok" : "FAIL",
		(ok_tscur && ok_clamp && ok_break) ? "PASS" : "FAIL");

	ctx->forced_remaining = 0;
	ctx->ts_cur = 0;
	ctx->slice_break = 0;
}
#endif
static uint64_t g_clock, g_clock_framebegin;
static uint64_t m68k_clock;
static EmuEvent events[MAX_EVENTS];
uint32_t profile_hw_io;
uint32_t profile_dma_load;
uint32_t profile_m68k;   // ticks inside the 68k core this frame (incl. MMIO)
uint32_t profile_snd;    // ticks synthesizing audio (Z80+YM2610) this frame
#ifdef MVS64_PERFCOUNT
// Draw-bucket split (see emu_render): framebuffer-wait vs command issue vs
// detach. Diagnostic builds only.
uint32_t perf_draw_wait, perf_draw_issue, perf_draw_end;
#endif
#ifdef MVS64_OPHIST
// Exact per-opcode execution histogram; bumped per dispatched instruction
// in m64k_asm.S (m64k_ophist_ptr points here). 256KB, diagnostic only.
uint32_t m64k_ophist_tab[65536] __attribute__((aligned(16)));
#endif
#ifdef M64K_DYNSTAT
// Dynarec coverage rig (m64k_asm.S dynstat_count): per-slot counts of
// control-transfer targets seen at jmp_exec (= the dynarec probe's exact
// visibility) + last-writer PC per slot for collision detection. 512KB,
// diagnostic builds only. Dumped+reset every DYNSTAT_WINDOW frames.
uint32_t m64k_dynstat_cnt[65536] __attribute__((aligned(16)));
uint32_t m64k_dynstat_pc[65536] __attribute__((aligned(16)));
#endif
#ifdef MVS64_PERFCOUNT
// m64k_run entries this frame: sizes the per-slice constant cost (icache
// re-entry, register save/restore) vs the per-instruction marginal cost.
uint32_t perf_m68k_slices;
#endif

static uint64_t m68k_exec(uint64_t clock) {
	clock /= M68K_CLOCK_DIV;
	if (clock > m68k_clock) {
		#ifdef USE_M64K
		#ifdef N64
		uint32_t t0 = TICKS_READ();
		#ifdef MVS64_PERFCOUNT
		perf_m68k_slices++;
		#endif
		m68k_clock = m64k_run(&m64k, clock);
		profile_m68k += TICKS_DISTANCE(t0, TICKS_READ());
		#else
		m68k_clock = m64k_run(&m64k, clock);
		#endif
		#else
		m68k_clock += m68k_execute(clock - m68k_clock);
		#endif
	}
	return m68k_clock * M68K_CLOCK_DIV;
}


// Return the next event that must be executed
static EmuEvent* next_event() {
    EmuEvent *e = NULL;
    for (int i=0;i<MAX_EVENTS;i++) {
        if (!events[i].cb) continue;
        if (!e || events[i].clock < e->clock) e=&events[i];
    }
    return e;
}

int emu_add_event(int64_t clock, EmuEventCb cb, void *cbarg) {
    for (int i=0;i<MAX_EVENTS;i++) {
        if (events[i].cb) continue;
        events[i].clock = clock;
        events[i].cb = cb;
        events[i].cbarg = cbarg;
        events[i].current = false;
        return i;
    }
    assert(0);
}

void emu_change_event(int event_id, int64_t newclock) {
	events[event_id].clock = newclock;
	if (events[event_id].current) {
		#ifdef USE_M64K
		m64k_run_stop(&m64k);
		#else
		m68k_end_timeslice();
		#endif
	}
}

int64_t emu_clock(void) {
	#ifdef USE_M64K
	return m64k_get_clock(&m64k) * M68K_CLOCK_DIV;
	#else
	return g_clock + m68k_cycles_run() * M68K_CLOCK_DIV;
	#endif
}

int64_t emu_clock_frame(void) {
	return emu_clock() - g_clock_framebegin;
}

void emu_cpu_reset(void) {
	#ifdef USE_M64K
	m64k_pulse_reset(&m64k);
	#else
	m68k_pulse_reset();
	#endif
}

uint32_t emu_pc(void) {
	#ifdef USE_M64K
	return m64k_get_pc(&m64k) & 0xFFFFFF;
	#else
	return m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
	#endif
}

void emu_cpu_irq(int irq, bool on) {
	#ifdef USE_M64K
	m64k_set_virq(&m64k, irq, on);
	#else
	m68k_set_virq(irq, on);
	#endif
}

#ifdef USE_M64K
int cpu_irqack(void *ctx, int level)
{
	// On NeoGeo hardware, interrupts must be manually acknowledged via a write
	// to register 0x3C000C. So we do nothing here.
	// NOTE: we still must register this hook, otherwise the m64k core will
	// by default auto-acnowledge the interrupts.
	return 0;
}
#endif

uint32_t emu_vblank_start(void* arg) {
	emu_cpu_irq(1, true);
	hw_vblank();
	framef("[EMU] VBlank - clock:%lld clock_frame:%lld\n", (long long)emu_clock(), (long long)emu_clock_frame());
	return FRAME_CLOCK;
}

uint32_t render_time;

#ifdef N64
// Auto frameskip (make ... FRAMESKIP=n, i.e. -DMVS64_FRAMESKIP=n; 0 = off).
// When emulation is behind the VI clock (N64_FRAME counts VIs, g_frame
// guest frames), skip DRAWING up to n frames in a row so the game logic
// keeps full speed instead of running in slow motion. The 68k, Z80 and
// audio run every frame either way; only the draw is dropped. After n
// skips the next frame always draws, and if it is still behind the lag
// is forgiven (N64_FRAME resynced) — never a catch-up sprint after a
// heavy scene. n=1 keeps the display at >= half the emulated rate.
// Runtime twin knob, pinned to .data so the OFF and ON binaries are
// layout-identical (twin-knob law: a 0 initializer would land in .sbss).
#ifndef MVS64_FRAMESKIP
#define MVS64_FRAMESKIP 0
#endif
int mvs64_fskip_max __attribute__((section(".data"))) = MVS64_FRAMESKIP;
uint32_t fskip_drawn, fskip_skipped;   // per-window counters ([FSKIP] line)
#endif

uint32_t emu_render(void *arg) {

	#ifdef N64
	if (mvs64_fskip_max > 0) {
		extern volatile int N64_FRAME;
		static int skip_run = 0;

		if (N64_FRAME > g_frame) {
			if (skip_run < mvs64_fskip_max) {
				skip_run++;
				fskip_skipped++;
				render_time = 0;
				plat_audio_pump();   // audio pumps once per frame regardless
				return FRAME_CLOCK;
			}
			// Drawing this one after a full skip run: forgive the lag.
			disable_interrupts();
			N64_FRAME = g_frame;
			enable_interrupts();
		}
		skip_run = 0;
	}
	#endif

	if (CONFIG_FRAMESKIP_MODE == 1) {
		if (g_frame & 1) {
			debugf("[RENDER] skip frame\n");
			#ifdef N64
			plat_audio_pump();   // audio pumps once per frame regardless
			#endif
			return FRAME_CLOCK;
		}
	}

	framef("[RENDER] render\n");
	#ifdef N64
	fskip_drawn++;
	uint32_t t0 = TICKS_READ();
	#endif
	#if defined(N64) && defined(MVS64_PERFCOUNT)
	// Split the draw bucket: display_get/attach wait vs command issue vs
	// detach — tells whether draw% is CPU work or RSP/RDP back-pressure.
	extern uint32_t perf_draw_wait, perf_draw_issue, perf_draw_end;
	plat_beginframe();
	perf_draw_wait = TICKS_DISTANCE(t0, TICKS_READ());
	uint32_t t1 = TICKS_READ();
	video_render();
	perf_draw_issue = TICKS_DISTANCE(t1, TICKS_READ());
	uint32_t t2 = TICKS_READ();
	plat_endframe();
	perf_draw_end = TICKS_DISTANCE(t2, TICKS_READ());
	#else
	plat_beginframe();
	video_render();
	plat_endframe();
	#endif

	rom_next_frame();

	#ifdef N64
	render_time = TICKS_DISTANCE(t0, TICKS_READ());

	// Pump the audio HERE, right after the frame's draw commands were
	// issued — not at the end of the main loop. The whole-pump offload
	// bursts ~12-16ms of RSP work per pump; pumped at loop end, that
	// burst was still draining when the NEXT frame's render (this event,
	// at line 24) issued its commands, and the CPU ate it as rspq
	// back-pressure (measured 60-76% of the frame in fights, tiles/frame
	// constant). Pumped here, the RSP finishes the (fast) video queue
	// first and chews the audio under the remaining ~90% of the frame's
	// 68k work, so the next render meets a drained queue.
	plat_audio_pump();
	#endif

	return FRAME_CLOCK;
}

void emu_run_frame(void) {
    uint64_t vsync = g_clock_framebegin + FRAME_CLOCK;
    EmuEvent *e;

    // Run all events that are scheduled before next vsync
    while ((e = next_event()) && (e->clock < vsync)) {
    	e->current = true;
        g_clock = m68k_exec(e->clock);
        e->current = false;

        // Call the event callback, and check if it must be repeated.
        if (g_clock >= e->clock) {
	        uint32_t repeat = e->cb(e->cbarg);
	        if (repeat != 0) e->clock += repeat;
	        else e->cb = NULL;
        }
    }

    while (g_clock < vsync)
    	g_clock = m68k_exec(vsync);

    // Frame completed
	framef("[EMU] Frame completed: %d (vsync: %llu)\n", g_frame, (unsigned long long)vsync);
    g_frame++;
	g_clock_framebegin += FRAME_CLOCK;
}

int main(int argc, char *argv[]) {
	#ifndef N64
	if (argc < 2) {
		fprintf(stderr, "Usage:\n    mvs64 <romdir>\n");
		return 1;
	}
	#else
	argc = 0; argv = NULL;
	#endif

	plat_init(MVS64_AUDIO_RATE, FPS);
	#if defined(MVS64_PCPROF) && defined(N64)
	{ extern void pcprof_init(void); pcprof_init(); }
	#endif

	#ifndef N64
	// Headless test harness: when MVS64_FRAMES=N is set, run N frames with no
	// SDL window (use SDL_VIDEODRIVER=dummy / SDL_AUDIODRIVER=dummy), dumping a
	// screenshot every MVS64_SHOT frames and the 68K PC each second, then exit.
	// Lets us validate boot progression with the human out of the loop.
	const char *hl_env = getenv("MVS64_FRAMES");
	int headless = hl_env ? atoi(hl_env) : 0;
	const char *shot_env = getenv("MVS64_SHOT");
	int shot_interval = shot_env ? atoi(shot_env) : 0;
	const char *sndchunk_env = getenv("MVS64_SNDCHUNK");
	int sndchunk = sndchunk_env ? atoi(sndchunk_env) : 0;
	// MVS64_SIMFPS=N reproduces the N64's REALTIME audio decoupling on the PC
	// headless harness: the N64 AI drains at the wall-clock rate, so when the 68k
	// loop runs at N fps it generates AUDIO_FREQ/N samples per LOGIC frame (more
	// than the 1/60s the game logic assumes), desyncing 68k-driven sound events
	// from the music. Setting this < 60 mimics a slow N64 so we can repro the
	// in-combat stuck note without hardware. 0/unset = faithful 60fps lock.
	const char *simfps_env = getenv("MVS64_SIMFPS");
	int simfps = simfps_env ? atoi(simfps_env) : 0;
	const int spf = AUDIO_FREQ / (simfps > 0 ? simfps : FPS); // samples per video frame
	if (headless) {
		keystate = hl_keys;                 // drive input from our scripted buffer
		const char *script = getenv("MVS64_INPUT");
		if (script) hl_load_script(script);
		const char *wav = getenv("MVS64_WAV");
		if (wav) wav_open(wav, AUDIO_FREQ);
	} else {
		plat_enable_audio(1);               // start SDL playback for interactive use
	}
	plat_enable_video(headless ? false : true);
	#else
	plat_enable_video(true);
	plat_enable_audio(1);
	#endif

	#ifdef N64
	rom_load("rom:/");
	#else
	rom_load(argv[1]);
	#endif

	#ifdef USE_M64K
	m64k_init(&m64k);
	m64k_set_hook_irqack(&m64k, cpu_irqack, NULL);
	#else
	m68k_init();
	#endif

	hw_init();
	g_clock = 0;

	#ifdef USE_M64K
	m64k_pulse_reset(&m64k);
	#if defined(N64) && defined(M64K_DYNREC)
	m64k_dyntest(&m64k);
	#endif
	#else
	m68k_set_cpu_type(M68K_CPU_TYPE_68000);
	m68k_pulse_reset();
	#endif
	m68k_clock = 0;

#ifdef MVS64_LAYOUT_PAD
	__asm__ volatile("" :: "r"(mvs64_layout_pad));   // keep it past --gc-sections
#endif
	emu_add_event(LINE_CLOCK*24,  emu_render, NULL);
	emu_add_event(LINE_CLOCK*248, emu_vblank_start, NULL);

	#ifdef N64
	uint32_t fps_frame = 0;
	uint32_t fps_time = TICKS_READ();
	#endif
	while (1) {
		render_time = 0;
		profile_hw_io = 0;
		profile_dma_load = 0;
		profile_m68k = 0;
		profile_snd = 0;
		#ifdef N64
		uint32_t t0 = TICKS_READ();
		#endif
		#ifndef N64
		if (headless) hl_apply_input(g_frame);
		#endif

		emu_run_frame();
		if (!plat_poll()) break;

		#ifndef N64
		// Produce one video-frame's worth of audio through the sound seam.
		if (headless) {
			int n;
			if (sndchunk > 0) {
				// Validation: produce spf samples in arbitrary sub-chunks to
				// exercise the rate-agnostic sound_gen_samples() path the N64
				// AI pump uses (it asks for ~1764 at a time). Concatenation must
				// be identical music to a single spf call (proves Rank 1 math).
				int off = 0;
				while (off < spf) {
					int c = spf - off; if (c > sndchunk) c = sndchunk;
					sound_gen_samples(audio_frame + off * 2, c);
					off += c;
				}
				n = spf;
			} else {
				n = sound_gen_samples(audio_frame, spf);
			}
			wav_write(audio_frame, n);
		} else {
			int16_t *abuf; int an;
			plat_beginaudio(&abuf, &an);
			sound_gen_samples(abuf, an);
			plat_endaudio();
		}
		#endif

		#ifdef N64
		// The audio pump moved into emu_render (right after the draw
		// commands are issued): pumped here at loop end, the RSP audio
		// burst was still draining when the next frame's render issued
		// its commands, and the CPU ate it as rspq back-pressure. See
		// the comment in emu_render; sound_gen_samples() is rate-
		// agnostic and the pump is wall-clock driven, so the phase
		// shift within the frame does not affect audio timing.
		#endif

		#ifndef N64
		if (headless) {
			if (shot_interval && (g_frame % shot_interval) == 0) {
				char fn[64];
				sprintf(fn, "shot_%05d.bmp", g_frame);
				plat_save_screenshot(fn);
			}
			if ((g_frame % 60) == 0)
				fprintf(stderr, "[HEADLESS] frame %d  PC=%06x\n",
					g_frame, (uint32_t)m68k_get_reg(NULL, M68K_REG_PC));
			if (g_frame >= headless) break;
		}
		#endif

		#ifdef N64
		uint32_t emu_time = TICKS_DISTANCE(t0, TICKS_READ());
		#ifdef MVS64_PERFOSD
		{
			extern void plat_perf_frame(uint32_t all, uint32_t m68k, uint32_t snd, uint32_t draw);
			plat_perf_frame(emu_time, profile_m68k, profile_snd, render_time);
		}
		#endif

		framef("[PROFILE] cpu:%.2f%% m68k:%.2f%% snd:%.2f%% io:%.2f%% draw:%.2f%% dma:%.2f%% PC:%06lx\n",
			(float)emu_time * 100.f / (float)(TICKS_PER_SECOND / 60),
			(float)profile_m68k * 100.f / (float)(TICKS_PER_SECOND / 60),
			(float)profile_snd * 100.f / (float)(TICKS_PER_SECOND / 60),
			(float)profile_hw_io * 100.f / (float)(TICKS_PER_SECOND / 60),
			(float)render_time * 100.f / (float)(TICKS_PER_SECOND / 60),
			(float)profile_dma_load * 100.f / (float)(TICKS_PER_SECOND / 60),
			#ifdef USE_M64K
			m64k_get_pc(&m64k));
			#else
			(uint32_t)m68k_get_reg(NULL, M68K_REG_PC));
			#endif
		#if defined(MVS64_PCPROF) && defined(USE_M64K)
		{
			extern void pcprof_frame(int frame, int in_fight);
			uint32_t fpc = m64k_get_pc(&m64k) & 0xFFFFFF;
			pcprof_frame(g_frame, fpc == 0x3200 || fpc == 0x31fe);
		}
		#endif
		#ifdef M64K_DYNSTAT
		#define DYNSTAT_WINDOW 600
		if (g_frame && (g_frame % DYNSTAT_WINDOW) == 0) {
			// One pass: total + PC-region aggregates (the blueprint's
			// P_ROM / WORK_RAM / PBROM-window split — decides whether
			// bank-keyed translation must be pulled forward) and the
			// top-16 hottest control-transfer targets.
			uint64_t total = 0;
			uint32_t reg_prom = 0, reg_ram = 0, reg_pbrom = 0, reg_bios = 0, reg_other = 0;
			enum { DYNH_N = 64 };   // wide list: cross-ref with [DYNTERM]
			int top[DYNH_N]; int ntop = 0;
			for (int i = 0; i < 65536; i++) {
				uint32_t n = m64k_dynstat_cnt[i];
				if (!n) continue;
				total += n;
				uint32_t pc = m64k_dynstat_pc[i] & 0xFFFFFF;
				if      (pc < 0x100000) reg_prom  += n;
				else if (pc < 0x200000) reg_ram   += n;
				else if (pc < 0x300000) reg_pbrom += n;
				else if (pc >= 0xC00000 && pc < 0xC20000) reg_bios += n;
				else reg_other += n;
				int j = ntop;
				while (j > 0 && m64k_dynstat_cnt[top[j-1]] < n) j--;
				if (j < DYNH_N) {
					if (ntop < DYNH_N) ntop++;
					for (int k = ntop - 1; k > j; k--) top[k] = top[k-1];
					top[j] = i;
				}
			}
			framef("[DYNSTAT] win=%d total=%llu prom=%lu ram=%lu pbrom=%lu bios=%lu other=%lu\n",
				DYNSTAT_WINDOW, (unsigned long long)total,
				(unsigned long)reg_prom, (unsigned long)reg_ram,
				(unsigned long)reg_pbrom, (unsigned long)reg_bios,
				(unsigned long)reg_other);
			for (int i = 0; i < ntop; i++)
				framef("[DYNH] pc=%06lx n=%lu\n",
					(unsigned long)(m64k_dynstat_pc[top[i]] & 0xFFFFFF),
					(unsigned long)m64k_dynstat_cnt[top[i]]);
			memset(m64k_dynstat_cnt, 0, sizeof(m64k_dynstat_cnt));
			#ifdef M64K_DYNREC
			{
				// Dynarec density: cumulative translations/chains — the
				// coverage levers phase-3 escalations are gated on.
				extern uint32_t __m64k_dyn_stat_blocks, __m64k_dyn_stat_insns;
				extern uint32_t __m64k_dyn_stat_chains, __m64k_dyn_stat_refused;
				// exec = guest insns EXECUTED inside blocks this window
				// (emitted counter). exec/DYNSTAT_WINDOW vs the ~8-11k
				// insns/frame the interpreter dispatches is the residency
				// fraction — the only coverage number that predicts fps.
				extern uint32_t __m64k_dyn_stat_exec;
				framef("[DYNSTAT2] blocks=%lu insns=%lu chains=%lu refused=%lu exec=%lu execpf=%lu\n",
					(unsigned long)__m64k_dyn_stat_blocks,
					(unsigned long)__m64k_dyn_stat_insns,
					(unsigned long)__m64k_dyn_stat_chains,
					(unsigned long)__m64k_dyn_stat_refused,
					(unsigned long)__m64k_dyn_stat_exec,
					(unsigned long)(__m64k_dyn_stat_exec / DYNSTAT_WINDOW));
				__m64k_dyn_stat_exec = 0;
			}
			#endif
		}
		#endif
		#if defined(M64K_BLOCKOPS) && (defined(M64K_DYNSTAT) || defined(MVS64_BOSTAT))
		// Own window: the DYNSTAT rig costs a jal per control transfer and
		// runs ~3x slower, which stops the autoinput harness reaching a
		// fight at all — so blockop accounting must be usable without it.
		if (g_frame && (g_frame % 600) == 0) {
			{
				// Why the fused VRAM-port copy declines: a rejected
				// blockop is silent everywhere else (the loop just runs
				// interpreted, one TLB exception per word).
				extern uint32_t blockop_fire, blockop_rej_dst;
				extern uint32_t blockop_rej_bud, blockop_rej_span;
				extern uint32_t blockop_rej_dstval, blockop_rej_shape;
				extern uint32_t blockop_seen, blockop_t3val, blockop_t9val;
				extern uint32_t blockop_rej_pc;
				framef("[BOSTAT] seen=%lu shape=%lu fire=%lu rej_dst=%lu rej_bud=%lu"
				       " rej_span=%lu t3=%ld body=%04lx dstval=%06lx rejpc=%06lx\n",
					(unsigned long)blockop_seen,
					(unsigned long)blockop_rej_shape,
					(unsigned long)blockop_fire,
					(unsigned long)blockop_rej_dst,
					(unsigned long)blockop_rej_bud,
					(unsigned long)blockop_rej_span,
					(long)(int32_t)blockop_t3val,
					(unsigned long)blockop_t9val,
					(unsigned long)blockop_rej_dstval,
					(unsigned long)(blockop_rej_pc & 0xFFFFFF));
				blockop_fire = blockop_rej_dst = 0;
				blockop_rej_bud = blockop_rej_span = 0;
				blockop_seen = blockop_rej_shape = 0;
			}
		}
		#endif
		#ifdef M64K_TRACECRC
		{
			// Per-frame 68k state-trace hash (dynarec bit-exactness rig,
			// m64k.c): two runs of the same build+inputs must emit identical
			// [TRCRC] streams; a dynarec build must match the interpreter's.
			extern uint32_t __m64k_tracecrc, __m64k_tracecrc_slices;
			framef("[TRCRC] f=%d crc=%08lx slices=%lu\n", g_frame,
				(unsigned long)__m64k_tracecrc,
				(unsigned long)__m64k_tracecrc_slices);
			__m64k_tracecrc = 2166136261u;
			__m64k_tracecrc_slices = 0;
			#ifdef M64K_TRCRC_SPLIT
			{
				// Content-only hash (regs/SR, no pc/cycles): separates
				// timing displacement from real state divergence.
				extern uint32_t __m64k_tracecrc_content;
				framef("[TRCCON] f=%d crc=%08lx\n", g_frame,
					(unsigned long)__m64k_tracecrc_content);
				__m64k_tracecrc_content = 2166136261u;
			}
			#endif
		}
		#endif
		#ifdef MVS64_PERFCOUNT
		{
			// Diagnostic counters (m64k_asm.S / hw_n64.S): executed 68k
			// instructions, idle-skip fires and TLB exceptions this frame,
			// plus the draw-bucket split from emu_render (in 0.01%-of-frame
			// units to stay integer: 100.00% == 10000).
			extern uint32_t perf_m68k_insns, perf_idle_skips, perf_tlb_faults;
			extern uint32_t perf_draw_wait, perf_draw_issue, perf_draw_end;
			extern uint32_t perf_snd_pub;
			const uint32_t fb = TICKS_PER_SECOND / 60 / 10000;  // ticks per 0.01%
			{
				// Trap histogram by 64KB page and direction (hw_n64.S):
				// zeroed at the fight-era frame 2400, dumped at 6600 as
				// traps per frame x100.
				extern uint32_t perf_trap_hist[512];
				extern uint32_t perf_trap_ring[2048], perf_trap_ring_n;
				if (g_frame == 2400) {
					memset(perf_trap_hist, 0, sizeof(uint32_t) * 512);
					perf_trap_ring_n = 0;
				}
				if (g_frame == 6600) {
					for (uint32_t i = 0; i < perf_trap_ring_n; i++)
						debugf("[TRAPS] epc=%08lx pc=%06lx\n",
						       (unsigned long)perf_trap_ring[2*i],
						       (unsigned long)(perf_trap_ring[2*i+1] & 0xFFFFFF));
					for (int i = 0; i < 512; i++)
						if (perf_trap_hist[i])
							debugf("[TRAPH] %s page=%02x per_frame_x100=%lu\n",
							       i >= 256 ? "W" : "R", i & 255,
							       (unsigned long)(perf_trap_hist[i] * 100UL / 4200));
				}
			}
			framef("[PERF] insns=%lu skips=%lu tlb=%lu slices=%lu dwait=%lu dissue=%lu dend=%lu pub=%lu\n",
				(unsigned long)perf_m68k_insns,
				(unsigned long)perf_idle_skips,
				(unsigned long)perf_tlb_faults,
				(unsigned long)perf_m68k_slices,
				(unsigned long)(perf_draw_wait / fb),
				(unsigned long)(perf_draw_issue / fb),
				(unsigned long)(perf_draw_end / fb),
				(unsigned long)(perf_snd_pub / fb));
			perf_m68k_insns = perf_idle_skips = perf_tlb_faults = 0;
			perf_m68k_slices = 0;
			perf_draw_wait = perf_draw_issue = perf_draw_end = 0;
			perf_snd_pub = 0;

			// DRAW-1 fine split of the draw-issue bucket (video.c): phase
			// ticks (same 0.01%-of-frame units) + per-frame draw counts.
			// walk = sprite pass minus cache lookups minus rspq issue.
			extern uint32_t perf_dr_begin, perf_dr_sprites, perf_dr_fix;
			extern uint32_t perf_dr_cache, perf_dr_rspq;
			extern uint32_t perf_dr_tiles, perf_dr_cells, perf_dr_empty;
			extern uint32_t perf_dr_wwait;
			extern uint32_t perf_walk_spr, perf_walk_iter;
			framef("[PERF2] begin=%lu spr=%lu (cache=%lu rspq=%lu) fix=%lu tiles=%lu cells=%lu empty=%lu wwait=%lu wspr=%lu witer=%lu\n",
				(unsigned long)(perf_dr_begin / fb),
				(unsigned long)(perf_dr_sprites / fb),
				(unsigned long)(perf_dr_cache / fb),
				(unsigned long)(perf_dr_rspq / fb),
				(unsigned long)(perf_dr_fix / fb),
				(unsigned long)perf_dr_tiles,
				(unsigned long)perf_dr_cells,
				(unsigned long)perf_dr_empty,
				(unsigned long)(perf_dr_wwait / fb),
				(unsigned long)perf_walk_spr,
				(unsigned long)perf_walk_iter);
			perf_dr_begin = perf_dr_sprites = perf_dr_fix = 0;
			perf_dr_cache = perf_dr_rspq = 0;
			perf_dr_tiles = perf_dr_cells = perf_dr_empty = 0;
			perf_dr_wwait = 0;
			perf_walk_spr = perf_walk_iter = 0;

			// PLAN-DRAW-RDP Phase 0 decision data ([PERF3]): drawn-stream
			// run/repeat/palette/modal stats (video.c consume), cache-miss
			// split (roms.c), and RDP busy fractions from the free-running
			// 24-bit DPC counters (delta per window, wrap-safe at >=4fps).
			// dppipe/dpclk ~= RDP pipe busy fraction; dptmem = TMEM loads.
			{
				extern uint32_t perf_dr_recs, perf_dr_adjrep, perf_dr_maxrun;
				extern uint32_t perf_dr_uniqx, perf_dr_psw, perf_dr_modal;
				extern uint32_t perf_dr_miss, perf_dr_missticks;
				static uint32_t dpc_clk0, dpc_pipe0, dpc_tmem0;
				uint32_t clk  = *(volatile uint32_t*)0xA4100010 & 0xFFFFFF;
				uint32_t pipe = *(volatile uint32_t*)0xA4100018 & 0xFFFFFF;
				uint32_t tmem = *(volatile uint32_t*)0xA410001C & 0xFFFFFF;
				extern uint32_t perf_vrom_reads, perf_vrom_ticks;
				framef("[PERF3] recs=%lu rep=%lu maxrun=%lu uniq=%lu psw=%lu modal=%lu miss=%lu dmat=%lu dpclk=%lu dppipe=%lu dptmem=%lu vrom=%lu vromt=%lu\n",
					(unsigned long)perf_dr_recs,
					(unsigned long)perf_dr_adjrep,
					(unsigned long)perf_dr_maxrun,
					(unsigned long)perf_dr_uniqx,
					(unsigned long)perf_dr_psw,
					(unsigned long)perf_dr_modal,
					(unsigned long)perf_dr_miss,
					(unsigned long)(perf_dr_missticks / fb),
					(unsigned long)((clk  - dpc_clk0)  & 0xFFFFFF),
					(unsigned long)((pipe - dpc_pipe0) & 0xFFFFFF),
					(unsigned long)((tmem - dpc_tmem0) & 0xFFFFFF),
					(unsigned long)perf_vrom_reads,
					(unsigned long)(perf_vrom_ticks / fb));
				perf_vrom_reads = perf_vrom_ticks = 0;
				dpc_clk0 = clk; dpc_pipe0 = pipe; dpc_tmem0 = tmem;
				perf_dr_recs = perf_dr_adjrep = perf_dr_maxrun = 0;
				perf_dr_uniqx = perf_dr_psw = perf_dr_modal = 0;
				perf_dr_miss = perf_dr_missticks = 0;
			}
		}
		#endif
		#ifdef MVS64_OPHIST
		// Exact per-opcode execution histogram (bumped in m64k_asm.S's
		// dispatch). Every 300 frames: dump every opcode above ~0.05% of
		// the interval's executed instructions, then reset. Offline
		// analysis: analyze-ophist.py (parent dir).
		if ((g_frame % 300) == 299) {
			uint64_t total = 0;
			for (int i = 0; i < 65536; i++) total += m64k_ophist_tab[i];
			uint32_t thresh = (uint32_t)(total / 2000);
			if (thresh < 4) thresh = 4;
			enum { OPHIST_MAX = 384 };
			static uint16_t sel_op[OPHIST_MAX];
			static uint32_t sel_n[OPHIST_MAX];
			int nsel = 0;
			uint64_t selected = 0;
			for (int i = 0; i < 65536 && nsel < OPHIST_MAX; i++) {
				if (m64k_ophist_tab[i] >= thresh) {
					sel_op[nsel] = (uint16_t)i;
					sel_n[nsel] = m64k_ophist_tab[i];
					selected += m64k_ophist_tab[i];
					nsel++;
				}
			}
			// insertion sort, descending by count (nsel <= 384)
			for (int i = 1; i < nsel; i++) {
				uint16_t o = sel_op[i]; uint32_t n = sel_n[i]; int j = i - 1;
				while (j >= 0 && sel_n[j] < n) {
					sel_op[j+1] = sel_op[j]; sel_n[j+1] = sel_n[j]; j--;
				}
				sel_op[j+1] = o; sel_n[j+1] = n;
			}
			framef("[OPHIST] total=%llu sel=%llu nsel=%d\n",
				(unsigned long long)total, (unsigned long long)selected, nsel);
			for (int i = 0; i < nsel; i += 8) {
				char line[160]; int p = 0;
				for (int j = i; j < nsel && j < i + 8; j++)
					p += sprintf(line + p, " %04x:%lu",
						sel_op[j], (unsigned long)sel_n[j]);
				framef("[OPH]%s\n", line);
			}
			memset(m64k_ophist_tab, 0, sizeof(m64k_ophist_tab));
		}
		#endif
		#ifdef MVS64_IDLEPROBE
		{
			extern uint32_t idle_probe_found;
			if (idle_probe_found) {
				debugf("[IDLEPROBE] long spin at 68k pc=%06lx\n",
					(unsigned long)(idle_probe_found & 0xFFFFFF));
				idle_probe_found = 0;
			}
		}
		#endif
		#endif

		// NOTE: rom_next_frame() is already called once per frame inside
		// emu_render() (after the frame is drawn); calling it again here ran it
		// twice/frame, halving the sprite-cache LRU retention window. Removed.
		#ifdef N64
		uint32_t curtime = TICKS_READ();
		if (TICKS_DISTANCE(fps_time, curtime) > TICKS_FROM_MS(1000)) {
			debugf("FPS: %.1f\n", (g_frame - fps_frame) * (float)TICKS_PER_SECOND / TICKS_DISTANCE(fps_time, curtime));
			fps_frame = g_frame;
			fps_time = curtime;
		}
		// Guest-frame-keyed speed window: the scripted input is frame-counted,
		// so window k covers the same game content in every build. Emulated
		// fps = 300 / ms; displayed fps = drawn / ms.
		if ((g_frame % 300) == 0) {
			static uint32_t fsk_t0;
			if (fsk_t0)
				debugf("[FSKIP] f=%d ms=%lu drawn=%lu skipped=%lu\n", g_frame,
					(unsigned long)(TICKS_DISTANCE(fsk_t0, curtime) / (TICKS_PER_SECOND / 1000)),
					(unsigned long)fskip_drawn, (unsigned long)fskip_skipped);
			fsk_t0 = curtime;
			fskip_drawn = fskip_skipped = 0;
		}
		#endif
	}

	debugf("end\n");
	cpu_start_trace(1000);
	m68k_exec(g_clock+100);

	#ifndef N64
	wav_close();
	FILE *f = fopen("vram.dump", "wb");
	fwrite(VIDEO_RAM, 1, sizeof(VIDEO_RAM), f);
	fclose(f);
	#endif

	plat_save_screenshot("screen.bmp");
}
