// m64k dynarec phase-2a: per-form MIPS emitter (DYNREC-PHASE2-TEMPLATES.md).
// Skeleton increment: MOVEQ only, straight-line blocks, no game-side
// translation trigger yet (the testsuite's forced mode is the only caller).
// NOTE (diagnostic counters): emitted blocks do not bump perf_m68k_insns /
// OPHIST â€” instruction counts drift low in instrumented runs with blocks
// active; use interpreter builds for those measurements.
#include "m64k.h"
#include "m64k_internal.h"
#include "m64k_config.h"
#include <libdragon.h>
#include <string.h>

#ifdef M64K_DYNREC

// Storage probed/ranged by m64k_asm.S and hw_n64.S (see phase-1 commit).
// LAYOUT-INVARIANCE LAW (2026-08-07): every resize of dynrec statics
// shifts the .bss behind them, changing the dcache/icache set alignment
// of unrelated hot data — measured as ±10-15fps swings that masqueraded
// as dynarec regressions (three different failure signatures across
// adjacent commits: snd% or m68k% doubling with identical guest work).
// Therefore: the big arrays are 8KB-aligned (deterministic dcache-set
// mapping) and ALL small dynrec statics live inside one fixed-size padded
// block so future dynrec changes never move other .bss.
uint8_t  __m64k_dyn_arena[M64K_DYN_ARENA_SIZE] __attribute__((aligned(8192)));
uint32_t __m64k_dyn_table[M64K_DYN_TABLE_SETS * 4] __attribute__((aligned(8192)));

static struct dyn_statics {
    uint32_t arena_used;
    int      npending;
    int      rc_enable;    // register-cache rung 1 (set once at init; lives
                           // here so the knob shifts no .bss — layout law)
    int      cov_enable;   // coverage-rung templates (fps twin knob)
    struct { uint32_t *slot; uint32_t target; } pending[512];
    uint8_t  tried[2048];
    uint8_t  pad[8192 - 16 - 512 * 8 - 2048];  // keep sizeof == 8KB forever
} __attribute__((aligned(8192))) dyn_s;
#define dyn_arena_used (dyn_s.arena_used)
#define dyn_npending   (dyn_s.npending)
#define dyn_pending    (dyn_s.pending)
#define dyn_tried      (dyn_s.tried)

// Exact tried-tag table (see dyn_tried_test_set): 16KB, 8KB-aligned per
// the layout law's big-array rule. Global (not static) because the
// jmp_exec miss path reads it to keep known-refused heads out of the
// first-writer sample slot (m64k_asm.S); same object, same placement.
uint16_t __m64k_dyn_tried2[8192] __attribute__((aligned(8192)));
#define dyn_tried2 __m64k_dyn_tried2
_Static_assert(sizeof(struct dyn_statics) == 8192, "dynrec statics must stay 8KB");

// Runtime translation gate: compile-time default only differs by one
// initializer constant, so ON and OFF binaries are layout-identical —
// the only clean A/B this platform allows.
#ifdef M64K_DYN_DISABLE
int __m64k_dyn_enable = 0;
#else
int __m64k_dyn_enable = 1;
#endif

extern char main_loop[];
// jmp_exec (m64k_asm.S): maps a 32-bit guest target into m_pc, updates
// pc_diff, raises the PC address error, runs the idle-skip compares and
// the dynarec probe. The RTS ender tail-jumps here with eaptr32 set.
extern char jmp_exec[];

// Publish primitive: emitted code must be written back from dcache and the
// stale icache lines invalidated BEFORE the table tag makes it reachable.
void __m64k_dyn_publish(void *dst, const void *src, int len)
{
    memcpy(dst, src, len);
    data_cache_hit_writeback(dst, len);
    inst_cache_hit_invalidate(dst, len);
}

// Translation-trigger mailboxes: the jmp_exec probe's miss edge stores the
// missed target in [0] (last-writer-wins within a slice) and every 61st
// miss in [1] ([2] holds the countdown) — an unbiased sample of the miss
// population, so hot mid-frame heads the slice-end-biased lottery never
// picks (the 00cee0 VRAM-upload function's inner loops, 80k misses/window)
// get attempted proportionally to their miss share. m64k_run services both
// at slice boundaries (the deferred-work law: never translate/reclaim
// while guest code may be mid-flight in the arena).
uint32_t __m64k_dyn_mailbox[3];
// (tried-filter, pending chain links and arena bump pointer live in the
// fixed-size dyn_s block above — layout-invariance law.)

// Visibility counters ([DYNSTAT2] in emu.c, M64K_DYNSTAT builds): how many
// blocks/insns actually translate in-game and whether chains ever resolve
// (2026-08-07: zero chains resolved in 240s attract — density-limited).
uint32_t __m64k_dyn_stat_blocks, __m64k_dyn_stat_insns;
uint32_t __m64k_dyn_stat_chains, __m64k_dyn_stat_refused;
// EXECUTED guest insns inside blocks (emitted bump at every block entry,
// DYNSTAT builds only). Translated-block counts say nothing about
// residency; this over frames-in-window vs the ~8-11k insns/frame the
// interpreter dispatches is the only number that predicts an fps win.
uint32_t __m64k_dyn_stat_exec;

void __m64k_dynrec_init(void)
{
    memset(__m64k_dyn_table, 0xFF, sizeof(__m64k_dyn_table));
    memset(dyn_tried2, 0, sizeof(dyn_tried2));
    __m64k_dyn_mailbox[0] = 0;
    __m64k_dyn_mailbox[1] = 0;
    __m64k_dyn_mailbox[2] = 61;   // sampling countdown (see mailbox comment)
    dyn_arena_used = 0;
    dyn_npending = 0;
    // Register-cache twin knob: compile-time default differs by one
    // immediate only, so ON and OFF binaries stay layout-identical.
#ifdef M64K_DYN_RC_DISABLE
    dyn_s.rc_enable = 0;
#else
    dyn_s.rc_enable = 1;
#endif
#ifdef M64K_DYN_COV_DISABLE
    dyn_s.cov_enable = 0;
#else
    dyn_s.cov_enable = 1;
#endif
}

static uint32_t *dyn_lookup(uint32_t pc)
{
    uint32_t *set = &__m64k_dyn_table[((pc >> 1) & (M64K_DYN_TABLE_SETS - 1)) * 4];
    if (set[0] == pc) return (uint32_t *)(uintptr_t)set[1];
    if (set[2] == pc) return (uint32_t *)(uintptr_t)set[3];
    return NULL;
}

// ---- MIPS encoders -------------------------------------------------------
#define R_ZERO 0
#define R_A0   4   // ctx
#define R_A1   5   // m_cycles (LIVE)
#define R_A2   6   // mmap_mask (map_m68k = or with it at MEMORY_BASE 0xFF000000)
#define R_T0   8   // canonical MMIO load target
#define R_T1   9
#define R_T2   10
#define R_T3   11
#define R_T4   12  // eaptr32 (interpreter alias; plain scratch inside blocks)
#define R_T5   13  // m_pc
#define R_T6   14  // result: canonical MMIO store source
#define R_T7   15  // regcache slot (trap-safe: hw_n64.S saves/restores $15/$24/$25)
#define R_T8   24  // regcache slot
#define R_T9   25  // regcache slot
#define R_V1   3   // zx64_mask (0x00000000FFFFFFFF)
#define R_S2   18  // dptr: must stay a valid even host pointer (stale-dptr law)
#define R_S6   22  // flag_nv
#define R_S7   23  // flag_zc
#define R_S8   30  // flag_x

#define ITYPE(op,rs,rt,imm) (((uint32_t)(op)<<26)|((rs)<<21)|((rt)<<16)|((uint16_t)(int16_t)(imm)))
#define RTYPE(rs,rt,rd,sh,fn) (((rs)<<21)|((rt)<<16)|((rd)<<11)|((sh)<<6)|(fn))
#define ADDIU(rt,rs,imm)  ITYPE(0x09,rs,rt,imm)
#define DADDIU(rt,rs,imm) ITYPE(0x19,rs,rt,imm)
#define SLTI(rt,rs,imm)   ITYPE(0x0A,rs,rt,imm)
#define ANDI(rt,rs,imm)   ITYPE(0x0C,rs,rt,imm)
#define ORI(rt,rs,imm)    ITYPE(0x0D,rs,rt,imm)
#define BEQ(rs,rt,off)    ITYPE(0x04,rs,rt,off)
#define BNE(rs,rt,off)    ITYPE(0x05,rs,rt,off)
#define LW(rt,base,off)   ITYPE(0x23,base,rt,off)
#define LBU(rt,base,off)  ITYPE(0x24,base,rt,off)
#define LHU(rt,base,off)  ITYPE(0x25,base,rt,off)
#define SW(rt,base,off)   ITYPE(0x2B,base,rt,off)
#define SWL(rt,base,off)  ITYPE(0x2A,base,rt,off)
#define SWR(rt,base,off)  ITYPE(0x2E,base,rt,off)
#define LWL(rt,base,off)  ITYPE(0x22,base,rt,off)
#define LWR(rt,base,off)  ITYPE(0x26,base,rt,off)
#define SH(rt,base,off)   ITYPE(0x29,base,rt,off)
#define SB(rt,base,off)   ITYPE(0x28,base,rt,off)
#define LUI(rt,imm)       ITYPE(0x0F,0,rt,imm)
#define OR(rd,rs,rt)      RTYPE(rs,rt,rd,0,0x25)
#define AND(rd,rs,rt)     RTYPE(rs,rt,rd,0,0x24)
#define XOR(rd,rs,rt)     RTYPE(rs,rt,rd,0,0x26)
#define SRLI(rd,rt,sa)    RTYPE(0,rt,rd,sa,0x02)
#define BGEZ(rs,off)      ITYPE(0x01,rs,1,off)
#define BLTZ(rs,off)      ITYPE(0x01,rs,0,off)
#define ADDU(rd,rs,rt)    RTYPE(rs,rt,rd,0,0x21)
#define MOVE(rd,rs)       RTYPE(rs,R_ZERO,rd,0,0x21)  // addu: 32-bit move (sign-extends)
#define DMOVE(rd,rs)      RTYPE(rs,R_ZERO,rd,0,0x25)  // or: 64-bit move (flag values!)
#define SLLI(rd,rt,sa)    RTYPE(0,rt,rd,sa,0x00)
#define DSLL(rd,rt,sa)    RTYPE(0,rt,rd,sa,0x38)
#define DSLL32(rd,rt,sa)  RTYPE(0,rt,rd,sa,0x3C)  // shift (sa+32)
#define DSRL32(rd,rt,sa)  RTYPE(0,rt,rd,sa,0x3E)  // shift (sa+32)
#define DADDU(rd,rs,rt)   RTYPE(rs,rt,rd,0,0x2D)
#define DSUBU(rd,rs,rt)   RTYPE(rs,rt,rd,0,0x2F)
#define JABS(addr)        ((2u<<26)|((((uint32_t)(uintptr_t)(addr))>>2)&0x3FFFFFF))
#define NOP               0u

// Guest code fetch (through the TLB window, same memory the core executes).
static inline uint16_t fetch16(uint32_t a)
{
    return *(uint16_t *)(uintptr_t)(((a) & 0x00FFFFFF) + M64K_CONFIG_MEMORY_BASE);
}

// The interpreter's pc_diff cell (.sdata, m64k_asm.S): guest_pc = m_pc +
// pc_diff. Delta exits preserve it; absolute-target enders (JMP/JSR
// (xxx).l) must store the recomputed constant, so its address is baked.
extern int32_t pc_diff;
#define HI16(a)  ((uint16_t)((((uint32_t)(a)) + 0x8000) >> 16))
#define LO16(a)  ((uint16_t)((uint32_t)(a) & 0xFFFF))

// ---- Templates -----------------------------------------------------------
// Emission context for one block. Address-error checks branch forward to a
// per-check bail stub emitted after the tail: the stub rewinds nothing
// (templates bail BEFORE any mutation, per the fast-path contract), sets
// m_pc to the *current* insn and jumps to main_loop so the generic path
// replays it from scratch and raises ADDRERR bit-exactly (check_addr_bail
// semantics; dispatch stores IR itself on the replay).
// Sized for max_insns=24 blocks: the widest template emits ~16 words and
// up to 3 fixups (two ADDRERR bails + one slice-break check). The decode
// loop's headroom guards below MUST stay >= those per-insn worsts —
// emit_bail_check drops the fix entry when the table is full but still
// emits its BNE, so an under-sized guard leaves a branch patched to
// offset 0, i.e. a jump into the middle of the block.
#define EMIT_MAXWORDS 384
#define EMIT_MAXFIX   40

typedef struct {
    uint32_t buf[EMIT_MAXWORDS];
    int len;            // emitted words
    int goff;           // guest byte offset of the insn being emitted
    struct { int at; int goff; } fix[EMIT_MAXFIX];
    int nfix;
    int ended;          // a block-ender (Bcc/BRA) was emitted: stop decoding
    struct { int at; uint32_t target; int taken; } chain[4];  // exit J slots
    int nchain;
    // Register cache (rung 1, see block comment below): guest An index held
    // by each slot (t7/t8/t9) or -1, LRU ages, translate-time hit count.
    int rc_on;
    int8_t rc_slot[3];
    uint8_t rc_age[3], rc_tick;
    int rc_hits;
} emit_t;

// Baked branch targets that match the interpreter's jmp_exec idle-skip
// list must NOT get a direct emitted branch: the skip only fires on the
// jmp_exec path, and bypassing it would un-skip the vblank spins (the
// single biggest perf lever). Translation refuses the ender instead.
static bool dyn_target_is_spin(uint32_t t)
{
    t &= 0xFFFFFF;
    return t == 0x00142C || t == 0x00FC02 || t == 0x00FB4A || t == 0x00FB74
        || t == 0x001FE2 || t == 0xC18714;
}

static void emit_bail_check(emit_t *e, int addr_reg)
{
#if M64K_CONFIG_ADDRERR
    e->buf[e->len++] = ANDI(R_T1, addr_reg, 1);
    if (e->nfix < EMIT_MAXFIX) {
        e->fix[e->nfix].at = e->len;
        e->fix[e->nfix].goff = e->goff;
        e->nfix++;
    }
    e->buf[e->len++] = BNE(R_T1, R_ZERO, 0);   // offset patched at finalize
    e->buf[e->len++] = NOP;
#endif
}

// Post-access slice-break check: a guest-memory access can MMIO-fault and
// the handler's slice-break clamp then zeroes live a1 â€” the interpreter
// exits at exactly that insn boundary (blez in dispatch), so the block
// must too or IRQ delivery shifts. The C_max entry gate guarantees a1 > 0
// at every internal boundary otherwise, so this branch is never taken in
// the no-clamp case. goff_next = guest offset of the NEXT insn (the
// checked insn has completed).
static void emit_break_check(emit_t *e, int goff_next)
{
    if (e->nfix < EMIT_MAXFIX) {
        e->fix[e->nfix].at = e->len;
        e->fix[e->nfix].goff = goff_next;
        e->nfix++;
    }
    e->buf[e->len++] = ITYPE(0x06, R_A1, 0, 0); // blez a1, <stub> (patched)
    e->buf[e->len++] = NOP;
}

// Common MOVE.w flag/store epilogue: value in reg (16-bit, zero-extended).
// flag_zc = value (bit32=0 => C clear, low32 !Z), flag_nv = value<<16
// (32-bit sll sign-extends: bit31=bit63=N, V clear). X unchanged.
static void emit_movew_flags(emit_t *e, int reg)
{
    e->buf[e->len++] = MOVE(R_S7, reg);
    e->buf[e->len++] = SLLI(R_S6, reg, 16);
}

// MOVE.b/TST.b flag idiom: value in reg (zero-extended byte); shift 24.
static void emit_moveb_flags(emit_t *e, int reg)
{
    e->buf[e->len++] = MOVE(R_S7, reg);
    e->buf[e->len++] = SLLI(R_S6, reg, 24);
}

// ---- Register cache (rung 1) ---------------------------------------------
// Guest ADDRESS registers cached full-width (canonical sign-extended LW
// form) in t7/t8/t9 for the lifetime of one emitted block. The design
// rests on three facts:
//  - t7/t8/t9 are per-instruction scratch in the interpreter (never live
//    across insn boundaries) and the hw_n64.S TLB/MMIO trap handler
//    saves/restores $15/$24/$25 around the C handlers, so cached values
//    survive a trapping guest access mid-block.
//  - WRITE-THROUGH: every guest-visible An update still stores to ctx at
//    the template's commit point, so ctx is architecturally current at
//    every exit (ADDRERR bail stubs, slice-break checks, chain exits) —
//    there is no flush code anywhere and the bail/replay contract is
//    untouched.
//  - The cache is a translate-time construct: fills are ordinary body
//    instructions, so every runtime (re-)entry — including chained
//    self-loops — refills from ctx; no state crosses block entries.
// Dn stays uncached in this rung: templates read b/w/l slices whose
// canonicalization would cost about what the load costs.
// Eviction is LRU over 3 slots; templates keep at most 2 An live at once
// (the mem-to-mem MOVEs), and the just-touched slot has max age, so a
// live registerd An can never be evicted mid-template.
static const uint8_t rc_hostreg[3] = { R_T7, R_T8, R_T9 };

// Host reg holding full An; emits the fill LW on a miss. pref = the
// scratch the uncached path loads into (rc off reproduces the historical
// template emission). The returned register is READ-ONLY to templates —
// updates go through rc_an_commit/rc_an_set.
static int rc_an(emit_t *e, int an, int pref)
{
    if (!e->rc_on) {
        e->buf[e->len++] = LW(pref, R_A0, M64K_OFF_AREGS + 4 * an);
        return pref;
    }
    int v = 0;
    for (int i = 0; i < 3; i++) {
        if (e->rc_slot[i] == an) {
            e->rc_age[i] = ++e->rc_tick;
            e->rc_hits++;
            return rc_hostreg[i];
        }
        if (e->rc_age[i] < e->rc_age[v])
            v = i;
    }
    e->rc_slot[v] = an;
    e->rc_age[v] = ++e->rc_tick;
    e->buf[e->len++] = LW(rc_hostreg[v], R_A0, M64K_OFF_AREGS + 4 * an);
    return rc_hostreg[v];
}

// Post-inc/dec commit An := h + delta (h = what rc_an returned for this
// An). Cached: bump the slot in place and write it through, so later
// insns see the new value for free. Uncached: classic scratch + store.
// Call only AFTER every use of the OLD value (address materialization,
// bail checks) — the slot is updated in place.
static void rc_an_commit(emit_t *e, int an, int h, int delta, int scratch)
{
    if (e->rc_on) {
        e->buf[e->len++] = ADDIU(h, h, delta);
        e->buf[e->len++] = SW(h, R_A0, M64K_OFF_AREGS + 4 * an);
    } else {
        e->buf[e->len++] = ADDIU(scratch, h, delta);
        e->buf[e->len++] = SW(scratch, R_A0, M64K_OFF_AREGS + 4 * an);
    }
}

// An := value in v (canonical 32-bit form): write-through store + drop any
// stale binding. No rebind — a MOVE into a slot would cost the insn the
// cache exists to save; LEA/MOVEA destinations simply refill on next use.
static void rc_an_set(emit_t *e, int an, int v)
{
    e->buf[e->len++] = SW(v, R_A0, M64K_OFF_AREGS + 4 * an);
    if (e->rc_on)
        for (int i = 0; i < 3; i++)
            if (e->rc_slot[i] == an && rc_hostreg[i] != v)
                e->rc_slot[i] = -1;
}

// ADD-family flags (add_f_* idiom): result(t6) = daddu of zero-extended
// operands in t0/t1; sh = 24/16/0 by size (0 = long: flag_zc is the raw
// 64-bit sum, sll-0 re-sign-extends the operands).
static void emit_add_flags(emit_t *e, int sh)
{
    if (sh) e->buf[e->len++] = DSLL(R_S7, R_T6, sh);
    else    e->buf[e->len++] = DMOVE(R_S7, R_T6);  // 64-bit: keep carry bit32
    e->buf[e->len++] = DSRL32(R_S8, R_S7, 0);
    e->buf[e->len++] = SLLI(R_T0, R_T0, sh);
    e->buf[e->len++] = SLLI(R_T1, R_T1, sh);
    e->buf[e->len++] = DADDU(R_S6, R_T0, R_T1);
}

// SUB-family flags (op_sub_rmwimpl_flags idiom): result(t6) = dsubu(t1,t0)
// (dst - src); flag_nv = dsubu of the shifted operands in the same order.
// with_x: SUB/SUBQ set X from the borrow; CMP leaves X untouched.
static void emit_sub_flags(emit_t *e, int sh, int with_x)
{
    if (sh) e->buf[e->len++] = DSLL(R_S7, R_T6, sh);
    else    e->buf[e->len++] = DMOVE(R_S7, R_T6);  // 64-bit: keep borrow bit32
    if (with_x)
        e->buf[e->len++] = DSRL32(R_S8, R_S7, 0);
    e->buf[e->len++] = SLLI(R_T0, R_T0, sh);
    e->buf[e->len++] = SLLI(R_T1, R_T1, sh);
    e->buf[e->len++] = DSUBU(R_S6, R_T1, R_T0);
}

// Load a Dn field sized b/w/l into reg (zero-extended). size: 0=b,1=w,2=l.
static void emit_load_dn(emit_t *e, int reg, int dn, int size)
{
    if (size == 0)      e->buf[e->len++] = LBU(reg, R_A0, M64K_OFF_DREGS + 4 * dn + 3);
    else if (size == 1) e->buf[e->len++] = LHU(reg, R_A0, M64K_OFF_DREGS + 4 * dn + 2);
    else { e->buf[e->len++] = LW(reg, R_A0, M64K_OFF_DREGS + 4 * dn);
           e->buf[e->len++] = AND(reg, reg, R_V1); }  // lwu equivalent
}

static void emit_store_dn(emit_t *e, int reg, int dn, int size)
{
    if (size == 0)      e->buf[e->len++] = SB(reg, R_A0, M64K_OFF_DREGS + 4 * dn + 3);
    else if (size == 1) e->buf[e->len++] = SH(reg, R_A0, M64K_OFF_DREGS + 4 * dn + 2);
    else                e->buf[e->len++] = SW(reg, R_A0, M64K_OFF_DREGS + 4 * dn);
}

// Materialize the mapped host address of a baked 24-bit guest address
// into reg: host = guest | 0xFF000000 (2 insns: lui sign-extends).
static void emit_abs_host(emit_t *e, int reg, uint32_t guest)
{
    uint32_t host = (guest & 0x00FFFFFF) | 0xFF000000u;
    e->buf[e->len++] = LUI(reg, host >> 16);
    e->buf[e->len++] = ORI(reg, reg, host & 0xFFFF);
}

// Emit one instruction if the form is supported. Returns the guest length
// in bytes (>0) and adds its cycle charge to *cmax, or 0 if untranslatable.
static int emit_insn(uint16_t op, uint32_t pc, emit_t *e, int *cmax)
{
    // MOVEQ #imm,Dn: 0111 rrr 0 iiiiiiii (op_moveq: charge 4;
    // flag_nv = sign-extended byte, flag_zc = zero-extended byte,
    // Dn = sign-extended long; X unchanged; cannot fault).
    if ((op & 0xF100) == 0x7000) {
        int reg = (op >> 9) & 7;
        int imm = (int8_t)(op & 0xFF);
        e->buf[e->len++] = ADDIU(R_A1, R_A1, -4);
        e->buf[e->len++] = DADDIU(R_S6, R_ZERO, imm);
        e->buf[e->len++] = ORI(R_S7, R_ZERO, op & 0xFF);
        e->buf[e->len++] = SW(R_S6, R_A0, M64K_OFF_DREGS + 4 * reg);
        *cmax += 4;
        return 2;
    }

    // MOVE.w family: 0011 DDD ddd sss SSS (dst reg/mode, src mode/reg).
    // Forms below are 1:1 transcriptions of the debugged fast-path bodies
    // (m64k_asm.S movew_*, see DYNREC-PHASE2-TEMPLATES.md) with decode
    // preambles collapsed and code-stream displacements baked.
    if ((op & 0xF000) == 0x3000) {
        int sreg = op & 7, smode = (op >> 3) & 7;
        int dreg = (op >> 9) & 7, dmode = (op >> 6) & 7;

        // move.w Dn,Dm  [movew_f_dst_dn, 4 cycles, 1 word]: cannot fault.
        if (smode == 0 && dmode == 0) {
            e->buf[e->len++] = LHU(R_T0, R_A0, M64K_OFF_DREGS + 4 * sreg + 2);
            e->buf[e->len++] = SH(R_T0, R_A0, M64K_OFF_DREGS + 4 * dreg + 2);
            emit_movew_flags(e, R_T0);
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -4);
            *cmax += 4;
            return 2;
        }

        // move.w Dn,(An)  [movew_f_dst_an, 8 cycles, 1 word].
        if (smode == 0 && dmode == 2) {
            e->buf[e->len++] = LHU(R_T2, R_A0, M64K_OFF_DREGS + 4 * sreg + 2);
            int h = rc_an(e, dreg, R_T4);
            emit_bail_check(e, h);
            e->buf[e->len++] = OR(R_T3, h, R_A2);
            e->buf[e->len++] = MOVE(R_T6, R_T2);
            emit_movew_flags(e, R_T2);
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -8);
            e->buf[e->len++] = SH(R_T6, R_T3, 0);
            emit_break_check(e, e->goff + 2);
            *cmax += 8;
            return 2;
        }

        // move.w Dn,(An)+  [movew_f_dst_anp, 8 cycles, 1 word]: A7 keeps
        // its word-align quirk generic; post-inc commits BEFORE the store.
        if (smode == 0 && dmode == 3 && dreg != 7) {
            e->buf[e->len++] = LHU(R_T2, R_A0, M64K_OFF_DREGS + 4 * sreg + 2);
            int h = rc_an(e, dreg, R_T4);
            emit_bail_check(e, h);
            e->buf[e->len++] = OR(R_T3, h, R_A2);
            rc_an_commit(e, dreg, h, 2, R_T1);
            e->buf[e->len++] = MOVE(R_T6, R_T2);
            emit_movew_flags(e, R_T2);
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -8);
            e->buf[e->len++] = SH(R_T6, R_T3, 0);
            emit_break_check(e, e->goff + 2);
            *cmax += 8;
            return 2;
        }

        // move.w (An)+,Dn  [movew_fanp_dn, 8 cycles, 1 word]: post-inc
        // commits BEFORE the load (ea_011 order); bail before any mutation.
        if (smode == 3 && dmode == 0) {
            int h = rc_an(e, sreg, R_T4);
            emit_bail_check(e, h);
            e->buf[e->len++] = OR(R_T3, h, R_A2);
            rc_an_commit(e, sreg, h, 2, R_T2);
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -8);
            e->buf[e->len++] = LHU(R_T0, R_T3, 0);
            e->buf[e->len++] = SH(R_T0, R_A0, M64K_OFF_DREGS + 4 * dreg + 2);
            emit_movew_flags(e, R_T0);
            emit_break_check(e, e->goff + 2);
            *cmax += 8;
            return 2;
        }

        // move.w (d16,An),Dn  [movew_fsrc_d16, 12 cycles, 2 words]: d16
        // baked from the code stream at translate time.
        if (smode == 5 && dmode == 0) {
            int16_t d16 = (int16_t)fetch16(pc + 2);
            int h = rc_an(e, sreg, R_T4);
            e->buf[e->len++] = ADDIU(R_T4, h, d16);
            emit_bail_check(e, R_T4);
            e->buf[e->len++] = OR(R_T3, R_T4, R_A2);
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -12);
            e->buf[e->len++] = LHU(R_T0, R_T3, 0);
            e->buf[e->len++] = SH(R_T0, R_A0, M64K_OFF_DREGS + 4 * dreg + 2);
            emit_movew_flags(e, R_T0);
            emit_break_check(e, e->goff + 4);
            *cmax += 12;
            return 4;
        }

        // move.w Dn,(d16,An)  [movew_f_dst_d16, 12 cycles, 2 words]:
        // flags from the source value BEFORE the store; store from result.
        if (smode == 0 && dmode == 5) {
            int16_t d16 = (int16_t)fetch16(pc + 2);
            e->buf[e->len++] = LHU(R_T2, R_A0, M64K_OFF_DREGS + 4 * sreg + 2);
            int h = rc_an(e, dreg, R_T4);
            e->buf[e->len++] = ADDIU(R_T4, h, d16);
            emit_bail_check(e, R_T4);
            e->buf[e->len++] = OR(R_T3, R_T4, R_A2);
            e->buf[e->len++] = MOVE(R_T6, R_T2);
            emit_movew_flags(e, R_T2);
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -12);
            e->buf[e->len++] = SH(R_T6, R_T3, 0);
            emit_break_check(e, e->goff + 4);
            *cmax += 12;
            return 4;
        }

        // move.w #imm,Dn (8 cycles, 2 words; flags baked, rig-verified).
        if (smode == 7 && sreg == 4 && dmode == 0) {
            uint16_t imm = fetch16(pc + 2);
            e->buf[e->len++] = ORI(R_T0, R_ZERO, imm);
            e->buf[e->len++] = SH(R_T0, R_A0, M64K_OFF_DREGS + 4 * dreg + 2);
            emit_movew_flags(e, R_T0);
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -8);
            *cmax += 8;
            return 4;
        }
        // move.w (An),Dn (8 cycles, 1 word; rig-verified charge).
        if (smode == 2 && dmode == 0) {
            int h = rc_an(e, sreg, R_T4);
            emit_bail_check(e, h);
            e->buf[e->len++] = OR(R_T3, h, R_A2);
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -8);
            e->buf[e->len++] = LHU(R_T0, R_T3, 0);
            e->buf[e->len++] = SH(R_T0, R_A0, M64K_OFF_DREGS + 4 * dreg + 2);
            emit_movew_flags(e, R_T0);
            emit_break_check(e, e->goff + 2);
            *cmax += 8;
            return 2;
        }
        // move.w An,Dn (4 cycles, 1 word; rig-verified charge).
        if (smode == 1 && dmode == 0) {
            e->buf[e->len++] = LHU(R_T0, R_A0, M64K_OFF_AREGS + 4 * sreg + 2);
            e->buf[e->len++] = SH(R_T0, R_A0, M64K_OFF_DREGS + 4 * dreg + 2);
            emit_movew_flags(e, R_T0);
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -4);
            *cmax += 4;
            return 2;
        }
        // move.w (xxx).l,Dn (16 cycles, 3 words; refuse odd abs).
        if (smode == 7 && sreg == 1 && dmode == 0) {
            uint32_t abs = ((uint32_t)fetch16(pc + 2) << 16) | fetch16(pc + 4);
            if (abs & 1)
                return 0;
            emit_abs_host(e, R_T3, abs);
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -16);
            e->buf[e->len++] = LHU(R_T0, R_T3, 0);
            e->buf[e->len++] = SH(R_T0, R_A0, M64K_OFF_DREGS + 4 * dreg + 2);
            emit_movew_flags(e, R_T0);
            emit_break_check(e, e->goff + 6);
            *cmax += 16;
            return 6;
        }
        // move.w (An)+,(Am) [generic movew body, 4+4+4 = 12 cycles]: the
        // single hottest refused head in samsho2 (0x0031FE, ~143k probe
        // transfers per DYNSTAT window — the per-frame copy loop). Generic
        // order: src EA (post-inc commits) -> src odd check -> read ->
        // flags -> dst EA -> dst odd check -> store. Both odd checks are
        // hoisted ahead of every mutation so a bail replays cleanly (the
        // predicates are unchanged: ea_011 leaves eaptr32 = the ORIGINAL
        // An). sreg == dreg is refused: the generic reads the dst register
        // AFTER the source post-inc commits, so it would see the bumped
        // value. A7 refused (word-align quirk stays generic).
        if (smode == 3 && dmode == 2 && sreg != 7 && sreg != dreg) {
#ifdef M64K_DYN_NO_R2MEM
            return 0;                   // TRCRC bisect knob
#endif
            int hs = rc_an(e, sreg, R_T4);
            emit_bail_check(e, hs);
            int hd = rc_an(e, dreg, R_T2);  // <=2 live An: cannot evict hs
            emit_bail_check(e, hd);
            e->buf[e->len++] = OR(R_T3, hs, R_A2);
            rc_an_commit(e, sreg, hs, 2, R_T1);
            // Charge SPLIT 8/4 like the interpreter (entry+ea_011 before
            // the read, dst ea before the write; the W3 path is explicit
            // about it and the generic path composes the same). A lump
            // -12 here is invisible to the rig (RAM never traps) but an
            // MMIO source that derives its value from the cycle counter —
            // the LSPC raster poll — reads a DIFFERENT value under it:
            // the mid-instruction-clock law is architectural, not
            // cosmetic.
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -8);
            e->buf[e->len++] = LHU(R_T0, R_T3, 0);
            e->buf[e->len++] = MOVE(R_T6, R_T0);
            emit_movew_flags(e, R_T0);
            e->buf[e->len++] = OR(R_T3, hd, R_A2);
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -4);
            e->buf[e->len++] = SH(R_T6, R_T3, 0);
            emit_break_check(e, e->goff + 2);
            *cmax += 12;
            return 2;
        }
        // move.w Dn,(xxx).l [4 + ea7_001 12 = 16 cycles, 3 words]: 2nd
        // hottest refused head (0xC1DF8A, ~91k/window, BIOS region).
        if (smode == 0 && dmode == 7 && dreg == 1) {
#ifdef M64K_DYN_NO_R2MEM
            return 0;                   // TRCRC bisect knob
#endif
            uint32_t abs = ((uint32_t)fetch16(pc + 2) << 16) | fetch16(pc + 4);
            if (abs & 1)
                return 0;
            e->buf[e->len++] = LHU(R_T6, R_A0, M64K_OFF_DREGS + 4 * sreg + 2);
            emit_abs_host(e, R_T3, abs);
            emit_movew_flags(e, R_T6);
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -16);
            e->buf[e->len++] = SH(R_T6, R_T3, 0);
            emit_break_check(e, e->goff + 6);
            *cmax += 16;
            return 6;
        }
        // (fall through to unsupported for other MOVE.w forms)
        // move.w #imm,(An)  [movew_fsrc_other tail, 12 cycles, 2 words].
        if (smode == 7 && sreg == 4 && dmode == 2) {
            uint16_t imm = fetch16(pc + 2);
            int h = rc_an(e, dreg, R_T4);
            emit_bail_check(e, h);
            e->buf[e->len++] = OR(R_T3, h, R_A2);
            e->buf[e->len++] = ORI(R_T6, R_ZERO, imm);
            emit_movew_flags(e, R_T6);
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -12);
            e->buf[e->len++] = SH(R_T6, R_T3, 0);
            emit_break_check(e, e->goff + 4);
            *cmax += 12;
            return 4;
        }
    }

    // CMPI/ADDI: no immediate-form btest suites exist; these encodings are
    // gated by the testsuite's emitter-differential rig (testsuite.c),
    // canary-certified 2026-08-07 (a corrupted X-flag idiom fails exactly
    // the ADDI sequences).
    // CMPI #imm,Dn  [cmpi_f_word/cmpi_f_byte, 8 cycles, 2 words]: CMP
    // flags (no X), no writeback. dptr is set by the interpreter's imm
    // decode; emitted code keeps the stale-dptr law with dptr = ctx
    // (valid, even). 64-bit idioms: flag_zc = (a-b)<<size (bit32 = borrow),
    // flag_nv = (a<<shift) - (b<<shift) of sign-extended 32-bit values.
    if ((op & 0xFFF8) == 0x0C40 || (op & 0xFFF8) == 0x0C00) {
        int reg = op & 7;
        int word = (op & 0x40) != 0;
        uint16_t rawimm = fetch16(pc + 2);
        uint16_t imm = word ? rawimm : (rawimm & 0xFF);
        int sh = word ? 16 : 24;
        e->buf[e->len++] = MOVE(R_S2, R_A0);
        e->buf[e->len++] = ORI(R_T0, R_ZERO, imm);
        e->buf[e->len++] = word ? LHU(R_T1, R_A0, M64K_OFF_DREGS + 4 * reg + 2)
                                : LBU(R_T1, R_A0, M64K_OFF_DREGS + 4 * reg + 3);
        e->buf[e->len++] = DSUBU(R_T6, R_T1, R_T0);
        e->buf[e->len++] = DSLL(R_S7, R_T6, sh);
        e->buf[e->len++] = SLLI(R_T2, R_T0, sh);
        e->buf[e->len++] = SLLI(R_T1, R_T1, sh);
        e->buf[e->len++] = DSUBU(R_S6, R_T1, R_T2);
        e->buf[e->len++] = ADDIU(R_A1, R_A1, -8);
        *cmax += 8;
        return 4;
    }

    // ADDI.w #imm,Dn  [wave-3 W3_FULL body, 8 cycles, 2 words]: full ADD
    // flags including X (add_f_word idiom: flag_zc = 64-bit sum<<16 with
    // bit32 = carry, flag_x = that carry, flag_nv = sum of operands<<16).
    if ((op & 0xFFF8) == 0x0640) {
        int reg = op & 7;
        uint16_t imm = fetch16(pc + 2);
        e->buf[e->len++] = MOVE(R_S2, R_A0);
        e->buf[e->len++] = ORI(R_T0, R_ZERO, imm);
        e->buf[e->len++] = LHU(R_T1, R_A0, M64K_OFF_DREGS + 4 * reg + 2);
        e->buf[e->len++] = DADDU(R_T6, R_T0, R_T1);
        e->buf[e->len++] = DSLL(R_S7, R_T6, 16);
        e->buf[e->len++] = DSRL32(R_S8, R_S7, 0);
        e->buf[e->len++] = SLLI(R_T0, R_T0, 16);
        e->buf[e->len++] = SLLI(R_T1, R_T1, 16);
        e->buf[e->len++] = DADDU(R_S6, R_T0, R_T1);
        e->buf[e->len++] = SH(R_T6, R_A0, M64K_OFF_DREGS + 4 * reg + 2);
        e->buf[e->len++] = ADDIU(R_A1, R_A1, -8);
        *cmax += 8;
        return 4;
    }
    // BTST #imm,Dn [8 cycles rig-measured (4 + bits_impl 8 - no-writeback
    // 4; the Dn +2 opmask arm does NOT fire), 2 words]: only Z is
    // affected — bits_impl idiom: keep C
    // (upper bits of flag_zc), replace the Z field with the RAW masked
    // bit (low32 nonzero <=> bit set). N/V/X untouched. Bit index mod 32
    // for the Dn form. Unlocks function-entry heads (00cee0/00cf22, the
    // in-fight VRAM-upload function) whose refusal starved the chain
    // seeder of the 80k-heat inner loops.
    if ((op & 0xFFF8) == 0x0800) {
        int reg = op & 7;
        int bit = fetch16(pc + 2) & 31;
        e->buf[e->len++] = MOVE(R_S2, R_A0);
        e->buf[e->len++] = LW(R_T1, R_A0, M64K_OFF_DREGS + 4 * reg);
        if (bit < 16)
            e->buf[e->len++] = ORI(R_T2, R_ZERO, 1u << bit);
        else
            e->buf[e->len++] = LUI(R_T2, 1u << (bit - 16));
        e->buf[e->len++] = AND(R_T1, R_T1, R_T2);
        e->buf[e->len++] = DSRL32(R_T0, R_S7, 0);
        e->buf[e->len++] = DSLL32(R_T0, R_T0, 0);
        e->buf[e->len++] = OR(R_S7, R_T0, R_T1);
        e->buf[e->len++] = ADDIU(R_A1, R_A1, -8);
        *cmax += 8;
        return 4;
    }

    // SUBI.w #imm,Dn [8 cycles, 2 words]: ADDI's mirror — full SUB flags
    // including the borrow X (op_sub_rmwimpl idiom). Rig-gated like ADDI.
    if ((op & 0xFFF8) == 0x0440) {
#if defined(M64K_DYN_NO_COV) || defined(M64K_DYN_NO_SUBI)
        return 0;                   // TRCRC bisect knob (coverage rung)
#endif
        if (!dyn_s.cov_enable)
            return 0;               // runtime fps-twin knob (layout-identical)
        int reg = op & 7;
        uint16_t imm = fetch16(pc + 2);
        e->buf[e->len++] = MOVE(R_S2, R_A0);
        e->buf[e->len++] = ORI(R_T0, R_ZERO, imm);
        e->buf[e->len++] = LHU(R_T1, R_A0, M64K_OFF_DREGS + 4 * reg + 2);
        e->buf[e->len++] = DSUBU(R_T6, R_T1, R_T0);
        emit_sub_flags(e, 16, 1);
        e->buf[e->len++] = SH(R_T6, R_A0, M64K_OFF_DREGS + 4 * reg + 2);
        e->buf[e->len++] = ADDIU(R_A1, R_A1, -8);
        *cmax += 8;
        return 4;
    }

    // LSR.w #imm,Dn [shift-imm group: 8+2c cycles at accuracy 0 —
    // rig-measured; the use_cycles_opmask -2 arm applies to LONG, not
    // word]. lsr_rmwimpl flag idiom: C = last
    // bit shifted out (bit32 of flag_zc), Z = 16-bit result, N = result
    // bit15 via sll 16 (V clear), X = C (imm count is always >= 1). dptr =
    // &DREGS[n] (rmw_eadst parity). Terminates the BIOS raster-poll loops
    // (c1e2bc/c0996e — DYNTERM #3).
    if ((op & 0xF1F8) == 0xE048) {
#if defined(M64K_DYN_NO_COV) || defined(M64K_DYN_NO_LSR)
        return 0;                   // TRCRC bisect knob (coverage rung)
#endif
        if (!dyn_s.cov_enable)
            return 0;               // runtime fps-twin knob (layout-identical)
        int reg = op & 7;
        int c = (((op >> 9) - 1) & 7) + 1;
        e->buf[e->len++] = ADDIU(R_S2, R_A0, M64K_OFF_DREGS + 4 * reg);
        e->buf[e->len++] = LHU(R_T1, R_A0, M64K_OFF_DREGS + 4 * reg + 2);
        e->buf[e->len++] = SRLI(R_T6, R_T1, c);          // 16-bit result
        e->buf[e->len++] = SRLI(R_T0, R_T1, c - 1);
        e->buf[e->len++] = ANDI(R_T0, R_T0, 1);          // carry = last bit out
        e->buf[e->len++] = DMOVE(R_S8, R_T0);            // X = carry
        e->buf[e->len++] = DSLL32(R_T0, R_T0, 0);
        e->buf[e->len++] = OR(R_S7, R_T0, R_T6);         // zc = carry<<32 | res
        e->buf[e->len++] = SLLI(R_S6, R_T6, 16);         // N = bit15, V clear
        e->buf[e->len++] = SH(R_T6, R_A0, M64K_OFF_DREGS + 4 * reg + 2);
        e->buf[e->len++] = ADDIU(R_A1, R_A1, -(8 + 2 * c));
        *cmax += 8 + 2 * c;
        return 2;
    }

    // NOP (op_nop: charge 4).
    if (op == 0x4E71) {
        e->buf[e->len++] = ADDIU(R_A1, R_A1, -4);
        *cmax += 4;
        return 2;
    }

    // LEA (xxx).l,An (W3 lea fast path: ea7_001's flat 12, OP(lea) itself
    // charges nothing at accuracy 0; no flags; raw 32-bit stored). Sits at
    // function preambles, so refusing it kept whole callees untranslated.
    if ((op & 0xF1FF) == 0x41F9) {
        int an = (op >> 9) & 7;
        uint32_t abs = ((uint32_t)fetch16(pc + 2) << 16) | fetch16(pc + 4);
        e->buf[e->len++] = LUI(R_T0, abs >> 16);
        e->buf[e->len++] = ORI(R_T0, R_T0, abs & 0xFFFF);
        rc_an_set(e, an, R_T0);
        e->buf[e->len++] = ADDIU(R_A1, R_A1, -12);
        *cmax += 12;
        return 6;
    }

    // MOVE.b family: byte accesses cannot raise address errors (no bail);
    // the A7 byte quirk (+2) keeps A7 generic for (An)+ forms. Bodies from
    // the wave-1/wave-3 fast paths (m64k_asm.S 2690-2781); abs.l forms are
    // baked-host extensions with charges verified by the differential rig.
    if ((op & 0xF000) == 0x1000) {
        int sreg = op & 7, smode = (op >> 3) & 7;
        int dreg = (op >> 9) & 7, dmode = (op >> 6) & 7;

        // move.b Dn,Dm (4 cycles, 1 word).
        if (smode == 0 && dmode == 0) {
            e->buf[e->len++] = LBU(R_T0, R_A0, M64K_OFF_DREGS + 4 * sreg + 3);
            e->buf[e->len++] = SB(R_T0, R_A0, M64K_OFF_DREGS + 4 * dreg + 3);
            emit_moveb_flags(e, R_T0);
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -4);
            *cmax += 4;
            return 2;
        }
        // move.b (An)+,Dn (8 cycles, 1 word; A7 generic): post-inc by 1
        // BEFORE the load (ea_011 order).
        if (smode == 3 && dmode == 0 && sreg != 7) {
            int h = rc_an(e, sreg, R_T4);
            e->buf[e->len++] = OR(R_T3, h, R_A2);
            rc_an_commit(e, sreg, h, 1, R_T2);
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -8);
            e->buf[e->len++] = LBU(R_T0, R_T3, 0);
            e->buf[e->len++] = SB(R_T0, R_A0, M64K_OFF_DREGS + 4 * dreg + 3);
            emit_moveb_flags(e, R_T0);
            emit_break_check(e, e->goff + 2);
            *cmax += 8;
            return 2;
        }
        // move.b (d16,An),Dn (12 cycles, 2 words).
        if (smode == 5 && dmode == 0) {
            int16_t d16 = (int16_t)fetch16(pc + 2);
            int h = rc_an(e, sreg, R_T4);
            e->buf[e->len++] = ADDIU(R_T4, h, d16);
            e->buf[e->len++] = OR(R_T3, R_T4, R_A2);
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -12);
            e->buf[e->len++] = LBU(R_T0, R_T3, 0);
            e->buf[e->len++] = SB(R_T0, R_A0, M64K_OFF_DREGS + 4 * dreg + 3);
            emit_moveb_flags(e, R_T0);
            emit_break_check(e, e->goff + 4);
            *cmax += 12;
            return 4;
        }
        // move.b Dn,(d16,An) (12 cycles, 2 words; W3 body order: flags
        // from source before the store).
        if (smode == 0 && dmode == 5) {
            int16_t d16 = (int16_t)fetch16(pc + 2);
            e->buf[e->len++] = LBU(R_T6, R_A0, M64K_OFF_DREGS + 4 * sreg + 3);
            int h = rc_an(e, dreg, R_T4);
            e->buf[e->len++] = ADDIU(R_T4, h, d16);
            e->buf[e->len++] = OR(R_T3, R_T4, R_A2);
            emit_moveb_flags(e, R_T6);
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -12);
            e->buf[e->len++] = SB(R_T6, R_T3, 0);
            emit_break_check(e, e->goff + 4);
            *cmax += 12;
            return 4;
        }
        // move.b Dn,(An) (8 cycles, 1 word; charge rig-verified).
        if (smode == 0 && dmode == 2) {
            e->buf[e->len++] = LBU(R_T6, R_A0, M64K_OFF_DREGS + 4 * sreg + 3);
            int h = rc_an(e, dreg, R_T4);
            e->buf[e->len++] = OR(R_T3, h, R_A2);
            emit_moveb_flags(e, R_T6);
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -8);
            e->buf[e->len++] = SB(R_T6, R_T3, 0);
            emit_break_check(e, e->goff + 2);
            *cmax += 8;
            return 2;
        }
        // move.b Dn,(xxx).l (16 cycles, 3 words; host baked, rig-verified).
        if (smode == 0 && dmode == 7 && dreg == 1) {
            uint32_t abs = ((uint32_t)fetch16(pc + 2) << 16) | fetch16(pc + 4);
            e->buf[e->len++] = LBU(R_T6, R_A0, M64K_OFF_DREGS + 4 * sreg + 3);
            emit_abs_host(e, R_T3, abs);
            emit_moveb_flags(e, R_T6);
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -16);
            e->buf[e->len++] = SB(R_T6, R_T3, 0);
            emit_break_check(e, e->goff + 6);
            *cmax += 16;
            return 6;
        }
        // move.b (xxx).l,Dn (16 cycles, 3 words; rig-verified charge).
        if (smode == 7 && sreg == 1 && dmode == 0) {
            uint32_t abs = ((uint32_t)fetch16(pc + 2) << 16) | fetch16(pc + 4);
            emit_abs_host(e, R_T3, abs);
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -16);
            e->buf[e->len++] = LBU(R_T0, R_T3, 0);
            e->buf[e->len++] = SB(R_T0, R_A0, M64K_OFF_DREGS + 4 * dreg + 3);
            emit_moveb_flags(e, R_T0);
            emit_break_check(e, e->goff + 6);
            *cmax += 16;
            return 6;
        }
        // move.b #imm,(xxx).l (20 cycles, 4 words; flags baked).
        if (smode == 7 && sreg == 4 && dmode == 7 && dreg == 1) {
            uint8_t imm = fetch16(pc + 2) & 0xFF;
            uint32_t abs = ((uint32_t)fetch16(pc + 4) << 16) | fetch16(pc + 6);
            e->buf[e->len++] = ORI(R_T6, R_ZERO, imm);
            emit_abs_host(e, R_T3, abs);
            e->buf[e->len++] = ORI(R_S7, R_ZERO, imm);
            e->buf[e->len++] = LUI(R_S6, (uint16_t)(imm << 8));
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -20);
            e->buf[e->len++] = SB(R_T6, R_T3, 0);
            emit_break_check(e, e->goff + 8);
            *cmax += 20;
            return 8;
        }
    }

    // TST.b/.w (tst fast path 2608-2637 for (d16,An); Dn and abs.l forms
    // are baked extensions, charges rig-verified. .l and TAS stay generic).
    if ((op & 0xFF00) == 0x4A00 && (op & 0xC0) != 0x80 && (op & 0xC0) != 0xC0) {
        int word = (op & 0xC0) == 0x40;
        int mode = (op >> 3) & 7, reg = op & 7;

        // tst.b/.w Dn (4 cycles, 1 word).
        if (mode == 0) {
            e->buf[e->len++] = word
                ? LHU(R_T0, R_A0, M64K_OFF_DREGS + 4 * reg + 2)
                : LBU(R_T0, R_A0, M64K_OFF_DREGS + 4 * reg + 3);
            if (word) emit_movew_flags(e, R_T0);
            else      emit_moveb_flags(e, R_T0);
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -4);
            *cmax += 4;
            return 2;
        }
        // tst.b/.w (d16,An) (12 cycles, 2 words; word form bails on odd).
        if (mode == 5) {
            int16_t d16 = (int16_t)fetch16(pc + 2);
            int h = rc_an(e, reg, R_T4);
            e->buf[e->len++] = ADDIU(R_T4, h, d16);
            if (word)
                emit_bail_check(e, R_T4);
            e->buf[e->len++] = OR(R_T3, R_T4, R_A2);
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -12);
            e->buf[e->len++] = word ? LHU(R_T0, R_T3, 0) : LBU(R_T0, R_T3, 0);
            if (word) emit_movew_flags(e, R_T0);
            else      emit_moveb_flags(e, R_T0);
            emit_break_check(e, e->goff + 4);
            *cmax += 12;
            return 4;
        }
        // tst.b/.w (xxx).l (16 cycles, 3 words; host baked, always even
        // for the word form or we refuse).
        if (mode == 7 && reg == 1) {
            uint32_t abs = ((uint32_t)fetch16(pc + 2) << 16) | fetch16(pc + 4);
            if (word && (abs & 1))
                return 0;
            emit_abs_host(e, R_T3, abs);
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -16);
            e->buf[e->len++] = word ? LHU(R_T0, R_T3, 0) : LBU(R_T0, R_T3, 0);
            if (word) emit_movew_flags(e, R_T0);
            else      emit_moveb_flags(e, R_T0);
            emit_break_check(e, e->goff + 6);
            *cmax += 16;
            return 6;
        }
    }

    // ADD/SUB/CMP .b/.w/.l Dn,Dm and ADDA.w Dn,An (add_f_*/sub-flags
    // idioms; charges 4/4/6, ADDA 8). ADDQ/SUBQ #q,Dn (addq_fast; 4/4/8).
    // MOVEA.l/.w (no flags, charge 4) and MOVE.l Dn,Dm (tst_long flag
    // idiom, charge 4). All operands register-resident: no faults.
    {
        int top = op >> 12;
        int sreg = op & 7, smode = (op >> 3) & 7;
        int dreg = (op >> 9) & 7;
        int opmode = (op >> 6) & 7;
        static const int shtab[3] = { 24, 16, 0 };

        // ADD.b (An)+,Dn [4 + ea_011 byte 4 = 8 cycles; A7 byte quirk stays
        // generic]: the c11c4e BIOS checksum loop body (DYNTERM top heat).
        // Post-inc commits BEFORE the read (ea_011 order); byte access
        // cannot ADDRERR; break check after the (possibly MMIO) read.
        if (top == 0xD && smode == 3 && opmode == 0 && sreg != 7) {
#if defined(M64K_DYN_NO_COV) || defined(M64K_DYN_NO_ADDB)
        return 0;                   // TRCRC bisect knob (coverage rung)
#endif
            if (!dyn_s.cov_enable)
                return 0;           // runtime fps-twin knob (layout-identical)
            int h = rc_an(e, sreg, R_T4);
            e->buf[e->len++] = OR(R_T3, h, R_A2);
            rc_an_commit(e, sreg, h, 1, R_T2);
            e->buf[e->len++] = ADDIU(R_S2, R_A0, M64K_OFF_DREGS + 4 * dreg);
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -8);
            e->buf[e->len++] = LBU(R_T0, R_T3, 0);
            e->buf[e->len++] = LBU(R_T1, R_A0, M64K_OFF_DREGS + 4 * dreg + 3);
            e->buf[e->len++] = DADDU(R_T6, R_T0, R_T1);
            emit_store_dn(e, R_T6, dreg, 0);
            emit_add_flags(e, 24);
            emit_break_check(e, e->goff + 2);
            *cmax += 8;
            return 2;
        }
        // CMP.w (d16,An),Dn [4 + ea d16 8 = 12 cycles, 2 words; no store,
        // no X]: the 0038ba in-game linked-list search loop (DYNTERM #2,
        // and its A4 traffic is regcache-hot).
        if (top == 0xB && opmode == 1 && smode == 5) {
#if defined(M64K_DYN_NO_COV) || defined(M64K_DYN_NO_CMPW)
        return 0;                   // TRCRC bisect knob (coverage rung)
#endif
            if (!dyn_s.cov_enable)
                return 0;           // runtime fps-twin knob (layout-identical)
            int16_t d16 = (int16_t)fetch16(pc + 2);
            int h = rc_an(e, sreg, R_T4);
            e->buf[e->len++] = ADDIU(R_T4, h, d16);
            emit_bail_check(e, R_T4);
            e->buf[e->len++] = OR(R_T3, R_T4, R_A2);
            e->buf[e->len++] = MOVE(R_S2, R_A0);
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -12);
            e->buf[e->len++] = LHU(R_T0, R_T3, 0);
            e->buf[e->len++] = LHU(R_T1, R_A0, M64K_OFF_DREGS + 4 * dreg + 2);
            e->buf[e->len++] = DSUBU(R_T6, R_T1, R_T0);
            emit_sub_flags(e, 16, 0);
            emit_break_check(e, e->goff + 4);
            *cmax += 12;
            return 4;
        }
        // ADD/SUB Dn,Dm (opmode 0-2, src Dn).
        if ((top == 0xD || top == 0x9) && smode == 0 && opmode <= 2) {
            int size = opmode, sh = shtab[size];
            e->buf[e->len++] = ADDIU(R_S2, R_A0, M64K_OFF_DREGS + 4 * dreg);
            emit_load_dn(e, R_T0, sreg, size);
            emit_load_dn(e, R_T1, dreg, size);
            if (top == 0xD) {
                e->buf[e->len++] = DADDU(R_T6, R_T0, R_T1);
                emit_store_dn(e, R_T6, dreg, size);
                emit_add_flags(e, sh);
            } else {
                e->buf[e->len++] = DSUBU(R_T6, R_T1, R_T0);
                emit_store_dn(e, R_T6, dreg, size);
                emit_sub_flags(e, sh, 1);
            }
            e->buf[e->len++] = ADDIU(R_A1, R_A1, size == 2 ? -6 : -4);
            *cmax += (size == 2) ? 6 : 4;
            return 2;
        }
        // ADDA.w Dn,An (opmode 3, src Dn: sign-extended word add, no flags).
        if (top == 0xD && smode == 0 && opmode == 3) {
            e->buf[e->len++] = ADDIU(R_S2, R_A0, M64K_OFF_AREGS + 4 * dreg);
            e->buf[e->len++] = ITYPE(0x21, R_A0, R_T0, M64K_OFF_DREGS + 4 * sreg + 2); // lh
            int h = rc_an(e, dreg, R_T1);
            if (e->rc_on) {
                // In-slot sum keeps the binding live (addu re-sign-extends,
                // preserving the canonical cached form).
                e->buf[e->len++] = ADDU(h, h, R_T0);
                e->buf[e->len++] = SW(h, R_A0, M64K_OFF_AREGS + 4 * dreg);
            } else {
                e->buf[e->len++] = MOVE(R_T2, R_T0);
                e->buf[e->len++] = ADDU(R_T2, h, R_T2);
                e->buf[e->len++] = SW(R_T2, R_A0, M64K_OFF_AREGS + 4 * dreg);
            }
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -8);
            *cmax += 8;
            return 2;
        }
        // CMP.b/.w Dn,Dm (opmode 0/1; no store, no X).
        if (top == 0xB && smode == 0 && opmode <= 1) {
            int size = opmode, sh = shtab[size];
            e->buf[e->len++] = MOVE(R_S2, R_A0);
            emit_load_dn(e, R_T0, sreg, size);
            emit_load_dn(e, R_T1, dreg, size);
            e->buf[e->len++] = DSUBU(R_T6, R_T1, R_T0);
            emit_sub_flags(e, sh, 0);
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -4);
            *cmax += 4;
            return 2;
        }
        // ADDQ/SUBQ #q,Dn (size 0-2, EA mode Dn).
        if (top == 0x5 && smode == 0 && ((op >> 6) & 3) <= 2) {
            int size = (op >> 6) & 3, sh = shtab[size];
            int q = (((op >> 9) - 1) & 7) + 1;
            int is_sub = (op >> 8) & 1;
            e->buf[e->len++] = ADDIU(R_S2, R_A0, M64K_OFF_PENDINGEXC + 4);
            e->buf[e->len++] = ORI(R_T0, R_ZERO, q);
            emit_load_dn(e, R_T1, sreg, size);
            if (!is_sub) {
                e->buf[e->len++] = DADDU(R_T6, R_T0, R_T1);
                emit_store_dn(e, R_T6, sreg, size);
                emit_add_flags(e, sh);
            } else {
                e->buf[e->len++] = DSUBU(R_T6, R_T1, R_T0);
                emit_store_dn(e, R_T6, sreg, size);
                emit_sub_flags(e, sh, 1);
            }
            e->buf[e->len++] = ADDIU(R_A1, R_A1, size == 2 ? -8 : -4);
            *cmax += (size == 2) ? 8 : 4;
            return 2;
        }
        // MOVEA.l Dn/An,Am (charge 12 - rig-measured flat .l rate; no flags).
        if (top == 0x2 && opmode == 1 && smode <= 1) {
            int h;
            if (smode)
                h = rc_an(e, sreg, R_T0);
            else {
                e->buf[e->len++] = LW(R_T0, R_A0, M64K_OFF_DREGS + 4 * sreg);
                h = R_T0;
            }
            rc_an_set(e, dreg, h);
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -12);
            *cmax += 12;
            return 2;
        }
        // MOVEA.w Dn/An,Am (sign-extended word, charge 4, no flags).
        if (top == 0x3 && opmode == 1 && smode <= 1) {
            int off = (smode ? M64K_OFF_AREGS : M64K_OFF_DREGS) + 4 * sreg + 2;
            e->buf[e->len++] = ITYPE(0x21, R_A0, R_T0, off);  // lh (sign-extends)
            rc_an_set(e, dreg, R_T0);
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -4);
            *cmax += 4;
            return 2;
        }
        // MOVE.l memory forms (generic base 12 + EA: (An) 4, (d16,An) 8).
        // Flags = tst_long idiom on the 32-bit value; MOVEA sets none.
        // move.l (An),Dn / movea.l (An),Am [16 cycles] — 0x031802, 37k/win.
        if (top == 0x2 && (opmode == 0 || opmode == 1) && smode == 2) {
#ifdef M64K_DYN_NO_R2MEM
            return 0;                   // TRCRC bisect knob
#endif
            int to_an = (opmode == 1);
            int h = rc_an(e, sreg, R_T4);
            emit_bail_check(e, h);
            e->buf[e->len++] = OR(R_T3, h, R_A2);
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -16);
            e->buf[e->len++] = LWL(R_T0, R_T3, 0);   // lw_m68k pair
            e->buf[e->len++] = LWR(R_T0, R_T3, 3);
            if (to_an)
                rc_an_set(e, dreg, R_T0);
            else {
                e->buf[e->len++] = SW(R_T0, R_A0, M64K_OFF_DREGS + 4 * dreg);
                e->buf[e->len++] = MOVE(R_S6, R_T0);
                e->buf[e->len++] = AND(R_S7, R_T0, R_V1);
            }
            emit_break_check(e, e->goff + 2);
            *cmax += 16;
            return 2;
        }
        // move(a).l (d16,An),Dm/Am [movel_fsrc_d16, 20 cycles, 2 words].
        if (top == 0x2 && (opmode == 0 || opmode == 1) && smode == 5) {
#ifdef M64K_DYN_NO_R2MEM
            return 0;                   // TRCRC bisect knob
#endif
            int to_an = (opmode == 1);
            int16_t d16 = (int16_t)fetch16(pc + 2);
            int h = rc_an(e, sreg, R_T4);
            e->buf[e->len++] = ADDIU(R_T4, h, d16);
            emit_bail_check(e, R_T4);
            e->buf[e->len++] = OR(R_T3, R_T4, R_A2);
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -20);
            e->buf[e->len++] = LWL(R_T0, R_T3, 0);
            e->buf[e->len++] = LWR(R_T0, R_T3, 3);
            if (to_an)
                rc_an_set(e, dreg, R_T0);
            else {
                e->buf[e->len++] = SW(R_T0, R_A0, M64K_OFF_DREGS + 4 * dreg);
                e->buf[e->len++] = MOVE(R_S6, R_T0);
                e->buf[e->len++] = AND(R_S7, R_T0, R_V1);
            }
            emit_break_check(e, e->goff + 4);
            *cmax += 20;
            return 4;
        }
        // move.l Dn/An,(d16,Am) [movel_f_dst_d16, 20 cycles, 2 words] —
        // 0x001598, 17k/win.
        if (top == 0x2 && opmode == 5 && smode <= 1) {
#ifdef M64K_DYN_NO_R2MEM
            return 0;                   // TRCRC bisect knob
#endif
            int16_t d16 = (int16_t)fetch16(pc + 2);
            int hv;
            if (smode)
                hv = rc_an(e, sreg, R_T6);
            else {
                e->buf[e->len++] = LW(R_T6, R_A0, M64K_OFF_DREGS + 4 * sreg);
                hv = R_T6;
            }
            int hd = rc_an(e, dreg, R_T4);   // <=2 live An: cannot evict hv
            e->buf[e->len++] = ADDIU(R_T4, hd, d16);
            emit_bail_check(e, R_T4);
            e->buf[e->len++] = OR(R_T3, R_T4, R_A2);
            // MMIO store-source law: the TLB trap decoder reads the stored
            // value from the saved t6, whatever rt the faulting SWL names —
            // a cached An (t7..t9) must be copied to t6 first.
            if (hv != R_T6)
                e->buf[e->len++] = MOVE(R_T6, hv);
            e->buf[e->len++] = MOVE(R_S6, R_T6);
            e->buf[e->len++] = AND(R_S7, R_T6, R_V1);
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -20);
            e->buf[e->len++] = SWL(R_T6, R_T3, 0);   // sw_m68k pair
            e->buf[e->len++] = SWR(R_T6, R_T3, 3);
            emit_break_check(e, e->goff + 4);
            *cmax += 20;
            return 4;
        }
        // MOVE.l Dn,Dm (charge 12 - rig-measured flat .l rate; flags =
        // tst_long idiom).
        if (top == 0x2 && opmode == 0 && smode == 0) {
            e->buf[e->len++] = LW(R_T0, R_A0, M64K_OFF_DREGS + 4 * sreg);
            e->buf[e->len++] = SW(R_T0, R_A0, M64K_OFF_DREGS + 4 * dreg);
            e->buf[e->len++] = MOVE(R_S6, R_T0);
            e->buf[e->len++] = AND(R_S7, R_T0, R_V1);
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -12);
            *cmax += 12;
            return 2;
        }
    }

    // DBF Dn block ender (dbcc_dec, m64k_asm.S 3939-3978): decrement Dn.w;
    // pre-decrement value 0 => expire (charge 14), else loop (charge 10 in
    // branch_exec). DBF's condition is never true, so the cc-true path
    // (charge 12) is unreachable. LAW: 2-word self-loops (disp == -4) are
    // refused — the interpreter's M64K_BLOCKOPS fuses the hot copy/fill
    // shapes natively and an emitted ender would bypass that fusion.
    // The generic sets dptr = &DREGS[n]; emitted keeps parity.
    if ((op & 0xFFF8) == 0x51C8) {
        int reg = op & 7;
        int disp = (int16_t)fetch16(pc + 2);
        uint32_t target = pc + 2 + disp;
        int tk_delta = e->goff + 2 + disp;
        if (disp == -4)                 // BLOCKOPS shapes stay interpreted
            return 0;
        if (dyn_target_is_spin(target) || (target & 1))
            return 0;
        // 32-bit-wrapped targets (PC near 0, negative disp) change
        // pc_diff semantics — the interpreter maps them via jmp_exec's OR
        // (landing at 0xFFFFxxxx inside the window); delta arithmetic on
        // m_pc would escape the window. Stay generic (found by
        // DBcc.btest: m_pc=0xFEFFxxxx crash + wrong-code flag fails).
        if (target & 0xFF000000)
            return 0;
        if (tk_delta < -32000 || tk_delta > 32000)
            return 0;
        if (e->nchain > 2)
            return 0;
        e->buf[e->len++] = ADDIU(R_S2, R_A0, M64K_OFF_DREGS + 4 * reg);
        e->buf[e->len++] = LHU(R_T0, R_A0, M64K_OFF_DREGS + 4 * reg + 2);
        e->buf[e->len++] = ADDIU(R_T1, R_T0, -1);
        e->buf[e->len++] = SH(R_T1, R_A0, M64K_OFF_DREGS + 4 * reg + 2);
        e->buf[e->len++] = BEQ(R_T0, R_ZERO, 5);   // expire path (below)
        e->buf[e->len++] = NOP;
        // taken (loop back): chainable — a self-loop chains to its own
        // C_max gate, re-checking the budget every iteration.
        e->buf[e->len++] = ADDIU(R_A1, R_A1, -10);
        e->buf[e->len++] = ADDIU(R_T5, R_T5, tk_delta);
        e->chain[e->nchain].at = e->len;
        e->chain[e->nchain].target = target;
        e->chain[e->nchain].taken = 1;
        e->nchain++;
        e->buf[e->len++] = JABS(main_loop);
        e->buf[e->len++] = NOP;
        // expired: fall through past the DBF (2 words).
        e->buf[e->len++] = ADDIU(R_A1, R_A1, -14);
        e->buf[e->len++] = ADDIU(R_T5, R_T5, e->goff + 4);
        e->buf[e->len++] = JABS(main_loop);
        e->buf[e->len++] = NOP;
        *cmax += 14;
        e->ended = 1;
        return 4;
    }

    // Bcc / BRA block enders (cond field from bcc_cctable, m64k_asm.S
    // ~3804-3870; flag encodings: C=bit32(s7), Z=(low32(s7)==0),
    // N=bit31(s6), N^V=bit63(s6), V=bit63^bit31). Displacement and both
    // exit PCs baked; charges: taken 10 (branch_exec), not-taken 8/12 by
    // form. Both exits are chainable J slots. BSR (cond 1) stays generic.
    // A baked target on the jmp_exec idle-skip list refuses the ender
    // (the emitted branch would bypass the spin fast-forward).
    if ((op & 0xF000) == 0x6000 && ((op >> 8) & 0xF) != 1) {
        int cond = (op >> 8) & 0xF;
        int disp8 = (int8_t)(op & 0xFF);
        int glen = (disp8 == 0) ? 4 : 2;
        int disp = (disp8 == 0) ? (int16_t)fetch16(pc + 2) : disp8;
        int c_nt = (disp8 == 0) ? 12 : 8;
        uint32_t target = pc + 2 + disp;
        int tk_delta = e->goff + 2 + disp;
        int nt_delta = e->goff + glen;
        if (dyn_target_is_spin(target))
            return 0;
        if (target & 1)                 // odd target: jmp_exec raises the
            return 0;                   // PC address error — stay generic
        if (target & 0xFF000000)        // 32-bit wrap: pc_diff changes —
            return 0;                   // stay generic (see DBF note)
        if (tk_delta < -32000 || tk_delta > 32000)
            return 0;
        if (e->nchain > 2)              // room for both exits
            return 0;

        // cc mini-sequence: branches with symbolic targets, offsets fixed
        // once the sequence length is known (exits are 4 words each, the
        // not-taken exit is the fallthrough).
        uint32_t cc[10];
        int ncc = 0, nbr = 0;
        struct { int at; int to_taken; } br[2];
        #define CCI(w)        (cc[ncc++] = (w))
        #define CCB(w, tk)    (br[nbr].at = ncc, br[nbr].to_taken = (tk), \
                               nbr++, cc[ncc++] = (w), cc[ncc++] = NOP)
        switch (cond) {
        case 0x0: break;                                    // BRA
        case 0x2:                                           // HI: !C && !Z
            CCI(DSRL32(R_T0, R_S7, 0)); CCI(ANDI(R_T0, R_T0, 1));
            CCB(BNE(R_T0, R_ZERO, 0), 0);
            CCI(AND(R_T0, R_S7, R_V1));
            CCB(BNE(R_T0, R_ZERO, 0), 1);
            break;
        case 0x3:                                           // LS: C || Z
            CCI(DSRL32(R_T0, R_S7, 0)); CCI(ANDI(R_T0, R_T0, 1));
            CCB(BNE(R_T0, R_ZERO, 0), 1);
            CCI(AND(R_T0, R_S7, R_V1));
            CCB(BEQ(R_T0, R_ZERO, 0), 1);
            break;
        case 0x4:                                           // CC: !C
            CCI(DSRL32(R_T0, R_S7, 0)); CCI(ANDI(R_T0, R_T0, 1));
            CCB(BEQ(R_T0, R_ZERO, 0), 1);
            break;
        case 0x5:                                           // CS: C
            CCI(DSRL32(R_T0, R_S7, 0)); CCI(ANDI(R_T0, R_T0, 1));
            CCB(BNE(R_T0, R_ZERO, 0), 1);
            break;
        case 0x6:                                           // NE: !Z
            CCI(AND(R_T0, R_S7, R_V1));
            CCB(BNE(R_T0, R_ZERO, 0), 1);
            break;
        case 0x7:                                           // EQ: Z
            CCI(AND(R_T0, R_S7, R_V1));
            CCB(BEQ(R_T0, R_ZERO, 0), 1);
            break;
        case 0x8:                                           // VC: !V
            CCI(DSRL32(R_T0, R_S6, 31)); CCI(SRLI(R_T1, R_S6, 31));
            CCI(XOR(R_T0, R_T0, R_T1));
            CCB(BEQ(R_T0, R_ZERO, 0), 1);
            break;
        case 0x9:                                           // VS: V
            CCI(DSRL32(R_T0, R_S6, 31)); CCI(SRLI(R_T1, R_S6, 31));
            CCI(XOR(R_T0, R_T0, R_T1));
            CCB(BNE(R_T0, R_ZERO, 0), 1);
            break;
        case 0xA:                                           // PL: !N
            CCI(SRLI(R_T0, R_S6, 31));
            CCB(BEQ(R_T0, R_ZERO, 0), 1);
            break;
        case 0xB:                                           // MI: N
            CCI(SRLI(R_T0, R_S6, 31));
            CCB(BNE(R_T0, R_ZERO, 0), 1);
            break;
        case 0xC:                                           // GE: !(N^V)
            CCB(BGEZ(R_S6, 0), 1);
            break;
        case 0xD:                                           // LT: N^V
            CCB(BLTZ(R_S6, 0), 1);
            break;
        case 0xE:                                           // GT: !(N^V) && !Z
            CCB(BLTZ(R_S6, 0), 0);
            CCI(AND(R_T0, R_S7, R_V1));
            CCB(BNE(R_T0, R_ZERO, 0), 1);
            break;
        case 0xF:                                           // LE: (N^V) || Z
            CCB(BLTZ(R_S6, 0), 1);
            CCI(AND(R_T0, R_S7, R_V1));
            CCB(BEQ(R_T0, R_ZERO, 0), 1);
            break;
        }
        #undef CCI
        #undef CCB
        // Fix symbolic branch offsets: not-taken exit starts at ncc,
        // taken exit at ncc+4 (or at ncc for BRA, which has no nt exit).
        for (int i = 0; i < nbr; i++) {
            int tgt = br[i].to_taken ? ncc + 4 : ncc;
            cc[br[i].at] |= (uint16_t)(int16_t)(tgt - (br[i].at + 1));
        }
        for (int i = 0; i < ncc; i++)
            e->buf[e->len++] = cc[i];
        if (cond != 0x0) {              // not-taken exit (fallthrough)
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -c_nt);
            e->buf[e->len++] = ADDIU(R_T5, R_T5, nt_delta);
            e->chain[e->nchain].at = e->len;
            e->chain[e->nchain].target = pc + glen;  // fallthrough head
            e->chain[e->nchain].taken = 0;
            e->nchain++;
            e->buf[e->len++] = JABS(main_loop);
            e->buf[e->len++] = NOP;
        }
        // taken exit
        e->buf[e->len++] = ADDIU(R_A1, R_A1, -10);
        e->buf[e->len++] = ADDIU(R_T5, R_T5, tk_delta);
        e->chain[e->nchain].at = e->len;
        e->chain[e->nchain].target = target;
        e->chain[e->nchain].taken = 1;
        e->nchain++;
        e->buf[e->len++] = JABS(main_loop);
        e->buf[e->len++] = NOP;
        *cmax += (c_nt > 10) ? c_nt : 10;
        e->ended = 1;
        return glen;
    }

    // RTS block ender (generic OP(rts): use_cycles 16, pop32, j jmp_exec).
    // The return target is dynamic, so there is no static chain slot —
    // instead the ender jumps to jmp_exec itself, which does map_m68k +
    // pc_diff + the PC address-error raise + the idle-skip compares + the
    // dynarec probe. That probe is what chains the return dynamically.
    // IR is stored explicitly (0x4E75): dispatch normally maintains it and
    // address_error's fixup dispatches on it (addrerr_fixup_rts) — emitted
    // blocks never touch IR, so without this an odd return address would
    // take the wrong fixup. Odd SP bails before any mutation (rts_fast's
    // contract: the generic replay raises read_address_error itself).
    if (op == 0x4E75) {
#ifdef M64K_DYN_NO_RTS
        return 0;                       // TRCRC bisect knob
#endif
        int h = rc_an(e, 7, R_T2);
        emit_bail_check(e, h);          // clobbers t1 only
        e->buf[e->len++] = ORI(R_T1, R_ZERO, 0x4E75);
        e->buf[e->len++] = SW(R_T1, R_A0, M64K_OFF_IR);
        e->buf[e->len++] = ADDIU(R_A1, R_A1, -16);
        e->buf[e->len++] = ADDIU(R_T1, h, 4);
        e->buf[e->len++] = OR(R_T3, h, R_A2);      // map_m68k of the OLD sp
        rc_an_set(e, 7, R_T1);
        e->buf[e->len++] = LWL(R_T0, R_T3, 0);     // lw_m68k: loads->t0 law
        e->buf[e->len++] = LWR(R_T0, R_T3, 3);
        e->buf[e->len++] = MOVE(R_T4, R_T0);       // eaptr32 = target
        e->buf[e->len++] = JABS(jmp_exec);
        e->buf[e->len++] = NOP;
        *cmax += 16;
        e->ended = 1;
        return 2;
    }

    // JMP (d16,PC) / JMP (xxx).l block enders (jmp_exec path; TIMING_
    // ACCURACY 0 charges: 2 + ea = 10 / 14). Static targets baked like
    // BRA; the abs form leaves the delta window, so it materializes the
    // mapped m_pc (lui sign-extends like map_m68k's or-with-mask) and
    // stores the recomputed pc_diff constant. Both exits chainable.
    if (op == 0x4EFA || op == 0x4EF9) {
        int is_abs = (op == 0x4EF9);
        uint32_t target; int glen, charge, tk_delta = 0;
        if (is_abs) {
            target = ((uint32_t)fetch16(pc + 2) << 16) | fetch16(pc + 4);
            glen = 6; charge = 14;
        } else {
            int16_t d16 = (int16_t)fetch16(pc + 2);
            target = pc + 2 + d16;
            tk_delta = e->goff + 2 + d16;
            glen = 4; charge = 10;
            if (target & 0xFF000000)    // 32-bit wrap: pc_diff changes
                return 0;
            if (tk_delta < -32000 || tk_delta > 32000)
                return 0;
        }
        if (dyn_target_is_spin(target) || (target & 1))
            return 0;
        if (e->nchain > 3)
            return 0;
        e->buf[e->len++] = ADDIU(R_A1, R_A1, -charge);
        if (is_abs) {
            uint32_t host = target | 0xFF000000u;
            uint32_t diff = target - host;      // 32-bit subu semantics
            e->buf[e->len++] = LUI(R_T5, host >> 16);
            e->buf[e->len++] = ORI(R_T5, R_T5, host & 0xFFFF);
            e->buf[e->len++] = LUI(R_T1, diff >> 16);
            e->buf[e->len++] = ORI(R_T1, R_T1, diff & 0xFFFF);
            e->buf[e->len++] = LUI(R_T2, HI16(&pc_diff));
            e->buf[e->len++] = SW(R_T1, R_T2, LO16(&pc_diff));
        } else {
            e->buf[e->len++] = ADDIU(R_T5, R_T5, tk_delta);
        }
        e->chain[e->nchain].at = e->len;
        e->chain[e->nchain].target = target;
        e->chain[e->nchain].taken = 1;
        e->nchain++;
        e->buf[e->len++] = JABS(main_loop);
        e->buf[e->len++] = NOP;
        *cmax += charge;
        e->ended = 1;
        return glen;
    }

    // JSR (d16,PC) / JSR (xxx).l / BSR.b/.w block enders (call edges).
    // Body transcribed from the W3 jsr/bsr fast paths (m64k_asm.S): odd
    // new-SP bails BEFORE any mutation (generic replay then decrements SP
    // and raises write_address_error itself); charges land pre-push
    // (16/20/16) + 2 (jmp_exec_cycles) before the target update, so an
    // MMIO trap on the push observes the interpreter's exact mid-insn
    // clock. Return address = next-insn guest pc, baked. The a1 slice-
    // break check runs AFTER m_pc moves to the callee, so its stub is a
    // plain J main_loop at the interpreter's own exit boundary.
    if (op == 0x4EBA || op == 0x4EB9 || (op & 0xFF00) == 0x6100) {
        uint32_t target; int glen, pre, is_abs = 0, tk_delta = 0;
        if (op == 0x4EBA) {
            int16_t d16 = (int16_t)fetch16(pc + 2);
            target = pc + 2 + d16; glen = 4; pre = 16;
            tk_delta = e->goff + 2 + d16;
        } else if (op == 0x4EB9) {
            target = ((uint32_t)fetch16(pc + 2) << 16) | fetch16(pc + 4);
            glen = 6; pre = 20; is_abs = 1;
        } else {                        // BSR
            int8_t d8 = (int8_t)(op & 0xFF);
            if (d8 == 0) {
                int16_t d16 = (int16_t)fetch16(pc + 2);
                target = pc + 2 + d16; glen = 4;
                tk_delta = e->goff + 2 + d16;
            } else {
                target = pc + 2 + d8; glen = 2;
                tk_delta = e->goff + 2 + d8;
            }
            pre = 16;
        }
        if (!is_abs) {
            if (target & 0xFF000000)
                return 0;
            if (tk_delta < -32000 || tk_delta > 32000)
                return 0;
        }
        if (dyn_target_is_spin(target) || (target & 1))
            return 0;
        if (e->nchain > 3)
            return 0;
        uint32_t ret = pc + glen;       // return address (guest space)
        int h = rc_an(e, 7, R_T1);
        e->buf[e->len++] = ADDIU(R_T2, h, -4);
        emit_bail_check(e, R_T2);
        e->buf[e->len++] = LUI(R_T6, ret >> 16);
        e->buf[e->len++] = ORI(R_T6, R_T6, ret & 0xFFFF);
        e->buf[e->len++] = ADDIU(R_A1, R_A1, -pre);
        rc_an_set(e, 7, R_T2);
        e->buf[e->len++] = OR(R_T3, R_T2, R_A2);
        e->buf[e->len++] = SWL(R_T6, R_T3, 0);  // sw_m68k result idiom
        e->buf[e->len++] = SWR(R_T6, R_T3, 3);
        e->buf[e->len++] = ADDIU(R_A1, R_A1, -2);
        if (is_abs) {
            uint32_t host = target | 0xFF000000u;
            uint32_t diff = target - host;
            e->buf[e->len++] = LUI(R_T5, host >> 16);
            e->buf[e->len++] = ORI(R_T5, R_T5, host & 0xFFFF);
            e->buf[e->len++] = LUI(R_T1, diff >> 16);
            e->buf[e->len++] = ORI(R_T1, R_T1, diff & 0xFFFF);
            e->buf[e->len++] = LUI(R_T2, HI16(&pc_diff));
            e->buf[e->len++] = SW(R_T1, R_T2, LO16(&pc_diff));
        } else {
            e->buf[e->len++] = ADDIU(R_T5, R_T5, tk_delta);
        }
        emit_break_check(e, 0);         // m_pc already at the callee
        e->chain[e->nchain].at = e->len;
        e->chain[e->nchain].target = target;
        e->chain[e->nchain].taken = 1;
        e->nchain++;
        e->buf[e->len++] = JABS(main_loop);
        e->buf[e->len++] = NOP;
        *cmax += pre + 2;
        e->ended = 1;
        return glen;
    }
    return 0;
}

// ---- Translation service (slice boundaries) ------------------------------
// Tried-filter: one translation attempt per head (translation is
// deterministic, so a refusal is stable). EXACT tag table — the original
// 16K-bit bitmap silently hash-shadowed heads (the 00ceec/00cf2e upload
// loops, 80k misses/window, collided with 0x4EEC-class heads and were
// never attempted). 23-bit head key = 13-bit slot + 10-bit tag + valid;
// a collision REPLACES the previous tag, so a shadowed head retries later
// instead of being blacklisted forever (bounded re-translate churn).
// (Array lives with the other big aligned statics near dyn_s; dyn_s.tried
// is retired but kept in the struct for layout stability.)
//
// Entries carry the OUTCOME (bit14 = refused): a head marked translated
// whose block is no longer in the table was EVICTED by a set conflict —
// it must be eligible for re-translation, or a 2-way set war permanently
// silences a hot head with the filter still claiming it was handled
// (observed: the 00ceec/00cf2e upload loops at full miss heat with no
// DYNREF). Known-refused heads stay skipped (translation deterministic).
static bool dyn_tried_check(uint32_t pc)
{
    uint32_t h = (pc & 0xFFFFFF) >> 1;
    uint16_t e = dyn_tried2[h & 8191];
    uint16_t tag = (uint16_t)((h >> 13) | 0x8000u);
    if (e == (uint16_t)(tag | 0x4000))
        return true;                    // known-refused
    if (e == tag && dyn_lookup(pc))
        return true;                    // translated and still live
    return false;                       // untried, shadowed, or evicted
}
static void dyn_tried_record(uint32_t pc, bool refused)
{
    uint32_t h = (pc & 0xFFFFFF) >> 1;
    dyn_tried2[h & 8191] =
        (uint16_t)((h >> 13) | 0x8000u | (refused ? 0x4000u : 0));
}

// Drop every pending chain link waiting on a target that can never get a
// block (refused/shadowed): the slots stay J main_loop, which is correct.
static void dyn_pending_drop(uint32_t target)
{
    for (int i = 0; i < dyn_npending; ) {
        if (dyn_pending[i].target == target)
            dyn_pending[i] = dyn_pending[--dyn_npending];
        else
            i++;
    }
}

// Per-slice translation budget. The mailbox admits at most one head per
// slice and hot already-tried heads win its last-writer lottery almost
// every time — measured 2026-08-07 at 18 blocks per 480s, far too slow
// for chain webs to form. Seeding closes the loop: the pending list IS
// the frontier of the web (every unresolved exit of every block), so
// translating pending targets directly grows a connected web around each
// mailbox seed instead of waiting for each successor to win the lottery.
#define DYN_SEEDS_PER_SLICE 4

// Block length cap (TRCRC bisect knob: rung-2 raised it 8 -> 24, which
// widens which templates actually run as blocks).
#ifndef M64K_DYN_MAXINSN
#define M64K_DYN_MAXINSN 24
#endif

void __m64k_dyn_service(m64k_t *m64k)
{
    if (!__m64k_dyn_enable)
        return;
    int budget = DYN_SEEDS_PER_SLICE;
    for (int mb = 0; mb < 2; mb++) {
        uint32_t pc = __m64k_dyn_mailbox[mb];
        if (!pc)
            continue;
        __m64k_dyn_mailbox[mb] = 0;
        #ifdef M64K_DYNSTAT
        // Service visibility (bounded): RAW mailbox value (unmasked — a
        // mapped 0xFFxxxxxx here would explain a lot), tried verdict,
        // translate outcome. Unconditional for the two mystery heads.
        {
            static int mblog;
            int interesting = ((pc & 0xFFFFFF) == 0xCEEC)
                           || ((pc & 0xFFFFFF) == 0xCF2E);
            if (mblog < 30 || interesting) {
                if (mblog < 1000) mblog++;
                int tried = dyn_tried_check(pc);
                debugf("[DYNMB] mb=%d raw=%08lx tried=%d\n",
                       mb, (unsigned long)pc, tried);
            }
        }
        #endif
        if (!dyn_tried_check(pc)) {
            budget--;
            int n = m64k_dyn_translate(m64k, pc, M64K_DYN_MAXINSN, false);
            dyn_tried_record(pc, n == 0);
            #ifdef M64K_DYNSTAT
            if (((pc & 0xFFFFFF) == 0xCEEC) || ((pc & 0xFFFFFF) == 0xCF2E))
                debugf("[DYNMB] translate %06lx -> %d\n",
                       (unsigned long)(pc & 0xFFFFFF), n);
            #endif
        }
    }
    // Chain-target seeding (deferred-work law holds: this is the same
    // slice-boundary call site). A successful translate patches and
    // removes every pending entry for that target (insert path), so the
    // index only advances past entries this pass cannot retire.
    for (int i = 0; i < dyn_npending && budget > 0; ) {
        uint32_t t = dyn_pending[i].target;
        uint32_t *host = dyn_lookup(t);
        if (host) {                     // target got a block: patch now
            *dyn_pending[i].slot = JABS(host);
            data_cache_hit_writeback(dyn_pending[i].slot, 4);
            inst_cache_hit_invalidate(dyn_pending[i].slot, 4);
            __m64k_dyn_stat_chains++;
            dyn_pending[i] = dyn_pending[--dyn_npending];
            continue;
        }
        if (dyn_tried_check(t)) {       // known-refused (host==NULL above
            dyn_pending_drop(t);        // rules out live): drop
            continue;
        }
        budget--;
        int n = m64k_dyn_translate(m64k, t, M64K_DYN_MAXINSN, false);
        dyn_tried_record(t, n == 0);
        if (n == 0)
            dyn_pending_drop(t);
    }
}

// ---- Translator ----------------------------------------------------------
// Translate a straight-line block starting at guest pc (up to max_insns
// supported forms) and insert it into the probe table. force=true skips the
// C_max entry gate (testsuite forced mode: vectors run with m_cycles=1,
// which the gate would always refuse; single-insn blocks are exact there).
// Returns the number of guest insns translated (0 = nothing inserted).
int m64k_dyn_translate(m64k_t *m64k, uint32_t pc, int max_insns, bool force)
{
    (void)m64k;
    if (pc & 1)
        return 0;
    if ((pc & 0x00FFFFFF) < 0x80)   // vector-swap page: never translated
        return 0;

    emit_t e = { .len = 0, .goff = 0, .nfix = 0,
                 .rc_on = dyn_s.rc_enable,
                 .rc_slot = { -1, -1, -1 } };
    int ninsns = 0, cmax = 0, guest_len = 0;
    while (ninsns < max_insns && e.len < EMIT_MAXWORDS - 24 && e.nfix < EMIT_MAXFIX - 4) {
        int save = e.len, savefix = e.nfix;
        // Regcache rollback snapshot: a refused form must not leave stale
        // bindings whose fill instructions were rolled back with e.len.
        int8_t save_rc[3] = { e.rc_slot[0], e.rc_slot[1], e.rc_slot[2] };
        uint8_t save_age[3] = { e.rc_age[0], e.rc_age[1], e.rc_age[2] };
        uint8_t save_tick = e.rc_tick;
        int save_hits = e.rc_hits;
        e.goff = guest_len;
        int gl = emit_insn(fetch16(pc + guest_len), pc + guest_len, &e, &cmax);
        if (gl == 0) {
            e.len = save;
            e.nfix = savefix;
            memcpy(e.rc_slot, save_rc, sizeof(save_rc));
            memcpy(e.rc_age, save_age, sizeof(save_age));
            e.rc_tick = save_tick;
            e.rc_hits = save_hits;
            break;
        }
        guest_len += gl;
        ninsns++;
        if (e.ended)
            break;
    }
    if (ninsns == 0) {
        __m64k_dyn_stat_refused++;
        #ifdef M64K_DYNSTAT
        debugf("[DYNREF] pc=%06lx op=%04x head\n",
               (unsigned long)(pc & 0xFFFFFF), fetch16(pc));
        #endif
        return 0;
    }
    // Ender-terminated blocks only (phase-3d lesson): a block that ends at
    // an unsupported form ("straight-line tail") on a hot loop head gets
    // re-entered through the probe EVERY iteration, paying entry+gate+tail
    // +icache ping-pong on top of the interpreted remainder — measured as
    // the 39->25fps collapse (min-length-insensitive: the hot loop bodies
    // are 3+ insns). Blocks must earn their entry with an emitted ender.
    if (!force && !e.ended) {
        __m64k_dyn_stat_refused++;
        #ifdef M64K_DYNSTAT
        // Coverage work list: the form that terminated decoding before an
        // ender (the whole block is refused, so this opcode is exactly what
        // keeps this head untranslated). Correlate head= with [DYNH] heat.
        debugf("[DYNTERM] head=%06lx stop=%06lx op=%04x n=%d\n",
               (unsigned long)(pc & 0xFFFFFF),
               (unsigned long)((pc + guest_len) & 0xFFFFFF),
               fetch16(pc + guest_len), ninsns);
        #endif
        return 0;
    }
    // No min-length filter (was M64K_DYN_MINLEN=3): it guarded against
    // straight-line tails re-entering the probe every loop iteration
    // (the phase-3d collapse), but the ender-only policy above already
    // refuses those, and an ENDER-terminated short block chains (incl.
    // to itself) so its entry overhead is paid once, not per iteration.
    // The hottest guest shapes are 2-insn poll loops (tst+bcc, e.g.
    // 0x0318A6 at ~55 transfers/frame) that minlen=3 refused wholesale.

    // Block frame: [C_max gate] body tail [bail stubs].
    uint32_t block[EMIT_MAXWORDS + 8 + 3 * EMIT_MAXFIX];
    int n = 0;
    if (!force) {
        block[n++] = SLTI(R_T0, R_A1, cmax + 1);
        block[n++] = BEQ(R_T0, R_ZERO, 3);      // gate passes -> skip bail
        block[n++] = NOP;
        block[n++] = JABS(main_loop);           // bail: interpreter runs it
        block[n++] = NOP;
    }
    #ifdef M64K_DYNSTAT
    // Residency counter, past the gate so only real entries count (t0/t1
    // are dead here: the gate owns t0, chained entries land on the gate).
    if (!force) {
        block[n++] = LUI(R_T0, HI16(&__m64k_dyn_stat_exec));
        block[n++] = LW(R_T1, R_T0, LO16(&__m64k_dyn_stat_exec));
        block[n++] = ADDIU(R_T1, R_T1, ninsns);
        block[n++] = SW(R_T1, R_T0, LO16(&__m64k_dyn_stat_exec));
    }
    #endif
    int body_at = n;
    memcpy(block + n, e.buf, e.len * 4);
    n += e.len;
    if (!e.ended) {
        // Straight-line tail (block ended at an unsupported form): the
        // interpreter must decode the next insn, so no chaining here.
        block[n++] = ADDIU(R_T5, R_T5, guest_len);
        block[n++] = JABS(main_loop);
        block[n++] = NOP;
    }
    // Ender exits already set m_pc; resolve their chain slots now if the
    // target block exists (forced/testsuite blocks never chain: they have
    // no C_max gate, so a chained loop would never re-check the budget).
    // Chain policy: ALL exits (taken + fallthrough). The 2026-08-07
    // frame-377 quarantine of fallthrough chaining was an instrumentation
    // artifact: the bisect builds carried the BOSTAT/DYNSTAT As-corruption
    // (port-blockop fire counter clobbered t8 = As, "restored" as the
    // constant 22 — fixed 2026-08-08), which forks guest content at the
    // first fired port copy. Clean matched-pair retest: TRCRC identical
    // over the full 9573-frame overlap with fallthrough chaining on.
    // Bisect knob kept: -DDYN_CHAIN_TAKEN_ONLY restores the old policy.
    if (!force) {
        for (int i = 0; i < e.nchain; i++) {
            #ifdef DYN_CHAIN_TAKEN_ONLY
            if (!e.chain[i].taken) continue;
            #endif
            uint32_t *host = dyn_lookup(e.chain[i].target);
            if (host) {
                block[body_at + e.chain[i].at] = JABS(host);
                __m64k_dyn_stat_chains++;
                #ifdef DYN_CHAIN_LOG
                debugf("[CHAIN] %06lx->%06lx %s emit\n",
                       (unsigned long)(pc & 0xFFFFFF),
                       (unsigned long)(e.chain[i].target & 0xFFFFFF),
                       e.chain[i].taken ? "tk" : "ft");
                #endif
            }
        }
    }
    // Address-error bail stubs: m_pc -> the checking insn, then replay
    // generically (nothing was mutated before the check fired).
    for (int i = 0; i < e.nfix; i++) {
        int stub = n;
        if (e.fix[i].goff != 0)
            block[n++] = ADDIU(R_T5, R_T5, e.fix[i].goff);
        block[n++] = JABS(main_loop);
        block[n++] = NOP;
        int br = body_at + e.fix[i].at;
        block[br] |= (uint16_t)(stub - (br + 1));  // patch branch offset
    }

    int bytes = n * 4;
    if (dyn_arena_used + bytes + 8 > M64K_DYN_ARENA_SIZE)
        return 0;
    uint8_t *dst = __m64k_dyn_arena + dyn_arena_used;
    dyn_arena_used += (bytes + 7) & ~7;
    __m64k_dyn_publish(dst, block, bytes);

    // Table insert: code published above, tag written LAST (publish order).
    // Victim way when the set is full: keyed by pc bit 1, not fixed way 0 —
    // a three-head set with a fixed victim evicts the same resident every
    // time (and the tried-filter then has to keep re-translating it).
    uint32_t *set = &__m64k_dyn_table[((pc >> 1) & (M64K_DYN_TABLE_SETS - 1)) * 4];
    int way;
    if (set[0] == 0xFFFFFFFF)      way = 0;
    else if (set[2] == 0xFFFFFFFF) way = 1;
    else                           way = (pc >> 1) & 1;
    #ifdef M64K_DYNSTAT
    if (set[way * 2] != 0xFFFFFFFF) {
        static int evlog;
        if (evlog < 40)
            { evlog++; debugf("[DYNEV] set=%lu evict=%06lx for=%06lx\n",
                (unsigned long)((pc >> 1) & (M64K_DYN_TABLE_SETS - 1)),
                (unsigned long)(set[way * 2] & 0xFFFFFF),
                (unsigned long)(pc & 0xFFFFFF)); }
    }
    #endif
    set[way * 2 + 1] = (uint32_t)(uintptr_t)dst;
    set[way * 2 + 0] = pc;

    if (!force) {
        // Register this block's unresolved exits as pending chain links,
        // then patch any earlier blocks waiting on this head (a self-loop
        // registers above and resolves here, landing on its own gate).
        for (int i = 0; i < e.nchain; i++) {
            #ifdef DYN_CHAIN_TAKEN_ONLY
            if (!e.chain[i].taken) continue;
            #endif
            if (block[body_at + e.chain[i].at] == JABS(main_loop)
                && dyn_npending < 512) {
                dyn_pending[dyn_npending].slot =
                    (uint32_t *)(dst + 4 * (body_at + e.chain[i].at));
                dyn_pending[dyn_npending].target = e.chain[i].target;
                dyn_npending++;
            }
        }
        for (int i = 0; i < dyn_npending; i++) {
            if (dyn_pending[i].target == pc) {
                *dyn_pending[i].slot = JABS(dst);
                data_cache_hit_writeback(dyn_pending[i].slot, 4);
                inst_cache_hit_invalidate(dyn_pending[i].slot, 4);
                __m64k_dyn_stat_chains++;
                #ifdef DYN_CHAIN_LOG
                debugf("[CHAIN] ->%06lx patch slot %p\n",
                       (unsigned long)(pc & 0xFFFFFF),
                       (void *)dyn_pending[i].slot);
                #endif
                dyn_pending[i] = dyn_pending[--dyn_npending];
                i--;
            }
        }
    }
    __m64k_dyn_stat_blocks++;
    __m64k_dyn_stat_insns += ninsns;
    #ifdef M64K_DYNSTAT
    // Regcache fire evidence (vacuous-gate law): translate-time count of
    // An loads eliminated in this block.
    if (e.rc_hits)
        debugf("[DYNRC] pc=%06lx hits=%d\n",
               (unsigned long)(pc & 0xFFFFFF), e.rc_hits);
    #endif
    return ninsns;
}

#endif // M64K_DYNREC
