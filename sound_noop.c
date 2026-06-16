// No-op sound module.
//
// Satisfies sound.h until the real Z80 (WS2) + YM2610 (WS3) module replaces it in
// the build (Makefile.mvs64 / Makefile.pctests). It preserves the exact pre-sound
// behavior of mvs64: the 68k<->Z80 latch reads back the constant the original
// hw.c stub returned, command writes are discarded, and sample generation is silent.
#include "sound.h"
#include <string.h>

void sound_init(void) {}
void sound_reset(void) {}
void sound_write_command(uint8_t cmd) { (void)cmd; }
uint8_t sound_read_status(void) { return 1; }

int sound_gen_samples(int16_t *out, int nsamples) {
	memset(out, 0, (size_t)nsamples * 2 * sizeof(int16_t));  // stereo silence
	return nsamples;
}
