# PLAN-OPTIMIZATION.md — mvs64 / samsho2 N64 Framerate Plan

> Produced 2026-06-19 by an 8-expert parallel audit (analyze → adversarial-verify → synthesize).
> Goal: raise framerate toward 60fps **without frameskip** and **without deviating from the
> original arcade experience** (cycle timing, audio sync, input feel must stay intact).

## ⚠️⚠️⚠️ Empirical update (2026-07-06, branch perf-fps) — IDLE-SKIP NEVER FIRED; FM SYNTH IS THE WALL

1. **The idle-skip list was dead code from day one** (so the 2026-06-19 "idle-skip is flat
   in-match" finding below is wrong). `beq m_pc, 0xFF00142C`-style compares made GAS emit the
   constants ZERO-extended (li+dsll+ori = 0x00000000_FF00142C) while map_m68k produces
   SIGN-extended pointers (0xFFFFFFFF_FF00142C): the 64-bit beq could never match. Proven by
   ELF disassembly + a new `-DMVS64_PERFCOUNT` counter (skips=0 across a full 300s run).
   Fixed in fa10101 (explicit lui/ori). Result: in-fight skips=2/frame, executed 68k insns
   16.9k→7.8-10.8k/frame, m68k 93%→70%, **real-fight fps 17.5→21.4 median**; menus ~40, peaks 60.
   The samsho2 spin (tst.b $100A30/beq .-8 @0x142C) was 39% of ALL in-match instructions.

2. **Run-content divergence:** guest content is NOT reproducible across ares runs (the
   68k↔Z80 handshake is wall-speed-sensitive → menu/fight timing shifts). Compare only
   content-matched windows: PROFILE PC 003200/0031fe + SNDRMS rms>0 = a real fight.
   The 2026-07-02 "17.5fps in-match" numbers were real fights and remain the valid baseline.

3. **In-fight split after the fix:** cpu ~247%, **snd ~137% (54% of ALL wall time)**,
   m68k ~70% (10.7k insns, 1.2k TLB faults, ~106 host cyc/insn), draw ~33%, io ~8%.
   YMPROF: **FM 722ms / ADPCM 220 / SSG 70 / EG 11 / mix 18 per ~2.4s interval — FM synthesis
   is the single biggest cost in the whole emulator (~30% of wall)**. Since audio is
   real-time, frame_wall = non_snd/(1−r): every non-snd saving is amplified ~2.2x, and true
   60fps needs BOTH r cut ~2-3x (RSP offload) and non_snd ≤ ~8ms.

4. **draw 33% is pure CPU command issue** (PERFCOUNT dwait 0.2% / dissue 32.2% / dend 0.1%):
   sprite walk + sprite_cache hash + uncached rspq writes; no RSP/RDP back-pressure. Diffuse —
   the 2x lever there is moving the SCB walk to the RSP, not micro-trims.

5. Landed: chan_calc_stream — FM channel calc with LOCAL index-routed sums (the s32*
   connect stores defeated TBAA and forced per-sample state reloads). Two lessons in one:
   the first attempt (always_inline x8 algorithm specialization, 1f6dbff) was WAV-exact but
   **40% slower** on N64 — 8 inlined copies of the sample loop blew the 16KB icache (fmms
   722→1022; even SSG rose 68→90 from cross-pass eviction). The single-body rework (54f72bc,
   runtime algo + u8 routing tables + local s[5]) measures fmms 722→~685. Net fps effect
   small — FM cost is mostly intrinsic arithmetic + EG replay on a 93MHz in-order CPU.

6. **Session result (final-auto, ares 300s):** real fight 17.5 → **21.5 fps median**
   (clock ≈ 2.75s per game second, was ~3.4s); menus 40-42 median, peaks ~60. Next
   step-change levers, in order: (1) RSP YM2610 offload (snd is 54% of wall; the
   integerized, chunked, channel-major core is now shaped for it), (2) draw 33% — move the
   SCB walk/sprite issue to the RSP (it is pure CPU command issue, dwait~0), (3) m68k
   residual 70% (1.2k TLB faults/frame ≈ 13%, interpreter ~106 cyc/insn: dcache-dominated).

7. BizHawk check (2026-07-06, per protocol): the play build wedges BizHawk-Mupen at ~frame
   421 deterministically — the KNOWN libdragon-audio platform limitation (see memory
   bizhawk-mupen-verdict), reproduced identically by stock libdragon audio ROMs. Not a
   regression signal; ares remains the pre-hardware gate (full boot→menus→fight validated).

## ✅ SCOPE CLOSED (2026-07-07 overnight) — CPU-side plan complete

Every remaining ranked item is now landed, measured-neutral-but-kept, or closed with
measured reasoning (see BUILDS-2026-07-07.md for the full table): rank 5 (io reads are
~0.1ms — write-dominated bucket already asm-fast-pathed), rank 7 (deterministic eviction,
landed), rank 9 (snapshot gating provably breaks WAV; bus inline priced ~1.5-2%), rank 11
(Y-cull landed, pixel-identical, ~neutral in fights), rank 12/13 (no meat on hot paths),
rank 4 (~2% total in the dcache-bound regime — deferred to the RSP era). Codegen sweet
spot confirmed from both directions: -O3 +40% (2026-07-02), -Os +14% (2026-07-07).

**Final in-fight state: ~22 fps median (menus 39-42, peaks 60), split snd 135% /
m68k 69% / draw 33% / io 7%.** The remaining levers are STRUCTURAL, in order:
(1) RSP YM2610 offload — snd is ~53% of wall; (2) RSP draw-issue offload — draw is pure
CPU command issue; (3) rank-4 fast paths become worthwhile again only after (1)/(2).
Runs are now near-bit-reproducible (LCG eviction), which will make the RSP session's
A/Bs much sharper. BizHawk note: the known libdragon-audio wedge now lands at ~frame
490-540 (was 421) — same platform limitation, no new failure class.

## ⚠️⚠️ Empirical update (2026-07-02 overnight session) — THE WALL IS SOUND, NOT THE 68K

A new `[PROFILE] m68k%/snd%` split (af95d16) corrected this plan's central premise: the old
`cpu ~235-272%` bucket lumped the 68k core together with **real-time audio synthesis**. In a
real match (not attract/menus, which is what the old numbers measured): **m68k ≈ 94-120% of
frame budget; snd (Z80+YM2610 synthesis) ≈ 280-530% — audio is the dominant wall.** The AI
pump synthesizes wall-clock real-time 11025Hz audio regardless of game speed, so at low fps
audio eats 50-75% of ALL wall time. YMPROF section split in-match: FM 38%, EG 29%, ADPCM 23%,
SSG 8%, mix 2%.

Landed 2026-07-02 (all gated by TomHarte testsuite 125/126 and/or a **bit-identical WAV** on a
3600-frame deterministic scripted PC run):
- 81e7d26 m64k dispatch slim (main loop 10→6 insns; TLB handler owns ts_cur + forced slice
  exits with exact cycle accounting) — took in-match m68k to ~real-time budget.
- ea6a4c8 M64K_BLOCKOPS: fused DBF copy/fill loops incl. the `move.w (A0)+,(A4=VRAM port)`
  SCB upload (was one TLB exception per word). Attract 23.5→30+fps.
- d17aced ADDQ/SUBQ testsuite coverage (Musashi-oracle generator).
- 73204cf/4b8e417 ym2610 table shrink: 16-bit tables, then tl_tab collapsed to a 512-byte
  base + shift/negate (VR4300 dcache is 8KB direct-mapped; the 13-26KB table thrashed it).
- 110f2ce EG exact-skip (state-indexed rate-shift array; skip off-mask slots).
- 7f30c76 LFO-PM phase-delta cache; d1d3b93 inlined streamed-ADPCM window fetch.
- 4b29c4d **-O3 on audio TUs REGRESSED snd ~+40% (icache bloat) — keep -O2.**

Measured (ares, 300s AUTOINPUT protocol, in-match window = the real fight):
17eb0d3 baseline 8.45fps → 4b8e417 **~14.9fps in-match (snd 261%, m68k ~110%, draw 33%,
io 9.5%), menus ~27-30fps**. Guest frames/300s: 3784→4949.

**2026-07-02 (daytime): lever (2) LANDED.** c1cb136 rewrote YM2610Update_stream channel-major
(64-sample chunks, per-channel register-resident state, precomputed LFO/EG schedules) and
7af2216 added a silent-FM-channel fast path (keyed-off channels batch-advance phases only).
Both byte-identical-WAV gated. Measured (same 300s protocol, same analysis): **in-match
median 15.0 → 17.5 fps; snd 255% → 143%; m68k ~100%; guest frames 4949 → 5256.** Within the
remaining snd, synthesis still outweighs Z80 stepping ~3.5:1 (ymms≈600-1170 vs z80ms≈280-370
per SNDRMS interval) — the residue is ADPCM decode + genuinely active channels, i.e. work
only the RSP offload can remove.

Next levers, in expected-value order: (1) **RSP YM2610/ADPCM offload** — the only step-change
available for snd (weeks); (2) ~~batch-per-channel synthesis rewrite~~ DONE (above);
(3) io read fast-path (input ports/VRAM read, ~3-5%);
(4) m64k rank 4/14 (MOVE/Bcc dominate: 19.8%/14.1% of productive cycles); (5) draw (33%).
The 2026-06-19 roadmap below is retained for history but its cpu-centric ranking is obsolete.

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
