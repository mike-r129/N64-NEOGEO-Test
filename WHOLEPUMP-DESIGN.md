# FM whole-pump batching on the RSP (MVS64_RSPWP) — design

Goal: remove FM synthesis (~28% of wall in-fight) from the CPU without the
per-chunk collect stall that killed FM-P1/P2 (see fm-rsp-economics memory).

## Core insights (all verified against this source, 2026-07-07)

1. **Defer within the pump, not across pumps.** sound_gen_samples() is one
   pump (~736 samples, ~6-17 internal chunks). Chunks kicked to the RSP at
   their boundary overlap the SUBSEQUENT Z80/SSG/ADPCM work; only the
   pipeline tail waits at pump end (~0.5-2ms once per pump), before emit's
   final output must be complete. No ring changes, no latency change, no
   cross-pump pending machinery (the fm-defer-experiment branch is NOT
   needed).
2. **The per-chunk pack IS the journal.** YM register writes only happen
   between YM2610Update_stream calls; every internal chunk of one call
   shares the same registers. Packing static params from the CPU's register
   mirror at chunk start delivers exactly the write timeline. NO
   sync-on-YM-write hooks (what killed FM-P2).
3. **FM dynamic state lives ONLY on the RSP** (RDRAM-resident block, chained
   across chunk commands and pumps): phase[4], volume[4], vol_out[4],
   state[4], op1_out[2], mem_value per channel. The CPU never needs it back
   (no YM register read exposes FM dynamic state; status reads = timers,
   CPU-side). Escape hatch: drain RSP → DMA block → adopt to CPU (existing
   rspfm_adopt_chan shape) → CPU-only synthesis for the session.

## Write-handler audit (what touches dynamic state)

- FM_KEYON: {key=1, phase=0, state=EG_ATT} — no volume dependence in this
  core. FM_KEYOFF: {key=0, if(state>REL) state=REL}.
  → CPU tracks the key-flag automaton (it owns SLOT->key) and accumulates a
  NET per-slot event code since the last shipped chunk:
  NONE / ON(phase=0,state=ATT) / OFF(state>REL→REL, conditional ON THE RSP) /
  ON_THEN_OFF(phase=0, state=REL unconditional). Exact because no synthesis
  happens between writes.
- set_tl / 0x90 vol_out recompute: vol_out == tl + volume is an invariant in
  non-SSG mode (advance_eg maintains it; KEYON doesn't touch volume), so the
  RSP recomputes vol_out for all 16 slots at chunk start from resident
  volume + shipped tl. Bit-exact, stateless.
- set_ar_ksr/set_dr/set_sr/set_sl_rr/set_det_mul/0xB0/0xB4/0xA0: all static
  (rates, shifts, Incr refresh at Update start is CPU-side) — covered by the
  existing rspfm_pack_chan fields.
- 0x90 SSG-EG: ssg static; ssgn write-tracked by CPU while ssg&8==0
  (advance_eg's ssgn mutation is gated on ssg&8). **Any slot with ssg&8 set
  → HARD HATCH** (chip-level: resync to CPU synthesis + counter; expected 0
  in samsho2 — the FM-P1 design already excluded SSG-EG).
- LFO (0x22) + eg_timer/eg_cnt: CPU pass 0 unchanged; egt[]/lfo_am[]/eg_base
  shipped per chunk as today.

## Per-chunk flow (MVS64_RSPWP)

CPU at chunk start (inside Update_stream loop):
1. pass 0 as today (egt/lfo_am/lfo_pm arrays, eg_base).
2. Pack DIRTY channels only (write handler sets per-channel dirty flags):
   static fields of the 416B channel block + key-event codes + ssgn. Clean
   channels: RSP reuses its resident copy of the previous block (static
   fields resident alongside dynamic). pm dp tables via existing pm_key
   cache; pmidx scan per pm-active channel per chunk (optimize later:
   lfo_pm∈0..31, a dp[32] indexed directly would kill the scan at +384B
   DMEM/channel — revisit).
3. Kick chunk command (highpri): args = journal block phys (header:
   n/eg_base/egt/lfo_am + dirty-mask + dirty blocks), out slot phys, resident
   state phys. DO NOT WAIT.
4. SSG pass + ADPCM as today (M1: ADPCM keeps its synchronous per-chunk
   round trip — its wait is exposed now that CPU FM is gone; M2 moves ADPCM
   into the same deferred command).
5. Store acc_l/acc_r (SSG+ADPCM+dtb partial) into a pending-slot pool
   (4 slots x ~1KB — keep SMALL, dcache law from draw-tier-2026-07-07) with
   the emit destination (out ptr, from). The silent-channel fast path moves
   to the RSP (CPU can't see state): check resident state/vol_out/op1/mem,
   batch phase-advance (pms==0: phase+=Incr*n; pms!=0: per-sample dp path).
6. At each chunk boundary: poll (nonblocking) older pending slots; for each
   finished: final mix = clamp(acc + rsp_fm_l/r) → packed u32 store to the
   uncached out buffer (emit's format).
7. At pump end (before sound_gen_samples returns): force-finish all pending.

RSP chunk command (rsp_fm.S evolution):
1. DMA journal header + dirty channel blocks over the resident copies
   (resident = RDRAM mirror of all 4 x 416B blocks + dynamics).
2. Apply key events (phase=0/state writes, OFF conditional), recompute
   vol_out (tl+volume, no negate — SSG-EG hatched away).
3. Silent-channel check + batch phase advance, else synthesize with the
   EXISTING bit-exact fm_chan code.
4. DMA dynamics (+updated statics) back to the resident block; write out
   l/r + seq word to the out slot.

## Verify mode (MVS64_RSPWP_VERIFY)

CPU keeps its own authoritative state and synthesizes every chunk as today
(pass 1 unchanged); the RSP chain runs deferred in parallel from its own
resident state. At collect, compare samples (and periodically state blocks).
Both apply identical writes at identical chunk boundaries → must be
bit-exact. Gate: 300s ares, 0 mismatches, plus hatch counters == 0.

## Hatches (all: drain pipeline → adopt state to CPU → rspwp_dead=1 + count)

- any FM slot written with ssg&8 (SSG-EG enable)
- YM2610Reset
- RSP timeout (existing rspfm_dead pattern)

## Milestones

- WP-M1: structure above, ADPCM synchronous, verify gate green.
- WP-M2: ADPCM into the deferred command + CPU address-model for the Z80's
  ADPCM end-flag reads (deterministic: cur_addr advances 2 nibbles/sample
  while playing; flags = f(key events, samples elapsed) — bit-exact because
  Z80 reads happen at chunk boundaries). ADPCM-B limit-wrap edge → hatch.
- WP-M3: measure (expect snd 121→~55-65 → in-fight ~40+fps), tune, validate
  (WAV: PC path untouched; ares 300s; BizHawk same-class), docs, play6.

## Expected economics (in-fight, from 29.3 fps baseline)

CPU keeps: Z80 (~25%), SSG, pass 0, pack (~dirty-only, small), pending mix.
CPU sheds: FM synthesis ~28% of wall, most ADPCM wait (M2).
r: 0.52 → ~0.28 → wall 34.1ms → ~23ms → **~40-44 fps** if glue stays small.
RSP: FM ~2.8ms per 128-sample chunk worst case, ~30% occupancy — fits with
draw + ADPCM. Chunk commands are short → highpri audio never blocks video
for long (the monolithic-command trap from FM-P1 does not apply).
