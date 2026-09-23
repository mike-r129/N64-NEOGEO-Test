// MVS64_PCPROF — statistical host-PC sampling profiler (diagnostic only).
//
// A continuous libdragon timer samples the interrupted EPC (read from the
// interrupt_exception_frame libdragon publishes during interrupt dispatch)
// into a per-frame ring. At frame end the ring is folded into a 32-byte
// (= one VR4300 icache line) histogram over .text ONLY for in-fight frames
// (the same content filter the fps analysis uses), and discarded otherwise.
// Every PCPROF_WINDOW frames the non-zero lines are dumped as
// "[PCP] <hexline>:<count> ..." for offline symbolization
// (analyze-pcprof.py next to this file, via nm).
//
// Perturbation: one short ISR per sample (~4kHz) — a few % of wall and some
// icache pollution. Use the shares, not the fps.
#ifdef MVS64_PCPROF
#include <libdragon.h>
#include <stdint.h>
#include "emu.h"

extern char __text_start[], __text_end[];
extern void *interrupt_exception_frame;

#define PCPROF_HZ      4000
#define PCPROF_RING    1024
#define PCPROF_WINDOW  1200
#define PCPROF_MAXLINES 16384   // 512KB of .text in 32B lines

static uint32_t ring[PCPROF_RING];
static volatile uint32_t ring_n;
static uint32_t hist[PCPROF_MAXLINES];
static uint32_t n_fight_frames, n_samples, n_other;

static void pcprof_tick(int ovfl) {
	(void)ovfl;
	// interrupt_exception_frame is the raw sp; the reg_block_t sits 32
	// bytes above it (inthandler.S STACK_GPR; cf. its `addiu a0, sp, 32`).
	char *sp = (char *)interrupt_exception_frame;
	if (!sp) return;
	reg_block_t *f = (reg_block_t *)(sp + 32);
	uint32_t n = ring_n;
	if (n < PCPROF_RING) { ring[n] = f->epc; ring_n = n + 1; }
}

void pcprof_init(void) {
	timer_init();
	new_timer(TICKS_FROM_US(1000000 / PCPROF_HZ), TF_CONTINUOUS, pcprof_tick);
}

// Called once per emulated frame after [PROFILE]; in_fight selects whether
// this frame's samples count.
void pcprof_frame(int frame, int in_fight) {
	disable_interrupts();
	uint32_t n = ring_n;
	if (in_fight) {
		uint32_t base = (uint32_t)__text_start;
		uint32_t lim = (uint32_t)__text_end;
		for (uint32_t i = 0; i < n; i++) {
			uint32_t pc = ring[i];
			if (pc >= base && pc < lim && ((pc - base) >> 5) < PCPROF_MAXLINES)
				hist[(pc - base) >> 5]++;
			else
				n_other++;
		}
		n_samples += n;
		n_fight_frames++;
	}
	ring_n = 0;
	enable_interrupts();

	if (frame && (frame % PCPROF_WINDOW) == 0 && n_samples) {
		uint32_t base = (uint32_t)__text_start;
		debugf("[PCPHDR] f=%d base=%08lx frames=%lu samples=%lu other=%lu\n",
			frame, (unsigned long)base, (unsigned long)n_fight_frames,
			(unsigned long)n_samples, (unsigned long)n_other);
		char buf[200]; int len = 0;
		for (int i = 0; i < PCPROF_MAXLINES; i++) {
			if (!hist[i]) continue;
			len += snprintf(buf + len, sizeof(buf) - len, " %x:%lx", i, (unsigned long)hist[i]);
			if (len > 150) { debugf("[PCP]%s\n", buf); len = 0; }
			hist[i] = 0;
		}
		if (len) debugf("[PCP]%s\n", buf);
		n_fight_frames = n_samples = n_other = 0;
	}
}
#endif
