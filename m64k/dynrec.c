// m64k dynarec phase-2a: per-form MIPS emitter (DYNREC-PHASE2-TEMPLATES.md).
// Skeleton increment: MOVEQ only, straight-line blocks, no game-side
// translation trigger yet (the testsuite's forced mode is the only caller).
// NOTE (diagnostic counters): emitted blocks do not bump perf_m68k_insns /
// OPHIST — instruction counts drift low in instrumented runs with blocks
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

void __m64k_dynrec_init(void)
{
    memset(__m64k_dyn_table, 0xFF, sizeof(__m64k_dyn_table));
    dyn_arena_used = 0;
}

// ---- MIPS encoders -------------------------------------------------------
#define R_ZERO 0
#define R_A0   4   // ctx
#define R_A1   5   // m_cycles (LIVE)
#define R_T0   8   // canonical MMIO load target
#define R_T5   13  // m_pc
#define R_S6   22  // flag_nv
#define R_S7   23  // flag_zc

#define ITYPE(op,rs,rt,imm) (((uint32_t)(op)<<26)|((rs)<<21)|((rt)<<16)|((uint16_t)(int16_t)(imm)))
#define ADDIU(rt,rs,imm)  ITYPE(0x09,rs,rt,imm)
#define DADDIU(rt,rs,imm) ITYPE(0x19,rs,rt,imm)
#define SLTI(rt,rs,imm)   ITYPE(0x0A,rs,rt,imm)
#define ORI(rt,rs,imm)    ITYPE(0x0D,rs,rt,imm)
#define BEQ(rs,rt,off)    ITYPE(0x04,rs,rt,off)
#define SW(rt,base,off)   ITYPE(0x2B,base,rt,off)
#define JABS(addr)        ((2u<<26)|((((uint32_t)(uintptr_t)(addr))>>2)&0x3FFFFFF))
#define NOP               0u

// Guest code fetch (through the TLB window, same memory the core executes).
static inline uint16_t fetch16(uint32_t a)
{
    return *(uint16_t *)(uintptr_t)(((a) & 0x00FFFFFF) + M64K_CONFIG_MEMORY_BASE);
}

// ---- Templates -----------------------------------------------------------
// Emit one instruction if the form is supported; returns its cycle charge
// (>0) with *len words appended to buf, or 0 if the form is untranslatable.
static int emit_insn(uint16_t op, uint32_t *buf, int *len)
{
    // MOVEQ #imm,Dn: 0111 rrr 0 iiiiiiii (op_moveq: charge 4;
    // flag_nv = sign-extended byte, flag_zc = zero-extended byte,
    // Dn = sign-extended long; X unchanged; cannot fault).
    if ((op & 0xF100) == 0x7000) {
        int reg = (op >> 9) & 7;
        int imm = (int8_t)(op & 0xFF);
        buf[(*len)++] = ADDIU(R_A1, R_A1, -4);
        buf[(*len)++] = DADDIU(R_S6, R_ZERO, imm);
        buf[(*len)++] = ORI(R_S7, R_ZERO, op & 0xFF);
        buf[(*len)++] = SW(R_S6, R_A0, M64K_OFF_DREGS + 4 * reg);
        return 4;
    }
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

    uint32_t body[64];
    int len = 0, ninsns = 0, cmax = 0;
    while (ninsns < max_insns && len < 48) {
        int save = len;
        int c = emit_insn(fetch16(pc + 2 * ninsns), body, &len);
        if (c == 0) {
            len = save;
            break;
        }
        cmax += c;
        ninsns++;
    }
    if (ninsns == 0)
        return 0;

    // Block frame: [C_max gate] body [tail].
    uint32_t block[64 + 8];
    int n = 0;
    if (!force) {
        block[n++] = SLTI(R_T0, R_A1, cmax + 1);
        block[n++] = BEQ(R_T0, R_ZERO, 3);      // gate passes -> skip bail
        block[n++] = NOP;
        block[n++] = JABS(main_loop);           // bail: interpreter runs it
        block[n++] = NOP;
    }
    memcpy(block + n, body, len * 4);
    n += len;
    block[n++] = ADDIU(R_T5, R_T5, 2 * ninsns); // m_pc -> next insn
    block[n++] = JABS(main_loop);
    block[n++] = NOP;

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
