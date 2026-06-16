#ifndef SOUND_H
#define SOUND_H

#include <stdint.h>

// Sound subsystem seam (NeoGeo Z80 + YM2610).
//
// This is the single boundary between the mvs64 chassis and all sound emulation.
// A no-op implementation (sound_noop.c) keeps the build and existing games working
// until the real Z80 (WS2) + YM2610 (WS3) module lands and replaces it. Both
// implementations satisfy this exact API, so the rest of the emulator never changes.

// Initialize the sound subsystem. The real module reads M_ROM/m_rom_size and the
// streamed v.rom (see roms.h); the no-op ignores them. Call after rom_load + hw_init.
void sound_init(void);

// Reset the sound CPU + chip (68k-driven Z80 reset, or soft reset).
void sound_reset(void);

// 68k -> Z80 sound-command latch (NeoGeo 0x320000 write). The real module latches
// the command and raises the Z80 NMI; the no-op discards it.
void sound_write_command(uint8_t cmd);

// Z80 -> 68k reply/status latch (NeoGeo 0x320000 read). The real module returns the
// Z80 reply byte; the no-op returns 1 (the constant the original stub returned, which
// the BIOS sound handshake tolerates).
uint8_t sound_read_status(void);

// Render `nsamples` interleaved stereo signed-16-bit samples at the audio output rate
// (the WS4 producer contract). The real module mixes YM2610 FM+SSG+ADPCM-A/B; the
// no-op emits silence. Returns the number of samples written.
int sound_gen_samples(int16_t *out, int nsamples);

#endif
