# RSP overlay for YM2610 ADPCM offload (rspq).
#
# Design notes (why this overlay looks the way it does):
#  - Commands are MONOLITHIC: each command DMAs in everything it needs and DMAs
#    out everything it produced. No state is kept in DMEM between commands, so
#    there is no saved-state block: rspq may interleave rsp_video commands
#    (which reload the overlay data/bss area on the switch) between any two
#    audio commands without corrupting us.
#  - The CPU side (ym2610.c rspa_* glue) builds a parameter block in RDRAM
#    (cache-written-back) and polls an uncached sequence word at the END of the
#    output block. The sequence word is written by the FINAL (synchronous) DMA,
#    so when it is visible the whole output block is.
#  - The decode is a bit-exact port of OPNB_ADPCMA_calc_chan / OPNB_ADPCMB_CALC
#    from ym2610.c, restricted to the LINEAR case: the CPU pre-stages exactly
#    the source bytes each channel will consume this chunk (the address stream
#    is deterministic - it does not depend on the decoded data) and falls back
#    to the C decoder for a whole chunk when ADPCM-B could hit its limit-wrap /
#    repeat-restart paths. ADPCM-A end-of-sample and ADPCM-B end-stop ARE
#    handled here (they are the common case).
#  - The SU has no multiply. Products go through VMUL16 (the VU multiplier,
#    operands that fit s16), which replaced the scalar shift-add loops that
#    were about half of the ADPCM-B chunk cost. Every product either fits in
#    32 bits or is used modulo 2^32 in a sum whose exact value fits (ADPCM-B
#    interpolator), so the results are exact.
#
# Commands:
#  0x0 cmd_adpcm_test: DMA-in bytes from a0, add 1 to each, DMA-out to a1.
#      Boot self-test (overlay registered, dispatch + both DMA directions OK).
#  0x1 cmd_adpcm_run:  decode one chunk (up to 128 samples).
#      a0 = param block RDRAM physical (see PARAM LAYOUT below)
#      a1 = output block RDRAM physical (see OUT LAYOUT below)
#      a2 = sequence number to write at out+1412 when done
#  0x2 cmd_adpcm_wp:   like 0x1, plus a3 = RDRAM physical of the resident
#      dynamic-state block (whole-pump mode: acc/astep/aout/now_data per A
#      channel and acc/adpcmd/prev_acc/adpcml/now_data for B chain RSP-side
#      across chunks; the param's dynamic fields are used only for channels
#      flagged FRESH at +8/+9).
#
# PARAM LAYOUT (384 bytes, mirrored in ym2610.c rspa_param_t):
#   +0   u32 jedi_phys      RDRAM physical of jedi_table[49*16] (s16)
#   +4   u16 n              samples this chunk (1..128)
#   +6   u8  amask          bit c = ADPCM-A channel c active
#   +7   u8  bflags         bit 0 = ADPCM-B active on RSP
#   +8   u8  fresha         (0x2 only) bit c = A channel c state is in param
#   +9   u8  freshb         (0x2 only) bit 0 = B state is in param
#   +10  pad to 16
#   +16  A channel c at +16 + c*48:
#     +0  u32 now_addr   +4  u32 now_step   +8  u32 step   +12 u32 end_x2m
#     +16 s32 acc        +20 s32 astep      +24 s32 aout
#     +28 u8 now_data    +29 u8 flagMask    +30 u8 vol_mul +31 u8 vol_shift
#     +32 s32 maskL      +36 s32 maskR      (0 or -1 pan routing masks)
#     +40 u32 src_phys   +44 u16 src_cur    +46 u8 flag_out (in: 1) +47 pad
#   +304 B channel (64 bytes):
#     +0  u32 now_addr   +4  u32 now_step   +8  u32 step   +12 u32 end_x2
#     +16 s32 acc        +20 s32 adpcmd     +24 s32 prev_acc  +28 s32 volume
#     +32 s32 maskL      +36 s32 maskR
#     +40 u32 src_phys   +44 u16 src_cur    +46 u8 now_data +47 u8 flag_out
#     +48 s32 adpcml(out) +52 u8 eosbit     +53 pad to 64
#   +368 pad to 384
#
# OUT LAYOUT (1424 bytes, mirrored in ym2610.c rspa_out_t):
#   +0    s32 l[128]     per-sample L contribution (A summed + B folded)
#   +512  s32 r[128]
#   +1024 updated param block echo (384 bytes; CPU reads back state fields)
#   +1408 u32 arrived    (flag bits to OR into adpcm_arrivedEndAddress)
#   +1412 u32 seq        (written LAST - CPU poll target)

#include <rsp_queue.inc>

    .data

    RSPQ_BeginOverlayHeader
        RSPQ_DefineCommand cmd_adpcm_test, 12   # 0x0: a0=src, a1=dst, a2=size
        RSPQ_DefineCommand cmd_adpcm_run, 12    # 0x1: a0=param, a1=out, a2=seq
        RSPQ_DefineCommand cmd_adpcm_wp, 16     # 0x2: + a3=resident dyn block
    RSPQ_EndOverlayHeader

    RSPQ_EmptySavedState

    .align 3
# ADPCM-B "Forecast to next Forecast" table (x adpcmd / 8)
BDEC1: .half 1, 3, 5, 7, 9, 11, 13, 15, -1, -3, -5, -7, -9, -11, -13, -15
# ADPCM-B "delta to next delta" table (x adpcmd / 64)
BDEC2: .half 57, 57, 57, 57, 77, 102, 128, 153, 57, 57, 57, 57, 77, 102, 128, 153
# ADPCM-A step_inc table (indexed by nibble & 7)
STEP_INC: .half -16, -16, -16, -16, 32, 80, 112, 144

    .bss
    .align 4
PARAM_BUF: .dcb.b 384
JEDI_BUF:  .dcb.b 1568
ACC_L:     .dcb.b 256      # ACC_R MUST be ACC_L+256 (code uses fixed offset)
ACC_R:     .dcb.b 256
SRC_BUF:   .dcb.b 192      # 176-byte source slice + alignment slack
TEST_BUF:  .dcb.b 128
    .align 3
ARRIVED:   .dcb.b 4        # SEQ MUST follow ARRIVED (final 8-byte DMA)
SEQ_OUT:   .dcb.b 4
RA_SAVE:   .dcb.b 4
CUR_SAVE:  .dcb.b 4
CH_MASK:   .dcb.b 4
CH_PTR:    .dcb.b 4
# whole-pump mode (cmd_adpcm_wp): the ADPCM waveform state chains RSP-side
# through an RDRAM block (see rspwpa_dyn in ym2610.c) instead of round-
# tripping through the param echo. DYNA layout: 6 x 16B A slots
# {s32 acc, s32 astep, s32 aout, u8 now_data} then a 24B B slot at +96
# {s32 acc, s32 adpcmd, s32 prev_acc, s32 adpcml, u8 now_data}, 128 total.
WP_MODE:   .dcb.b 4
DYN_PHYS:  .dcb.b 4
    .align 3               # DMA alignment (the rsp_fm DYNBUF lesson)
DYNA:      .dcb.b 128

    .text

    #############################################################
    # VMUL16: dst = (s16)x * (s16)y, exact 32-bit product via the
    # VU multiplier: vmudh places the full product in ACC[47:16],
    # so ACC_HI = product[31:16] and ACC_MD = product[15:0].
    # BOTH OPERANDS MUST FIT s16. Clobbers \tmp and $v27-$v29
    # lane 0. ~10 ops flat vs ~7 per multiplier bit for the scalar
    # shift-add multiply it replaced, whose loops were roughly half
    # the ADPCM-B chunk cost.
    #############################################################
    .macro VMUL16 dst, x, y, tmp
    mtc2 \x, $v27.e0
    mtc2 \y, $v28.e0
    vmudh $v29, $v27, $v28.e0
    vsar $v29, COP2_ACC_HI
    vsar $v28, COP2_ACC_MD
    mfc2 \dst, $v29.e0
    mfc2 \tmp, $v28.e0
    sll \dst, 16
    andi \tmp, 0xFFFF
    or \dst, \tmp
    .endm

    #############################################################
    # cmd_adpcm_test - echo/transform self-test
    #   a0: RDRAM source (8-byte aligned)
    #   a1: RDRAM destination (8-byte aligned)
    #   a2: transfer size, pre-encoded by the CPU as DMA_SIZE(len, 1)
    #       (len multiple of 8, <= 128)
    #############################################################
    .func cmd_adpcm_test
cmd_adpcm_test:
    sll a0, 8                   # strip the rspq command byte from arg word 0
    srl a0, 8
    move s0, a0
    li s4, %lo(TEST_BUF)
    jal DMAIn
    move t0, a2

    # Add 1 to every byte (proves the SU actually ran over the data)
    andi t1, a2, 0xFFF
    addiu t1, 1                 # decode DMA_SIZE width back to a byte count
    li s1, %lo(TEST_BUF)
1:  lbu t2, 0(s1)
    addiu t2, 1
    sb t2, 0(s1)
    addiu t1, -1
    bnez t1, 1b
    addiu s1, 1

    move s0, a1
    li s4, %lo(TEST_BUF)
    jal DMAOut
    move t0, a2

    j RSPQ_Loop
    nop
    .endfunc

    #############################################################
    # cmd_adpcm_run - decode one chunk
    #   a0 = param RDRAM physical, a1 = out RDRAM physical, a2 = seq
    #
    # Register map (main):
    #   s5 = out_phys   s6 = seq   s7 = out byte offset of current
    #   64-sample half   s8 = samples remaining   s2 = nh (this half)
    #############################################################
    .func cmd_adpcm_run
cmd_adpcm_wp:
    sw a3, %lo(DYN_PHYS)
    li t0, 1
    j adpcm_run_common
    sw t0, %lo(WP_MODE)
cmd_adpcm_run:
    sw zero, %lo(WP_MODE)
adpcm_run_common:
    sll a0, 8                   # strip the rspq command byte from arg word 0
    srl a0, 8
    move s5, a1
    move s6, a2

    # param block in
    move s0, a0
    li s4, %lo(PARAM_BUF)
    jal DMAIn
    li t0, DMA_SIZE(384, 1)

    # jedi table in
    lw s0, %lo(PARAM_BUF) + 0
    li s4, %lo(JEDI_BUF)
    jal DMAIn
    li t0, DMA_SIZE(1568, 1)

    # whole-pump: overlay the resident dynamic state over the (stale) fields
    # the CPU packed, unless the channel is FRESH (param holds its key-on /
    # re-seed state). Then recompute aout from the effective acc + shipped
    # volume: aout == ((s16)acc * vol_mul) >> vol_shift & ~3 is an invariant
    # at every chunk boundary of a live channel, which makes CPU-side TL/IL
    # volume writes free (they recompute their copy from a stale acc).
    lw t0, %lo(WP_MODE)
    beqz t0, wp_in_done
    nop
    lw s0, %lo(DYN_PHYS)
    li s4, %lo(DYNA)
    jal DMAIn
    li t0, DMA_SIZE(128, 1)

    lbu t5, %lo(PARAM_BUF) + 6  # amask
    lbu t6, %lo(PARAM_BUF) + 8  # fresh-A bits
    li s1, %lo(PARAM_BUF) + 16
    li s2, %lo(DYNA)
wpa_in_loop:
    andi t0, t5, 1
    beqz t0, wpa_in_next
    andi t1, t6, 1
    bnez t1, wpa_in_aout        # fresh: param dynamic fields are current
    nop
    lw t0, 0(s2)                # acc
    sw t0, 16(s1)
    lw t0, 4(s2)                # astep
    sw t0, 20(s1)
    lbu t0, 12(s2)              # now_data
    sb t0, 28(s1)
wpa_in_aout:
    lw t1, 16(s1)               # acc (12-bit signed, sign-extended)
    lbu t2, 30(s1)              # vol_mul (small, >= 0)
    VMUL16 t0, t1, t2, t3
    lbu t2, 31(s1)              # vol_shift
    srav t0, t0, t2
    srl t0, 2
    sll t0, 2                   # & ~3
    sw t0, 24(s1)
wpa_in_next:
    srl t5, 1
    srl t6, 1
    addiu s1, 48
    addiu s2, 16
    li t0, %lo(PARAM_BUF) + 16 + 6*48
    bne s1, t0, wpa_in_loop
    nop
    # B: overlay acc/adpcmd/prev_acc/now_data unless fresh (adpcml is
    # recomputed every sample by the decoder — no overlay-in needed)
    lbu t0, %lo(PARAM_BUF) + 7
    andi t0, 1
    beqz t0, wp_in_done
    lbu t1, %lo(PARAM_BUF) + 9  # fresh-B (delay slot: harmless load)
    bnez t1, wp_in_done
    li s1, %lo(PARAM_BUF) + 304
    li s2, %lo(DYNA) + 96
    lw t0, 0(s2)                # acc
    sw t0, 16(s1)
    lw t0, 4(s2)                # adpcmd
    sw t0, 20(s1)
    lw t0, 8(s2)                # prev_acc
    sw t0, 24(s1)
    lbu t0, 16(s2)              # now_data
    sb t0, 46(s1)
wp_in_done:

    sw zero, %lo(ARRIVED)

    lhu s8, %lo(PARAM_BUF) + 4
    move s7, zero

half_loop:
    # nh = min(64, samples remaining)
    li s2, 64
    slt t0, s8, s2
    beqz t0, 1f
    nop
    move s2, s8
1:
    # zero the accumulators (always the full 64 entries; tails just use fewer)
    li t0, %lo(ACC_L)
    li t1, 64
2:  sw zero, 0(t0)
    sw zero, 256(t0)
    addiu t1, -1
    bnez t1, 2b
    addiu t0, 4

    # ADPCM-A channels
    lbu t0, %lo(PARAM_BUF) + 6
    sw t0, %lo(CH_MASK)
    li t0, %lo(PARAM_BUF) + 16
    sw t0, %lo(CH_PTR)
ch_loop:
    lw t0, %lo(CH_MASK)
    andi t1, t0, 1
    srl t0, 1
    sw t0, %lo(CH_MASK)
    beqz t1, ch_next
    lw s1, %lo(CH_PTR)
    jal adpcm_a_chan
    nop
ch_next:
    lw t0, %lo(CH_PTR)
    addiu t0, 48
    sw t0, %lo(CH_PTR)
    li t1, %lo(PARAM_BUF) + 16 + 6*48
    bne t0, t1, ch_loop
    nop

    # ADPCM-B
    lbu t0, %lo(PARAM_BUF) + 7
    andi t0, 1
    beqz t0, no_b
    nop
    jal adpcm_b_chan
    li s1, %lo(PARAM_BUF) + 304
no_b:

    # DMA the accumulator halves out: L -> out+s7, R -> out+512+s7
    sll t4, s2, 2               # nh*4 bytes
    addiu t4, 7
    srl t4, 3
    sll t4, 3                   # round up to multiple of 8
    addiu t4, -1                # DMA_SIZE(len, 1)
    li s4, %lo(ACC_L)
    addu s0, s5, s7
    jal DMAOut
    move t0, t4
    li s4, %lo(ACC_R)
    addu s0, s5, s7
    addiu s0, 512
    jal DMAOut
    move t0, t4

    addiu s7, 256
    subu s8, s2
    bnez s8, half_loop
    nop

    # whole-pump: chain the post-chunk dynamic fields into the resident
    # block and DMA it home (must complete BEFORE the final seq DMA — the
    # CPU's drain/pull waits on seq)
    lw t0, %lo(WP_MODE)
    beqz t0, wp_out_done
    nop
    lbu t5, %lo(PARAM_BUF) + 6
    li s1, %lo(PARAM_BUF) + 16
    li s2, %lo(DYNA)
wpa_out_loop:
    andi t0, t5, 1
    beqz t0, wpa_out_next
    nop
    lw t0, 16(s1)               # acc
    sw t0, 0(s2)
    lw t0, 20(s1)               # astep
    sw t0, 4(s2)
    lw t0, 24(s1)               # aout
    sw t0, 8(s2)
    lbu t0, 28(s1)              # now_data
    sb t0, 12(s2)
wpa_out_next:
    srl t5, 1
    addiu s1, 48
    addiu s2, 16
    li t0, %lo(PARAM_BUF) + 16 + 6*48
    bne s1, t0, wpa_out_loop
    nop
    lbu t0, %lo(PARAM_BUF) + 7
    andi t0, 1
    beqz t0, wpa_out_b_done
    li s1, %lo(PARAM_BUF) + 304 # (delay slot: harmless)
    li s2, %lo(DYNA) + 96
    lw t0, 16(s1)               # acc
    sw t0, 0(s2)
    lw t0, 20(s1)               # adpcmd
    sw t0, 4(s2)
    lw t0, 24(s1)               # prev_acc
    sw t0, 8(s2)
    lw t0, 48(s1)               # adpcml
    sw t0, 12(s2)
    lbu t0, 46(s1)              # now_data
    sb t0, 16(s2)
wpa_out_b_done:
    lw s0, %lo(DYN_PHYS)
    li s4, %lo(DYNA)
    jal DMAOut
    li t0, DMA_SIZE(128, 1)
wp_out_done:

    # echo the (state-updated) param block to out+1024
    li s4, %lo(PARAM_BUF)
    addiu s0, s5, 1024
    jal DMAOut
    li t0, DMA_SIZE(384, 1)

    # arrived + seq, the FINAL 8 bytes at out+1408 (CPU polls seq)
    sw s6, %lo(SEQ_OUT)
    li s4, %lo(ARRIVED)
    addiu s0, s5, 1408
    jal DMAOut
    li t0, DMA_SIZE(8, 1)

    j RSPQ_Loop
    nop
    .endfunc

    #############################################################
    # adpcm_a_chan - decode s2 samples of one ADPCM-A channel,
    # accumulating into ACC_L/ACC_R. s1 = channel param DMEM ptr.
    #
    # Mirrors OPNB_ADPCMA_calc_chan exactly, including:
    #  - end-of-sample: contribution 0 for that sample and the rest
    #    of the chunk, aout NOT recomputed, state frozen (C's early
    #    return + the "i < n && ch->flag" loop guard).
    #  - 12-bit signed accumulator wrap after every nibble.
    #  - aout = ((s16)acc * vol_mul) >> vol_shift & ~3, recomputed
    #    only on samples that consumed nibbles.
    #
    # Regs: t8=now_addr t7=now_step t6=step t5=end_x2m t4=acc
    #       t3=astep s3=aout a3=now_data k0=vol_mul k1=vol_shift
    #       v0=maskL v1=maskR t9=0x1FFFFF a0=src ptr a1=acc ptr
    #       a2=nibble count s0=sample counter t0-t2=scratch
    #############################################################
    .func adpcm_a_chan
adpcm_a_chan:
    sw ra, %lo(RA_SAVE)
    lbu t0, 46(s1)              # flag_out: 0 = ended in an earlier half
    beqz t0, a_ret
    nop

    # source slice: DMA 176 bytes from src_phys + (src_cur & ~7)
    lhu t0, 44(s1)
    sw t0, %lo(CUR_SAVE)
    srl t1, t0, 3
    sll t1, 3
    lw s0, 40(s1)
    addu s0, t1
    li s4, %lo(SRC_BUF)
    jal DMAIn
    li t0, DMA_SIZE(176, 1)

    lw t0, %lo(CUR_SAVE)
    andi t0, 7
    addiu a0, t0, %lo(SRC_BUF)  # a0 = DMEM addr of next source byte

    lw t8, 0(s1)
    lw t7, 4(s1)
    lw t6, 8(s1)
    lw t5, 12(s1)
    lw t4, 16(s1)
    lw t3, 20(s1)
    lw s3, 24(s1)
    lbu a3, 28(s1)
    lbu k0, 30(s1)
    lbu k1, 31(s1)
    lw v0, 32(s1)
    lw v1, 36(s1)
    lui t9, 0x1F
    ori t9, 0xFFFF              # 0x1FFFFF: YM2610 compares 21 addr bits
    move s0, s2
    li a1, %lo(ACC_L)

a_sample:
    addu t7, t6                 # now_step += step
    srl a2, t7, 16
    beqz a2, a_emit             # no nibble this sample: contribute old aout
    andi t7, 0xFFFF

a_nib:
    and t0, t8, t9              # end check (21 bits)
    beq t0, t5, a_end_hit
    andi t1, t8, 1
    bnez t1, a_odd
    nop
    lbu a3, 0(a0)               # even nibble: fetch next source byte
    addiu a0, 1
    srl t2, a3, 4
    b a_have
    andi t2, 0xF
a_odd:
    andi t2, a3, 0xF
a_have:
    addiu t8, 1                 # now_addr++

    addu t0, t3, t2             # jedi_table[astep + data]
    sll t0, 1
    lh t1, %lo(JEDI_BUF)(t0)
    addu t4, t1
    sll t4, 20                  # 12-bit signed wrap
    sra t4, 20

    andi t0, t2, 7              # astep += step_inc[data & 7], clamp [0,768]
    sll t0, 1
    lh t1, %lo(STEP_INC)(t0)
    addu t3, t1
    bgez t3, 1f
    nop
    move t3, zero
1:  slti t0, t3, 769
    bnez t0, 2f
    addiu a2, -1
    li t3, 768
2:  bnez a2, a_nib
    nop

    # aout = (acc * vol_mul) >> vol_shift & ~3   (acc is 12-bit signed)
    VMUL16 t0, t4, k0, t1
    srav t0, t0, k1
    srl t0, 2
    sll t0, 2                   # & ~3
    move s3, t0

a_emit:
    and t0, s3, v0
    lw t1, 0(a1)
    addu t1, t0
    sw t1, 0(a1)
    and t0, s3, v1
    lw t1, 256(a1)
    addu t1, t0
    sw t1, 256(a1)
    addiu a1, 4
    addiu s0, -1
    bnez s0, a_sample
    nop

a_store:
    sw t8, 0(s1)
    sw t7, 4(s1)
    sw t4, 16(s1)
    sw t3, 20(s1)
    sw s3, 24(s1)
    sb a3, 28(s1)
    # src_cur = (saved cur & ~7) + bytes consumed from SRC_BUF
    lw t0, %lo(CUR_SAVE)
    srl t0, 3
    sll t0, 3
    li t1, %lo(SRC_BUF)
    subu t1, a0, t1
    addu t0, t1
    sh t0, 44(s1)
a_ret:
    lw ra, %lo(RA_SAVE)
    jr ra
    nop

a_end_hit:
    # flag=0, arrived |= flagMask; this sample and the rest contribute 0
    sb zero, 46(s1)
    lbu t0, 29(s1)
    lw t1, %lo(ARRIVED)
    or t1, t0
    b a_store
    sw t1, %lo(ARRIVED)
    .endfunc

    #############################################################
    # adpcm_b_chan - decode s2 samples of the ADPCM-B (deltaT)
    # channel, folding (adpcml >> 9) into ACC_L/ACC_R (the same
    # value pass 4 would have added from dtb[]). s1 = param ptr.
    #
    # Mirrors OPNB_ADPCMB_CALC restricted to the linear case (the
    # CPU already guaranteed limit-wrap / repeat-restart cannot
    # happen this chunk). End-stop: portstate=0 (flag_out=0),
    # adpcml=0, prev_acc=0, contribution 0 from that sample on.
    #
    # Regs: t8=now_addr t7=now_step t6=step t5=end_x2 t4=acc
    #       t3=adpcmd s3=prev_acc a3=now_data k0=volume k1=adpcml
    #       v0=maskL v1=maskR t9=0x1FFFFFF a0=src ptr a1=acc ptr
    #       a2=nibble count s0=sample counter t0-t2,s4=scratch
    #############################################################
    .func adpcm_b_chan
adpcm_b_chan:
    sw ra, %lo(RA_SAVE)
    lbu t0, 47(s1)              # flag_out: 0 = stopped in an earlier half
    beqz t0, b_ret
    nop

    lhu t0, 44(s1)
    sw t0, %lo(CUR_SAVE)
    srl t1, t0, 3
    sll t1, 3
    lw s0, 40(s1)
    addu s0, t1
    li s4, %lo(SRC_BUF)
    jal DMAIn
    li t0, DMA_SIZE(176, 1)

    lw t0, %lo(CUR_SAVE)
    andi t0, 7
    addiu a0, t0, %lo(SRC_BUF)

    lw t8, 0(s1)
    lw t7, 4(s1)
    lw t6, 8(s1)
    lw t5, 12(s1)
    lw t4, 16(s1)
    lw t3, 20(s1)
    lw s3, 24(s1)
    lw k0, 28(s1)               # volume
    lw v0, 32(s1)
    lw v1, 36(s1)
    lbu a3, 46(s1)
    lui t9, 0x1FF
    ori t9, 0xFFFF              # 0x1FFFFFF: 24+1 bit address wrap
    move s0, s2
    li a1, %lo(ACC_L)

b_sample:
    addu t7, t6
    srl a2, t7, 16
    beqz a2, b_interp
    andi t7, 0xFFFF

b_nib:
    beq t8, t5, b_end_hit       # end-stop check (exact compare)
    andi t1, t8, 1
    bnez t1, b_odd
    nop
    lbu a3, 0(a0)
    addiu a0, 1
    b b_have
    srl t2, a3, 4
b_odd:
    andi t2, a3, 0xF
b_have:
    addiu t8, 1
    and t8, t9                  # now_addr &= (1<<25)-1
    move s3, t4                 # prev_acc = acc

    # acc += BDEC1[data] * adpcmd / 8 (C truncating division), clamp +-32767
    sll t0, t2, 1
    lh t1, %lo(BDEC1)(t0)
    VMUL16 t0, t1, t3, k1       # BDEC1[data] (s5) * adpcmd (<= 24576)
    sra k1, t0, 31
    andi k1, 7
    addu t0, k1
    sra t0, 3
    addu t4, t0
    li k1, 32767
    slt t0, k1, t4
    beqz t0, 1f
    nop
    move t4, k1
1:  li k1, -32768
    slt t0, t4, k1
    beqz t0, 2f
    nop
    move t4, k1
2:
    # adpcmd = adpcmd * BDEC2[data] / 64, clamp [127, 24576]
    sll t0, t2, 1
    lh t1, %lo(BDEC2)(t0)
    VMUL16 t0, t3, t1, k1       # adpcmd (<= 24576) * table (<= 153)
    srl t3, t0, 6
    li k1, 24576
    slt t0, k1, t3
    beqz t0, 3f
    nop
    move t3, k1
3:  li k1, 127
    slt t0, t3, k1
    beqz t0, 4f
    addiu a2, -1
    move t3, k1
4:  bnez a2, b_nib
    nop

b_interp:
    # adpcml = ((prev*(65536-now_step) + acc*now_step) >> 16) * volume
    # (the C core's own formulation). Both products are s16 * u16 and sum
    # exactly in the 48-bit ACC (|sum| <= 2^31), so ACC[47:16] IS the
    # arithmetic >>16 of the s32 sum. now_step==0 short-circuits to prev
    # (the u16 lane can't hold 65536; prev*65536>>16 == prev exactly).
    beqz t7, 1f
    move t0, s3
    subu t1, zero, t7           # (65536 - step) & 0xFFFF, step in [1,65535]
    mtc2 s3, $v25.e0            # prev
    mtc2 t1, $v26.e0            # 65536-step (u16)
    mtc2 t4, $v27.e0            # acc
    mtc2 t7, $v28.e0            # step (u16)
    vmudm $v29, $v25, $v26.e0   # ACC  = prev * (65536-step)   (s16 x u16)
    vmadm $v29, $v27, $v28.e0   # ACC += acc * step            (s16 x u16)
    vsar $v29, COP2_ACC_HI
    vsar $v28, COP2_ACC_MD
    mfc2 t0, $v29.e0
    mfc2 t1, $v28.e0
    sll t0, 16
    andi t1, 0xFFFF
    or t0, t1                   # (prev<<16 + (acc-prev)*step) >> 16
1:
    VMUL16 k1, t0, k0, t1       # adpcml = interp (s16) * volume (<= 255)

    sra t0, k1, 9               # pass-4 fold: dtb[i] >> 9
    and t2, t0, v0
    lw t1, 0(a1)
    addu t1, t2
    sw t1, 0(a1)
    and t2, t0, v1
    lw t1, 256(a1)
    addu t1, t2
    sw t1, 256(a1)
    addiu a1, 4
    addiu s0, -1
    bnez s0, b_sample
    nop

b_store:
    sw t8, 0(s1)
    sw t7, 4(s1)
    sw t4, 16(s1)
    sw t3, 20(s1)
    sw s3, 24(s1)
    sb a3, 46(s1)
    sw k1, 48(s1)
    lw t0, %lo(CUR_SAVE)
    srl t0, 3
    sll t0, 3
    li t1, %lo(SRC_BUF)
    subu t1, a0, t1
    addu t0, t1
    sh t0, 44(s1)
b_ret:
    lw ra, %lo(RA_SAVE)
    jr ra
    nop

b_end_hit:
    # EOS: arrived |= eosbit, portstate off, adpcml = prev_acc = 0,
    # contribution 0 from this sample on
    lbu t0, 52(s1)
    lw t1, %lo(ARRIVED)
    or t1, t0
    sw t1, %lo(ARRIVED)
    sb zero, 47(s1)
    move k1, zero
    b b_store
    move s3, zero
    .endfunc
