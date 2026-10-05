// Throwaway disassembler: loads the (byteswapped) Universe BIOS into the 68k
// address space at 0xC00000 (mirrored at 0) and disassembles a requested range
// using Musashi's m68kdasm.c. Build from mvs64/: cc -I. tools/disbios.c m68kdasm.c -o disbios
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m68k.h"

static unsigned char bios[0x20000]; // 68k-order bytes

static unsigned int rd(unsigned int a, int sz) {
    a &= 0xFFFFFF;
    unsigned int off;
    if (a >= 0xC00000 && a < 0xC20000) off = a - 0xC00000;
    else if (a < 0x20000) off = a;           // low mirror of BIOS vectors/code
    else return 0;
    unsigned int v = 0;
    for (int i = 0; i < sz; i++) v = (v << 8) | bios[(off + i) & 0x1FFFF];
    return v;
}
unsigned int m68k_read_disassembler_8 (unsigned int a) { return rd(a,1); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return rd(a,2); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return rd(a,4); }

int main(int argc, char **argv) {
    FILE *f = fopen("bios/uni-bios_4_0.rom", "rb");
    if (!f) { perror("bios"); return 1; }
    unsigned char raw[0x20000];
    fread(raw, 1, 0x20000, f); fclose(f);
    for (int i = 0; i < 0x20000; i += 2) { bios[i] = raw[i+1]; bios[i+1] = raw[i]; }

    unsigned int start = strtoul(argv[1], 0, 16);
    unsigned int end   = strtoul(argv[2], 0, 16);
    char buf[256];
    unsigned int pc = start;
    while (pc < end) {
        unsigned int n = m68k_disassemble(buf, pc, M68K_CPU_TYPE_68000);
        printf("%06X: %s\n", pc, buf);
        if (n == 0) n = 2;
        pc += n;
    }
    return 0;
}
