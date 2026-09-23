#!/bin/bash
set -uo pipefail
export N64_INST="$HOME/n64inst"
export PATH="$N64_INST/bin:$PATH"
cd /root/N64-NEOGEO/mvs64 || exit 1
make -f Makefile.pctests EXTRA_DEFINES="-Wno-error=unused-result" 2>&1 | tail -25
echo "rc=${PIPESTATUS[0]}"
ls -la emu 2>/dev/null && echo EMU_OK || echo NO_EMU
