# PLAN — Samurai Shodown 2 on real Nintendo 64 (mvs64 fork)

> **Goal:** play **Samurai Shodown 2** (NeoGeo `samsho2`, NGH‑063) on **real N64 hardware**,
> loaded from a flash cart (EverDrive‑64 X7 / 64drive) as a single **`.z64`** ROM.
>
> **Strategy (decided & ratified):** **fork [`rasky/mvs64`](https://github.com/rasky/mvs64)**
> (MIT) as the N64 chassis and **transplant the missing sound subsystem** (Z80 + YM2610)
> into it. We are *not* porting gngeo from scratch — see §2.
>
> **Honest verdict (read §4 first):** the chassis already boots NeoGeo games; the real work is
> **sound (entirely absent today)** + **performance tuning**. Locked 60 fps *with sound* on a
> **stock 4 MB** console is implausible; the realistic, achievable deliverable is **fully playable
> samsho2 with the 8 MB Expansion Pak** (60 fps where the budget allows, adaptive frameskip
> otherwise), audio at 22050 Hz. **Phase 0 measures the real number before we invest.**

This plan is built to be **parallelized**: a short gating phase, then a wide fan‑out of
independent workstreams that meet at well‑defined code seams (§8). Tasks carry IDs, effort
(S/M/L/XL), dependencies, and a `∥`(parallel)/`→`(sequential) marker.

---

## 0. TL;DR — what to do

1. **Phase 0 (GATE):** build mvs64 for samsho2, boot it on the real console, **measure** silent
   FPS + the cpu/io/draw/dma split, and micro‑benchmark YM2610/Z80 cost. Output a **GO/NO‑GO**
   report with the real budget. *Nothing heavy starts until this number exists.*
2. **Phase 1 (Contracts):** fork hygiene + pin libdragon; teach `mvsmakerom` to emit the
   currently‑**dropped** `v.rom`/`m.rom`; land the `sound.h` stub seam; make the SDL build
   deterministic + wire the BizHawk oracle. These unblock everyone.
3. **Phase 2 (Fan‑out):** build Z80→YM2610→N64‑audio (sound), harden video/LSPC, drive
   performance levers, and add input/saves — all in parallel behind the seams.
4. **Phase 3 (Integrate):** first boot **with sound**; re‑profile; tune to the target.
5. **Phase 4 (Validate & ship):** BizHawk MCP A/B + on‑hardware acceptance; package the `.z64`.

---

## 1. Goal & deliverable

| | |
|---|---|
| **Game** | Samurai Shodown II, NeoGeo NGH‑063 (`samsho2.zip`, verified on disk) |
| **Target** | Real N64 console + flash cart (EverDrive‑64 X7 primary, 64drive secondary) |
| **Artifact** | `mvs64-samsho2.z64` (big‑endian native; **not** `.x64` — that is a C64/VICE format) |
| **Definition of done** | Boots on hardware → playable round → saves persist → audio present → validated vs BizHawk |

**`samsho2` ROM set (verified):** P `063-p1` 2 MB (68k program, banked) · C `063-c1..c8` 16 MB
(sprite tiles) · V `063-v1..v4` 7 MB (YM2610 ADPCM) · S `063-s1` 128 KB (fix layer) ·
M `063-m1` 128 KB (Z80). Total ≈ **25.25 MB** → fits a 64 MB cart with room to spare.
**Unencrypted** (ships its own S‑ROM → no CMC; no SMA) → the easiest class of NeoGeo bring‑up.

---

## 2. Why fork mvs64 (not gngeo‑from‑scratch)

The hard parts of NeoGeo‑on‑N64 are all **N64‑specific**, and mvs64 already solved them. gngeo's
speed comes from ARM assembly + CPU software blitting + whole‑ROM‑in‑RAM — **none of which work
on N64** — so a from‑scratch port would have to *rebuild mvs64*, slower and buggier.

| Subsystem | Hard part | mvs64 | gngeo |
|---|---|---|---|
| Fast 68000 on MIPS | N64‑specific, the crux | ✅ `m64k`: ~8 KB asm, fits I‑cache, TLB memory, TomHarte‑tested | ❌ fast cores are ARM asm; only slow C ports |
| Sprite rendering | N64‑specific | ✅ RSP microcode (`rsp_video.S`) + RDP | ❌ CPU software blit (wrong model) |
| 25 MB ROM in 4–8 MB RAM | N64‑specific | ✅ sprite + PBROM caches, DMA‑streamed from cart | ❌ assumes whole ROM in RAM |
| 68k memory map | N64‑specific | ✅ TLB‑mapped | ❌ generic |
| **YM2610 + Z80 sound** | **portable C** | ❌ **absent** | ✅ mature |
| Game compatibility | portable C | ⚠️ early | ✅ broad |

mvs64 below full speed measures **how hard the problem is, not how bad the solution is** — it is
the performance frontier. We add the portable bits it lacks (sound) rather than re‑fighting the
N64 battles. (MIT license — fork is unrestricted.)

---

## 3. mvs64 current state (what we inherit)

**Works / present:** dual **N64 + SDL** build (`platform_n64.c` / `platform_sdl.c`); `m64k`
68000 core (N64) + Musashi (SDL reference); RSP+RDP sprite pipeline with chaining, two‑half
split, tile‑granular V‑shrink, repeat/overfill, fix layer, auto‑animation
(`video.c`/`rsp_video.S`); cart streaming with `crom`/`srom` sprite caches + PBROM bank cache /
TLB‑linear program ROM (`roms.c`); `mvsmakerom` ROM converter; **per‑frame profiler** printing
`cpu/io/draw/dma %` + FPS over USB/ISViewer (`emu.c:289‑322`); frameskip modes; idle‑skip
mechanism; RTC, watchdog, P1 input.

**Missing / broken (our work):**
- **Sound: entirely absent** — no Z80, no YM2610, no ADPCM, no N64 audio output. The
  68k↔Z80 latch is stubbed (`hw.c:125` read returns `1`, `hw.c:157` write is a no‑op).
- **`mvsmakerom` drops V and M ROMs** — only P/C/S/BIOS are packed today (`mvsmakerom.c:364‑368`).
  So the audio data isn't even on the cart yet.
- **`samsho2` (NGH‑063) not in the game DB** — only `GAME_SAMSHO=0x0045` (samsho1).
- **idle_skip broken end‑to‑end** — force‑zeroed at `roms.c:410`; PCs hardcoded at
  `m64k_asm.S:2550‑2551`. A major 68k perf lever currently disabled.
- **`genhle` AOT recompiler won't build** — `Makefile.genhle` includes a missing `n64rasky.mk`.
- **No P2 input, no memory‑card/backup‑RAM persistence, no on‑hardware screenshot.**
- **Stale repo** (last commit 2024‑08‑17) → we own the fork + possible libdragon API drift.

---

## 4. Honest feasibility & performance budget (red‑team)

**Frame budget:** 93.75 MHz / 60 ≈ **1.5625 M VR4300 cycles/frame** (profiler normalizes to
781,250 COUNT ticks). Adversarial worst‑case fighting scene, **CPU‑side** audio:

| Cost | Estimate (% frame) | Notes / lever |
|---|---|---|
| 68k via `m64k` | 26–45% → **15–25%** with idle_skip + fast timing | idle_skip is decisive (currently broken) |
| Z80 @ 4 MHz | 5–12% | idle‑skippable poll loop |
| YM2610 synth @ 22050 Hz | ~4.5–13% (CPU) | ADPCM‑A is ~half; **RSP offload** if over budget |
| Sprite raster (RSP/RDP) | large; the existing bottleneck | COPY‑mode + state elision + early reject |
| C/V‑ROM DMA streaming | 5–15% | coalesce, prefetch, Expansion Pak caches |

**Verdict:**
- **Locked 60 fps + sound on stock 4 MB: NO.** The chassis is already <60 fps on many games
  *without* sound, and 4 MB forces slow PBROM cache mode + leaves no room for ADPCM caches.
- **Realistic deliverable: fully playable samsho2 with the 8 MB Expansion Pak** — sound at
  22050 Hz, 60 fps where the budget allows and **adaptive frameskip** elsewhere, with audio
  prioritized over a locked framerate. **Expansion Pak is a hard requirement for the sound build.**
- **Project‑killer to retire first (Phase 0):** YM2610+Z80 synthesis cost exceeding the spare
  VR4300/RSP headroom. We measure this *before* committing the transplant (WSX experiments).

---

## 5. Prerequisites (user‑supplied / environment)

- **NeoGeo system BIOS** + **`sfix.sfix`** in one folder (`mvsmakerom` hard‑requires both). *Not
  in the repo; never committed (copyrighted).* UniBIOS or a standard MVS/AES BIOS.
- **8 MB Expansion Pak** in the console (required for the sound build).
- **Toolchain:** Windows 11 + **WSL2 Ubuntu** + Docker Desktop + **libdragon‑docker**
  (`npm i -g libdragon`). Repo on WSL **ext4**, not `/mnt/c` (line‑ending + fsync hazards).
- **Flash cart** + USB tooling (UNFLoader for ED64 X7 / 64drive USB) to capture the profiler.
- **BizHawk MCP server** (for Phase 4 validation) — confirm it's connected when that phase starts.

---

## 6. Parallelization model

### Phases & dependency flow

```
 PHASE 0 — DE-RISK (GATE)        PHASE 1 — CONTRACTS            PHASE 2 — PARALLEL BUILD        PHASE 3 — INTEGRATE      PHASE 4 — VALIDATE/SHIP
 ┌───────────────────────┐      ┌────────────────────────┐     ┌──────────────────────────┐
 │ WS0 build+boot+measure │      │ WS9 fork/pin libdragon │     │  WS2 Z80 ─┐              │
 │ WSX budget experiments │ ───► │     sound.h + noop stub│ ──► │  WS3 YM2610├─► sound mod ─┼──► WS9-T14 first  ──► WS8 BizHawk MCP A/B
 │ SDL up + BizHawk probe │      │     v.rom/m.rom emit+IO │     │  WS4 audio-out/RSP offld ┘│     boot WITH sound      + acceptance
 │  => GO/NO-GO + budget   │      │ WS1 samsho2 bring-up   │     │  WS5 video/LSPC harden    │ ──► WS6 re-profile  ──► WS9 release .z64
 └───────────────────────┘      │ WS8 SDL determ.+golden │     │  WS6 perf levers          │     + tune to target      + FLASHING.md
                                 └────────────────────────┘     │  WS7 input/save/region    │
                                                                └──────────────────────────┘
   (single-thread gate)            (1-2 devs, unblock all)        (4 parallel tracks)            (merge order)         (finale, BizHawk)
```

### Critical path (longest pole = the sound chain)

```
WS0 (measure) → WS1/WS9 (v.rom+m.rom emitted, sound.h seam) → WS2 (Z80) → WS3 (YM2610)
            → WS4 (N64 audio out) → WS9-T14 (integrate: first sound) → WS6 (tune to budget)
            → WS8 (BizHawk validate) → WS9 (release)
```

Everything **not** on that line — video hardening (WS5), perf levers (WS6), input/saves (WS7),
the whole validation harness (WS8) — runs **in parallel** and feeds the budget/quality.

### Suggested team allocation (if multiple contributors)

| Track | Workstreams | Can start after |
|---|---|---|
| **Gate** | WS0 + WSX | now |
| **Platform/Build** | WS9, WS1 | WS0 toolchain (WS0‑T1) |
| **Sound** | WS2 → WS3 → WS4 | WS9 `sound.h` + `v.rom`/`m.rom` |
| **Video** | WS5 | WS1 DB entry |
| **Perf** | WS6 | WS0 profiler baseline |
| **System** | WS7 | independent |
| **Validation** | WS8 | SDL build (WS0‑T10) |

---

## 7. Workstreams

> Effort: **S**<½d · **M** 1‑3d · **L** ~1wk · **XL** multi‑wk. Marker: `∥` parallel · `→` sequential.

### WS0 — Build, Boot & Measure spike *(GATE — feasibility=high)*
Stand up the toolchain, build & boot samsho2 on real hardware, and **measure**. Produces the
GO/NO‑GO number that sizes everything else. Everything WS0 needs already exists and is wired.

| ID | Task | Eff | Dep | ∥ |
|---|---|---|---|---|
| WS0‑T1 | Install WSL2 + Docker + libdragon‑docker; export `N64_INST` | M | — | ∥ |
| WS0‑T2 | Sync repo into WSL ext4; fix LF/`mkdir(0777)` hazards | S | T1 | → |
| WS0‑T3 | Stage BIOS+`sfix.sfix` (one folder) and `samsho2.zip` | S | T1 | ∥ |
| WS0‑T4 | Build `mvsmakerom` host‑side; validate samsho2 conversion (code `0x0063`, non‑SMA path) | M | T2,T3 | → |
| WS0‑T5 | Register samsho2 (NGH‑063) in converter (enum + game_ini) | M | T4 | → |
| WS0‑T6 | `make mvs64 ROM=samsho2.zip BIOS=…` → `mvs64-samsho2.z64` | M | T5 | → |
| WS0‑T7 | Flash to ED64 X7 / 64drive; cold‑boot; record boot state | M | T6 | → |
| WS0‑T8 | Capture profiler + FPS over USB (UNFLoader/64drive) | M | T7 | → |
| WS0‑T9 | Measure lever deltas: Expansion Pak × frameskip × idle_skip × timing‑accuracy | L | T8 | → |
| WS0‑T10 | Build + run **SDL `emu`** on Windows for A/B reference | M | T4 | ∥ |
| WS0‑T11 | **GO/NO‑GO report** with the number + dominant cost bucket | M | T9,T10 | → |
| WS0‑T12 | (Stretch) get `genhle` building or formally defer | L | T8 | ∥ |
| **WSX‑1..7** | Red‑team budget experiments: snd `%` profiler bucket, 4 MB vs 8 MB baseline headroom, **YM2610 cycles/sample micro‑bench (CPU & RSP)**, Z80 cost probe, ADPCM‑DMA model, consolidated 4‑config go/no‑go, fast‑timing/frameskip calibration | M–L | — | ∥ |

**Top risks:** samsho2 may not boot past BIOS (mitigate in SDL first); silent build may be far
<60 fps (that's a valid NO‑GO output); profiler unreadable on stock console (use cart USB).

### WS1 — samsho2 bring‑up (game DB, ROM mapping, LSPC correctness) *(high)*
Make NGH‑063 specifically load and render right; fix idle_skip plumbing; close LSPC gaps.

| ID | Task | Eff | Dep | ∥ |
|---|---|---|---|---|
| WS1‑T1 | Add `GAME_SAMSHO2=0x0063` to enum + `game_ini` | S | — | ∥ |
| WS1‑T2 | Run mvsmakerom; verify P=1 MB`p.rom`+1 MB`b.rom`, C≈16 MB, S slicing | S | T1 | → |
| WS1‑T3 | **Emit `v.rom` (7 MB) + `m.rom` (128 KB)** from makerom *(unblocks sound)* | M | T1 | ∥ |
| WS1‑T4 | **Fix idle_skip:** remove `roms.c:410` kill; make `m64k_asm.S:2550‑1` data‑driven | L | — | ∥ |
| WS1‑T5 | Find samsho2 idle‑loop PC; tune idle_skip | M | T4,T2 | → |
| WS1‑T6 | Verify PBROM linear mapping on 4 MB and 8 MB | M | T2 | → |
| WS1‑T7 | Implement LSPC timer interrupt (IRQ2) for raster effects | L | — | ∥ |
| WS1‑T8 | A/B sprite shrink + overfill vs BizHawk on samsho2 | L | T2 | → |
| WS1‑T9 | Verify auto‑animation + fix layer | M | T2 | ∥ |
| WS1‑T10 | Add samsho2 `patch_game` hook if a layout quirk appears (expected no‑op) | S | T2,T8 | → |

**Top risks:** idle_skip broken end‑to‑end (T4 fixes); LSPC timer IRQ2 unimplemented (T7);
makerom drops V/M (T3); shrink/overfill FIXMEs in `video.c`.

### WS2 — Z80 sound CPU *(high)*
Vendor a permissive Z80 core, wire the memory map + banking + 68k↔Z80 latch + timer IRQ/NMI,
and step it in lockstep with `m64k`. ~4–7% of frame; integration surface is tiny.

| ID | Task | Eff | Dep | ∥ |
|---|---|---|---|---|
| WS2‑T1 | Vendor + build **cz80** (MIT/BSD), dual N64+SDL, behind a `z80.c/.h` wrapper | M | — | ∥ |
| WS2‑T2 | Z80 memory map + 4‑window bank controller | M | T1,T6 | → |
| WS2‑T3 | Sound‑command latch; replace `hw.c:125/157` stubs (cmd→NMI, reply readback) | M | T1 | → |
| WS2‑T4 | 68k‑side Z80 reset/halt lines (0x3A region) | S | T1 | → |
| WS2‑T5 | `emu.c` timeslice integration (`z80_exec` + `z80_sync`) | L | T1,T3 | → |
| WS2‑T6 | M‑ROM resident load path in `roms.c` | S | — | ∥ |
| WS2‑T7 | (with WS1‑T3) makerom emits `m.rom`/`v.rom` + samsho2 DB | M | — | ∥ |
| WS2‑T8 | YM2610 timer IRQ/NMI line ownership (WS3 boundary) | M | T1,T5 | → |
| WS2‑T9 | SDL validation harness vs BizHawk (latch stream + IRQ cadence) | L | T2‑T5 | → |
| WS2‑T10 | N64 cycle‑budget profiling + slice tuning | M | T5,T8 | → |

**Top risks:** latch handshake ordering (force `z80_sync` before any 68k latch access);
I‑cache contention with `m64k` (coarse slices, profile).

### WS3 — YM2610 emulation (FM + SSG + ADPCM‑A/B) *(medium)*
Port a **fixed‑point** YM2610 producing stereo PCM; stream the 7 MB ADPCM source from cart.
Use **ymfm (BSD‑3)** or MAME `fmopn` math. **No floating point in the hot path.**

| ID | Task | Eff | Dep | ∥ |
|---|---|---|---|---|
| WS3‑T1 | Add V/M extraction to `mvsmakerom` (flat, no byteswap/interleave) | M | — | ∥ |
| WS3‑T2 | Wire `v.rom`/`m.rom` into `rom_load` + dfs open | S | T1 | → |
| WS3‑T3 | **`vrom_cache`** — ADPCM streaming cache (prefetch one chunk ahead/voice) | L | T2 | → |
| WS3‑T4 | Port FM(4ch)+SSG(3ch) integer core; gen tables once at init | XL | — | ∥ |
| WS3‑T5 | Port ADPCM‑A×6 + ADPCM‑B decoders (stream via `vrom_cache`) | L | T3,T4 | → |
| WS3‑T6 | Timer A/B + status reg + IRQ hook (binds to WS2‑T8) | M | T4 | → |
| WS3‑T7 | Register interface `ym2610_write/read` (Z80 IO 0x04‑0x07) | M | T4‑T6 | → |
| WS3‑T8 | N64 audio output in `platform_n64.c` (libdragon `audio_*`, 22050 Hz) | M | — | ∥ |
| WS3‑T9 | `ym2610_update()` in the frame loop | S | T7,T8 | → |
| WS3‑T10 | Profile on N64; **RSP‑offload decision gate** | L | T9 | → |
| WS3‑T11 | SDL A/B vs BizHawk (boot jingle, SFX, BGM; per‑channel mute) | M | T7 | ∥ |

**Top risks:** total CPU over budget (build incrementally, 22050 Hz default, RSP‑offload ready);
V‑ROM cache‑miss stall → clicks (exploit monotonic ADPCM cursor, prefetch); float creeping in
(audit hot path); wrong V‑ROM concat order.

### WS4 — N64 audio output (AI) + RSP offload *(high)*
Own the AI double‑buffered output + the producer/consumer **ring buffer**, and the RSP audio
offload *decision* (gated on measured cost). The CPU path ships first; RSP is the escalation.

| ID | Task | Eff | Dep | ∥ |
|---|---|---|---|---|
| WS4‑T1 | libdragon AI backend in `platform_n64.c` (`plat_*audio`) | M | — | ∥ |
| WS4‑T2 | Sample‑rate constant (default 22050; verify realized DAC rate) | S | T1 | → |
| WS4‑T3 | Decoupling stereo ring buffer + AI pump (zero‑fill on underrun) | L | T1 | → |
| WS4‑T4 | Produce audio **per emulated frame** (not per rendered frame) | M | T1,T3 | → |
| WS4‑T5 | Define `ym2610_update(int16*, nsamples)` fill contract (header+stub) | S | T4 | ∥ |
| WS4‑T6 | Underrun/overrun + mix‑cost instrumentation in `[PROFILE]` | S | T4 | ∥ |
| WS4‑T7 | Profile audio cost; **RSP offload go/no‑go** | M | T6 | → |
| WS4‑T8 | (gated) RSP ADPCM‑decode + accumulate overlay via `rspq` | XL | T7 | → |
| WS4‑T9 | SDL parity for identical sample streams (A/B) | S | T5 | ∥ |

**Top risks:** CPU starvation pushing over budget (22050 Hz, profile, gate RSP); strict
per‑rendered‑frame buffering underruns on frameskip (ring buffer + per‑emulated‑frame produce);
RSP contention with sprite blitter (cooperative `rspq`, schedule in vblank); 367.5 samples/frame
non‑integer drift (fractional accumulator).

### WS5 — Video / LSPC accuracy + RSP/RDP hardening *(high)*
The pipeline is feature‑complete and runs other games; this is **bug‑hardening + throughput**,
not green‑field. Fix shrink/overfill FIXMEs, add shadow/darken, and cut RDP per‑tile overhead.

| ID | Task | Eff | Dep | ∥ |
|---|---|---|---|---|
| WS5‑T1 | Add samsho2 to DB; verify unencrypted C/S preprocess | S | — | ∥ |
| WS5‑T2 | **Write CI4 tile + TLUT format contract** + golden‑tile self‑test | M | T1 | ∥ |
| WS5‑T3 | samsho2 sprite/scene A/B test harness on SDL | M | T1 | ∥ |
| WS5‑T4 | Fix `ssh`‑vs‑`sh` vertical clip + overfill FIXMEs (`video.c`) | L | T3 | → |
| WS5‑T5 | Exact per‑line V‑shrink on RSP/RDP path *(hardest item)* | XL | T4 | → |
| WS5‑T6 | RSP `cmd_sprite_draw` redundant‑state elision + LoadBlock batching | L | T3 | ∥ |
| WS5‑T7 | COPY‑mode fast path for unscaled onscreen tiles (RSP) | L | T6 | → |
| WS5‑T8 | Early offscreen sprite rejection (bounding box) | M | T4 | ∥ |
| WS5‑T9 | Shadow / global‑darken (DARK bit) in palette convert | L | T3 | ∥ |
| WS5‑T10 | Coalesce chained‑sprite tile DMA; remove per‑tile flush FIXME | L | T3 | ∥ |
| WS5‑T11 | Retune `crom`/`srom` cache sizes for samsho2 (16 MB C) | M | T10 | → |
| WS5‑T12 | Validate fix layer + auto‑animation timing | M | T3 | ∥ |
| WS5‑T13 | End‑to‑end perf pass + budget sign‑off on hardware | M | T5‑T11 | → |

**Top risks:** exact per‑line V‑shrink may be infeasible in RSP budget (keep tile‑granular
fallback); hand‑written RSP asm regressions (keep 1‑cycle path selectable, diff on SDL first);
CROM DMA miss storms on transitions (coalesce + frameskip + Expansion Pak).

### WS6 — Performance engineering & `genhle` *(medium)*
Reach a stable playable framerate using the existing levers, reserving budget for sound.
**Locked 30 fps + frameskip = high feasibility; solid 60 fps with sound = low‑medium.**

| ID | Task | Eff | Dep | ∥ |
|---|---|---|---|---|
| WS6‑T1 | Release‑gate hot logging; delete per‑timeslice `debugf` (`emu.c:72`) | S | — | ∥ |
| WS6‑T2 | Harden/extend profiler (+RSP busy%, +RDP busy%, +snd%) | M | T1 | → |
| WS6‑T3 | **Data‑driven idle_skip in `m64k`** (C‑populated PC table) | M | — | ∥ |
| WS6‑T4 | Determine samsho2 idle PC(s); add to DB | M | T3 | → |
| WS6‑T5 | `M64K_CONFIG_TIMING_ACCURACY` sweep {0,−8,−10,−12} + select | M | T2 | → |
| WS6‑T6 | Confirm/force Expansion Pak linear PBROM; assert if absent | M | T2 | ∥ |
| WS6‑T7 | Grow C/S caches when 8 MB present | M | T2,T6 | → |
| WS6‑T8 | Async/overlapped PI DMA for C‑ROM tile streaming | L | T2 | → |
| WS6‑T9 | Cost‑driven adaptive frameskip (mode 2) | M | T2 | ∥ |
| WS6‑T10 | Reserve & verify sound CPU/RSP budget after WS3/WS4 | M | T5,T9 | → |
| WS6‑T11 | `genhle` AOT recompiler spike (>15% hot‑block win gate; default OFF for v1) | XL | T2,T4 | ∥ |
| WS6‑T12 | Host‑side profiling harness (ingest per‑frame CSV) | S | T2 | ∥ |

**Top risks:** `genhle` is Musashi‑coupled & unwired (time‑box, default not‑ship); negative
timing accuracy desyncs raster/IRQ/audio (validate each value); wrong idle PC hangs game;
sound cost over budget (reserve headroom up front).

### WS7 — Input, save (memory card), frontend *(high)*
All small, N64‑side. Add P2, persist saves to cart flash, soft‑reset/pause/region.

| ID | Task | Eff | Dep | ∥ |
|---|---|---|---|---|
| WS7‑T1 | Expand `PLAT_KEY_*` (P2, coin, reset, pause, region) | S | — | ∥ |
| WS7‑T2 | Rewrite `plat_poll` for 2 controllers + combos (N64) | M | T1 | → |
| WS7‑T3 | Mirror remap in SDL `plat_poll` | S | T1 | ∥ |
| WS7‑T4 | Add P2 port read + decode `0x340000` | S | T2 | → |
| WS7‑T5 | Define `plat_save_*` persistence API | S | — | ∥ |
| WS7‑T6 | Implement `plat_save_*` on N64 (FlashRAM/SRAM via libdragon) | L | T5 | → |
| WS7‑T7 | Implement `plat_save_*` on SDL (`.sav`) | S | T5 | ∥ |
| WS7‑T8 | Backup‑RAM load/store + dirty flag (`0xD` bank) | M | T6 | → |
| WS7‑T9 | Memory‑card buffer/bank/status bits (`0x800000`) | L | T8 | → |
| WS7‑T10 | Decode memory‑card control writes (`0x380000`) | M | T9 | → |
| WS7‑T11 | Soft reset (flush‑then‑reset) | S | T2,T6 | → |
| WS7‑T12 | Pause | M | T2 | → |
| WS7‑T13 | Region (AES/MVS) variable | M | T4 | → |
| WS7‑T14..T16 | samsho2 DB metadata; parse new `game.ini` keys; stamp N64 save type | S | — | ∥ |
| WS7‑T17 | On‑hardware save durability test (power‑cycle) | M | T8,T9,T16 | → |
| WS7‑T18 | (optional) game‑select frontend | L | T2 | ∥ |

**Top risks:** flash‑cart save‑type honoring varies (stamp header, test both carts, in‑emu "save
now"); power‑off before debounced flush (short debounce + flush on reset/pause); register‑decode
collision with sound in `0x320000/0x380000` (single owner per branch).

### WS8 — Validation: BizHawk MCP oracle + SDL A/B *(high)*
Layered verification culminating in the **user‑required BizHawk MCP** comparison. Mostly additive
plumbing on the SDL build (which is the first‑line oracle). **Adds zero cost to the shipping ROM.**

| ID | Task | Eff | Dep | ∥ |
|---|---|---|---|---|
| WS8‑T1 | SDL `--frames/--headless/--no-audio` flags | M | — | ∥ |
| WS8‑T2 | Honor `game.ini` idle_skip on PC build (un‑zero `roms.c:410`) | S | — | ∥ |
| WS8‑T3/T4 | Inputlog format + record + **replay** (deterministic) | M | T1 | → |
| WS8‑T5 | Checkpoint dumper (RAM+regs CRC + screenshot) | L | T1 | → |
| WS8‑T6 | samsho2 in DB; build `game.n64/` | S | — | ∥ |
| WS8‑T7 | samsho2 scenario script + checkpoint list | M | T3,T6 | → |
| WS8‑T8 | Generate SDL golden corpus (Tier‑1 baseline) | S | T4,T5,T7 | → |
| WS8‑T9 | Comparator + tolerance policy (exact CRC / SSIM+mask) | L | T5 | ∥ |
| WS8‑T10 | **BizHawk MCP adapter** (probe live server, map tools) | L | — | ∥ |
| WS8‑T11 | BizHawk oracle harness (replay→checkpoints→compare) | L | T7,T9,T10 | → |
| WS8‑T12 | Implement N64 `plat_save_screenshot` + state out (USB/SD) | L | T5 | → |
| WS8‑T13 | N64‑vs‑SDL A/B run + idle_skip profiling | M | T8,T12 | → |
| WS8‑T14 | Audio A/B hooks (after sound lands) | L | T5,T11 | → |
| WS8‑T15 | CI regression runner (headless replay vs golden) | M | T8,T9 | → |
| WS8‑T16 | **Real‑hardware acceptance checklist** (`ACCEPTANCE.md`) | S | T13 | ∥ |

**Top risks:** BizHawk MCP tool surface unknown until probed (isolate behind adapter; Tiers 1/3
work without it); exact pixel parity unattainable (tolerance tiers; exact only on static
screens/RAM); BizHawk is an oracle, not ground truth (final arbiter = on‑hardware checklist).

### WS9 — Fork hygiene, libdragon drift, integration & release *(high)*
Pure plumbing, no algorithmic risk. Owns the seams that let everyone parallelize, the pinned
build, CI, and the shippable artifact.

| ID | Task | Eff | Dep | ∥ |
|---|---|---|---|---|
| WS9‑T1 | MIT fork + `upstream` remote + `NOTICE`/provenance (✔ repo already created) | S | — | ∥ |
| WS9‑T2 | **Pin libdragon** as submodule at a 2024‑era known‑good commit | M | T1 | → |
| WS9‑T3 | Fix `genhle` build (restore/replace `n64rasky.mk`) → `make all` green | S | T2 | ∥ |
| WS9‑T4 | samsho2 in DB + idle_skip | S | — | ∥ |
| WS9‑T5 | makerom emits `v.rom`(7 MB)+`m.rom`(128 KB) | M | T4 | → |
| WS9‑T6 | `v.rom`/`m.rom` loader + accessor stubs in `roms.c` | M | T5 | → |
| WS9‑T7 | **`sound.h` module API** + `sound_noop.c`; route `hw.c` latches through it | M | T2 | → |
| WS9‑T8 | N64 audio backend in `platform_n64.c` | M | T2 | ∥ |
| WS9‑T9 | Per‑frame audio event in emu loop (no‑op safe) | M | T7,T8 | → |
| WS9‑T10 | CI: build `.z64` + SDL `emu` on every PR | L | T2,T3 | ∥ |
| WS9‑T11 | CI: headless SDL boot smoke + golden screenshot CRC | L | T10 | → |
| WS9‑T12 | Release packaging: assert `.z64` magic `80 37 12 40`, title, SHA256 | S | T10 | ∥ |
| WS9‑T13 | `FLASHING.md` user guide (ED64 X7 / 64drive, Expansion Pak, BIOS) | S | T12 | ∥ |
| WS9‑T14 | **Integration milestone: first samsho2 boot WITH sound** | L | T6,T9,T11 | → |
| WS9‑T15 | (backlog) port to current libdragon | XL | T10 | ∥ |

**Top risks:** **license contamination** — transplanting GPL code (gngeo/MAME) makes the binary
GPL‑effective (prefer **ymfm BSD** + **cz80**; land `NOTICE` + per‑file provenance; treat combined
work's redistribution accordingly); libdragon API drift (pin); V/M‑ROM concept missing (prioritize
T5/T6); ship a byteswapped image (CI asserts big‑endian magic; document `.z64`, not `.x64`).

---

## 8. Cross‑workstream contracts (the seams that enable parallel work)

These are the **stable interfaces** so teams can build behind stubs and integrate cleanly.

1. **`sound.h` module boundary** *(WS9‑T7)* — the single seam between the chassis and all sound:
   ```c
   void    sound_init(const uint8_t *mrom, /*vrom handle*/, uint32_t vrom_size);
   void    sound_reset(void);
   void    sound_write_command(uint8_t cmd);   // 68k → Z80 (replaces hw.c:157)
   uint8_t sound_read_status(void);            // Z80 → 68k (replaces hw.c:125)
   int     sound_gen_samples(int16_t *out, int nsamples);  // = ym2610_update
   ```
   `sound_noop.c` satisfies it from day 1 so existing games never break.
2. **Audio fill** *(WS3/WS4)* — `void ym2610_update(int16_t *out, int nsamples)`: interleaved
   **stereo S16** at `AUDIO_FREQ` (default **22050**). Producer runs **per emulated frame**;
   consumer is a ring buffer + AI pump (zero‑fill on underrun).
3. **`v.rom` / `m.rom` file format** *(WS1/WS9 → WS2/WS3)* — `m.rom` = raw `063-m1` (128 KB, no
   byteswap, Z80 little‑endian); `v.rom` = `063-v1..v4` concatenated in index order (7 MB, **flat,
   no byteswap, no interleave**). `v.rom` is **streamed** from cart via the `crom`‑style cache.
4. **CI4 tile + TLUT contract** *(WS1 → WS5)* — `c.rom` = 128‑byte CI4 tiles (8 bytes wide × 16
   rows), `s.rom` = 4×8 CI4 fix tiles; 16‑entry RGBA16 TLUT order per `mvsmakerom` preprocess.
   Versioned spec + golden‑tile round‑trip self‑test in CI.
5. **idle_skip table** *(WS6 → WS1/core)* — `extern uint32_t m64k_idle_skip_pcs[4];`
   (zero‑terminated, `M64K_CONFIG_MEMORY_BASE`‑mapped addresses); replaces the hardcoded
   `m64k_asm.S:2550‑2551` compares.
6. **Z80↔YM2610 timer line** *(WS2/WS3)* — YM2610 Timer A/B overflow calls a registered IRQ
   callback; WS2 owns Z80 mode‑1 IRQ vectoring + the enable port.
7. **Save API** *(WS7)* — `plat_save_init/load/mark_dirty/tick/flush_all`, regions
   `SAVE_BACKUP_RAM` / `SAVE_MEMCARD`, persisted to cart FlashRAM/SRAM.
8. **Checkpoint state contract** *(WS8)* — read‑only CRC of `WORK_RAM/BACKUP_RAM/VIDEO_RAM/
   PALETTE_RAM` + `m64k_t` regs + RGBA5551 screenshot, versioned.

---

## 9. Milestone ladder & GO/NO‑GO gates

| # | Milestone | Gate |
|---|---|---|
| **G0** | samsho2 boots in **SDL** (Musashi) on PC | proves emulation‑layer correctness off‑target |
| **G1** | `mvs64-samsho2.z64` builds; **boots on real N64** to attract/in‑game | chassis viable for samsho2 |
| **G2** | **Silent‑build FPS measured** on hardware + lever matrix | **GO/NO‑GO #1** — is the budget plausible? |
| **G3** | YM2610/Z80 **cycles/sample measured** (WSX) | **GO/NO‑GO #2** — does sound fit (4 MB vs 8 MB, CPU vs RSP)? |
| **G4** | `v.rom`/`m.rom` emitted; `sound.h` seam + SDL determinism live | unblocks the fan‑out |
| **G5** | **First sound on hardware** (WS9‑T14) end‑to‑end | the project's hardest integration |
| **G6** | Re‑profiled with sound; **playable target hit** (60 fps / agreed frameskip, 8 MB) | performance sign‑off |
| **G7** | **BizHawk MCP A/B passes** + on‑hardware acceptance checklist | **definition of done** |

If **G2/G3 fail to close**, pivot scope: drop to locked 30 fps + frameskip, lower audio rate, or
(worst case) ship a silent but full‑speed build first and add sound as a follow‑on.

---

## 10. Risk register (top, cross‑cutting)

| Sev | Risk | Mitigation |
|---|---|---|
| **critical** | YM2610+Z80 cost exceeds spare VR4300/RSP headroom → no 60 fps+sound | Measure first (WSX‑2/3); gate transplant on it; pivot scope if needed |
| high | 4 MB working set overflow (PBROM cache mode + ADPCM + audio buffers) | **Require 8 MB Expansion Pak for sound**; refuse/disable sound on 4 MB |
| high | RSP contention: audio offload vs sprite blitter (already the bottleneck) | Cooperative `rspq`; schedule audio synth in vblank; double‑buffer |
| high | ADPCM streaming DMA competes with sprite‑cache DMA → dropouts | Per‑voice read‑ahead (monotonic cursor); prioritize audio DMA; 8 MB |
| high | License contamination from GPL transplants | Prefer **ymfm (BSD)** + **cz80**; `NOTICE` + provenance; handle redistribution accordingly |
| high | libdragon API drift breaks the stale fork | **Pin** a 2024‑era libdragon submodule; CI builds the pin |
| medium | Fast‑timing / aggressive idle_skip break raster/IRQ/audio sync | Validate each lever vs SDL/BizHawk; keep `TIMING_ACCURACY=0` default |
| medium | BizHawk MCP tool surface unknown / server absent | Adapter probe (WS8‑T10); non‑BizHawk tiers stand alone |

---

## 11. Validation & acceptance (the finale)

Three tiers, in order of cost:

1. **Tier 1 — SDL self‑consistency:** deterministic headless SDL `emu` with record/replay;
   checkpoint CRCs + screenshots → golden corpus; runs in CI on every change.
2. **Tier 2 — BizHawk MCP oracle** *(user‑required):* replay the same scenario into BizHawk's
   NeoGeo core via the MCP adapter; compare screenshots (exact on static screens, SSIM+mask on
   gameplay), NeoGeo RAM/registers, and — once sound lands — PCM (spectral/cross‑correlation).
   Per the mvs64 README split: a bug in **SDL** ⇒ emulation‑layer; **N64‑only** ⇒ backend.
3. **Tier 3 — On‑hardware acceptance** *(final arbiter):* `ACCEPTANCE.md` — boots to BIOS,
   reaches title within *X* s, character‑select navigable, **one full round playable**, sustained
   FPS target met, audio present and in‑sync, **save survives a power cycle** on ED64 X7 + 64drive.

> BizHawk is a strong reference, **not** ground truth; ambiguous cases are decided on real hardware.

---

## 12. Licensing note

mvs64 is **MIT** (kept verbatim; `LICENSE`). To keep our redistributable clean, prefer
**permissively‑licensed** transplants: **cz80** (Z80) and **ymfm, BSD‑3‑Clause** (YM2610/SSG/ADPCM
math). If any **GPL** code (gngeo/MAME) is used, the combined binary is GPL‑effective for
redistribution — add a `NOTICE` + per‑file provenance headers and treat distribution accordingly.
**Never commit ROMs/BIOS** (enforced by `.gitignore`).

---

## 13. Immediate next actions

1. **WS0‑T1..T6** — toolchain up, register samsho2, build `mvs64-samsho2.z64`.
2. **WS0‑T10** — SDL `emu` running samsho2 on PC (fastest correctness feedback).
3. **WS0‑T7..T11 + WSX** — boot on hardware, **measure**, deliver GO/NO‑GO with the real budget.
4. In parallel: **WS9‑T1/T2/T5/T7** (fork hygiene, pin libdragon, emit `v.rom`/`m.rom`, `sound.h`
   seam) and **WS1‑T4** (fix idle_skip) — these unblock the whole fan‑out.

*Plan derived from a source‑grounded analysis of the mvs64 tree; task IDs map to the engineering
breakdown. Workstreams WS2–WS9 develop in parallel behind the §8 contracts.*
