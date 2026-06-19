// NeoGeo sound subsystem: Z80 audio CPU (+ YM2610 in WS3).
//
// Implements the sound.h seam with a real Z80 (superzazu core) running the
// m.rom driver. Memory map, bank switching and I/O port layout follow the
// NeoGeo hardware (cross-checked against gngeo). The YM2610 is not wired yet
// (WS3): its ports are stubbed, so this boots the driver and completes the
// 68k<->Z80 command handshake but still emits silence.
#include "sound.h"
#include "roms.h"
#include "platform.h"
#include "z80.h"
#include "ym2610/ym2610.h"
#include <string.h>
#include <stdlib.h>

// NeoGeo audio Z80 runs at 4 MHz; produce one video frame's worth per call.
#define Z80_CLOCK            4000000
#define Z80_CYCLES_PER_FRAME (Z80_CLOCK / 60)
#define YM_CLOCK             8000000
#define AUDIO_RATE           44100

static z80 cpu;
static int  z80_active;                 // false when there is no m.rom
static uint8_t z80_ram[0x800];          // 2KB work RAM at 0xF800-0xFFFF
static const uint8_t *z80_bank[4];      // window base pointers into M_ROM

static uint8_t sound_code;              // 68k -> Z80 command latch
static uint8_t result_code;             // Z80 -> 68k reply latch
static uint8_t pending_command;
static int snd_dbg;                     // MVS64_SNDDBG: trace Z80 PC

// YM2610 stream output (interleaved s16 L/R), filled by YM2610Update_stream().
uint16_t play_buffer[16384];

// Resident ADPCM sample ROM (v.rom). The YM2610 core needs random access to it;
// mvs64 normally streams v.rom from cart, so we pull it into RAM once here.
static uint8_t *vrom_resident;

// --- YM2610 timer/IRQ glue (cycle-based) -----------------------------------
// The chip schedules timers in seconds; we track deadlines in Z80 cycles and
// fire YM2610TimerOver (which raises the Z80 IRQ that ticks the music driver).
static int           ym_timer_on[2];
static unsigned long ym_timer_deadline[2];   // absolute cpu.cyc

double ym2610_time_now(void) {              // FM_GET_TIME_NOW source (seconds)
	return (double)cpu.cyc / (double)Z80_CLOCK;
}

static void ym_timer_handler(int c, int count, double stepTime) {
	if (count == 0) {
		ym_timer_on[c] = 0;
	} else {
		ym_timer_on[c] = 1;
		ym_timer_deadline[c] =
			cpu.cyc + (unsigned long)((double)count * stepTime * Z80_CLOCK);
	}
}

static void ym_irq_handler(int irq) {
	if (irq) z80_gen_int(&cpu, 0xff);       // assert (IM1 -> RST 38h)
	else     cpu.int_pending = 0;           // deassert if not yet serviced
}

// Bank switch: window <bank> is remapped to M_ROM + size*(porthi & mask).
// Window/bank geometry (see gngeo cpu_z80_switchbank):
//   bank 0 -> 0x8000, 16KB, mask 0x0f   bank 1 -> 0xC000, 8KB,  mask 0x1f
//   bank 2 -> 0xE000, 4KB,  mask 0x3f   bank 3 -> 0xF000, 2KB,  mask 0x7f
static void switchbank(int bank, uint16_t port) {
	static const uint32_t bsize[4] = { 0x4000, 0x2000, 0x1000, 0x0800 };
	static const uint32_t bmask[4] = { 0x0f, 0x1f, 0x3f, 0x7f };
	uint32_t off = bsize[bank] * ((port >> 8) & bmask[bank]);
	if (off < m_rom_size)
		z80_bank[bank] = M_ROM + off;
}

// --- Z80 bus ---------------------------------------------------------------
static uint8_t z80_read(void *ud, uint16_t addr) {
	(void)ud;
	if (addr < 0x8000) return M_ROM[addr];               // fixed first 32KB
	if (addr < 0xC000) return z80_bank[0][addr - 0x8000];
	if (addr < 0xE000) return z80_bank[1][addr - 0xC000];
	if (addr < 0xF000) return z80_bank[2][addr - 0xE000];
	if (addr < 0xF800) return z80_bank[3][addr - 0xF000];
	return z80_ram[addr - 0xF800];
}

static void z80_write(void *ud, uint16_t addr, uint8_t val) {
	(void)ud;
	if (addr >= 0xF800) z80_ram[addr - 0xF800] = val;    // only work RAM is writable
}

static uint8_t z80_in(z80 *z, uint16_t port) {
	(void)z;
	switch (port & 0xff) {
	case 0x00: pending_command = 0; return sound_code;   // read command, ack
	case 0x04: return YM2610Read(0);                     // YM2610 status A
	case 0x05: return YM2610Read(1);                     // YM2610 read port
	case 0x06: return YM2610Read(2);                     // YM2610 status B
	case 0x08: switchbank(3, port); return 0;
	case 0x09: switchbank(2, port); return 0;
	case 0x0a: switchbank(1, port); return 0;
	case 0x0b: switchbank(0, port); return 0;
	}
	return 0;
}

static void z80_out(z80 *z, uint16_t port, uint8_t val) {
	(void)z;
	switch (port & 0xff) {
	case 0x04: YM2610Write(0, val); break;               // control A
	case 0x05: YM2610Write(1, val); break;               // data A
	case 0x06: YM2610Write(2, val); break;               // control B
	case 0x07: YM2610Write(3, val); break;               // data B
	case 0x0c: result_code = val; break;                 // reply to 68k
	}
}

static void z80_run(unsigned cycles) {
	if (!z80_active) return;
	unsigned long target = cpu.cyc + cycles;
	while (cpu.cyc < target)
		z80_step(&cpu);
}

// --- sound.h seam ----------------------------------------------------------
void sound_init(void) {
	snd_dbg = getenv("MVS64_SNDDBG") != NULL;
	if (!M_ROM || m_rom_size == 0) {
		z80_active = 0;
		debugf("[SND] no m.rom; sound disabled\n");
		return;
	}
	z80_init(&cpu);
	cpu.read_byte  = z80_read;
	cpu.write_byte = z80_write;
	cpu.port_in    = z80_in;
	cpu.port_out   = z80_out;

	// Pull v.rom (ADPCM-A/B sample ROM) resident for the YM2610. samsho2 has no
	// separate ADPCM-B ROM, so A and B share the same data (as on real NeoGeo).
	// If the alloc fails (e.g. tight N64 RAM), ADPCM is disabled but FM/SSG run.
	if (v_rom_size) {
#ifdef N64
		// mvs64 streams v.rom from cart. Pulling the ~7MB ADPCM sample ROM fully
		// resident overruns the N64 heap into the stack region and corrupts return
		// addresses (intermittent wild-jump crash ~9s into boot). Leave it
		// non-resident: ADPCM disabled (vrom_resident NULL), FM/SSG still play.
		// ADPCM via streaming is future work.
		debugf("[SND] N64: v.rom not pulled resident (%u bytes); ADPCM disabled\n", v_rom_size);
#else
		vrom_resident = malloc(v_rom_size);
		if (vrom_resident) {
			vrom_read(0, vrom_resident, v_rom_size);
			debugf("[SND] v.rom resident: %u bytes\n", v_rom_size);
		} else {
			debugf("[SND] v.rom alloc failed; ADPCM disabled\n");
		}
#endif
	}
	YM2610Init(YM_CLOCK, AUDIO_RATE,
		vrom_resident, vrom_resident ? v_rom_size : 0,   // ADPCM-A
		vrom_resident, vrom_resident ? v_rom_size : 0,   // ADPCM-B (shared)
		ym_timer_handler, ym_irq_handler);

	sound_reset();
	z80_active = 1;
	debugf("[SND] Z80 sound CPU init (m.rom %u bytes)\n", m_rom_size);
}

void sound_reset(void) {
	// Initial 1:1 window mapping (matches gngeo cpu_z80_init).
	z80_bank[0] = M_ROM + 0x8000;
	z80_bank[1] = M_ROM + 0xC000;
	z80_bank[2] = M_ROM + 0xE000;
	z80_bank[3] = M_ROM + 0xF000;
	memset(z80_ram, 0, sizeof(z80_ram));
	sound_code = result_code = pending_command = 0;
	ym_timer_on[0] = ym_timer_on[1] = 0;
	z80_init(&cpu);
	cpu.read_byte  = z80_read;
	cpu.write_byte = z80_write;
	cpu.port_in    = z80_in;
	cpu.port_out   = z80_out;
	YM2610Reset();
}

void sound_write_command(uint8_t cmd) {
	sound_code = cmd;
	pending_command = 1;
	if (!z80_active) return;
#ifdef N64
	// This runs inside the TLB/MMIO exception handler, which clears SR.CU1
	// (FPU disabled) in hw_n64.S. The Z80 driver writes the YM2610, which uses
	// FP math -> a Coprocessor-Unusable fault here nests into a wild jump. The
	// NeoGeo BIOS polls for the sound reply in a bounded loop at boot, so the run
	// MUST be synchronous (deferring it to the next frame mis-sequences the
	// handshake -> the BIOS jumps wild ~frame 537). Fix: re-enable the FPU (CU1,
	// status bit 29) just around the run, then restore it. Safe because the
	// interrupted m64k 68k interpreter is integer-only and holds no live FP state.
	uint32_t sr;
	__asm__ volatile("mfc0 %0, $12" : "=r"(sr));
	__asm__ volatile("mtc0 %0, $12" :: "r"(sr | (1u << 29)));  // set CU1
	z80_gen_nmi(&cpu);
	z80_run(300);
	__asm__ volatile("mtc0 %0, $12" :: "r"(sr));               // restore CU1
#else
	z80_gen_nmi(&cpu);    // command latch raises Z80 NMI...
	z80_run(300);         // ...and the driver replies within the 68k busy-wait
#endif
}

uint8_t sound_read_status(void) {
	return result_code;
}

static void emit(int16_t *out, int from, int count) {
	YM2610Update_stream(count);
	for (int i = 0; i < count; i++) {
		out[(from + i) * 2 + 0] = (int16_t)play_buffer[i * 2 + 0];
		out[(from + i) * 2 + 1] = (int16_t)play_buffer[i * 2 + 1];
	}
}

int sound_gen_samples(int16_t *out, int nsamples) {
	if (!z80_active) {
		memset(out, 0, (size_t)nsamples * 2 * sizeof(int16_t));
		return nsamples;
	}

	const unsigned long frame_start = cpu.cyc;
	const unsigned long frame_end   = cpu.cyc + Z80_CYCLES_PER_FRAME;
	int produced = 0;

	while (cpu.cyc < frame_end) {
		// Service any due FM timers first (re-arms them forward + raises IRQ).
		for (int c = 0; c < 2; c++)
			if (ym_timer_on[c] && cpu.cyc >= ym_timer_deadline[c])
				YM2610TimerOver(c);

		// Run the Z80 in a bounded slice, stopping early on a timer deadline so
		// the music driver's IRQ tick stays on time.
		unsigned long next = cpu.cyc + 1000;
		if (next > frame_end) next = frame_end;
		for (int c = 0; c < 2; c++)
			if (ym_timer_on[c] && ym_timer_deadline[c] > cpu.cyc &&
			    ym_timer_deadline[c] < next)
				next = ym_timer_deadline[c];
		while (cpu.cyc < next)
			z80_step(&cpu);

		// Generate samples up to the cycle-proportional point in the frame.
		int target = (int)((unsigned long long)(cpu.cyc - frame_start) *
		                    nsamples / Z80_CYCLES_PER_FRAME);
		if (target > nsamples) target = nsamples;
		if (target > produced) { emit(out, produced, target - produced); produced = target; }
	}
	if (produced < nsamples) emit(out, produced, nsamples - produced);

	if (snd_dbg)
		debugf("[SND] z80 pc=%04x code=%02x result=%02x timers=%d%d s0=%d\n",
			cpu.pc, sound_code, result_code, ym_timer_on[0], ym_timer_on[1],
			(int)play_buffer[0]);

#ifdef MVS64_AUTOINPUT
	// Audio-activity probe: report the RMS amplitude of the generated frame so
	// "is sound actually being produced" is verifiable headless (non-zero,
	// varying RMS = the Z80 music driver is feeding the YM2610).
	{
		static int sc = 0;
		uint64_t acc = 0; int pk = 0;
		for (int i = 0; i < nsamples * 2; i++) {
			int v = out[i]; if (v < 0) v = -v;
			acc += (uint64_t)v * v; if (v > pk) pk = v;
		}
		int rms = 0; if (nsamples) { uint64_t m = acc / (nsamples * 2); while ((uint64_t)(rms+1)*(rms+1) <= m) rms++; }
		if ((sc++ % 60) == 0)
			debugf("[SNDRMS] rms=%d peak=%d z80pc=%04x code=%02x\n", rms, pk, cpu.pc, sound_code);
	}
#endif
	return nsamples;
}
