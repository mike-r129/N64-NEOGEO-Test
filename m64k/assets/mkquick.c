/* mkquick.c - synthesize TomHarte-schema SingleStepTests vectors for opcodes
 * that the upstream ProcessorTests/680x0 68000 set does not cover (ADDQ, SUBQ).
 *
 * The upstream TomHarte 68000 suite has files for ADD/ADDA/ADDX/SUB/... but no
 * ADDQ or SUBQ. This tool closes that gap so the m64k testsuite can gate future
 * changes to those hot-path ALU ops.
 *
 * Oracle: the vendored Musashi 68000 core (../m68kcpu.c, ../m68kops.c). Musashi
 * is the reference behaviour this project trusts (m64k is validated against it,
 * and Musashi matches real-silicon TomHarte vectors). For each randomized case
 * we set up CPU + memory state, execute exactly one instruction, and record the
 * before/after state in the exact JSON schema that assets/mktest.go consumes.
 *
 * Output: TomHarte-schema JSON array on stdout (pipe through gzip to make a
 * *.json.gz, then run mktest.go to convert to *.btest like every other file).
 *
 * Usage:  ./mkquick ADDQ   (or SUBQ)   [count]   [seed]
 *
 * Coverage mirrors the TomHarte style: all sizes (b/w/l), immediate data 1..8,
 * and every legal destination EA mode: Dn, An (w/l only - byte to An is illegal,
 * and An updates the whole 32-bit register with no CCR change), (An), (An)+,
 * -(An), d16(An), d8(An,Xn), abs.w, abs.l. CCR/X and the S bit are randomized.
 * The T (trace) bit is forced 0: Musashi is built with trace off here, and a
 * trace exception would exercise exception dispatch, not the ADDQ/SUBQ ALU path.
 *
 * EA bases are restricted to A0-A6 (never A7) so we never disturb USP/SSP; the
 * A7-relative byte SP-adjust quirk is already covered by the ADD/SUB TomHarte
 * files. All word/long memory EAs are forced even because the testsuite ROM is
 * built with M64K_CONFIG_ADDRERR=1 (m64k traps odd word/long access) while this
 * Musashi build has address-error emulation off.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "../../m68k.h"

#define MEMSIZE   0x1000000u   /* 16 MB (24-bit address space) */
#define LOWWIN    0x10000u     /* window we track for RAM entries */
#define PC_ADDR   0x1000u      /* where the instruction lives */

/* Referenced by this fork's Musashi idle-skip hook (m68kinline.h). Normally
 * defined in roms.c; provide it here so we can link the core standalone. */
unsigned int rom_pc_idle_skip = 0;

static uint8_t  MEM[MEMSIZE];
static uint8_t  MEM_INIT[LOWWIN];
static uint8_t  touched[LOWWIN];
static uint32_t gPC;

/* ---- Musashi memory interface: all reads/writes funnel through here ---- */
static void mark(uint32_t addr) {
    if (addr >= LOWWIN) return;
    /* opcode word (PC,PC+1) and prefetch[1] word (PC+2,PC+3) are supplied via
     * the prefetch[] fields, not the ram[] array - exclude them. */
    if (addr >= gPC && addr < gPC + 4) return;
    touched[addr] = 1;
}

unsigned int m68k_read_memory_8(unsigned int a) {
    a &= 0xFFFFFF; mark(a);
    return MEM[a];
}
unsigned int m68k_read_memory_16(unsigned int a) {
    a &= 0xFFFFFF; mark(a); mark(a+1);
    return (MEM[a] << 8) | MEM[a+1];
}
unsigned int m68k_read_memory_32(unsigned int a) {
    a &= 0xFFFFFF; mark(a); mark(a+1); mark(a+2); mark(a+3);
    return (MEM[a] << 24) | (MEM[a+1] << 16) | (MEM[a+2] << 8) | MEM[a+3];
}
void m68k_write_memory_8(unsigned int a, unsigned int v) {
    a &= 0xFFFFFF; if (a < LOWWIN) touched[a] = 1;
    MEM[a] = v & 0xFF;
}
void m68k_write_memory_16(unsigned int a, unsigned int v) {
    a &= 0xFFFFFF;
    if (a < LOWWIN)   touched[a]   = 1;
    if (a+1 < LOWWIN) touched[a+1] = 1;
    MEM[a] = (v >> 8) & 0xFF; MEM[a+1] = v & 0xFF;
}
void m68k_write_memory_32(unsigned int a, unsigned int v) {
    a &= 0xFFFFFF;
    for (int i = 0; i < 4; i++) if (a+i < LOWWIN) touched[a+i] = 1;
    MEM[a] = (v>>24)&0xFF; MEM[a+1] = (v>>16)&0xFF;
    MEM[a+2] = (v>>8)&0xFF; MEM[a+3] = v&0xFF;
}
/* Disassembler hooks (linked from m68kdasm.c but never called). */
unsigned int m68k_read_disassembler_8 (unsigned int a){ return MEM[a & 0xFFFFFF]; }
unsigned int m68k_read_disassembler_16(unsigned int a){ a&=0xFFFFFF; return (MEM[a]<<8)|MEM[a+1]; }
unsigned int m68k_read_disassembler_32(unsigned int a){ a&=0xFFFFFF; return (MEM[a]<<24)|(MEM[a+1]<<16)|(MEM[a+2]<<8)|MEM[a+3]; }

/* ---- helpers ---- */
static uint32_t r32(void) { /* full 32-bit random */
    return ((uint32_t)(rand() & 0xFFFF) << 16) | (uint32_t)(rand() & 0xFFFF);
}
static uint32_t reven(uint32_t range) { /* even value in [0, range) */
    uint32_t half = range / 2; if (half == 0) half = 1;
    return (uint32_t)(rand() % half) * 2u;
}
static void poke16(uint32_t a, uint16_t v) { MEM[a] = v >> 8; MEM[a+1] = v & 0xFF; }

/* EA mode ids */
enum { M_DN, M_AN, M_AI, M_PI, M_PD, M_DI, M_IX, M_AW, M_AL, M_COUNT };
static const char *ea_names[] = {
    "Dn","An","(An)","(An)+","-(An)","d16(An)","d8(An,Xn)","abs.w","abs.l"
};
static const char *sz_names[] = { "b","w","l" };

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s ADDQ|SUBQ [count] [seed]\n", argv[0]); return 2; }
    int is_sub = 0;
    if      (!strcmp(argv[1], "ADDQ")) is_sub = 0;
    else if (!strcmp(argv[1], "SUBQ")) is_sub = 1;
    else { fprintf(stderr, "opcode must be ADDQ or SUBQ\n"); return 2; }
    int count = (argc > 2) ? atoi(argv[2]) : 600;
    unsigned seed = (argc > 3) ? (unsigned)strtoul(argv[3], 0, 0) : 0xADDABEEF ^ is_sub;
    srand(seed);

    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    memset(MEM, 0, 8);          /* reset vector: SSP=0, PC=0 */
    m68k_pulse_reset();
    /* The first m68k_execute() after a reset only burns the reset cycle count
     * (it returns without executing an instruction). Drain it here so every
     * per-test m68k_execute(1) below runs exactly one real instruction. */
    m68k_execute(0);

    printf("[\n");
    for (int t = 0; t < count; t++) {
        /* pick destination EA mode + size */
        int mode = rand() % M_COUNT;
        int size;                                   /* 0=b,1=w,2=l */
        if (mode == M_AN) size = 1 + (rand() & 1);  /* w or l only */
        else              size = rand() % 3;
        int data = 1 + (rand() % 8);                /* immediate 1..8 */

        /* randomize architectural state */
        uint32_t D[8], A[7], usp, ssp, sr;
        for (int i = 0; i < 8; i++) D[i] = r32();
        for (int i = 0; i < 7; i++) A[i] = r32();
        usp = r32() & ~1u;
        ssp = r32() & ~1u;
        sr  = r32() & 0x271F;                       /* S | intmask | CCR, no T */

        /* fill instruction region with NOP filler, operand region with noise */
        for (uint32_t a = PC_ADDR; a < PC_ADDR + 0x40; a += 2) poke16(a, 0x4E71);
        for (uint32_t a = 0x2000; a < LOWWIN; a++) MEM[a] = rand() & 0xFF;

        int regfield;                               /* low 3 bits of opcode */
        int modebits;                               /* mode bits of opcode */
        uint16_t ext[2]; int next = 0;              /* extension words */

        switch (mode) {
        case M_DN: { int dn = rand() % 8; modebits = 0; regfield = dn; } break;
        case M_AN: { int an = rand() % 7; modebits = 1; regfield = an; } break;
        case M_AI: { int an = rand() % 7; modebits = 2; regfield = an;
                     A[an] = 0x8000 + reven(0x800); } break;
        case M_PI: { int an = rand() % 7; modebits = 3; regfield = an;
                     A[an] = 0x8000 + reven(0x800); } break;
        case M_PD: { int an = rand() % 7; modebits = 4; regfield = an;
                     A[an] = 0x8100 + reven(0x800); } break;   /* room to predec */
        case M_DI: { int an = rand() % 7; modebits = 5; regfield = an;
                     A[an] = 0x8000; ext[next++] = (uint16_t)reven(0x800); } break;
        case M_IX: { int an = rand() % 7; modebits = 6; regfield = an;
                     A[an] = 0x8000;
                     int xi = rand() % 8; D[xi] = reven(0x400);
                     uint16_t disp8 = (uint16_t)reven(0x40);
                     /* brief ext: Dn index, long size, scale 0 */
                     ext[next++] = (uint16_t)((0 << 15) | (xi << 12) | (1 << 11) | (disp8 & 0xFF));
                   } break;
        case M_AW: { modebits = 7; regfield = 0;
                     ext[next++] = (uint16_t)(0x2000 + reven(0x800)); } break;   /* positive 16-bit */
        case M_AL: { modebits = 7; regfield = 1;
                     uint32_t addr = 0x8000 + reven(0x1000);
                     ext[next++] = (uint16_t)(addr >> 16);
                     ext[next++] = (uint16_t)(addr & 0xFFFF); } break;
        default: modebits = 0; regfield = 0; break;
        }

        uint16_t opcode = 0x5000
                        | ((uint16_t)(data & 7) << 9)     /* data 8 -> field 0 */
                        | ((uint16_t)is_sub << 8)
                        | ((uint16_t)size << 6)
                        | ((uint16_t)modebits << 3)
                        | (uint16_t)regfield;

        poke16(PC_ADDR, opcode);
        for (int i = 0; i < next; i++) poke16(PC_ADDR + 2 + 2*i, ext[i]);

        /* snapshot the low window so we can report initial RAM byte values */
        memcpy(MEM_INIT, MEM, LOWWIN);
        memset(touched, 0, LOWWIN);
        gPC = PC_ADDR;

        /* load CPU state (order matters: SR first, then USP/ISP swap correctly) */
        m68k_set_reg(M68K_REG_SR, sr);
        m68k_set_reg(M68K_REG_USP, usp);
        m68k_set_reg(M68K_REG_ISP, ssp);
        for (int i = 0; i < 8; i++) m68k_set_reg(M68K_REG_D0 + i, D[i]);
        for (int i = 0; i < 7; i++) m68k_set_reg(M68K_REG_A0 + i, A[i]);
        m68k_set_reg(M68K_REG_PC, PC_ADDR);

        uint16_t pref0 = opcode;
        uint16_t pref1 = (MEM[PC_ADDR+2] << 8) | MEM[PC_ADDR+3];

        /* run exactly one instruction */
        int cycles = m68k_execute(1);

        /* read back final state */
        uint32_t fD[8], fA[7], fusp, fssp, fsr, fpc;
        for (int i = 0; i < 8; i++) fD[i] = m68k_get_reg(0, M68K_REG_D0 + i);
        for (int i = 0; i < 7; i++) fA[i] = m68k_get_reg(0, M68K_REG_A0 + i);
        fusp = m68k_get_reg(0, M68K_REG_USP);
        fssp = m68k_get_reg(0, M68K_REG_ISP);
        fsr  = m68k_get_reg(0, M68K_REG_SR);
        fpc  = m68k_get_reg(0, M68K_REG_PC);
        uint16_t fpref0 = (MEM[fpc]   << 8) | MEM[fpc+1];
        uint16_t fpref1 = (MEM[fpc+2] << 8) | MEM[fpc+3];

        /* emit JSON */
        char nm[128];
        snprintf(nm, sizeof nm, "%s.%s #%d,%s [%04X] %d",
                 is_sub ? "SUBQ" : "ADDQ", sz_names[size], data,
                 ea_names[mode], opcode, t);

        printf("%s {\n", t ? "," : "");
        printf("  \"name\": \"%s\",\n", nm);

        for (int half = 0; half < 2; half++) {
            const char *key = half ? "final" : "initial";
            uint32_t *pD  = half ? fD  : D;
            uint32_t *pA  = half ? fA  : A;
            uint32_t pusp = half ? fusp : usp;
            uint32_t pssp = half ? fssp : ssp;
            uint32_t psr  = half ? fsr  : sr;
            uint32_t ppc  = half ? fpc  : PC_ADDR;
            uint16_t q0   = half ? fpref0 : pref0;
            uint16_t q1   = half ? fpref1 : pref1;

            printf("  \"%s\": {\n", key);
            for (int i = 0; i < 8; i++) printf("   \"d%d\": %u,\n", i, pD[i]);
            for (int i = 0; i < 7; i++) printf("   \"a%d\": %u,\n", i, pA[i]);
            printf("   \"usp\": %u,\n", pusp);
            printf("   \"ssp\": %u,\n", pssp);
            printf("   \"sr\": %u,\n",  psr);
            printf("   \"pc\": %u,\n",  ppc);
            printf("   \"prefetch\": [%u, %u],\n", q0, q1);
            printf("   \"ram\": [");
            int first = 1;
            for (uint32_t a = 0; a < LOWWIN; a++) {
                if (!touched[a]) continue;
                uint32_t val = half ? MEM[a] : MEM_INIT[a];
                printf("%s[%u, %u]", first ? "" : ", ", a, val);
                first = 0;
            }
            printf("]\n");
            printf("  },\n");
        }
        printf("  \"length\": %d\n", cycles);
        printf(" }\n");
    }
    printf("]\n");
    return 0;
}
