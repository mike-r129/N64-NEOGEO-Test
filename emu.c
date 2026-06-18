#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#ifndef N64
#include <stdlib.h>
#endif
#include "emu.h"
#ifdef N64
#include "m64k/m64k.h"
#else
#include "m68k.h"
#endif
#include "hw.h"
#include "video.h"
#include "roms.h"
#include "platform.h"

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

static int g_frame;

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
#endif
#ifdef N64
m64k_t m64k;
#endif
static uint64_t g_clock, g_clock_framebegin;
static uint64_t m68k_clock;
static EmuEvent events[MAX_EVENTS];
uint32_t profile_hw_io;
uint32_t profile_dma_load;

static uint64_t m68k_exec(uint64_t clock) {
	clock /= M68K_CLOCK_DIV;
	if (clock > m68k_clock) {
		#ifdef N64
		debugf("m68k_exec: %d\n", (int)(clock - m68k_clock));
		m68k_clock = m64k_run(&m64k, clock);
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
		#ifdef N64
		m64k_run_stop(&m64k);
		#else
		m68k_end_timeslice();
		#endif
	}
}

int64_t emu_clock(void) {
	#ifdef N64
	return m64k_get_clock(&m64k) * M68K_CLOCK_DIV;
	#else
	return g_clock + m68k_cycles_run() * M68K_CLOCK_DIV;
	#endif
}

int64_t emu_clock_frame(void) {
	return emu_clock() - g_clock_framebegin;
}

void emu_cpu_reset(void) {
	#ifdef N64
	m64k_pulse_reset(&m64k);
	#else
	m68k_pulse_reset();
	#endif
}

uint32_t emu_pc(void) {
	#ifdef N64
	return m64k_get_pc(&m64k) & 0xFFFFFF;
	#else
	return m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
	#endif
}

void emu_cpu_irq(int irq, bool on) {
	#ifdef N64
	m64k_set_virq(&m64k, irq, on);
	#else
	m68k_set_virq(irq, on);
	#endif
}

#ifdef N64
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
	debugf("[EMU] VBlank - clock:%lld clock_frame:%lld\n", (long long)emu_clock(), (long long)emu_clock_frame());
	return FRAME_CLOCK;
}

uint32_t render_time;

uint32_t emu_render(void *arg) {

	#ifdef N64
	if (CONFIG_FRAMESKIP_MODE == 2) {
		extern volatile int N64_FRAME;
		const int MAX_SKIP = 4;
		static int skip = 0;

		if (N64_FRAME > g_frame) {
			skip++;
			if (skip < MAX_SKIP) {
				debugf("[RENDER] skip frame\n");
				return FRAME_CLOCK;
			}
			debugf("[RENDER] max skip\n");
			skip = 0;
			disable_interrupts();
			N64_FRAME = g_frame;
			enable_interrupts();
		}
	}
	#endif

	if (CONFIG_FRAMESKIP_MODE == 1) {
		if (g_frame & 1) {
			debugf("[RENDER] skip frame\n");
			return FRAME_CLOCK;
		}
	}

	debugf("[RENDER] render\n");
	#ifdef N64
	uint32_t t0 = TICKS_READ();
	#endif
	plat_beginframe();
	video_render();
	plat_endframe();

	rom_next_frame();

	#ifdef N64
	render_time = TICKS_DISTANCE(t0, TICKS_READ());
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
	debugf("[EMU] Frame completed: %d (vsync: %llu)\n", g_frame, (unsigned long long)vsync);
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

	plat_init(44100, FPS);

	#ifndef N64
	// Headless test harness: when MVS64_FRAMES=N is set, run N frames with no
	// SDL window (use SDL_VIDEODRIVER=dummy / SDL_AUDIODRIVER=dummy), dumping a
	// screenshot every MVS64_SHOT frames and the 68K PC each second, then exit.
	// Lets us validate boot progression with the human out of the loop.
	const char *hl_env = getenv("MVS64_FRAMES");
	int headless = hl_env ? atoi(hl_env) : 0;
	const char *shot_env = getenv("MVS64_SHOT");
	int shot_interval = shot_env ? atoi(shot_env) : 0;
	if (headless) {
		keystate = hl_keys;                 // drive input from our scripted buffer
		const char *script = getenv("MVS64_INPUT");
		if (script) hl_load_script(script);
	}
	plat_enable_video(headless ? false : true);
	#else
	plat_enable_video(true);
	#endif

	#ifdef N64
	rom_load("rom:/");
	#else
	rom_load(argv[1]);
	#endif

	#ifdef N64
	m64k_init(&m64k);
	m64k_set_hook_irqack(&m64k, cpu_irqack, NULL);
	#else
	m68k_init();
	#endif

	hw_init();
	g_clock = 0;

	#ifdef N64
	m64k_pulse_reset(&m64k);
	#else
	m68k_set_cpu_type(M68K_CPU_TYPE_68000);
	m68k_pulse_reset();
	#endif
	m68k_clock = 0;

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
		#ifdef N64
		uint32_t t0 = TICKS_READ();
		#endif
		#ifndef N64
		if (headless) hl_apply_input(g_frame);
		#endif

		emu_run_frame();
		if (!plat_poll()) break;

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

		debugf("[PROFILE] cpu:%.2f%% io:%.2f%% draw:%.2f%% dma:%.2f%% PC:%06lx\n",
			(float)emu_time * 100.f / (float)(TICKS_PER_SECOND / 60),
			(float)profile_hw_io * 100.f / (float)(TICKS_PER_SECOND / 60),
			(float)render_time * 100.f / (float)(TICKS_PER_SECOND / 60),
			(float)profile_dma_load * 100.f / (float)(TICKS_PER_SECOND / 60),
			#ifdef N64
			m64k_get_pc(&m64k));
			#else
			(uint32_t)m68k_get_reg(NULL, M68K_REG_PC));
			#endif
		#endif

		rom_next_frame();
		#ifdef N64
		uint32_t curtime = TICKS_READ();
		if (TICKS_DISTANCE(fps_time, curtime) > TICKS_FROM_MS(1000)) {
			debugf("FPS: %.1f\n", (g_frame - fps_frame) * (float)TICKS_PER_SECOND / TICKS_DISTANCE(fps_time, curtime));
			fps_frame = g_frame;
			fps_time = curtime;
		}
		#endif
	}

	debugf("end\n");
	cpu_start_trace(1000);
	m68k_exec(g_clock+100);

	#ifndef N64
	FILE *f = fopen("vram.dump", "wb");
	fwrite(VIDEO_RAM, 1, sizeof(VIDEO_RAM), f);
	fclose(f);
	#endif

	plat_save_screenshot("screen.bmp");
}
