// Throwaway: disassemble the converted samsho2 program ROM (p.rom, already in
// 68k big-endian order for m64k) at a 68k address range. Build:
//   cc disgame.c m68kdasm.c -o disgame   (run from N64-NEOGEO/)
#include <stdio.h>
#include <stdlib.h>
#include "m68k.h"

static unsigned char prom[0x100000];

static unsigned int rd(unsigned int a, int sz) {
    a &= 0xFFFFFF;
    unsigned int v = 0;
    for (int i = 0; i < sz; i++) v = (v << 8) | (a + i < 0x100000 ? prom[a + i] : 0);
    return v;
}
unsigned int m68k_read_disassembler_8 (unsigned int a) { return rd(a, 1); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return rd(a, 2); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return rd(a, 4); }

int main(int argc, char **argv) {
    FILE *f = fopen("samsho2.n64/p.rom", "rb");
    if (!f) { perror("p.rom"); return 1; }
    fread(prom, 1, sizeof(prom), f); fclose(f);
    unsigned int start = strtoul(argv[1], 0, 16);
    unsigned int end   = strtoul(argv[2], 0, 16);
    char buf[256];
    for (unsigned int pc = start; pc < end; ) {
        unsigned int n = m68k_disassemble(buf, pc, M68K_CPU_TYPE_68000);
        printf("%06X: %s\n", pc, buf);
        pc += (n ? n : 2);
    }
    return 0;
}
