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

## WP-M2 measured addendum (2026-07-07 evening) — why full M2 is required

M2-lite (lockstep-1 deferred ADPCM adopt, commit 84bfac1) measured 23.8 fps
sustained in-fight (wpperf9, 600s, no crashes, 0 watchdog kicks) with adpcm
waits ~1.1s/128pumps. The wpperf6 45.6 fps was a short atypical fight window
(n=219); its own fight-phase RSPWAIT entries were already escalating.

Root cause (now fully characterized): within a pump the CPU generates chunks
at BURST speed (~1.5ms of CPU work per chunk), while the RSP needs ~3-4ms
per chunk (FM 1.5-2.5 + ADPCM 1.5-2). The deficit accumulates inside every
pump and is paid at the lockstep-1 adopt and the pump-end force-finish. NO
in-pump pipeline depth fixes this: the deficit can only drain during the
inter-pump 68k window (~25-40ms), which requires:

1. RSP-resident ADPCM waveform state (acc/astep/aout/now_data per A channel,
   acc/adpcmd/prev_acc/now_data/adpcml for B) with a fresh-mask for key-ons —
   ucode overlay command mirroring cmd_fm_wp. Address fields (now_addr,
   now_step) advance arithmetically at build time (the nib math already in
   rspa_build; post-end values are dead state since key-on resets them).
   B limit/repeat chunks: hard-drain hatch (download resident block, C
   decode, re-fresh next chunk).
2. rspa param/out/src rings of 16 aligned with the FM ring (same slot);
   ADPCM l/r folds at the SAME deferred collect as FM (pend gains the rspa
   seq; final mix = acc + fm_ob + rspa_ob).
3. Cross-pump OUTPUT deferral: the pull-ring holds ~80ms of lead (TARGET_
   LEAD 2 buffers), so pump N may return with tail chunks pending; they
   complete during the inter-pump 68k window and fold at the next pump's
   start. Force-finish only when the ring read pointer approaches a pending
   segment (the fm-defer-experiment "lead<n force-finish" concept, but
   WITHOUT the per-YM-write sync hooks — state consistency comes from
   residency, not adoption). Ring wrap is safe: pendings live < one pump
   << ring size. SNDRMS tail-chunk rms becomes approximate (diagnostic only).

Also measured/landed on the way:
- lowpri audio queue: NET-NEGATIVE (adpcm waits 3ms -> 1.6s/128pumps, fps
  18.9) — the deferred adopt inherits the video pipeline's RDP stalls. Audio
  stays highpri.
- rspq lost-wakeup watchdog (plat_audio_pump): halted+SIG_MORE persisting
  across two pumps -> clear halt. Covers the kernel race that panicked
  display_get three times; 1200s of soaks since with zero false positives.

## Full WP-M2 implementation (2026-07-08)

Landed exactly per the addendum, with these concrete choices:

- **cmd_adpcm_wp (0x2)** in rsp_audio.S mirrors cmd_fm_wp: a3 = resident
  dyn block phys (128B: 6 x 16B A slots {acc,astep,aout,now_data} + 24B B
  slot {acc,adpcmd,prev_acc,adpcml,now_data}); DMA in after the param,
  overlay per channel unless its FRESH bit (param +8/+9) is set, DMA back
  BEFORE the seq word. `.align 3` before DYNA (the rsp_fm DMA lesson).
- **aout is NOT resident**: aout == ((s16)acc*vol_mul)>>vol_shift & ~3 is
  an invariant at every chunk boundary of a live channel (recomputed on
  every nibble batch; A consumes >=1 nibble per sample at the fixed 18.5k
  rate), so the RSP recomputes it at chunk start from the effective acc +
  shipped volume. This makes ADPCM-A TL/IL writes FREE under residency
  (they recompute the CPU copy from a stale acc — same shape as FM's
  vol_out = tl + volume). B's adpcml rescale (reg 0x1b) is the ONE write
  that mutates resident state in place: rare drain+pull+refresh hatch.
- **Residency masks, not fresh flags**: wpa_res/wpb_res track which
  channels the RSP owns. Key-on clears the bit (CPU fields just got
  reset) -> next build ships FRESH. Pulls (B nonlinear window, staging
  overflow, 0x1b rescale, offload death) drain the last kicked seq, read
  the block back, and clear the bit.
- **B eager EOS at build** (arrived | EOS bit, PCM_BSY=0, portstate=0 when
  the walk hits end this chunk): M2-lite adopted these one chunk late,
  which could cross a Z80 slice between emit calls — full M2 restores the
  C core's call-boundary timing exactly. (A's eager end flags were already
  in M2-lite.)
- **rspa param/out/src rings of 16**, indexed by the SAME slot as the FM
  pend ring; pend gains {a_on, a_seq} and rspwp_collect waits on both seq
  words and folds acc + fm + adpcm in one place.
- **Cross-pump deferral is at BUFFER granularity** in plat_audio_pump, not
  per chunk: sound_gen_samples ends with YM2610_wp_finish_async() (ship +
  sweep, no drain); the stage->aring publish is deferred to the next pump
  entry behind the blocking YM2610_wp_finish(). The fill policy counts the
  pending buffer as staged lead; a safety valve publishes immediately if
  published lead drops under one AI callback (n frames), which degrades to
  exactly the old behavior when already behind. [SNDRMS] reads the tail
  spans pre-final (diagnostic only).
- **Verify**: MVS64_RSPWP_VERIFY now also dual-computes ADPCM (C stays
  authoritative incl. flags/addresses — eager+advance are compiled out;
  the RSP chain runs from its own resident state and is compared per
  sample + per dyn field at collect; aout compare skipped for channels
  that END in the chunk, where C freezes a write-time value and the RSP a
  chunk-start recompute — dead state either way).

## The pump-position discovery (2026-07-08, the second big lever)

Measured (600s, same source, RSPWP on vs off, spr-bucket-matched): the
offload roughly DOUBLED in-fight fps (19.2->35.9 etc). But the restored
per-tile [PERF2] split then showed tiles/frame ~CONSTANT (~565) across
"scene density" buckets while the rspq bucket exploded 3% -> 76% — the
buckets were really RSP-SATURATION buckets: the offload freed the CPU but
made the RSP the contended resource (~12-16ms of audio per pump), and the
main loop pumped audio at loop end, ~0.7ms of 68k before the NEXT frame's
render event issued its draw commands into a queue still full of audio.

Fix: pump the audio at the END of emu_render (right after the frame's
draw issue). The RSP drains the fast video queue first and chews audio
under the remaining ~90% of the frame's 68k work. Result: rspq bucket
2.6-3.6% in ALL buckets, in-fight median 35.6 fps over 600s (n=12127),
audio starvation back at the WP-off structural baseline.

Also landed the same night: sprite-layer empty-tile skip (crom bitmap,
pixel-gated 21/21, ~50/565 tiles), per-tile SyncPipe removal from the
sprite RDP stream (spec-safe; aimed at real hardware — ares barely
models sync stalls), and VU multipliers for the ADPCM MULU loops
(vmudh/vmudm + vsar; verify gate green, 36864 chunks 0 mismatch).

Ladder of record (in-fight medians, content-matched): play4-era ~22.9 ->
play5 29.3 (fix skip) -> WP-off same-source baseline 19.2* -> full WP-M2
21.6-35.9 by bucket -> +pump reorder 35.6 overall median.
(*today's baseline runs slower than play5-era measurements at matched
spr — content/music mix differs across eras; same-day A/B is the truth.)

Remaining walls at 35.6: m68k ~78%, sprite walk ~12-14%, snd ~62%
(of a 60fps frame budget) — the 68k core is the next big lever.
