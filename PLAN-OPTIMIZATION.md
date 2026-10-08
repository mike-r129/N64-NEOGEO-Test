# PLAN-OPTIMIZATION.md — mvs64 / samsho2 N64 Framerate Plan

> **Status: this repo is no longer the live home of this work.** It is now
> maintained in the [mvs64 fork](https://github.com/mike-r129/mvs64). This log
> covers the initial bulk of the performance work, which targeted samsho2
> specifically; once samsho2 was stable, the changes were prepared for the
> fork. Entries below are the samsho2 record up to release27. The libdragon rspq
> fixes found during this work are carried in
> [mike-r129/libdragon](https://github.com/mike-r129/libdragon) (PRs #1-#3).

## 🧹 2026-10-04 (night) — CLEANUP (branch cleanup): dead code out, layout pinned

35 commits, ~16.7k lines removed (most of it dead experiments, the July
Musashi profile files and the never-built genhle). Method: every commit
rebuilt release + PERFOSD and compared section sizes and every symbol
address against the pre-cleanup build; most were byte-identical except
assert line numbers. Two Fable review passes over the risky commits found
no behaviour change.

| Group | What went | Gate |
| --- | --- | --- |
| 68k | M64K_W3_FULL (testsuite now tests the game's fast-path set), M64K_PREDECODE, BOSTAT_SEEN, COPYL_NODISPATCH, #if 0 | layout identical; testsuite 125/126 |
| Video C | RSP walk + batch glue, CPU RDP fallback, NORENDER, Phase-0 counters, PERFOSD K line | layout identical |
| Sound | sync FM-on-RSP path, YM2610Update/SOUND_TEST/SAVE_STATE, FASTBOOT, Z80WARM, sound_noop.c, twins Z80_RMAP_OFF/Z80STEP_CALL/WP_REVIVE_OFF/AUDIO_OS/Z80_OS | layout identical |
| Diagnostics | DPCOSD, RDPDBG; PERFOSD/FBCRC moved to platform_n64_osd.c, per-frame telemetry to emu_diag.c | release identical; DET FBCRC 2,940 f; TRCRC 3,295 f after boot |
| Pin | hw_n64.S .text held at 0x980 by .org (68k interpreter stays at 0x80000e00) | release identical; oversize = assembler error |
| Layout batch | draw-path twin knobs fixed (cdt, cdt_inline/unc, spr2w, spr64, flush, fuse, cullfast, fixfast), walk/batch ucode (IMEM 0xd74 -> 0x88c), wp_dirty, stage_bulk twin, ym_addr_a, evict_gen, game.ini parse; .bss -96 KB | DET FBCRC 6,430 f (6,218 f on the final tree); RSPWP_VERIFY 44,032 chunks 0 bad; review clean |

Layout-batch timing (ares PERFOSD+AUTOINPUT, 243 matched fight windows,
ms/frame; ares is deterministic, repeat runs are identical): R 2.05 ->
1.97, V 3.44 -> 3.33, S 4.42 -> 4.32, M 8.49 -> 8.53, whole frame flat.
The first cut was R +0.12: with one caller left GCC inlined the walk into
video_render, so the walk is now noinline (55779b7).

LAWS:
- **ares runs are deterministic** (same ROM, same AUTOINPUT: identical
  PERFOSD streams), so a single matched pair is a valid A/B in ares.
- **Watch inlining when a function loses callers.** Removing the twin
  paths left sprite_walk with one caller and GCC inlined it into
  video_render, which cost the per-tile loop ~0.2 ms; noinline fixed it.
- **The Q/E split moves with scheduling.** Q rose +0.15 while the walk part
  of R fell by more: compare R (or V) totals across code changes, not Q.
- **hw_n64.S is size-pinned.** Growing it is an assembler error by design;
  bump HW_N64_TEXT_SIZE only together with a PERFOSD re-measure.
- Left alone on purpose: the dynarec (frozen, off by default), ROTA_OFF
  (hardware verdict still pending), the 2-buffer display twin (baseline for
  the FBCRC_PIPE gate), the audio pump in platform_n64.c (hardware-validated,
  intertwined with plat_init), play_buffer (32 KB, unused by N64 whole-pump
  builds but still the WP_OFF/PC target), rsp_fm.S's parked cmd_fm_run entry
  and start stamp (shared with the whole-pump body).

## 📊 2026-10-04 (later) — ROUND 4 (branch perf-round4): SPRITE PATH + THE 68K LAYOUT CLIFF

Hardware heavy fights went from 36.7 fps (N 1018 tiles: M 11.0, S 6.1,
V 8.6, R 4.8, Q 1.7, C 1.1, E 1.0, P 17.4, W 1.0) to 41.9 fps (N 785:
M 9.8, S 5.7, V 5.4, R 3.1, Q 0.5, C 0.3, E 0.7, P 16.9, W 2.4); the user
reports the final build "felt higher fps at most times".

| Step (commit) | Evidence | Gate |
| --- | --- | --- |
| PERFOSD split of V and R: B/L, R/K, Q/E, C/N, G/H (a572246, 808668f, 93f8772) | diagnostic | - |
| 2-word sprite command, slot + walk word (7820a9e, twin MVS64_SPR2W_OFF) | ares E 0.56 -> 0.22 ms | DET FBCRC 14,320 f |
| Force-inline sprite_consume_one into the walk (8e949ce) | call overhead was 3.5% of in-fight samples; R 2.77 -> 1.98 | FBCRC 4,689 f |
| m64k context allocated in the core's .sdata (af33807) | removes a 3.2 ms M cliff (see law); pad sweep M within +0.5 | FBCRC 13,903 f, testsuite 125/126 |
| One uncached sd per sprite command (31967a1, twin MVS64_SPR64_OFF) | ares flat; hardware E/N 0.89 vs 0.95 us (rounding) - kept, neutral | FBCRC 13,895 f |
| 4096-slot C-ROM cache with >4MB RAM (5f6c36a) | ares C 0.42 -> 0.07, V -0.64 ms, +1.2 fps; hardware C 1.1 -> 0.3 | FBCRC 13,895 f |

LAWS:
- **The m64k ctx vs .sdata dispatch-table collision was a live 3 ms
  lottery** in every build since the tables moved to .sdata: m64k (fixed
  .bss) and optable/ea_table/rmw_table (.sdata, shifts with any .rodata
  edit) on the same dcache sets ran the 68k ~40% slower. The ctx is now
  .space'd in m64k_asm.S's .sdata block. Check layout sensitivity with
  -DMVS64_LAYOUT_PAD=<bytes> (shifts .rodata onward) before trusting a
  cross-binary M delta; residual sensitivity is ~0.5 ms (pad 3200).
- **ares underprices hardware costs in the sprite path:** E per tile ~0.4
  us in ares vs ~0.9 on hardware, C-ROM misses ~3x. Per-tile timers on
  the PERFOSD overlay are the instrument; fps twins in ares miss them.
- **Hardware heavy scenes now see the RDP:** P ~17 ms with W up to 2.4.
  G/H say ~80% of tiles already draw in COPY mode and flipped full tiles
  are only 1-8%, so pre-flipping is not the lever; the RDP time per tile
  (~25 us at P 17 / N 700) is far above fill cost - unexplained.
- Fix-layer/begin are small (L ~1.0, B ~0-0.4 ms); the walk itself (R-Q-E)
  is ~2 ms on hardware.

### Round 4, part 2 (after the push): traps + layout pinning
| Step (commit) | Evidence | Gate |
| --- | --- | --- |
| Sprite-command knobs passed to the walk as one packed local (1287a79) | E 0.30 -> 0.24 ms | FBCRC 14,083 f |
| Uniform trap sampler, every 1024th port trap (216bf4d) | the first-1024 ring had caught one burst (CLR.w SCB loop) | - |
| MOVE.l to 0x3C0000 inline (112f874, twin MVS64_PORTL_OFF) | port traps 268 -> 32/frame; twin M -0.15 ms, +0.4 fps | matched pair TRCRC 5,620 f + IO stream 21,732 reads; FBCRC 13,694 f |
| z80.o rodata anchored to 8KB, z80_hot at 0x1CF0 (fae50c0) | S 5.31 -> 4.41 vs the unlucky build; pads move M/S/R <= 0.01 ms | layout only |
| m64k .sdata block pinned at set 140 (0882a17) | same sets in PERFOSD and release builds; M/S/R = p24a | testsuite 125/126 |

Dropped: CLR.w (An) port check in rmw16_eadst (flat: only ~16 traps/frame).

LAWS:
- **Layout is now pinned for the two CPU cores:** z80_anchor.c (linked
  before z80.o) and the 8KB .balign at the top of m64k_asm.S's .sdata.
  Edits before them no longer move the Z80/68k hot data. If z80.c's jump
  tables change, re-derive MVS64_Z80_ANCHOR_PAD (see z80_anchor.c).
- **Cross-tree FBCRC/TRCRC pairs still fork at f=3153 after pure speed or
  layout changes** (fb7 vs fb8off: anchors only, no logic). Matched pairs
  (same tree, knob flipped) are the only valid gate.
- **68k opcode histogram (ophist3, in-fight):** 57% of executed insns take
  fast paths; the generic remainder is a flat tail (largest forms grp0.w
  #,Dn 1.45%, BTST #,(d16,An) 1.41%, JSR (xxx).L 1.26%, misc48 (d8,An,Xn)
  1.24%, CMP.w (d16,An),Dn 1.02%). Each further fast path is worth < 0.1
  ms and grows m64k text toward the wave-3 icache cliff - not pursued. The
  remaining big 68k lever is the dynarec.

## 📊 2026-10-04 — ROUND 3 (branch perf-round3): HARDWARE-GUIDED, TRIPLE BUFFERING

New hardware instrument MVS64_PERFOSD (1874703, 076ea2c): non-stalling
overlay (text texture blitted in-frame; SNDOSD/DPCOSD drain the RDP every
frame and read low). First hardware fight reading: F 37.4, M 7.5, S 6.1,
V 4.7, **W 7.5**, A 26.2, X 0.4, P 10.2, T 0.9 (ms/frame). Hardware fights
are **CPU-bound, not RDP-bound** (RDP busy 10 of 26 ms); W was vsync
quantization with 2 display buffers.

| Step (commit) | Evidence | Gate |
| --- | --- | --- |
| Triple buffering + explicit prev-frame RDP fence (2bc4363, twin MVS64_DISPLAY_BUFFERS=2) | ares twin +3.65 fps, 60/62, 1.29 ms; hardware expected ~W 7.5 -> ~1 | DET FBCRC_PIPE vs drained FBCRC: 12,127 frames pixel-identical |
| Z80 per-step working set in one contiguous block (cbd0d3d) | fixes a link-layout cliff: 5.49 -> 2.53 us/step (2.80 before) | PC WAV byte-identical |
| MOVE.w #imm,(d16,An) fast path (2011f85, twin MVS64_IMMD16_OFF) | +0.14 fps, 39/62 | DET TRCRC twin identical 11,964 f |
| ADPCM staging in runs (8487722, twin MVS64_STAGEBULK_OFF) | +0.15 fps, 31/55; genms -3.5% | STAGE_VERIFY 183k runs 0 bad |
| PERFCOUNT trap histogram + LSPC trap sampler (df3b548) | diagnostic | - |

INVARIANT CHANGED: the display is now 3 buffers; the pointer-lifetime fence
is the frames_done wait in plat_beginframe, NOT display_get. Anything that
reuses per-frame RDP/RSP inputs must happen after that wait.

LAWS:
- **dcache layout cliffs are real and silent**: cyc_00 landing on z80_rmap's
  sets doubled Z80 time (~5 fps) from an unrelated .data edit. Hot per-step
  tables belong in one contiguous block (z80_hot in .rodata.* next to the
  jump tables). Check with dcsets/z80layout scripts when Z80 us/step jumps.
- Measured flat and dropped: 64-bit rspq buffer clear (libdragon patch
  reverted, still 3 patches), direct PI DMA for tile misses (dmat 2.08 ->
  2.03%: the transfer is the cost), palette dirty chunks (5 palette writes
  per fight frame), 68k read fast paths (~20 trapped reads per frame).

## 📊 2026-10-03 (night) — 68K/Z80 SESSION: +2.55 fps in fights (paired), ~1 ms/frame

Branch `frameskip`. Same release config and method as the draw session below
(QUIET+AUTOINPUT, 420 s ares runs, paired 300-guest-frame fight windows, one
emulator at a time). Planned from a fresh in-fight PC profile (pcprof7) and
two Fable advisor reviews (68k, Z80).

| Step (commit) | Evidence | Gate |
| --- | --- | --- |
| Trap-path PROFILE_START/END only in PERFCOUNT builds (cb3dd0f) | with the next row: +1.03 fps paired vs morning HEAD | pure deletion (k0/k1 + diagnostic global) |
| jmp_exec idle-skip page prefilter (09f81dc, twin MVS64_IDLEFILTER_OFF) | twin +0.47 fps, 43/56 faster, 0.20 ms | DET TRCRC twin identical 9045 f |
| Z80 parity xor-fold, inline step, frameless exec_opcode leaf (76fac28, f413dea, a9d2fc5) | 2.96 -> 2.80 us/Z80 step; +0.99 fps cross-binary | PC WAV byte-identical (3600 f) |
| Inline LSPC VRAM-port stores in hot MOVE.w paths (40c5a1a, twin MVS64_PORTSTORE_OFF) | tlb traps 1147 -> 461 per fight frame; twin +0.70 fps, 48/61 faster, 0.30 ms | DET TRCRC twin identical 9045 f |

Cumulative vs the session's starting HEAD: **+2.55 fps median (+2.41 mean),
53/57 fight windows faster, 0.98 ms/frame**; fight median 48.4 -> 51.8.
TRCRC all-changes vs old HEAD: identical over 10,165 frames (past 377/3153).

Findings:
- **The PC sampler cannot see the asm trap path** (it never clears EXL; the
  timer sample lands after eret on the instruction after the faulting sh).
  ~6% of samples on movew_f_dst_* / movew_fsrc_* were really MMIO-trap cost.
- **TRCRC pairs must run on a quiet host.** A DET pair whose new-side run
  overlapped heavy WSL builds forked at f=3153 (and ran 20% fewer frames);
  the quiet rerun of the same code matched over 9-10k frames.
- **dma_read (2.7% of samples) split** via new PERFCOUNT [PERF3] vrom=/vromt=
  (85fae5b): C-ROM tile misses 9.6/frame = 2.00% of a 60 Hz frame (~0.33 ms,
  in video_render); ADPCM V-ROM window refills 0.48/frame = 1.25% (~0.21 ms).
- WSL `/tmp` does not survive between wsl.exe invocations — keep staging
  copies in the Windows scratchpad.

Gap to full speed now: ~19.5-20 ms vs 16.9 ms in a median fight frame.

### 🗂️ 68K/Z80 BACKLOG (from the advisors; estimates unverified)
1. **Z80 register-resident run loop** (cyc/R/PC/rmap in registers inside a
   core-side run_until; flush cyc before every callout - YM timers/busy read
   cpu.cyc). ~0.1 ms, medium.
2. **ADPCM window prefetch** (async PI DMA into a second per-voice buffer at
   the top of sound_gen_samples, fetch_slow waits on a matching tag). Bit-
   exact (ROM immutable). Ceiling ~0.2 ms (vromt 1.25%). Check the live
   decoder path under MVS64_RSPADPCM first.
3. **Z80 spin-snapshot via 4 ld + masks** (sound_neogeo.c z80_snap, N64 only,
   same field set). ~0.03-0.06 ms. And g_z80_steps local counter (SND_HEALTH
   builds only, ~0.03 ms).
4. **Remaining 68k traps (~460/frame):** histogram them by address class
   (PERFCOUNT counters at mvs64_asm_io_write + the C path) before extending
   port_store_check to more MOVE forms or the palette (0x400000+) writes.
5. **68k asm read fast paths** (0x320000 sound reply, input ports) ~0.1 ms.
6. **Dynarec coverage** stays a multi-session bet (~0.3-0.6 ms at 40%
   residency; work list = DYNTERM/DYNREF in dyn-stat2.txt). Keep OFF.
7. YM/Z80 rescheduling (journal YM writes, synthesize per segment): 0.2-0.3
   ms but must catch synthesis up at every status read; high complexity.

## 📊 2026-10-03 (later) — DRAW PATH: rspq buffers + three CPU cuts, +1.5..+2.9 fps in fights

All on branch `frameskip`, each a layout-identical twin knob (one .data byte),
QUIET+AUTOINPUT release config, 420 s ares runs, paired 300-guest-frame
fight windows, one emulator at a time. 0 crashes / 0 underruns / hpwedge=0
in every run.

| Step (commit) | Fight fps, paired median | Faster windows | ms/frame | Pixel gate (DET+FBCRC) |
| --- | --- | --- | --- | --- |
| 16 KB rspq lowpri buffers + flush/64 (e71fc9a patch, 332b57a, d65f9b4, default 81afc69) | +1.09 | 40/56 | 0.54 | 9,651 identical |
| Fix-layer fast path + walk early-out (3120b6f, 5189e1d) | +1.17 | 23/25 | 0.49 | 9,677 identical |
| Per-render CDT context + uncached slot ticks (7b6dca2) | +0.61 | 40/57 | 0.26 | 9,491 identical |

Cross-binary morning baseline -> now: fight median 46.0 -> 48.3 (paired
+1.45 median, +1.63 mean, 45/56 faster). Twin steps sum to ~+2.9 / 1.3 ms;
the truth for any one binary is in between (layout-luck law).

Laws/findings:
- **rspq only hands commands to the RSP on a flush.** Bigger buffers alone
  were SLOWER (SNDOSD config 45.6 -> 42.9 at 16 KB): with 2 KB buffers every
  buffer switch flushed, with 16 KB nothing did until render_end, so CPU
  issue and RSP/RDP execution serialized. Flush every 64 sprite commands.
- **SNDOSD/DPCOSD/FBCRC builds drain the whole queue every frame**
  (rdpq_detach_wait) — they cannot show queue-depth gains. Hardware-test
  config with the change: +0.18 (no regression). Measure perf on QUIET.
- **Pointer-lifetime audit (Fable advisor):** display_get (2 buffers) returns
  only after the previous frame's RSP+RDP work is done, which fences every
  cache-slot pointer. One real race: pal_convert read the LIVE PALETTE_RAM,
  which the 68k rewrites for the next frame — hidden only by the tiny
  buffers. Fixed with a per-frame snapshot (0341cc1). Invariant: exactly 2
  display buffers, no cache inserts outside video_render.
- libdragon now carries a 3rd vendored patch (lowpri size hook, BUILDING.md).

Gap to full speed after this: ~20.5-21.0 ms vs the 16.9 ms target in a
median fight frame. Focus moves to the 68k (~6.5 ms) and Z80 (~3.7 ms).

### 🗂️ DRAW-PATH BACKLOG (parked 2026-10-03, not started; estimates unverified)
Ranked by expected ms per unit of risk. Each should land as a layout-
identical twin knob and pass the DET+FBCRC pixel gate.
1. **2-word cmd_sprite_draw carrying the slot index (~0.3 ms CPU, medium
   risk, ucode change).** Today sprite_consume_one unpacks w0/w1 into 8
   args and rsp_sprite_draw repacks 3 words (~25 instr) + 3 uncached
   stores per record. Slots are sprites + e<<7 from a 16 B-aligned base,
   so send the slot (11 bits) instead of the address: word0 arg =
   ((w0>>20)&0x3FF) | (e<<10) (pal bits 0-7, flips 8-9 = the ucode's a2
   flip layout), word1 = w1 verbatim. Ucode (rsp_video.S): cmd size 8,
   pixel base added to cmd_sprite_begin, palette from andi a0,0xFF, x/y
   shift swap, sw/sh from srl a1,24, address = base + slot<<7, modal
   (COPY-mode) test rebuilt from sw|sh|flips. Saves ~15k instr + 622
   uncached stores + 2.5 KB of memset per frame. MVS64_SPRBATCH's
   batch_synth_args must be updated or declared broken. Twin: keep cmd
   0x2, add 0x7, CPU chooses by .data knob (OFF twin pays an RSP shim -
   judge by CPU time, not fps).
2. **Palette convert per-2 KB dirty chunk (0..0.5 ms, low risk, measure
   first).** render_begin snapshots/writes back/converts all 8 KB whenever
   any palette word changed. Track dirty per 0x400-entry chunk in
   video_palette_w (and the asm palette write path) and copy/convert only
   those. Only worth it if fights dirty the palette often: read
   perf_dr_begin in a PERFCOUNT build first.
3. **64-bit zeroing in rspq_switch_buffer (~0.1-0.2 ms, libdragon patch).**
   Every command byte is zeroed once through uncached memory (memset ~2%
   of fight time). If newlib memset uses 32-bit stores there, an sd loop
   halves the uncached transactions. Vendored-patch change, not knob-able.
4. **Hoist rspq_cur_pointer/sentinel into locals across the walk** (part of
   the CDT-context idea, not done): store back before rspq_flush /
   rspq_next_buffer / the miss path. Small; risky only if a path is missed.
5. **SROM direct table for drawn fix cells** (~0.05 ms, 16 KB table) -
   rejected for now (table pollution > gain).
Rejected by data: last-tnum memo, crom front cache, sprite batching,
RSP walk, triple display buffering (perf_draw_wait 0.2%), partial
high-water buffer zeroing, LRU tick inside the CDT entry, uncached crom_dt
reads, auto frameskip (bad visual trade).

## 📊 2026-10-03 — HW SOAK PASSED; AUTO FRAMESKIP MEASURED: BAD TRADE (knob stays OFF)

**Hardware:** the user ran the latest build all day (a full workday) on a
real N64: still responsive, sound working, no crash. First long session
since the highpri-wedge fix (f8fbf75). OSD K/R/W/D readings and the SD log
not yet collected.

**Auto frameskip (3473f18, 4b00f4d; `make ... FRAMESKIP=n`, default 0):**
skips DRAWING up to n frames in a row while behind the VI clock; the 68k,
Z80 and audio run every frame. Layout-identical twins (one .data byte),
QUIET+SNDOSD+AUTOINPUT, 420s each in ares, 62 paired 300-guest-frame
windows ([FSKIP] lines; fs-analyze.py in the session scratchpad):

| Build | Fight game speed (median / min) | Fight drawn fps (median / min) |
| --- | --- | --- |
| FRAMESKIP=0 | 45.8 / 36.6 | 45.8 / 36.6 |
| FRAMESKIP=1 | 49.2 / 41.1 | 35.5 / 24.7 |
| FRAMESKIP=2 | 53.4 / 42.5 | 28.8 / 17.0 |

0 crashes, 0 underruns, hpwedge=0 in all three. **Verdict: bad trade.** A
skipped draw saves only ~6.7 ms; the rest of a fight frame (68k + Z80 +
audio glue, ~15.1 ms) runs regardless. FRAMESKIP=1 buys +3.4 fps of game
speed for -10 fps on screen; =2 buys +7.6 for -17. Kept as an opt-in knob.

**What the fit says about the real budget (fight median):** frame ≈ 21.8 ms
= logic ≈ 15.1 ms + draw issue ≈ 6.7 ms; full speed (59.19 Hz) needs
≤ 16.9 ms, i.e. -4.9 ms (-23%). Logic alone already fits; the draw-issue
path is the single largest, most tractable block (sprite walk ~1.4 ms, RSP
wait ~1.2 ms, CROM lookups, rspq writes, fix layer). Next levers re-ranked
for that: (1) larger lowpri rspq buffers (+ pointer-lifetime guard);
(2) cheaper per-tile command issue / CDT hit path made layout-robust;
(3) ADPCM async prefetch (~0.5 ms); (4) 68k dynarec coverage (~6.5 ms of
68k, long road). Real-hardware fps still unmeasured.

## 🔴 2026-09-23 — HARDWARE RSP CRASH AFTER 30+ MIN = rspq HIGHPRI WEDGE (libdragon race), FIXED; CDT +1.2ms/frame

**HEADLINE (ares, shipped config QUIET+SNDOSD, same AUTOINPUT script, 35
content windows): sndfix4 36.5 fps → sndfix6 45.9 fps median, +9.8 median per
window (+9.3 in fights; boot 12 → 43).** sndms/pump 8.67 → 4.82. The Z80
self-jump fast-forward (4f87318) is NOT boot-only: the driver waits in-game
in DI+`JP $` spins, and the new build executes 10-30% fewer Z80 steps for the
same content (bit-exact skip). Stack: that + read page map + CDT + fused walk
+ the SND_HEALTH rms-probe fix (OSD/SNDHEALTH builds only, ~2-3 fps of it) —
release builds gain somewhat less.

**Hardware report:** first long session on the sndfix4 line — sound healthy past
the 17.9-min wrap (cd0b72f validated on real HW) — ended after 30+ min in
`RSP CRASH | rsp_queue | rspq_next_buffer (rspq.c:951)`, "wait loop timed out
(200 ms)", `SP_STATUS=0x1403` = HALTED|BROKE|SIG3 HIGHPRI_RUNNING|SIG5
BUFDONE_HIGH, kernel PC 0x018 (just past the idle `break` in
RSPQCmd_WaitNewInput), current DRAM 0x18d994 == saved highpri pointer,
overlay rsp_fm. NOT the known lost-wakeup: SIG_MORE clear, so neither the
closed-loop flush nor the pump watchdog applies.

**Mechanism:** upstream `rspq_highpri_begin` appended its
WRITE_STATUS(CLEAR_REQUESTED|SET_RUNNING) *before* storing
SET_HIGHPRI_REQUESTED. The audio offload issues highpri segments back to back
(~500/s); an RSP still running the previous segment follows the epilog-skip
JUMP and can execute the new WRITE_STATUS inside that few-store window, so
REQUESTED lands afterwards and stays stale. At the SWAP_BUFFERS epilog the
kernel drops to lowpri, sees REQUESTED, re-enters highpri at the EMPTY end of
the stream and sleeps there with RUNNING set — CLEAR_HALT-immune (every lowpri
flush refetches the same 0x00 terminator). Lowpri starves until the next
highpri segment; a frame's video commands fill both 2KB lowpri buffers first
and the CPU dies in rspq_next_buffer. Every field of the HW dump matches.

**Fix** (f8fbf75, `patches/libdragon-rspq-highpri-wedge.patch`, vendored into
/root/libdragon and installed; applies after the closed-loop-flush patch):
1. Root cause: raise REQUESTED *before* appending the WRITE_STATUS (uncached
   stores reach the RCP in order → the WRITE_STATUS always consumes it).
2. Safety net: `__rspq_wedge_check` on every RSP_WAIT_LOOP iteration (weak hook
   in rsp.c `__rsp_check_assert`): wedge signature held 2ms with the CPU in
   lowpri → queue an empty highpri segment; the RSP runs it back to lowpri.
   Telemetry 88c4974: `[AIPUMP] hpwedge=` / SNDOSD `W` (healthy: 0).

**Gates (ares, one emulator at a time):**
- Fault injection (d323d88, MVS64_RSPQ_WEDGETEST: stale REQUESTED by hand).
  Unpatched toolchain: the EXACT hardware crash screen (rspq.c:951, "wait loop
  timed out", PC:018, STATUS:1403 [halt broke sig3 sig5], current == highpri
  pointer) at the first injection that stuck (1 of 5 — injected while the
  pump's segments are still queued, their own WRITE_STATUS eats it). Patched:
  17 injections, 7 stuck → 7 recoveries (hpwedge=7), 0 crashes, 11,758 frames.
  Drain-first rig (every injection sticks): 43 injections → 43 recoveries
  (hpwedge=43), 0 crashes, 0 audio deaths, 13,660 frames.
- Race-window widening (throwaway libdragon variants, a ~1500-iteration plain
  loop inside highpri_begin): UPSTREAM order 367 wedges in 23,101 pump
  passes, every one recovered by the watchdog (0 crashes, 0 audio deaths);
  FIXED order with the same window: 0 in 19,981. (A first attempt polled
  COUNT for the delay; ares crawled to ~1000 frames: 1 vs 0.) The race is
  real, the reorder closes it, and the net holds under a wedge storm.
- Deliverable soak (sndfix5 code + AUTOINPUT, 600s): 15,121 pump passes, 0 crashes,
  0 audio deaths, hpwedge=0, sound in 131/142 [SNDRMS] windows; the only underrun
  is pass 1 (boot, before the first fill).

**Deliverables** (patched toolchain; handed to the user for the next HW soak):
sndfix5/release5 at 1daf5c6, then **sndfix6/release6 at 33dc60c** (+ Z80 read
page map; soak 26,281 passes clean — one 40ms pad at a KO sound burst, the
known load-hitch class; shipped-config fps vs sndfix5 +0.6 median over 26
content windows).
sndfixN = QUIET+SNDOSD (as sndfix4), releaseN = QUIET. Build 5 over sndfix4:
the highpri-wedge fix + telemetry, CDT, fused walk, Z80 self-jump
fast-forward, SND_HEALTH rms-probe fix; build 6 adds the Z80 read page map.

### Perf work this session (tracks resumed after the 2026-09-22 crash)
The previous session ran four forked tracks in parallel that together launched
~12 ares instances and froze the host. NEW STANDING RULE: one emulator at a
time — `ps-ares-run.ps1` now refuses to start (mutex + process checks) if ares
or a WSL PC emu run is active; `ps-ares-queue.ps1` runs jobs strictly in
sequence; WSL scripts source `emu-guard.sh`.

- **CDT — CROM direct table (1daf5c6): cpu −7.3, draw −7.2 points/frame**
  (DET twins frame-paired, 692 in-fight frames, content-matched; ~1.2ms/frame,
  ≈+1.5fps at 35fps). One u16 per tile fuses the empty-tile fact and the
  resident slot; per-slot LRU ticks. Pixel: CDT off vs on identical 2354
  frames; fuse-OFF baseline vs CDT on identical 2521 frames.
- **Fused sprite walk (9952b78):** −0.5 cpu points; pixel-identical (N64 2354
  frames, PC 101 shots).
- **Z80 self-jump fast-forward (4f87318):** JP $/JR $ spins (the boot DI park
  = 48% of all Z80 steps) fast-forwarded bit-exactly; WAV IDENTICAL. Also
  cuts 10-30% of in-game Z80 steps (the driver's DI+JP $ waits).
- **SND_HEALTH rms probe (2571377):** the per-call linear-search isqrt cost
  ~2.8% of frame time in every SND_HEALTH build (SNDOSD HW builds + all
  measurement twins). Now computed only on the reporting call, exact isqrt.
  NOTE: genms in [SNDRMS] is not comparable across this commit.
- **FIXBLK (fix-layer rspq block replay): NOT landed, parked on the draw
  branch (0e7768b).** The fix layer genuinely changes on 5-75% of frames
  (re-record counts identical with the hidden rows excluded), and a re-record
  costs more than the plain path: refined version cpu −1.5 median / −0.2 mean
  over 5,445 paired frames, pixel-identical 8,710 frames. Only per-column
  blocks could pay; the whole fix layer is ~4% of the frame.
- **Z80 read page map (33dc60c): LANDED.** Inline per-256-byte-page host
  pointer table instead of the read_byte callback. Bit-exact (PC WAV
  identical both inputs; every [SNDRMS] window has identical Z80 step counts
  on N64). Z80 stepping time −30.2% in the B5-worktree twins but only −6.9% in
  the integrated twins; shipped-config soak fps +0.6.
- **LAYOUT-LUCK LAW, re-measured:** the integrated rmap twins moved CDT's
  hit-path cost 932 → 293µs/frame with no CDT change — rmap's 1KB .bss table
  shifts the heap start, re-aliasing the heap-allocated CDT tables in the 8KB
  dcache. Cross-binary comparisons must read guest-tick channels (z80ms,
  bit-exact step counts) or use layout-identical twins, and every shipped
  binary carries its own luck. Next lever candidate: make the CDT hot path
  layout-robust (hoist table pointers out of the per-record call, or keep the
  LRU tick on the crom_dt line).
- **Dynarec re-test (cc6d19d sampler fix + d7b7723 MMIO store-source fix +
  39783a2 knob pin; all DYNREC/DYNSTAT-only, merged, default stays OFF).**
  The sampler fix works (the VRAM upload loops 00ceec/00cf2e now translate),
  but residency stays ~10%: 2,990 refused heads over 352 distinct opcodes, the
  top ten only ~half the refused heat. Layout-identical twins, 3,149 paired
  in-fight frames: cpu −0.6 median / −1.6 mean, m68k −0.7 / −1.7; content
  identical (0 record mismatches, 0 frame-end PC mismatches over 4,145
  frames); testsuite 123 differential sequences 0 fails, vectors 125 PASS +
  CHK (pre-existing). The payer is template coverage, a multi-session job.
- **TWIN-KNOB LAW:** a runtime twin knob with a plain `= 0` initializer lands
  in .sbss in the OFF twin and shifts every gp-relative small-data global by
  8 bytes: the dynrec "identical" twins differed in 1,962 loadable bytes, so
  every past dynrec on-vs-disabled verdict carried a layout shift (also the
  walk and batch knobs, pinned in 2f43b5a). Pin knobs to .data/.sdata and
  check twins with a loadable-section compare (objcopy .text/.data/.rodata/
  .sdata), not `cmp` on the ELF (DWARF records the -D flags).
- **Walk occupancy (aaf19d6 counters, 9,455 in-fight frames):** 81 sprites
  reach the tile loop, 1,042 iterations/frame, 622 records → 437 culled
  iterations. The real (uninstrumented) walk is ~0.7-0.9ms/frame spread over
  per-sprite and per-tile work: no cheap big win.
- **SNDOSD overlay cost: below layout noise** (QUIET+AUTOINPUT without OSD
  vs sndfix6auto: −1.1 fps median over 41 windows, range −3.1..+1.4). OSD test
  builds are representative of release speed.
- **Integrated profile (QUIET, pcprof2, sndfix6-equivalent, in-fight):** Z80
  ~17% (exec_opcode 8.4, z80_step 2.8, ddfd 1.7, sound_gen_samples 3.5), YM glue
  ~12.5% (YM2610Update_stream 7.3, rspwp_collect 2.9, rspa_stage 2.3),
  sprite_walk_produce 6.6%, **rspq_next_buffer 3.7% + wait-loop checks 2.1% ≈
  5.7% of in-fight time the CPU now WAITS on the RSP** (tail frames: rspq issue
  407µs median, 1.85ms p90 — the RSP still chewing the previous pump's audio
  burst), dma_read 2.5%, memset 2.1%, 68k interpreter ~30%.
- **NEXT LEVERS (ranked):** (1) larger lowpri rspq buffers so the CPU can run
  ahead of the RSP's audio burst (CPU-side-only libdragon constant; MUST first
  add a pointer-lifetime guard — the RSP DMAs sprite tiles straight from
  sprite-cache slots, PLAN-DRAW-RDP law 5); (2) ADPCM window async prefetch
  (dma_read 2.5%; all async DMAs completed before the pump returns, since
  io_read does not wait for DMA and logging/SD share the PI bus); (3) dynarec
  template coverage; (4) CDT hit path made layout-robust.
- **Profile (MVS64_PCPROF host-PC sampler, in-fight, pre-CDT):** draw
  video_render 7.8% (walk loop ~4.5%, fix scan ~2%), CROM lookup 8.1% (CDT
  target), Z80 ~15% (z80_read 3.4% = rmap target), YM2610Update_stream 6.6%,
  sound_gen_samples 6.4% (2.8% was the rms probe), dma_read 2.2% (synchronous
  cart DMA), memset 2.0% (most likely rspq_switch_buffer zeroing each new 2KB
  command buffer), m68k interpreter ~25-30%.

## 🔴 2026-08-30 evening — THE 17.9-MINUTE SOUND DEATH, SOLVED (cd0b72f)

User's "sound eventually fails forever" (long HW sessions, all-green
SNDOSD) was **cpu.cyc wrapping 2^32** (32-bit on N64 = 17.9 min of audio
time): sound_gen_samples' loop bounds were magnitude compares, so at the
wrap the step loop never runs again — and stepping is the only thing that
advances cyc. Z80 frozen forever, NMIs unserviced (silent coin test),
every downstream layer healthy. Proven: HW SD log AND the 90-min ares
soak deaths both integrate to exactly 2^32 generated cycles; PC (64-bit
long) sails past — WAV gates are STRUCTURALLY BLIND to N64-width wrap
bugs. Fix: signed-distance loop bounds (5 sites). Gate: MVS64_CYCWRAP_TEST
parks cyc 120s pre-wrap; control freezes on cue (steps=0 forever), fixed
build plays through. Also: SNDOSD now implies SND_HEALTH everywhere
(sndfix3's SD log was missing [SNDRMS]) and [SNDRMS] gained sp=/hi=
forensics. Earlier same day: the revive-budget permanence fix (3570612)
— real but a different, rarer class. Deliverables: sndfix4/release4.
LAW: audio-time u32 counters never gate loops by magnitude; wrap-class
bugs need the CYCWRAP rig, not PC gates.

## 📊 SESSION 2026-08-30 — TRACK C COMPLETE; walk-dyn-gate + B4 killed by data

Context: user reports the sndfix build has had NO sound loss on real
hardware (extended play) — revive validated; lows ~30fps are the pain.
Re-baseline (PERFCOUNT+DET+AUTOINPUT, 480s): modal 35.0, heavy 33.6/32.3;
snd 62-66% is the largest share in every bucket, m68k 58-65, draw 33->91
scaling with density. New [YMPROF] split (in-fight): kick/pack ~60,
SSG ~59, mix+collect ~61, eg ~12, adpcm ~3 ms/s.

Landed (one gated commit each):
- **C3 banking-copy elision** (9981b76): WP chunks accumulate SSG/ADPCM/
  deltaT partials straight into the pend slot (ax_l/ax_r target pointers);
  pass-4 copy gone. Gates: WAV IDENTICAL; RSPWP_VERIFY 61,440 chunks 0 bad.
- **C4 emit-copy elision** (4c8607d): non-WP chunks pack directly into the
  staging span; emit()'s play_buffer->stage copy deleted under RSPWP.
  Gates: WAV IDENTICAL; RSPWP_VERIFY 59,392 chunks 0 bad; WP_DEATHTEST run
  (no revive compiled) played the whole post-death half through the new
  fallback pack path — rms healthy, 0 underruns.
- **MVS64_WP_REVIVE default-ON** (5dde585): hardware-validated by the user;
  WP_REVIVE_OFF=1 to disable.
- **C5 SSG chunk batching** (03bd2d7): SSG_CALC_N, state hoisted to locals,
  bit-exact (s8 count_env wrap + unsigned mix promotion preserved).
  Gates: WAV IDENTICAL. Guest-tick verdict vs pre-C3 (same instr.):
  ssgms 58.6->43.5 (-26%), mixms 59.3->49.1 (-17%), z80ms 317->300,
  genms -23ms/s ≈ -0.8ms/frame for C3+C4+C5. Cross-binary fps inside
  layout noise at this size — guest-tick counters are the verdict channel.

Killed / repriced by data (do not retread):
- **B4 single-site computed-goto**: exec_opcode ALREADY compiles to 2
  indirect-jr sites (no per-case duplication) — no fat to trim.
- **Walk dynamic heavy-scene gating** (the track-D close-out idea): the
  archived SAME-BINARY twin A/B (ares-walkON/OFF-480) reads heavy buckets
  at only +0.1..+0.2 (the +0.7..1.0 was the older protocol), modal -1.3.
  A density gate would buy ~nothing. Idea dead unless walk economics
  change again (e.g. batch-consume world).
- **B2/B3 (Z80 loop fusion / hot-cold)**: B1's measured lesson stands —
  call overhead isn't the cost; per-step loop overhead is ~10% best case
  and the snap logic is semantic. Low EV; not worth gated increments.
- **B5 uop cache honest reprice**: phase-1 (2B records, prefix-collapse
  only, operands still via callback) saves ~10-20 of ~300 host cyc/step
  → +0.3..+0.5 fps, NOT +1..+1.5; the bigger cut needs operand-direct
  handler variants (icache-risk surgery). Still the only structural Z80
  lever left. Next session's candidate — design against the inline-bus
  postmortem (sound_neogeo.c:256).

State after this session: snd's YM half is trimmed ~9%; Z80 (~300ms/s
guest) is now clearly the dominant snd cost; m68k 58-65% remains the
biggest overall bucket with only the dynarec ladder open against it;
dense-scene draw remains hardware-gated (Phase 3 DPC verdict STILL
PENDING the user's flashcart A/B: dpcosd vs dpcosd+ROTA_OFF builds).

## 🔥 THE WALL-CHANNEL LAW — frame-377 fully mechanized (2026-08-08, 3ff9dc7)

The last unexplained divergence class is closed. A clean dynarec-vs-
interpreter TRCRC pair (coverage templates on) forked at f=377 — but:
[IO] read streams (raster/Z80 banks) IDENTICAL for ~2900 frames; each
binary SELF-deterministic; and a -DMVS64_DET_AUDIO pair is TRCRC
IDENTICAL over 3851 frames. Mechanism: the non-DET plat_audio_pump's
wall-clock-driven fill turns cross-build SPEED differences into guest-
content differences at marginal frames (the lsr-unlocked BIOS blocks
made the eye-catcher faster; content forked where the pump behavior
crossed a threshold). This RETROACTIVELY explains: the original
frame-377 "fallthrough attractor" (chaining changed speed, not
correctness) and most likely the blockops-ON/OFF f=3153 fork (the
"IRQ-coarsening" hypothesis is now second-choice; re-verify with a DET
pair when convenient).

STANDING LAWS:
- TRCRC content gates comparing builds of DIFFERENT SPEED must pin the
  pump: build both sides with -DMVS64_DET_AUDIO. Same-speed pairs
  (initializer-flip twins that measure fps-flat) may skip it.
- fps twins whose knob changes speed also need DET pinning to stay
  content-matched (the covPON run reached only 2784/8116 fight frames
  vs its twin; the DET rebuild gave EXACTLY 2784 fight frames on both
  sides — perfect bucket matching).
- Diagnostic: M64K_TRCRC_SPLIT adds a content-only hash ([TRCCON],
  regs/SR without pc/cycles) to separate timing displacement from state
  corruption; MVS64_IOLOG_N64=<frame> diffs the IO-read streams.

## COVERAGE RUNG LANDED (2026-08-08, 3ff9dc7) — [DYNTERM]-driven

New instrument: [DYNTERM] (DYNSTAT builds) logs head/stop-pc/opcode for
every block refused for lack of an ender — the exact template work list,
heat-weighted via [DYNH]. Top 4 forms = 250k heat/window, all landed:
add.b (An)+,Dn (131k, BIOS checksum), cmp.w (d16,An),Dn (64k, in-game
list search — regcache-hot A4 traffic), lsr.w #imm,Dn (8+2c — the
shift-imm charge's -2 arm is LONG-only, rig-measured) and subi.w #imm,Dn
(46k raster polls). Deferred: the 00b7cc chain (needs ROXL + pre-dec +
ANDI, 12k heat). Rig 116/0 with carry-sensitive canaries; DET TRCRC
identical 3851f; residency execpf 390→643 (+65%).

**fps: +0.5 in the sole measurable bucket** (DET twins, 39.8→40.3) —
the new residency is BIOS-era-heavy. The DYNTERM loop (measure →
template in-fight heads → repeat) is now cheap; but with in-fight
residency still ~6-8%, each rung buys little until the ladder reaches
in-fight majority share. Judgment call pending: keep climbing vs pivot
to PLAN-DRAW-RDP (draw = 27-66% in-fight).

## REGISTER-CACHE RUNG 1 LANDED — CORRECT, GATED, fps-FLAT (2026-08-08, 537a98c)

Write-through An caching in t7/t8/t9 across emitted blocks (dynrec.c).
Design: t7/t8/t9 are per-insn interpreter scratch AND explicitly
saved/restored by the hw_n64.S TLB/MMIO trap path, so cached values
survive trapping accesses; write-through means ctx is architecturally
current at every exit, so bail stubs/break checks/chain exits need ZERO
flush code — the whole dirty-state correctness surface of classic
dynarec regcaches doesn't exist here. Fills are body insns: every
(re-)entry refills, no state crosses block entries. Knob M64K_DYN_RC_DISABLE
(initializer flip inside the 8KB dyn_s block — layout law); [DYNRC]
fire evidence under DYNSTAT.

Gates: rig 101/0 (5 new RC sequences + canary: dropping the write-through
store fails exactly the 6 post-inc sequences); vectors 126 PASS (CHK
pre-existing); clean matched pair TRCRC IDENTICAL 9438f, non-vacuous
(11 blocks with hits, 32 An-loads eliminated at translate time).

**fps: FLAT bucket-matched** (45.8→45.6 / 48.0→48.0 / 36.1→35.8 / 34.9→34.5;
m68k share unchanged). WHY, quantitatively: blocks average ~2.6 guest
insns (278 insns/109 blocks in the 240s DYNSTAT window) and only 32
static An-loads were eliminated — there is nothing for a per-block cache
to pay on until blocks get LONGER. This is the residency-share law from
a second angle: rung 1 is INFRASTRUCTURE; the payer is template coverage
(longer blocks → more intra-block reuse → the same cache starts paying
without further work). Default ON (free, correct, gated).

NEXT RUNGS (the register-caching ladder, in order):
1. **Template coverage** — the top refused heads ([DYNREF]/DYNSTAT) are
   the direct lever: every newly-templated form both raises residency
   AND lengthens blocks (compounding with the cache already landed).
2. **Dn caching** — worth it only once blocks are long enough that the
   slice-canonicalization cost amortizes; re-evaluate after coverage.
3. **Cross-block persistence** (chain-web calling convention) — the big
   structural step; only worth designing at majority residency.

## 🔥 THE ATTRACTOR SAGA, ACTUALLY RESOLVED (2026-08-08, copy_l session)

Chasing the (An)+→(Am)+ memcpy fusion's gate exposed the REAL causes of
every content fork this campaign. Two concrete bugs, one design finding:

### 1. BOSTAT/DYNSTAT As-corruption (FIXED)
blockop_movew_port's instrumented commit bumped blockop_fire in t8 and
"restored" it with `li t8, 22` — the BUDGET constant — where t8 held the
As register value. Every fired port copy in an instrumented build wrote
As := 22+2K: guest corruption at the FIRST fire (f≈385, BIOS era).
Bisect-proven (cplY no-BOSTAT matches; cplZ +BOSTAT forks at exactly
385). Fix: reload As via `lw t8, 0(t9)`. This is why instrumented runs
"parked in attract" (forked BIOS content ate the scripted coins), and it
poisons every historical TRCRC comparison involving an instrumented
recording. Shipping builds were never affected.

### 2. Blockops-ON vs blockops-OFF guest content forks at f=3153
Byte-identical-binary chain: my tree with BLOCKOPS_OFF ≡ old tree with
BLOCKOPS_OFF (cmp: identical), and that binary forks from blockops-ON
recordings at exactly 3153 while all clean ON-family recordings
(trcseed5-b, detA/B/C, iodP) match each other 6270-7918f. So the fused
port copy is NOT bit-exact against the unfused interpreter at 3153-class
events. Mechanism (design-level): the port copy's fused span completes
ALL K MMIO writes and only then re-enters the interpreter; a mid-slice
IRQ (raised by those very VRAM-port writes through the trap path, or by
a guest-clock event landing inside the span) is granted at the END of
the fused block instead of at the interpreter's per-instruction
boundary — a bounded IRQ-latency coarsening that flips content at
marginal frames. RAM-only fusions (fill_w/fill_l, copy_l) have no
mid-span IRQ source and are immune. KNOWN-ISSUE, not fixed tonight:
the port fusion's +0.9..+3.2 fps stands, the coarsening is within the
slice-quantization class the emulator already has, but TRCRC gates must
be like-vs-like (both sides same blockop config).

### FUSION VERDICTS (bucket-matched PERFCOUNT twins, 480s)
- **fillw_port (move.w Dn,(An) VRAM-port fill): FLAT-to-marginal.**
  40.7→40.7 dominant bucket, +0.6..+1.1 light/heavy, m68k −1..−2 pts —
  despite ~3.7-4k fires/window (fire-delta-proven). The fill runs are
  short; per-entry savings are small. Kept ON (free, correct, gated).
- **copy_l ((As)+,(Ad)+ memcpy): IDLE in samsho2** — every such DBF loop
  targets PALETTE RAM (0x401558/0x401eb8, trapped MMIO with conversion
  side effects, ~2 entries/frame + fight-load bursts). A palette-dst
  helper would mostly speed LOADS, not in-fight fps: not worth it now.
- CONCLUSION: the cheap interpreter-fusion class is EXHAUSTED. The port
  copy's +0.9..+3.2 (2026-08-07) was its one big win. Remaining levers
  for significant fps: dynarec register caching + chain webs (the only
  +6-10 projection), the PLAN-DRAW-RDP dense-scene redesign, overclock.

### 3. Gate discipline hardened
- VACUOUS-GATE LAW: a TRCRC-identical pair proves nothing unless the new
  path FIRED — require a per-window fire-counter delta (ON >> OFF).
  (The cpl3 "green" gate was vacuous: copy_l span-rejected 1584/window —
  source spans live in the PB-ROM bank window 0x200000+, now admitted.)
- Same-config-family comparisons only: instrumented↔instrumented,
  blockops-ON↔ON. Cross-family forks at 385 (instrumented, bug #1) and
  3153 (ON/OFF, finding #2) are now explained and expected.
- The DET_AUDIO "causal confirmation" of the audio wall channel was
  CONFOUNDED: all det-trio builds were clean-ON family, so they matched
  regardless of DET. Clean non-DET ≡ clean DET over 6270f (trcseed5-b vs
  detB): the audio channel has NO observed content effect. DET stays as
  a theoretical pin; the plat_audio_pump analysis stands as code-reading.

## 🎯 DIVERGENCE SITE FOUND + RUNG-2 FPS VERDICT (2026-08-08 早)

### THE WALL→GUEST CHANNEL: plat_audio_pump's lead-driven fill loop
The wall-profile-sensitive channel behind the 377/3153 attractor forks (and
July's "run-content divergence" note) is in `platform_n64.c`:
- The fill loop stops on `lead = aring_wr - aring_rd` vs TARGET_LEAD, but
  **aring_rd advances in the AI interrupt at real VR4300 rate**. How many
  `sound_gen_samples()` passes run per guest frame therefore depends on
  wall-clock load (logging overhead, dynarec speed, ares scheduling).
- Each pass steps the Z80/YM2610; the 68k **observes** Z80 phase through
  its 0x320000 sound-reply polls → a marginal handshake read lands on a
  different reply → guest content forks. Frames 377/3153 are moments where
  the handshake is content-marginal.
- Second wall coupling at the same site: the underrun governor
  (`sound_silent` from aring_pad / underrun_streak).
CONFIRMATION KNOB: `-DMVS64_DET_AUDIO` — fixed guest-frame sample quantum
(`audio_get_frequency()/60` with carry accumulator), pass budget effectively
unbounded, `sound_silent`/`underrun_streak` pinned, aring_push overflow
guards at both push sites (host-side drop only — guest state untouched).
Causal test = remove-the-channel: previously-forking profile deltas
(IOLOG-vs-not; dynarec-vs-interpreter) must match past their attractors
under DET. **VERDICT: CONFIRMED.** detA(IOLOG)-vs-detB: TRCRC IDENTICAL
over 4981 frames (past 3153). detC(dynarec)-vs-detB: TRCRC IDENTICAL over
the full 6270-frame overlap (past 377 AND 3153) — a *cross-profile*
interpreter-vs-dynarec pair matching perfectly once the audio channel is
pinned. Every attractor-frame "divergence" chased this campaign is
explained; cross-profile TRCRC gating is now VALID when both builds carry
-DMVS64_DET_AUDIO. (The fallthrough-chaining quarantine retest, task #2,
should use DET builds.)
NOTE: DET_AUDIO is a **gate instrument**, not a shipping mode — real
hardware wants the lead-driven loop for latency; determinism only matters
for cross-profile TRCRC comparison.

### RUNG-2 FPS VERDICT: FLAT (bucket-matched, solo twins r2ON/r2OFF)
36.1→36.1 / 37.2→38.0 / 35.6→36.0 / 30.0→30.1 / 29.0→29.5 across spr
buckets; m68k share −2..−3 pts. Consistent with rung-1: **~10-13%
residency is real but too small to move fps** — the residency-share law
holds (need several ×10% more, i.e. register caching + chain webs, for
the projected +6-10). Rung-2 stays merged but dynarec remains DEFAULT-OFF.

### BLOCKOP FIX CONFIRMED (bucket-matched PERFCOUNT twins pcbON/pcbOFF)
+0.9 fps dominant bucket (2000-3000 spr), **+3.2** in 1000-2000, +1.4-1.8
in heavy buckets (3000-6500); m68k share −4..−7 pts. The fused-DBF class
is validated as a real lever → next cheap fusion: (An)+→(Am)+ long memcpy
(`0x20D8` at 0x31B8).

## ⚖️ TRCRC MATCHED-PAIR LAW + RUNG-2 GATED (2026-08-07 late night, 6a4e2be)

**The frame-377/3153 'divergences' were largely a broken gate, not broken
code.** Nine recordings: three same-tree builds fork from the stored trcref
at EXACTLY f=3153 — one of them a PURE INTERPRETER build — while three
others match it through 5576-7066; two IOLOG builds fork together and match
each other; the subtractive bisect halves forked at 377/3153 non-
monotonically. Conclusion: some wall-profile-sensitive channel (not yet
identified: frameskip is compiled out, RTC/watchdog are guest-clocked, perf
TICKS are diagnostic-only) flips a marginal guest decision at content-
sensitive frames; 377 and 3153 are attractors. LAWS:
- **TRCRC gate v2: matched pairs ONLY** — same tree, back-to-back solo
  runs, one knob flipped. Never a stored reference from another tree/day/
  load profile. (The historical DYN_CHAIN_FALLTHROUGH quarantine — also
  f=377 — is now suspect as this artifact: re-test with a matched pair.)
- **Subtractive template bisects are population-poisoned**: disabling any
  template reshapes the seeding cascade + tried-filter shadowing, moving
  divergence frames arbitrarily. Use the [IO] stream differ instead
  (MVS64_IOLOG_N64=<frame>: guest-frame-gated (addr,value) log of 0x3C/
  0x32 reads; iodiff.sh compares value sequences).
- **Mid-insn charge placement is architectural** (root cause #1, fixed):
  the mem-to-mem MOVE.w template lumped -12 before the read where the
  interpreter splits 8/4; lspc_mode_r derives the raster from the mid-
  slice clock (trap reads saved a1), so a 4-late MMIO read flips near
  line boundaries. Invisible to the rig under BOTH configs (RAM never
  traps) — every multi-access template must split charges at the
  interpreter's exact access points.
**RUNG-2 VERDICT (matched-pair): interpreter-vs-dynarec TRCRC IDENTICAL
over 5404 frames + [IO] MMIO stream IDENTICAL over 21104 reads; rig 96/96
both W3 configs; testsuite 125/126.** Rung 2 (RTS ender + 5 memory
templates + residency counter, ~10-13%% in-fight residency) is CORRECT by
the strongest available instrument. fps twins measured this session (see
below); traps recorded: N64_FRAME is the host VI count (guest key is
g_frame); unfiltered IO logging throttles ares below the target frame.

## 🔬 RUNG-2 SESSION 2026-08-07 night — two findings, one shipped-blocked

### 1. THE GATE WAS LYING: rig builds ≠ game builds (m64k/Makefile)
The emitter-differential rig — the standing gate for every template without
a TomHarte suite — compiles the testsuite with **-DM64K_W3_FULL**, which the
GAME build does NOT define (game = M64K_FASTPATHS only). So the rig has been
comparing emitted blocks against a *different interpreter* than samsho2 runs:
any template whose charge or order is fast-path-config-dependent can be
rig-green and still diverge in-game. Knob added: `make ... W3_OFF=1` builds
the testsuite with the game's fast-path set. **Run the rig BOTH ways before
trusting any new template.** This is the prime suspect for the frame-377
divergences (both this session's and the historical fallthrough one — same
frame, same bug class: a template that only becomes reachable when coverage
widens).

### 2. RUNG 2 (RTS ender + hot templates): DOES NOT SHIP — TRCRC diverges
Built: RTS ender (tail-jumps jmp_exec, so returns chain dynamically through
the existing probe and keep the idle-skip; IR=0x4E75 stored so
addrerr_fixup_rts is reached), MOVE.w (An)+,(Am) mem-to-mem, MOVE.w
Dn,(xxx).l, MOVE.l (An)/(d16,An) src + (d16,An) dst, service max_insns 8→24,
plus an emitted **residency counter** (`[DYNSTAT2] exec/execpf`) — the metric
that was missing all along: translated-block counts say nothing, only
executed-in-block insns/frame vs the ~8-11k dispatched do.
- Residency: ~0 → **901-1303 insns/frame in-fight (~10-13%)**. Seeding +
  enders + templates genuinely work.
- Gates: rig 96/96 (W3_FULL build), testsuite 125/126, all six control-flow
  btests green through the ender templates.
- **TRCRC DIVERGES at frame 377** → withheld. Dynarec stays DEFAULT-OFF so
  the deliverable is unaffected. Bisect knobs added: `M64K_DYN_NO_RTS`,
  `M64K_DYN_MAXINSN`. Start the bisect by re-running the rig with W3_OFF=1.
DATA-DRIVEN TEMPLATE METHOD (keep this): cross-reference `[DYNREF]` refused
heads against the `[DYNH]` hot-head histogram — it ranks refusals by actual
execution frequency instead of by count, and is what surfaced 0x0031FE.

### 3. BLOCKOPS HAS NEVER FIRED — shape guard off by two (FIXED, bit-exact)
Chasing the #1 refused-and-hot head (0x0031FE, `move.w (a0)+,(a4)` / `dbra`,
**13.7% of all control transfers**, 239 iterations/frame) led into the fused
DBF copy path, which the dynarec deliberately refuses ("BLOCKOPS handles
it"). New `[BOSTAT]` instrumentation: **fire=0** over 293k taken DBFs per
window, 94% rejected at the shape guard. Cause: the guard tests `t3 == -4`,
but dispatch_body's `addi m_pc,2` plus OP(dbcc)'s own put m_pc at body+6, so
a 1-word-body loop yields **-6**; -4 can only match a DBF branching to its
own opcode (a zero-body `dbra dn,*`), which is why the "body opcode" fetch
read back 0x51C8 (the DBF itself). The per-iteration charges are the
independent proof: 22 = move.w mem-to-mem (12) + taken dbra (10), and 18 =
move.w Dn,(An)+ (8) + 10 — those only balance for a REAL one-instruction
body. Fixed all six constants (guard, body fetch, three m_pc rollbacks).
**Gate: TRCRC fused-vs-unfused IDENTICAL over 5576 frames** (bit- AND
cycle-exact). Runtime twin gate added (`mvs64_blockop_enable` /
-DMVS64_BLOCKOP_DISABLE, layout-identical binaries) plus `BLOCKOPS_OFF=1`.
**MEASURED POSITIVE (layout-identical twins, mvs64_blockop_enable flip,
420s each, both reaching gameplay with ~9.6k in-fight samples):
median fps 41.0 -> 42.4 (+1.4), late-run 39.0 -> 40.4 (+1.4), and the
in-fight m68k share 59.9% -> 54.6% (-5.3 points).** The m68k-share drop is
the decisive number: it only moves that far if the 239-iteration/frame
copy loop is actually being fused, which also settles the destination
question — A4 IS the 0x3C0002 VRAM data port, exactly as the disassembly
predicts (0x31EA writes the modulo at A4+2, 0x31F4 the VRAM address at
A4-2, 0x31FE streams to A4).
CAVEAT: not bucket-matched — analyze-buckets.py needs [PERF2], which only
PERFCOUNT builds emit, and the DYNSTAT/BOSTAT rigs run ~3x slower so the
autoinput harness never reaches a fight in them (that is why the direct
in-fight [BOSTAT] fire count is still uncaptured; the fps/m68k twins
answer the same question better). Redo as PERFCOUNT twins for a
bucket-matched number before treating +1.4 as final.
SECOND LEVER FOUND: 0x0031B8 `move.l (A0)+,(A1)+ / dbra` is a genuine long
memcpy that NO blockop shape covers (0x20D8 vs the fill's 0x20C0) — adding
an (An)+->(Am)+ copy fusion is the obvious follow-up.

## 📊 DYNAREC RESIDENCY ESCALATION SESSION 2026-08-07 night (aa54fe1) — VERDICT: FLAT

Option-A escalation step 1 landed and measured. The wall was POPULATION,
not translation quality: 480s translated 18-32 blocks total (mailbox =
one head/slice, last-writer-wins; hot tried heads win the lottery;
~zero chains ever resolved). Landed, one gate cycle:
- **Chain-target seeding** (DYN_SEEDS_PER_SLICE=4): the pending-link
  list is the web frontier; dyn_service translates pending targets
  directly, patches resolved links, drops permanently-refused ones.
  Tried-filter 16384 bits (same fixed 8KB block).
- **Min-length filter removed** for ender blocks (it refused the
  hottest shapes — 2-insn tst+bcc poll loops, 0x0318A6 ~55/frame; the
  straight-line-tail hazard is already covered by ender-only policy).
- **Call/jump enders**: JMP (d16,PC)/(xxx).l, JSR (d16,PC)/(xxx).l,
  BSR.b/.w (W3-body transcriptions; abs targets store recomputed
  pc_diff — now .globl), LEA (xxx).l,An template, max_insns 8→24.
Population 480s: 32 blk/138 insns/10 chains → 79/213/44. Gates all
green: rig 83/83 (8 new call/jump/LEA seqs), testsuite 125/126,
JSR/JMP/BSR/RTS/Bcc/DBcc btests through the ender templates, TRCRC
IDENTICAL 7066 frames.
**PERF: twin-binary bucket-matched 480s = FLAT** (dominant 38.7/38.7,
light +0.6, heavy +0.1/+0.2 — at the ±0.3 line). 213 resident insns vs
~8-11k executed/frame is too small a fraction; webs break at every RTS
(×14 top refusal) and at coverage edges. LESSON: population/chaining
infrastructure alone does not pay at this coverage level — fps arrives
only if the resident fraction becomes a large share of executed insns.
NEXT (value order): (1) RTS ender (inline 2-way probe + jr t3; odd-
target raise design needed — pc_address_error entry from arena), (2)
template breadth at refusal heads (BTST/CLR/NOT/ANDI/MOVE.w dst forms,
ADD (An)+ forms), (3) fallthrough-chain root-cause (TRCRC bisect,
DYN_CHAIN_FALLTHROUGH still quarantined), (4) register caching once
residency is long. Measurement notes: ~9fps early-boot phase is the
normal UniBIOS/conversion stretch in every build; TRCRC frame-count
deltas across runs are load artifacts (ref ran under concurrent load)
— only solo twin runs count.

## 📊 CAMPAIGN DAY 2026-08-07 — SESSION RECORD (25 commits, 9bed4a5..2b55962)

State: ~39-40 fps modal / 34-36 heavy (unchanged deliverable). Landed:
- **Dynarec phases 1-3** (m64k/dynrec.c): correctness-COMPLETE — ~40
  templates (MOVEQ/MOVE.w×9/MOVE.b×7/TST×6/CMPI/ADDI/ALU×15/MOVEA/NOP),
  Bcc×14+BRA+DBF enders, taken-edge chaining, mailbox translation.
  Armor: differential rig (73 seq, canary-certified), TRCRC per-frame
  state-hash gate (bit-exact 6300 frames), [DYNTEST] EPC proof.
  PERF VERDICT: twin-binary A/B = FLAT (infra −0.5..−1.2, translation 0).
  Escalation to arena-residency = the remaining (long) path.
- **Measurement science** (the day's biggest lesson): cross-binary fps
  deltas carry ±10-15fps cache-set-alignment luck. Laws: twin binaries
  differing by one initializer constant; layout-invariant statics
  (fixed-size 8KB-aligned blocks); nm-perfsyms.sh counter/ctx dcache-set
  disjointness gate before trusting ANY PERFCOUNT fps (a collision
  doubled m68k time in a whole build family). Tools in repo root:
  nm-hotsets.sh / nm-hotdata.sh / nm-perfsyms.sh.
- **rspq lost-wakeup FIXED** (vendored libdragon closed-loop flush) —
  the stability bug behind every wedge since July.
- Measured-closed levers (clean methodology): walk-on-RSP (−1.3 modal
  confirmed), crom front-cache (−0.6), predecode (killed earlier), Z80
  quick steps, YM trims C1-C3, min-length sweeps, idle-probe sweep (two
  new rare vblank spins added, 2b55962; no large spins remain).
- Sprite-cache lookup measured at its structural floor (~1 dcache miss
  per draw); heavy-bucket cache% is irreducible without fewer draws.

**60FPS OUTLOOK**: all incremental levers are at measured floors. The
remaining structural candidates: (a) dynarec arena-residency escalation
(chain webs incl. fallthrough + call-graph enders + register caching;
multi-session, bounded by the 29.5KB-hot-text/16KB-icache overlap),
(b) RDP-side redesign of the dense-scene draw path, (c) accepting an
overclock/expansion-dependent target. Honest estimate: 60fps is NOT
reachable with the remaining incremental toolbox; it needs (a) or (b)
to pay off at full scale, both multi-session bets.

## 🚀 M64K DYNAREC BLUEPRINT (2026-08-07, workflow wf_e7459c1e-49c judge synthesis)

Predecode was measured-killed (see below); a true 68k→MIPS dynarec is the
only remaining lever big enough for the 60fps gate (est +6..+10 fps full
line, 8-14 sessions). SPINE = minimal-risk hybrid, escalate with DYNSTAT
evidence. NON-NEGOTIABLE LAWS (judge-verified against sources):
- Interpreter-identical register/flag conventions: ctx=a0, m_cycles=a1
  LIVE (hw_n64.S clamps saved a1), flags s6/s7/s8 eager in the existing
  encodings, loads→t0, stores from t6/result, canonical lwl/lwr-swl/swr
  pairs lifted as macros. Zero marshalling at any block boundary; bail to
  the interpreter at ANY insn boundary.
- **C_max entry gate**: enter a block only if m_cycles > worst-case
  one-pass charge (re-tested on DBF back-edges) — the interpreter blezes
  m_cycles before EVERY insn, so this is the only slice model that keeps
  slice-exit/IRQ boundaries bit-identical (block-head-only checks overshoot
  up to a block and change IRQ delivery points).
- Per-insn template-order cycle charges (no batching initially); MMIO
  observes the same mid-instruction clock.
- **hw_n64.S EPC ranges**: BOTH checks (ts_cur refresh ~:249, slice-break
  a1 clamp ~:417) must also accept [dyn_arena_lo, dyn_arena_hi) or MMIO
  from translated code silently loses mid-slice clock accuracy. Prove with
  an arena-resident MMIO stub unit test.
- **Delay-slot law**: hw_n64.S's delay-slot resolver only recognizes
  jr t0/t1/t3, jalr t7, jr ra — no trap-capable access in any other
  indirect delay slot, ever.
- **PBROM bank hook**: bank switches run in the asm TLB fast path
  (asm_pbrom_bankno_w, hw_n64.S ~:783) and NEVER reach hw.c write_pbrom —
  any bank-keyed translation must hook the asm path. Phases 1-2: the
  0x2xxxxx window is simply never translated. Page 0x000000-0x00007F
  (vector swap memcpy target) also excluded. ROM-only translation; RAM
  code never translated.
- Invalidation/arena reclamation DEFERRED to slice boundaries (an MMIO
  trap inside translated code must not free the code it returns into).
- Block table 1024×2-way from day one (direct-mapped hash evicts hot heads
  silently). Arena 256KB→2MB kseg0-cached; publish primitive = write +
  data_cache_hit_writeback + inst_cache_hit_invalidate + pointer publish.
- Density law: ≤40B emitted per guest insn average, hot/cold arena split
  (bails/resolver stubs in a cold sub-arena); [DYNSTAT] gates every phase.
- s3 stays reserved (predecode scaffold). Interpreter hot loop untouched:
  probes ONLY at _m64k_asmrun entry (after IRQ check) and jmp_exec (after
  the idle-skip compares), ~8 insns each.

**PHASE 1 (one session, NO emitted guest code):** M64K_DYNREC knob
default-OFF; arena + publish primitive; both probe sites with an empty
hash; the hw_n64.S EPC-range extension + arena MMIO-stub proof; DBF/branch
hot-counters + [DYNSTAT] listing candidate superblock spans, % of executed
insns in-span NET of M64K_BLOCKOPS-covered shapes, and the PC-region split
(P_ROM / PBROM-window / WORK_RAM — decides if bank-keyed translation must
be pulled forward); the deterministic per-frame 68k PC/reg/cycle trace-CRC
rig (ISViewer) demonstrated baseline-vs-baseline identical. Gates: build
green both knob states; testsuite 125/126; bucket-matched fps flat with
probes-in (≤0.3); hot-text audit ≤13.7KB; MMIO-stub test green.

**PHASE 1 RESULTS (2026-08-07, landed 4dd7c9e + DYNSTAT commit):** all
gates green — [DYNTEST] PASS (both EPC checks accept the arena; the first
run's deliberate-style failure machine-proved tlb_readhwio's SAFE_MODE
check enforces loads->t0 on emitted code); TRCRC determinism 2x240s, 6300
frames, 0 mismatches; testsuite 125/126 both knob states (150s run needed
when instrumented); fps A/B 480s dominant buckets -0.3 (probe cost priced
in); hot text 13,704B. **[DYNSTAT] coverage (480s, samsho2 attract+fights,
jmp_exec visibility):** in-game windows are 93-95% P_ROM / 5-6% BIOS
(0xC0xxxx) / **ZERO WORK_RAM and ZERO PBROM-window targets** — ROM-only
translation loses nothing and bank-keyed translation stays deferred
(phase-3+ at most, for other games). Hot heads are concentrated (top-2 =
0x0031FE+0x00335C ~15% of transfers; clusters 0x0318xx, 0x0045xx, all
comfortably above the excluded page 0). Density: ~1 counted transfer per
~5 executed insns in-game -> dynamic spans are SHORT; the C_max entry
gate + probe must stay lean and direct chaining is the key phase-2/3
escalation. BIOS region is immutable ROM: translate it in phase 2 (it is
5-6% of transfers).

**PHASE 2+:** per-form emitter from the debugged fast-path templates
(collapse decode preambles; PC-relative/immediates baked at translate
time), forced-superblock testsuite mode, then A's escalations gated on
DYNSTAT: direct chaining (resolver stubs, publish-ordering law),
per-block register caching (dirty writeback at every exit incl. bails),
dead-flag elimination (flags always correct at every possible exit),
PBROM bank-keyed maps via the asm hook. Est ~28-40 host cyc/insn after
phase 2, ~25-35 after density work (vs ~73 today → m68k ~11ms → ~4-5ms).

Full designs + judge: workflow wf_e7459c1e-49c journal (A = maximal block
dynarec, B = hybrid; judge = synthesis, all flaws enumerated).

## 🎯 60FPS CAMPAIGN ROADMAP (2026-08-07) — four tracks, workflow-planned + adversarially verified

Goal (user): reach 60 fps in ares testing as the finish gate, sound/graphics
bit-true, one gated commit per increment. Current: ~34-41 in-fight (play8).
Four track plans were produced by parallel planner agents reading the sources
and adversarially verified (2 lenses each on A/B; all verdicts proceed=true).
Full plans + verdicts: workflow wf_d758761b-bf5 journal. Execution order by
value-per-risk: C → B(step1) → A(phase 1) → interleave D groundwork, then
A phase 2 / B predecode / C tail / D fix as gates allow.

**Track C — YM emit trims (est +2..+3.5 total, five independent steps):**
0. Instrumentation: SSG-silent-fraction + WP-chunk-fraction counters.
1. WP dead-store elision: rspfm_pack_chan_wp for rspwp_kick_chunk — drop
   per-slot masks compute + slot-dynamics/op1_out/mem_value stores that
   cmd_fm_wp overlays with the RSP-resident dyn block and the kick
   overwrites (masks=0x000F). VERIFY claims against sources before coding.
2. Hoist pms lfo_pm dedup scan once per chunk (identical across 4 chans) +
   wp_dirty-gated static-field cache.
3. Mix banking-copy elision: point chunk accumulators at pd->acc_l/r after
   kick decision (keep the fallback path intact — timeout/death relies on it).
4. Emit copy elision: copy only CPU-mixed spans to stage.
5. SSG chunk batching (SSG_CALC_N, state hoisted); closed-form silent skip
   ONLY if step-0 shows it worth ~0.4ms+ (subtle exactness edges).
Gates: steps 1-4 are N64-only (PC WAV green by construction) → gate =
MVS64_RSPWP_VERIFY dual-compute + bucket-matched 560s; step 5 = WAV gate.
Risk: icache law applies to new pack/SSG variants — size-audit each.

**Track B — Z80 (re-priced: steps 1-3 +0.5..+1.5; uop-cache is the lever):**
1. Fused event check, ZERO NEW STATE (reviewer-revised): gate
   process_interrupts on the EXISTING packed bitfield byte
   (int_pending/nmi_pending share storage with iff1/iff2/halted in z80.h)
   plus iff_delay — a mask test, no dual-state maintenance to go stale.
   WAV-gated, ~30 lines.
2. Loop fusion z80_run_spin(z,until) — port sound_neogeo.c inner loop
   VERBATIM (couplings: SND_HEALTH counters, Z80HIST, NOIDLESKIP ifdefs,
   z80_ext_wrote, service_level_irq ordering). cyc/int state stays
   struct-resident across port/write callbacks (YM re-enters z80).
3. Hot/cold partition ONLY from MVS64_Z80OPHIST data (function granularity;
   do NOT pre-commit CB/ED cold — they're staple driver ops).
4. Single-site computed-goto: --param max-goto-duplication-insns=0 and an
   objdump gate that exec_opcode has EXACTLY 1 indirect-jump site.
5. ROM decoded-uop cache (the big one, +1..+1.5): records keyed by M_ROM
   offset (immutable → no invalidation), 4B/record {handler u16, len, cyc}
   — NO imm16 (operands read from resident M_ROM[] directly); residency =
   per-step check of PC+len-1 vs current bank-window end (windows differ:
   16/8/4/2KB); RAM/boundary/bank-edge falls back to classic; default-OFF
   knob + WAV gate.

**Track A — m68k predecode (est +2..+3; reviewer-revised phase 1):**
1a. M64K_PREDECODE scaffold, all pages classic, default-OFF. Record
   dispatch (12 insns) expanded at main_loop ONLY; the 30 active
   dispatch_next tail sites compile to `j main_loop` in predecode builds →
   phase-1 hot text SMALLER than the 13,624B baseline. s3 = L1 base
   (verified free, saved in prologue). L1 = 4096×u32 pre-biased:
   L1[page] = block − ((0xFF000000|page<<12)<<1) mod 2^32 (m_pc carries
   the 0xFF000000 map base!). Records 4B {s16 handler-off-from-main_loop,
   u16 operand}; consider 2B handler-only variant if dcache-bound (record
   stream costs ~1 line per 2-3 insns at 4B, not 1-per-8 as first
   estimated). Odd-PC law: opcode lhu PRECEDES record lh (bit-identical
   odd-PC behavior). Shared classic-trampoline block for unpromoted pages.
1b. Lazy promotion via trigger records + cold-C builder (own section,
   outside hot text); testsuite gets a forced-promotion mode. STALE-RECORD
   SOURCES: hw.c REG_SWPBIOS/REG_SWPROM memcpy INTO P_ROM page 0 →
   must call m64k_predecode_invalidate(0) (or never promote 0x000-0x07F);
   promote 0x200-0x2FF ONLY if pbrom_linear()!=NULL (cache mode → classic
   forever).
2. PBROM bank slices (8×256 L1 entries, swap on write_pbrom, ~1KB copy).
3. Family resolution + opsize baking (record targets final handler).
4. Specialize from the parked W3 corpus. RECORD-LAYOUT LAW: specialized
   handlers may borrow only the u16 operand halfword of adjacent records;
   every record's s16 handler halfword always holds the classic-equivalent
   target (mid-instruction jumps stay exact). Charge parity per-form vs
   the W3 body (CLR splits charges; JSR is 20/2 — no single blanket rule).
   Do NOT demote slim-wave-3 optable entries until [PDSTAT] proves the
   classic-page share <2-3%. Coverage honest target: ~72-77% (85% needs a
   further wave).

**Track D — walk-on-RSP coexistence (est +1.5..+3, killable per gate):**
New ranked hypothesis: the wedge signature (idle halt, st=0x3003, no
SIG_MORE, CLEAR_HALT-immune) matches a QUEUE-POINTER DESYNC (stale
RSPQ_RDRAM_PTR/POINTER_STACK → RSP refetches a 0x00 terminator and
re-breaks forever), driven by the walk's unique drain-to-idle edge ~8×/
frame colliding with coalesced highpri epilog-skip patches. Steps:
(1) zero-change groundwork: DMEM map audit of overlay ELFs, vendored-vs-
upstream rspq diff, WALKTO wedge-state dumper (SP_PC/SP_STATUS/DMA +
DMEM RSPQ_RDRAM_PTR/POINTER_STACK/CURRENT_OVL + CPU-side pointers);
(2) discrimination A: serialize audio highpri vs walk lifetime;
(3) structural: eliminate the walk's per-frame rspq_wait (sentinel
trailer poll + early kick + deferred consume); (4) evidence-driven
vendored-kernel patch; (5) default-ON flip + soak. Kill the track if the
dump implicates the un-halt hardware quirk.

Validation rig for every increment: testsuite (m64k) / WAV byte-identity
(shared sound code) / MVS64_RSPWP_VERIFY dual-compute (WP-only paths) /
560s bucket-matched ares run (fps gate) / ares boot + Sonnet-driven
BizHawk same-class check on deliverable builds. One commit per gated
increment, pushed individually.

**Session 1 results (2026-08-07 night):**
- C step 1 (WP pack dead-store elision) LANDED: +0.2..+1.1 fps, dual-
  compute green 37,888 chunks (7976315).
- C step 2 (pms scan memo) LANDED: ~flat, dual-compute green (856e4e1).
- **THE RSPQ LOST-WAKEUP IS FIXED** (7a7bc90 records the vendored-
  libdragon patch): C step 2's timing shift made the latent race
  DETERMINISTIC (wedge at frame 2668 every run, SP_STATUS=0x7003 =
  HALTED|BROKE|SIG_MORE — both open-loop wakeup writes landed inside a
  DMA-stretched mfc0->break window). Fix: closed-loop rspq_flush_internal
  (re-clear HALT while SIG_MORE pending, bounded). Repro passes full
  480s; same crash class found in four July logs (drawprof2, wpperf2/6/7).
- **Track D UNBLOCKED**: walk + RSP audio survives a full 480s
  (10,807 frames, zero wedges) — the shelving wedge WAS the lost-wakeup.
  Perf is mixed: +0.7..+1.0 fps in 3000+ spr buckets, −1.6 in the modal
  2000-3000 bucket (fixed kick/trailer-poll overhead). Walk stays
  default-OFF until D step 3 (early kick after VRAM writeback, deferred
  consume, no per-frame wait) removes the overhead; then re-A/B.
- D step 3 (early kick + sentinel poll) landed OPT-IN (9f6d351):
  WALKDBG green 7.8k frames, but MEASURED flat-to-worse vs the old wait
  protocol and modal 38.3 vs 39.9 no-walk. VERDICT: walk stays OFF —
  its overhead exceeds the C-walk savings at moderate sprite counts.
  Possible future: dynamic heavy-scene-only gating (spr>3000). Track D
  closed; the rspq fix is its lasting win.
- B step 1 (event-check gate) landed (8b4354c): WAV-identical, MEASURED
  FLAT (us/step 3.50 vs 3.49) — the call overhead was never the cost.
  B's remaining value is the ROM decoded-uop cache.
- PC (-Werror) build break from the walk split fixed (45b14cb) — run
  the WAV gate after any video.c N64-path change; it builds the PC side.
- **Next session start here: track A phase-1a predecode scaffold** (the
  reviewer-revised design above — record dispatch at main_loop only,
  tails become `j main_loop`, L1 bias includes the 0xFF000000 base),
  then C steps 3-5 (mix copy elision, emit spans, SSG batch).
  State: modal fight bucket ~39.9-40.3 fps, heavy ~34-36.

## 📐 OPHIST + FASTPATHS WAVE 3 (2026-08-06) — the icache cliff, measured

Session goal (user-picked): start the 68k structural spike toward 60fps.
Re-baseline after a month away was bucket-identical to July (39.5 fps modal
fight bucket) — state reproduced, toolchain healthy.

**New diagnostic: MVS64_OPHIST** — an exact 65536-slot per-opcode execution
histogram (dispatch bump in m64k_asm.S, table + periodic dump in emu.c,
analyze-ophist.py in the parent dir decodes forms and classifies fast-path
coverage). Also new: `slices=` in [PERF] (m64k_run entries/frame) and a
one-time [HEAP] free-RDRAM report. Key numbers from a 560s run:
- **Existing fast paths cover ~55-60% of executed instructions.** The
  uncovered remainder is a long tail; the top uncovered LONG-path forms
  (RTS 3.0%, MOVE.w (An)+,(An) 2.7%, BTST #,(d16,An) 1.3%, ADDI/ANDI #,Dn
  ~1.7%, JSR/BSR ~2.2%, CMP mem/reg ~1.8%, CLR mem ~0.7%, ...) total
  ~15-17% in-fight. NOP (1.3%) and MOVEQ (1.5%) look uncovered but their
  generic handlers are already 1-hop-minimal — no headroom there.
- DBF alone is 7.7-11.9% of ALL executed instructions (loop-heavy code).
- ~4MB of RDRAM headroom exists for a predecode table (heap span 7MB,
  ~2.9MB used).

**Wave 3 (full, 14 forms) REGRESSED: the icache cliff is real.** All 14
fast paths built and testsuite-green (125/126 CHK-only, cycle diff 2.67%
unchanged), but the full set grew m64k_asm.o text 13,208 → 15,320 bytes and
the bucket-matched result was **−2.9..−3.9 fps** with the m68k bucket UP
6-11 points. The interpreter shares the direct-mapped 16KB icache with the
TLB/MMIO trap handlers (~1000 traps/frame) and per-slice event code: at
13.2KB there was headroom, at 15.3KB the conflict/capacity misses swamp the
~1ms of saved instructions. **LAW: total hot interpreter text is a hard
budget (~13.5KB). Fast paths must REPLACE cost, not ADD text.**

**Slim wave 3 KEPT (+0.1..+1.2 fps all buckets, m68k bucket −1..−2):** only
the top-density paths stay default-on — RTS (inline pop+jmp_exec, 3.05%)
and MOVE.w (An)+,(An) / (An)+,Dn (3.3%) — text 13,624 bytes. The other 11
bodies are parked behind `M64K_W3_FULL` (default-OFF, still built+green in
the m64k testsuite Makefile) as the debugged seed corpus for the predecode
project.

**BUG FOUND AND FIXED (crashed the first full-wave run in the ares gate,
NOT the testsuite): the stale-dptr invariant.** Generic immediate/register
ops re-set `dptr` every time (decode_imm/decode_dptr/ADDQ's dummy slot);
dptr-blind rmw consumers (the shift path's dummy `lhu t0, 2(dptr)`) rely on
the leftover being an EVEN host pointer. Ops like CMPM.b legally leave dptr
ODD (guest pointer), and any fast path that skips a dptr-setting generic
extends the odd value's lifetime → misaligned-lhu CPU exception (hit at
BIOS RAM code, PC 0xC12598). Waves 2-3 both had this latently; every fast
path that replaces a dptr-setter now restores dptr identically (1 insn
each: CMPI×3, ADD, ADDQ/SUBQ, and the parked wave-3 forms). Lesson: the
testsuite runs opcodes in isolation and cannot catch cross-instruction
register-lifetime bugs — the ares boot/play gate is the real gate for
interpreter-state invariants.

**Predecode design implications (the structural project, next):**
1. Budget: the specialized-handler set must fit ~13KB WITH dispatch —
   i.e. predecode must REPLACE the generic decode paths, not sit beside
   them. Compact operand-in-record handlers, hot-first layout.
2. Phase 1 can be semantics-neutral: per-PC records that just point at the
   EXISTING handler (raw opcode as operand) — testsuite-gated plumbing
   (lazy per-page tables keyed off the TLB-mapped, PBROM-linear code
   space; ~4MB RDRAM headroom; RAM-code pages fall back to classic
   dispatch). Phase 2 specializes per OPHIST, respecting the text budget
   by REMOVING the corresponding generic paths.
3. Honest sizing: dispatch+decode is only part of the 73 cyc/insn; guest
   dcache misses and handler-body icache remain. Expect predecode to be
   worth a few fps, not a doubling — the 60fps gap also needs the Z80
   (threaded-code) and draw walls.

Remaining walls unchanged otherwise: m68k ~65-74% (this work), draw ~30-44%
in fights, snd 52-68% (Z80 interpreter at floor), TMEM rotation
(hardware-facing). Deliverable: play8 (slim wave 3 + the z80 snap repack
that missed play7).

## ❌ SPRITE WALK-ON-RSP SHELVED (2026-07-10) — ucode correct, deadlocks the RSP audio

The rank-3 lever (move the 381-sprite SCB walk off the CPU onto the RSP, ~12-19% of
the in-fight draw budget, plan-estimated +2-3 fps). **Phase A built and PROVEN
CORRECT, then shelved: it cannot coexist with the RSP audio offload.**

Design (Phase A): the CPU splits `render_sprites` into a **producer** (walk the SCB +
tilemaps, apply the exact chaining/vshrink/cull/auto-anim math, emit an 8-byte
visible-tile record list) and a **consumer** (unchanged empty-test + `draw_sprite`).
`cmd_sprite_walk` (rsp_video.S, cmd 0x5) is the producer ported to the RSP: it DMAs the
SCB + sprite tilemaps out of the writeback'd emulated VRAM in 16-sprite groups and DMAs
back the record list + an `{nrec,ovfl}` trailer the CPU polls. Consuming the list is the
unchanged CPU draw path, so the RDP stream is identical by construction.

**The ucode is correct and stable in isolation:**
- ✅ MVS64_WALKDBG dual-compute (C reference vs RSP list compared every frame): a 600s
  ares run through dense fights = **11,400 frames, 0 mismatches, 0 overflows**.
- ✅ Walk + **all audio on CPU** (`WP_OFF=1 ADPCM_CPU=1`): **5,150 frames, reached and
  held gameplay, no wedge**. Refactor also PC-pixel-gated 21/21 + WAV byte-identical.

**The blocker — walk + RSP audio wedges within ~100 frames.** Isolated by controlled
A/B: stable with CPU audio; wedges with the whole-pump FM offload (`~frame 100`) AND
with ADPCM-only/FM-on-CPU (`WP_OFF=1`, `~frame 70`). So it is the RSP-walk ↔ RSP-audio
**rspq-queue-sharing**, not any specific synth path. The walk drains the RSP queue every
frame right before the audio highpri burst, and that idle→wake edge trips the rspq
**lost-wakeup race** (the RSP breaks to idle between reading SP_STATUS and the CPU's
wake — see `RSPQCmd_WaitNewInput` in rsp_queue.inc; libdragon's own double-write and the
project's pump-entry watchdog do not cover this load). WALKTO instrumentation caught the
RSP **halted at the kernel idle loop, `st=0x3003`** (no SIG_MORE / SIG_HIGHPRI). The
hang is `rspq_write` blocking once ships pile up behind the wedged RSP.

**Fixes tried, ALL failed:** (1) HALTED+SIG_MORE clear at ship sites; (2) broadened
HALTED re-kick via `rspq_flush` before every ship; (3) fix-prep overlap (run the fix
layer's CPU prep while the walk runs — matches the surviving dual-compute build's
timing); (4) targeted `CLEAR_HALT` unwedge inside every audio wait-spin loop. **(4) is
decisive: CLEAR_HALT does NOT recover the RSP** (only 1 kick fired before the hang), so
at the wedge the RSP is NOT cleanly halted — it is in an unrecoverable state (running-
stuck/crash or a video↔audio overlay-DMEM/DMA interaction), not the simple lost-wakeup a
CPU-side re-kick can patch.

**Verdict: shelved.** Coexistence needs open-ended libdragon rspq-kernel work +
re-validation — a multi-session effort for +2-3 fps, versus the audio offload it fights
being a **~2x** in-fight win (dropping audio-offload to ship the walk is strongly
net-negative). **Landed & kept:** the produce/consume split (dde33a9, pixel+WAV gated —
a permanent clean seam) and the proven ucode (de7f1ff), now **default-OFF, opt-in via
`MVS64_WALK_RSP`** so a future rspq-hardening pass (or a libdragon upgrade) can revisit
without re-deriving it. **Deliverable stays play7.** Remaining tractable levers unchanged:
TMEM slot rotation (hardware-facing, can't gate in ares), then the diffuse YM ~0.5ms
candidates.

## ⚖ Z80/SND PASS CLOSED WITH MEASUREMENT (2026-07-09) — the interpreter is at its floor

**The sound bucket's books now balance exactly** (genms/[SNDRMS] + pub/[PERF]
instrumentation, 19736a3): in-fight profile_snd = **60% Z80 stepping (~5.7ms/frame,
~13% of ALL wall) + 31% YM emit (SSG+mix+copy+WP glue) + ~9% seams/publish**. The Z80
is the single biggest remaining CPU cost after m68k. Steps are REAL driver work:
MVS64_Z80HIST shows ~92-133k steps/interval across ~10 music-engine regions at
~300-430 steps per timer segment (only ~5k/interval is spin-detector re-arm at the
0x0130 idle loop, which is semantics-locked — the skip must fire at the same cycle
boundary or the WAV diverges).

Three experiments, all WAV-gated (3600-frame deterministic PC run, byte-identical):
- ❌ **Inline bus** (kill the read_byte/write_byte indirect call per access):
  bit-exact but MEASURED WORSE — us/step 3.29→3.62 (+10%), fps −1..−1.8. Inlining
  the 6-branch map into hundreds of rb/wb switch sites blew the icache; the callback
  keeps the decode in one hot line. Reverted; documented in z80_read's comment.
- ❌ **-Os on z80.c only** (Z80_OS=1 knob): text 39KB→23KB (−40%) but us/step and
  bucket-matched fps FLAT. Not switch-footprint-bound either.
- ✅ **Spin-snapshot repack** (three u64s composed in registers vs memset+memcmp on
  every backward-branch edge): kept — WAV-identical, +0.1..+0.8 fps in all buckets
  (noise-level-positive), smaller hot loop.

**Verdict: ~3.4us/Z80-step (in-fight ~5.3) is the interpreter's practical floor on
this CPU** — not call overhead, not icache footprint, struct already ~3 hot dcache
lines. A step-change needs structural work (threaded-code/JIT Z80 — a large project
with WAV risk). The YM half (~2.9ms/frame: ssg 55 + mix 59 + fm-glue 70 + eg 11 per
interval) is a set of ~0.5ms candidates, none dominant.

**Remaining walls, re-ranked by measured size:** (1) m68k ~65-78% of budget
(~11ms/frame at ~73 host cyc/insn — stall floor, hot forms drained); (2) Z80 ~5.7ms
(floor, above); (3) draw ~5ms in fights — **sprite walk 12-19% is the biggest
tractable target: walk-on-RSP ucode project**; (4) YM emit ~2.9ms (diffuse);
(5) TMEM slot rotation (hardware-facing, can't gate in ares).

## ✅ M64K FAST PATHS LANDED (2026-07-08 day session) — rank 4 finally paid, in the RSP era

**Measure-first result that unblocked the work:** a per-frame least-squares fit of the
m68k bucket against executed instructions and TLB faults (analyze-m68k.py on a PERFCOUNT
run, R²=0.99) split the bucket cleanly: **~90 host cycles per 68k instruction, 85-90% of
the bucket scaling on instruction count, TLB/MMIO traps only ~9-10% (~1.1ms/frame,
handlers included).** The interpreter is stall-dominated (ALU path ≈ 25-30 cycles), so
the lever is fewer icache lines touched per instruction, not fewer arithmetic ops —
which is why inlining the hot forms (2-3 straight-line lines vs 6-7 scattered lines of
decode_ea/check_cc chains) pays now, and why traps/io are documented as NOT the wall.

Landed as `M64K_FASTPATHS` (default ON, `FP_OFF=1` reverts; compiled out at
TIMING_ACCURACY>=1), two waves, both testsuite-gated (125/126 PASS = CHK-only known
failure; aggregate cycle diff unchanged at 2.67%):
- **a1b94b7 wave 1:** Bcc direct condition dispatch (bcc_cctable, sense baked in, GE/LT
  collapse to bgez/bltz on flag_nv; taken path unchanged so idle-skip still fires);
  MOVE.w Dn->{Dn,(An),(An)+,(d16,An)}, (d16,An)->Dn, #imm->(An); MOVE.b (d16,An)->Dn +
  (An)+->Dn; TST.b/.w (d16,An); dispatch_next macro (main_loop's dispatch expanded at
  fast-path tails — main_loop expands the same macro).
- **f5af2c3 wave 2:** DBF skips check_cc; ADD.b/.w/.l Dn,Dm; ADDQ/SUBQ #,Dn; CMPI #,Dn;
  MOVE.l/MOVEA.l reg->reg + (d16,An)->Dn/An.

**Bit-exactness discipline:** identical accuracy-0 cycle charges applied BEFORE the m68k
data access (MMIO observes the same mid-instruction clock — samsho2 reads LSPCMODE);
loads in t0 / stores from result (TLB trap register convention); A7 quirks, MOVEA.w,
ADDA/ADDX and all odd EAs stay generic; on ADDRERR builds an odd address bails to the
generic implementation with no state mutated, so address errors raise via the original
machinery.

**Measured (ares 560s runs, spr-bucket-matched fight frames): +3.1..+3.8 fps in every
bucket — 36.9→40.0 / 36.0→39.0 / 32.2→35.9 / 30.1→33.9; m68k bucket 76→67 / 77→68 /
83→71 / 93→78; fitted per-instruction cost 90→73 host cycles.** Deliverable: play7.

**Next walls, measured tonight:** (1) **snd ~56-64%, of which the Z80 interpreter is
~half** ([SNDRMS] z80ms≈310 vs ymms≈300 per interval; within YM: fm-glue 70 / ssg 55 /
mix 59 / eg 11 / adpcm 3) — a Z80 core fast-path/structural pass is the next single
biggest CPU lever, WAV-gated; (2) sprite walk 12-19% (walk-on-RSP, ucode project);
(3) m64k residual: remaining forms are each <1% of count — diminishing, the well is
mostly drained; (4) TMEM/descriptor slot rotation (hardware-facing, can't be gated in
ares).

## ✅/❌ DRAW TIER MEASURED (2026-07-07 day session) — the fix layer was the lever, not the RSP

[PERF2] fine profiling split the in-fight draw 33% into: **fix layer 15.5%**
(all 40x28=1120 cells drawn every frame — the map is full of nonzero blank
tile codes), sprite walk 7.2%, crom hash lookups 5.9%, rspq writes only 2.7%.

- ✅ **Fix-layer empty-tile skip (21230e5): in-fight 22.9 → 29.3 fps median**
  (content-matched music fights, rms>0 + PC filter). Tiles that decode to all
  index-0 pixels can never touch the screen (color 0 alpha forced 0 in every
  palette); emptiness is ROM-stable per SROM bank, learned on first sight
  (srom_tile_empty). PC pixel gate: 21/21 screenshots byte-identical.
- ❌ **Batched draw records (CPU arena + RSP replay loop): NET-NEGATIVE, reverted.**
  Cached-arena appends pay a write-allocate dcache miss per line (rspq bucket
  292→531); adding CACHE 0xD create-dirty-exclusive made the whole system
  WORSE (fps 20.2, snd/m68k +10%): a 7-12KB per-frame streaming write sweep
  through the 8KB direct-mapped dcache evicts every other subsystem's hot
  data. The uncached rspq_write design pollutes nothing and is already
  near-optimal — same lesson class as -O3/-Os icache results.
- ❌ **Direct-mapped L1 in front of the crom hash: NET-NEGATIVE, reverted**
  (cache bucket 565→815). The robin-hood table at 31% load averages ~1.2
  probes; an L1 adds a guaranteed extra dcache miss + 16KB of table pressure.
- ⚠ **Latent hazard noted for ANY deferred-consumption draw scheme** (batch
  records or SCB-walk-on-RSP): a mid-frame forced cache eviction can recycle
  a pixel slot an in-flight record still references. Needs a syncpoint guard
  on the forced-evict path.

**In-fight state after this session: ~29.3 fps median; split snd ~121 /
m68k ~66 / draw ~23 (fix 6.3, walk 7.2, cache 5.7, rspq 2.9) / io ~6.**
Remaining draw levers are small (walk-on-RSP ≈ +2-3 fps for a full ucode
project). The dominant remaining lever is snd ~121% → **FM whole-pump
batching with RSP-resident state** (see fm-rsp-economics memory): CPU
journals YM writes per chunk, one RSP command per pump replays
writes+synthesis, collect deferred to the next pump. The bit-exact FM ucode
(rsp_fm.S) already exists.

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

## ❌ RSP TIER, STEP 2 CLOSED WITH MEASUREMENT (2026-07-08 night 2) — FM-on-RSP parked

The FM port is bit-exact (rsp_fm.S; two 300s dual-compute gates, 0 mismatches) but
NET-NEGATIVE in every collection strategy measured: ship-4 sync 12.6 fps ([RSPWAIT]
fm≈2.4ms/chunk, q=0 → pure compute stall the CPU cannot overlap), exact skip masks
no change (fight channels keep slots live), ship-2 split 21.8 (glue > saving),
cross-pump deferred collect 21.25 (samsho2's driver writes YM regs every tick, so
sync-on-touch fires immediately; branch fm-defer-experiment), ADPCM-only+defer 20.25.
Full table in docs/archive/BUILDS-2026-07-08.md. The C FM path (~1.1ms/chunk/channel, dcache-
resident narrowed tables) is the bar; a future attempt needs whole-pump batching
with RSP-resident state. Remaining viable levers: RSP draw-issue offload (draw ~33%),
m68k rank-4 residual, and the deeper Z80/m68k structural work.

## ✅ RSP TIER, STEP 1 LANDED (2026-07-08 overnight) — ADPCM on the RSP

The first structural lever is in (see docs/archive/BUILDS-2026-07-08.md): YM2610 ADPCM-A (6ch) +
ADPCM-B decode runs on the RSP (`rsp_audio.S` rspq overlay), kicked at the top of each
synthesis chunk so it decodes underneath the CPU's FM/SSG passes. Gated bit-exact by a
300s ares dual-compute run (35840 chunks, 0 mismatches) + PC WAV byte-identity.
**In-fight: 22.0 → 26.3 fps median; snd 135% → 119%; cpu 244% → 226%.** Default ON
(`ADPCM_CPU=1` reverts). Remaining structural levers, in order: (1) FM synthesis on the
RSP — snd ~119% is now nearly all FM+Z80+SSG, but tl_tab (26KB) exceeds DMEM so it needs
table restructuring, a full session of its own; (2) RSP draw-issue offload (draw ~33%);
(3) m68k residual (rank-4 fast paths, ~67%).

## ✅ SCOPE CLOSED (2026-07-07 overnight) — CPU-side plan complete

Every remaining ranked item is now landed, measured-neutral-but-kept, or closed with
measured reasoning (see docs/archive/BUILDS-2026-07-07.md for the full table): rank 5 (io reads are
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

## 2026-07-08 session — whole-pump default + pump reorder: in-fight 35.6 fps

Landed (perf-fps a3c1a3c..b357256, all gated):
1. Full WP-M2 (RSP-resident ADPCM + cross-pump publish deferral): same-source
   bucket-matched A/B ~doubles in-fight fps; RSPWP now DEFAULT-ON (WP_OFF=1).
2. Sprite-layer empty-tile skip (crom bitmap; pixel gate 21/21).
3. Per-tile [PERF2] split restored (+empty=): exposed that "dense scene"
   buckets were RSP saturation (tiles ~565 const, rspq 3->76%).
4. THE REORDER: plat_audio_pump moved to end of emu_render — the audio RSP
   burst drains under the frame's 68k instead of stalling the next render.
   rspq bucket -> 2.6-3.6% everywhere. 600s in-fight median 35.6 fps.
5. SyncPipe dropped from the per-tile sprite RDP stream (hardware-facing).
6. ADPCM VU multiplies (vmudh/vmudm); verify gate 36864 chunks 0 mismatch.

Method note: NEVER compare unbucketed fight medians across builds — faster
builds reach different content. analyze-buckets.py (parent dir) buckets by
[PERF2] spr ticks.

Next walls at 35.6 fps in-fight: m68k ~78% of frame budget, sprite walk
~12-14%, snd ~62%. The 68k core (m64k throughput/RMW) is the next lever;
draw-side options: descriptor/TMEM slot rotation to pipeline RDP loads
(hardware-facing), walk micro-opts.
