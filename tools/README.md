# tools

Helper scripts for the measurement and correctness gates described in
`PLAN-OPTIMIZATION.md`. Shell scripts run in WSL from the `mvs64/` directory;
the PowerShell scripts run on Windows.

| Script | Use |
| --- | --- |
| `ps-ares-run.ps1` | Run one ROM in ares for N seconds and capture its log. Refuses to start if another emulator is running. Set `ARES_EXE` or pass `-Ares`. |
| `ps-ares-queue.ps1` | Run several ares jobs strictly one after another (`-JobList "rom|log|secs;..."`). |
| `emu-guard.sh` | Source at the top of WSL scripts that run the PC `emu`, so only one emulator runs at a time. |
| `compare-fbcrc.py` | Pixel gate: compare the `[FBCRC]` frame checksums of two `MVS64_FBCRC` runs. |
| `trcdiff.sh` | 68k gate: first divergence between two `[TRCRC]` streams (`TRCRC_ON=1` builds). |
| `iodiff.sh` | First divergence between two `[IO]` MMIO read streams (`MVS64_IOLOG_N64` builds). |
| `posdcmp.py` | Compare two `MVS64_PERFOSD` logs window by window (fight windows with matching tile counts). |
| `analyze-ophist.py` | Rank the `[OPHIST]` 68k opcode histogram by form and fast-path coverage. |
| `nm-hotsets.sh` | Addresses and cache sets of the hot code and data in an ELF. |
| `nm-perfsyms.sh` | Cache sets of the 68k perf counters against the m64k context. |
| `disbios.c`, `disgame.c` | 68k disassemblers for the BIOS and the converted program ROM (build notes in each file). |
| `l0.py` | Derives the vertical-shrink table in `video.c`. |
