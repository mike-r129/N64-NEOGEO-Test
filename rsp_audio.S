# RSP overlay for YM2610 ADPCM offload (rspq).
#
# Design notes (why this overlay looks the way it does):
#  - Commands are MONOLITHIC: each command DMAs in everything it needs and DMAs
#    out everything it produced. No state is kept in DMEM between commands, so
#    there is no RSPQ_BeginSavedState block: rspq may interleave rsp_video
#    commands (which clobber the overlay data/bss area on the switch) between
#    any two audio commands without corrupting us.
#  - The CPU side (ym2610.c / platform_n64.c) builds a parameter block in RDRAM
#    (cache-written-back) and polls an uncached sequence word at the END of the
#    output block. A single DMAOut writes RDRAM in ascending order, so when the
#    final word is visible the whole block is.
#
# 0x0 cmd_adpcm_test: DMA-in `size` bytes from a0, add 1 to each byte, DMA-out
#     to a1. Boot-time self-test that the overlay is registered, commands
#     dispatch, and both DMA directions work.

#include <rsp_queue.inc>

    .data

    RSPQ_BeginOverlayHeader
        RSPQ_DefineCommand cmd_adpcm_test, 12   # 0x0: a0=src, a1=dst, a2=size
    RSPQ_EndOverlayHeader

    RSPQ_EmptySavedState

    .bss
    .align 4
TEST_BUF: .dcb.b 128

    .text

    #############################################################
    # cmd_adpcm_test — echo/transform self-test
    #   a0: RDRAM source (8-byte aligned)
    #   a1: RDRAM destination (8-byte aligned)
    #   a2: transfer size, pre-encoded by the CPU as DMA_SIZE(len, 1)
    #       (len multiple of 8, <= 128)
    #############################################################
    .func cmd_adpcm_test
cmd_adpcm_test:
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
