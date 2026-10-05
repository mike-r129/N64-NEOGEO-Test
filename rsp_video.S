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
        RSPQ_DefineCommand cmd_sprite_begin, 8 # 0x4
        RSPQ_DefineCommand cmd_nop, 4          # 0x5 (retired: cmd_sprite_walk)
        RSPQ_DefineCommand cmd_nop, 4          # 0x6 (retired: cmd_sprite_batch)
        RSPQ_DefineCommand cmd_sprite_draw2, 8 # 0x7
        RSPQ_DefineCommand cmd_nop, 4          # 0x8
	RSPQ_EndOverlayHeader

RSPQ_BeginSavedState
    .align 4
PALETTE_CACHE: .half -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
LAST_PALETTE:  .long 0
RDRAM_PALETTE: .long 0
SPR_PIXBASE:   .long 0        # C-ROM pixel slots phys base (cmd_sprite_draw2)
# Phase 3 rotation state (PLAN-DRAW-RDP §7): must live in the SAVED state
# block — overlay .data/.bss reset on every overlay swap (the audio ucode
# swaps in mid-frame constantly), and this state persists record-to-record
# across the whole sprite pass. Reset by cmd_sprite_begin each frame.
SPR_RSTATE:    .byte 0        # descriptor/TMEM pair in rotation (0..2)
SPR_MODE:      .byte 0        # current RDP cycle mode: 0=1cyc, 1=copy
    .align 3                  # keep saved-state DMA size a multiple of 8
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

    # Phase 3 (PLAN-DRAW-RDP §7.1): the per-record syncs left the template.
    # Three baked variants rotate BOTH the tile-descriptor pair (draw tile
    # 2p, load tile 2p+1) and the TMEM slot (p*128B; the TLUT half at
    # 0x800+ is untouched, TILE7 stays the TLUT-load descriptor). A
    # descriptor/slot is reused every 3rd record, so SyncTile+SyncLoad are
    # emitted only on rotation wrap (see sprite_draw prologue): between
    # wraps the RDP overlaps record i+1's LoadBlock with record i's
    # TexRect instead of ping-ponging load→draw per tile. The old stream
    # paid SyncTile+SyncLoad per record, ~565+/frame in fights.
    # Variants sit 64B apart (7 quads + pad): base = TPL0 + (pair<<6).
.macro SPRITE_DRAW_TPL ldtile, drtile, tmemw
    .quad RdpSetTexImage(RDP_TILE_FORMAT_RGBA, RDP_TILE_SIZE_16BIT, 0, 4)
    .quad RdpSetTile(RDP_TILE_FORMAT_RGBA, RDP_TILE_SIZE_16BIT, 0, \tmemw, \ldtile)
    .quad RdpSetTile(RDP_TILE_FORMAT_INDEX, RDP_TILE_SIZE_4BIT, 8/8, \tmemw, \drtile)
    .quad RdpSetTileSizeI(\drtile, 0, 0, 16-1, 16-1)
    .quad RdpLoadBlock(\ldtile, 0, 0, 16*16/4, 16/2)
    .quad RdpTextureRectangle1I(\drtile, 0, 0, 0, 0)       # x1,y1 / x0,y0
    .quad RdpTextureRectangle2I(0, 0, 0, 0)                # s,t,ds,dt
    .quad 0                                                # pad to 64B stride
.endm
    .align 4
RDP_SPRITE_TPL0: SPRITE_DRAW_TPL 1, 0, 0
RDP_SPRITE_TPL1: SPRITE_DRAW_TPL 3, 2, 16
RDP_SPRITE_TPL2: SPRITE_DRAW_TPL 5, 4, 32

# Layout-identical fps-twin knob (matched-pair law): with -DMVS64_ROTA_OFF
# this single DATA byte flips to 1 and the body pins pair=0 (TPL0 = tiles
# 0/1, tmem 0, syncs every record — byte-identical to the pre-Phase-3
# stream) and never picks copy mode. Both twin binaries carry identical
# text and layout; only this initializer differs.
#ifdef MVS64_ROTA_OFF
ROTA_DISABLE: .byte 1
#else
ROTA_DISABLE: .byte 0
#endif

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
    # a1: C-ROM pixel slots base in RDRAM (for cmd_sprite_draw2)
    ################################
    .func cmd_sprite_begin
cmd_sprite_begin:
    sw a1, %lo(SPR_PIXBASE)
    # Initialize palette cache entries to -1
    li t0, 0xFFFF
    mtc2 t0, $v01.e0
    vor $v01, vzero, $v01.e0
    li s0, %lo(PALETTE_CACHE)
    sqv $v01, 0,s0
    sqv $v01, 16,s0

    # Reset Phase 3 rotation/mode state: the CPU emits the standard-mode
    # SetOtherModes right after this command each frame, so pair=0 and
    # mode=1cyc are consistent with the RDP state by construction.
    sh zero, %lo(SPR_RSTATE)

    jr ra
    sw a0, %lo(RDRAM_PALETTE)
    .endfunc

    ######################################################
    # cmd_nop - 4-byte filler: the CPU uses it to put the sprite commands
    # on 8-byte boundaries so it can store each one with a single sd.
    ######################################################
    .func cmd_nop
cmd_nop:
    jr ra
    nop
    .endfunc

    ######################################################
    # cmd_sprite_draw2 - draw a sprite, 2-word form
    #
    # a0: bit 0 .. 7: palette
    #     bit 8:      flip x
    #     bit 9:      flip y
    #     bit 10..23: C-ROM pixel slot (128 bytes each, from SPR_PIXBASE)
    # a1: the walk record's w1 verbatim: x[0..11] y[12..23] sw-1[24..27]
    #     sh-1[28..31]
    #
    # Rebuilds exactly the a0/a1/a2 the CPU packs for cmd_sprite_draw and
    # runs that body: same draw stream, one word less per record and no
    # unpack/repack on the CPU.
    ######################################################
    .func cmd_sprite_draw2
cmd_sprite_draw2:
    # a2 = (sw-1)|((sh-1)<<4) | flips<<8
    srl a2, a1, 24
    andi t0, a0, 0x300
    or a2, t0
    # t1 = pal<<24 | x<<12 | y
    andi t0, a0, 0xFF
    sll t1, t0, 24
    andi t0, a1, 0xFFF
    sll t0, 12
    or t1, t0
    srl t0, a1, 12
    andi t0, 0xFFF
    or t1, t0
    # a0 = pixel base + slot*128, with the top byte cmd_sprite_draw would
    # see (overlay id nibble | command 2): it lands in the SetTexImage
    # address word, so the RDP stream stays byte-identical.
    srl t0, a0, 10
    andi t0, 0x3FFF
    sll t0, 7
    lw t2, %lo(SPR_PIXBASE)
    add t0, t2
    lui t2, 0xF000
    and t2, a0
    or t0, t2
    lui t2, 0x0200
    or a0, t0, t2
    j cmd_sprite_draw
    move a1, t1
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
    # Extract X/Y and size fields first: the rotation prologue (and the
    # Phase 3 copy-mode decision) need them before the template is staged.
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

    # --- Phase 3.2 modal test: copy-eligible iff full 16x16, no flips,
    # and no x-clipping — the proven predicate of the old CPU dual-mode
    # draw path (the PERFOSD G/H counters in video.c mirror it). Top y-clip is handled inside the
    # copy path, bottom by the 224 clamp; y is additionally bounded to
    # (-16, 224) so the copy rect can never go degenerate (the 1cyc
    # fallback scissors those away, same as today). t3 = record mode.
    li t3, 0                  # 1cyc
    andi t0, a2, 0x3FF
    xori t0, 0xFF             # sw-1==15 && sh-1==15 && !flipx && !flipy
    bnez t0, 9f
    nop
    bltz rect_x0, 9f          # x0 >= 0
    sltiu t0, rect_x0, 305    # x0 <= 304 (x0+16 <= 320), delay slot
    beqz t0, 9f
    addi t0, rect_y0, 15      # y0 >= -15, delay slot
    bltz t0, 9f
    slti t0, rect_y0, 224     # y0 <= 223, delay slot
    beqz t0, 9f
    nop
    li t3, 1                  # copy
9:
    lbu t0, %lo(ROTA_DISABLE)
    beqz t0, 10f
    nop
    li t3, 0                  # legacy twin: never copy
10:

    # --- Rotation prologue (§7.1): t2 = descriptor/TMEM pair in use for
    # this record. Syncs are emitted only on rotation wrap (pair 0 about
    # to be reused): the previous user of pair 0's descriptors and TMEM
    # slot is record i-3, and SyncTile+SyncLoad here drain every record
    # <= i-1. Between wraps there are no syncs, so the RDP loads record
    # i+1's texels into its own slot while record i's rect still draws.
    # A mode switch (§7.2) instead emits SyncPipe + SetOtherModes: the
    # pipe drain covers every rotation hazard too, so the wrap syncs are
    # skipped on those records. SOM words are the VERBATIM payloads the
    # rdpq engine composes for these two modes, captured off the
    # validated stream (2026-08-08, with a since-removed rdpq log rig):
    #   1cyc: EF008CF000000201  "1cyc tlut alpha_compare cvg=[zap]"
    #   copy: EF20800000000001  "copy tlut alpha_compare" (fix layer's
    #         own copy mode — proven with the same CI4+TLUT+alpha setup)
    lbu t2, %lo(SPR_RSTATE)
    lbu t0, %lo(SPR_MODE)
    beq t0, t3, 3f
    addi t1, t2, 1            # next pair (delay slot, all paths)
    # mode switch
    sb t3, %lo(SPR_MODE)
    li t0, 0x27000000         # SyncPipe
    sw t0, 0(s3)
    sw zero, 4(s3)
    bnez t3, 1f
    nop
    li t0, 0x2F008CF0         # -> standard 1cyc
    li t4, 0x00000201
    b 2f
    nop
1:  li t0, 0x2F208000         # -> copy
    li t4, 0x00000001
2:  sw t0, 8(s3)
    sw t4, 12(s3)
    b 5f
    addi s3, 16
3:  bnez t2, 5f
    nop
    li t0, 0x28000000         # SyncTile
    sw t0, 0(s3)
    sw zero, 4(s3)
    li t0, 0x26000000         # SyncLoad
    sw t0, 8(s3)
    sw zero, 12(s3)
    addi s3, 16
5:
    slti t0, t1, 3
    bnez t0, 4f
    nop
    li t1, 0
4:
    lbu t0, %lo(ROTA_DISABLE)
    beqz t0, 6f
    nop
    li t1, 0                  # legacy twin: pair pinned to 0 (per-record syncs)
6:  sb t1, %lo(SPR_RSTATE)

    # Stage this pair's template (7 quads, 56B; the 4th quad copy
    # over-copies the 8B pad, harmless: s3 advances 56 and RDPQ_Send
    # sends up to s3).
    sll t4, t2, 6
    addi t4, %lo(RDP_SPRITE_TPL0)
    lqv vload0,  0,t4
    lqv vload1, 16,t4
    lqv vload2, 32,t4
    lqv vload3, 48,t4

    sqv vload0,  0,s3
    sqv vload1, 16,s3
    sqv vload2, 32,s3
    sqv vload3, 48,s3

    # Store sprite pixel address in SetTexImage
    sw a0, 0*8+4(s3)

    # Store palette slot into the draw-tile SetTile
    lbu t0, 2*8+5(s3)
    sll t1, palslot, 4
    or t0, t1
    sb t0, 2*8+5(s3)

    bnez t3, sprite_copy_rect
    nop

    # Fetch scale factors from table
    sll rect_ds, rect_sw, 1
    sll rect_dt, rect_sh, 1
    lhu rect_ds, %lo(SCALE_FACTORS)(rect_ds)
    lhu rect_dt, %lo(SCALE_FACTORS)(rect_dt)

    # Handle negative rectangle positions
    bgez rect_x0, 5f
    li rect_s0, 0
    neg rect_s0, rect_x0
    sub rect_sw, rect_s0
    li rect_x0, 0
5:
    bgez rect_y0, 6f
    li rect_t0, 0
    neg rect_t0, rect_y0
    sub rect_sh, rect_t0
    li rect_y0, 0
6:

    # Handle flipping
    andi t0, a2, 1<<8
    beqz t0, 7f
    nop
    li t0, 16
    sub rect_s0, t0, rect_s0
    neg rect_ds, rect_ds
7:
    andi t0, a2, 1<<9
    beqz t0, 8f
    nop
    li t0, 16
    sub rect_t0, t0, rect_t0
    neg rect_dt, rect_dt
8:

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
    lw t0, 5*8+4(s3)
    or t0, rect_x0
    sw t0, 5*8+4(s3)

    andi rect_sw, 0x3FF
    andi rect_sh, 0x3FF
    sll rect_sw, 14
    sll rect_sh, 2
    or rect_sw, rect_sh
    lw t0, 5*8+0(s3)
    or t0, rect_sw
    sw t0, 5*8+0(s3)

    sll rect_s0, 5
    sll rect_t0, 5
    sh rect_s0, 6*8+0(s3)
    sh rect_t0, 6*8+2(s3)
    sh rect_ds, 6*8+4(s3)
    sh rect_dt, 6*8+6(s3)

    addi s3, 7*8

    # Send to RDP and go back to main loop
    j RDPQ_Send
    li s4, %lo(RDPQ_CMD_STAGING)

sprite_copy_rect:
    # COPY-mode rect (§7.2): full 16x16, no flip, no x-clip guaranteed by
    # the modal test. Top y-clip via the t texel offset (the old CPU copy
    # path's workaround), bottom clamped to 224. COPY semantics follow
    # the libdragon fixup + the fix layer's own template: XL/YL are
    # INCLUSIVE (one full pixel less than the exclusive x1/y1, hence
    # -0x4004 on the packed 10.2 pair), dsdx = 4 texels/pixel (the RDP
    # copies 4 16-bit texels per clock), dtdy = 1.
    li rect_t0, 0
    bgez rect_y0, 1f
    addi rect_sw, rect_x0, 16      # x1 exclusive (delay slot, both paths)
    neg rect_t0, rect_y0           # t offset = -y0
    li rect_y0, 0
1:
    li t0, 16
    sub t0, rect_t0
    add rect_sh, rect_y0, t0       # y1 exclusive = y0 + 16 - t
    li t0, 224
    ble rect_sh, t0, 2f
    nop
    move rect_sh, t0               # bottom clamp
2:
    sll rect_sw, 14
    sll rect_sh, 2
    or rect_sw, rect_sh
    addi rect_sw, -0x4004          # exclusive -> inclusive XL/YL
    lw t0, 5*8+0(s3)
    or t0, rect_sw
    sw t0, 5*8+0(s3)

    sll rect_x0, 14
    sll rect_y0, 2
    or rect_x0, rect_y0
    lw t0, 5*8+4(s3)
    or t0, rect_x0
    sw t0, 5*8+4(s3)

    sh zero, 6*8+0(s3)             # s = 0
    sll rect_t0, 5
    sh rect_t0, 6*8+2(s3)          # t
    li t0, 4<<10
    sh t0, 6*8+4(s3)               # dsdx = 4.0
    li t0, 1<<10
    sh t0, 6*8+6(s3)               # dtdy = 1.0

    addi s3, 7*8

    # Send to RDP and go back to main loop
    j RDPQ_Send
    li s4, %lo(RDPQ_CMD_STAGING)

    .endfunc


#include <rsp_rdpq.inc>
