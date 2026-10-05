#!/bin/bash
# Dump hot-symbol addresses + VR4300 cache-set mappings from an mvs64 elf.
# dcache: 8KB direct, 16B lines -> set = (addr >> 4) & 511
# icache: 16KB direct, 32B lines -> set = (addr >> 5) & 511
# Usage: tools/nm-hotsets.sh [elf]   (default: build/mvs64/mvs64-samsho2.elf)
export N64_INST="${N64_INST:-$HOME/n64inst}"
NM="$N64_INST/bin/mips64-elf-nm"
ELF="${1:-build/mvs64/mvs64-samsho2.elf}"
"$NM" -S "$ELF" | grep -E ' (m64k|optable|main_loop|_m64k_asmrun|__m64k_asm_end|pc_diff|mvs_tlbhandler|tlb_readhwio|read_hwio|write_hwio|banks|VIDEO_RAM|PALETTE_RAM|WORK_RAM|BIOS|P_ROM|reg_vram_addr|z80|ym2610|YM2610|snd_|audio_|__m64k_dyn_arena|__m64k_dyn_table)' | sort | awk '
{
  addr = strtonum("0x" $1)
  size = (NF == 4) ? strtonum("0x" $2) : 0
  name = $NF
  dset = int(addr / 16) % 512
  iset = int(addr / 32) % 512
  printf "%s addr=%08x size=%6d dset=%3d iset=%3d\n", name, addr, size, dset, iset
}'
