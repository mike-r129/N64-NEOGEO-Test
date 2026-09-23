// THROWAWAY INSTRUMENTATION (untracked). 68000 opcode/PC histogram profiler for
// the PC reference build. Auto-compiled via the hle_*.c wildcard in
// Makefile.pctests. Registered from emu.c via prof_init(); gated to an in-match
// frame window via MVS64_PROF_START / MVS64_PROF_END. Dumps CSV/txt at exit.
#ifndef N64
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "m68k.h"

// Musashi per-opcode cycle table, populated in m68k_init() and INCLUDING the
// effective-address cycle costs (see m68kops.c build loop). [0] = 68000.
extern unsigned char m68ki_cycles[][0x10000];

#define PC_SPAN 0x200000u

static uint64_t *op_all, *op_match;        // [65536]
static uint32_t *pc_all, *pc_match;        // [PC_SPAN/2] indexed by pc>>1
static int prof_start = -1, prof_end = -1; // inclusive window (frames)
static int prof_active = 0;
static uint64_t n_all = 0, n_match = 0;

// DBRA copy-loop A/D-register sampling (region around 0x3200)
#define DB_LO 0x31C0u
#define DB_HI 0x3260u
#define DB_MAX 200
static int db_samples = 0;
static FILE *db_fp = NULL;

// Branch-outcome tracking (in-match only): resolve the previous instruction's
// outcome by comparing the actual next PC to the fall-through PC.
static unsigned int prev_pc = 0, prev_op = 0, prev_len = 0, prev_pending = 0;
static uint64_t bcc_taken = 0, bcc_nottaken = 0;     // Bcc conditional (not BRA/BSR)
static uint64_t dbcc_looped = 0, dbcc_fell = 0;       // DBcc: looped back vs fell through

void prof_hook(unsigned int pc);
void prof_init(void);
void prof_set_frame(int f);
void prof_dump(void);

static int op_is_bcc(unsigned int op) {   // 0x6200..0x6F00 = conditional Bcc
    return (op & 0xF000) == 0x6000 && (op & 0x0F00) >= 0x0200;
}
static int op_is_dbcc(unsigned int op) {  // 0101 cccc 11001 rrr
    return (op & 0xF0F8) == 0x50C8;
}
static unsigned int bcc_len(unsigned int op) {
    unsigned int d8 = op & 0xFF;
    if (d8 == 0x00) return 4;   // 16-bit displacement
    if (d8 == 0xFF) return 6;   // 32-bit displacement
    return 2;                    // 8-bit displacement
}

void prof_init(void) {
    op_all   = calloc(65536, sizeof(uint64_t));
    op_match = calloc(65536, sizeof(uint64_t));
    pc_all   = calloc(PC_SPAN/2, sizeof(uint32_t));
    pc_match = calloc(PC_SPAN/2, sizeof(uint32_t));
    const char *s = getenv("MVS64_PROF_START");
    const char *e = getenv("MVS64_PROF_END");
    prof_start = s ? atoi(s) : -1;
    prof_end   = e ? atoi(e) : -1;
    m68k_set_instr_hook_callback(prof_hook);
    fprintf(stderr, "[PROF] init: window frames [%d..%d] (-1 = whole run)\n",
            prof_start, prof_end);
}

void prof_set_frame(int f) {
    prof_active = (prof_start < 0) ? 1 : (f >= prof_start && f <= prof_end);
}

void prof_hook(unsigned int pc) {
    unsigned int a = pc & 0xFFFFFF;
    unsigned int op = m68k_read_disassembler_16(a) & 0xFFFF;
    op_all[op]++; n_all++;
    if (a < PC_SPAN) pc_all[a >> 1]++;
    if (prof_active) {
        // Resolve outcome of the previous branch instruction.
        if (prev_pending) {
            int taken = (a != ((prev_pc + prev_len) & 0xFFFFFF));
            if (op_is_bcc(prev_op)) { if (taken) bcc_taken++; else bcc_nottaken++; }
            else /* dbcc */          { if (taken) dbcc_looped++; else dbcc_fell++; }
            prev_pending = 0;
        }
        if (op_is_bcc(op)) { prev_pending = 1; prev_len = bcc_len(op); }
        else if (op_is_dbcc(op)) { prev_pending = 1; prev_len = 4; }
        prev_pc = a; prev_op = op;

        op_match[op]++; n_match++;
        if (a < PC_SPAN) pc_match[a >> 1]++;
        if (a >= DB_LO && a < DB_HI && db_samples < DB_MAX) {
            if (!db_fp) db_fp = fopen("prof_dbra_areg.txt", "w");
            if (db_fp) {
                fprintf(db_fp,
                    "pc=%06x op=%04x A0=%08x A1=%08x A2=%08x A3=%08x A4=%08x A5=%08x D0=%08x D1=%08x D7=%08x\n",
                    a, op,
                    m68k_get_reg(NULL, M68K_REG_A0), m68k_get_reg(NULL, M68K_REG_A1),
                    m68k_get_reg(NULL, M68K_REG_A2), m68k_get_reg(NULL, M68K_REG_A3),
                    m68k_get_reg(NULL, M68K_REG_A4), m68k_get_reg(NULL, M68K_REG_A5),
                    m68k_get_reg(NULL, M68K_REG_D0), m68k_get_reg(NULL, M68K_REG_D1),
                    m68k_get_reg(NULL, M68K_REG_D7));
                db_samples++;
            }
        }
    }
}

typedef struct { unsigned int key; uint64_t cnt; } Ent;
static int cmp_ent(const void *pa, const void *pb) {
    const Ent *x = pa, *y = pb;
    if (y->cnt > x->cnt) return 1;
    if (y->cnt < x->cnt) return -1;
    return 0;
}

static void dump_op_csv(const char *path, uint64_t *tbl) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "opcode,count,basecyc\n");
    for (unsigned int op = 0; op < 65536; op++)
        if (tbl[op])
            fprintf(f, "%04x,%llu,%u\n", op,
                    (unsigned long long)tbl[op], (unsigned)m68ki_cycles[0][op]);
    fclose(f);
}

static void dump_pc_top(const char *path, uint32_t *tbl, int topn, uint64_t total) {
    // collect nonzero
    int cap = 0;
    for (unsigned int i = 0; i < PC_SPAN/2; i++) if (tbl[i]) cap++;
    Ent *ents = malloc(sizeof(Ent) * (cap ? cap : 1));
    int n = 0;
    for (unsigned int i = 0; i < PC_SPAN/2; i++)
        if (tbl[i]) { ents[n].key = i << 1; ents[n].cnt = tbl[i]; n++; }
    qsort(ents, n, sizeof(Ent), cmp_ent);
    FILE *f = fopen(path, "w");
    if (!f) { free(ents); return; }
    fprintf(f, "# rank pc count pct disasm   (total in-scope insns=%llu)\n",
            (unsigned long long)total);
    char buf[256];
    int lim = n < topn ? n : topn;
    for (int i = 0; i < lim; i++) {
        m68k_disassemble(buf, ents[i].key, M68K_CPU_TYPE_68000);
        fprintf(f, "%3d %06x %12llu %6.3f%%  %s\n", i + 1, ents[i].key,
                (unsigned long long)ents[i].cnt,
                total ? 100.0 * ents[i].cnt / total : 0.0, buf);
    }
    fclose(f);
    free(ents);
}

void prof_dump(void) {
    if (db_fp) fclose(db_fp);
    fprintf(stderr, "[PROF] dump: n_all=%llu n_match=%llu\n",
            (unsigned long long)n_all, (unsigned long long)n_match);
    FILE *s = fopen("prof_summary.txt", "w");
    if (s) {
        fprintf(s, "n_all=%llu\nn_match=%llu\nwindow=[%d..%d]\n",
                (unsigned long long)n_all, (unsigned long long)n_match,
                prof_start, prof_end);
        fprintf(s, "bcc_taken=%llu\nbcc_nottaken=%llu\n",
                (unsigned long long)bcc_taken, (unsigned long long)bcc_nottaken);
        fprintf(s, "dbcc_looped=%llu\ndbcc_fell=%llu\n",
                (unsigned long long)dbcc_looped, (unsigned long long)dbcc_fell);
        fclose(s);
    }
    dump_op_csv("prof_op_all.csv", op_all);
    dump_op_csv("prof_op_match.csv", op_match);
    dump_pc_top("prof_pc_all_top.txt", pc_all, 80, n_all);
    dump_pc_top("prof_pc_match_top.txt", pc_match, 80, n_match);
    fprintf(stderr, "[PROF] wrote prof_op_{all,match}.csv prof_pc_{all,match}_top.txt "
                    "prof_dbra_areg.txt prof_summary.txt\n");
}
#endif
