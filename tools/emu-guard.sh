#!/bin/bash
# ONE EMULATOR AT A TIME (2026-09-22: a dozen parallel ares instances froze the
# whole machine). Source this at the top of any WSL script that runs the
# headless PC emu; it exits the script if ares (Windows side) or another PC emu
# run (WSL side) is already active.
#   . tools/emu-guard.sh
if tasklist.exe /FI "IMAGENAME eq ares.exe" /NH 2>/dev/null | grep -qi "ares.exe"; then
    echo "[emu-guard] REFUSED: ares is running on the Windows side" >&2
    exit 2
fi
if pgrep -x emu >/dev/null 2>&1; then
    echo "[emu-guard] REFUSED: a PC emu run is already active in WSL" >&2
    exit 2
fi
