// Pins the Z80's per-step data to fixed dcache sets. z80.o's switch jump
// tables (.rodata.<function> sections) and z80_hot (.rodata.z80_hot) are
// read on every Z80 step and lie in one ~7.4 KB run, but that run used to
// start wherever the preceding .text/.rodata happened to end, so ANY edit
// earlier in the image slid it across the 8 KB direct-mapped dcache. Most
// positions are fine; some put it on sound data at fixed .bss addresses
// (2026-10-04: 160 bytes of new 68k code moved z80_hot by 10 sets and
// cost +0.85 ms/frame of sound time in ares).
//
// This object is linked right before z80.o (Makefile.mvs64 core_src), and
// the linker places .rodata.* input sections file by file, so z80.o's
// rodata starts MVS64_Z80_ANCHOR_PAD bytes after an 8 KB boundary no
// matter what changes elsewhere. The pad is chosen so z80_hot lands where
// it measured fast: offset 0x1CF0 in the 8 KB page (sets 463.., as in the
// p22a build), with z80.o's jump tables just before it. z80_hot sits
// 0xE30 bytes into z80.o's rodata, so pad = 0x1CF0 - 0xE30 = 0xEC0; if
// z80.c's tables change, re-derive from nm (z80_hot - z80_rodata_anchor
// - pad). Costs < 8 KB.
#include <stdint.h>

#ifndef MVS64_Z80_ANCHOR_PAD
#define MVS64_Z80_ANCHOR_PAD 0xEC0
#endif

const uint8_t z80_rodata_anchor[MVS64_Z80_ANCHOR_PAD]
	__attribute__((aligned(8192), section(".rodata.z80_anchor"))) = { 1 };
