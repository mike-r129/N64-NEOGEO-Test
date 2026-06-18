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
#include <string.h>
#include <stdlib.h>

// NeoGeo audio Z80 runs at 4 MHz; produce one video frame's worth per call.
#define Z80_CLOCK            4000000
#define Z80_CYCLES_PER_FRAME (Z80_CLOCK / 60)

static z80 cpu;
static int  z80_active;                 // false when there is no m.rom
static uint8_t z80_ram[0x800];          // 2KB work RAM at 0xF800-0xFFFF
static const uint8_t *z80_bank[4];      // window base pointers into M_ROM

static uint8_t sound_code;              // 68k -> Z80 command latch
static uint8_t result_code;             // Z80 -> 68k reply latch
static uint8_t pending_command;
static int snd_dbg;                     // MVS64_SNDDBG: trace Z80 PC

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
	case 0x04: return 0;                                 // YM2610 status A (WS3)
	case 0x05: return 0;                                 // YM2610 read      (WS3)
	case 0x06: return 0;                                 // YM2610 status B  (WS3)
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
	case 0x04: case 0x05: case 0x06: case 0x07: break;   // YM2610 (WS3)
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
	z80_init(&cpu);
	cpu.read_byte  = z80_read;
	cpu.write_byte = z80_write;
	cpu.port_in    = z80_in;
	cpu.port_out   = z80_out;
}

void sound_write_command(uint8_t cmd) {
	sound_code = cmd;
	pending_command = 1;
	if (!z80_active) return;
	z80_gen_nmi(&cpu);    // command latch raises Z80 NMI...
	z80_run(300);         // ...and the driver replies within the 68k busy-wait
}

uint8_t sound_read_status(void) {
	return result_code;
}

int sound_gen_samples(int16_t *out, int nsamples) {
	z80_run(Z80_CYCLES_PER_FRAME);
	if (snd_dbg)
		debugf("[SND] z80 pc=%04x cyc=%lu code=%02x result=%02x\n",
			cpu.pc, cpu.cyc, sound_code, result_code);
	memset(out, 0, (size_t)nsamples * 2 * sizeof(int16_t));  // silence until WS3
	return nsamples;
}
