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
uint8_t  __m64k_dyn_arena[M64K_DYN_ARENA_SIZE] __attribute__((aligned(32)));
uint32_t __m64k_dyn_table[M64K_DYN_TABLE_SETS * 4] __attribute__((aligned(16)));
static uint32_t dyn_arena_used;

extern char main_loop[];

// Publish primitive: emitted code must be written back from dcache and the
// stale icache lines invalidated BEFORE the table tag makes it reachable.
void __m64k_dyn_publish(void *dst, const void *src, int len)
{
    memcpy(dst, src, len);
    data_cache_hit_writeback(dst, len);
    inst_cache_hit_invalidate(dst, len);
}

// Translation-trigger mailbox: the jmp_exec probe's miss edge stores the
// missed target here (last-writer-wins within a slice); m64k_run services
// it at slice boundaries (the deferred-work law: never translate/reclaim
// while guest code may be mid-flight in the arena).
uint32_t __m64k_dyn_mailbox;
static uint8_t dyn_tried[1024];  // 8192-bit once-only filter (hash collisions
                                 // only suppress translation, never break it)

void __m64k_dynrec_init(void)
{
    memset(__m64k_dyn_table, 0xFF, sizeof(__m64k_dyn_table));
    memset(dyn_tried, 0, sizeof(dyn_tried));
    __m64k_dyn_mailbox = 0;
    dyn_arena_used = 0;
}

void __m64k_dyn_service(m64k_t *m64k)
{
    uint32_t pc = __m64k_dyn_mailbox;
    if (!pc)
        return;
    __m64k_dyn_mailbox = 0;
    uint32_t h = (pc >> 1) & 8191;
    if (dyn_tried[h >> 3] & (1u << (h & 7)))
        return;
    dyn_tried[h >> 3] |= 1u << (h & 7);
    m64k_dyn_translate(m64k, pc, 8, false);
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
#define SH(rt,base,off)   ITYPE(0x29,base,rt,off)
#define OR(rd,rs,rt)      RTYPE(rs,rt,rd,0,0x25)
#define MOVE(rd,rs)       RTYPE(rs,R_ZERO,rd,0,0x21)  // addu rd, rs, zero
#define SLLI(rd,rt,sa)    RTYPE(0,rt,rd,sa,0x00)
#define DSLL(rd,rt,sa)    RTYPE(0,rt,rd,sa,0x38)
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

// ---- Templates -----------------------------------------------------------
// Emission context for one block. Address-error checks branch forward to a
// per-check bail stub emitted after the tail: the stub rewinds nothing
// (templates bail BEFORE any mutation, per the fast-path contract), sets
// m_pc to the *current* insn and jumps to main_loop so the generic path
// replays it from scratch and raises ADDRERR bit-exactly (check_addr_bail
// semantics; dispatch stores IR itself on the replay).
#define EMIT_MAXWORDS 160
#define EMIT_MAXFIX   20

typedef struct {
    uint32_t buf[EMIT_MAXWORDS];
    int len;            // emitted words
    int goff;           // guest byte offset of the insn being emitted
    struct { int at; int goff; } fix[EMIT_MAXFIX];
    int nfix;
} emit_t;

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
            e->buf[e->len++] = LW(R_T4, R_A0, M64K_OFF_AREGS + 4 * dreg);
            emit_bail_check(e, R_T4);
            e->buf[e->len++] = OR(R_T3, R_T4, R_A2);
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
            e->buf[e->len++] = LW(R_T4, R_A0, M64K_OFF_AREGS + 4 * dreg);
            emit_bail_check(e, R_T4);
            e->buf[e->len++] = ADDIU(R_T1, R_T4, 2);
            e->buf[e->len++] = SW(R_T1, R_A0, M64K_OFF_AREGS + 4 * dreg);
            e->buf[e->len++] = OR(R_T3, R_T4, R_A2);
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
            e->buf[e->len++] = LW(R_T4, R_A0, M64K_OFF_AREGS + 4 * sreg);
            emit_bail_check(e, R_T4);
            e->buf[e->len++] = ADDIU(R_T2, R_T4, 2);
            e->buf[e->len++] = SW(R_T2, R_A0, M64K_OFF_AREGS + 4 * sreg);
            e->buf[e->len++] = OR(R_T3, R_T4, R_A2);
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
            e->buf[e->len++] = LW(R_T4, R_A0, M64K_OFF_AREGS + 4 * sreg);
            e->buf[e->len++] = ADDIU(R_T4, R_T4, d16);
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
            e->buf[e->len++] = LW(R_T4, R_A0, M64K_OFF_AREGS + 4 * dreg);
            e->buf[e->len++] = ADDIU(R_T4, R_T4, d16);
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

        // (fall through to unsupported for other MOVE.w forms)
        // move.w #imm,(An)  [movew_fsrc_other tail, 12 cycles, 2 words].
        if (smode == 7 && sreg == 4 && dmode == 2) {
            uint16_t imm = fetch16(pc + 2);
            e->buf[e->len++] = LW(R_T4, R_A0, M64K_OFF_AREGS + 4 * dreg);
            emit_bail_check(e, R_T4);
            e->buf[e->len++] = OR(R_T3, R_T4, R_A2);
            e->buf[e->len++] = ORI(R_T6, R_ZERO, imm);
            emit_movew_flags(e, R_T6);
            e->buf[e->len++] = ADDIU(R_A1, R_A1, -12);
            e->buf[e->len++] = SH(R_T6, R_T3, 0);
            emit_break_check(e, e->goff + 4);
            *cmax += 12;
            return 4;
        }
    }

    // CMPI/ADDI templates: transcription-complete but GATED OFF — the
    // vector set has no immediate-form suites and a TRCRC flag-canary run
    // (240s) proved these forms never enter translated blocks in the test
    // window, so no active gate covers the hand-encodings. Enable once the
    // emitter-differential rig (synthetic sequences run through emitted
    // blocks vs the interpreter) exists to prove them.
#ifdef M64K_DYNREC_UNPROVEN
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
#endif // M64K_DYNREC_UNPROVEN
    return 0;
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

    emit_t e = { .len = 0, .goff = 0, .nfix = 0 };
    int ninsns = 0, cmax = 0, guest_len = 0;
    while (ninsns < max_insns && e.len < EMIT_MAXWORDS - 20 && e.nfix < EMIT_MAXFIX - 2) {
        int save = e.len, savefix = e.nfix;
        e.goff = guest_len;
        int gl = emit_insn(fetch16(pc + guest_len), pc + guest_len, &e, &cmax);
        if (gl == 0) {
            e.len = save;
            e.nfix = savefix;
            break;
        }
        guest_len += gl;
        ninsns++;
    }
    if (ninsns == 0)
        return 0;
    // Minimum profitable length (spec): entry overhead (probe hit + gate +
    // tail) exceeds the dispatch savings of short blocks — measured
    // net-negative at 1-3 insn blocks (2026-08-07 480s A/B). Forced mode
    // (testsuite) keeps single-insn blocks for coverage.
    if (!force && ninsns < 3)
        return 0;

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
    int body_at = n;
    memcpy(block + n, e.buf, e.len * 4);
    n += e.len;
    block[n++] = ADDIU(R_T5, R_T5, guest_len);  // m_pc -> next insn
    block[n++] = JABS(main_loop);
    block[n++] = NOP;
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
    uint32_t *set = &__m64k_dyn_table[((pc >> 1) & (M64K_DYN_TABLE_SETS - 1)) * 4];
    int way = (set[0] != 0xFFFFFFFF && set[2] == 0xFFFFFFFF) ? 1 : 0;
    set[way * 2 + 1] = (uint32_t)(uintptr_t)dst;
    set[way * 2 + 0] = pc;
    return ninsns;
}

#endif // M64K_DYNREC
