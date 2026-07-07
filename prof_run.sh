#!/bin/bash
set -uo pipefail
export N64_INST="$HOME/n64inst"
export PATH="$N64_INST/bin:$PATH"
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
cd /mnt/c/Users/Mike/Desktop/N64-NEOGEO/mvs64 || exit 1
ROM=/mnt/c/Users/Mike/Desktop/N64-NEOGEO/samsho2.n64/
FRAMES="${1:-3000}"
SHOT="${2:-0}"
INPUT="${3:-}"
PSTART="${4:-}"
PEND="${5:-}"
rm -f shot_*.bmp
ENV="MVS64_FRAMES=$FRAMES"
[ "$SHOT" != "0" ] && ENV="$ENV MVS64_SHOT=$SHOT"
[ -n "$INPUT" ] && ENV="$ENV MVS64_INPUT=$INPUT"
[ -n "$PSTART" ] && ENV="$ENV MVS64_PROF_START=$PSTART"
[ -n "$PEND" ] && ENV="$ENV MVS64_PROF_END=$PEND"
echo "env: $ENV"
env $ENV timeout 600 ./emu "$ROM" 2>&1 | grep -aE '\[HEADLESS\]|\[PROF\]|\[INPUT\]' | tail -40
echo "shots: $(ls shot_*.bmp 2>/dev/null | wc -l)"
ls shot_*.bmp 2>/dev/null
