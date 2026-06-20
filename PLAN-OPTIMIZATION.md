# PLAN-OPTIMIZATION.md — mvs64 / samsho2 N64 Framerate Plan

> Produced 2026-06-19 by an 8-expert parallel audit (analyze → adversarial-verify → synthesize).
> Goal: raise framerate toward 60fps **without frameskip** and **without deviating from the
> original arcade experience** (cycle timing, audio sync, input feel must stay intact).

## ⚠️ Empirical update (2026-06-19, after implementing rank 1)

Rank 1 (idle-skip) was implemented and **measured**, which corrected the plan's central
assumption. An in-match idle probe (`-DMVS64_IDLEPROBE`, the m_pc histogram from Gate 3)
found samsho2's real spin: **0x00142C** (`clr.b $100A30 ; 142C: tst.b $100A30 ; beq 142C` —
a verified pure vblank-wait poll; secondary at 0x00FC02). Both were added to the idle-skip
list (`m64k_asm.S`). Testsuite stayed green (only the known CHK edge fails).

**But the in-match framerate is essentially flat (~19.5→~19.8 emu-fps; cpu% median 288% vs
the 272% baseline — within noise).** The reason: in a heavy match samsho2 is genuinely
**CPU-bound** — it nearly fills the entire emulated frame budget with real work (game logic,
the per-frame DBRA copy loops at 0x3200/0x31FE, sprite setup, sound), leaving little idle
spin to skip. Idle-skip therefore helps **idle-heavy scenes (boot, menus, light scenes) and
other games**, and is correct/safe to keep, but it is **NOT the in-match fps lever**.

**Reprioritization:** the in-match fps headline now belongs to **m64k interpreter throughput**
— rank 4 (inline RMW fast paths for the hot ALU ops) and the rank-14 dispatch/icache items,
gated by the testsuite (close the ADDQ/SUBQ gap first). These are `effort=large` and are the
focus of the next optimization session. Idle-skip + the quick wins are banked; treat the
roadmap below as still valid but with ranks 4/14 promoted above the other low-gain items.

## Intro

samsho2 already boots and is playable with audio on the hand-written m64k 68000
interpreter, but runs at ~22 fps. The per-frame profile (in-game match, N64 ticks)
is the optimization target:

```
cpu(68k emulation): ~272%   io(HWIO/TLB): ~9%   draw(RSP/RDP): ~13%   dma: ~0%
```

The bottleneck is unambiguously the **68k interpreter (cpu, 2.72x the frame budget)**.
It is CPU-bound, not draw-bound. **Frameskip is off the table.**

Two levers dominate:

1. **Idle-loop skipping — the single highest-value win, and it is currently DEAD for
   samsho2.** `roms.c:481` unconditionally executes `rom_pc_idle_skip = 0;` right after
   parsing it from `game.ini` (`roms.c:478`), and the m64k asm only matches two hardcoded
   literal PCs at `m64k_asm.S:2552-2553` (`0xFF001FE2` mslug, `0xFFC18714` BIOS vblank
   wait) — neither is samsho2's in-game SYSTEM_IO spin (candidate `0xC128A6`). The skip
   idiom (`li m_cycles,0; j main_loop`) only fast-forwards the dead remainder of a
   timeslice to the next scheduled IRQ — the same point the timeslice would reach anyway —
   so it is **accuracy-safe for a true spin**.
2. **The m64k dispatch / RMW per-instruction cost.** A typical ALU op pays 3–4 indirect
   jumps plus a dependent cycle-table load. Inlining the dominant register-direct forms is
   a real, bounded win, but `effort=large` and must ride a green testsuite.

Everything else (io read fast-path, video/dma, sound) is verified-real but bounded and is
correctness/cleanliness/fill-in, **not** an fps lever.

---

## Parallel Work-Streams (Waves)

### Wave 0 — Serial prerequisite (everyone blocks on this)
- Stand up the benchmark + testing harness (see Testing Strategy).
- Capture **baseline** `[PROFILE]` cpu/io/draw/dma + total frame ticks over a scripted,
  input-replay-deterministic in-game match.
- Run the TomHarte m64k testsuite (123/124 baseline) and **close the ADDQ/SUBQ coverage
  gap** — the RMW work touches exactly those flag/cycle paths.
- Instrument an **m_pc histogram** over an in-game match to find samsho2's dominant spin
  PC(s). Unblocks rank 1.

### Wave 1 — Three streams in parallel
- **Stream A (m64k core, critical path):** idle-skip discovery + wiring (ranks 1–2), the
  `roms.c:481` deletion. Serializes internally — one agent on the asm hot path at a time,
  every change gated by TomHarte + `[PROFILE]`.
- **Stream B (io read fast-path):** rank 5. Independent at the file level; real gain only
  after A's idle-skip lands.
- **Stream C (video/dma + sound, zero 68k-timing coupling):** ranks 3, 6, 7 now; 9–13 in
  Wave 2.

### Wave 2 — Re-profile, integrate, larger/riskier work
- Re-capture `[PROFILE]` after Wave 1; re-prioritize on what now dominates.
- Stream A: RMW inlining (rank 4), op_stop fast-forward (8), measure-first dispatch/
  interrupt/icache items (14) **only if** still dominant.
- Stream C: Z80 inlining (9), sound emission decoupling (10), Y-cull (11), CROM coalesce
  (13). Stream B: hw.c cleanup (12).

### Cross-stream serialization points
- `roms.c` — touched by A (idle wiring, line 481) and C (CROM path). Split by line region.
- `sound_neogeo.c` — B drops `static` on `result_code`; C touches snd findings. Trivial.

---

## Ranked Roadmap

| # | Item | Gain | Acc. risk | Effort | Stream/Wave |
|---|------|------|-----------|--------|-------------|
| 1 | Discover + add samsho2 real idle PC(s) to beq list (`m64k_asm.S:2552`) | high | low | small | A / W1 |
| 2 | Data-driven idle-skip table (Tier 1) + remove `roms.c:481` | medium | low | medium | A / W1 |
| 3 | Call `rom_next_frame()` once/frame (remove `emu.c:306` or `:476`) | low | none | small | C / W1 |
| 4 | Inline EA-reg-direct / Dn,#imm RMW fast paths | medium | low | large | A / W2 |
| 5 | asm READ fast-path for hot side-effect-free regs (`hw_n64.S:584`) | low | low | medium | B / W1 |
| 6 | emit() direct-write (drop redundant copy) | low | none | small | C / W1 |
| 7 | Deterministic `sprite_cache_pop` (drop rand()) | low | none | small | C / W1 |
| 8 | op_stop halt-until-IRQ fast-forward | low | medium | small | A / W2 |
| 9 | Inline Z80 bus + hoist per-step interrupt check | low | low | medium | C / W2 |
| 10 | Decouple YM2610 emission from Z80 timer slicing | low | medium | medium | C / W2 |
| 11 | Coarse Y-axis sprite culling | low | medium | medium | C / W2 |
| 12 | hw.c slow-path cleanup (sz==4 dead branch, tail-call) | low | low | small | B / W2 |
| 13 | CROM coalesce / drop per-tile cache invalidate | low | medium | medium | C / W2 |
| 14 | DEFER/measure-first: interrupt hoist, dispatch widen, icache cluster | low | medium | medium | A / W2 |

---

## Testing / Benchmark Strategy

**Gate 1 — TomHarte m64k testsuite (correctness, hard gate for any m64k change).**
123/124 baseline, run in ares via stdout. **Close the ADDQ/SUBQ coverage gap in Wave 0**
before any RMW-touching work (TomHarte has no ADDQ/SUBQ file — synthesize vectors or add a
dedicated test). Document the 1 known failure so a NEW failure is detectable.

**Gate 2 — Perf-regression harness (the built-in benchmark).** Scrape the per-frame
`[PROFILE]` cpu/io/draw/dma from ares stdout during a scripted, **input-replay-deterministic**
match (use the `-DMVS64_AUTOINPUT` harness). Baseline in Wave 0; every item reports its delta.

**Gate 3 — Idle-PC discovery + validation (idle-skip specific).** m_pc histogram finds the
spin PC(s); each candidate is **proven side-effect-free** before adding; then A/B on
(a) `[PROFILE]` cpu%, (b) BizHawk frame-counter boot trace, (c) WAV + autocorrelation audio
loop. `lspc_mode_r` (raster line, `lspc.c:42`) is the **negative control** — it must NOT be
skipped (its polled value legitimately advances).

**Gate 4 — Visual/audio spot-check (output preservation).** Pixel-diff a captured frame
sequence vs the PC/SDL build; run the WAV/autocorrelation audio test. **Y-cull (11) and the
dropped invalidate (13) require an N64/ares pixel diff** — cache-coherency / wrap-math hazards
that silently corrupt on N64 only.

---

## Guardrails

- **Idle-skip:** never add a PC that is not empirically proven a pure spin. It only
  fast-forwards a dead timeslice to the next IRQ — safe for a true spin, desyncing for a loop
  that ticks a counter, consumes a polled value (e.g. raster line), or drives the synchronous
  Z80/YM2610 stepping. Histogram + per-PC verify + A/B before trusting any PC.
- **Reject the generic idle auto-detector** — too loose (misfires on memcpy / sprite scan /
  sound buffer fill). Ship the data-driven literal table only.
- **Any m64k_asm.S hot-path change is gated by TomHarte (ADDQ/SUBQ gap closed first).** RMW
  fast paths must match flags AND cycles bit-identically — a cycle mismatch drifts audio/raster
  sync even with correct flags.
- **REJECTED — do not pursue:** `flush/reload_sr` timeslice widening (m64k_run enters only
  ~2-4x/frame already; Z80 runs inside the exception handler), and ym2610 idle-channel gating
  (freezing slot phase counters corrupts key-on transients; already gated by `eg_out<ENV_QUIET`).
- **N64 cache coherency:** dropping `data_cache_hit_writeback_invalidate` (13) and the Y-cull
  wrap math (11) pass on PC but glitch on N64 — validate with an N64/ares pixel diff.
- **Scope:** draw~13%, dma~0% — overlap/coalescing/cull cannot move fps while cpu is 272% over
  budget. The fps headline is owned by Stream A (idle-skip + RMW).
- **Diagnostics cleanup:** resolve/remove `[CACHE]`/`[PROFILE]`/FPS `debugf`, the
  `mvsmakerom.c:543` idle_skip TODO, and the `roms.c:100` FIXME invalidate comment as each
  owning item lands.
