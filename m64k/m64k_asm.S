#include <regdef.h>
#include "m64k_internal.h"
#include "m64k_config.h"

#define SR_SUPERV  0x2000
#define SR_MASK    0xA71F
#define CCR_MASK   0x1F

# The inline fast paths replicate the generic paths' cycle charges exactly at
# TIMING_ACCURACY<=0. The >=1 penalty fixups (cyc_move_penalty, the .l
# reg-direct quirks, etc.) are NOT replicated, so accuracy>=1 builds compile
# the fast paths out and use the generic implementations everywhere.
#if defined(M64K_FASTPATHS) && (M64K_CONFIG_TIMING_ACCURACY >= 1)
#undef M64K_FASTPATHS
#endif

# When set to 1, most MIPS registers will be set to random values at start.
# This is useful when running the testsuite, to verify that the code does not
# depend on the initial values of the registers.
#define RANDOM_INITIAL_VALUES  0

    .set noreorder

.macro check_addr_error reg, label
    #if M64K_CONFIG_ADDRERR
    .set noat
    andi $1, \reg, 1
    bnez $1, \label
    .set at
    #endif
.endm

.macro check_supervisor
    #if M64K_CONFIG_PRIVERR
    jal check_supervisor
    #endif
.endm

.macro breakpoint
    tne ra,ra
.endm

     .section .sdata
#ifndef M64K_SDATA_SETOFS
#define M64K_SDATA_SETOFS 0x8C0
#endif
# Pin this object's .sdata (context + dispatch tables, per-instruction hot)
# to fixed dcache sets: 8KB-aligned, then offset to set 140. Otherwise it
# moves with every .data change before it (e.g. PERFOSD vs release builds
# put it on different sets). Set 140..194 overlaps only the tail of the Z80
# work RAM in z80_hot, which z80_anchor.c pins at 0x1CF0; this pairing
# measured fast (p24a). Costs < 10KB of .sdata padding.
    .balign 8192
    .space M64K_SDATA_SETOFS
#ifdef M64K_SDATA_PAD
# Layout steering (2026-08-07): the per-insn PERFCOUNT counter bump and the
# per-insn m64k ctx accesses must not share a dcache set (8KB direct-mapped;
# set=(addr>>4)&511). A collision (measured: counters at ctx+7*8KB exactly)
# ping-pongs one set EVERY dispatched insn — ~2x m68k time, corrupting every
# fps measurement in that build. Verify disjointness with nm-perfsyms.sh
# after ANY build used for fps numbers; steer with -DM64K_SDATA_PAD=<bytes>.
    .space M64K_SDATA_PAD
#endif
# The CPU context (m64k_t; emu.c/testsuite.c only declare it). It is read
# and written on every dispatched instruction, like the dispatch tables in
# this object's .sdata (optable, ea_table, rmw_table, opcctable, ...). As a
# C .bss global it sat at a fixed address while .sdata moves with every
# .rodata/.data edit, and whenever the two landed on the same dcache sets
# the 68k ran ~40% slower (2026-10-04: M 7.9 -> 11.1 ms/frame in ares from
# an unrelated RSP ucode change). Here, all of it is one contiguous block
# well under 8KB, so it can never collide with itself.
    .globl m64k
    .balign 16
m64k:         .space M64K_CTX_SIZE
# .globl: dynrec.c bakes &pc_diff into absolute-target enders (JMP/JSR
# (xxx).l recompute it; delta exits preserve it).
    .globl pc_diff
pc_diff:      .long 0
# jmp_exec idle-skip prefilter: bit (31-p) is set for every 2KB page
# p = (pc>>11)&31 that holds an idle-loop PC of the compare chain there
# (0x142C,0x1FE2 -> 2,3; 0xFB4A/0xFB74/0xFC02 -> 31; 0xC18714 -> 16).
# Keep in sync with that chain. A .data word, not an immediate, so the
# all-ones OFF twin (every target runs the chain) is layout-identical.
    .globl m64k_idle_pagemask
m64k_idle_pagemask:
#ifdef MVS64_IDLEFILTER_OFF
    .long 0xFFFFFFFF
#else
    .long 0x30008001
#endif
# movew_imm_d16 fast path switch (MOVE.w #imm,(d16,An)); 0 = generic path.
# A .data word so the -DMVS64_IMMD16_OFF twin is layout-identical.
    .globl m64k_fp_imm_d16
m64k_fp_imm_d16:
#ifdef MVS64_IMMD16_OFF
    .long 0
#else
    .long 1
#endif
#ifdef M64K_PORTSTORE
# port_store_check base: the mapped (sign-extended by lw) LSPC port address.
# OFF twin = 1: mapped pointers are even and >= 0xFF000000, so nothing
# matches and every port store traps as before (layout-identical).
    .globl m64k_port_base
m64k_port_base:
#ifdef MVS64_PORTSTORE_OFF
    .long 1
#else
    .long 0xFF3C0000
#endif
# MOVE.l to the address port (0x3C0000) = address write + data write, the
# VRAM upload loops at 0xCEF2/0xCF34 (~90% of the port stores still
# trapping: two traps per instruction). Own word for a layout-identical
# OFF twin (-DMVS64_PORTL_OFF).
    .globl m64k_portl_base
m64k_portl_base:
#if defined(MVS64_PORTSTORE_OFF) || defined(MVS64_PORTL_OFF)
    .long 1
#else
    .long 0xFF3C0000
#endif
#endif
#ifdef MVS64_PERFCOUNT
# Per-frame diagnostic counters (read+reset by emu.c's [PERF] print):
# executed 68k instructions and idle-skip fires. Diagnostic builds only —
# the dispatch loop increment costs ~3 insns/instruction.
    .globl perf_m68k_insns
perf_m68k_insns: .long 0
    .globl perf_idle_skips
perf_idle_skips: .long 0
#endif
#ifdef MVS64_OPHIST
# Exact per-opcode execution histogram (65536 u32 slots, table lives in
# emu.c). Diagnostic builds only: the dispatch bump costs ~6 insns plus a
# dcache touch per executed instruction, so absolute timings are skewed —
# use the counts, not the fps, from an OPHIST run.
    .globl m64k_ophist_ptr
m64k_ophist_ptr: .long m64k_ophist_tab
#endif
#ifdef M64K_BLOCKOPS
# Runtime gate for the fused copy/fill path. Twin builds differ ONLY by this
# initializer, so ON-vs-OFF is a layout-identical A/B (the ±10-15fps
# cache-set lottery makes any other cross-binary fps comparison worthless).
    .globl mvs64_blockop_enable
#ifdef MVS64_BLOCKOP_DISABLE
mvs64_blockop_enable: .long 0
#else
mvs64_blockop_enable: .long 1
#endif
# Separate gate for the (An)+ -> (Am)+ long-copy fusion so its own fps
# increment can be twinned independently of the established fusions.
    .globl mvs64_blockop_copyl_enable
#ifdef MVS64_COPYL_DISABLE
mvs64_blockop_copyl_enable: .long 0
#else
mvs64_blockop_copyl_enable: .long 1
#endif
# Gate for the "move.w Dn,(An)" VRAM-port constant fill fusion.
    .globl mvs64_blockop_fwp_enable
#ifdef MVS64_FWP_DISABLE
mvs64_blockop_fwp_enable: .long 0
#else
mvs64_blockop_fwp_enable: .long 1
#endif
#endif
#if defined(M64K_BLOCKOPS) && (defined(M64K_DYNSTAT) || defined(MVS64_BOSTAT))
# Fused-blockop accounting ([BOSTAT]); diagnostic builds only.
    .globl blockop_fire
blockop_fire:     .long 0
    .globl blockop_rej_dst
blockop_rej_dst:  .long 0
    .globl blockop_rej_bud
blockop_rej_bud:  .long 0
    .globl blockop_rej_span
blockop_rej_span: .long 0
    .globl blockop_rej_dstval
blockop_rej_dstval: .long 0
    .globl blockop_rej_shape
blockop_rej_shape: .long 0
    .globl blockop_seen
blockop_seen:     .long 0
    .globl blockop_t3val
blockop_t3val:    .long 0
    .globl blockop_t9val
blockop_t9val:    .long 0
    .globl blockop_rej_pc
blockop_rej_pc:   .long 0
#endif
#ifdef MVS64_IDLEPROBE
# Idle-spin probe: detects a branch/jump target hit many times CONSECUTIVELY
# (a true wait-for-IRQ spin) vs a bounded backward loop (memcpy/DBRA). The PC
# that spins past the threshold is recorded in idle_probe_found for the C side
# to print. Gated by -DMVS64_IDLEPROBE; zero cost in normal builds.
idle_probe_last:  .long 0
idle_probe_cnt:   .long 0
     .globl idle_probe_found
idle_probe_found: .long 0
#endif

optable:
    .long   op_tbl0,  op_moveb, op_movel, op_movew, op_tbl4,  op_addq,  op_bcc,   op_moveq
    .long   op_or,    op_sub,   op_unk,   op_cmp,   op_and,   op_add,   op_shifts,op_unk
optable0:
    .long   op_ori,   op_bitsr, op_andi,  op_bitsr, op_subi,  op_bitsr, op_addi,  op_bitsr
    .long   op_bitsi, op_bitsr, op_eori,  op_bitsr, op_cmpi,  op_bitsr, op_unk,   op_bitsr
optable4:
    .long   op_negx,  op_lea,   op_clr,   op_lea,   op_neg,   op_lea,   op_not,   op_lea
    .long   op_pea,   op_lea,   op_tst,   op_lea, op_movem_mr,op_lea,   op_tbl4e, op_lea
optable4e:
    .long   op_unk,    op_unk,    op_unk,    op_unk,    op_trap,   op_link,   op_mvusp,  op_tbl4e7
    .long   op_jsr,    op_jsr,    op_jsr,    op_jsr,    op_jmp,    op_jmp,    op_jmp,    op_jmp
optable4e7:
#ifdef M64K_FASTPATHS
    .long   op_reset,  op_nop,    op_stop,   op_rte,    op_rtd,    rts_fast,  op_trapv,  op_rtr
#else
    .long   op_reset,  op_nop,    op_stop,   op_rte,    op_rtd,    op_rts,    op_trapv,  op_rtr
#endif
    .long   op_unk,    op_unk,    op_movec,  op_movec,  op_unk,    op_unk,    op_unk,    op_unk

opshifts:
    .long   op_asli,  op_lsli,  op_roxli,  op_roli
    .long   op_asl,   op_lsl,   op_roxl,   op_rol
opshiftsmem:
    .long   op_aslm,  op_asrm,  op_lslm,  op_lsrm,  op_roxlm, op_roxrm, op_rolm,  op_rorm
    .long   op_bftst, op_bfextu,op_bfchg, op_bfexts,op_bfclr, op_bfffo, op_bfset, op_bfins

    .text

#define ALIGN(lbl)  .balign 32; lbl

#define OP(x)       .balign 32; op_##x
#define ctx          a0    // pointer to the CPU context (m64k_t*)
#define m_cycles     a1    // cycles left to execute (NOTE: must be kept caller-saved register)
#define mmap_mask    a2    // mask used by the map_m68k macro
#if M64K_CONFIG_TIMING_ACCURACY >= 0
#define rmw_cyctable a3    // RMW: pointer to the current cycletable, or small positive value for NULL
#endif

#define lax_base    v0     // base pointer used by lax macro
#define zx64_mask   v1     // mask used by the zx64 macro

#define opcode      s0     // current opcode being executed
#define eaptr       s1     // effective address pointer (in N64 space)
#define dptr        s2     // pointer to the other register in the opcode (in N64 space)
#ifdef M64K_DYNREC
#define dyn_tab     s3     // base of the dynarec block table (__m64k_dyn_table)
#endif
#define opsize      s4     // size of the current instruction (1=byte, 2=word, 4=long, 0=not decoded yet)
#define __________  s5     // unused (NOTE: this is used within movem)
#define flag_nv     s6     // bit 31: N; bit 63: N^V
#define flag_zc     s7     // bit 0..31: !Z; bit 32: C
#define flag_x      s8     // bit 0: X

#define eaptr32       t4   // 32-bit effective address pointer (in m68k space)
#define m_pc          t5   // pointer to the next instruction to fetch (in N64 space)
#define result        t6   // RMW: result of the current opcode
#define rmw_op        t7   // RMW: pointer to the implementation function
#define rmw_bitsize   t8   // RMW: bitsize value useful in flag calcuation (24, 16, 0)
#define rmw_srmask    t8   // for SR RMWs, it contains the mask for SR bits to be written

# Calculate an address of a member field of the CPU context
.macro la_ctx  reg, offset,base
    .ifnb \base
        .set noat
        add \reg, ctx, \base
        addi \reg, \offset
        .set at
    .else
        add \reg, ctx, \offset
    .endif
.endm

# Load the value of a member field of the CPU context
.macro lw_ctx  reg, offset,base
    .ifnb \base
        .set noat
        add $1, ctx, \base
        lw \reg, \offset($1)
        .set at
    .else
        lw \reg, \offset(ctx)
    .endif
.endm

# Store the value of a member field of the CPU context
.macro sw_ctx  reg, offset,base
    .ifnb \base
        .set noat
        add $1, ctx, \base
        sw \reg, \offset($1)
        .set at
    .else
        sw \reg, \offset(ctx)
    .endif
.endm

# Consume the specified number of cycles (if timing accuracy is enabled)
.macro use_cycles cycles
    #if M64K_CONFIG_TIMING_ACCURACY >= 0
    addi m_cycles, -\cycles
    #endif
.endm

# Consume the specified number of cycles if opcode & mask == value
# This is only used when M64K_CONFIG_TIMING_ACCURACY==1, as it is used to
# handle cycle counting exceptions for specific variants of the current opcode
.macro use_cycles_opmask mask, value, cycles
    #if M64K_CONFIG_TIMING_ACCURACY >= 1
    .set noat
    .set noreorder
    andi $1, opcode, \mask
    .if \value != 0
        xori $1, \value
    .endif
    beqzl $1, 1f
    addi m_cycles, -\cycles
1:  .set at
    .set reorder
    #endif
.endm

# Configure the current RMW cycle table (if timing accuracy is enabled)
.macro set_rmw_cyctable table
    #if M64K_CONFIG_TIMING_ACCURACY >= 0
    la rmw_cyctable, \table
    #endif
.endm

# Decrement the cycle counter using the value fetched from the current cycletable
# at the specified offset.
.macro use_cycles_table offset
    #if M64K_CONFIG_TIMING_ACCURACY >= 0
    bgez rmw_cyctable, 1f                     # If cycletable is not "zero-ish" (not 0x80nn_nnnn)
    lbu rmw_cyctable, \offset(rmw_cyctable)   #  fetch it
    sub m_cycles, rmw_cyctable                #  fecrement cycle counter
1:  # NOTE: the fetched number will be left as rmw_cyctable
    # and will be treated as "zero-ish" next time we run this macro.
    # This saves one instruction as we don't need to set rmw_cyctable to zero
    #endif
.endm

#define LAX_BASE_ADDR main_loop

# "lax" macro. Load the address of a label into a register. This is similar to
# "la", but it expands to a single opcode as we dedicate one register to hold
# a base address, so all labels in the m64k code can be expressed as a 16-bit
# offset from that base address.
.macro lax reg, label
    addiu \reg, lax_base, \label - LAX_BASE_ADDR
.endm

# "zx64" macro. Zero-extend a 32-bit value to 64-bit.
.macro zx64 dst, src
    .ifnb \src
    and \dst, \src, zx64_mask
    .else
    and \dst, zx64_mask
    .endif
.endm

# "not32sx" macro. Like "not", but only complements the lower
# 32-bits of the register (and then sign extends).
.macro not32sx dst, src
    .ifnb \src
    subu \dst, zx64_mask, \src
    .else
    subu \dst, zx64_mask, \dst
    .endif
.endm

# "not32" macro. Like "not", but only complements the lower
# 32-bits of the register (and keep the upper half unchanged).
.macro not32 dst, src
    .ifnb \src
    xor \dst, zx64_mask, \src
    .else
    xor \dst, zx64_mask, \dst
    .endif
.endm


# "jal_and_j" macro. Compact version of "jal" followed by "j", with a tail-call
# for optimal icache usage.
.macro jal_and_j func, label
    lax ra, \label
    j \func
.endm

# "jalr_and_j" macro. Similar to jal_and_j, but uses jalr instead.
.macro jalr_and_j reg, label
    lax ra, \label
    jr \reg
.endm

# "map_m68k" macro. Maps a 32-bit M68K address to a 32-bit MIPS address.
# This macro implements different codepaths depending on M64K_CONFIG_MEMORY_BASE.
# A fixed register mmap_mask is used to implement the mapping more compactly,
# and it must be initialized before using this macro.
.macro map_m68k dst, src
    #if M64K_CONFIG_MEMORY_BASE == 0x00000000
        .ifnb \src
        and \dst, \src, mmap_mask
        .else
        and \dst, mmap_mask
        .endif
    #elif M64K_CONFIG_MEMORY_BASE == 0xFF000000
        .ifnb \src
        or \dst, \src, mmap_mask
        .else
        or \dst, mmap_mask
        .endif
    #else
        .ifnb \src
        sll \dst, \src, 8
        .else
        sll \dst, 8
        .endif
        srl \reg, 8
        add \reg, mmap_mask
    #endif
.endm

# Macros to store to the M68K memory space (after mapping).
# These macros are meant to explicitly mark m68k memory space writes,
# and to enforce the convention that all writes to the M68K memory space
# must have the value to be stored in the "result" register.
# This simplifies the implementation of MMIO handlers.
.macro sw_m68k reg, address
    .if (\reg != result)
        .error "sw_m68k: register used to store into m68k memory must be 'result'"
    .endif
    swl \reg, 0(\address)
    swr \reg, 3(\address)
.endm

.macro sh_m68k reg, address
    .if (\reg != result)
        .error "sh_m68k: register used to store into m68k memory must be 'result'"
    .endif
    sh result, 0(\address)
.endm

# "port_store_check" (game builds, M64K_PORTSTORE): placed right before the
# sh_m68k of the hottest MOVE.w-to-memory fast paths, after their cycle
# charge and flag update. A mapped address equal to the LSPC VRAM address /
# data / modulo port (0x3C0000/2/4) branches to m64k_port_store, which
# performs the hw_n64.S asm_lspc_vram_*_w update inline instead of taking
# the TLB trap (~100 cycles each, ~1k per fight frame). Every other address,
# the 0x3C0006+ registers and the mirrors included, stores (and traps) as
# before. t1/t2 are dead at every use site. m64k_port_base: see .sdata.
.macro port_store_check
#ifdef M64K_PORTSTORE
    lw t1, %gprel(m64k_port_base)(gp)
    xor t1, eaptr, t1
    sltiu t2, t1, 6
    bnez t2, m64k_port_store
#endif
.endm

.macro sb_m68k reg, address
    .if (\reg != result)
        .error "sb_m68k: register used to store into m68k memory must be 'result'"
    .endif
    sb \reg, 0(\address)
.endm

# Macros to load from the M68K memory space (after mapping).
.macro lw_m68k reg, address
    .if (\reg != t0)
        .error "lw_m68k: register used to store into m68k memory must be 't0'"
    .endif
    lwl \reg, 0(\address)
    lwr \reg, 3(\address)
.endm

.macro lhu_m68k reg, address
    .if (\reg != t0)
        .error "lhu_m68k: register used to store into m68k memory must be 't0'"
    .endif
    lhu \reg, 0(\address)
.endm

.macro lh_m68k reg, address
    .if (\reg != t0)
        .error "lh_m68k: register used to store into m68k memory must be 't0'"
    .endif
    lh \reg, 0(\address)
.endm

.macro lbu_m68k reg, address
    .if (\reg != t0)
        .error "lhu_m68k: register used to store into m68k memory must be 't0'"
    .endif
    lbu \reg, 0(\address)
.endm


# "dispatch_body" macro: the real main_loop dispatch. It is also expanded at
# the tail of every M64K_FASTPATHS handler (as "dispatch_next") to skip the
# "j main_loop" hop. Any change here IS a change to main_loop.
.macro dispatch_body
    .set push
    .set noreorder
    blez m_cycles, main_loop_exit
     lhu opcode, 0(m_pc)
#ifdef MVS64_PERFCOUNT
    lw t1, perf_m68k_insns
    addiu t1, 1
    sw t1, perf_m68k_insns
#endif
#ifdef MVS64_OPHIST
    lw t1, m64k_ophist_ptr
    sll t2, opcode, 2
    addu t1, t2
    lw t2, 0(t1)
    addiu t2, 1
    sw t2, 0(t1)
#endif
    #if M64K_CONFIG_ADDRERR
    sw opcode, M64K_OFF_IR(ctx)
    #endif
    #if M64K_CONFIG_TIMING_ACCURACY < 0
    addi m_cycles, M64K_CONFIG_TIMING_ACCURACY
    #endif
    srl t1, opcode, 12
    sll t1, 2
    lw t0, optable(t1)
    jr t0
     addi m_pc, 2
    .set pop
.endm

# "dispatch_next": fast-path handler tails. See dispatch_body.
.macro dispatch_next
    dispatch_body
.endm

# "check_addr_bail" macro. Used by the M64K_FASTPATHS handlers on ADDRERR
# builds: an odd word address bails to the opcode's GENERIC implementation
# (label) with no architectural state mutated, so the generic path re-executes
# the instruction from scratch and raises the address error through its normal
# machinery with bit-exact state. Compiles to nothing on non-ADDRERR builds.
.macro check_addr_bail reg, label
    #if M64K_CONFIG_ADDRERR
    .set noat
    andi $1, \reg, 1
    bnez $1, \label
    .set at
    #endif
.endm

    .globl _m64k_asmrun
_m64k_asmrun:
    addiu sp,sp,-128
    sd s0,32(sp)
    sd s1,40(sp)
    sd s2,48(sp)
    sd s3,56(sp)
    sd s4,64(sp)
    sd s5,72(sp)
    sd s6,80(sp)
    sd s7,88(sp)
    sd s8,96(sp)
    sd ra,104(sp)

    #if M64K_CONFIG_MEMORY_BASE == 0x00000000
    li mmap_mask, 0x00FFFFFF
    #else
    li mmap_mask, M64K_CONFIG_MEMORY_BASE
    #endif
    la lax_base, LAX_BASE_ADDR
#ifdef M64K_DYNREC
    la dyn_tab, __m64k_dyn_table
#endif
    dli zx64_mask, 0x00000000FFFFFFFF
    lw t1, M64K_OFF_PC(ctx)
    lw t0, M64K_OFF_SR(ctx)

    # Convert PC to the N64 address space.
    map_m68k m_pc, t1
    sub t1, m_pc
    sw t1, pc_diff                    # Save difference between m_pc and PC

    # Initialize dptr to a valid pointer, for ops that don't set it
    # and then ignore the read value.
    move dptr, ctx

    jal reload_sr
    nop

    # Entry-time IRQ check. The per-instruction check_interrupts poll is gone
    # from main_loop: mid-slice IRQ/run_stop requests now force the timeslice
    # to end (see slice_break / forced_remaining in the TLB handler and the
    # reload_sr clamp), and the pending IRQ lines are re-examined here on the
    # next entry. This keeps IRQ latency at the same instruction boundary as
    # the old poll while removing 3 instructions from the dispatch hot path.
    sb zero, M64K_OFF_SLICE_BREAK(ctx)
    sb zero, M64K_OFF_CHECK_INTERRUPTS(ctx)
    lbu t0, M64K_OFF_NMI_PENDING(ctx)
    li t1, 7
    bnez t0, entry_raise_irq
    lbu t0, M64K_OFF_SR+2(ctx)
    lbu t1, M64K_OFF_IPL(ctx)
    andi t0, 7
    bgt t1, t0, entry_raise_irq

    sw m_cycles, M64K_OFF_TS_START(ctx)
    sw m_cycles, M64K_OFF_TS_CUR(ctx)
    #if M64K_CONFIG_TIMING_ACCURACY >= 0
    li rmw_cyctable, 0
    #endif

#ifdef M64K_DYNREC
    # Dynarec block-table probe, slice-entry site (phase 1: the table is
    # empty — tags are all 0xFFFFFFFF and guest PCs are 24-bit, so the hit
    # path is unreachable; this measures the shipping lookup cost and
    # reserves the site). Set = ((pc >> 1) & (SETS-1)) * 16 bytes.
    .set push
    .set noreorder
    lw t1, M64K_OFF_PC(ctx)
    srl t2, t1, 1
    andi t2, (M64K_DYN_TABLE_SETS-1)
    sll t2, 4
    addu t2, dyn_tab
    lw t3, 0(t2)                       # way-0 tag
    beq t3, t1, dyn_probe_hit
     lw t3, 8(t2)                      # way-1 tag
    beq t3, t1, dyn_probe_hit1
     nop
    .set pop
#endif

    #if RANDOM_INITIAL_VALUES
    #define C0_COUNT	$9
    # Corrupt registers to trigger bugs
    li cycles_long, 0x1234
    li opsize, 0x1234
    li eaptr, 0x12345
    li eaptr32, 0x12345
    li result, 0x12345678
    li rmw_bitsize, 0x11
    dli t0, 0x8000000080000000
    mfc0 t1, C0_COUNT
    dsll t1, 32
    mfc0 t2, C0_COUNT
    or t1, t2
    and flag_nv, t0
    not t0
    and t1, t0
    or flag_nv, t0
    mfc0 t1, C0_COUNT
    andi t0, t1, 1
    xor t1, t0
    or flag_x, t1
    #endif

#ifdef M64K_DYNREC
    .globl main_loop
#endif
ALIGN(main_loop):
    dispatch_body
op_tbl0:
    srl t0, opcode, 6
    andi t0, 0xF << 2
    lw t0, optable0(t0)
    jr t0
    li opsize, 0
OP(tbl4):
    srl t0, opcode, 6
    andi t0, 0xF << 2
    lw t0, optable4(t0)
    jr t0
    li opsize, 0
op_tbl4e:
    srl t0, opcode, 2
    andi t0, 0xF << 2
    lw t0, optable4e(t0)
    jr t0
    nop
op_tbl4e7:
    sll t0, opcode, 2
    andi t0, 0xF << 2
    lw t0, optable4e7(t0)
    jr t0
    nop
OP(shifts):
    andi t0, opcode, 0x3<<6
    beq t0, 0x3<<6, 1f
    srl t0, opcode, 1
    andi t0, 0x7<<2
    lw t0, opshifts(t0)
    jr t0
    li opsize, 0
1:  srl t0, opcode, 6
    andi t0, 0xF<<2
    lw t0, opshiftsmem(t0)
    jr t0
    li opsize, 2

entry_raise_irq:
    # Raise an IRQ (level in t1). Reached only from the entry-time check.
    li t0, M64K_PENDINGEXC_IRQ
    sw_ctx t0, M64K_OFF_PENDINGEXC+0
    j main_loop_exit
    sw_ctx t1, M64K_OFF_PENDINGEXC+4


main_loop_exit:
    lw t0, pc_diff
    addu m_pc, t0
    sw m_pc, M64K_OFF_PC(ctx)
    #if !M64K_CONFIG_ADDRERR
    sw opcode, M64K_OFF_IR(ctx)
    #endif

    sw zero, M64K_OFF_TS_START(ctx)

    jal flush_sr
    nop

    ld s0,32(sp)
    ld s1,40(sp)
    ld s2,48(sp)
    ld s3,56(sp)
    ld s4,64(sp)
    ld s5,72(sp)
    ld s6,80(sp)
    ld s7,88(sp)
    ld s8,96(sp)
    ld ra,104(sp)
    addiu sp,sp,128
    jr ra
    move v0, m_cycles

    # Store flags into SR (preserving other SR bits)
    # Also store the current stack pointer (A7) into USP/SSP
    # Destroy: t0, t1, t2
    # Return: t1 = SR
flush_sr:
    dsrl t0, flag_nv, 63    # N^V
    srl t1, flag_nv, 31     # N
    xor t0, t1              # V
    sll t0, 1
    sll t1, 3
    or t0, t1

    dsrl t1, flag_zc, 32     # C
    andi t1, 1
    or t0, t1
    dsll t1, flag_zc, 32
    seq t1, zero             # Z
    sll t1, 2
    or t0, t1

    andi flag_x, 1
    sll t1, flag_x, 4
    or t0, t1

    lw t1, M64K_OFF_SR(ctx)
    and t1, ~CCR_MASK
    or t1, t0
    sw t1, M64K_OFF_SR(ctx)

flush_sr_chstack:
    srl t0, t1, 11                        # Isolate supervisor bit
    andi t0, 4
    lw_ctx t2, M64K_OFF_AREGS+7*4        # Copy A7 to USP/SSP
    sw_ctx t2, M64K_OFF_USP,t0

    jr ra   # return SR in t1
    nop

    # Store the current stack pointer (A7) into USP/SSP
    # Return: t1 = SR
flush_stack:
    j flush_sr_chstack
    lw t1, M64K_OFF_SR(ctx)


    # Reload flag registers from SR bits.
    # Also load the current stack pointer (A7) from USP/SSP
    # Input: t0: SR
    # Destroy: t0, t1, t2
reload_sr:
    # Extract flags from SR into flag_nv / flag_zc
    # SR Format: ---X NZVC
    srl flag_x, t0, 4          # X to bit 0
    andi flag_x, 1

    sll t1, t0, 28         # N to bit 31 (and 63)
    dsll flag_nv, t0, 62   # V to bit 63 (and 0 to 31)
    xor flag_nv, t1

    andi t1, t0, 4         # Isolate Z
    xori t1, 4             # !Z
    dsll flag_zc, t0, 32   # C to bit 32
    or flag_zc, t1

reload_sr_chstack:
    srl t1, t0, 11                    # Isolate supervisor bit
    andi t1, 4
    lw_ctx t2, M64K_OFF_USP,t1       # Copy USP/SSP to A7
    sw_ctx t2, M64K_OFF_AREGS+7*4

    # If the (new) SR leaves a pending IRQ unmasked (or an NMI is pending),
    # force the timeslice to end: bank the live cycle counter into
    # forced_remaining and zero it. The current instruction still completes
    # (so e.g. RTE/STOP finish updating PC/SR first), then main_loop exits
    # and the re-entry check raises the IRQ at the exact same instruction
    # boundary the old per-instruction poll would have.
    lbu t1, M64K_OFF_NMI_PENDING(ctx)
    lbu t2, M64K_OFF_IPL(ctx)
    bnez t1, reload_sr_clamp
    srl t0, 8
    andi t0, 7
    ble t2, t0, 1f
reload_sr_clamp:
    lw t1, M64K_OFF_FORCED_REMAINING(ctx)
    addu t1, m_cycles
    sw t1, M64K_OFF_FORCED_REMAINING(ctx)
    sw m_cycles, M64K_OFF_TS_CUR(ctx)
    li m_cycles, 0
1:  jr ra
    nop

reload_stack:
    j reload_sr_chstack
    lw t0, M64K_OFF_SR(ctx)


###############################################################
# Address error handling
###############################################################
#
# Address errors require quite some code to exactly match the
# state of the processor at the time they are generated. In fact,
# they happen mid-instructions, but the interpreter favors speed
# and code size and doesn't bother to generate them with a consistent
# state. An example is moves to a post-increment address: if the
# write happens to cause an address error, the address register is
# supposedely not incremented yet, but in our interpreter we raise
# the address error after the increment has happened.
#
# So in general, we need to perform several fixups here, including
# instruction-specific ones. We don't care if this is slow as
# address errors are *extremely* rare in 68000 code, so it is a super
# cold path.

#if M64K_CONFIG_ADDRERR
    .data
    # Table to help setting up address error. Each entry is the PC offset to rollback.
addrerr_pcoff:  .byte  0,  0, -2, -2, -2, -2, -2,  0
addrerr7_pcoff: .byte -2, -2, -2, -2,  0,  0,  0,  0
    # Table to rollback EA cycle count
addrerr_cycoff: .byte  0,  0,  4,  4,  6,  8, 10,  0
                .byte  8, 12,  8, 10,  4,  0,  0,  0

#define FC_DATA     1         // Error while accessing data
#define FC_PROGRAM  2         // Error while fetching code
#define FC_SUPERV   4         // Error while in supervisor mode
#define FC_NONOP    8         // Error happened outside of opcode execution
#define FC_READ     16        // Error while reading from the bus

    .text
    .set reorder
pc_address_error:
    li t2, FC_READ | FC_PROGRAM | FC_NONOP
    j address_error
read_address_error:
    li t2, FC_READ | FC_DATA
    j address_error
write_address_error:
    li t2, FC_DATA
address_error:
    li t0, M64K_PENDINGEXC_ADDRERR
    sw_ctx t0, M64K_OFF_PENDINGEXC+0
    sw_ctx eaptr32, M64K_OFF_PENDINGEXC+4
    use_cycles 50

    # Most opcodes that generate an address error have a standard EA
    # encoding in bits 0..6, and we handle those just below. Some of them,
    # though, don't have a standard EA, so we need to handle with separate
    # code.
    lw_ctx t1, M64K_OFF_IR       # Check original opcode
    andi t0, t1, 0xF000
    beq t0, 0x1000, addrerr_fixup_move # Fixup for MOVE (src/dst EA)
    beq t0, 0x2000, addrerr_fixup_move # Fixup for MOVE (src/dst EA)
    beq t0, 0x3000, addrerr_fixup_move # Fixup for MOVE (src/dst EA)
    beq t0, 0x6000, addrerr_fixup_bcc  # Fixup for BCC (which doesn't have EA)
    andi t0, t1, 0xF0F8
    beq t0, 0x50C8, addrerr_fixup_dbcc # Fixup for DBCC (which doesn't have EA)
    beq t1, 0x4e73, addrerr_fixup_rte  # Fixup for RTE (which doesn't have EA)
    beq t1, 0x4e75, addrerr_fixup_rts  # Fixup for RTS (which doesn't have EA)
    beq t1, 0x4e77, addrerr_fixup_rtr  # Fixup for RTR (which doesn't have EA)
    andi t0, t1, 0xF138
    beq t0, 0xB108, address_fixup_cmpm # Fixup for CMPM (two registers post-increment)

addrerr_fixup_standard_ea:
    # Compute PC offset from the opcode. Notice that we used the
    # "current" opcode that might have been modified by the handler to force
    # a specific EA mode; but this is exactly the one that we should be parsing here.
    # Fetch the PC table entry using EA
    srl t0, opcode, 3
    andi t0, 0x7
    bne t0, 7, addrerr_adjust_pc
adderr_off7:
    andi t0, opcode, 7
    addi t0, 8
addrerr_adjust_pc:
    lb t1, addrerr_pcoff(t0)             # PC offset
    add m_pc, t1                         # Rollback PC
    #if M64K_CONFIG_TIMING_ACCURACY >= 0
    lb t1, addrerr_cycoff(t0)            # Cycle offset
    add m_cycles, t1                     # Rollback cycle count
    bne opsize, 4, addrerr_finish        # If the opcode is 32-bit
    add m_cycles, 4                      # Remove another 4 cycles from EA
    #endif
addrerr_finish:
    lw_ctx opcode, M64K_OFF_IR          # Restore original opcode
    andi t0, opcode, 0xFFE0
    or t0, t2                            # Merge FC code (with read/write flag)
    lw_ctx t1, M64K_OFF_SR              # Check if we were running in supervisor mode
    andi t1, SR_SUPERV
    beqz t1, 1f
    ori t0, FC_SUPERV                    # Mark supervisor FC bit
1:  sw_ctx t0, M64K_OFF_PENDINGEXC+8    # Store FC

    # Check if we need fixups
    andi t0, opcode, 0xF138
    beq t0, 0xD108, addrerr_fixup_addq  # Fixup for ADDX pre-decrement
    beq t0, 0x9108, addrerr_fixup_addq  # Fixup for SUBX pre-decrement
    andi t0, opcode, 0xFFC0
    beq t0, 0x4EC0, addrerr_fixup_jmp   # Fixup for JMP
    beq t0, 0x4E80, addrerr_fixup_jsr   # Fixup for JSR
    andi t0, opcode, 0xFF80
    beq t0, 0x4880, addrerr_fixup_movem_rm   # Fixup for MOVEM reg->mem
    beq t0, 0x4C80, addrerr_fixup_movem_mr   # Fixup for MOVEM mem->reg
    j main_loop_exit

addrerr_fixup_addq:
    #define lhs eaptr
    #define rhs eaptr32

    li t1, 0               # compute address adjustment
    bne opsize, 4, 1f      # 16-bit: 0
    li t1, 2               # 32-bit: 2

    # Decode LHS operand (Ry)
1:  andi t0, opcode, 0x7
    sll t0, 2
    la_ctx lhs, M64K_OFF_AREGS,t0
    lw t0, 0(lhs)
    andi t2, t0, 1   # check if LHS is odd
    beqz t2, 1f      # jump to RHS odd

    add t0, t1       # adjust LHS to the expected error address
    sw t0, 0(lhs)
    sw_ctx t0, M64K_OFF_PENDINGEXC+4  # store LHS as address error
    addi m_pc, -2                      # adjust expected PC
    j main_loop_exit

1:  # rhs is odd and causing exception
    # Decode RHS operand (Ry)
    srl t0, opcode, 7
    andi t0, 0x7 << 2
    la_ctx rhs, M64K_OFF_AREGS,t0
    lw t0, 0(rhs)                      # read RHS
    add t0, t1                         # adjust RHS to the expected error address
    sw t0, 0(rhs)                      # write RHS
    sw_ctx t0, M64K_OFF_PENDINGEXC+4  # store RHS as address error
    j main_loop_exit
    #undef lhs
    #undef rhs

addrerr_fixup_move:
    andi t0, t2, 1<<4                  # Check if it's a read
    bnez t0, addrerr_fixup_standard_ea # If it's a read, do the standard EA fixup

    # This is a write exception. We must do fixup based *mainly* on the
    # destination EA, but there are some cases in which the source EA also
    # affects the stack frame.

    # Restore original opcode and prepare standard FC code.
    lw_ctx opcode, M64K_OFF_IR
    andi t0, opcode, 0xFFE0
    or t0, 0x5
    sw_ctx t0, M64K_OFF_PENDINGEXC+8    # Store FC

addrerr_fixup_move_dst:
    # Check destination EA
    andi t0, opcode, 0x7 << 6
    beq t0, 2<<6, addrerr_fixup_move_areg      # (An)
    beq t0, 3<<6, addrerr_fixup_move_postincr  # (An)+
    beq t0, 4<<6, addrerr_fixup_move_predecr   # -(An)
    beq t0, 5<<6, addrerr_fixup_move_disp16    # (d16,An)
    beq t0, 6<<6, addrerr_fixup_move_disp8     # (d8,An,Xn)

    andi t0, opcode, 0x3F << 6
    beq t0, 0x07<<6, addrerr_fixup_move_wordabs  # (xxx).W
    beq t0, 0x0F<<6, addrerr_fixup_move_longabs  # (xxx).L

1:  j main_loop_exit

    add m_pc, -2
    j main_loop_exit

addrerr_fixup_move_postincr: # (An)+
    srl t0, opcode, 7                  # Extract An register number
    andi t0, 0x7 << 2
    la_ctx t1, M64K_OFF_AREGS,t0      # Revert the post-increment
    lw t0, 0(t1)
    sub t0, opsize
    sw t0, 0(t1)
    addi m_pc, -2                      # Adjust PC
    j main_loop_exit

addrerr_fixup_move_predecr: # -(An)
    bne opsize, 4, main_loop_exit      # 16-bit: no fixup
    srl t0, opcode, 7                  # Extract An register number
    andi t0, 0x7 << 2
    la_ctx t1, M64K_OFF_AREGS,t0      # Revert half of the pre-decrement
    lw t0, 0(t1)
    add t0, 2
    sw t0, 0(t1)
    lw_ctx t0, M64K_OFF_PENDINGEXC+4  # Adjust also the exception address
    add t0, 2
    sw_ctx t0, M64K_OFF_PENDINGEXC+4
    j main_loop_exit

addrerr_fixup_move_areg:    # (An)
addrerr_fixup_move_disp8:   # (d8,An,Xn)
addrerr_fixup_move_disp16:  # (d16,An)
addrerr_fixup_move_wordabs: # (xxx).W
    addi m_pc, -2
    j main_loop_exit

addrerr_fixup_move_longabs: # (xxx).L
    addi m_pc, -4

    # Check source EA. It has an impact has the (xxx.L) decoding of dest
    # is partly overlap to EA source. We check for the source EA mode
    # that don't access memory.
    andi t0, opcode, 0x38
    beq t0, 0b000000, addrerr_fixup_move_longabs2    # Dn
    beq t0, 0b001000, addrerr_fixup_move_longabs2    # An
    andi t0, opcode, 0x3F
    beq t0, 0b111100, addrerr_fixup_move_longabs2    # #<data>
    j main_loop_exit
addrerr_fixup_move_longabs2:
    addi m_pc, 2
    j main_loop_exit

addrerr_fixup_dbcc:
addrerr_fixup_bcc:
addrerr_fixup_rte:
addrerr_fixup_rts:
addrerr_fixup_rtr:
    addi m_pc, -4                            # adjust current PC
    j addrerr_finish

addrerr_fixup_jsr:
    lw_ctx t0, M64K_OFF_AREGS+7*4     # revert SP-=4 performed by JSR
    addiu t0, 4
    sw_ctx t0, M64K_OFF_AREGS+7*4
addrerr_fixup_jmp:
    addiu m_pc, -2
    j main_loop_exit

addrerr_fixup_movem_rm:
    # We use a shared implementation between reg->mem and mem->reg, so
    # we always generate a read_address_error. Change to write when the
    # opcode is reg->mem.
    lw_ctx t0, M64K_OFF_PENDINGEXC+8
    xori t0, FC_READ
    sw_ctx t0, M64K_OFF_PENDINGEXC+8

    andi t0, opcode, 1<<6                # check if word transfer
    beqz t0, main_loop_exit              # if word, fixup is finished
    andi t0, opcode, 7<<3                # check if EA mode is pre-decremented
    bne t0, 4<<3, main_loop_exit         # otherwise, fixup is finished
    andi t0, opcode, 7
    sll t0, 2
    lw_ctx t1, M64K_OFF_PENDINGEXC+4
    addiu t1, 2
    sw_ctx t1, M64K_OFF_PENDINGEXC+4
    j main_loop_exit

addrerr_fixup_movem_mr:
    andi t0, opcode, 1<<6                # check if word transfer
    beqz t0, main_loop_exit              # if word, no fixup
    andi t0, opcode, 7<<3                # check if EA mode is post-increment
    bne t0, 3<<3, main_loop_exit         # otherwise, no fixup

    andi t0, opcode, 7
    sll t0, 2
    lw_ctx t1, M64K_OFF_AREGS,t0        # Revert the post-increment
    addiu t1, -2
    sw_ctx t1, M64K_OFF_AREGS,t0
    j main_loop_exit

address_fixup_cmpm:
    addiu m_pc, -2
    j addrerr_finish

    .set noreorder
#endif

###############################################################
# Privilege violation error handling
###############################################################
#if M64K_CONFIG_PRIVERR
check_supervisor:
    .set reorder
    .set noat
    lw_ctx $1, M64K_OFF_SR
    andi $1, SR_SUPERV
    bnez $1, 1f

    li $1, M64K_PENDINGEXC_PRIVERR
    sw_ctx $1, M64K_OFF_PENDINGEXC     # (was storing t0 by mistake)
    addi m_pc, -2                      # 68000 stacks PC of the privileged instruction
    j main_loop_exit
1:  jr ra
    .set at
    .set noreorder
#endif

###############################################################
# Division by zero exception
###############################################################
#if M64K_CONFIG_DIVBYZERO
    .section .sdata
exc_dbz_pcoff: .byte -2,-2,-2,-2,-2,-4,-4,-2
               .byte -4,-6,-2,-2,-2,-2,-2,-2
    .text

exception_divbyzero:
    .set reorder
    li t0, M64K_PENDINGEXC_DIVBYZERO   # set pending exception code to DIVBYZERO
    sw_ctx t0, M64K_OFF_PENDINGEXC

    srl t0, opcode, 3                  # extract EA mode
    andi t0, 7
    bne t0, 7, dbz_fetch_pcoff
    andi t0, opcode, 7                 # if EA mode is 7, use register as submode
    add t0, 8
dbz_fetch_pcoff:
    lb t0, exc_dbz_pcoff(t0)           # get PC offset correction
    addu m_pc, t0                      # apply it
    li flag_nv, 0                      # undefined in the manual, set N=V=0
    li flag_zc, 1                      # undefined in the manual, set Z=C=0
    j main_loop_exit
    .set noreorder
#endif

###############################################################
# Immediate computation
###############################################################

    .text
ALIGN(decode_imm):
    set_rmw_cyctable rmw_cyctable_immediate
decode_imm_nocyctable:
    srl t0, opcode, 6
    andi t0, 0x3
    beqz t0, imm_8
    addi t0, -1
    beqz t0, imm_16
imm_32:
    move dptr, m_pc
    addi m_pc, 4
    jr ra
    li opsize, 4
imm_16:
    addi dptr, m_pc, -2
    addi m_pc, 2
    jr ra
    li opsize, 2
imm_8:
    addi dptr, m_pc, -2
    addi m_pc, 2
    jr ra
    li opsize, 1

###############################################################
# EA computation
###############################################################

    .sdata
ea_table:
    .long   ea_000, ea_001, ea_010, ea_011, ea_100, ea_101, ea_110, ea_111
ea7_table:
    .long   ea7_000, ea7_001, ea7_010, ea7_011, ea7_100, ea7_101, ea7_110, ea7_111

.macro decode_opsize
    .set noat
    bnez opsize, 1f            # if opsize was not specified
    srl opsize, opcode, 6      # extract it from opcode bit 6-7
    andi opsize, 0x3
    li $1, 1
    sllv opsize, $1, opsize    # 0b00=1, 0b01=2, 0b10=4
    .set at
1:
.endm

    .text
    .set reorder

     # Decode EA. opsize must be set to 1, 2 or 4.
     # When 0, extracts from opcode
ALIGN(decode_ea):
    andi t1, opcode, 0x7
decode_ea_customreg:    # Like decode_ea, but t1 is already set to the register to use in decoding
    srl t0, opcode, 1
    andi t0, 0x7 << 2
    lw t0, ea_table(t0)
    sll t1, 2
    jr t0
ea_111: # Register-less EAs
    lw t1, ea7_table(t1)
    jr t1

ea_001: # An
    addiu t1, 8*4
ea_000: # Dn
    la_ctx eaptr, M64K_OFF_DREGS+4,t1
    decode_opsize
    sub eaptr, opsize
    li eaptr32, 0
    #if M64K_CONFIG_TIMING_ACCURACY >= 0
    addi rmw_cyctable, 4
    #endif
    jr ra
ea_010: # (An)
    lw_ctx eaptr32, M64K_OFF_AREGS,t1
    map_m68k eaptr, eaptr32
    use_cycles 4
    jr ra
ea_011: # (An)+
    lw_ctx eaptr32, M64K_OFF_AREGS,t1
    decode_opsize
    xor t2, t1, 7*4                      # check if t1(reg) is A7 (stack pointer)
    add t0, eaptr32, opsize              # increment address register
    bnez t2, 1f                          # if A7 (stack pointer), align to word
    add t0, 1
    and t0, ~1
1:  sw_ctx t0, M64K_OFF_AREGS,t1         # writeback incremented register
    map_m68k eaptr, eaptr32              # map address to N64 space
    use_cycles 4
    jr ra
ea_100: # -(An)
    lw_ctx eaptr32, M64K_OFF_AREGS,t1    # read address register
    decode_opsize                        # decode opsize (if wasn't already)
    xor t2, t1, 7*4
    sub eaptr32, opsize                  # decrement address register
    bnez t2, 1f                          # if A7 (stack pointer), align to word
    and eaptr32, ~1
1:  sw_ctx eaptr32, M64K_OFF_AREGS,t1    # write back address register
    map_m68k eaptr, eaptr32              # map address to N64 space
    use_cycles 6
    jr ra
ea_101: # (d16,An)
    lw_ctx eaptr32, M64K_OFF_AREGS,t1
    lh t0, 0(m_pc)
    addi m_pc, 2
    addu eaptr32, t0
    map_m68k eaptr, eaptr32
    use_cycles 8
    jr ra
ea7_011: # (d8,PC,Xn)
    lw t0, pc_diff
    addu eaptr32, m_pc, t0              # Compute exact 32-bit PC
    j ea_110_inner
ea_110: # (d8,An,Xn)
    lw_ctx eaptr32, M64K_OFF_AREGS,t1   # read An as base address
ea_110_inner:
    lbu t1, 0(m_pc)                     # read 8-bit extension word
    lb t0, 1(m_pc)                      # read 8-bit signed displacement
    addi m_pc, 2                        # increment PC after fetch
    addu eaptr32, t0                    # Add displacement to address
    srl t0, t1, 2                       # Extract Xn register index
    andi t0, 0xF << 2
    andi t2, t1, 0x8                    # Extract size bit (word / long)
    add t0, ctx                         # Compute Xn register address in context
    .set noreorder
    bnezl t2, 1f                        # if size bit is long, load with lw
     lw t1, M64K_OFF_DREGS(t0)
    .set reorder
    lh t1, M64K_OFF_DREGS+2(t0)         # if size bit is word, load with lh
1:  use_cycles 10
    addu eaptr32, t1
    map_m68k eaptr, eaptr32
    jr ra
ea7_000: # (xxx).W
    lh eaptr32, 0(m_pc)
    addi m_pc, 2
    map_m68k eaptr, eaptr32
    use_cycles 8
    jr ra
ea7_001: # (xxx).L
    lwl eaptr32, 0(m_pc)
    lwr eaptr32, 3(m_pc)
    addi m_pc, 4
    map_m68k eaptr, eaptr32
    use_cycles 12
    jr ra
ea7_010: # (d16,PC)
    lw t0, pc_diff
    addu eaptr32, m_pc, t0    # Compute exact eaptr32 (in case it's read via LEA/PEA)
    lh t0, 0(m_pc)
    addi m_pc, 2
    add eaptr32, t0
    map_m68k eaptr, eaptr32
    use_cycles 8
    jr ra
ea7_100: # #<data>
    move eaptr32, m_pc        # The address is stored at the current PC (in the fetch stream)
    decode_opsize
    xori t0, opsize, 1        # check if opsize is 1 (byte)
    use_cycles 4
    bnez t0, 1f               # if it is 1
    addi eaptr32, 1           #  skip a byte (byte to fetch is right-aligned)
1:  add m_pc, eaptr32, opsize # compute new PC after the address
    map_m68k eaptr, eaptr32
    jr ra

ea7_101: ea7_110: ea7_111:
    move a0, opcode
    jal __m64k_assert_invalid_ea
    addiu a1, m_pc, -2
    .set noreorder

###############################################################
# RMW accesses
###############################################################

    .sdata
rmw_table:
    .long   rmw8_easrc, rmw16_easrc, rmw32_easrc, invalid_opmode
    .long   rmw8_eadst, rmw16_eadst, rmw32_eadst, invalid_opmode

#if M64K_CONFIG_TIMING_ACCURACY >= 0
            # EA-mem: easrc 8/16, easrc 32, eadst 8/16, eadst 32
            # EA-reg: easrc 8/16, easrc 32, eadst 8/16, eadst 32
rmw_cyctable_logical:
    .byte   4, 10, 8, 16
    .byte   4, 6, 4, 8
rmw_cyctable_immediate:
    .byte   12, 24, 12, 24
    .byte   8, 16, 8, 16
rmw_cyctable_cmpi:
    .byte   8, 16, 8, 16
    .byte   8, 14, 8, 14
rmw_cyctable_quick:
    .byte   8, 16, 8, 16
    .byte   4, 8, 4, 8
rmw_cyctable_singleop:
    .byte   8, 16, 8, 16
    .byte   4, 6, 4, 6
rmw_cyctable_multiprec:
    .byte   6, 18, 6, 18
    .byte   4, 8, 4, 8
    .byte   4, 8, 4, 8       # we decode EA twice for multiprec, so repeat the EA-reg timings
rmw_cyctable_decimal:
    .byte   0, 0,6,8         # ABCD/SBCD use byte EA-Mem/EA-reg
    .byte   0, 0,6,6         # NBCD (ab-)uses eadst32
    .byte   0, 0,6,6         # we decode EA twice for decimal, so repeat the EA-reg timings
#endif

    .text
    .set noreorder

ALIGN(decode_dptr):
    srl dptr, opcode, 7       # get data register pointer
    andi dptr, 0x7 << 2       # isolate data register pointer
    la_ctx dptr, M64K_OFF_DREGS,dptr
    jr ra

ALIGN(rmw_easrc):
    andi opcode, 0xFFFF ^ (1<<8)
    j rmw
rmw_eadst:
    ori opcode, 1<<8          # force EA mode to EA-dst
rmw:
    jal decode_ea             # decode EA into eaptr/eaptr32
rmw_no_ea:
    srl t3, opcode, 4         # extract size+src/dst flag
    andi t3, 0x7 << 2
    lw t3, rmw_table(t3)
    jr t3                     # jump to the rmw function
    nop
    .set reorder
rmw8_easrc:
    li rmw_bitsize, 24
    use_cycles_table 0
    lbu_m68k t0, eaptr
    lbu t1, 3(dptr)
    jalr rmw_op
    sb result, 3(dptr)
    j main_loop
rmw16_easrc:
    use_cycles_table 0
    check_addr_error eaptr32, read_address_error
    li rmw_bitsize, 16
    lhu_m68k t0, eaptr
    lhu t1, 2(dptr)
    jalr rmw_op
    sh result, 2(dptr)
    j main_loop
rmw32_easrc:
    #if M64K_CONFIG_TIMING_ACCURACY >= 1
    use_cycles_opmask 0b111<<3, 0, 2          # 2 more cycles if EA is Dn
    use_cycles_opmask 0b111111, 0b111100, 2   # 2 more cycles if EA is immediate
    #endif
    use_cycles_table 1
rmw32_easrc_nocycfixup:
    check_addr_error eaptr32, read_address_error
    li rmw_bitsize, 0
    lw_m68k t0, eaptr
    lwu t1, 0(dptr)
    zx64 t0
    jalr rmw_op
    sw result, 0(dptr)
    j main_loop
rmw8_eadst:
    lbu_m68k t0, eaptr
    li rmw_bitsize, 24
    use_cycles_table 2
    move t1, t0
    lbu t0, 3(dptr)
    jalr rmw_op
    sb_m68k result, eaptr
    j main_loop
rmw16_eadst:
    check_addr_error eaptr32, read_address_error
    lhu_m68k t0, eaptr
    li rmw_bitsize, 16
    use_cycles_table 2
    move t1, t0
    lhu t0, 2(dptr)
    jalr rmw_op
    sh_m68k result, eaptr
    j main_loop
rmw32_eadst:
    check_addr_error eaptr32, read_address_error
    lw_m68k t0, eaptr
    li rmw_bitsize, 0
    use_cycles_table 3
    move t1, t0
    lwl t0, 0(dptr)   # NOTE: this is to handle immediate case (where dptr is PC-relative)
    lwr t0, 3(dptr)
    zx64 t0
    zx64 t1
    jalr rmw_op
    sw_m68k result, eaptr
    j main_loop

invalid_opmode:
    move a0, opcode
    addiu a1, m_pc, -2
    jal __m64k_assert_invalid_opmode
    .set noreorder

###############################################################
# Opcodes
###############################################################

    .set reorder

OP(addq): OP(subq):
    andi t0, opcode, 3<<6
    beq t0, 3<<6, op_scc
#ifdef M64K_FASTPATHS
    # ADDQ/SUBQ #imm,Dn fast path; An destinations (no flags, always long)
    # and memory EAs stay generic. Accuracy-0 charges: quick table, EA-reg
    # column (4 for .b/.w, 8 for .l).
    andi t0, opcode, 0x38
    beqz t0, addq_fast
#endif
    set_rmw_cyctable rmw_cyctable_quick
    use_cycles_opmask (7<<3)|(3<<6), (1<<3)|(2<<6), -2   # 2 fewer cycles if EA is An/long
    use_cycles_opmask (7<<3)|(3<<6), (1<<3)|(1<<6), +4   # 4 more cycles if EA is An/word
    lax rmw_op, op_subq_rmwimpl
    andi t0, opcode, 1<<8
    bnez t0, 1f
    lax rmw_op, op_addq_rmwimpl
1:  srl t1, opcode, 9   # extract immediate value
    addi t1, -1         # convert 0 to 8
    andi t1, 0x7
    addi t1, 1
    sw t1, M64K_OFF_PENDINGEXC+4(ctx)      # store imediate into dummy space
    la dptr, M64K_OFF_PENDINGEXC+4(ctx)    # set dptr to dummy space
    li opsize, 0            # opsize not calculated yet
    andi result, opcode, 0x7<<3  # isolate EA mode
    xori result, 1<<3            # set 0 if EA mode is An
    j rmw_eadst
op_addq_rmwimpl:
    .set noreorder
    bnez result, op_add_rmwimpl_flags   # if EA mode is not An, calculate flags
    daddu result, t0, t1
    jr ra
    nop
op_subq_rmwimpl:
    bnez result, op_sub_rmwimpl_flags   # if not, calculate flags like add
    dsubu result, t1, t0
    jr ra
    nop
    .set reorder

#ifdef M64K_FASTPATHS
addq_fast:
    srl t0, opcode, 9                  # immediate: bits 11:9, 0 means 8
    addi t0, -1
    andi t0, 7
    addi t0, 1
    andi t2, opcode, 7                 # destination Dn
    sll t2, 2
    add t2, ctx
    la dptr, M64K_OFF_PENDINGEXC+4(ctx) # generic-path dptr parity: successors
                                       # may dummy-read a stale dptr, so it
                                       # must stay a valid EVEN host pointer
    andi t1, opcode, 0x100             # bit 8: 0 = ADDQ, 1 = SUBQ
    bnez t1, subq_f
    andi t3, opcode, 0xC0
    beqz t3, addq_f_byte
    xori t1, t3, 0x40
    beqz t1, addq_f_word
addq_f_long:                           # addq.l #,Dn (8 cycles)
    lwu t1, M64K_OFF_DREGS(t2)
    daddu result, t0, t1
    move flag_zc, result
    dsrl flag_x, flag_zc, 32
    sll t0, 0
    sll t1, 0
    daddu flag_nv, t0, t1
    sw result, M64K_OFF_DREGS(t2)
    use_cycles 8
    dispatch_next
addq_f_word:                           # addq.w #,Dn (4 cycles)
    lhu t1, M64K_OFF_DREGS+2(t2)
    daddu result, t0, t1
    dsll flag_zc, result, 16
    dsrl flag_x, flag_zc, 32
    sll t0, 16
    sll t1, 16
    daddu flag_nv, t0, t1
    sh result, M64K_OFF_DREGS+2(t2)
    use_cycles 4
    dispatch_next
addq_f_byte:                           # addq.b #,Dn (4 cycles)
    lbu t1, M64K_OFF_DREGS+3(t2)
    daddu result, t0, t1
    dsll flag_zc, result, 24
    dsrl flag_x, flag_zc, 32
    sll t0, 24
    sll t1, 24
    daddu flag_nv, t0, t1
    sb result, M64K_OFF_DREGS+3(t2)
    use_cycles 4
    dispatch_next
subq_f:
    andi t3, opcode, 0xC0
    beqz t3, subq_f_byte
    xori t1, t3, 0x40
    beqz t1, subq_f_word
subq_f_long:                           # subq.l #,Dn (8 cycles)
    lwu t1, M64K_OFF_DREGS(t2)
    dsubu result, t1, t0
    move flag_zc, result
    dsrl flag_x, flag_zc, 32
    sll t0, 0
    sll t1, 0
    dsubu flag_nv, t1, t0
    sw result, M64K_OFF_DREGS(t2)
    use_cycles 8
    dispatch_next
subq_f_word:                           # subq.w #,Dn (4 cycles)
    lhu t1, M64K_OFF_DREGS+2(t2)
    dsubu result, t1, t0
    dsll flag_zc, result, 16
    dsrl flag_x, flag_zc, 32
    sll t0, 16
    sll t1, 16
    dsubu flag_nv, t1, t0
    sh result, M64K_OFF_DREGS+2(t2)
    use_cycles 4
    dispatch_next
subq_f_byte:                           # subq.b #,Dn (4 cycles)
    lbu t1, M64K_OFF_DREGS+3(t2)
    dsubu result, t1, t0
    dsll flag_zc, result, 24
    dsrl flag_x, flag_zc, 32
    sll t0, 24
    sll t1, 24
    dsubu flag_nv, t1, t0
    sb result, M64K_OFF_DREGS+3(t2)
    use_cycles 4
    dispatch_next
#endif

OP(addi):
    lax rmw_op, op_add_rmwimpl
    jal_and_j decode_imm, rmw_eadst
op_subi:  # (same cacheline)
    lax rmw_op, op_sub_rmwimpl
    jal_and_j decode_imm, rmw_eadst
#ifdef M64K_FASTPATHS
# CMPI #imm,Dn fast path: CMP flags (no X), no writeback. The flag code is an
# exact inline copy of op_cmp_rmwimpl with rmw_bitsize folded. Accuracy-0
# charges: cmpi table, EA-reg column (8 for .b/.w, 14 for .l). The (illegal)
# size-11 encoding keeps the generic path's behavior.
op_cmpi:
    andi t0, opcode, 0x38
    bnez t0, cmpi_slow
    andi t2, opcode, 7                 # Dn operand
    sll t2, 2
    add t2, ctx
    andi t3, opcode, 0xC0
    beqz t3, cmpi_f_byte
    beq t3, 0xC0, cmpi_slow
    xori t1, t3, 0x40
    beqz t1, cmpi_f_word
cmpi_f_long:                           # cmpi.l #,Dn (14 cycles)
    move dptr, m_pc                    # imm_32 parity (stale-dptr rule)
    lwl t0, 0(m_pc)                    # 32-bit immediate from the code stream
    lwr t0, 3(m_pc)
    addiu m_pc, 4
    lwu t1, M64K_OFF_DREGS(t2)
    zx64 t0
    dsubu result, t1, t0
    move flag_zc, result
    sll t0, 0
    sll t1, 0
    dsubu flag_nv, t1, t0
    use_cycles 14
    dispatch_next
cmpi_f_word:                           # cmpi.w #,Dn (8 cycles)
    addi dptr, m_pc, -2                # imm_16 parity (stale-dptr rule)
    lhu t0, 0(m_pc)
    addiu m_pc, 2
    lhu t1, M64K_OFF_DREGS+2(t2)
    dsubu result, t1, t0
    dsll flag_zc, result, 16
    sll t0, 16
    sll t1, 16
    dsubu flag_nv, t1, t0
    use_cycles 8
    dispatch_next
cmpi_f_byte:                           # cmpi.b #,Dn (8 cycles)
    addi dptr, m_pc, -2                # imm_8 parity (stale-dptr rule)
    lbu t0, 1(m_pc)                    # low byte of the extension word
    addiu m_pc, 2
    lbu t1, M64K_OFF_DREGS+3(t2)
    dsubu result, t1, t0
    dsll flag_zc, result, 24
    sll t0, 24
    sll t1, 24
    dsubu flag_nv, t1, t0
    use_cycles 8
    dispatch_next
cmpi_slow:
#else
op_cmpi:
#endif
    set_rmw_cyctable rmw_cyctable_cmpi
    lax rmw_op, op_cmp_rmwimpl
    jal_and_j decode_imm_nocyctable, rmw_eadst

#ifdef M64K_FASTPATHS
# ADD <Dn>,Dm reg-direct fast path (~3% of in-match instructions). The flag
# code is an exact inline copy of op_add_rmwimpl with rmw_bitsize folded to a
# constant; charges are the generic accuracy-0 costs (logical table, EA-reg
# column: 4 for .b/.w, 6 for .l — the hardware's 8 for .l only appears at
# accuracy>=1, where fast paths are compiled out). ADDA/ADDX stay generic.
OP(add):
    andi t0, opcode, 0x138             # EA mode Dn, direction <ea>+Dm -> Dm
    bnez t0, add_slow
    andi t0, opcode, 0xC0
    beq t0, 0xC0, add_slow             # opmode 011 = ADDA.w
    andi t1, opcode, 7                 # source Dn
    sll t1, 2
    add t1, ctx
    srl t2, opcode, 7                  # destination Dm
    andi t2, 0x7<<2
    add t2, ctx
    move dptr, t2                      # decode_dptr parity (stale-dptr rule)
    beqz t0, add_f_byte
    xori t3, t0, 0x40
    beqz t3, add_f_word
add_f_long:                            # add.l Dn,Dm (6 cycles at accuracy 0)
    lwu t0, M64K_OFF_DREGS(t1)
    lwu t1, M64K_OFF_DREGS(t2)
    daddu result, t0, t1
    move flag_zc, result               # bit32 = carry, low32 = !Z
    dsrl flag_x, flag_zc, 32
    sll t0, 0                          # re-sign-extend, like sllv by 0
    sll t1, 0
    daddu flag_nv, t0, t1
    sw result, M64K_OFF_DREGS(t2)
    use_cycles 6
    dispatch_next
add_f_word:                            # add.w Dn,Dm (4 cycles)
    lhu t0, M64K_OFF_DREGS+2(t1)
    lhu t1, M64K_OFF_DREGS+2(t2)
    daddu result, t0, t1
    dsll flag_zc, result, 16
    dsrl flag_x, flag_zc, 32
    sll t0, 16
    sll t1, 16
    daddu flag_nv, t0, t1
    sh result, M64K_OFF_DREGS+2(t2)
    use_cycles 4
    dispatch_next
add_f_byte:                            # add.b Dn,Dm (4 cycles)
    lbu t0, M64K_OFF_DREGS+3(t1)
    lbu t1, M64K_OFF_DREGS+3(t2)
    daddu result, t0, t1
    dsll flag_zc, result, 24
    dsrl flag_x, flag_zc, 32
    sll t0, 24
    sll t1, 24
    daddu flag_nv, t0, t1
    sb result, M64K_OFF_DREGS+3(t2)
    use_cycles 4
    dispatch_next
add_slow:
#else
OP(add):
#endif
    set_rmw_cyctable rmw_cyctable_logical
    lax rmw_op, op_add_rmwimpl
    jal decode_dptr
    andi t0, opcode, 0xC0
    beq t0, 0xC0, op_adda     # if adda jump to implementation
    andi t0, opcode, 0x130    # check if the opcode is addx instead
    xori t0, 0x100
    use_cycles_opmask (7<<3)|(3<<6)|(1<<8), (1<<3)|(2<<6)|(0<<8), 2   # 2 more cycles if EA is dst-An/word
    li opsize, 0              # opsize not calculated yet
    beqz t0, op_addx          # if addx, jump to implementation
    j rmw
op_add_rmwimpl:
    daddu result, t0, t1
op_add_rmwimpl_flags:
    dsll flag_zc, result, rmw_bitsize
    dsrl flag_x, flag_zc, 32
    sllv t0, t0, rmw_bitsize
    sllv t1, t1, rmw_bitsize
    daddu flag_nv, t0, t1
    jr ra

OP(sub):
    set_rmw_cyctable rmw_cyctable_logical
    lax rmw_op, op_sub_rmwimpl
    jal decode_dptr
    andi t0, opcode, 0xC0
    beq t0, 0xC0, op_suba     # if suba jump to implementation
    andi t0, opcode, 0x130
    xori t0, 0x100
    use_cycles_opmask (7<<3)|(3<<6)|(1<<8), (1<<3)|(2<<6)|(0<<8), 2   # 2 more cycles if EA is dst-An
    li opsize, 0              # opsize not calculated yet
    beqz t0, op_subx          # if subx, jump to implementation
    j rmw
op_sub_rmwimpl:
    dsubu result, t1, t0
op_sub_rmwimpl_flags:
    dsll flag_zc, result, rmw_bitsize
    dsrl flag_x, flag_zc, 32
    sllv t0, t0, rmw_bitsize
    sllv t1, t1, rmw_bitsize
    dsubu flag_nv, t1, t0
    jr ra

OP(nbcd):
    set_rmw_cyctable rmw_cyctable_decimal+1
    li opsize, 1
    lax rmw_op, op_nbcd_rmwimpl
    j rmw_eadst
OP(abcd):
    set_rmw_cyctable rmw_cyctable_decimal
    li opsize, 1
    lax rmw_op, op_abcd_rmwimpl
    j op_addx_start
op_sbcd:
    set_rmw_cyctable rmw_cyctable_decimal
    li opsize, 1
    lax rmw_op, op_sbcd_rmwimpl
    j op_addx_start
op_subx: # opsize already set to 0 by caller
    set_rmw_cyctable rmw_cyctable_multiprec
    lax rmw_op, op_subx_rmwimpl
    j op_addx_start
op_addx: # opsize already set to 0 by caller
    set_rmw_cyctable rmw_cyctable_multiprec
    lax rmw_op, op_addx_rmwimpl
op_addx_start:
    andi t0, opcode, 0x8         # isolate R/M bit
    andi t1, opcode, 0x7         # extract Ry*4
    sll t1, 2
    beqz t0, op_addx_data        # checl if R/M is data or address
op_addx_addr:
    jal ea_100                   # decode Rx as pre-decrement address register
    #if M64K_CONFIG_ADDRERR
    beq opsize, 1, 1f
    check_addr_error eaptr32, read_address_error
    #endif
1:  xori opcode, 0x28            # force EA mode to predecrement
    j op_addx_finish
op_addx_data:
    jal ea_000                   # decode Rx as data register
op_addx_finish:
    add dptr, eaptr, opsize      # set dptr to decoded address
    srl t1, opcode, 9            # extract Ry
    andi t1, 0x7
    jal decode_ea_customreg      # decode EA with Ry
    addi dptr, -4                # finish adjusting dptr (so that rmw does the right thing)
    j rmw_no_ea
op_addx_rmwimpl:
    andi flag_x, 1                       # isolate flag_x to 0/1
    zx64 t2, flag_zc                     # put current Z flag (without C) into t2
    daddu result, t0, t1                 # compute full size result
    daddu result, flag_x                 # add flag_x
    dsll flag_zc, result, rmw_bitsize    # compute new flag_zc
    or flag_zc, t2                       # combine with old Z flag
    sllv t0, t0, rmw_bitsize             # put operands into MSB
    sllv t1, t1, rmw_bitsize
    sllv flag_x, flag_x, rmw_bitsize
    daddu flag_nv, t0, t1                # compute flag_nv
    daddu flag_nv, flag_x
    dsrl flag_x, flag_zc, 32             # copy new carry bit into flag_x
    jr ra
OP(subx_rmwimpl):
    andi flag_x, 1                       # isolate flag_x to 0/1
    zx64 t2, flag_zc                     # put current Z flag (without C) into t2
    dsubu result, t1, t0                 # compute full size result
    dsubu result, flag_x                 # subtract flag_x
    dsll flag_zc, result, rmw_bitsize    # compute new flag_zc
    or flag_zc, t2                       # combine with old Z flag
    sllv t0, t0, rmw_bitsize             # put operands into MSB
    sllv t1, t1, rmw_bitsize
    sllv flag_x, flag_x, rmw_bitsize
    dsubu flag_nv, t1, t0                # compute flag_nv
    dsubu flag_nv, flag_x
    dsrl flag_x, flag_zc, 32             # copy new carry bit into flag_x
    jr ra
OP(abcd_rmwimpl):
    andi flag_x, 1                       # isolate X flag
    addu result, t0, t1
    addu result, flag_x                  # calculate t0+t1+X (uncorrected result)
    xor t0, t1
    xor t0, result                       # get carries from addition (both 8-bit and 4-bit)
    addiu t1, result, 0x66
    xor t1, result                       # get carries from adding 0x66
    or t0, t1                            # combine carries
    andi t0, 0x110                       # isolate carries
    srl flag_x, t0, 8                    # set X flag to the combined 8-bit carry
    srl t1, t0, 2
    subu t0, t1                          # calculate corf
    sll t0, 23                           # align corf to top 32-bit
    sll flag_nv, result, 24              # align result to top 32-bit
    daddu flag_nv, t0                    # add corf and affact N/V
    srl result, flag_nv, 24              # shift result back to 24-bit
    zx64 flag_zc                         # clear C
    or flag_zc, result                   # accumulate Z and set C
    dsll t0, flag_x, 32
    or flag_zc, t0                       # C combines both the intermediate and the final carry
    jr ra
OP(nbcd_rmwimpl):
    move t0, t1
    li t1, 0
op_sbcd_rmwimpl:
    # Calculate uncorrected result
    andi flag_x, 1
    subu result, t1, t0
    subu result, flag_x
    # Get borrows from the subtraction
    xor t0, t1
    xor t0, result
    # Calculate corf, then align to top of 32-bit
    andi t0, 0x110
    srl flag_x, t0, 8  # Also set X flag to the borrow
    srl t1, t0, 2
    subu t0, t1
    sll t0, 23
    # Subtract corf and set sign/overflow
    sll flag_nv, result, 24
    sltu t1, flag_nv, t0 # Also update X flag on subtraction borrow
    or flag_x, t1
    dsubu flag_nv, t0
    srl result, flag_nv, 24
    # Accumulate zero and set carry
    zx64 flag_zc
    or flag_zc, result
    dsll t0, flag_x, 32
    or flag_zc, t0
    jr ra

OP(suba):
    li result, 1
    j op_adda_start
op_adda:
    li result, 0
op_adda_start:
    addi dptr, 32                   # Point to AREG instead of DREG
    andi t0, opcode, 0x100          # Check W/L bit
    bnez t0, 1f                     # Jump to correct body
    lax rmw_op, op_adda16_rmwimpl   # ADDA 16-bit
    li opsize, 2                      # Force word size in EA decoding
    jal decode_ea                     # Decode EA
    addi eaptr, -2                    # Convert eaptr to long (for rmw32)
    use_cycles 8
    j rmw32_easrc_nocycfixup          # Do RMW
1:  lax rmw_op, op_adda32_rmwimpl   # ADDA 32-bit
    li opsize, 4                      # Force long size in EA decoding
    jal decode_ea                     # Decode EA
    use_cycles 10
    use_cycles_opmask 7<<3,1<<3,-2
    use_cycles_opmask 7<<3,0<<3,-2
    use_cycles_opmask 0b111111,0b111100,2
    j rmw32_easrc_nocycfixup          # Do RMW
op_adda16_rmwimpl:
    sll t0, 16
    sra t0, 16
op_adda32_rmwimpl:
    bnez result, 1f
    daddu result, t0, t1
    jr ra
1:  dsubu result, t1, t0
    jr ra

OP(cmp):
    andi t0, opcode, 0b111 << 6
    beq t0, 0b011 << 6, op_cmpa16
    beq t0, 0b111 << 6, op_cmpa32
    bge t0, 0b100 << 6, op_eor
    use_cycles_opmask (1<<7)|(7<<3), 1<<7, -2     # cmp.l with EA=Dn takes 2 cycle less
    use_cycles_opmask 0b11111111, 0b10111100, -2  # cmp.l #<data>,Dn takes 2 cycle less
    lax rmw_op, op_cmp_rmwimpl
    j op_group_logical
op_cmp_rmwimpl:
    dsubu result, t1, t0
    dsll flag_zc, result, rmw_bitsize
    sllv t0, t0, rmw_bitsize
    sllv t1, t1, rmw_bitsize
    dsubu flag_nv, t1, t0
    j main_loop               # skip rmw writeback

OP(cmpa16):
    use_cycles 6
    li opsize, 2
    jal decode_ea
    lax rmw_op, op_cmpa16_rmwimpl
    j op_cmpa_start
op_cmpa32:
    use_cycles 10
    use_cycles_opmask 0b110000, 0b000000, -4  # cmpa.l Dn/An,An takes 4 cycle less
    li opsize, 4
    jal decode_ea
    lax rmw_op, op_cmp_rmwimpl
op_cmpa_start:
    jal decode_dptr
    addi dptr, 8*4
    j rmw32_easrc_nocycfixup
op_cmpa16_rmwimpl:
    lh_m68k t0, eaptr
    zx64 t0
    j op_cmp_rmwimpl

OP(cmpm):
    use_cycles 4
    use_cycles_opmask 1<<7, 1<<7, 8   # cycle fixup for long size
    li opsize, 0                      # opsize not calculated yet
    andi t1, opcode, 7                # extract Ay*4 (needed for EA calculation)
    sll t1, 2
    jal ea_011                        # calculate EA as post-increment
    sub dptr, eaptr, 4                # set dptr to decoded address, adjusting for size
    add dptr, opsize                  #  (rmw treats dptr accesses differently from eaptr)
    #if M64K_CONFIG_ADDRERR
    andi t0, opcode, 3<<6             # if the size is not byte, check for address error
    beqz t0, 1f                       #  (we do this because rmw will not check address errors in dptr)
    check_addr_error eaptr32, read_address_error
    #endif
1:  srl t1, opcode, 7                 # extract Ax*4
    andi t1, 7<<2
    jal ea_011                        # calculate EA as post-increment
    lax rmw_op, op_cmp_rmwimpl
    j rmw_no_ea                       # do rmw

OP(mul):
    #if M64K_CONFIG_TIMING_ACCURACY == 0
    use_cycles 50
    #endif
    li opsize, 2
    jal decode_ea
    check_addr_error eaptr32, read_address_error
    jal decode_dptr
    andi t0, opcode, 1<<8
    bnez t0, op_muls_exec
op_mulu_exec:
    lhu t1, 2(dptr)
    lhu_m68k t0, eaptr
    multu t0, t1
    j op_mul_finish
op_muls_exec:
    lh t1, 2(dptr)
    lh_m68k t0, eaptr
    mult t0, t1
    #if M64K_CONFIG_TIMING_ACCURACY >= 1
    sll t1, t0, 1
    xor t0, t1                    # t0 = (t0<<1) ^ t0
    andi t0, 0xFFFF               # (it now contains 1 on 0->1 transitions)
    #endif
op_mul_finish:
    mflo flag_nv
    zx64 flag_zc, flag_nv
    sw flag_nv, 0(dptr)
    #if M64K_CONFIG_TIMING_ACCURACY < 1
    j main_loop
    #else
    use_cycles 38
    sll t1, t0, 1                  # optimized popcount algorithm
    andi t0, 0x5555<<1             # from Bit Twiddling Hacks
    sub t0, t1, t0                 # also tweaked to count twice the population
    srl t1, t0, 2
    andi t1, 0x3333<<1
    andi t0, 0x3333<<1
    add t0, t1
    srl t1, t0, 4
    add t0, t1
    andi t0, 0x0F0F<<1
    srl t1, t0, 8
    add t0, t1
    andi t0, 0xFF
    sub m_cycles, t0
    j main_loop
    #endif

OP(div):
    li opsize, 2
    jal decode_ea
    check_addr_error eaptr32, read_address_error
    jal decode_dptr
    andi t0, opcode, 1<<8
    use_cycles 12
    bnez t0, op_divs_exec
op_divu_exec:
    lw t1, 0(dptr)
    lhu_m68k t0, eaptr
    #if M64K_CONFIG_DIVBYZERO
    beqz t0, exception_divbyzero
    #endif
    sgt t3, t1, t0
    li rmw_cyctable, 100
    divu zero, t1, t0
    mflo t0
    and t2, t0, 0xFFFF
    j op_div_finish
op_divs_exec:
    lw t1, 0(dptr)
    lh_m68k t0, eaptr
    #if M64K_CONFIG_DIVBYZERO
    beqz t0, exception_divbyzero
    #endif
    sgt t3, t1, t0
    li rmw_cyctable, 115
    div zero, t1, t0
    mflo t0
    sll t2, t0, 16
    sra t2, 16
op_div_finish:
    bne t0, t2, op_div_overflow
    mfhi t1
    zx64 flag_zc, t0
    sll flag_nv, t0, 16
    sh t1, 0(dptr)
    sh t0, 2(dptr)
    .set noreorder
    bnez t3, 1f
    sub m_cycles, rmw_cyctable
1:  .set reorder
    j main_loop
op_div_overflow:
    not32sx flag_nv
    not32 flag_nv
    zx64 flag_zc
    j main_loop

OP(group_logical):
    li opsize, 0
    set_rmw_cyctable rmw_cyctable_logical
    jal_and_j decode_dptr, rmw

OP(andi):
    andi t0, opcode, 0b111111
    beq t0, 0b111100, op_andi_sr
    lax rmw_op, op_and_rmwimpl
    jal_and_j decode_imm, rmw_eadst
OP(ori):
    andi t0, opcode, 0b111111
    beq t0, 0b111100, op_ori_sr
    lax rmw_op, op_or_rmwimpl
    jal_and_j decode_imm, rmw_eadst

OP(and):
    li t1, 3<<6
    and t0, opcode, t1                      # Whan opsize is 3
    beq t0, t1, op_mul                      # ... it is the MUL opcode instead
    andi t0, opcode, (1<<8) | (0b110 << 3)  # When EA-Dst (bit 8) and EA mode An/Dn (bit 3-5: 000/001)
    beq t0, 1<<8, op_exg                    # ... it is the EXG opcode instead
    lax rmw_op, op_and_rmwimpl
    j op_group_logical
op_and_rmwimpl:
    and result, t0, t1
    sllv flag_nv, result, rmw_bitsize
    zx64 flag_zc, flag_nv
    jr ra

OP(or):
    .set noat
    .set noreorder
    and t0, opcode, 3<<6
    li $1, 3<<6
    beq t0, $1, op_div
     andi t0, opcode, 0b11111<<4
    li $1, 0b10000<<4
    beq t0, $1, op_sbcd
     li $1, 0b10100<<4
    beq t0, $1, op_pack
     li $1, 0b11000<<4
    beq t0, $1, op_unpk
     nop
    .set at
    .set reorder
    lax rmw_op, op_or_rmwimpl
    j op_group_logical
op_or_rmwimpl:
    or result, t0, t1
    sllv flag_nv, result, rmw_bitsize
    zx64 flag_zc, flag_nv
    jr ra

OP(eori):
    andi t0, opcode, 0b111111
    beq t0, 0b111100, op_eori_sr
    lax rmw_op, op_eor_rmwimpl
    jal_and_j decode_imm, rmw_eadst
OP(eor):
    andi t0, opcode, 0b111000
    beq t0, 0b001000, op_cmpm
    lax rmw_op, op_eor_rmwimpl
    j op_group_logical
op_eor_rmwimpl:
    xor result, t0, t1
    sllv flag_nv, result, rmw_bitsize
    zx64 flag_zc, flag_nv
    jr ra

OP(not):
    srl t0, opcode, 6
    beq t0, 0b0100011011, op_mvtsr
    set_rmw_cyctable rmw_cyctable_singleop
    lax rmw_op, op_not_rmwimpl
    j rmw_eadst
op_not_rmwimpl:
    not result, t1
    sll flag_nv, result, rmw_bitsize
    zx64 flag_zc, flag_nv
    jr ra

OP(neg):
    srl t0, opcode, 6
    beq t0, 0b0100010011, op_mvccr
    set_rmw_cyctable rmw_cyctable_singleop
    lax rmw_op, op_neg_rmwimpl
    j rmw_eadst
op_neg_rmwimpl:
    move t0, t1
    li t1, 0
    j op_sub_rmwimpl

OP(negx):
    srl t0, opcode, 6
    beq t0, 0b0100000011, op_mvfsr
    set_rmw_cyctable rmw_cyctable_singleop
    lax rmw_op, op_negx_rmwimpl
    j rmw_eadst
op_negx_rmwimpl:
    move t0, t1
    li t1, 0
    j op_subx_rmwimpl


OP(roxli): OP(roxri):
    andi opcode, 0xFFFF^(7<<3)   # turn off ir so that EA is Dn
    lax t1, op_roxl_rmwimpl
    lax t2, op_roxr_rmwimpl
    j op_shift_imm
OP(roli): OP(rori):
    andi opcode, 0xFFFF^(7<<3)   # turn off ir so that EA is Dn
    lax t1, op_rol_rmwimpl
    lax t2, op_ror_rmwimpl
    j op_shift_imm
OP(lsli): OP(lsri):
    xori opcode, 1<<3
    lax t1, op_lsl_rmwimpl
    lax t2, op_lsr_rmwimpl
    j op_shift_imm
OP(asli): OP(asri):
    lax t1, op_asl_rmwimpl
    lax t2, op_asr_rmwimpl
op_shift_imm:
    andi t0, opcode, 1<<8
    move rmw_op, t1
    bnez t0, asli_start
    move rmw_op, t2
asli_start:
    srl t2, opcode, 9    # extract immediate value in t2
    addi t2, -1          # if value is 0, it means 8
    andi t2, 7
    addi t2, 1
    #if M64K_CONFIG_TIMING_ACCURACY >= 0
    jal_and_j group_shifts_cycles, rmw_eadst
    #else
    j rmw_eadst          # call RMW (note opsize=0 in OP(shifts)).
    #endif

OP(lsl): OP(lsr):
    andi t0, opcode, 1<<8
    andi opcode, 0xFFFF^(1<<5)^(1<<3)   # turn off ir so that EA is Dn
    lax rmw_op, op_lsl_rmwimpl
    bnez t0, group_shifts
    lax rmw_op, op_lsr_rmwimpl
    j group_shifts
OP(asl): OP(asr):
    andi t0, opcode, 1<<8
    andi opcode, 0xFFFF^(1<<5)   # turn off ir so that EA is Dn
    lax rmw_op, op_asl_rmwimpl
    bnez t0, group_shifts
    lax rmw_op, op_asr_rmwimpl
    j group_shifts
OP(rol): OP(ror):
    andi t0, opcode, 1<<8
    andi opcode, 0xFFFF^(7<<3)   # turn off ir so that EA is Dn
    lax rmw_op, op_rol_rmwimpl
    bnez t0, group_shifts
    lax rmw_op, op_ror_rmwimpl
    j group_shifts
OP(roxl): OP(roxr):
    andi t0, opcode, 1<<8
    andi opcode, 0xFFFF^(7<<3)   # turn off ir so that EA is Dn
    lax rmw_op, op_roxl_rmwimpl
    bnez t0, group_shifts
    lax rmw_op, op_roxr_rmwimpl
    j group_shifts

    #if M64K_CONFIG_TIMING_ACCURACY >= 0
group_shifts_cycles:
    sub m_cycles, t2
    sub m_cycles, t2
group_shifts_cycles_nocount:
    use_cycles 8
    use_cycles_opmask 1<<7, 0, -2
    jr ra
    #endif

group_shifts:
    srl t0, opcode, 7            # read source Dn register into t2; similar to decode_dptr,
    andi t0, 0x7<<2              #  but we use t2 instead so that we can reuse the impl
    lw_ctx t2, M64K_OFF_DREGS,t0 #  functions used by immediate shifts.
    andi t2, 63                  # mask the shift amount
    #if M64K_CONFIG_TIMING_ACCURACY >= 0
    jal_and_j group_shifts_cycles, rmw_eadst  # call RMW (note opsize=0 in OP(shifts))
    #else
    j rmw_eadst
    #endif

OP(lsl_rmwimpl): # t1=register, t2=shift amount
    dsllv result, t1, t2                # calculate extended result
    dsllv flag_zc, result, rmw_bitsize  # affect Z and C flags
    sllv flag_nv, result, rmw_bitsize   # affect N flag and reset V flag
    beqz t2, 1f                         # affect X flag only if shift amount is 0
    dsrl flag_x, flag_zc, 32
1:  jr ra
OP(lsr_rmwimpl): # t1=register, t2=shift amount
    dsrlv result, t1, t2                # do the right shift into result
    addi t0, t2, -1                     # reduce shift amount
    dsrlv flag_zc, t1, t0               # shift one less (last bit = carry)
    dsll flag_zc, 32                    # extract carry bit
    or flag_zc, result                  # combine with zero bit
    sll flag_nv, result, rmw_bitsize    # set N to result sign and reset V
    beqz t2, 1f                         # affect X flag only if shift amount is 0
    dsrl flag_x, flag_zc, 32
1:  jr ra
OP(asl_rmwimpl): # t1=register, t2=shift amount
    dsllv result, t1, t2                # calculate extended result
    dsllv flag_zc, result, rmw_bitsize  # affect Z and C flags
    sllv flag_nv, result, rmw_bitsize   # affect N flag and reset V flag
    dsrav t0, flag_nv, t2               # shift back expected sign bits and
    sllv t1, t1, rmw_bitsize            # ...
    sne t0, t1                          # compare to sign-extended input
    dsll t0, 63                         # set V flag on mismatch
    xor flag_nv, t0
    beqz t2, 1f                         # affect X flag only if shift amount is 0
    dsrl flag_x, flag_zc, 32
1:  jr ra
OP(asr_rmwimpl): # t1=register, t2=shift amount
    sllv flag_nv, t1, rmw_bitsize       # set N flag to sign of input, reset V flag
    dsll result, flag_nv, 32            # align input to the upper bits
    dsrav result, result, t2            # arithmetic shift right
    dsrlv result, result, rmw_bitsize   # align result to bit 32, carry to bit 31
    dsrl flag_zc, result, 31            # extract carry bit
    dsll flag_zc, 32
    dsrl result, 32                     # align result to bit 0, zero-extended
    or flag_zc, result                  # combine Z and C
    beqz t2, 1f                         # affect X flag only if shift amount is 0
    dsrl flag_x, flag_zc, 32
1:  jr ra
OP(rol_rmwimpl): # t1=register, t2=shift amount
    sll t3, opsize, 3                   # calculate bitsize in bits (8, 16, 32)
    addi t0, t3, -1                     # calculate bytessize mask (7, 15, 31)
    neg result, t2
    and result, t0                      # res = shift right amount (range 0..N-1)
    sub t0, t3, result                  # t0 = shift left amount (range 1..N)
    dsllv t0, t1, t0                    # shift left
    dsrlv t1, t1, result                # shift right
    or result, t0, t1                   # combine
    dsllv flag_zc, result, rmw_bitsize  # affect Z and C flags
    sllv flag_nv, result, rmw_bitsize   # affect N flag and reset V flag
    bnez t2, 1f                         # clear C flag if shift amount is 0
    zx64 flag_zc
1:  jr ra
OP(ror_rmwimpl): # t1=register, t2=shift amount
    sll t3, opsize, 3                   # calculate bitsize in bits (8, 16, 32)
    addi result, t3, -1                 # calculate bytessize mask (7, 15, 31)
    and t0, t2, result
    sub result, t3, t0                  # t0 = shift right amount (range 1..N)
    dsrlv t0, t1, t0                    # shift right
    dsllv t1, t1, result                # shift left
    or result, t0, t1                   # combine
    sllv flag_nv, result, rmw_bitsize   # affect N flag and reset V flag
    move flag_zc, flag_nv               # affect Z and C flags
    bnez t2, 1f                         # clear C flag if shift amount is 0
    zx64 flag_zc
1:  jr ra

OP(roxr_rmwimpl): # t1=register, t2=shift amount
    andi opcode, 0xFFFF^(1<<8)          # clear direction bit again (it was turned on in rmw_eadst)
OP(roxl_rmwimpl): # t1=register, t2=rotate amount
    andi flag_x, 1
    dsll t0, flag_x, 32                 # move input carry (from X) to bit 32
    dsll t1, rmw_bitsize                # left-align input register to bit 31
    or t1, t0                           # combine with input carry
    srl t3, opsize, 1
    add t3, 3                           # t3 = 3,4,5 for b,w,l. Next lines explain the byte case.
    xori opsize, rmw_bitsize, 31        # bitsize = 7,15,31 for b,w,l
    srlv t0, t2, t3                     # t0 = ra>>3 = ra/8
    and t2, opsize                      # t2 = ra&7 = ra%8
    sub t2, t0                          # ra%8 - ra/8 = ra%9 (though with negative remainders)
    andi result, opcode, 1<<8           # check rotate direction
    bnez result, 1f                     # if roxr, negate t2
    neg t2
1:  addi opsize, 2                      # opsize = 9,17,33 for b,w,l
    bgtz t2, 1f                         # adjust remainder if negative or zero
    add t2, opsize                      # t2 = 1..9 = shift-left amount
1:  sub t3, opsize, t2                  # t3 = 8..0 = shift-right amount
    dsllv t0, t1, t2                    # shift left
    dsrlv t1, t1, t3                    # shift right
    or result, t0, t1                   # combine
    dsrlv result, result, rmw_bitsize   # shift back result into correct place
    dsllv flag_zc, result, rmw_bitsize  # affect Z and C flags
    sllv flag_nv, result, rmw_bitsize   # affect N flag and reset V flag
    dsrl flag_x, flag_zc, 32
    jr ra

OP(aslm): OP(asrm):
    jal decode_ea                       # decode EA (note opsize=2 in OP(bits))
    check_addr_error eaptr32, read_address_error
    andi t1, opcode, 0x100
    #if M64K_CONFIG_TIMING_ACCURACY >= 0
    jal group_shifts_cycles_nocount
    #endif
    lh_m68k t0, eaptr
    bnez t1, op_asl_ea_left
op_asl_ea_right:
    sra result, t0, 1
    andi flag_x, t0, 1
    j op_asl_ea_finish
op_asl_ea_left:
    sll result, t0, 1
    srl flag_x, result, 16
op_asl_ea_finish:
    sh_m68k result, eaptr
    dsll flag_nv, result, 16
    zx64 flag_zc, flag_nv
    dsll t0, flag_x, 32
    or flag_zc, t0
    j main_loop

OP(lslm): OP(lsrm):
    jal decode_ea                       # decode EA (note opsize=2 in OP(bits))
    check_addr_error eaptr32, read_address_error
    andi t1, opcode, 0x100
    #if M64K_CONFIG_TIMING_ACCURACY >= 0
    jal group_shifts_cycles_nocount
    #endif
    lhu_m68k t0, eaptr
    bnez t1, op_lsl_ea_left
op_lsl_ea_right:
    srl result, t0, 1
    andi flag_x, t0, 1
    j op_lsl_ea_finish
op_lsl_ea_left:
    sll result, t0, 1
    srl flag_x, result, 16
op_lsl_ea_finish:
    sh_m68k result, eaptr
    sll flag_nv, result, 16
    zx64 flag_zc, flag_nv
    dsll t0, flag_x, 32
    or flag_zc, t0
    j main_loop

OP(rolm): OP(rorm):
    jal decode_ea                       # decode EA (note opsize=2 in OP(bits))
    check_addr_error eaptr32, read_address_error
    andi t1, opcode, 0x100
    #if M64K_CONFIG_TIMING_ACCURACY >= 0
    jal group_shifts_cycles_nocount
    #endif
    lhu_m68k t0, eaptr
    bnez t1, op_rom_ea_left
op_rom_ea_right:
    srl result, t0, 1
    sll t0, 15
    or result, t0
    sll flag_nv, result, 16
    move flag_zc, flag_nv
    sh_m68k result, eaptr
    j main_loop
op_rom_ea_left:
    sll result, t0, 1
    srl t0, 15
    or result, t0
    sll flag_nv, result, 16
    dsll flag_zc, result, 16
    sh_m68k result, eaptr
    j main_loop

OP(roxlm): OP(roxrm):
    jal decode_ea                       # decode EA (note opsize=2 in OP(bits))
    check_addr_error eaptr32, read_address_error
    andi t1, opcode, 0x100        # check rotate direction
    #if M64K_CONFIG_TIMING_ACCURACY >= 0
    jal group_shifts_cycles_nocount
    #endif
    lhu_m68k t0, eaptr            # fetch memory operand
    andi flag_x, 1                # isolate input carry bit
    bnez t1, op_roxm_ea_left
op_roxm_ea_right:
    srl result, t0, 1
    sll flag_x, 15
    sll t0, 16
    or result, t0
    j op_roxm_finish
op_roxm_ea_left:
    sll result, t0, 1             # shift left, new carry in bit 16
op_roxm_finish:
    or result, flag_x             # merge input carry
    sll flag_nv, result, 16       # set N flag, and clear V flag
    dsll flag_zc, result, 16      # set Z and C flags
    dsrl flag_x, flag_zc, 32      # set X flag as a copy of C
    sh_m68k result, eaptr         # store result
    j main_loop

OP(clr):
    set_rmw_cyctable rmw_cyctable_singleop
    lax rmw_op, op_clr_rmwimpl
    j rmw_eadst                   # run rmw cycle (note: clr does a dummy read on 68000)
op_clr_rmwimpl:
    li result, 0
    li flag_nv, 0
    li flag_zc, 0
    jr ra

#ifdef M64K_FASTPATHS
# TST.b/.w (d16,An) fast path (the dominant productive TST forms in-match).
# .l and every other EA keep the generic path. At TIMING_ACCURACY==0 the
# generic byte/word path charges 4 (entry) + 8 (ea_101), consulted from no
# cycle table, so the flat 12 below is exact; at >=1 the charges are the same
# for these forms (the singleop-table quirk only affects .l).
OP(tst):
    andi t0, opcode, 0x38
    xori t0, 0x28
    bnez t0, tst_slow                  # only (d16,An)
    andi t0, opcode, 0xC0
    beq t0, 0x80, tst_slow             # .l stays generic
    beq t0, 0xC0, tst_slow             # TAS stays generic
    andi t1, opcode, 7
    sll t1, 2
    add t1, ctx
    lw eaptr32, M64K_OFF_AREGS(t1)
    lh t2, 0(m_pc)
    addu eaptr32, t2
    beqz t0, tst_f_byte
    check_addr_bail eaptr32, tst_slow  # word: odd address raises via generic path
    addiu m_pc, 2                      # ---- tst.w (d16,An)  (12 cycles) ----
    map_m68k eaptr, eaptr32
    use_cycles 12
    lhu_m68k t0, eaptr
    move flag_zc, t0
    sll flag_nv, t0, 16
    dispatch_next
tst_f_byte:
    addiu m_pc, 2                      # ---- tst.b (d16,An)  (12 cycles) ----
    map_m68k eaptr, eaptr32
    use_cycles 12
    lbu_m68k t0, eaptr
    move flag_zc, t0
    sll flag_nv, flag_zc, 24
    dispatch_next

tst_slow:
#else
OP(tst):
#endif
    srl rmw_bitsize, opcode, 6
    andi rmw_bitsize, 3
    beq rmw_bitsize, 3, op_tas
    set_rmw_cyctable rmw_cyctable_singleop
    use_cycles 4
    jal decode_ea
    beqz rmw_bitsize, tst_byte
    check_addr_error eaptr32, read_address_error
    addi rmw_bitsize, -1
    beqz rmw_bitsize, tst_word
tst_long:
    lw_m68k t0, eaptr
    #if M64K_CONFIG_TIMING_ACCURACY >= 1
    .set noreorder
    .set noat
    la $1, rmw_cyctable_singleop
    beql rmw_cyctable, $1, 1f        # if rmw_cyctable wasn't changed...
    sub m_cycles, 4                  # it is a EA-mem, so we need 4 more cycles for 32-bit accesses
1:  .set reorder
    #endif
    move flag_nv, t0
    zx64 flag_zc, t0
    j main_loop
tst_word:
    lhu_m68k t0, eaptr
    move flag_zc, t0
    sll flag_nv, t0, 16
    j main_loop
tst_byte:
    lbu_m68k t0, eaptr
    move flag_zc, t0
    sll flag_nv, flag_zc, 24
    j main_loop

#if M64K_CONFIG_TIMING_ACCURACY >= 1
cyc_movel_penalty:
    use_cycles_opmask 6<<6, 0, -4      # if Ea-dst is Dn/An, then 4 cycles faster
    use_cycles_opmask 6<<3, 0, -4      # if Ea-src is Dn/An, then 4 cycles faster
cyc_move_penalty:
    use_cycles_opmask 7<<6, 4<<6, -2      # -(An) in EA-dst is 2 cycle faster than expected
    jr ra
#endif

#ifdef M64K_FASTPATHS
# MOVE.b fast paths: (d16,An)->Dn is the single hottest MOVE form in-match,
# (An)+->Dn the second. Byte accesses cannot raise address errors, so no
# ADDRERR bail is needed; the A7 byte quirk (+2) keeps A7 on the generic path.
OP(moveb):
    andi t0, opcode, 0x1C0             # destination mode field
    bnez t0, moveb_slow
    andi t0, opcode, 0x38
    xori t1, t0, 0x28
    beqz t1, moveb_f_d16
    xori t1, t0, 0x18
    bnez t1, moveb_slow
    andi t1, opcode, 7                 # ---- move.b (An)+,Dm  (8 cycles) ----
    xori t2, t1, 7
    beqz t2, moveb_slow                # A7: +2 quirk stays generic
    sll t1, 2
    add t1, ctx
    lw eaptr32, M64K_OFF_AREGS(t1)
    addiu t2, eaptr32, 1
    sw t2, M64K_OFF_AREGS(t1)          # writeback before the load, like ea_011
    map_m68k eaptr, eaptr32
    use_cycles 8
    lbu_m68k t0, eaptr
    srl t1, opcode, 7
    andi t1, 0x7<<2
    add t1, ctx
    sb t0, M64K_OFF_DREGS+3(t1)
    move flag_zc, t0
    sll flag_nv, t0, 24
    dispatch_next


moveb_f_d16:                           # ---- move.b (d16,An),Dm  (12 cycles) ----
    andi t1, opcode, 7
    sll t1, 2
    add t1, ctx
    lw eaptr32, M64K_OFF_AREGS(t1)
    lh t2, 0(m_pc)
    addiu m_pc, 2
    addu eaptr32, t2
    map_m68k eaptr, eaptr32
    use_cycles 12
    lbu_m68k t0, eaptr
    srl t1, opcode, 7
    andi t1, 0x7<<2
    add t1, ctx
    sb t0, M64K_OFF_DREGS+3(t1)
    move flag_zc, t0
    sll flag_nv, t0, 24
    dispatch_next

moveb_slow:
#else
OP(moveb):
#endif
    use_cycles 4
    #if M64K_CONFIG_TIMING_ACCURACY >= 1
    jal cyc_move_penalty
    #endif
    li opsize, 1                          # opsize is byte
    jal decode_ea                         # decode source EA
    lbu_m68k t0, eaptr                    # load source byte
    move result, t0
    srl opcode, 3                         # shift EA-dst-mode into EA-mode
    srl t1, opcode, 6                     # extract EA-dst-reg
    andi t1, 7
    jal decode_ea_customreg               # decode EA-dst
    sb_m68k result, eaptr                 # store byte
    move flag_zc, result                  # affect Z, clear C
    sll flag_nv, result, 24               # addect N, clear V
    j main_loop

#ifdef M64K_FASTPATHS
# MOVE.w fast paths for the profile-dominant forms (samsho2 in-match:
# Dn->(d16,An)/(An)/(An)+/Dn, (d16,An)->Dn and #imm->(An) together are ~60%
# of all MOVE.w). Each form decodes inline, skipping both decode_ea indirect
# calls. Cycle charges are identical to the generic path and are applied
# BEFORE the m68k-space data access so MMIO handlers observe the same
# mid-instruction clock. Anything unusual — other EA modes, MOVEA, the A7
# post-increment quirk, odd addresses on ADDRERR builds — bails to the
# generic implementation below with no architectural state mutated.
OP(movew):
    andi t0, opcode, 0x38
    bnez t0, movew_fsrc_other
    andi t1, opcode, 7                 # ---- source Dn: value into t0 ----
    sll t1, 2
    add t1, ctx
    lhu t0, M64K_OFF_DREGS+2(t1)
    srl t1, opcode, 6                  # destination mode (bits 8:6)
    andi t1, 7
    beqz t1, movew_f_dst_dn
    xori t2, t1, 2
    beqz t2, movew_f_dst_an
    xori t2, t1, 3
    beqz t2, movew_f_dst_anp
    xori t2, t1, 5
    beqz t2, movew_f_dst_d16
    j movew_slow

movew_f_dst_dn:                        # move.w Dn,Dm  (4 cycles)
    srl t1, opcode, 7
    andi t1, 0x7<<2
    add t1, ctx
    sh t0, M64K_OFF_DREGS+2(t1)
    move flag_zc, t0                   # !Z = value, C cleared
    sll flag_nv, t0, 16                # N from bit 15, V cleared
    use_cycles 4
    dispatch_next

movew_f_dst_an:                        # move.w Dn,(An)  (8 cycles)
    srl t1, opcode, 7
    andi t1, 0x7<<2
    add t1, ctx
    lw eaptr32, M64K_OFF_AREGS(t1)
    check_addr_bail eaptr32, movew_slow
    map_m68k eaptr, eaptr32
    move result, t0
    move flag_zc, result
    sll flag_nv, result, 16
    use_cycles 8
    port_store_check
    sh_m68k result, eaptr
    dispatch_next

movew_f_dst_anp:                       # move.w Dn,(An)+  (8 cycles)
    srl t1, opcode, 7
    andi t1, 0x7<<2
    xori t2, t1, 7<<2
    beqz t2, movew_slow                # A7 keeps its word-align quirk generic
    add t1, ctx
    lw eaptr32, M64K_OFF_AREGS(t1)
    check_addr_bail eaptr32, movew_slow
    addiu t2, eaptr32, 2
    sw t2, M64K_OFF_AREGS(t1)          # writeback before the store, like ea_011
    map_m68k eaptr, eaptr32
    move result, t0
    move flag_zc, result
    sll flag_nv, result, 16
    use_cycles 8
    sh_m68k result, eaptr
    dispatch_next

movew_f_dst_d16:                       # move.w Dn,(d16,An)  (12 cycles)
    srl t1, opcode, 7
    andi t1, 0x7<<2
    add t1, ctx
    lw eaptr32, M64K_OFF_AREGS(t1)
    lh t2, 0(m_pc)
    addu eaptr32, t2
    check_addr_bail eaptr32, movew_slow
    addiu m_pc, 2
    map_m68k eaptr, eaptr32
    move result, t0
    move flag_zc, result
    sll flag_nv, result, 16
    use_cycles 12
    port_store_check
    sh_m68k result, eaptr
    dispatch_next

movew_fsrc_other:
    xori t1, t0, 0x28                  # source (d16,An)?
    beqz t1, movew_fsrc_d16
    xori t1, t0, 0x18                  # source (An)+?
    beqz t1, movew_fsrc_anp
    xori t1, t0, 0x38                  # source mode 111?
    bnez t1, movew_slow
    andi t1, opcode, 7
    xori t1, 4                         # submode #imm?
    bnez t1, movew_slow
    srl t1, opcode, 6
    andi t1, 7
    xori t2, t1, 5                     # #imm -> (d16,An)?
    beqz t2, movew_imm_d16
    xori t1, 2                         # else only ->(An) is fast for #imm
    bnez t1, movew_slow
    srl t1, opcode, 7                  # ---- move.w #imm,(An)  (12 cycles) ----
    andi t1, 0x7<<2
    add t1, ctx
    lw eaptr32, M64K_OFF_AREGS(t1)
    check_addr_bail eaptr32, movew_slow
    lhu t0, 0(m_pc)                    # immediate word (code stream, cannot fault)
    addiu m_pc, 2
    map_m68k eaptr, eaptr32
    move result, t0
    move flag_zc, result
    sll flag_nv, result, 16
    use_cycles 12
    port_store_check
    sh_m68k result, eaptr
    dispatch_next

movew_imm_d16:                         # ---- move.w #imm,(d16,An)  (16 cycles) ----
    # samsho2 drives the LSPC modulo/data ports this way in its sprite loops
    # (move.w #$40,($2,A4) / #1,($2,A4)): ~200 port traps per fight frame.
    # Extension words in decode order: immediate, then d16. The address is
    # parity-checked before m_pc moves, so a bail re-executes generically.
    lw t2, %gprel(m64k_fp_imm_d16)(gp)
    beqz t2, movew_slow                # OFF twin: the generic path
    srl t1, opcode, 7
    andi t1, 0x7<<2
    add t1, ctx
    lw eaptr32, M64K_OFF_AREGS(t1)
    lh t2, 2(m_pc)                     # d16 (second extension word)
    addu eaptr32, t2
    check_addr_bail eaptr32, movew_slow
    lhu t0, 0(m_pc)                    # immediate word (code stream, cannot fault)
    addiu m_pc, 4
    map_m68k eaptr, eaptr32
    move result, t0
    move flag_zc, result
    sll flag_nv, result, 16
    use_cycles 16
    port_store_check
    sh_m68k result, eaptr
    dispatch_next

movew_fsrc_anp:
    srl t1, opcode, 6                  # destination mode
    andi t1, 7
    beqz t1, movew_fanp_dn
    xori t2, t1, 2
    bnez t2, movew_slow
    # ---- move.w (An)+,(An)  (wave 3; 12 cycles: 8 before the read, +4
    # before the write, matching entry+ea_011 / ea_010). The source
    # post-increment commits before the load like ea_011, and the
    # destination An is re-read after that writeback so the same-register
    # case matches the generic src-then-dst decode order. Both addresses
    # are parity-checked before any mutation (same-reg dst = src+2 shares
    # the source's parity). No A7 quirk for word size.
    andi t1, opcode, 7
    sll t1, 2
    add t1, ctx
    lw eaptr32, M64K_OFF_AREGS(t1)
    check_addr_bail eaptr32, movew_slow
    srl t2, opcode, 7
    andi t2, 0x7<<2
    add t2, ctx
    lw t3, M64K_OFF_AREGS(t2)
    check_addr_bail t3, movew_slow
    addiu t0, eaptr32, 2
    sw t0, M64K_OFF_AREGS(t1)          # src post-increment (before load, like ea_011)
    map_m68k eaptr, eaptr32
    use_cycles 8
    lhu_m68k t0, eaptr
    lw eaptr32, M64K_OFF_AREGS(t2)     # dst An AFTER the src writeback
    map_m68k eaptr, eaptr32
    move result, t0
    move flag_zc, result
    sll flag_nv, result, 16
    use_cycles 4
    port_store_check
    sh_m68k result, eaptr
    dispatch_next
#ifdef M64K_PORTSTORE
# port_store_check target: t1 = port offset (0 address, 2 data, 4 modulo),
# result = the value the trapping sh would have stored (hw_n64.S `value`).
# Same updates, in the same order, as asm_lspc_vram_{data,addr,modulo}_w.
m64k_port_store:
    xori t2, t1, 2
    bnez t2, 1f
    lhu t1, %gprel(reg_vram_addr)(gp)  # ---- data port (hottest) ----
    lw t2, %gprel(reg_vram_bank)(gp)
    addu t2, t1
    addu t2, t1
    sh result, 0(t2)
    lhu t2, %gprel(reg_vram_mod)(gp)
    addu t1, t2
    lhu t2, %gprel(reg_vram_mask)(gp)
    and t1, t2
    sh t1, %gprel(reg_vram_addr)(gp)
    dispatch_next
1:  bnez t1, 4f                        # ---- address port ----
    andi t2, result, 0x8000
    bnez t2, 2f
    la t2, VIDEO_RAM
    li t1, 0x7FFF
    j 3f
2:  la t2, VIDEO_RAM + 0x8000*2
    li t1, 0x7FF
3:  sw t2, %gprel(reg_vram_bank)(gp)
    sh t1, %gprel(reg_vram_mask)(gp)
    and t1, result
    sh t1, %gprel(reg_vram_addr)(gp)
    dispatch_next
4:  sh result, %gprel(reg_vram_mod)(gp)  # ---- modulo port ----
    dispatch_next

# MOVE.l to 0x3C0000 (movel_slow): what the SWL+SWR trap pair does - the
# high word to the address port (asm_lspc_vram_addr_w), then the low word
# to the data port (asm_lspc_vram_data_w), in that order.
m64k_port_store_l:
    srl t0, result, 16                 # SWL value: the high word
    andi t2, t0, 0x8000
    bnez t2, 1f
    la t2, VIDEO_RAM
    li t1, 0x7FFF
    j 2f
1:  la t2, VIDEO_RAM + 0x8000*2
    li t1, 0x7FF
2:  sw t2, %gprel(reg_vram_bank)(gp)
    sh t1, %gprel(reg_vram_mask)(gp)
    and t1, t0                         # t1 = new VRAM address, t2 = bank
    sh t1, %gprel(reg_vram_addr)(gp)
    addu t2, t1                        # data port with the low word
    addu t2, t1
    sh result, 0(t2)
    lhu t2, %gprel(reg_vram_mod)(gp)
    addu t1, t2
    lhu t2, %gprel(reg_vram_mask)(gp)
    and t1, t2
    sh t1, %gprel(reg_vram_addr)(gp)
    j main_loop
#endif
movew_fanp_dn:                         # ---- move.w (An)+,Dn  (8 cycles) ----
    andi t1, opcode, 7
    sll t1, 2
    add t1, ctx
    lw eaptr32, M64K_OFF_AREGS(t1)
    check_addr_bail eaptr32, movew_slow
    addiu t2, eaptr32, 2
    sw t2, M64K_OFF_AREGS(t1)          # writeback before the load, like ea_011
    map_m68k eaptr, eaptr32
    use_cycles 8
    lhu_m68k t0, eaptr
    srl t1, opcode, 7
    andi t1, 0x7<<2
    add t1, ctx
    sh t0, M64K_OFF_DREGS+2(t1)
    move flag_zc, t0
    sll flag_nv, t0, 16
    dispatch_next

movew_fsrc_d16:                        # ---- move.w (d16,An),Dm  (12 cycles) ----
    srl t1, opcode, 6
    andi t1, 7
    bnez t1, movew_slow                # only ->Dn is fast
    andi t1, opcode, 7
    sll t1, 2
    add t1, ctx
    lw eaptr32, M64K_OFF_AREGS(t1)
    lh t2, 0(m_pc)
    addu eaptr32, t2
    check_addr_bail eaptr32, movew_slow
    addiu m_pc, 2
    map_m68k eaptr, eaptr32
    use_cycles 12
    lhu_m68k t0, eaptr
    srl t1, opcode, 7
    andi t1, 0x7<<2
    add t1, ctx
    sh t0, M64K_OFF_DREGS+2(t1)
    move flag_zc, t0
    sll flag_nv, t0, 16
    dispatch_next

movew_slow:
#else
OP(movew):
#endif
    use_cycles 4
    #if M64K_CONFIG_TIMING_ACCURACY >= 1
    jal cyc_move_penalty
    #endif
    li opsize, 2
    jal decode_ea
    check_addr_error eaptr32, read_address_error
    and t1, opcode, 0x1C0
    lhu_m68k t0, eaptr              # read word
    beq t1, 0x040, op_movea16
    move result, t0
    move flag_zc, result            # this is already good as flag_zc (as C is always cleared)
    sll flag_nv, result, 16         # flag_nv is the sign extension of the 16 bit value
    srl opcode, 3                   # move the EA dst mode into the EA src mode field
    srl t1, opcode, 6               # decode the EA dst register
    andi t1, 7
    jal decode_ea_customreg         # decode dst EA
    check_addr_error eaptr32, write_address_error
    sh_m68k result, eaptr           # store the word into the destination
    j main_loop

#ifdef M64K_FASTPATHS
# MOVE.l/MOVEA.l fast paths: reg->reg copies (Dn/An -> Dn/An) and the hot
# (d16,An) source. NOTE: at TIMING_ACCURACY==0 the generic path charges a
# flat 12 at entry for every MOVE.l (the reg-direct -4/-4 penalties only
# exist at accuracy>=1), so the reg->reg forms below charge 12 to stay
# bit-identical to the generic path, not the hardware's 4.
OP(movel):
    andi t0, opcode, 0x38
    xori t1, t0, 0x28
    beqz t1, movel_fsrc_d16
    andi t1, t0, 0x30                  # source mode 000/001 (Dn/An)?
    bnez t1, movel_slow
    andi t1, opcode, 0xF               # D0-D7/A0-A7 as one register file
    sll t1, 2
    add t1, ctx
    lw t0, M64K_OFF_DREGS(t1)          # 32-bit value, sign-extended
    srl t1, opcode, 6                  # destination mode
    andi t1, 7
    beqz t1, movel_f_dst_dn
    xori t2, t1, 1
    bnez t2, movel_slow
    srl t1, opcode, 7                  # ---- movea.l Dn/An,Am: no flags ----
    andi t1, 0x7<<2
    add t1, ctx
    sw t0, M64K_OFF_AREGS(t1)
    use_cycles 12
    dispatch_next
movel_f_dst_dn:                        # ---- move.l Dn/An,Dm ----
    srl t1, opcode, 7
    andi t1, 0x7<<2
    add t1, ctx
    sw t0, M64K_OFF_DREGS(t1)
    move flag_nv, t0                   # sign-extended: N in bits 31 and 63, V=0
    zx64 flag_zc, flag_nv
    use_cycles 12
    dispatch_next

movel_fsrc_d16:                        # ---- move(a).l (d16,An),Dm/Am  (20 cycles) ----
    srl t1, opcode, 6
    andi t1, 7
    xori t2, t1, 1
    beqz t2, 1f                        # dst An ok
    bnez t1, movel_slow                # else only dst Dn
1:  andi t2, opcode, 7
    sll t2, 2
    add t2, ctx
    lw eaptr32, M64K_OFF_AREGS(t2)
    lh t2, 0(m_pc)
    addu eaptr32, t2
    check_addr_bail eaptr32, movel_slow
    addiu m_pc, 2
    map_m68k eaptr, eaptr32
    use_cycles 20
    lwl t0, 0(eaptr)                   # unaligned-capable pair, like lw_m68k
    lwr t0, 3(eaptr)
    bnez t1, movel_f16_an
    srl t1, opcode, 7
    andi t1, 0x7<<2
    add t1, ctx
    sw t0, M64K_OFF_DREGS(t1)
    move flag_nv, t0
    zx64 flag_zc, flag_nv
    dispatch_next
movel_f16_an:
    srl t1, opcode, 7
    andi t1, 0x7<<2
    add t1, ctx
    sw t0, M64K_OFF_AREGS(t1)
    dispatch_next

movel_slow:
#else
OP(movel):
#endif
    use_cycles 12
    #if M64K_CONFIG_TIMING_ACCURACY >= 1
    jal cyc_movel_penalty
    #endif
    li opsize, 4
    jal decode_ea
    check_addr_error eaptr32, read_address_error
    and t1, opcode, 0x1C0
    lw_m68k t0, eaptr               # read unaligned 32-bit word (sign-extended)
    move result, t0
    beq t1, 0x040, op_movea32
    move flag_nv, result            # It is already good as flag_nv, so store it there
    zx64 flag_zc, flag_nv           # flag_zc is the zero-extended copy (as C is always cleared)
    srl opcode, 3
    srl t1, opcode, 6
    andi t1, 7
    jal decode_ea_customreg
    check_addr_error eaptr32, write_address_error
    andi t0, opcode, 7<<3           # isolate EA-destination mode
    xori t0, 4<<3                   # check if it's pre-decrement mode
    beqz t0, 1f                     # if so, write memory in reversed order
#ifdef M64K_PORTSTORE
    lw t1, %gprel(m64k_portl_base)(gp)
    beq eaptr, t1, m64k_port_store_l  # MOVE.l to 0x3C0000 (standard order only)
#endif
    .set noreorder
    swl result, 0(eaptr)            # standard order: SWL+SWR
    j main_loop
1:  swr result, 3(eaptr)            # reversed order: SWR+SWL
    j main_loop
    swl result, 0(eaptr)
    .set reorder

OP(moveq):
    use_cycles 4
    sll flag_nv, opcode, 24
    sra flag_nv, 24
    andi flag_zc, opcode, 0xFF
    srl t1, opcode, 7
    andi t1, 0x7 << 2
    sw_ctx flag_nv, M64K_OFF_DREGS,t1
    j main_loop

OP(movea16):
    lh_m68k t0, eaptr
    move result, t0
op_movea32:
    srl t0, opcode, 7
    andi t0, 0x7<<2
    sw_ctx result, M64K_OFF_AREGS,t0
    j main_loop

    #define movem_mask        rmw_bitsize
    #define movem_op          rmw_op
    #define ea_reg            t1            // set by decode_ea
    #define reg_ptr           dptr
    #define reg_incr          t3
    #define eaptr32_diff      t2
    #define ctz_lut           opcode
    #define reg_incr_3x       s5

    #define MOVEM_4BIT        1            // Faster 4-bit-at-a-time movem

OP(movem_rm):  # reg -> mem
    andi t0, opcode, 7<<3
    beqz t0, op_ext
    andi t0, opcode, 1<<6                   # check if word/long
    use_cycles 4
    beqz t0, movem_rm16                     # jump to 16 bit variant setup
    li opsize, 4                            # 32-bit variant: opsize is 4
    lax movem_op, movem_rm_impl_32          # 32-bit variant: implementation function
    j movem_start
movem_rm16:
    li opsize, 2                            # 16-bit variant: opsize is 2
    lax movem_op, movem_rm_impl_16          # 16-bit variant: implementation function
    j movem_start

OP(movem_mr): # mem -> reg
    andi t0, opcode, 1<<6                   # check if word/long
    use_cycles 8
    beqz t0, movem_mr16                     # jump to 16 bit variant setup
    li opsize, 4
    lax movem_op, movem_mr_impl_32
    j movem_start
movem_mr16:
    li opsize, 2
    lax movem_op, movem_mr_impl_16

movem_start:  # Generic implementation (16/32-bit, mem->reg/reg->mem)
    lhu movem_mask, 0(m_pc)                 # fetch register mask
    addiu m_pc, 2
    jal decode_ea                           # decode EA
    la_ctx reg_ptr, M64K_OFF_DREGS-4       # Initialize pointer to register array (one slot before)
    li reg_incr, 4                          # Initialize register pointer increment (4 bytes)
    li reg_incr_3x, 12                      # Initialize 3x register pointer increment (12 bytes)
    andi t0, opcode, 7<<3                   # check if EA is predecrement or postincrement
    #if MOVEM_4BIT
    li ctz_lut, 0x67646764                  # Load CTZ table (WARN: reusing opcode register)
    #endif
    beq t0, 3<<3, movem_postincr            # jump to setup the postincrement variant
    beq t0, 4<<3, movem_predecr             # jump to setup the predecrement variant
    li ea_reg, -1                           # Otherwise (standard EA), no register writeback is needed
movem_pre_loop:
    check_addr_error eaptr32, read_address_error

#if MOVEM_4BIT
    # Main loop: 4-bit version. Uses a 4-bit based CTZ primitive
    # to process four bits at a time. This is slightly faster
    # than the 1-bit version (and much faster in worst cases).
movem_loop:
    .set noreorder
    andi t0, movem_mask, 1
    bnez t0, 1f
     addu reg_ptr, reg_incr
    srlv t0, ctz_lut, movem_mask
    andi t0, 3
    bnez t0, 2f
     srlv movem_mask, movem_mask, t0
    beqz movem_mask, movem_mr_end
     srl movem_mask, 4
    j movem_loop
     addu reg_ptr, reg_incr_3x
2:  bgez reg_incr, 3f
     sll t0, 2
    neg t0, t0
3:  addu reg_ptr, t0
1:  srl movem_mask, 1
    #if M64K_CONFIG_ADDR_WRAP
    jalr movem_op
     addu eaptr32, opsize
    .set reorder
    map_m68k eaptr, eaptr32
    j movem_loop
    #else
    .set reorder
    jalr movem_op
    addu eaptr, opsize
    j movem_loop
    #endif
#else
    # Main loop: 1-bit version. This is slightly slower than the
    # 4-bit version, but it is more compact and easier to understand.
movem_loop:                              # Main movem loop
    .set noreorder
    beqz movem_mask, movem_mr_end        # If the mask is empty, we are done
1:   andi t0, movem_mask, 1              # Check if bit 0 of the mask is set
    srl movem_mask, 1                    # Shift the mask
    beqz t0, 1b                          # If not, loop again
     addu reg_ptr, reg_incr              # Increment register pointer for each bit
    #if M64K_CONFIG_ADDR_WRAP
    jalr movem_op                        # Do one reg/mem transfer
     addu eaptr32, opsize                # Increment memory pointer (full 32-bit value
    .set reorder
    map_m68k eaptr, eaptr32              # Map the 32-bit memory pointer to the N64 address space
    j movem_loop                         # Loop again
    #else
    .set reorder
    jalr movem_op                        # Do one reg/mem transfer
    addu eaptr, opsize                   # Increment memory pointer
    j movem_loop                         # Loop again
    #endif
#endif

movem_mr_end:
    bltz ea_reg, main_loop                   # If no register writeback is needed, we are done
    addu eaptr32, eaptr, eaptr32_diff        # Calculate the new 32-bit address given the current pointer
    sw_ctx eaptr32, M64K_OFF_AREGS,ea_reg   # Write back the new address to the address register
    j main_loop

movem_predecr:                               # Predecrement variant
    use_cycles -2
    addu t0, eaptr32, opsize                 # Revert the predecrement in the address register
    sw_ctx t0, M64K_OFF_AREGS,t1            #  (beheavior specific of M68000)
    subu eaptr32_diff, t0, eaptr             # Calculate the difference between the 32-bit address and the pointer
    neg opsize                               # Negate opsize, so that we can use it to decrement the memory location
    la_ctx reg_ptr, M64K_OFF_AREGS+8*4      # Initialize pointer to register array (one slot after)
    neg reg_incr                             # Negate register pointer increment (-4 bytes)
    neg reg_incr_3x                          # Negate 3x register pointer increment (-12 bytes)
    j movem_pre_loop
movem_postincr:                              # Postincrement variant
    check_addr_error eaptr32, read_address_error  # Check if the post-incremented address is valid
    subu eaptr32_diff, eaptr32, eaptr        # Calculate the difference between the 32-bit address and the pointer
    subu t0, eaptr32, opsize                 # Rever the post-increment in the address register
    sw_ctx t0, M64K_OFF_AREGS,t1            #  (beheavior specific of M68000)
    j movem_loop


movem_mr_impl_16:
    lh_m68k t0, eaptr
    use_cycles 4
    sw t0, 0(reg_ptr)
    jr ra
movem_mr_impl_32:
    lw_m68k t0, eaptr
    use_cycles 8
    sw t0, 0(reg_ptr)
    jr ra
movem_rm_impl_16:
    lh result, 2(reg_ptr)
    use_cycles 4
    sh_m68k result, eaptr
    jr ra
movem_rm_impl_32:
    lw result, 0(reg_ptr)
    use_cycles 8
    sw_m68k result, eaptr
    jr ra

    #undef movem_mask
    #undef movem_op
    #undef ea_reg
    #undef reg_ptr
    #undef reg_incr
    #undef eaptr32_diff
    #undef ctz_lut
    #undef reg_incr_3x


OP(mvfsr):
    use_cycles_opmask 6<<3, 0, -2
    # MOVE from SR is NON-privileged on the 68000 (privilege was added in the
    # 68010). NeoGeo is a plain 68000, so do not check supervisor here.
    li opsize, 2
    jal decode_ea
    check_addr_error eaptr32, read_address_error
    use_cycles 8
    jal flush_sr
    lh_m68k t0, eaptr               # dummy read before write
    move result, t1
    sh_m68k result, eaptr
    j main_loop

OP(mvccr):
    li opsize, 2
    jal decode_ea
    check_addr_error eaptr32, read_address_error
    use_cycles 12
    jal flush_sr
    lw_ctx t1, M64K_OFF_SR
    lhu_m68k t0, eaptr
    and t1, SR_MASK ^ CCR_MASK
    and t0, CCR_MASK
    or t0, t1
    sw_ctx t0, M64K_OFF_SR
    jal_and_j reload_sr, main_loop

OP(mvtsr):
    check_supervisor
    li opsize, 2
    jal decode_ea
    check_addr_error eaptr32, read_address_error
    use_cycles 12
    jal flush_sr
    lhu_m68k t0, eaptr
    and t0, SR_MASK
    sw_ctx t0, M64K_OFF_SR
    jal_and_j reload_sr, main_loop

OP(rmw_sr_imm):
    andi t0, opcode, 1<<6           # check if the access is just to CCR or the full SR
    use_cycles 20
    bnez t0, rmw_sr_full
    li rmw_srmask, CCR_MASK         # use CCR mask to only modify CCR bits
    j rmw_sr_start
rmw_sr_full:
    check_supervisor                # SR access requires supervisor mode
    li rmw_srmask, SR_MASK          # use SR mask to modify all implemented bits in SR
rmw_sr_start:
    add m_pc, 2
    jal flush_sr                    # sync SR with current values of flags / stack pointer
    lhu t0, -2(m_pc)                # read immediate value
    lw_ctx t1, M64K_OFF_SR         # load SR
    jal rmw_op                      # execute operation
    sw_ctx t0, M64K_OFF_SR         # store modified SR
    jal_and_j reload_sr, main_loop  # reload flags / stack pointer

OP(andi_sr):
    lax rmw_op, op_andi_sr_impl
    j op_rmw_sr_imm
op_andi_sr_impl:
    not rmw_srmask
    or t0, rmw_srmask
    and t0, t1
    jr ra
OP(ori_sr):
    lax rmw_op, op_ori_sr_impl
    j op_rmw_sr_imm
op_ori_sr_impl:
    and t0, rmw_srmask
    or t0, t1
    jr ra
OP(eori_sr):
    lax rmw_op, op_eori_sr_impl
    j op_rmw_sr_imm
op_eori_sr_impl:
    and t0, rmw_srmask
    xor t0, t1
    jr ra

OP(ext):
    use_cycles 4
    andi dptr, opcode, 7                 # extract dreg index
    sll dptr, 2
    andi t0, opcode, 1<<6                # opcode bit 6: 1=word, 0=byte
    la_ctx dptr, M64K_OFF_DREGS,dptr    # compute dreg address
    bnez t0, ext_long
ext_word:
    lb flag_nv, 3(dptr)                  # extend byte to word
    sh flag_nv, 2(dptr)
    zx64 flag_zc, flag_nv
    j main_loop
ext_long:
    lh flag_nv, 2(dptr)                  # extend word to long
    sw flag_nv, 0(dptr)
    zx64 flag_zc, flag_nv
    j main_loop

OP(swap):
    andi dptr, opcode, 7                 # extract dreg index
    sll dptr, 2
    la_ctx dptr, M64K_OFF_DREGS,dptr    # compute dreg address
    use_cycles 4
    lh t0, 0(dptr)
    lh flag_nv, 2(dptr)
    sh t0, 2(dptr)
    sh flag_nv, 0(dptr)
    lwu flag_zc, 0(dptr)
    j main_loop

OP(mvusp):
    use_cycles 4
    check_supervisor
    andi t0, opcode, 8         # DR bit
    sll t1, opcode, 2          # Isolate register offset
    andi t1, 0x7<<2
    bnez t0, mvusp_read        # See if it's read or write
    lw_ctx t2, M64K_OFF_AREGS,t1
    sw_ctx t2, M64K_OFF_USP
    j main_loop
mvusp_read:
    lw_ctx t2, M64K_OFF_USP
    sw_ctx t2, M64K_OFF_AREGS,t1
    j main_loop

#if M64K_CONFIG_TIMING_ACCURACY >= 1
cyc_leapea_penalty:
    use_cycles_opmask 7<<3, 6<<3, 2            # d(An,ix) takes 2 extra cycles
    use_cycles_opmask 0b111111, 0b111011, 2    # d(PC,ix) takes 2 extra cycles
    jr ra
#endif

OP(pea):
    andi t0, opcode, 1<<7
    bnez t0, op_movem_rm
    andi t0, opcode, 1<<6
    beqz t0, op_nbcd
    andi t0, opcode, 0b111000
    beqz t0, op_swap
    use_cycles 8
    #if M64K_CONFIG_TIMING_ACCURACY >= 1
    jal cyc_leapea_penalty
    #endif
    li opsize, 2
    jal decode_ea
    lw_ctx t0, M64K_OFF_AREGS+7*4
    addiu t0, -4
    check_addr_error t0, write_address_error
    map_m68k t1, t0
    move result, eaptr32
    sw_m68k result, t1
    sw_ctx t0, M64K_OFF_AREGS+7*4
    j main_loop

OP(lea):
    andi t0, opcode, 1<<6
    beqz t0, op_chk
    #if M64K_CONFIG_TIMING_ACCURACY >= 1
    jal cyc_leapea_penalty
    #endif
    li opsize, 2
    jal decode_ea
    srl t1, opcode, 7
    andi t1, 0x7<<2
    sw_ctx eaptr32, M64K_OFF_AREGS,t1
    j main_loop

ALIGN(push32):
    li t0, -4
pushn:
    lw_ctx t1, M64K_OFF_AREGS+7*4
    addu t1, t0
    sw_ctx t1, M64K_OFF_AREGS+7*4
    check_addr_error t1, write_address_error
    map_m68k t0, t1
    jr ra

# Pop from the stack:
#
# * pop16: pop 16 bits
# * pop32: pop 32 bits
# * popn:  pop the number of bytes in t0
#
# The function returns the address of the stack slot in t0. The caller
# must perform the actual read.
ALIGN(pop16):
    li t0, 2
    j popn
pop32:
    li t0, 4
popn:
    lw_ctx t1, M64K_OFF_AREGS+7*4       # read SP
    check_addr_error t1, read_address_error
    .set noat
    addu $1, t1, t0                      # SP += 2/4
    map_m68k t0, t1
    sw_ctx $1, M64K_OFF_AREGS+7*4       # write SP
    .set at
    jr ra

OP(link):
    andi t2, opcode, 7                 # extract An from opcode
    sll t2, 2
    la_ctx t2, M64K_OFF_AREGS,t2      # compute An pointer

    andi t1, opcode, 1<<3
    bnez t1, op_unlk

    jal push32
    lw result, 0(t2)    # read An
    sw_m68k result, t0  # An -> (SP)
    sw t1, 0(t2)        # SP -> An

    lh t2, 0(m_pc)  # read displacement
    addiu m_pc, 2
    addu t1, t2     # SP += disp
    sw_ctx t1, M64K_OFF_AREGS+7*4
    use_cycles 16
    j main_loop

OP(unlk):
    lw t1, 0(t2)                         # read An
    use_cycles 12
    sw_ctx t1, M64K_OFF_AREGS+7*4       # An -> SP
    jal pop32
    lwl t3, 0(t0)                        # read (SP)
    lwr t3, 3(t0)                        # read (SP)
    sw t3, (t2)                          # (SP) -> An
    j main_loop

OP(jmp):
    jal decode_ea
jmp_exec_cycles:
    use_cycles 2
    #if M64K_CONFIG_TIMING_ACCURACY >= 1
    use_cycles_opmask 0b111000, 0b010000, 2
    use_cycles_opmask 0b111111, 0b111001, -2
    jal cyc_leapea_penalty
    #endif
    .globl jmp_exec
jmp_exec:
    map_m68k m_pc, eaptr32             # map 32-bit target to host pointer
    subu t0, eaptr32, m_pc             # save new pc_diff
    sw t0, pc_diff
    check_addr_error eaptr32, pc_address_error
#ifdef MVS64_IDLEPROBE
    lw t0, idle_probe_last
    beq t0, m_pc, 1f                   # same branch target as last time?
    sw m_pc, idle_probe_last           # no: new target, reset run length
    sw zero, idle_probe_cnt
    j 2f
1:  lw t0, idle_probe_cnt
    addiu t0, 1
    sw t0, idle_probe_cnt
    li t1, 4000                        # 4000 consecutive = a real wait-for-IRQ spin
    bne t0, t1, 2f                     #   (a bounded DBRA/memcpy loop never reaches this)
    sw m_pc, idle_probe_found          # record the spinning PC for the C side to print
2:
#endif
    # samsho2 vblank-wait spins: "clr.b $100A30; <here>: tst.b $100A30; beq <here>"
    # — pure side-effect-free polls of a flag the game's VBlank IRQ sets. Verified
    # via the in-match idle probe (0x00142C is the dominant in-game spin). Skipping
    # them fast-forwards the dead wait to the next scheduled IRQ. (See PLAN-OPTIMIZATION.md
    # rank 1/2; the general fix is a data-driven per-game table.)
    #
    # The constants MUST be materialized with lui (sign-extending) so their
    # 64-bit register value matches the map_m68k result: mmap_mask is loaded
    # with lui, so m_pc is the SIGN-extended 0xFFFFFFFF_FFxxxxxx. Writing
    # "beq m_pc, 0xFF00142C" makes GAS emit a ZERO-extended li+dsll+ori
    # (0x00000000_FF00142C), and the 64-bit beq can then never be equal —
    # which kept this whole skip list dead since it was introduced.
    #
    # Prefilter (m64k_idle_pagemask): one shift tests the target's 2KB page
    # against the pages that hold a chain PC; every other target (nearly
    # all of the ~2k transfers per fight frame) skips the 22-instruction
    # chain. sllv uses only the low 5 bits of t0 = (pc>>11)&31, moving
    # mask bit (31-p) to bit 31; the result is sign-extended, so bgez is
    # "page not listed". No false negatives: same skip set as the chain.
    .set noreorder
    lw t1, m64k_idle_pagemask
    srl t0, m_pc, 11
    sllv t1, t1, t0
#if defined(M64K_DYNREC) || defined(M64K_DYNSTAT)
    bgez t1, jmp_exec_nonidle
#else
    bgez t1, main_loop
#endif
    lui t0, 0xFF00                    # delay slot: the chain's base (dead on the skip path)
    .set reorder
    ori t1, t0, 0x142C
    beq m_pc, t1, idle_skip           # samsho2 main vblank wait (hottest)
    ori t1, t0, 0xFC02
    beq m_pc, t1, idle_skip           # samsho2 secondary vblank wait
    ori t1, t0, 0xFB4A
    beq m_pc, t1, idle_skip           # samsho2 in-fight vblank wait #3
    ori t1, t0, 0xFB74
    beq m_pc, t1, idle_skip           # samsho2 in-fight vblank wait #4
                                      # (both: tst.b $100A30; beq self —
                                      # found by MVS64_IDLEPROBE 2026-08-07,
                                      # byte-identical to the 0x142C shape)
    ori t1, t0, 0x1FE2
    beq m_pc, t1, idle_skip           # mslug
    lui t1, 0xFFC1
    ori t1, 0x8714
    beq m_pc, t1, idle_skip           # Universe BIOS vblank wait
jmp_exec_nonidle:
#ifdef M64K_DYNREC
    # Dynarec block-table probe, jmp_exec site (phase 1: unreachable hit
    # path — see the slice-entry probe). eaptr32 is the 32-bit guest target.
    srl t2, eaptr32, 1
    andi t2, (M64K_DYN_TABLE_SETS-1)
    sll t2, 4
    addu t2, dyn_tab
    lw t3, 0(t2)                       # way-0 tag
    beq t3, eaptr32, dyn_probe_hit
    lw t3, 8(t2)                       # way-1 tag
    beq t3, eaptr32, dyn_probe_hit1
    # Miss: note the target for the C-side translator (serviced at slice
    # boundaries). Slot 0 = last-writer-wins (biased to slice-end heads).
    # Slot 1 = every-61st-miss SAMPLE ([2] is the countdown): unbiased
    # rotation over the whole miss population, so hot mid-frame heads the
    # lottery never picks (the 00ceec/00cf2e upload loops, 80k misses/
    # window) get attempted proportionally to their miss share. (A
    # first-writer-when-empty design was tried first and WEDGES: the
    # first miss of every slice is the same recurring head, and if that
    # head is already tried-and-refused the frontier never advances.)
    #
    # Slot 1 is FIRST-UNTRIED-writer-wins (2026-09-22): a last-writer
    # sample slot is overwritten by the ~5 later samples of the same slice,
    # so a hot MID-slice head (the 00ceec/00cf2e upload loops, 30k misses
    # per window) never survived to the slice-end service — not once in
    # any DYNSTAT run. Known-refused heads are filtered here (exact
    # dyn_tried2 tag compare, dynrec.c), which is what made the earlier
    # first-writer attempt wedge on a recurring refused head.
    la t2, __m64k_dyn_mailbox
    sw eaptr32, 0(t2)
    lw t3, 8(t2)
    addiu t3, -1
    bgtz t3, 1f
    li t3, 61
    lw t0, 4(t2)
    bnez t0, 1f                        # slot taken this slice: keep it
    sll t0, eaptr32, 8
    srl t0, t0, 9                      # h = (pc & 0xFFFFFF) >> 1
    andi t1, t0, 8191
    sll t1, 1
    la t2, __m64k_dyn_tried2
    addu t1, t2
    lhu t1, 0(t1)
    srl t0, t0, 13
    ori t0, 0xC000                     # refused tag: valid|refused|h>>13
    la t2, __m64k_dyn_mailbox
    beq t0, t1, 1f                     # known-refused: never sample it
    sw eaptr32, 4(t2)
1:  sw t3, 8(t2)
#endif
#ifdef M64K_DYNSTAT
    jal dynstat_count                  # coverage rig: count this target PC
#endif
    j main_loop
idle_skip:
#ifdef MVS64_PERFCOUNT
    lw t0, perf_idle_skips
    addiu t0, 1
    sw t0, perf_idle_skips
#endif
    li m_cycles, 0
    j main_loop

OP(jsr):
    use_cycles 8
    jal decode_ea
jsr_exec:
    lw result, pc_diff
    addu result, m_pc
    jal push32
    sw_m68k result, t0
    j jmp_exec_cycles

OP(rts):
    use_cycles 16
    jal pop32
    lwl eaptr32, 0(t0)
    lwr eaptr32, 3(t0)
    j jmp_exec

#ifdef M64K_FASTPATHS
# RTS fast path (~3% of in-match instructions; wave 3). Inline pop32 +
# jmp_exec's map/pc_diff update, ending in dispatch_next. The jmp_exec
# spin-target compare list is skipped: the registered idle-spin PCs are
# beq loop heads, so even if an RTS returned into one, the loop's own Bcc
# routes through branch_exec and the skip still fires one iteration later.
# Charge is the generic flat 16, applied before the stack read like the
# generic path. Odd SP bails to the generic path with no state mutated
# (pop32 checks SP before its own writeback, so the replay is identical).
ALIGN(rts_fast):
    lw_ctx t1, M64K_OFF_AREGS+7*4
    check_addr_bail t1, op_rts
    use_cycles 16
    map_m68k t0, t1
    addiu t2, t1, 4
    sw_ctx t2, M64K_OFF_AREGS+7*4
    lwl eaptr32, 0(t0)
    lwr eaptr32, 3(t0)
    map_m68k m_pc, eaptr32
    subu t0, eaptr32, m_pc
    sw t0, pc_diff
    check_addr_error eaptr32, pc_address_error
    dispatch_next
#endif


OP(bsr):
    lb t3, -1(m_pc)                    # fetch signed 8-bit displacement (from opcode)
    bnez t3, bsr_exec                  # if displacement is 0, there's a 16-bit displacement
    lh t3, 0(m_pc)                     # fetch 16-bit displacement
    addiu m_pc, 2
    addiu t3, -2
bsr_exec:
    use_cycles 16
    #if M64K_CONFIG_TIMING_ACCURACY >= 1
    andi opcode, 0xFF00                # avoid timing adjustment made by jmp_exec_cycles later
    #endif
    lw eaptr32, pc_diff                # calculate full 32bit branch target (using pc_diff)
    addu eaptr32, t3
    addu eaptr32, m_pc
    j jsr_exec

#ifdef M64K_FASTPATHS
# Bcc fast path (hottest opcode class in-match): dispatch directly on the
# condition field (opcode bits 11:8) through bcc_cctable; each entry tests its
# condition with the sense baked in and branches straight to bcc_taken /
# bcc_nottaken. This removes the check_cc call/return pair and the generic
# invert logic. BSR (cond 1) routes through the same table to its original
# implementation. Displacement decode and cycle charges are identical to the
# generic path (not-taken: 8 for the 8-bit form, 12 for the 16-bit form;
# taken charges 10 in branch_exec). check_cc itself stays: DBcc/Scc use it.
OP(bcc):
    .set push
    .set noreorder
    srl t0, opcode, 6
    andi t0, 0x3C
    lw t0, bcc_cctable(t0)
    lb t3, -1(m_pc)                    # signed 8-bit displacement (0 => 16-bit follows)
    jr t0
    #if M64K_CONFIG_TIMING_ACCURACY >= 0
     li rmw_cyctable, 8                # not-taken charge for the 8-bit form
    #else
     nop
    #endif
    .set pop

bcct_hi:                               # HI: C==0 && Z==0
    dsrl t0, flag_zc, 32
    andi t0, 1
    bnez t0, bcc_nottaken
    zx64 t0, flag_zc
    bnez t0, bcc_taken
    j bcc_nottaken
bcct_ls:                               # LS: C==1 || Z==1
    dsrl t0, flag_zc, 32
    andi t0, 1
    bnez t0, bcc_taken
    zx64 t0, flag_zc
    beqz t0, bcc_taken
    j bcc_nottaken
bcct_cc:                               # CC: C==0
    dsrl t0, flag_zc, 32
    andi t0, 1
    beqz t0, bcc_taken
    j bcc_nottaken
bcct_cs:                               # CS: C==1
    dsrl t0, flag_zc, 32
    andi t0, 1
    bnez t0, bcc_taken
    j bcc_nottaken
bcct_ne:                               # NE: Z==0
    zx64 t0, flag_zc
    bnez t0, bcc_taken
    j bcc_nottaken
bcct_eq:                               # EQ: Z==1
    zx64 t0, flag_zc
    beqz t0, bcc_taken
    j bcc_nottaken
bcct_vc:                               # VC: V==0  (V = bit63(N^V) ^ bit31(N))
    dsrl t0, flag_nv, 63
    srl t1, flag_nv, 31
    xor t0, t1
    beqz t0, bcc_taken
    j bcc_nottaken
bcct_vs:                               # VS: V==1
    dsrl t0, flag_nv, 63
    srl t1, flag_nv, 31
    xor t0, t1
    bnez t0, bcc_taken
    j bcc_nottaken
bcct_pl:                               # PL: N==0
    srl t0, flag_nv, 31
    beqz t0, bcc_taken
    j bcc_nottaken
bcct_mi:                               # MI: N==1
    srl t0, flag_nv, 31
    bnez t0, bcc_taken
    j bcc_nottaken
bcct_ge:                               # GE: N==V  (bit63 of flag_nv is N^V)
    bgez flag_nv, bcc_taken
    j bcc_nottaken
bcct_lt:                               # LT: N!=V
    bltz flag_nv, bcc_taken
    j bcc_nottaken
bcct_gt:                               # GT: N==V && Z==0
    bltz flag_nv, bcc_nottaken
    zx64 t0, flag_zc
    bnez t0, bcc_taken
    j bcc_nottaken
bcct_le:                               # LE: N!=V || Z==1
    bltz flag_nv, bcc_taken
    zx64 t0, flag_zc
    beqz t0, bcc_taken
    j bcc_nottaken

bcc_taken:
    bnez t3, branch_exec               # 8-bit displacement: straight to the branch
    lh t3, 0(m_pc)                     # 0 => fetch the 16-bit displacement
    addiu m_pc, 2
    addiu t3, -2                       # displacement refers to PC before increment
    j branch_exec
bcc_nottaken:
    bnez t3, 1f
    addiu m_pc, 2                      # skip the unused 16-bit displacement word
    #if M64K_CONFIG_TIMING_ACCURACY >= 0
    li rmw_cyctable, 12                # not-taken charge for the 16-bit form
    #endif
1:
    #if M64K_CONFIG_TIMING_ACCURACY >= 0
    sub m_cycles, rmw_cyctable
    #endif
    j main_loop
#else
OP(bcc):
    andi t0, opcode, 0xFF00
    beq t0, 0x6100, op_bsr
    lb t3, -1(m_pc)                    # fetch signed 8-bit displacement (from opcode)
    #if M64K_CONFIG_TIMING_ACCURACY >= 0
    li rmw_cyctable, 8
    #endif
    bnez t3, bcc_exec                  # if displacement is 0, there's a 16-bit displacement
bcc_disp_16:
    lh t3, 0(m_pc)                     # fetch 16-bit displacement
    #if M64K_CONFIG_TIMING_ACCURACY >= 0
    li rmw_cyctable, 12
    #endif
    addiu m_pc, 2                      # increment PC
    addiu t3, -2                       # displacement refers to PC before increment
bcc_exec:
    jal check_cc
    .set noreorder
    beqzl t0, main_loop                # cc is false => branch is not taken
     #if M64K_CONFIG_TIMING_ACCURACY >= 0
     sub m_cycles, rmw_cyctable
     #else
     nop
     #endif
    .set reorder
#endif
branch_exec:
    lw eaptr32, pc_diff                # calculate full 32bit branch target (using pc_diff)
    addu eaptr32, t3
    addu eaptr32, m_pc
    use_cycles 10
    j jmp_exec

OP(dbcc):
#ifdef M64K_FASTPATHS
    # DBF (cond=false, i.e. DBRA) is by far the most common DBcc form and its
    # condition can never be true: skip the check_cc call and go straight to
    # the counter decrement.
    andi t0, opcode, 0x0F00
    xori t0, 0x0100
    bnez t0, dbcc_checkcc
    addiu m_pc, 2
    j dbcc_dec
dbcc_checkcc:
#endif
    addiu m_pc, 2
    jal check_cc
    #if M64K_CONFIG_TIMING_ACCURACY >= 0
    .set noreorder
    bnezl t0, main_loop
     use_cycles 12
    .set reorder
    #else
    bnez t0, main_loop
    #endif
dbcc_dec:
    andi t0, opcode, 0x7
    sll t0, 2
    la_ctx dptr, M64K_OFF_DREGS,t0
    lhu t0, 2(dptr)
    addiu t1, t0, -1
    sh t1, 2(dptr)
    #if M64K_CONFIG_TIMING_ACCURACY >= 0
    .set noreorder
    beqzl t0, main_loop
     use_cycles 14
    .set reorder
    #else
    beqz t0, main_loop
    #endif
    lh t3, -2(m_pc)
    addiu t3, -2
#ifdef M64K_BLOCKOPS
# Blockop rejection accounting (DYNSTAT diagnostic builds only): the fused
# VRAM-port copy has three guards, and a silent rejection is invisible in
# every other metric — the loop just runs interpreted. Route each guard to
# its own counting stub so [BOSTAT] says WHICH one declines.
#if defined(M64K_DYNSTAT) || defined(MVS64_BOSTAT)
#define BO_REJ_DST   bo_rej_dst
#define BO_REJ_BUD   bo_rej_bud
#define BO_REJ_SPAN  bo_rej_span
#define BO_REJ_SHAPE bo_rej_shape
#else
#define BO_REJ_DST   dbcc_no_blockop
#define BO_REJ_BUD   dbcc_no_blockop
#define BO_REJ_SPAN  dbcc_no_blockop
#define BO_REJ_SHAPE dbcc_no_blockop
#endif
    # Fused block-op fast path for single-instruction DBF copy/fill loops.
    #
    # LOOP-SHAPE LAW (fixed 2026-08-07; the guard was -4 and this path had
    # therefore NEVER fired — [BOSTAT] fire=0 over 293k taken DBFs/window,
    # 94% rejected right here). For the shape these charges are written
    # for (1-word body + 2-word DBF), with dispatch_body's "addi m_pc,2"
    # plus OP(dbcc)'s own, m_pc sits at body+6 when t3 is formed, so the
    # self-loop delta is -6 and the body opcode is at -6(m_pc). -4 instead
    # selects a DBF branching to its own opcode (a zero-body "dbra dn,*"
    # delay loop) whose body fetch reads back the DBF word 0x51C8 — which
    # is exactly what the diagnostic saw. The per-iteration charges below
    # (22 = move.w mem-to-mem 12 + taken dbra 10; 18 = move.w Dn,(An)+ 8
    # + 10) only balance for a REAL one-instruction body, so they are the
    # independent confirmation that -6 is the intended constant.
    # samsho2 spends several % of every frame in "move.w (As)+,(Ad)" uploads
    # to the LSPC VRAM data port (one TLB exception per word on N64) and in
    # "move.w/l Dn,(Ad)+" RAM fills, all shaped as 2-word DBRA self-loops.
    # Execute up to count-1 iterations natively with identical cycle charges;
    # the final (expiring) iteration and any budget tail always run through
    # the normal interpreter, so flags, PC and register state at every
    # observable boundary are bit-exact. Restricted to DBF (cc can never
    # become true mid-loop; other DBcc react to flags the body changes).
    bne t3, -6, BO_REJ_SHAPE           # 1-word body + 2-word DBF only
    andi t2, opcode, 0x0F00
    bne t2, 0x0100, dbcc_no_blockop    # DBF only
    lw t8, mvs64_blockop_enable        # runtime A/B gate
    beqz t8, dbcc_no_blockop
#ifdef M64K_DYNSTAT
    lhu t8, -6(m_pc)
    sw t8, blockop_t9val               # what the "body opcode" fetch sees
#endif
    addi t2, t0, -1                    # fast-loopable iterations (keep last)
    blez t2, dbcc_no_blockop
    lhu t9, -6(m_pc)                   # body opcode
    andi t1, t9, 0xF1F8
    beq t1, 0x3098, blockop_movew_port # move.w (As)+,(Ad)
    beq t1, 0x30C0, blockop_fill_w     # move.w Dn,(Ad)+
    beq t1, 0x20C0, blockop_fill_l     # move.l Dn,(Ad)+
    beq t1, 0x20D8, blockop_copy_l     # move.l (As)+,(Ad)+
    beq t1, 0x3080, blockop_fillw_port # move.w Dn,(Ad)
#if defined(M64K_DYNSTAT) || defined(MVS64_BOSTAT)
bo_rej_dst:
    lw t8, blockop_rej_dst
    addiu t8, 1
    sw t8, blockop_rej_dst
    sw t1, blockop_rej_dstval          # the destination that did not match
    lw t7, pc_diff                     # ...and WHICH loop it came from:
    addu t7, m_pc                      # guest pc of the body = m_pc+diff-6
    addiu t7, -6
    sw t7, blockop_rej_pc
    j dbcc_no_blockop
bo_rej_bud:
    lw t8, blockop_rej_bud
    addiu t8, 1
    sw t8, blockop_rej_bud
    j dbcc_no_blockop
bo_rej_span:
    lw t8, blockop_rej_span
    addiu t8, 1
    sw t8, blockop_rej_span
    j dbcc_no_blockop
bo_rej_shape:
    j dbcc_no_blockop
#endif
dbcc_no_blockop:
#endif
    j branch_exec

#ifdef M64K_BLOCKOPS
blockop_movew_port:
    # Destination (An) must be the LSPC VRAM data port (0x3C0002): the only
    # (An)-fixed-destination copy the game runs hot, and the expensive one.
    srl t1, t9, 7
    andi t1, 7<<2
    lw_ctx t1, M64K_OFF_AREGS,t1       # Ad
    sll t1, 8
    srl t1, 8                          # 24-bit address
    li result, 0x003C0002
    bne t1, result, BO_REJ_DST
    # Budget: K = min(count-1, (m_cycles-11)/22); keeps m_cycles > 0 so the
    # interpreter always resumes at the loop body. 22 = move.w (An)+,(An)
    # [12] + taken dbra [10], matching the interpreter's own charges.
    addi result, m_cycles, -11
    blez result, BO_REJ_BUD
    li t8, 22
    divu zero, result, t8
    mflo result
    beqz result, BO_REJ_BUD
    ble t2, result, 1f
    move t2, result
1:  # Source (As)+ span must stay inside the always-mapped P-ROM + work-RAM
    # window (< 0x110000): a native lhu there can never TLB-fault.
    andi t1, t9, 7
    sll t1, 2
    la_ctx t9, M64K_OFF_AREGS,t1       # &As
    lw t8, 0(t9)                       # As (full register value)
    sll t1, t8, 8
    srl t1, 8                          # 24-bit source address
    sll result, t2, 1                  # 2K bytes
    addu result, t1
    li eaptr32, 0x00110000
    bgt result, eaptr32, BO_REJ_SPAN
    # ---- commit ----
#if defined(M64K_DYNSTAT) || defined(MVS64_BOSTAT)
    lw t8, blockop_fire
    addiu t8, 1
    sw t8, blockop_fire
    lw t8, 0(t9)                       # re-load As: the counter bump clobbered
                                       # it. (This was "li t8, 22" — restoring
                                       # the BUDGET constant, not As — so every
                                       # INSTRUMENTED build corrupted As := 22
                                       # + 2K on each fire: the real source of
                                       # the "diagnostic builds diverge
                                       # content" forks, first fire ~f=385.)
#endif
    sll result, t2, 1                  # As += 2K
    addu t8, result
    sw t8, 0(t9)
    sub t8, t0, t2                     # counter = (old-1) - K
    addi t8, -1
    sh t8, 2(dptr)
    li t8, 22                          # m_cycles -= 10 + 22K
    multu t2, t8
    mflo t8
    sub m_cycles, t8
    addi m_cycles, -10
    addi m_pc, -6                      # resume at the loop body
    map_m68k t1                        # t1 = mapped src, t2 = count
    jal mvs64_blockop_vram_copy_w
    j main_loop

blockop_fill_w:
    # value = Dn.w
    andi t1, t9, 7
    sll t1, 2
    la_ctx t1, M64K_OFF_DREGS,t1
    lhu result, 2(t1)
    # Budget: 18 = move.w Dn,(An)+ [8] + taken dbra [10]
    addi t1, m_cycles, -11
    blez t1, dbcc_no_blockop
    li t8, 18
    divu zero, t1, t8
    mflo t1
    beqz t1, dbcc_no_blockop
    ble t2, t1, 1f
    move t2, t1
1:  # Destination (Ad)+ span must stay inside work RAM [0x100000,0x110000)
    srl t1, t9, 7
    andi t1, 7<<2
    la_ctx t9, M64K_OFF_AREGS,t1       # &Ad
    lw t8, 0(t9)                       # Ad (full register value)
    sll t1, t8, 8
    srl t1, 8                          # 24-bit dest address
    li eaptr32, 0x00100000
    blt t1, eaptr32, dbcc_no_blockop
    sll t7, t2, 1
    addu t7, t1
    li eaptr32, 0x00110000
    bgt t7, eaptr32, dbcc_no_blockop
    # ---- commit ----
    sll t7, t2, 1                      # Ad += 2K
    addu t8, t7
    sw t8, 0(t9)
    sub t7, t0, t2                     # counter = (old-1) - K
    addi t7, -1
    sh t7, 2(dptr)
    li t7, 18                          # m_cycles -= 10 + 18K
    multu t2, t7
    mflo t7
    sub m_cycles, t7
    addi m_cycles, -10
    addi m_pc, -6                      # resume at the loop body
    map_m68k t1                        # native fill (work RAM: mapped RW)
1:  sh result, 0(t1)
    addiu t1, 2
    addiu t2, -1
    bnez t2, 1b
    j main_loop

blockop_fill_l:
    # value = Dn (32-bit)
    andi t1, t9, 7
    sll t1, 2
    la_ctx t1, M64K_OFF_DREGS,t1
    lw result, 0(t1)
    # Budget: 26 = move.l Dn,(An)+ as m64k charges it [16] + taken dbra [10]
    addi t1, m_cycles, -11
    blez t1, dbcc_no_blockop
    li t8, 26
    divu zero, t1, t8
    mflo t1
    beqz t1, dbcc_no_blockop
    ble t2, t1, 1f
    move t2, t1
1:  # Destination (Ad)+ span must stay inside work RAM [0x100000,0x110000)
    srl t1, t9, 7
    andi t1, 7<<2
    la_ctx t9, M64K_OFF_AREGS,t1       # &Ad
    lw t8, 0(t9)                       # Ad (full register value)
    sll t1, t8, 8
    srl t1, 8                          # 24-bit dest address
    li eaptr32, 0x00100000
    blt t1, eaptr32, dbcc_no_blockop
    sll t7, t2, 2
    addu t7, t1
    li eaptr32, 0x00110000
    bgt t7, eaptr32, dbcc_no_blockop
    # ---- commit ----
    sll t7, t2, 2                      # Ad += 4K
    addu t8, t7
    sw t8, 0(t9)
    sub t7, t0, t2                     # counter = (old-1) - K
    addi t7, -1
    sh t7, 2(dptr)
    li t7, 26                          # m_cycles -= 10 + 26K
    multu t2, t7
    mflo t7
    sub m_cycles, t7
    addi m_cycles, -10
    addi m_pc, -6                      # resume at the loop body
    map_m68k t1
    srl t3, result, 16                 # high word first (68k byte order;
1:  sh t3, 0(t1)                       #  2-byte stores keep any even address
    sh result, 2(t1)                   #  legal, matching the 68000 bus)
    addiu t1, 4
    addiu t2, -1
    bnez t2, 1b
    j main_loop

blockop_copy_l:
    # move.l (As)+,(Ad)+ / dbf: the block-unpack memcpy the game runs at
    # 0x31B8 (count word from the stream, longs copied ROM/RAM -> work RAM).
    # As = bits 2-0, Ad = bits 11-9. The same-register form (e.g. 0x20D8
    # proper, move.l (a0)+,(a0)+) interleaves the two pointer bumps and does
    # NOT have memcpy semantics: reject it. NOTE: every reject path below
    # must leave t3 intact (branch_exec consumes it).
    lw t8, mvs64_blockop_copyl_enable  # runtime A/B gate (twin fps builds)
    beqz t8, dbcc_no_blockop
    srl t7, t9, 7
    andi t7, 7<<2                      # Ad index*4
    andi t1, t9, 7
    sll t1, 2                          # As index*4
    beq t1, t7, dbcc_no_blockop
    la_ctx t1, M64K_OFF_AREGS,t1       # &As
    la_ctx t7, M64K_OFF_AREGS,t7       # &Ad
    # Budget: K = min(count-1, (m_cycles-11)/30); keeps m_cycles > 0 so the
    # interpreter always resumes at the loop body. 30 = move.l (As)+,(Ad)+
    # [12 entry + 4 src (An)+ + 4 dst (An)+] + taken dbra [10], matching the
    # interpreter's own charges (charge placement inside the fused span is
    # unobservable: both spans are guarded RAM/ROM and can never trap).
    addi t9, m_cycles, -11
    blez t9, BO_REJ_BUD
    li t8, 30
    divu zero, t9, t8
    mflo t9
    beqz t9, BO_REJ_BUD
    ble t2, t9, 1f
    move t2, t9
1:  lw t8, 0(t1)                       # As (full register value)
    lw t9, 0(t7)                       # Ad (full register value)
    or result, t8, t9                  # an odd long access would raise an
    andi result, 1                     #  address error in the interpreter:
    bnez result, dbcc_no_blockop       #  leave those loops interpreted
    # Source span must stay inside an always-mapped window: P-ROM + work RAM
    # (< 0x110000) or the PB-ROM bank window [0x200000,0x300000) — samsho2's
    # 2MB P-ROM upper half lives there (the unpack loops at 0x31B8 read it,
    # measured 1584 span-rejects/window before this was widened). The bank
    # mapping can only change via an MMIO write, which the 1-insn loop body
    # cannot issue, so the window is stable across the fused span.
    sll eaptr32, t8, 8
    srl eaptr32, 8                     # 24-bit source address (kept for map)
    sll result, t2, 2                  # 4K bytes
    addu result, eaptr32
    li eaptr, 0x00110000
    ble result, eaptr, 1f              # low window: end <= 0x110000
    li eaptr, 0x00200000
    blt eaptr32, eaptr, bo_cpl_rejsrc  # straddles/starts in unmapped gap
    li eaptr, 0x00300000
    bgt result, eaptr, bo_cpl_rejsrc   # PB-ROM window: end <= 0x300000
1:
    # Destination span must stay inside work RAM [0x100000,0x110000).
    sll result, t9, 8
    srl result, 8                      # 24-bit dest address (kept for map)
    li eaptr, 0x00100000
    blt result, eaptr, bo_cpl_rejdst
    sll t9, t2, 2
    addu t9, result
    li eaptr, 0x00110000
    bgt t9, eaptr, bo_cpl_rejdst
    # ---- commit ----
#if defined(M64K_DYNSTAT) || defined(MVS64_BOSTAT)
    lw t9, blockop_fire
    addiu t9, 1
    sw t9, blockop_fire
#endif
    sll t9, t2, 2                      # bytes = 4K
    addu t8, t9                        # As += 4K
    sw t8, 0(t1)
    lw t8, 0(t7)
    addu t8, t9                        # Ad += 4K
    sw t8, 0(t7)
    sub t8, t0, t2                     # counter = (old-1) - K
    addi t8, -1
    sh t8, 2(dptr)
    li t8, 30                          # m_cycles -= 10 + 30K
    multu t2, t8
    mflo t8
    sub m_cycles, t8
    addi m_cycles, -10
    addi m_pc, -6                      # resume at the loop body
    map_m68k eaptr32                   # mapped source
    map_m68k result                    # mapped dest
1:  lwl t8, 0(eaptr32)                 # unaligned-capable pairs, byte order
    lwr t8, 3(eaptr32)                 #  identical to lw_m68k/sw_m68k; the
    swl t8, 0(result)                  #  sequential forward long-by-long
    swr t8, 3(result)                  #  order matches the interpreter, so
    addiu eaptr32, 4                   #  overlapping src/dst behave the same
    addiu result, 4
    addiu t2, -1
    bnez t2, 1b
    j main_loop

    # copy_l span-reject diagnostics: record WHICH bound rejected and the
    # offending 24-bit address in the otherwise-unused [BOSTAT] t3/body
    # slots (t3val = rejecting SRC, t9val = rejecting DST). Finding
    # (2026-08-08): ALL of samsho2's (As)+,(Ad)+ DBF loops target PALETTE
    # RAM (0x401558/0x401eb8) — trapped MMIO with conversion side effects,
    # ~2 entries/frame in-fight — so copy_l has no RAM->RAM traffic in
    # this game and stays as correct-but-idle infrastructure.
bo_cpl_rejsrc:
#if defined(M64K_DYNSTAT) || defined(MVS64_BOSTAT)
    sw eaptr32, blockop_t3val
#endif
    j BO_REJ_SPAN
bo_cpl_rejdst:
#if defined(M64K_DYNSTAT) || defined(MVS64_BOSTAT)
    sw result, blockop_t9val
#endif
    j BO_REJ_SPAN

blockop_fillw_port:
    # move.w Dn,(An) / dbf with An = the LSPC VRAM data port (0x3C0002):
    # a constant-value fill through the port's auto-increment (the #1
    # remaining un-fused DBF class: masked 0x3080 at guest 0x3412,
    # 8k-24k rejected entries per 600-frame window in-fight, one TLB
    # exception per word when interpreted). Same structure as the port
    # copy: K = count-1 iterations run natively via the hw_n64.S helper
    # (identical modulo/mask/bank semantics), the expiring iteration and
    # any budget tail stay interpreted, charges match the interpreter
    # exactly. NOTE: every reject path must leave t3 intact (branch_exec).
    lw t8, mvs64_blockop_fwp_enable    # runtime A/B gate (twin fps builds)
    beqz t8, dbcc_no_blockop
    srl t1, t9, 7
    andi t1, 7<<2
    lw_ctx t1, M64K_OFF_AREGS,t1       # Ad
    sll t1, 8
    srl t1, 8                          # 24-bit address
    li result, 0x003C0002
    bne t1, result, BO_REJ_DST
    # Budget: K = min(count-1, (m_cycles-11)/18); keeps m_cycles > 0 so
    # the interpreter always resumes at the loop body. 18 = move.w
    # Dn,(An) [4 entry + 0 Dn + 4 (An)] + taken dbra [10].
    addi result, m_cycles, -11
    blez result, BO_REJ_BUD
    li t8, 18
    divu zero, result, t8
    mflo result
    beqz result, BO_REJ_BUD
    ble t2, result, 1f
    move t2, result
1:  # value = Dn.w (constant across the run: the body cannot change Dn)
    andi t1, t9, 7
    sll t1, 2
    la_ctx t1, M64K_OFF_DREGS,t1
    lhu t1, 2(t1)                      # 16-bit fill value
    # ---- commit ----
#if defined(M64K_DYNSTAT) || defined(MVS64_BOSTAT)
    lw t8, blockop_fire
    addiu t8, 1
    sw t8, blockop_fire
#endif
    sub t8, t0, t2                     # counter = (old-1) - K
    addi t8, -1
    sh t8, 2(dptr)
    li t8, 18                          # m_cycles -= 10 + 18K
    multu t2, t8
    mflo t8
    sub m_cycles, t8
    addi m_cycles, -10
    addi m_pc, -6                      # resume at the loop body
    jal mvs64_blockop_vram_fill_w      # t1 = value, t2 = count
    j main_loop
#endif

OP(scc):
    andi t0, opcode, 0b111000
    beq t0, 0b001000, op_dbcc
    li opsize, 1
    jal decode_ea
    lbu_m68k t0, eaptr                 # dummy fetch (performed by 68000)
    jal check_cc
    neg result, t0                     # convert 0x01 into 0xFF
    sb_m68k result, eaptr              # store result
    #if M64K_CONFIG_TIMING_ACCURACY >= 1
    .set noreorder
    andi t0, opcode, 0b110000          # check EA mode (register Dn/An)
    bnezl t0, main_loop                # if EA is not register direct
     use_cycles 8                      #  go back to main loop with 8 cycles (fixed)
    bltzl result, main_loop            # if result is negative ("true")
     use_cycles 6                      #  go back to main loop with 6 cycles (fixed)
    .set reorder
    use_cycles 4                       # if result is positive, use 4 cycles
    #else
    use_cycles 6                       # use an approximated average of 6 cycles among all cases
    #endif
    j main_loop

    .section .sdata
opcctable:
    .long   cc_t, cc_hi, cc_cc, cc_ne, cc_vc, cc_pl, cc_ge, cc_gt
#ifdef M64K_FASTPATHS
# Bcc fast-path dispatch: one entry per condition code (opcode bits 11:8),
# sense baked in. Cond 0 (BRA) is always taken; cond 1 is BSR.
bcc_cctable:
    .long   bcc_taken, op_bsr,   bcct_hi, bcct_ls
    .long   bcct_cc,   bcct_cs, bcct_ne, bcct_eq
    .long   bcct_vc,   bcct_vs, bcct_pl, bcct_mi
    .long   bcct_ge,   bcct_lt, bcct_gt, bcct_le
#endif

    .text
ALIGN(check_cc):
    srl t0, opcode, 7
    andi t0, 0x7<<2
    lw t0, opcctable(t0)               # fetch condition code function pointer
    srl t2, opcode, 8
    andi t2, 1                         # t2=1 => invert branch condition
    jr t0                              # call condition code function
    # Condition codes checking functions
    # Input: t2 => 0: standard chaeck, 1: inverted chack
    # Output: t0 => 0: branch not taken, 1: branch taken
cc_t:  # true
    xori t0, t2, 1
    jr ra
cc_cc: # carry clear (C == 0)
    dsrl t0, flag_zc, 32
    andi t0, 1
    seq t0, t2
    jr ra
cc_ne: # not equal (Z == 0)
    zx64 t0, flag_zc
    seq t0, zero
    seq t0, t2
    jr ra
cc_pl: # plus (N == 0)
    srl t0, flag_nv, 31
    seq t0, t2
    jr ra
cc_hi: # hi (C==0 && Z==0)
    zx64 t0, flag_zc
    seq t0, zero
    dsrl t1, flag_zc, 32
    andi t1, 1
    or t0, t1
    seq t0, t2
    jr ra
cc_vc: # overflow clear (V == 0)
    dsrl t0, flag_nv, 63
    srl t1, flag_nv, 31
    xor t0, t1
    seq t0, t2
    jr ra
cc_ge: # greater equal (N == V)
    sge t0, flag_nv, zero
    xor t0, t2
    jr ra
cc_gt: # greater than (N == V && Z == 0)
    sge t0, flag_nv, zero
    zx64 t1, flag_zc
    sne t1, zero
    and t0, t1
    xor t0, t2
    jr ra

OP(reset):
    check_supervisor
    use_cycles 132
    li t0, M64K_PENDINGEXC_RSTO
    sw_ctx t0, M64K_OFF_PENDINGEXC     # exit with code M64K_PENDINGEXC_RSTO
    j main_loop_exit

OP(rtr):
    li rmw_srmask, CCR_MASK
    j rte_exec
op_rte:
    li rmw_srmask, SR_MASK
    check_supervisor
rte_exec:
    use_cycles 20
    li t0, 6                            # pop 6 bytes from the stack
    jal popn
    move eaptr, t0                      # store pointer to frame in eaptr
    jal flush_stack                     # store current stack pointer to USP/SSP, return t1=SR
    lhu t0, 0(eaptr)                    # read SR from stack
    and t0, rmw_srmask
    not rmw_srmask
    and t1, rmw_srmask
    or t0, t1
    sw_ctx t0, M64K_OFF_SR
    lwl eaptr32, 2(eaptr)               # read PC from stack
    lwr eaptr32, 5(eaptr)
    jal_and_j reload_sr, jmp_exec       # reload flags/A7 using SR, and then jump to new PC

OP(trap):
    use_cycles 4
    andi eaptr32, opcode, 0xF
    addi eaptr32, 32
trap:
    use_cycles 30
    jal flush_sr
    ori t1, SR_SUPERV
    sw_ctx t1, M64K_OFF_SR
    jal reload_stack

    li t0, -6
    jal pushn
    lw result, pc_diff
    addu result, m_pc
    swl result, 2(t0)
    swr result, 5(t0)

    lw_ctx result, M64K_OFF_SR
    sh result, 0(t0)

    lw_ctx t1, M64K_OFF_VBR
    sll eaptr32, 2
    addu eaptr32, t1
    map_m68k eaptr, eaptr32
    check_addr_error eaptr32, read_address_error
    lw eaptr32, 0(eaptr)
    j jmp_exec

OP(trapv):
    use_cycles 4
    li t2, 0
    jal cc_vc
    bnez t0, main_loop
    li eaptr32, 7
    j trap

    #define bitidx_mask     result
OP(bitsr):  # bit operation using a register as index
    jal_and_j decode_dptr, op_bits_start
op_bitsi:   # bit operation using immediate value as index
    use_cycles 4
    addi dptr, m_pc, -2
    addiu m_pc, 2
op_bits_start:
    lax rmw_op, bits_impl
    andi t0, opcode, 0b111000
    beq t0, 0<<3, op_bits_dreg   # check if EA mode is Dn
    beq t0, 1<<3, op_movep       # check if EA mode is An => movep
op_bits_ea:
    use_cycles_opmask 0b1111111, 0b0111100, 2  # Adjust BTST+BCLR: 2 cycles more when EA is #<data>
    li bitidx_mask, 7            # standard EA: 8-bit operation
    li opsize, 1
    jal_and_j decode_ea, rmw8_eadst
op_bits_dreg:
    use_cycles_opmask 1<<6, 0, 2               # Adjust BTST+BCLR: 2 cycles more when using a register
    li bitidx_mask, 31           # EA is Dn: 32-bit operation
    li opsize, 4
    jal_and_j decode_ea, rmw32_eadst
    .sdata
    # Address table of the 4 bit-op handlers, indexed by opcode bits 6-7.
    # (Was previously indexed as bittable+bitop*8, but the handlers assemble to
    # 12 bytes each in reorder mode -- the jr/j delay slots get a nop -- so the
    # fixed *8 stride landed BCLR/BSET on the wrong instructions. An explicit
    # pointer table is immune to handler size.)
bittable_ptrs:
    .long op_btst_e, op_bchg_e, op_bclr_e, op_bset_e
    .text
bits_impl:  # First part is common to all bit operations
    use_cycles 8
    srl t2, opcode, 6            # extract bitop type (opcode bits 6-7)
    andi t2, 3                   # 0=btst, 1=bchg, 2=bclr, 3=bset
    sll t3, t2, 2               # *4 to index the .long pointer table
    lw rmw_op, bittable_ptrs(t3) # rmw_op = address of the selected handler
    dsrl flag_zc, 32             # isolate C flag (clear bits of Z)
    dsll flag_zc, 32
    and t0, bitidx_mask          # mask bit index (either 0..7 o 0..31)
    #if M64K_CONFIG_TIMING_ACCURACY >= 1
    .set noreorder
    beqz t2, 1f                  # do nothing if opcode is btst (t2==0)
     li t2, 1
    beq opsize, t2, 1f           # do nothing if opsize is 8-bit (aka: EA access)
     li t2, 16
    bltl t0, t2, 1f              # if bitindex < 16
     use_cycles -2               # bchg/bclr/bset: 2 cycles less if bitindex < 16
1:  .set reorder
    #endif
    li t2, 1
    sllv t2, t2, t0              # t2 = 1 << bitindex
    and t3, t2, t1               # t3 = value & (1 << bitindex)
    or flag_zc, t3               # set Z flag with current bit value
    jr rmw_op                    # jump to bitop function
op_btst_e:
    use_cycles -4                # btst: no writeback necessary
    j main_loop
op_bchg_e:
    xor result, t1, t2           # bchg: result = value ^ (1 << bitindex)
    jr ra
op_bclr_e:
    xor result, t1, t3           # bclr: result = value ^ (value & (1 << bitindex))
    jr ra
op_bset_e:
    or result, t1, t2            # bset: result = value | (1 << bitindex)
    jr ra
    #undef bitmask

OP(movep): # NTOE: dptr was already decoded in op_bits
    use_cycles 8
    xori opcode, 1<<5            # convert EA mode to (d16,An)
    andi t0, opcode, 1<<6        # check size bit
    li opsize, 4                 #  bit 6 = 1 => 32-bit operation
    bnez t0, movep_start
    li opsize, 2                 #  bit 6 = 0 => 16-bit operation
    addi dptr, 2
movep_start:
    jal decode_ea                # now decode EA
    andi t1, opcode, 1<<7        # check direction bit
    bnez t1, movep_wmem
movep_wreg:
    lbu_m68k t0, eaptr           # read memory
    addiu opsize, -2
    sb t0, 0(dptr)               # write register
    lb t0, 2(eaptr)
    sb t0, 1(dptr)
    beqz opsize, main_loop
    lb t0, 4(eaptr)
    sb t0, 2(dptr)
    lb t0, 6(eaptr)
    sb t0, 3(dptr)
    use_cycles 8
    j main_loop
movep_wmem:
    lbu result, 0(dptr)          # read register
    addiu opsize, -2
    sb_m68k result, eaptr        # write memory
    lbu result, 1(dptr)
    sb result, 2(eaptr)
    beqz opsize, main_loop
    lbu result, 2(dptr)
    sb result, 4(eaptr)
    lbu result, 3(dptr)
    sb result, 6(eaptr)
    use_cycles 8
    j main_loop

OP(exg):
    andi t0, opcode, 3<<6
    beqz t0, op_abcd
    use_cycles 6
    jal decode_dptr
    li opsize, 4
    jal decode_ea
    andi t2, opcode, 0b11111000
    bne t2, 0b01001000, op_exg_start
    addi dptr, 8*4
op_exg_start:
    lw result, (dptr)
    lw t3, (eaptr)
    sw result, (eaptr)
    sw t3, (dptr)
    j main_loop

OP(tas):
    use_cycles 10
    use_cycles_opmask 7<<3, 0<<3, -6
    li opsize, 1                       # TAS is always 8-bit
    lax rmw_op, op_tas_rmwimpl
    jal_and_j decode_ea, rmw8_eadst    # decode the EA and run RMW
op_tas_rmwimpl:
    dsllv flag_zc, t1, rmw_bitsize     # affect Z and C flags
    sllv flag_nv, t1, rmw_bitsize      # affect N and V flags
    #if M64K_CONFIG_BROKEN_TAS
    andi t0, opcode, 3<<3              # check if EA mode is Dn
    bnez t0, main_loop                 # if not, avoid writeback (broken TAS)
    #endif
    ori result, t1, 0x80               # writeback value: set bit 7
    jr ra

OP(nop):
    use_cycles 4
    j main_loop


# CHK.W <ea>,Dn: compare signed Dn.w against the range [0, <ea>.w]. If Dn is
# out of range, raise a CHK exception (vector 6); the common in-range case just
# falls through to the next instruction. Reached from OP(lea) when opcode bit 6
# is 0. Previously CHK was aliased into the illegal-opcode block below and hard
# aborted, which is what crashed samsho2 at the BIOS->cart handoff.
op_chk:
    li opsize, 2
    jal decode_ea                       # eaptr -> bound operand (consumes EA ext words)
    check_addr_error eaptr32, read_address_error  # odd word EA -> 68k address error
    lh_m68k t0, eaptr                   # t0 = bound (sign-extended .w); must be t0
    move t2, t0                          # save bound across the Dn decode
    srl t1, opcode, 7                    # extract Dn (opcode bits 9-11) -> reg*4
    andi t1, 0x7<<2
    addu t1, ctx
    lh t1, M64K_OFF_DREGS+2(t1)         # t1 = src = Dn.w (sign-extended)
    use_cycles 10
    # CHK updates the CCR on every path (X preserved): Z=(src.w==0), V=0, C=0,
    # N=(src<0). Then trap if src<0 or src>bound; otherwise fall through.
    andi flag_zc, t1, 0xFFFF             # Z from src low word, C=0
    bltz t1, op_chk_neg                  # src < 0 -> N=1, always traps
    li flag_nv, 0                        # src >= 0: N=0, V=0
    bgt  t1, t2, op_chk_trap             # src > bound -> trap
    j main_loop                          # in range: continue (PC already at next insn)
op_chk_neg:
    dli flag_nv, 0x8000000080000000      # src < 0: N=1, V=0
op_chk_trap:
    li t0, M64K_PENDINGEXC_CHK
    sw_ctx t0, M64K_OFF_PENDINGEXC       # 68000 stacks PC of the *next* instruction
    j main_loop_exit


# STOP #<imm>: privileged. Loads the immediate word into SR (which is what sets
# the interrupt mask the following idle/VBlank-wait loop relies on) and then
# continues. A real 68000 halts here until an interrupt, but NeoGeo idle loops
# are of the form "STOP #imm ; <poll a vblank flag> ; bra back", so continuing
# (the loop busy-polls until the VBlank IRQ fires) boots correctly. Previously
# STOP was aliased into the illegal-opcode block, which made the post-eyecatch
# wait loop spin forever taking the illegal vector.
op_stop:
    check_supervisor                     # STOP is privileged
    jal flush_sr                         # sync live flags / A7 into SR before overwrite
    lhu t0, 0(m_pc)                      # immediate (new SR), sits right after the opcode
    addi m_pc, 2                         # advance past the immediate word
    and t0, SR_MASK
    sw_ctx t0, M64K_OFF_SR
    use_cycles 4
    jal_and_j reload_sr, main_loop       # reload flags/stack from new SR, then continue


op_unk:
# 68010+ instructions
op_movec: op_bkpt: op_mvfccr: op_moves: op_rtd:
# 68020+ instructions
op_bftst: op_bfextu: op_bfchg: op_bfexts: op_bfclr: op_bfffo: op_bfset: op_bfins:
op_callm: op_cas: op_cas2: op_chk2: op_cmp2: op_cpbcc: op_cpdbcc: op_cpgen:
op_cprestore: op_cpsave: op_cpscc: op_cptrapcc: op_divul: op_divsl: op_extb:
op_pack: op_rtm: op_trapcc: op_unpk:
    # No illegal/unimplemented-instruction path existed here: every such opcode
    # hard-aborted via assertf(). A real 68000 instead raises an illegal-
    # instruction exception (vector 4) and keeps running, which the BIOS->cart
    # handoff depends on. Vector through it instead of asserting.
    sw_ctx opcode, M64K_OFF_PENDINGEXC+4   # stash opcode for logging
    li t0, M64K_PENDINGEXC_ILLEGAL
    sw_ctx t0, M64K_OFF_PENDINGEXC+0
    addi m_pc, -2                          # 68000 stacks PC of the faulting opcode
    j main_loop_exit

#ifdef M64K_DYNREC
# Dynarec probe hit: way-resolved block dispatch. Both probe sites leave
# t2 = &set; the emitted block (dynrec.c) carries its own C_max entry gate
# and every exit restores the m_pc/main_loop contract. "jr t0" is in the
# TLB handler's delay-slot resolver set; delay slots stay nop regardless.
    .set push
    .set noreorder
ALIGN(dyn_probe_hit):                  # way-0 hit
    lw t0, 4(t2)
    jr t0
     nop
ALIGN(dyn_probe_hit1):                 # way-1 hit
    lw t0, 12(t2)
    jr t0
     nop
    .set pop
#endif

#ifdef M64K_DYNSTAT
# Dynarec coverage rig (diagnostic builds only, like MVS64_OPHIST): count
# every control-transfer target seen at jmp_exec — deliberately the SAME
# visibility as the dynarec block-table probe (taken Bcc and non-blockop-
# fused DBcc route through branch_exec->jmp_exec; JMP/JSR/BSR/RTS generic
# paths land here; rts_fast/bsr_fast bypass this exactly as they bypass the
# probe). Tables live in emu.c ([DYNSTAT] dump); slot = (pc>>1) & 0xFFFF,
# last-writer PC recorded for collision detection. Clobbers t0/t2/t3 and
# ra — all dead on the jmp_exec -> main_loop edge.
ALIGN(dynstat_count):
    srl t2, eaptr32, 1
    andi t2, 0xFFFF
    sll t2, 2
    la t3, m64k_dynstat_cnt
    addu t3, t2
    lw t0, 0(t3)
    addiu t0, 1
    sw t0, 0(t3)
    la t3, m64k_dynstat_pc
    addu t3, t2
    sw eaptr32, 0(t3)
    jr ra
#endif

    # End-of-text marker: the TLB handler (hw_n64.S) uses [_m64k_asmrun,
    # __m64k_asm_end) to recognize faults coming from the interpreter itself
    # (where saved a1 == live m_cycles) vs nested/outside faults.
    .globl __m64k_asm_end
__m64k_asm_end:
