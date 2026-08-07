# PLAN-OPTIMIZATION.md — mvs64 / samsho2 N64 Framerate Plan

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
Full table in BUILDS-2026-07-08.md. The C FM path (~1.1ms/chunk/channel, dcache-
resident narrowed tables) is the bar; a future attempt needs whole-pump batching
with RSP-resident state. Remaining viable levers: RSP draw-issue offload (draw ~33%),
m68k rank-4 residual, and the deeper Z80/m68k structural work.

## ✅ RSP TIER, STEP 1 LANDED (2026-07-08 overnight) — ADPCM on the RSP

The first structural lever is in (see BUILDS-2026-07-08.md): YM2610 ADPCM-A (6ch) +
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
