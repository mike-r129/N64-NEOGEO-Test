// Z80 CPU core — vendored from superzazu/z80 (https://github.com/superzazu/z80).
// Copyright (c) 2019 Nicolas Allemand. MIT License — see z80.LICENSE.
#ifndef Z80_Z80_H_
#define Z80_Z80_H_

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct z80 z80;
struct z80 {
  uint8_t (*read_byte)(void*, uint16_t);
  void (*write_byte)(void*, uint16_t, uint8_t);
  // MVS64: widened port to 16-bit so the full I/O address (high byte = B for
  // IN/OUT (C), or A for IN/OUT (n)) reaches the handler. NeoGeo Z80 bank
  // switching encodes the bank number in the high byte of the port address.
  uint8_t (*port_in)(z80*, uint16_t);
  void (*port_out)(z80*, uint16_t, uint8_t);
  void* userdata;

  unsigned long cyc; // cycle count (t-states)

  uint16_t pc, sp, ix, iy; // special purpose registers
  uint16_t mem_ptr; // "wz" register
  uint8_t a, b, c, d, e, h, l; // main registers
  uint8_t a_, b_, c_, d_, e_, h_, l_, f_; // alternate registers
  uint8_t i, r; // interrupt vector, memory refresh

  // flags: sign, zero, yf, half-carry, xf, parity/overflow, negative, carry
  bool sf : 1, zf : 1, yf : 1, hf : 1, xf : 1, pf : 1, nf : 1, cf : 1;

  uint8_t iff_delay;
  uint8_t interrupt_mode;
  uint8_t int_data;
  bool iff1 : 1, iff2 : 1;
  bool halted : 1;
  bool int_pending : 1, nmi_pending : 1;
  // MVS64 read page map: byte at addr = *(uint8_t*)(rmap[addr >> 8] + addr).
  // Entries are host pointers pre-biased by the page's Z80 base address, so
  // every memory READ (opcode/operand fetch, data) is a branchless inline
  // table lookup instead of the read_byte callback. The owner keeps it in
  // sync with its memory map (bank switches); read_byte must still be set
  // for owners that call it directly. Writes stay on write_byte.
  const uintptr_t* rmap;
};

// MVS64: the Z80 per-step working set in one contiguous, 16-byte aligned
// block (~3.9 KB): the owner's CPU struct, the opcode cycle tables, the read
// page map and the 2 KB work RAM (NeoGeo 0xF800-0xFFFF). Contiguous objects
// smaller than the 8 KB direct-mapped dcache cannot evict each other, so no
// relink can make the Z80 loop ping-pong between its own tables.
struct z80_hot {
  z80 cpu;
  uint8_t cyc_00[256], cyc_ed[256], cyc_ddfd[256];
  uintptr_t rmap[256];
  uint8_t ram[0x800];
};
extern struct z80_hot z80_hot;

void z80_init(z80* const z);
void z80_step(z80* const z);
void z80_debug_output(z80* const z);
void z80_gen_nmi(z80* const z);
void z80_gen_int(z80* const z, uint8_t data);

// z80_step for the owner's hot run loop: same fetch / execute / event gate,
// inlined into the caller so the per-step call and its 6-register frame go
// away; interrupt servicing stays out of line. Identical to z80_step.
void z80_exec_opcode(z80* const z, uint8_t opcode);
void z80_process_interrupts(z80* const z);
#ifndef MVS64_Z80OPHIST
static inline void z80_step_inline(z80* const z) {
  uint8_t opcode = 0x00;               // HALT executes NOPs in place
  if (!z->halted) {
    opcode = *(const uint8_t*)(z->rmap[z->pc >> 8] + z->pc);
    z->pc++;
  }
  z80_exec_opcode(z, opcode);
  if (z->iff_delay | (uint8_t)(z->nmi_pending | (z->int_pending & z->iff1)))
    z80_process_interrupts(z);
}
#else
#define z80_step_inline z80_step
#endif

#endif // Z80_Z80_H_
