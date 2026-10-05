#!/bin/bash
# Print the dcache set of the 68k perf counters and the m64k context, to check
# that instrumented builds keep the counters off the context's sets.
# Usage: tools/nm-perfsyms.sh [elf...]   (default: build/mvs64/mvs64-samsho2.elf)
export N64_INST="${N64_INST:-$HOME/n64inst}"
NM="$N64_INST/bin/mips64-elf-nm"
[ $# -eq 0 ] && set -- build/mvs64/mvs64-samsho2.elf
for e in "$@"; do
  echo "== $e"
  "$NM" "$e" | grep -E '(perf_m68k_insns|perf_idle_skips|perf_tlb_faults|perf_m68k_slices| m64k$|pc_diff)' \
    | sort | awk '{ a = strtonum("0x" $1); printf "%-18s %08x dset=%3d\n", $3, a, int(a/16)%512 }'
done
