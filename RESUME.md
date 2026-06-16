# RESUME — NeoGeo samsho2 → N64 (mvs64 fork)

> Pick-up point for resuming work. For the full plan see `PLAN.md`; for build
> steps see `BUILDING.md`.

## ▶ Kickoff prompt (paste this to Claude to resume)

> We're porting NeoGeo **Samurai Shodown 2** (`samsho2`) to run on real N64
> hardware by forking **mvs64** (at `<workspace>\mvs64`,
> pushed to github.com/mike-r129/N64-NEOGEO-Test). Read `RESUME.md`, `PLAN.md`,
> and your memory. The native libdragon toolchain is already built in WSL at
> `/root/n64inst` and `make mvs64` produces a valid `.z64`. **Status:** waiting
> on a real NeoGeo BIOS + `sfix.sfix` in `…\N64-NEOGEO\bios\`. If I've added
> them, build the real ROM and boot-test; otherwise continue per the plan.
> Commit work individually; never commit ROMs/BIOS.

## Where we are

**Goal:** play `samsho2` on a real N64 from a flash cart (EverDrive‑64 X7 / 64drive),
as a `.z64`. Strategy: **fork mvs64 + transplant Z80/YM2610 sound** (not gngeo
from scratch — decided & validated; see `PLAN.md §2`).

**DONE & pushed (11 commits on `main`):**
- ✅ `PLAN.md` — parallelizable plan (workstreams WS0–WS9, contracts, gates).
- ✅ `mvsmakerom` emits `v.rom` (7 MB ADPCM) + `m.rom` (Z80) — was dropping them; **verified on real samsho2** (v.rom=7,340,032, m.rom=131,072).
- ✅ `samsho2` (NGH‑063) added to the converter game DB.
- ✅ `roms.c` — `m.rom` resident loader + `v.rom` streaming reader (`vrom_read`).
- ✅ `sound.h` seam + `sound_noop.c` — the boundary the Z80/YM2610 plug into; `hw.c` sound latch routed through it. (compile‑validated, PC + N64.)
- ✅ libdragon‑drift fixes: reconstructed missing `m64k/cycles.h`; fixed a `-Werror` break in `pbrom_init`. **No libdragon pin needed.**
- ✅ Native toolchain built (WSL `/root/n64inst`, gcc 14.2 + libdragon) and **`mvs64-samsho2.z64` builds** (26.7 MB, valid magic `80371240`) — with a *placeholder* BIOS, so it validates the pipeline but won't boot a game.
- ✅ `.gitattributes`, `BUILDING.md`.

**THE BLOCKER:** a real **NeoGeo BIOS + `sfix.sfix`** (SNK copyright — must be user‑supplied: UniBIOS from unibios.free.fr + an `sfix.sfix`, or a hardware dump). Without it the ROM can't boot a game.

**Running:** a background watcher (`wsl-bios-watch.sh`) auto‑builds the real ROM when a BIOS lands in `…\N64-NEOGEO\bios\` (may have timed out — just rebuild manually, see below).

## To resume → get to a playable ROM

1. **Drop the BIOS:** put a NeoGeo BIOS (e.g. `uni-bios.rom`) **and** `sfix.sfix` in `<workspace>\bios\`.
2. **Build the real ROM** (in WSL):
   ```sh
   export N64_INST=/root/n64inst; export PATH=$PATH:$N64_INST/bin
   cd /mnt/c/Users/Mike/Desktop/N64-NEOGEO/mvs64
   make mvs64 ROM=/mnt/c/Users/Mike/Desktop/N64-NEOGEO/samsho2.zip \
              BIOS=/mnt/c/Users/Mike/Desktop/N64-NEOGEO/bios/<your-bios>
   ```
   (run via `MSYS_NO_PATHCONV=1 wsl bash -c "…"`; toolchain already built.)
3. **Boot‑test** — `mvs64-samsho2.z64` to EverDrive/64drive → boot on N64. Also `make pctest && ./emu <samsho2.n64/>` for PC debugging.
4. **Iterate to playable** (Phase 0 → on): does it boot/attract/in‑game? FPS?

## Next engineering tasks (not yet started — see PLAN.md)

- **WS1‑T4** fix idle_skip (still disabled: `roms.c:410` force‑zero + hardcoded `m64k_asm.S:2550‑1` PCs) + find samsho2 idle PC for `game_ini`.
- **WS2** Z80 core (cz80) behind `sound.h`; **WS3** YM2610 (FM+SSG+ADPCM, stream `v.rom`); **WS4** N64 AI audio out.
- **WS5** LSPC/sprite correctness for samsho2; **WS6** perf (Expansion Pak, frameskip, timing); **WS7** P2 input + saves; **WS8** BizHawk MCP validation.

## Key locations

| What | Where |
|---|---|
| Repo (fork) | `<workspace>\mvs64` → github.com/mike-r129/N64-NEOGEO-Test (`main`) |
| Game ROM set | `<workspace>\samsho2.zip` (never committed) |
| BIOS drop folder | `<workspace>\bios\` (needs bios + `sfix.sfix`) |
| Toolchain | WSL `/root/n64inst` (gcc 14.2 + libdragon); source copy `~/libdragon` |
| Build/helper scripts | `…\N64-NEOGEO\wsl-build-toolchain.sh`, `wsl-build-z64.sh`, `wsl-bios-watch.sh` |
| Claude memory | `…\.claude\projects\C--Users-Mike-Desktop-N64-NEOGEO\memory\` |

## Conventions

Commit work **individually** (clean history). **Never commit ROMs/BIOS** (`.gitignore` enforces). Deliverable is `.z64` (not `.x64` — that's a C64 format). 8 MB Expansion Pak recommended once sound is added.
