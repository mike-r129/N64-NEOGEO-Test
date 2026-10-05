# RSP overlay for YM2610 FM synthesis offload (rspq).
#
# Bit-exact port of the CPU pass-1 loop in YM2610Update_stream: per chunk
# (<=128 samples), for each shipped FM channel, replay the shared EG tick
# schedule (advance_eg_channel), synthesize each sample (chan_calc_stream)
# and advance the phase generators, folding (carrier>>1)&pan into per-sample
# L/R contribution arrays. Same monolithic/no-saved-state/high-priority-queue
# design as rsp_audio.S (see there for the rationale).
#
# What the CPU guarantees per shipped channel (ym2610.c rspfm_* glue):
#  - No slot uses SSG-EG (ssg&8): the EG port below has no swap/negate logic.
#  - The per-sample phase deltas come from a per-channel dp table indexed by
#    a per-sample pmidx byte (CPU precomputed via update_phase_lfo's math,
#    covering LFO-PM); non-PMS channels use dp[0] = the four Incr values
#    (pms_mask forces index 0).
#  - Register writes cannot occur mid-chunk (they happen between update
#    calls), so tl/sl/rates/algo/pan are chunk constants.
#
# The 26KB tl_tab never existed here: tl_tab_base[256] (s16) + shift/negate
# reproduces it exactly (see ym2610.c tl_tab_lookup). sin_tab[1024] is
# quarter-folded to its first 256 entries (the CPU verifies the fold against
# the real table at boot and disables the offload on any mismatch):
#   j = idx & 255;  if (idx & 256) j = 255 - j;
#   sin_tab[idx] == SINQ[j] + ((idx >> 9) & 1)      (SINQ entries are even)
#
# Commands:
#  0x0 cmd_fm_run: a0 = param block physical, a1 = out block physical,
#                  a2 = sequence word (written last; CPU poll target).
#                  The synchronous per-chunk path; no longer issued by the CPU.
#  0x1 cmd_fm_wp:  as 0x0, plus a3 = RDRAM physical of the RSP-resident
#                  dynamic-state block (whole-pump mode; see below). Shares
#                  cmd_fm_run's body.
#
# PARAM LAYOUT (header 288 bytes + 4 channel blocks of 416):
#  header:
#   +0   u32 sinq_phys     (first 256 entries of sin_tab, u16 each)
#   +4   u32 tlb_phys      (tl_tab_base, 256 x s16)
#   +8   u16 n             samples this chunk (1..128)
#   +10  u8  chmask        bit j = channel block j present
#   +11  u8  pad
#   +12  u32 eg_base       EG counter at chunk start
#   +16  u8  egt[128]      EG ticks per sample
#   +144 u8  lfo_am[128]   LFO AM level per sample
#   +272 pad to 288
#  channel block j at +288 + j*416:
#   +0   s32 op1_out[2]    +8 s32 mem_value
#   +12  s32 maskL         +16 s32 maskR
#   +20  u8 i_memc, i_om1, i_om2, i_oc1     (indices into the s[5] sums)
#   +24  u8 algo5          (1 = algorithm 5: op1 fans out to MEM/C1/C2)
#   +25  u8 fb             (feedback shift; 0 = no feedback)
#   +26  u8 ams            (AM depth shift for this channel)
#   +27  u8 pms_mask       (0xFF = use pmidx[], 0 = always dp[0])
#   +28  u16 masks: bit s   = slot s may be EG-due this chunk (else its
#                             envelope/state provably cannot change: skip)
#                  bit 8+s  = slot s is quiet-locked (vol_out >= ENV_QUIET
#                             and never due: its operator output is 0 for
#                             the whole chunk; skip env+op entirely)
#   +30  u16 pad
#   +32  u32 dp[8][4]      phase deltas per pm index, slot order S1,S3,S2,S4
#   +160 u8 pmidx[128]     per-sample dp index (garbage if pms_mask=0)
#   +288 slot blocks x4, 32 bytes each, memory order S1,S3,S2,S4:
#     +0 u32 phase  +4 s32 volume  +8 u32 vol_out  +12 pad
#     +16 u16 tl    +18 u16 sl
#     +20 u8 sh[4]  (EG rate shift, indexed state-1: REL,SUS,DEC,ATT)
#     +24 u8 sel[4] (eg_inc row offset, same indexing)
#     +28 u8 state  +29 u8 amflag (1 = AM enabled)  +30 u16 pad
#
# OUT LAYOUT (2704 bytes):
#   +0    s32 l[128]
#   +512  s32 r[128]
#   +1024 channel block echoes (4 x 416; state fields updated in place)
#   +2688 u32 seq (+pad to 2696) - written LAST
#
# Register map inside fm_chan:
#   s1=CHBUF  s2=(scratch)  s7=eg counter  s8=sample down-counter
#   a0=sample index (byte)  a1=ACC_L cursor  a2=AM this sample
#   a3=op1_out[0]  s3=op1_out[1]  s4=mem_value
#   v0=phase S1  v1=phase S3  k0=phase S2  k1=phase S4
#   t0-t7 scratch (op_calc/eg_tick internal), t8/t9 sub-scratch

#include <rsp_queue.inc>

    .data

    RSPQ_BeginOverlayHeader
        RSPQ_DefineCommand cmd_fm_run, 12    # 0x0: a0=param, a1=out, a2=seq
        RSPQ_DefineCommand cmd_fm_wp, 16     # 0x1: + a3=resident dyn block
    RSPQ_EndOverlayHeader

    RSPQ_EmptySavedState

    .align 3
# eg_inc[19*8] — identical to the C table (envelope increment patterns)
EG_INC:
    .byte 0,1,0,1,0,1,0,1
    .byte 0,1,0,1,1,1,0,1
    .byte 0,1,1,1,0,1,1,1
    .byte 0,1,1,1,1,1,1,1
    .byte 1,1,1,1,1,1,1,1
    .byte 1,1,1,2,1,1,1,2
    .byte 1,2,1,2,1,2,1,2
    .byte 1,2,2,2,1,2,2,2
    .byte 2,2,2,2,2,2,2,2
    .byte 2,2,2,4,2,2,2,4
    .byte 2,4,2,4,2,4,2,4
    .byte 2,4,4,4,2,4,4,4
    .byte 4,4,4,4,4,4,4,4
    .byte 4,4,4,8,4,4,4,8
    .byte 4,8,4,8,4,8,4,8
    .byte 4,8,8,8,4,8,8,8
    .byte 8,8,8,8,8,8,8,8
    .byte 16,16,16,16,16,16,16,16
    .byte 0,0,0,0,0,0,0,0

    .bss
    .align 4
HDR:      .dcb.b 288
SINQ:     .dcb.b 512
TLB:      .dcb.b 512
CHBUF:    .dcb.b 416
ACC_L:    .dcb.b 512      # ACC_R MUST be ACC_L+512 (fixed offset in code)
ACC_R:    .dcb.b 512
SBUF:     .dcb.b 20       # the s[5] connection sums (m2,c1,c2,mem,carrier)
    .align 3
SEQW:     .dcb.b 8
RA_SAVE:  .dcb.b 4
EGT_LEFT: .dcb.b 4
CH_MASK:  .dcb.b 4
CH_IDX:   .dcb.b 4
PARAM_PHYS: .dcb.b 4
# whole-pump mode (cmd_fm_wp): the FM dynamic state chains RSP-side through
# an RDRAM-resident block (4 x 80B, one per channel: op1_out[2]/mem_value +
# 4 x {phase,volume,vol_out,state} in RSP slot order S1,S3,S2,S4), overlaid
# onto each chunk's DMA'd channel block before synthesis and written back
# after. WP_MODE selects the extra steps; DYNBUF is the staging area.
WP_MODE:  .dcb.b 4
DYN_PHYS: .dcb.b 4
    .align 3                    # DMA target: DMEM address must be 8-aligned
DYNBUF:   .dcb.b 80

    .text

    #############################################################
    # MULU: dst = mcand * mult (mult >= 0), exact mod 2^32.
    #############################################################
    .macro MULU dst, mcand, mult, tmp
    move \dst, zero
    beqz \mult, 9f
    nop
8:  andi \tmp, \mult, 1
    subu \tmp, zero, \tmp
    and  \tmp, \tmp, \mcand
    addu \dst, \dst, \tmp
    srl  \mult, \mult, 1
    bnez \mult, 8b
    sll  \mcand, \mcand, 1
9:
    .endm

    #############################################################
    # cmd_fm_run
    #############################################################
    .func cmd_fm_run
cmd_fm_wp:
    sw a3, %lo(DYN_PHYS)
    li t0, 1
    j fm_run_common
    sw t0, %lo(WP_MODE)
cmd_fm_run:
    sw zero, %lo(WP_MODE)
fm_run_common:
    sll a0, 8                   # strip the rspq command byte
    srl a0, 8
    sw a0, %lo(PARAM_PHYS)
    move s5, a1
    move s6, a2

    # start stamp: out+2692 = seq (queue-latency vs compute diagnostics).
    # SEQW word 0 stays 0 here; the final DMA at the end writes it = seq.
    sw zero, %lo(SEQW)
    sw s6, %lo(SEQW) + 4
    li s4, %lo(SEQW)
    addiu s0, s5, 2688
    jal DMAOut
    li t0, DMA_SIZE(8, 1)

    # header in
    lw s0, %lo(PARAM_PHYS)
    li s4, %lo(HDR)
    jal DMAIn
    li t0, DMA_SIZE(288, 1)

    # sin quarter table + tl base table in
    lw s0, %lo(HDR) + 0
    li s4, %lo(SINQ)
    jal DMAIn
    li t0, DMA_SIZE(512, 1)
    lw s0, %lo(HDR) + 4
    li s4, %lo(TLB)
    jal DMAIn
    li t0, DMA_SIZE(512, 1)

    # zero the full-chunk accumulators
    li t0, %lo(ACC_L)
    li t1, 128
1:  sw zero, 0(t0)
    sw zero, 512(t0)
    addiu t1, -1
    bnez t1, 1b
    addiu t0, 4

    # channel loop
    lbu t0, %lo(HDR) + 10
    sw t0, %lo(CH_MASK)
    sw zero, %lo(CH_IDX)
ch_loop:
    lw t0, %lo(CH_MASK)
    andi t1, t0, 1
    srl t0, 1
    sw t0, %lo(CH_MASK)
    beqz t1, ch_next
    lw t2, %lo(CH_IDX)
    # DMA channel block in: param + 288 + idx*416
    sll t3, t2, 8
    sll t4, t2, 7
    addu t3, t4                 # idx*384
    sll t4, t2, 5
    addu t3, t4                 # idx*416
    lw s0, %lo(PARAM_PHYS)
    addu s0, t3
    addiu s0, 288
    li s4, %lo(CHBUF)
    jal DMAIn
    li t0, DMA_SIZE(416, 1)

    # whole-pump: overlay the RSP-resident dynamic state over the (stale)
    # dynamic fields the CPU packed, then apply the chunk's net key events
    # and recompute vol_out = tl + volume (exact: the CPU hatches any
    # SSG-EG use, and this core's key handlers never touch volume).
    lw t0, %lo(WP_MODE)
    beqz t0, wp_in_done
    nop
    lw t2, %lo(CH_IDX)
    sll t3, t2, 6
    sll t4, t2, 4
    addu t3, t4                 # idx*80
    lw s0, %lo(DYN_PHYS)
    addu s0, t3
    li s4, %lo(DYNBUF)
    jal DMAIn
    li t0, DMA_SIZE(80, 1)
    # channel header: op1_out[0], op1_out[1], mem_value
    lw t0, %lo(DYNBUF) + 0
    sw t0, %lo(CHBUF) + 0
    lw t0, %lo(DYNBUF) + 4
    sw t0, %lo(CHBUF) + 4
    lw t0, %lo(DYNBUF) + 8
    sw t0, %lo(CHBUF) + 8
    # key-event byte for this channel: HDR+272+idx, 2 bits per slot in RSP
    # slot order (0=none 1=ON 2=OFF 3=ON_then_OFF)
    lw t2, %lo(CH_IDX)
    li t6, %lo(HDR) + 272
    addu t6, t2
    lbu t5, 0(t6)
    li t7, 4                    # slot counter
    li t8, %lo(DYNBUF) + 16
    li t9, %lo(CHBUF) + 288
wp_slot_in:
    lw t0, 0(t8)                # phase
    lw t1, 4(t8)                # volume
    lw t3, 12(t8)               # state (widened to u32 in the dyn block)
    andi t4, t5, 3
    beqz t4, wp_apply
    srl t5, 2
    li t2, 2
    beq t4, t2, wp_key_off
    li t2, 1                    # ON and ON_OFF both restart the phase
    move t0, zero
    beq t4, t2, wp_apply
    li t3, 4                    # ON: state = EG_ATT(4)
    j wp_apply
    li t3, 1                    # ON_OFF: state = EG_REL(1), unconditional
wp_key_off:
    slt t2, t2, t3              # OFF: only an active slot (state>1) releases
    beqz t2, wp_apply
    nop
    li t3, 1
wp_apply:
    sw t0, 0(t9)                # phase
    sw t1, 4(t9)                # volume
    lhu t2, 16(t9)              # tl (static, from the fresh CPU pack)
    addu t2, t1
    sw t2, 8(t9)                # vol_out = tl + volume
    sb t3, 28(t9)               # state
    addiu t7, -1
    addiu t8, 16
    bnez t7, wp_slot_in
    addiu t9, 32
wp_in_done:

    # (whole-pump only) silent-channel fast path, the RSP mirror of the CPU
    # rule: four slots parked in EG_OFF at quiet-locked attenuation with
    # decayed feedback history contribute exact zero and cannot change state
    # this chunk — only the phase counters advance. This is the difference
    # between ~4 and ~1-2 fully synthesized channels per chunk in-game, and
    # the RSP is the pipeline bottleneck ([RSPWAIT] adpcm≈11ms/pump without
    # it). Runs after key events: a key-on this chunk fails the state test.
    lw t0, %lo(WP_MODE)
    beqz t0, wp_synth
    nop
    lw t0, %lo(CHBUF) + 0
    lw t1, %lo(CHBUF) + 4
    or t0, t1
    lw t1, %lo(CHBUF) + 8
    or t0, t1
    bnez t0, wp_synth           # feedback/MEM history not decayed
    li t7, 4
    li t9, %lo(CHBUF) + 288
wp_sil_chk:
    lbu t1, 28(t9)              # state must be EG_OFF(0)
    bnez t1, wp_synth
    lw t1, 8(t9)                # vol_out must be quiet-locked (>= 832)
    sltiu t2, t1, 832
    bnez t2, wp_synth
    addiu t7, -1
    bnez t7, wp_sil_chk
    addiu t9, 32
    # silent: batch phase advance only
    lhu t6, %lo(HDR) + 8        # n
    lbu t0, %lo(CHBUF) + 27     # pms_mask
    bnez t0, wp_sil_pm
    li t7, 4
    li t8, %lo(CHBUF) + 32      # dp[0][s]
    li t9, %lo(CHBUF) + 288
wp_sil_nopm:
    lw t0, 0(t8)
    move t3, t6
    MULU t1, t0, t3, t2
    lw t2, 0(t9)
    addu t2, t1
    sw t2, 0(t9)
    addiu t7, -1
    addiu t8, 4
    bnez t7, wp_sil_nopm
    addiu t9, 32
    j wp_synth_done
    nop
wp_sil_pm:
    # per-sample dp[pmidx[i]] phase advance (same tables the synthesis
    # path uses, so the phase trajectory is identical)
    li t4, %lo(CHBUF) + 160     # pmidx[]
wp_sil_pm_loop:
    lbu t5, 0(t4)
    sll t5, 4
    addiu t5, %lo(CHBUF) + 32
    lw t0, 0(t5)
    lw t1, %lo(CHBUF) + 288 + 0
    addu t1, t0
    sw t1, %lo(CHBUF) + 288 + 0
    lw t0, 4(t5)
    lw t1, %lo(CHBUF) + 288 + 32
    addu t1, t0
    sw t1, %lo(CHBUF) + 288 + 32
    lw t0, 8(t5)
    lw t1, %lo(CHBUF) + 288 + 64
    addu t1, t0
    sw t1, %lo(CHBUF) + 288 + 64
    lw t0, 12(t5)
    lw t1, %lo(CHBUF) + 288 + 96
    addu t1, t0
    sw t1, %lo(CHBUF) + 288 + 96
    addiu t6, -1
    bnez t6, wp_sil_pm_loop
    addiu t4, 1
    j wp_synth_done
    nop

wp_synth:
    jal fm_chan
    li s1, %lo(CHBUF)
wp_synth_done:

    # whole-pump: write the evolved dynamic state back to the resident block
    lw t0, %lo(WP_MODE)
    beqz t0, wp_out_done
    nop
    lw t0, %lo(CHBUF) + 0
    sw t0, %lo(DYNBUF) + 0
    lw t0, %lo(CHBUF) + 4
    sw t0, %lo(DYNBUF) + 4
    lw t0, %lo(CHBUF) + 8
    sw t0, %lo(DYNBUF) + 8
    li t7, 4
    li t8, %lo(DYNBUF) + 16
    li t9, %lo(CHBUF) + 288
wp_slot_out:
    lw t0, 0(t9)
    sw t0, 0(t8)
    lw t0, 4(t9)
    sw t0, 4(t8)
    lw t0, 8(t9)
    sw t0, 8(t8)
    lbu t0, 28(t9)
    sw t0, 12(t8)
    addiu t7, -1
    addiu t8, 16
    bnez t7, wp_slot_out
    addiu t9, 32
    lw t2, %lo(CH_IDX)
    sll t3, t2, 6
    sll t4, t2, 4
    addu t3, t4
    lw s0, %lo(DYN_PHYS)
    addu s0, t3
    li s4, %lo(DYNBUF)
    jal DMAOut
    li t0, DMA_SIZE(80, 1)
wp_out_done:

    # echo the updated channel block: out + 1024 + idx*416
    lw t2, %lo(CH_IDX)
    sll t3, t2, 8
    sll t4, t2, 7
    addu t3, t4
    sll t4, t2, 5
    addu t3, t4
    addu s0, s5, t3
    addiu s0, 1024
    li s4, %lo(CHBUF)
    jal DMAOut
    li t0, DMA_SIZE(416, 1)

ch_next:
    lw t2, %lo(CH_IDX)
    addiu t2, 1
    sw t2, %lo(CH_IDX)
    li t3, 4
    bne t2, t3, ch_loop
    nop

    # accumulators out (tail-rounded to 8)
    lhu t4, %lo(HDR) + 8
    sll t4, 2
    addiu t4, 7
    srl t4, 3
    sll t4, 3
    addiu t4, -1                # DMA_SIZE(nbytes, 1)
    li s4, %lo(ACC_L)
    move s0, s5
    jal DMAOut
    move t0, t4
    li s4, %lo(ACC_R)
    addiu s0, s5, 512
    jal DMAOut
    move t0, t4

    # seq word, final 8 bytes at out+2688
    sw s6, %lo(SEQW)
    li s4, %lo(SEQW)
    addiu s0, s5, 2688
    jal DMAOut
    li t0, DMA_SIZE(8, 1)

    j RSPQ_Loop
    nop
    .endfunc

    #############################################################
    # op_calc — one operator.
    #  in:  t0 = phase (raw counter), t1 = env (< 832, checked by
    #       caller), t2 = pm term ALREADY shifted as the C code
    #       shifts it (out<<FB for op1, s[x]<<15 for modulators)
    #  out: t0 = operator output
    #  clobbers t3..t7
    #############################################################
    .func op_calc
op_calc:
    lui t3, 0xFFFF
    and t3, t0, t3              # phase & ~FREQ_MASK
    addu t3, t2
    sra t3, 16
    andi t3, 1023               # sin index
    # quarter fold: j = idx&255, (idx&256) -> j = 255-j
    andi t4, t3, 255
    andi t5, t3, 256
    beqz t5, 1f
    srl t6, t3, 9               # sign bit (delay slot, both paths)
    xori t4, 255
1:  sll t4, 1
    lhu t5, %lo(SINQ)(t4)
    addu t5, t6                 # + sign (SINQ entries are even)
    sll t6, t1, 3               # env << 3
    addu t5, t6                 # p
    slti t6, t5, 6656           # p >= TL_TAB_LEN -> 0
    beqz t6, 2f
    move t0, zero
    # v = TLB[(p>>1)&255] >> (p>>9); negate if p&1
    srl t6, t5, 1
    andi t6, 255
    sll t6, 1
    lh t7, %lo(TLB)(t6)
    srl t6, t5, 9
    srav t7, t7, t6
    andi t6, t5, 1
    beqz t6, 3f
    move t0, t7
    subu t0, zero, t7
3:
2:  jr ra
    nop
    .endfunc

    #############################################################
    # EG_SLOT — advance one slot's envelope for the EG count in s7,
    # skipped entirely when the CPU proved it cannot be due this
    # chunk (s2 due bit). No SSG-EG (CPU never ships such channels).
    # \off = slot byte offset (0/32/64/96), \bit = due-mask bit.
    # Clobbers t0..t8. Slot fields relative to s1 (CHBUF).
    #############################################################
    .macro EG_SLOT off, bit
    andi t0, s2, \bit
    beqz t0, 6f
    nop
    lbu t0, 316+\off (s1)       # state (0=OFF 1=REL 2=SUS 3=DEC 4=ATT)
    beqz t0, 6f
    addu t1, s1, t0
    lbu t2, 307+\off (t1)       # sh[state-1]  (sh[] at slot+20)
    li t3, 1
    sllv t3, t3, t2
    addiu t3, -1
    and t3, s7, t3
    bnez t3, 6f                 # not due at this eg count
    nop
    lbu t4, 311+\off (t1)       # sel[state-1] (sel[] at slot+24)
    srlv t5, s7, t2
    andi t5, 7
    addu t4, t5
    lbu t4, %lo(EG_INC)(t4)     # inc
    lw t5, 292+\off (s1)        # volume
    li t6, 4
    beq t0, t6, 4f              # EG_ATT
    li t6, 3
    beq t0, t6, 3f              # EG_DEC
    li t6, 2
    beq t0, t6, 2f              # EG_SUS
    nop
    # EG_REL
    addu t5, t4
    slti t6, t5, 1023           # C: volume >= MAX_ATT_INDEX (1023)
    bnez t6, 7f
    nop
    li t5, 1023
    b 7f
    sb zero, 316+\off (s1)      # state = EG_OFF
2:  # EG_SUS
    addu t5, t4
    slti t6, t5, 1023
    bnez t6, 7f
    nop
    b 7f
    li t5, 1023                 # clamp, state unchanged
3:  # EG_DEC
    addu t5, t4
    lhu t6, 306+\off (s1)       # sl (slot+18)
    slt t6, t5, t6
    bnez t6, 7f
    nop
    li t6, 2
    b 7f
    sb t6, 316+\off (s1)        # state = EG_SUS
4:  # EG_ATT: volume += (~volume * inc) >> 4
    nor t6, t5, zero
    MULU t7, t6, t4, t8
    sra t7, 4
    addu t5, t7
    bgtz t5, 7f
    nop
    move t5, zero
    li t6, 3
    sb t6, 316+\off (s1)        # state = EG_DEC
7:  sw t5, 292+\off (s1)        # volume
    lhu t6, 304+\off (s1)       # tl (slot+16)
    addu t6, t5
    sw t6, 296+\off (s1)        # vol_out = tl + volume
6:
    .endm

    .func eg_tick
eg_tick:
    EG_SLOT 0, 0x1
    EG_SLOT 32, 0x2
    EG_SLOT 64, 0x4
    EG_SLOT 96, 0x8
    jr ra
    nop
    .endfunc

    #############################################################
    # fm_chan — synthesize s8=n samples of one channel (CHBUF).
    #############################################################
    .func fm_chan
fm_chan:
    sw ra, %lo(RA_SAVE)
    lw a3, 0(s1)                # op1_out[0]
    lw s3, 4(s1)                # op1_out[1]
    lw s4, 8(s1)                # mem_value
    lhu s2, 28(s1)              # masks: due (bits 0-3) | quiet-locked (8-11)
    lw v0, 288(s1)              # phase S1
    lw v1, 320(s1)              # phase S3
    lw k0, 352(s1)              # phase S2
    lw k1, 384(s1)              # phase S4
    lw s7, %lo(HDR) + 12        # eg_base
    lhu s8, %lo(HDR) + 8        # n
    move a0, zero
    li a1, %lo(ACC_L)

fm_sample:
    # ----- EG ticks for this sample -----
    lbu t0, %lo(HDR) + 16 (a0)  # egt[i]
    beqz t0, fm_eg_done
    andi t1, s2, 0xF            # due mask (delay slot, both paths)
    bnez t1, fm_eg_loop
    nop
    b fm_eg_done
    addu s7, t0                 # no due-able slot: just advance the counter
fm_eg_loop:
    sw t0, %lo(EGT_LEFT)
    addiu s7, 1                 # cnt++
    jal eg_tick
    nop
    lw t0, %lo(EGT_LEFT)
    addiu t0, -1
    bnez t0, fm_eg_loop
    nop
fm_eg_done:

    # ----- AM for this sample -----
    lbu t1, %lo(HDR) + 144 (a0) # lfo_am[i]
    lbu t2, 26(s1)              # ams
    srlv a2, t1, t2

    # ----- connection sums -----
    sw zero, %lo(SBUF) + 0
    sw zero, %lo(SBUF) + 4
    sw zero, %lo(SBUF) + 8
    sw zero, %lo(SBUF) + 12
    sw zero, %lo(SBUF) + 16
    lbu t0, 20(s1)              # i_memc
    sll t0, 2
    sw s4, %lo(SBUF)(t0)        # s[i_memc] = mem_value

    # ----- SLOT 1 (M1, feedback) -----
    addu t8, a3, s3             # out = op1[0] + op1[1]
    move a3, s3                 # op1[0] = op1[1]
    lbu t0, 24(s1)              # algo5?
    beqz t0, 1f
    lbu t1, 21(s1)              # i_om1 (delay slot, both paths)
    sw a3, %lo(SBUF) + 4        # algo5: s[C1] =
    sw a3, %lo(SBUF) + 8        #        s[C2] =
    b 2f
    sw a3, %lo(SBUF) + 12       #        s[MEM] = op1_out[0]
1:  sll t1, 2
    lw t2, %lo(SBUF)(t1)
    addu t2, a3
    sw t2, %lo(SBUF)(t1)        # s[i_om1] += op1_out[0]
2:  move s3, zero               # op1_out[1] = 0
    andi t0, s2, 0x100          # S1 quiet-locked: output 0 all chunk
    bnez t0, fm_slot3
    nop
    lw t3, 296(s1)              # S1 vol_out (288+8)
    beqz a2, 3f                 # no AM this sample
    move t1, t3
    lbu t4, 317(s1)             # S1 amflag (288+29)
    subu t4, zero, t4
    and t4, a2
    addu t1, t3, t4             # env
3:  slti t5, t1, 832
    beqz t5, fm_slot3
    nop
    lbu t6, 25(s1)              # fb
    beqz t6, 4f
    move t2, zero               # !FB -> pm = 0
    sllv t2, t8, t6             # pm = out << FB
4:  move t0, v0
    jal op_calc
    nop
    move s3, t0                 # op1_out[1]

fm_slot3:
    # ----- SLOT 3 (M2): s[i_om2] += op(phase3, env, s[M2]<<15) -----
    andi t0, s2, 0x200
    bnez t0, fm_slot2
    nop
    lw t3, 328(s1)              # S3 vol_out (320+8)
    beqz a2, 3f
    move t1, t3
    lbu t4, 349(s1)             # S3 amflag
    subu t4, zero, t4
    and t4, a2
    addu t1, t3, t4
3:  slti t5, t1, 832
    beqz t5, fm_slot2
    nop
    lw t2, %lo(SBUF) + 0        # s[M2]
    sll t2, 15
    move t0, v1
    jal op_calc
    nop
    lbu t1, 22(s1)              # i_om2
    sll t1, 2
    lw t2, %lo(SBUF)(t1)
    addu t2, t0
    sw t2, %lo(SBUF)(t1)

fm_slot2:
    # ----- SLOT 2 (C1): s[i_oc1] += op(phase2, env, s[C1]<<15) -----
    andi t0, s2, 0x400
    bnez t0, fm_slot4
    nop
    lw t3, 360(s1)              # S2 vol_out (352+8)
    beqz a2, 3f
    move t1, t3
    lbu t4, 381(s1)             # S2 amflag
    subu t4, zero, t4
    and t4, a2
    addu t1, t3, t4
3:  slti t5, t1, 832
    beqz t5, fm_slot4
    nop
    lw t2, %lo(SBUF) + 4        # s[C1]
    sll t2, 15
    move t0, k0
    jal op_calc
    nop
    lbu t1, 23(s1)              # i_oc1
    sll t1, 2
    lw t2, %lo(SBUF)(t1)
    addu t2, t0
    sw t2, %lo(SBUF)(t1)

fm_slot4:
    # ----- SLOT 4 (C2): s[CAR] += op(phase4, env, s[C2]<<15) -----
    andi t0, s2, 0x800
    bnez t0, fm_mem
    nop
    lw t3, 392(s1)              # S4 vol_out (384+8)
    beqz a2, 3f
    move t1, t3
    lbu t4, 413(s1)             # S4 amflag
    subu t4, zero, t4
    and t4, a2
    addu t1, t3, t4
3:  slti t5, t1, 832
    beqz t5, fm_mem
    nop
    lw t2, %lo(SBUF) + 8        # s[C2]
    sll t2, 15
    move t0, k1
    jal op_calc
    nop
    lw t2, %lo(SBUF) + 16
    addu t2, t0
    sw t2, %lo(SBUF) + 16

fm_mem:
    lw s4, %lo(SBUF) + 12       # mem_value = s[MEM]

    # ----- phase advance via dp[pmidx & pms_mask] -----
    lbu t0, 27(s1)              # pms_mask (0 or 0xFF)
    addu t1, s1, a0
    lbu t1, 160(t1)             # pmidx[i]
    and t1, t0
    sll t1, 4                   # x16 bytes per dp entry
    addu t1, s1
    lw t2, 32(t1)
    addu v0, t2                 # phase S1
    lw t2, 36(t1)
    addu v1, t2                 # phase S3
    lw t2, 40(t1)
    addu k0, t2                 # phase S2
    lw t2, 44(t1)
    addu k1, t2                 # phase S4

    # ----- carrier -> accumulators -----
    lw t0, %lo(SBUF) + 16
    sra t0, 1
    lw t1, 12(s1)               # maskL
    and t1, t0
    lw t2, 0(a1)
    addu t2, t1
    sw t2, 0(a1)
    lw t1, 16(s1)               # maskR
    and t1, t0
    lw t2, 512(a1)
    addu t2, t1
    sw t2, 512(a1)

    addiu a0, 1
    addiu a1, 4
    addiu s8, -1
    bnez s8, fm_sample
    nop

    # store channel state back
    sw a3, 0(s1)
    sw s3, 4(s1)
    sw s4, 8(s1)
    sw v0, 288(s1)
    sw v1, 320(s1)
    sw k0, 352(s1)
    sw k1, 384(s1)
    lw ra, %lo(RA_SAVE)
    jr ra
    nop
    .endfunc
