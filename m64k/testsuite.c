#include <libdragon.h>
#include <stdlib.h>
#include <stdalign.h>
#include "m64k.h"
#include "tlb.h"

m64k_t m64k;
uint8_t ram_pages[16][8192] __attribute__((aligned(8192)));
uint32_t ram_address[16];

static void m68k_ram_init(void) {
    memset(ram_address, 0xFF, sizeof(ram_address));
}

static void m68k_ram_w8(uint32_t addr, uint8_t v) {
    uint32_t page = addr & ~0x1FFF;

    for (int i=0;i<16;i++) {
        if (ram_address[i] == 0xFFFFFFFF) {
            ram_address[i] = page;
            memset(ram_pages[i], 0, 8192);
            m64k_map_memory(&m64k, page, 8192, ram_pages[i], true);
        }
        if (ram_address[i] == page) {
            ram_pages[i][addr & 0x1FFF] = v;
            return;
        }
    }
    assertf(0, "Out of RAM pages");
}

static uint8_t m68k_ram_r8(uint32_t addr) {
    uint32_t page = addr & ~0x1FFF;
    for (int i=0;i<16;i++) {
        if (ram_address[i] == page) {
            return ram_pages[i][addr & 0x1FFF];
        }
        if (ram_address[i] == 0xFFFFFFFF) {
            ram_address[i] = page;
            memset(ram_pages[i], 0, 8192);
            // debugf("Mapping RAM page %d at %08lx\n", i, page);
            m64k_map_memory(&m64k, page, 8192, ram_pages[i], true);
            return 0;
        }
    }
    assertf(0, "Out of RAM pages");
}

typedef struct  {
    uint32_t dregs[8];
    uint32_t aregs[7];
    uint32_t usp, ssp;
    uint32_t sr;
    uint32_t pc;
    uint32_t prefetch[2];
    uint32_t nrams;
    uint32_t ram[80][2];
} test_state_t;

void read_state(FILE *f, test_state_t *s)
{
    fread(s->dregs, 1, 32, f);
    fread(s->aregs, 1, 28, f);
    fread(&s->usp, 1, 4, f);
    fread(&s->ssp, 1, 4, f);
    fread(&s->sr, 1, 4, f);
    fread(&s->pc, 1, 4, f);
    fread(s->prefetch, 1, 8, f);
    fread(&s->nrams, 1, 4, f);
    assertf(s->nrams <= 80, "Too many RAM entries in test: %ld", s->nrams);
    for (int i=0; i<s->nrams; i++) {
        fread(s->ram[i], 1, 8, f);
    }
}

int total_cycle_total = 0;
int total_cycle_diff = 0;

void run_testsuite(const char *fn)
{
    debugf("Running testsuite: %s\n", fn);
    FILE *f = asset_fopen(fn, NULL);

    bool asl_test = strstr(fn, "ASL.b.") != NULL;
    bool asr_test = strstr(fn, "ASR.") != NULL;

    // We don't have cycle accurate timing for some tests
    bool approx_timing = (M64K_CONFIG_TIMING_ACCURACY <= 0);
    if (!approx_timing) {
        if (strstr(fn, "DIVU") || strstr(fn, "DIVS"))
            approx_timing = true;
    }

    // Read ID
    char id[4]; fread(id, 1, 4, f); (void)id;
    assert(id[0] == 'M' && id[1] == '6' && id[2] == '4' && id[3] == 'K');

    // Read number of tests
    uint32_t num_tests; fread(&num_tests, 1, 4, f);

    int cycle_total = 0;
    int cycle_diff = 0;
    int file_fails = 0;
    for (int t=0; t<num_tests; t++) {
        fread(id, 1, 4, f);
        assert(id[0] == 'T' && id[1] == 'E' && id[2] == 'S' && id[3] == 'T');

        uint32_t nl; fread(&nl, 1, 4, f);
        char *name = alloca(nl+1); fread(name, 1, nl, f); name[nl] = 0;
        uint32_t cycles; fread(&cycles, 1, 4, f);
        test_state_t initial, final;
        read_state(f, &initial);
        read_state(f, &final);

        // skip buggy tests
        // see https://github.com/TomHarte/ProcessorTests/issues/21
        if (asl_test && (t == 1583-1 || t == 1761-1)) continue;
        // if (t!=31-1) continue;
        // debugf("Running test: %s\n", name);

        // Run the test
        m64k_init(&m64k);
        memcpy(m64k.dregs, initial.dregs, sizeof(m64k.dregs));
        memcpy(m64k.aregs, initial.aregs, sizeof(m64k.aregs));
        m64k.usp = initial.usp;
        m64k.ssp = initial.ssp;
        m64k.pc = initial.pc;
        m64k.sr = initial.sr;

        bool address_error = false;
        m68k_ram_init();
        m68k_ram_w8(initial.pc+0, initial.prefetch[0] >> 8);
        m68k_ram_w8(initial.pc+1, initial.prefetch[0] & 0xff);
        m68k_ram_w8(initial.pc+2, initial.prefetch[1] >> 8);
        m68k_ram_w8(initial.pc+3, initial.prefetch[1] & 0xff);
        for (int i=0; i<initial.nrams; i++) {
            uint32_t addr = initial.ram[i][0];
            uint32_t value = initial.ram[i][1];
            m68k_ram_w8(addr, value);
            if (addr == 0xC) address_error = true;
            // debugf("RAM[%08lx] = %02lx\n", addr, value);
        }
        // Make sure also locations mentioned in final state are mapped
        for (int i=0; i<final.nrams; i++)
            (void)m68k_ram_r8(final.ram[i][0]);

        // Run the opcode
        #ifdef M64K_DYNREC
        // Forced-superblock mode: translate the vector's single insn with
        // the C_max gate skipped (vectors run with m_cycles=1, which the
        // gate would always refuse), so every supported-form vector
        // exercises the emitted block end-to-end via the slice-entry
        // probe. m64k_init above reset the table+arena, so blocks never
        // leak across vectors (they rewrite RAM at reused addresses).
        m64k_dyn_translate(&m64k, m64k.pc, 1, true);
        #endif
        int elapsed_cycles = m64k_run(&m64k, 1);

        // Check the results
        bool failed = false;
        for (int i=0; i<8; i++) {
            if (m64k.dregs[i] != final.dregs[i])  {
                if (!failed) debugf("Running test: %s\n", name);
                debugf("D%d: %08lx != %08lx\n", i, m64k.dregs[i], final.dregs[i]);
                failed = true;
            }
        }
        for (int i=0; i<7; i++) {
            if (m64k.aregs[i] != final.aregs[i])  {
                if (!failed) debugf("Running test: %s\n", name);
                debugf("A%d: %08lx != %08lx\n", i, m64k.aregs[i], final.aregs[i]);
                failed = true;
            }
        }
        if (m64k.usp != final.usp) {
            if (!failed) debugf("Running test: %s\n", name);
            debugf("USP: %08lx != %08lx\n", m64k.usp, final.usp);
            failed = true;
        }
        if (m64k.ssp != final.ssp) {
            if (!failed) debugf("Running test: %s\n", name);
            debugf("SSP: %08lx != %08lx\n", m64k.ssp, final.ssp);
            failed = true;
        }
        if(asr_test) {
            // buggy tests, ignore flags C and X
            m64k.sr &= ~0x11; final.sr &= ~0x11;
        }
        if (m64k.sr != final.sr) {
            if (!failed) debugf("Running test: %s\n", name);
            debugf("SR: %08lx != %08lx\n", m64k.sr, final.sr);
            failed = true;
        }
        if (m64k.pc != final.pc) {
            if (!failed) debugf("Running test: %s\n", name);
            debugf("PC: %08lx != %08lx\n", m64k.pc, final.pc);
            failed = true;
        }
        for (int i=0; i<final.nrams; i++) {
            uint8_t got = m68k_ram_r8(final.ram[i][0]);
            if (got != final.ram[i][1]) {
                if (!failed) debugf("Running test: %s\n", name);
                debugf("RAM[%lx] = %02x != %02lx\n", final.ram[i][0], got, final.ram[i][1]);
                failed = true;
            }
        }

        if (failed) {
            if (address_error) debugf("(this test is designed to trigger an address error)\n");
            // Don't abort: tally and move on so one run surfaces every broken
            // opcode. Stop a file after a few failures to keep the log readable.
            if (++file_fails >= 3) {
                debugf("(stopping %s after %d failures)\n", fn, file_fails);
                break;
            }
            continue;
        }

        // Check cycle count difference
        if (M64K_CONFIG_TIMING_ACCURACY >= 0 && !address_error) {
            if (!approx_timing) {
                if (elapsed_cycles != cycles) {

                    debugf("Running test: %s\n", name);
                    debugf("Cycle count: %d != %ld\n", elapsed_cycles, cycles);
                    abort();
                }
            } else {
                cycle_total += cycles;
                int diff = elapsed_cycles - cycles;
                if (diff < 0) diff = -diff;
                cycle_diff += diff;
            }
        }
    }

    if (M64K_CONFIG_TIMING_ACCURACY >= 0 && approx_timing && cycle_total) {
        total_cycle_total += cycle_total;
        total_cycle_diff += cycle_diff;
        debugf("Cycle count difference: %.2f%%\n", (double)cycle_diff * 100.0 / cycle_total);
    }

    // One-line verdict per opcode file, easy to grep from the ISViewer log.
    if (file_fails) debugf(">>> FAIL %s (%d failing tests)\n", fn, file_fails);
    else            debugf(">>> PASS %s\n", fn);

    fclose(f);
}

bool strendswith(const char *s, const char *suffix)
{
    int sl = strlen(s);
    int sufl = strlen(suffix);
    if (sl < sufl) return false;
    return strcmp(s+sl-sufl, suffix) == 0;
}

#ifdef M64K_DYNREC
// ---- Emitter-differential rig ------------------------------------------
// Proves dynarec template hand-encodings that the TomHarte vector set
// cannot cover (there are no immediate-form suites): each synthetic
// sequence runs twice from an identical seed state — once pure
// interpreter, once with a forced translated block — and the final
// architectural state (regs, SR, PC, USP/SSP) plus elapsed cycles must
// match bit-for-bit. Memory effects are verified by loading stored
// values back into registers within the sequence. Canary rule: a
// template counts as covered here only if corrupting its idiom makes a
// sequence FAIL (verified for ADDI/CMPI at rig introduction).
static int diff_fails, diff_runs;

static void diff_run(const uint16_t *code, int ncode, int nexpect,
                     int budget, const char *name)
{
    m64k_t st[2];
    for (int side = 0; side < 2; side++) {
        m64k_init(&m64k);               // also resets the dynrec table/arena
        m68k_ram_init();
        for (int i = 0; i < 8; i++)
            m64k.dregs[i] = (0x11111111u * i) ^ 0x8000;
        m64k.dregs[3] = 0;              // Z-flag material
        m64k.dregs[4] = 0x0000FFFF;     // carry/borrow material
        for (int i = 0; i < 7; i++)
            m64k.aregs[i] = 0x4000 + 0x100 * i;
        m64k.aregs[5] = 0x4501;         // odd: exercises the emitted
                                        // ADDRERR bail -> generic replay
        m64k.usp = 0x5000;
        m64k.ssp = 0x5800;
        m64k.pc = 0x2000;
        m64k.sr = 0x2700;
        for (int i = 0; i < ncode; i++) {
            m68k_ram_w8(0x2000 + i * 2,     code[i] >> 8);
            m68k_ram_w8(0x2000 + i * 2 + 1, code[i] & 0xFF);
        }
        (void)m68k_ram_r8(0x0000);      // map the vector page (ADDRERR)
        (void)m68k_ram_r8(0x4000);      // map the data/stack page
        if (side == 1) {
            int got = m64k_dyn_translate(&m64k, m64k.pc, nexpect, true);
            if (got != nexpect) {
                debugf(">>> DIFF FAIL %s: translated %d insns, expected %d\n",
                       name, got, nexpect);
                diff_fails++;
                diff_runs++;
                return;
            }
        }
        m64k_run(&m64k, budget);
        st[side] = m64k;
    }
    diff_runs++;
    bool ok = memcmp(st[0].dregs, st[1].dregs, sizeof(st[0].dregs)) == 0
           && memcmp(st[0].aregs, st[1].aregs, sizeof(st[0].aregs)) == 0
           && st[0].usp == st[1].usp && st[0].ssp == st[1].ssp
           && st[0].pc == st[1].pc && st[0].sr == st[1].sr
           && st[0].cycles == st[1].cycles;
    if (!ok) {
        diff_fails++;
        debugf(">>> DIFF FAIL %s\n", name);
        for (int i = 0; i < 8; i++)
            if (st[0].dregs[i] != st[1].dregs[i])
                debugf("  D%d: %08lx != %08lx\n", i, st[0].dregs[i], st[1].dregs[i]);
        for (int i = 0; i < 7; i++)
            if (st[0].aregs[i] != st[1].aregs[i])
                debugf("  A%d: %08lx != %08lx\n", i, st[0].aregs[i], st[1].aregs[i]);
        if (st[0].sr != st[1].sr)
            debugf("  SR: %04lx != %04lx\n", st[0].sr, st[1].sr);
        if (st[0].pc != st[1].pc)
            debugf("  PC: %08lx != %08lx\n", st[0].pc, st[1].pc);
        if (st[0].cycles != st[1].cycles)
            debugf("  cycles: %lld != %lld\n",
                   (long long)st[0].cycles, (long long)st[1].cycles);
    }
}

static void run_emitter_differential(void)
{
    // MOVEQ edges: zero (Z), -1 (N), positive; and a 3-insn run.
    diff_run((const uint16_t[]){0x7000}, 1, 1, 100, "moveq #0,d0");
    diff_run((const uint16_t[]){0x72FF}, 1, 1, 100, "moveq #-1,d1");
    diff_run((const uint16_t[]){0x747F, 0x7000, 0x76FF}, 3, 3, 100, "moveq x3");
    // MOVE.w forms (templated set), incl. store->load-back memory checks.
    diff_run((const uint16_t[]){0x3A03}, 1, 1, 100, "move.w d3,d5");
    diff_run((const uint16_t[]){0x3481, 0x3E12}, 2, 1, 100, "move.w d1,(a2); (a2),d7");
    diff_run((const uint16_t[]){0x36C0, 0x3419}, 2, 2, 100, "move.w d0,(a3)+; (a1)+,d2");
    diff_run((const uint16_t[]){0x3C28, 0x0008}, 2, 1, 100, "move.w (8,a0),d6");
    diff_run((const uint16_t[]){0x3942, 0x0004, 0x3C2C, 0x0004}, 4, 2, 100, "move.w d2,(4,a4); (4,a4),d6");
    diff_run((const uint16_t[]){0x3CBC, 0x8000, 0x3E16}, 3, 1, 100, "move.w #0x8000,(a6); (a6),d7");
    // ADDRERR bail from an emitted block (a5 is odd): the bail replays
    // the insn generically and raises the address error bit-exactly.
    diff_run((const uint16_t[]){0x3A84}, 1, 1, 200, "move.w d4,(a5) ADDRERR");
    // CMPI edges: equal (Z), borrow (C), byte form.
    diff_run((const uint16_t[]){0x0C42, 0x1111}, 2, 1, 100, "cmpi.w #0x1111,d2");
    diff_run((const uint16_t[]){0x0C43, 0x0000}, 2, 1, 100, "cmpi.w #0,d3 (Z)");
    diff_run((const uint16_t[]){0x0C42, 0xFFFF}, 2, 1, 100, "cmpi.w #0xFFFF,d2 (C)");
    diff_run((const uint16_t[]){0x0C04, 0x00FF}, 2, 1, 100, "cmpi.b #0xFF,d4");
    // ADDI.w edges: carry+X wrap, Z, sign/overflow.
    diff_run((const uint16_t[]){0x0644, 0xFFFF}, 2, 1, 100, "addi.w #0xFFFF,d4 (C/X)");
    diff_run((const uint16_t[]){0x0643, 0x0000}, 2, 1, 100, "addi.w #0,d3 (Z)");
    diff_run((const uint16_t[]){0x0640, 0x8000}, 2, 1, 100, "addi.w #0x8000,d0 (V)");
    // Mixed 6-insn block: sequencing + guest-length accounting.
    diff_run((const uint16_t[]){0x7001, 0x3A03, 0x0C42, 0x1111,
                                0x0644, 0xFFFF, 0x3419, 0x36C0},
             8, 6, 200, "mixed x6");
    // Bcc enders: taken/not-taken, forward/backward, both charge forms.
    diff_run((const uint16_t[]){0x7000, 0x6702, 0x7201, 0x7402}, 4, 2, 100,
             "moveq #0; beq.s +2 (taken)");
    diff_run((const uint16_t[]){0x7001, 0x6702, 0x74AA}, 3, 2, 100,
             "moveq #1; beq.s +2 (not taken)");
    diff_run((const uint16_t[]){0x6002, 0x7201, 0x7402}, 3, 1, 100,
             "bra.s +2");
    diff_run((const uint16_t[]){0x6700, 0x0004, 0x7201}, 3, 1, 100,
             "beq.w +4 (not taken, 12-cycle form)");
    diff_run((const uint16_t[]){0x6A02, 0x7201, 0x7402}, 3, 1, 100,
             "bpl.s +2 (N from seed flags)");
    // Backward loop: addi.w #-1,d0 decrements to Z; bne.s -6 loops once.
    diff_run((const uint16_t[]){0x7002, 0x0640, 0xFFFF, 0x66FA}, 4, 3, 200,
             "moveq #2; addi.w #-1,d0; bne.s -6 (loop)");
    debugf("%s emitter differential: %d sequences, %d fails\n",
           diff_fails ? ">>> DIFFRIG FAIL" : ">>> PASS", diff_runs, diff_fails);
}
#endif

int main()
{
    debug_init_isviewer();
    debug_init_usblog();

    dfs_init(DFS_DEFAULT_LOCATION);

    if (!M64K_CONFIG_ADDRERR) {
        debugf("\nWARNING: this testsuite requires address errors to be emulated.\n");
        debugf("Make sure to compile M64K with M64K_CONFIG_ADDRERR=1\n\n");
    }

#if 1
    // Auto-discover every .btest in the DFS so the test set is whatever was
    // dropped into assets/ at build time (no need to edit the list below).
    char* testfns[256];
    int num_tests = 0;

	char sbuf[1024];
	strcpy(sbuf, "rom:/");
	if (dfs_dir_findfirst(".", sbuf+5) == FLAGS_FILE) {
		do {
			if (strendswith(sbuf, ".btest")) {
                assert(num_tests < 256);
				testfns[num_tests++] = strdup(sbuf);
            }
		} while (dfs_dir_findnext(sbuf+5) == FLAGS_FILE);
	}
    debugf("Discovered %d .btest files\n", num_tests);
#else
    static const char *testfns[] = {
        "rom:/MOVEP.w.btest",
        "rom:/MOVEP.l.btest",

        "rom:/TRAPV.btest",
        "rom:/TAS.btest",

        "rom:/ABCD.btest",
        "rom:/SBCD.btest",
        "rom:/NBCD.btest",

        "rom:/ROXL.b.btest",
        "rom:/ROXL.l.btest",
        "rom:/ROXL.w.btest",
        "rom:/ROXR.b.btest",
        "rom:/ROXR.l.btest",
        "rom:/ROXR.w.btest",
        "rom:/ROL.b.btest",
        "rom:/ROL.l.btest",
        "rom:/ROL.w.btest",
        "rom:/ROR.b.btest",
        "rom:/ROR.l.btest",
        "rom:/ROR.w.btest",

        "rom:/ASL.b.btest",
        "rom:/ASL.l.btest",
        "rom:/ASL.w.btest",
        "rom:/ASR.b.btest",   // these seem too buggy
        "rom:/ASR.l.btest",   // these seem too buggy
        "rom:/ASR.w.btest",   // these seem too buggy
        "rom:/LSR.b.btest",
        "rom:/LSR.l.btest",
        "rom:/LSR.w.btest",
        "rom:/LSL.b.btest",
        "rom:/LSL.l.btest",
        "rom:/LSL.w.btest",

        "rom:/TRAP.btest",
        "rom:/EXG.btest",

        "rom:/BTST.btest",
        "rom:/BCHG.btest",
        "rom:/BSET.btest",
        "rom:/BCLR.btest",

        "rom:/CMPA.w.btest",
        "rom:/CMPA.l.btest",
        "rom:/CMP.b.btest",
        "rom:/CMP.w.btest",
        "rom:/CMP.l.btest",

        "rom:/EXT.l.btest",
        "rom:/EXT.w.btest",

        "rom:/MOVEM.w.btest",
        "rom:/MOVEM.l.btest",

        "rom:/DIVS.btest",
        "rom:/DIVU.btest",

        "rom:/MULS.btest",
        "rom:/MULU.btest",

        "rom:/DBcc.btest",

        "rom:/RTE.btest",
        "rom:/RTR.btest",
        "rom:/RTS.btest",

        "rom:/NOP.btest",
        "rom:/RESET.btest",

        "rom:/LINK.btest",
        "rom:/UNLINK.btest",

        "rom:/BSR.btest",
        "rom:/JMP.btest",
        "rom:/JSR.btest",
        "rom:/Bcc.btest",
        "rom:/Scc.btest",

        "rom:/SWAP.btest",

        "rom:/EORItoCCR.btest",
        "rom:/EORItoSR.btest",
        "rom:/ORItoCCR.btest",
        "rom:/ORItoSR.btest",
        "rom:/ANDItoCCR.btest",
        "rom:/ANDItoSR.btest",
        "rom:/MOVEfromSR.btest",
        "rom:/MOVEfromUSP.btest",
        "rom:/MOVEtoSR.btest",
        "rom:/MOVEtoUSP.btest",
        "rom:/MOVEtoCCR.btest",

        "rom:/TST.b.btest",
        "rom:/TST.l.btest",
        "rom:/TST.w.btest",

        "rom:/NEGX.b.btest",
        "rom:/NEGX.l.btest",
        "rom:/NEGX.w.btest",
        "rom:/NEG.b.btest",
        "rom:/NEG.l.btest",
        "rom:/NEG.w.btest",

        "rom:/NOT.b.btest",
        "rom:/NOT.l.btest",
        "rom:/NOT.w.btest",

        "rom:/CLR.b.btest",
        "rom:/CLR.l.btest",
        "rom:/CLR.w.btest",

        "rom:/PEA.btest",
        "rom:/LEA.btest",

        "rom:/AND.b.btest",
        "rom:/AND.l.btest",
        "rom:/AND.w.btest",
        "rom:/OR.b.btest",
        "rom:/OR.l.btest",
        "rom:/OR.w.btest",
        "rom:/EOR.b.btest",
        "rom:/EOR.l.btest",
        "rom:/EOR.w.btest",

        "rom:/ADD.b.btest",
        "rom:/ADD.l.btest",
        "rom:/ADD.w.btest",
        "rom:/ADDX.b.btest",
        "rom:/ADDX.l.btest",
        "rom:/ADDX.w.btest",
        "rom:/ADDA.l.btest",
        "rom:/ADDA.w.btest",

        "rom:/SUB.b.btest",
        "rom:/SUB.l.btest",
        "rom:/SUB.w.btest",
        "rom:/SUBX.b.btest",
        "rom:/SUBX.l.btest",
        "rom:/SUBX.w.btest",
        "rom:/SUBA.l.btest",
        "rom:/SUBA.w.btest",

        "rom:/MOVE.b.btest",
        "rom:/MOVE.w.btest",
        "rom:/MOVE.l.btest",
        "rom:/MOVE.q.btest",
        "rom:/MOVEA.w.btest",
        "rom:/MOVEA.l.btest",
    };
    int num_tests = sizeof(testfns)/sizeof(testfns[0]);
#endif

    #ifdef M64K_DYNREC
    run_emitter_differential();
    #endif

    for (int i=0; i<num_tests; i++) {
        run_testsuite(testfns[i]);
    }

    debugf("Finished testsuite\n");
    if (M64K_CONFIG_TIMING_ACCURACY == 0) {
        debugf("TOTAL: Cycle count difference: %.2f%%\n", (double)total_cycle_diff * 100.0 / total_cycle_total);
    }
}
