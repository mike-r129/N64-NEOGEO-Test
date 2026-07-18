#include <rsp_queue.inc>
#include "lib/rdp_commands.h"
#define TILE0 0
#define TILE1 1
#define TILE7 7

    .data

	RSPQ_BeginOverlayHeader
        RSPQ_DefineCommand cmd_fix_init, 4     # 0x0
		RSPQ_DefineCommand cmd_fix_draw, 8     # 0x1
        RSPQ_DefineCommand cmd_sprite_draw, 12 # 0x2
        RSPQ_DefineCommand cmd_pal_convert, 8  # 0x3
        RSPQ_DefineCommand cmd_sprite_begin, 4 # 0x4
        RSPQ_DefineCommand cmd_sprite_walk, 12 # 0x5
	RSPQ_EndOverlayHeader

RSPQ_BeginSavedState
    .align 4
PALETTE_CACHE: .half -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
LAST_PALETTE:  .long 0
RDRAM_PALETTE: .long 0
RSPQ_EndSavedState

.macro vsll_acc vdstreg, vsrcreg, qty
    .if (\qty == 7)
        vmadn \vdstreg, \vsrcreg, vshift, e(0)
    .elseif (\qty == 6)
        vmadn \vdstreg, \vsrcreg, vshift, e(1)
    .elseif (\qty == 5)
        vmadn \vdstreg, \vsrcreg, vshift, e(2)
    .elseif (\qty == 4)
        vmadn \vdstreg, \vsrcreg, vshift, e(3)
    .elseif (\qty == 3)
        vmadn \vdstreg, \vsrcreg, vshift, e(4)
    .elseif (\qty == 2)
        vmadn \vdstreg, \vsrcreg, vshift, e(5)
    .elseif (\qty == 1)
        vmadn \vdstreg, \vsrcreg, vshift, e(6)
    .elseif (\qty == 0)
        vmadn \vdstreg, \vsrcreg, vshift, e(7)
    .elseif (\qty >= 8 && \qty <= 15)
        .error "Use vsll8_acc for quantities in range 8-15"
    .else
        .error "Invalid quantity in vsll_acc"
    .endif
.endm

.macro vsrl_acc vdstreg, vsrcreg, qty
    .if (\qty == 1)
        vmadl \vdstreg, \vsrcreg, vshift8, e(0)
    .elseif (\qty == 2)
        vmadl \vdstreg, \vsrcreg, vshift8, e(1)
    .elseif (\qty == 3)
        vmadl \vdstreg, \vsrcreg, vshift8, e(2)
    .elseif (\qty == 4)
        vmadl \vdstreg, \vsrcreg, vshift8, e(3)
    .elseif (\qty == 5)
        vmadl \vdstreg, \vsrcreg, vshift8, e(4)
    .elseif (\qty == 6)
        vmadl \vdstreg, \vsrcreg, vshift8, e(5)
    .elseif (\qty == 7)
        vmadl \vdstreg, \vsrcreg, vshift8, e(6)
    .elseif (\qty == 8)
        vmadl \vdstreg, \vsrcreg, vshift8, e(7)
    .elseif (\qty >= 9 && \qty <= 15)
        .error "Use vsrl8_acc for quantities in range 9-15"
    .else
        .error "Invalid quantity in vsrl_acc"
    .endif
.endm

.macro vsrl8_acc vdstreg, vsrcreg, qty
    .if (\qty == 9)
        vmadl \vdstreg, \vsrcreg, vshift, e(0)
    .elseif (\qty == 10)
        vmadl \vdstreg, \vsrcreg, vshift, e(1)
    .elseif (\qty == 11)
        vmadl \vdstreg, \vsrcreg, vshift, e(2)
    .elseif (\qty == 12)
        vmadl \vdstreg, \vsrcreg, vshift, e(3)
    .elseif (\qty == 13)
        vmadl \vdstreg, \vsrcreg, vshift, e(4)
    .elseif (\qty == 14)
        vmadl \vdstreg, \vsrcreg, vshift, e(5)
    .elseif (\qty == 15)
        vmadl \vdstreg, \vsrcreg, vshift, e(6)
    .elseif (\qty >= 1 && \qty <= 8)
        .error "Use vsrl_acc for quantities in range 1-8"
    .else
        .error "Invalid quantity in vsrl8_acc"
    .endif
.endm

#########################################################################
# PALETTES
#########################################################################

    .data
    .align 4
VPALCONST:
    .half 0x0F00
    .half 0x00F0
    .half 0x000F
    .half 0x4000
    .half 0x2000
    .half 0x1000
    .half 0
    .half 0

VPALCONST2:
    .half 0xFFFE
    .half 0xFFFF
    .half 0xFFFF
    .half 0xFFFF
    .half 0xFFFF
    .half 0xFFFF
    .half 0xFFFF
    .half 0xFFFF

    .bss
    .align 4
PAL_BUFFER: .dcb.b 0x800


    .text

    #######################################
    # a0: RDRAM src
    # a1: RDRAM dst
    #######################################

    #define vconst     $v16
    #define valphamask $v17
    #define kR1    vconst.e0
    #define kG1    vconst.e1
    #define kB1    vconst.e2
    #define kR2    vconst.e3
    #define kG2    vconst.e4
    #define kB2    vconst.e5

    #define vcolor  $v01
    #define vr1     $v02
    #define vg1     $v03
    #define vb1     $v04
    #define vr2     $v05
    #define vg2     $v06
    #define vb2     $v07


    .func cmd_pal_convert
cmd_pal_convert:
    li s0, %lo(VPALCONST)
    lqv vconst,      0,s0
    lqv valphamask, 16,s0

    # Fetch the palettes to convert from RDRAM
    li s4, %lo(PAL_BUFFER)
    move s0, a0
    jal DMAIn
    li t0, DMA_SIZE(0x800, 1)

    li t0, 0x800
pal_loop:
    # Convert first 8 colors of the palette
    lqv vcolor, 0,s4
    vand vr1, vcolor, kR1
    vand vg1, vcolor, kG1
    vand vb1, vcolor, kB1
    vand vr2, vcolor, kR2
    vand vg2, vcolor, kG2
    vand vb2, vcolor, kB2
    vsll     vcolor, vr1, 4
    vsll_acc vcolor, vg1, 3
    vsll_acc vcolor, vb1, 2
    vsrl_acc vcolor, vr2, 3
    vsrl_acc vcolor, vg2, 7
    vsrl8_acc vcolor, vb2, 11

    # Set alpha 1 on all colors, but then turn it off on color 0,
    # which is always transparent.
    vor vcolor, vcolor, K1
    vand vcolor, valphamask
    sqv vcolor, 0,s4

    # Convert second 8 colors of the palette
    lqv vcolor, 16,s4
    vand vr1, vcolor, kR1
    vand vg1, vcolor, kG1
    vand vb1, vcolor, kB1
    vand vr2, vcolor, kR2
    vand vg2, vcolor, kG2
    vand vb2, vcolor, kB2
    vsll     vcolor, vr1, 4
    vsll_acc vcolor, vg1, 3
    vsll_acc vcolor, vb1, 2
    vsrl_acc vcolor, vr2, 3
    vsrl_acc vcolor, vg2, 7
    vsrl8_acc vcolor, vb2, 11

    # Set alpha 1 on all colors.
    vor vcolor, vcolor, K1
    sqv vcolor, 16,s4

    # Loop for all palettes
    addiu t0, -32
    bnez t0, pal_loop
    addiu s4, 32

    # Copy the converted palettes to RDRAM
    li s4, %lo(PAL_BUFFER)
    move s0, a1
    jal DMAOut
    li t0, DMA_SIZE(0x800, 1)

    j RSPQ_Loop
    nop

    .endfunc


#########################################################################
# FIX LAYER
#########################################################################

# [0xa018b8b8] fd481c030018c5b0    SET_TEX_IMAGE    dram=0018c5b0 w=4 ci8
# [0xa018b8c0] f40000000100c01c    LOAD_TILE        tile=1 st=(0.00,0.00)-(3.00,7.00)
# [0xa018b8c8] f540020000000000    SET_TILE         tile=0 ci4 tmem[0x0,line=8] pal=0 mask=[0, 0]
# [0xa018b8d0] f20000000001c01c    SET_TILE_SIZE    tile=0 st=(0.00,0.00)-(7.00,7.00)
# [0xa018b8d8] e401c01c00000000    TEX_RECT         tile=0 xy=(0.00,0.00)-(7.00,7.00)
# [0xa018b8e0] 0000000010000400                     st=(0.00,0.00) dst=(4.00000,1.00000)

    .data
    .align 4
FIX_SPRITE_LOAD:
    .quad RdpSyncPipe()
    .quad RdpSyncLoad()
    .quad RdpSyncTile()
    .quad RdpSetTexImage(RDP_TILE_FORMAT_INDEX, RDP_TILE_SIZE_8BIT, 0, 4)
    .quad RdpLoadTileI(TILE1, 0, 0, 4-1, 8-1)
    .quad 0
FIX_PALETTE_SET:
    .quad RdpSyncTile()
    .quad RdpSyncTile()
    .quad RdpSetTile(RDP_TILE_FORMAT_INDEX, RDP_TILE_SIZE_4BIT, 1, 0, TILE0)
    .quad RdpSetTileSizeI(TILE0, 0, 0, 8-1, 8-1)
FIX_SPRITE_DRAW:
    .quad RdpTextureRectangle1I(TILE0, 0, 0, 8-1, 8-1)   # x0,y1,x1,y1
    .quad RdpTextureRectangle2I(0, 0, 4, 1)              # s,t,ds,dt


    .text


    .func cmd_fix_init
cmd_fix_init:
    li t0, -1
    jr ra
    sw t0, %lo(LAST_PALETTE)
    .endfunc

    ######################################################
    # cmd_fix_draw - draw a sprite in the fix layer
    #
    # a0: sprite pointer (or NULL if same of last one)
    # a1: bit 0 .. 9: y position
    #     bit 10..19: x position
    #     bit 20..23: palette
    #
    ######################################################

    #define vspriteload0   $v01
    #define vspriteload1   $v02
    #define vspriteload2   $v03
    #define vpaletteset0   $v04
    #define vpaletteset1   $v05
    #define vfixdraw       $v06
    #define last_palette   t8
    #define ypos           t7
    #define xpos           t6

    .func cmd_fix_draw
cmd_fix_draw:
    li s0, %lo(FIX_SPRITE_LOAD)
    li s3, %lo(RDPQ_CMD_STAGING)
    lqv vspriteload0,  0,s0
    lqv vspriteload1, 16,s0
    lqv vspriteload2, 32,s0
    lqv vpaletteset0, 48,s0
    lqv vpaletteset1, 64,s0
    lqv vfixdraw,     80,s0

check_sprite_load:
    # Isolate the sprite pointer. If not zero, we must draw it
    and a0, 0x003fffff
    beqz a0, check_palette_load
    lw last_palette, %lo(LAST_PALETTE)

    # Copy the sprite load sequence into the rdpq staging buffer (16 bytes)
    sqv vspriteload0,  0,s3
    sqv vspriteload1, 16,s3
    sqv vspriteload2, 32,s3

    # Store the updated pointer
    sw a0, 28(s3)
    addi s3, 48

check_palette_load:
    # Isolate the palette number (4 bits) from a1. The palette number
    # is stored in the same position where it will go in the SET_TILE command.
    and t1, a1, 0xF << 20

    # Compare the palette with the last one we configured.
    # If it changed, we need to reconfigure the tile
    beq t1, last_palette, fix_draw
    sw t1, %lo(LAST_PALETTE)

    # Copy the palette set sequence into the rdpq staging buffer (16 bytes)
    sqv vpaletteset0,  0,s3
    sqv vpaletteset1, 16,s3

    # Store the updated palette number into the SET_TILE command
    lw t0, 20(s3)
    or t0, t1
    sw t0, 20(s3)
    addi s3, 32

fix_draw:
    # Store the draw command into the output buffer
    sqv vfixdraw, 0,s3

    # Extract x and y components from input, and store them
    # into bits 14..24 and 2..12 of t0. This is the format
    # expected for the set rectangle command.
    andi ypos, a1, 0x3ff
    srl xpos, a1, 10
    andi xpos, xpos, 0x3ff
    sll xpos, 14
    sll ypos, 2
    or t0, xpos, ypos

    lw t1, 0(s3)
    lw t2, 4(s3)
    add t1, t0
    add t2, t0
    sw t1, 0(s3)
    sw t2, 4(s3)

    addi s3, 16

    # Send to RDP and go back to main loop
    j RDPQ_Send
    li s4, %lo(RDPQ_CMD_STAGING)

    .endfunc

    #under vspriteload0
    #under vspriteload1
    #under vpaletteset0
    #under vpaletteset1
    #under vfixdraw
    #under last_palette
    #under ypos
    #under xpos


#########################################################################
# SPRITES
#########################################################################

    .data

    .align 4
RDP_SPRITE_DRAW:
    # No SyncPipe: it is only required before pipeline ATTRIBUTE changes
    # (SetOtherModes/SetCombine/...), which this stream never does. The
    # per-tile hazards are exactly what SyncTile (TILE0/TILE1 descriptor
    # reuse by the previous rect) and SyncLoad (LoadBlock over TMEM the
    # previous rect reads) cover — same policy as libdragon's rdpq
    # autosync for texrect streams. The old per-tile SyncPipe forced a
    # full pipeline drain per 16x16 tile, ~565 times a frame in fights.
    .quad RdpSyncTile()
    .quad RdpSyncLoad()
    .quad RdpSetTexImage(RDP_TILE_FORMAT_RGBA, RDP_TILE_SIZE_16BIT, 0, 4)
    .quad RdpSetTile(RDP_TILE_FORMAT_RGBA, RDP_TILE_SIZE_16BIT, 0, 0, TILE1)
    .quad RdpSetTile(RDP_TILE_FORMAT_INDEX, RDP_TILE_SIZE_4BIT, 8/8, 0, TILE0)
    .quad RdpSetTileSizeI(TILE0, 0, 0, 16-1, 16-1)
    .quad RdpLoadBlock(TILE1, 0, 0, 16*16/4, 16/2)
    .quad RdpTextureRectangle1I(TILE0, 0, 0, 0, 0)         # x0,y1,x1,y1
    .quad RdpTextureRectangle2I(0, 0, 0, 0)                # s,t,ds,dt

    .align 4
RDP_SPRITE_LOAD_PALETTE:
    #rdpq_set_texture_image_raw(0, PhysicalAddr(tlut), FMT_RGBA16, num_colors, 1);
    #rdpq_set_tile(RDPQ_TILE_INTERNAL, FMT_I4, TMEM_PALETTE_ADDR + color_idx*2*4, num_colors, 0);
    #rdpq_load_tlut_raw(RDPQ_TILE_INTERNAL, 0, num_colors);
    .quad RdpSyncPipe()
    .quad RdpSyncLoad()
    .quad RdpSyncTile()
    .quad RdpSetTexImage(RDP_TILE_FORMAT_RGBA, RDP_TILE_SIZE_16BIT, 0, 16)
    .quad RdpSetTile(RDP_TILE_FORMAT_I, RDP_TILE_SIZE_4BIT, 16/8, 0x800/8, TILE7)
    .quad RdpLoadTlut(TILE7, 0, 16)

CACHE_INDICES:
    .byte 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16

SCALE_FACTORS:
    .half (16<<10)/1, (16<<10)/2, (16<<10)/3, (16<<10)/4, (16<<10)/5, (16<<10)/6, (16<<10)/7, (16<<10)/8, (16<<10)/9, (16<<10)/10, (16<<10)/11, (16<<10)/12, (16<<10)/13, (16<<10)/14, (16<<10)/15, (16<<10)/16

    .text

    ######################################################
    # cmd_sprite_begin - begin drawing sprites
    #
    # a0: Palette RAM in RDRAM
    ################################
    .func cmd_sprite_begin
cmd_sprite_begin:
    # Initialize palette cache entries to -1
    li t0, 0xFFFF
    mtc2 t0, $v01.e0
    vor $v01, vzero, $v01.e0
    li s0, %lo(PALETTE_CACHE)
    sqv $v01, 0,s0
    sqv $v01, 16,s0

    jr ra
    sw a0, %lo(RDRAM_PALETTE)
    .endfunc



    ######################################################
    # cmd_sprite_draw - draw a sprite
    #
    # a0: sprite pointer
    # a1: bit 0 ..11: signed y position
    #     bit 12..23: signed x position
    #     bit 24..31: palette
    # a2: bit 0 .. 3: width of the sprite (0..15 meaning 1..16)
    #     bit 4 .. 7: height of the sprite (0..15 meaning 1..16)
    #     bit 8:      flip x
    #     bit 9:      flip y
    #
    ######################################################

    #define palettenum    t8
    #define palslot       t7
    #define rect_x0       t6
    #define rect_y0       t5
    #define rect_sw       k0
    #define rect_sh       k1
    #define rect_s0       s8
    #define rect_t0       s7
    #define rect_ds       s6
    #define rect_dt       s5

    #define vcache0       $v01
    #define vcache1       $v02
    #define kpalettenum   $v03.e0
    #define vindex0       $v04
    #define vindex1       $v05
    #define vsearch0      $v06
    #define vsearch1      $v07
    #define vload0        $v08
    #define vload1        $v09
    #define vload2        $v10
    #define vload3        $v11
    #define vload4        $v12
    #define vload5        $v13

    #define v___          $v27

    .func cmd_sprite_draw
cmd_sprite_draw:
    li s3, %lo(RDPQ_CMD_STAGING)

    li s0, %lo(PALETTE_CACHE)
    li s1, %lo(CACHE_INDICES)
    lqv vcache0,  0,s0
    lqv vcache1, 16,s0
    luv vindex0,  0,s1
    luv vindex1,  8,s1

    srl palettenum, a1, 24
    mtc2 palettenum, kpalettenum

    # Check if the palette is already in the cache
    veq v___, vcache0, kpalettenum
    vmrg vsearch0, vindex0, vzero
    veq v___, vcache1, kpalettenum
    vmrg vsearch1, vindex1, vzero
    vaddc vsearch0, vsearch1
    vaddc vsearch0, vsearch0.q1
    vaddc vsearch0, vsearch0.h2
    vaddc vsearch0, vsearch0.e4
    mfc2 palslot, vsearch0.e0
    bnez palslot, sprite_draw
    addi palslot, -1

    # Palette not found, we need to load it.
    # Calculate a palette slot to use. For now, just infer it from the palette
    # number. We should probably do a LRU-like, but for now this is enough.
    andi palslot, palettenum, 0xF

    li s0, %lo(RDP_SPRITE_LOAD_PALETTE)
    lqv vload0,  0,s0
    lqv vload1, 16,s0
    lqv vload2, 32,s0

    sqv vload0,  0,s3
    sqv vload1, 16,s3
    sqv vload2, 32,s3

    # Calculate palette address in RDRAM, and store it into the SetTexImage command
    lw t0, %lo(RDRAM_PALETTE)
    sll palettenum, 5
    add t0, palettenum
    lw t1, 3*8+4(s3)
    or t1, t0
    sw t1, 3*8+4(s3)

    # Calculate palette slot address and put it into the set tile command
    sll t0, palslot, 4
    lh t1, 34(s3)
    or t1, t0
    sh t1, 34(s3)

    # Go past the palette load commands
    addi s3, 48

sprite_draw:
    # The template is 9 quads (72 bytes) since the SyncPipe removal; the
    # fifth 16-byte copy over-copies 8 stale bytes past the stream end,
    # which is harmless: s3 advances by 72 and RDPQ_Send sends up to s3.
    li s0, %lo(RDP_SPRITE_DRAW)
    lqv vload0,  0,s0
    lqv vload1, 16,s0
    lqv vload2, 32,s0
    lqv vload3, 48,s0
    lqv vload4, 64,s0

    sqv vload0,  0,s3
    sqv vload1, 16,s3
    sqv vload2, 32,s3
    sqv vload3, 48,s3
    sqv vload4, 64,s3

    # Store sprite pixel address in SetTexImage
    sw a0, 2*8+4(s3)

    # Store palette number into SetTile
    lbu t0, 4*8+5(s3)
    sll t1, palslot, 4
    or t0, t1
    sb t0, 4*8+5(s3)

    # Extract X/Y
    sll rect_x0, a1, 8
    sll rect_y0, a1, 20
    sra rect_x0, 20
    sra rect_y0, 20

    # Convert the coordinates from [0..511] to [-16..496]
    li t0, 512-16
    blt rect_x0, t0, 1f
    nop
    addi rect_x0, -512
1:  blt rect_y0, t0, 2f
    nop
    addi rect_y0, -512
2:

    andi rect_sw, a2, 0xF
    srl rect_sh, a2, 4
    andi rect_sh, 0xF

    # Fetch scale factors from table
    sll rect_ds, rect_sw, 1
    sll rect_dt, rect_sh, 1
    lhu rect_ds, %lo(SCALE_FACTORS)(rect_ds)
    lhu rect_dt, %lo(SCALE_FACTORS)(rect_dt)

    # Handle negative rectangle positions
    bgez rect_x0, 1f
    li rect_s0, 0
    neg rect_s0, rect_x0
    sub rect_sw, rect_s0
    li rect_x0, 0
1:
    bgez rect_y0, 2f
    li rect_t0, 0
    neg rect_t0, rect_y0
    sub rect_sh, rect_t0
    li rect_y0, 0
2:

    # Handle flipping
    andi t0, a2, 1<<8
    beqz t0, 1f
    nop
    li t0, 16
    sub rect_s0, t0, rect_s0
    neg rect_ds, rect_ds
1:
    andi t0, a2, 1<<9
    beqz t0, 2f
    nop
    li t0, 16
    sub rect_t0, t0, rect_t0
    neg rect_dt, rect_dt
2:

    # Calculate X1/Y1 coordinates
    add rect_sw, rect_x0
    add rect_sh, rect_y0
    addi rect_sw, 1
    addi rect_sh, 1

    # Store coordinates into the rectangle primitives
    andi rect_x0, 0x3FF
    andi rect_y0, 0x3FF
    sll rect_x0, 14
    sll rect_y0, 2
    or rect_x0, rect_y0
    lw t0, 7*8+4(s3)
    or t0, rect_x0
    sw t0, 7*8+4(s3)

    andi rect_sw, 0x3FF
    andi rect_sh, 0x3FF
    sll rect_sw, 14
    sll rect_sh, 2
    or rect_sw, rect_sh
    lw t0, 7*8+0(s3)
    or t0, rect_sw
    sw t0, 7*8+0(s3)

    sll rect_s0, 5
    sll rect_t0, 5
    sh rect_s0, 8*8+0(s3)
    sh rect_t0, 8*8+2(s3)
    sh rect_ds, 8*8+4(s3)
    sh rect_dt, 8*8+6(s3)

    addi s3, 9*8

    # Send to RDP and go back to main loop
    j RDPQ_Send
    li s4, %lo(RDPQ_CMD_STAGING)

    .endfunc



#########################################################################
# SPRITE WALK (SCB -> visible-tile record list)
#########################################################################
#
# cmd_sprite_walk ports sprite_walk_produce() (video.c) bit-exactly: it
# walks the 381-sprite SCB + tilemaps out of the emulated VRAM (writeback'd
# by the CPU before the kick), applies the same chaining / vshrink / cull /
# auto-animation math, and emits the same 8-byte SprWalkRec records the C
# producer would, in the same order. No pixel pointers, no palette work:
# the CPU consume pass is unchanged. Gated by the MVS64_WALKDBG
# dual-compute build (C and RSP lists compared per frame).
#
# DMEM budget note: the 2KB tilemap staging shares PAL_BUFFER (pal_convert
# and the walk never overlap — FIFO command order). Everything else is
# dedicated: 96B SCB group, 512B list staging, 32B magic, ~40B state.
#
# Command: 0x5, 12 bytes
#   a0 (word0, cmd byte in top): VRAM physical base
#   a1: record list RDRAM physical base
#   a2: aa byte [0..7] | aa_enabled [8] | maxrecs [16..28]
# Trailer written at list + maxrecs*8: {u32 nrec, u32 ovfl}.

    .data
    .align 4
WALK_MAGIC:   # copy of VSHRINK_MAGIC (video.c)
    .byte 1, 9, 5, 13, 3, 11, 7, 15, 0, 8, 4, 12, 2, 10, 6, 14
    .byte 14, 6, 10, 2, 12, 4, 8, 0, 15, 7, 11, 3, 13, 5, 9, 1

    .bss
    .align 4
WALK_SCB:     .dcb.b 96        # one 16-sprite group: z[32] y[32] x[32]
WALK_LIST:    .dcb.b 512       # 64-record staging
WALK_TRAILER: .dcb.b 8
    .align 2
WALK_VRAM:    .dcb.b 4         # VRAM phys base
WALK_LBASE:   .dcb.b 4         # list RDRAM base
WALK_LCUR:    .dcb.b 4         # current flush phys
WALK_FILL:    .dcb.b 4         # bytes staged in WALK_LIST
WALK_NREC:    .dcb.b 4
WALK_OVFL:    .dcb.b 4
WALK_MAX:     .dcb.b 4

    .text

    # Register map (DMAIn/DMAOut clobber only t2/at; args in s0/s4/t0):
    #   v0 = group (0..23)      v1 = sprite-in-group (0..15)
    #   s1 = VRAM phys          s2/s3 = free (unused)
    #   s5 = sx   s6 = sy   s7 = sh   s8 = sw
    #   k0 = vshrink   k1 = repeat_tiles
    #   a2 = half flag   a3 = aa | aa_en<<8
    #   t3 = y   t4 = nt   t5 = maxy   t7 = ssh (per iteration)
    #   t6/t8/t9/t0/t1 = scratch
    .func cmd_sprite_walk
cmd_sprite_walk:
    sll a0, 8                    # strip the rspq command byte from arg word 0
    srl a0, 8
    sw a0, %lo(WALK_VRAM)
    sw a1, %lo(WALK_LBASE)
    sw a1, %lo(WALK_LCUR)
    sw zero, %lo(WALK_FILL)
    sw zero, %lo(WALK_NREC)
    sw zero, %lo(WALK_OVFL)
    andi a3, a2, 0x1FF           # aa | aa_en<<8
    srl t0, a2, 16
    sw t0, %lo(WALK_MAX)

    li v0, 0                     # group index
walk_group:
    # --- DMA this group's SCB (3 x 32B) and tilemaps (2048B) ---
    lw s1, %lo(WALK_VRAM)
    sll t4, v0, 5                # g*32
    addu s0, s1, t4
    li t5, 0x10000               # SCB z at word 0x8000
    addu s0, s0, t5
    li s4, %lo(WALK_SCB)
    jal DMAIn
    li t0, DMA_SIZE(32, 1)

    lw s1, %lo(WALK_VRAM)
    sll t4, v0, 5
    addu s0, s1, t4
    li t5, 0x10400               # SCB y at word 0x8200
    addu s0, s0, t5
    li s4, %lo(WALK_SCB) + 32
    jal DMAIn
    li t0, DMA_SIZE(32, 1)

    lw s1, %lo(WALK_VRAM)
    sll t4, v0, 5
    addu s0, s1, t4
    li t5, 0x10800               # SCB x at word 0x8400
    addu s0, s0, t5
    li s4, %lo(WALK_SCB) + 64
    jal DMAIn
    li t0, DMA_SIZE(32, 1)

    lw s1, %lo(WALK_VRAM)
    sll t4, v0, 11               # g*2048
    addu s0, s1, t4
    li s4, %lo(PAL_BUFFER)
    jal DMAIn
    li t0, DMA_SIZE(2048, 1)

    li v1, 0                     # sprite-in-group
walk_sprite:
    sll t4, v0, 4
    addu t4, t4, v1              # snum
    li t5, 381
    beq t4, t5, walk_group_next
    nop
    sll t4, v1, 1
    addiu t5, t4, %lo(WALK_SCB)
    lhu t6, 0(t5)                # zc
    lhu t7, 32(t5)               # yc
    lhu t8, 64(t5)               # xc

    andi t9, t7, 0x40
    bnez t9, walk_chain
    nop
    # fresh sprite: sx/sy/sh/vshrink/repeat from SCB
    srl s5, t8, 7                # sx = xc>>7
    srl s6, t7, 7
    li t9, 496
    subu s6, t9, s6              # sy = 496 - (yc>>7)
    andi s7, t7, 0x3F
    sll s7, s7, 4                # sh = (yc&0x3F)*16
    li k1, 0                     # repeat = false
    li t9, 512
    slt t9, t9, s7               # sh > 512 ?
    beqz t9, 1f
    nop
    li s7, 512
    li k1, 1                     # repeat = true
1:  andi k0, t6, 0xFF            # vshrink
    b walk_scb_done
    nop
walk_chain:
    addu s5, s5, s8              # sx += sw (previous sprite's sw)
walk_scb_done:
    srl s8, t6, 8
    andi s8, s8, 0xF
    addiu s8, s8, 1              # sw = ((zc>>8)&0xF)+1

    beqz s7, walk_next_sprite    # sh == 0
    nop
    slti t9, s5, 320
    bnez t9, walk_chk_y          # sx < 320: not x-skipped
    nop
    addu t9, s5, s8
    slti t9, t9, 513
    bnez t9, walk_next_sprite    # sx >= 320 && sx+sw <= 512: skip
    nop
walk_chk_y:
    slti t9, s6, 224
    bnez t9, walk_visible        # sy < 224: cannot Y-cull
    nop
    addu t9, s6, s7
    slti t9, t9, 513
    bnez t9, walk_next_sprite    # sy >= 224 && sy+sh <= 512: skip
    nop
walk_visible:
    li t3, 0                     # y = 0
    li t4, 0                     # nt = 0
    li a2, 0                     # half = 0
    # maxy = min(sh, 256)
    li t5, 256
    slti t9, s7, 256
    beqz t9, walk_tile_loop
    nop
    move t5, s7

walk_tile_loop:
    slt t9, t3, t5               # while (y < maxy)
    beqz t9, walk_half_end
    nop
    # ssh = (vs+1)>>4 + (((vs+1)&15) > MAGIC[nt])
    addiu t6, k0, 1
    srl t7, t6, 4
    andi t6, t6, 15
    addiu t8, t4, %lo(WALK_MAGIC)
    lbu t8, 0(t8)
    slt t6, t8, t6
    addu t7, t7, t6              # t7 = ssh
    beqz t7, walk_advance        # ssh == 0: no draw, no clip
    nop
    # clip: when y + ssh > sh, ssh becomes sh - y
    addu t6, t3, t7
    slt t6, s7, t6
    beqz t6, 2f
    nop
    subu t7, s7, t3
2:
    # ssy = sy + y; visible if ssy < 224 || ssy+ssh > 512
    addu t6, s6, t3
    slti t8, t6, 224
    bnez t8, walk_emit
    nop
    addu t8, t6, t7
    slti t8, t8, 513
    bnez t8, walk_advance
    nop
walk_emit:
    # tmap fetch: PAL_BUFFER + i*128 + nt*4
    sll t6, v1, 7
    sll t8, t4, 2
    addu t6, t6, t8
    addiu t6, t6, %lo(PAL_BUFFER)
    lhu t9, 0(t6)                # tnum (low 16)
    lhu t8, 2(t6)                # tc
    srl t6, t8, 4                # tnum |= ((tc>>4)&0xF)<<16
    andi t6, t6, 0xF
    sll t6, t6, 16
    or t9, t9, t6
    # auto animation (a3 = aa | aa_en<<8)
    andi t6, a3, 0x100
    beqz t6, 4f
    nop
    andi t6, t8, 8
    beqz t6, 3f
    nop
    addi t6, zero, -8
    and t9, t9, t6               # tnum &= ~7
    andi t6, a3, 7
    or t9, t9, t6                # tnum |= aa & 7
    b 4f
    nop
3:  andi t6, t8, 4
    beqz t6, 4f
    nop
    addi t6, zero, -4
    and t9, t9, t6               # tnum &= ~3
    andi t6, a3, 3
    or t9, t9, t6                # tnum |= aa & 3
4:
    # w0 = tnum | palnum<<20 | (tc&1)<<28 | (tc&2)<<28
    srl t6, t8, 8
    andi t6, t6, 0xFF
    sll t6, t6, 20
    or t9, t9, t6
    andi t6, t8, 1
    sll t6, t6, 28
    or t9, t9, t6
    andi t6, t8, 2
    sll t6, t6, 28               # bit 29
    or t9, t9, t6                # t9 = w0
    # w1 = (sx&0xFFF) | (ssy&0xFFF)<<12 | (sw-1)<<24 | (ssh-1)<<28
    addu t6, s6, t3              # ssy
    andi t6, t6, 0xFFF
    sll t6, t6, 12
    andi t8, s5, 0xFFF
    or t6, t6, t8
    addiu t8, s8, -1
    sll t8, t8, 24
    or t6, t6, t8
    addiu t8, t7, -1
    sll t8, t8, 28
    or t6, t6, t8                # t6 = w1
    # capacity check
    lw t8, %lo(WALK_NREC)
    lw t0, %lo(WALK_MAX)
    slt t0, t8, t0
    beqz t0, walk_over
    nop
    lw t0, %lo(WALK_FILL)
    addiu t1, t0, %lo(WALK_LIST)
    sw t9, 0(t1)
    sw t6, 4(t1)
    addiu t0, t0, 8
    sw t0, %lo(WALK_FILL)
    addiu t8, t8, 1
    sw t8, %lo(WALK_NREC)
    li t1, 512
    bne t0, t1, walk_advance     # staging not full yet
    nop
    # flush 512B of records
    lw s0, %lo(WALK_LCUR)
    li s4, %lo(WALK_LIST)
    jal DMAOut
    li t0, DMA_SIZE(512, 1)
    lw t0, %lo(WALK_LCUR)
    addiu t0, t0, 512
    sw t0, %lo(WALK_LCUR)
    sw zero, %lo(WALK_FILL)
    b walk_advance
    nop
walk_over:
    lw t0, %lo(WALK_OVFL)
    addiu t0, t0, 1
    sw t0, %lo(WALK_OVFL)
walk_advance:
    addu t3, t3, t7              # y += ssh
    addiu t4, t4, 1
    andi t4, t4, 31              # nt = (nt+1) & 31
    bnez k1, walk_tile_loop      # repeat: no nt==16 break
    nop
    li t6, 16
    beq t4, t6, walk_half_end    # non-repeat: break at tile #16
    nop
    b walk_tile_loop
    nop
walk_half_end:
    li t6, 256
    slt t6, t6, s7               # sh > 256 ?
    beqz t6, walk_next_sprite
    nop
    bnez a2, walk_next_sprite    # second half already done
    nop
    li a2, 1                     # half = 1
    move t5, s7                  # maxy = sh
    beqz k1, walk_h1_norep
    nop
    # repeat: y -= (y-256)*2 ; nt = (32-nt) & 31
    addiu t6, t3, -256
    sll t6, t6, 1
    subu t3, t3, t6
    li t6, 32
    subu t4, t6, t4
    andi t4, t4, 31
    b walk_tile_loop
    nop
walk_h1_norep:
    li t6, 512
    subu t3, t6, t3              # y = 512 - y
    b walk_tile_loop
    nop
walk_next_sprite:
    addiu v1, v1, 1
    li t6, 16
    bne v1, t6, walk_sprite
    nop
walk_group_next:
    addiu v0, v0, 1
    li t6, 24
    bne v0, t6, walk_group
    nop

    # final partial flush (fill is a multiple of 8)
    lw t0, %lo(WALK_FILL)
    beqz t0, walk_trailer
    nop
    lw s0, %lo(WALK_LCUR)
    li s4, %lo(WALK_LIST)
    jal DMAOut
    addiu t0, t0, -1             # DMA_SIZE(fill, 1)
walk_trailer:
    lw t0, %lo(WALK_NREC)
    sw t0, %lo(WALK_TRAILER)
    lw t0, %lo(WALK_OVFL)
    sw t0, %lo(WALK_TRAILER) + 4
    lw t0, %lo(WALK_MAX)
    sll t0, t0, 3
    lw s0, %lo(WALK_LBASE)
    addu s0, s0, t0              # trailer at list + maxrecs*8
    li s4, %lo(WALK_TRAILER)
    jal DMAOut
    li t0, DMA_SIZE(8, 1)

    j RSPQ_Loop
    nop
    .endfunc


#include <rsp_rdpq.inc>
