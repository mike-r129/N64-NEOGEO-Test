#ifndef CYCLES_H
#define CYCLES_H

#include <stdint.h>

// 68000 exception-processing cycle counts, indexed by exception vector number.
//
// This header was referenced by m64k.c (#include "cycles.h") but is absent from
// the upstream mvs64 tree, which broke the N64 build against current libdragon.
// Reconstructed here from the standard Motorola M68000 exception timings (the
// same values as the vendored Musashi table m68ki_exception_cycle_table[0] in
// m68kcpu.c). m64k.c reads vectors 0x3 (Address Error=50), 0x5 (Zero Divide=38),
// and 24+level (interrupt autovectors=44); the full standard set is included for
// completeness. Unlisted vectors default to 0 (unused by m64k.c).
static const uint16_t __m64k_exception_cycle_table[256] = {
	[2]  = 50,  // Bus Error
	[3]  = 50,  // Address Error
	[4]  = 34,  // Illegal Instruction
	[5]  = 38,  // Divide by Zero
	[6]  = 40,  // CHK
	[7]  = 34,  // TRAPV
	[8]  = 34,  // Privilege Violation
	[9]  = 34,  // Trace
	[10] = 34,  // Line 1010 (A)
	[11] = 34,  // Line 1111 (F)
	[15] = 44,  // Uninitialized Interrupt
	[24] = 44, [25] = 44, [26] = 44, [27] = 44,  // Spurious + Level 1-3 autovectors
	[28] = 44, [29] = 44, [30] = 44, [31] = 44,  // Level 4-7 autovectors
	[32] = 34, [33] = 34, [34] = 34, [35] = 34,  // TRAP #0-3
	[36] = 34, [37] = 34, [38] = 34, [39] = 34,  // TRAP #4-7
	[40] = 34, [41] = 34, [42] = 34, [43] = 34,  // TRAP #8-11
	[44] = 34, [45] = 34, [46] = 34, [47] = 34,  // TRAP #12-15
};

#endif
