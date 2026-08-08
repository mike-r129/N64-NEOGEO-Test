# PLAN-DRAW-RDP.md — Dense-Scene Draw Path Redesign (structural candidate (b))

**Status: NOT STARTED. Execute after the dynarec track (candidate (a), task #38
lineage) reaches a stopping point.** This is the plan for the second of the two
structural 60fps bets named in PLAN-OPTIMIZATION.md's campaign outlook:

> (b) RDP-side redesign of the dense-scene draw path

Written by Fable 2026-08-07. Intended executors: **Opus** (ucode/asm design
steps, hazard analysis) + **Sonnet** (instrumentation, builds, measurement
runs, emulator driving, mechanical C edits). Branch: `perf-fps`. One gated
commit per increment, pushed individually. Update the RESULTS section at the
bottom of this file as increments land.

---

## 1. What "(b) RDP-side redesign" means, and what it is NOT

The dense-scene draw tier costs **44–68% of the frame in heavy buckets** (the
user-observed round-start / death-blow / multi-character dips), and every
*incremental* draw lever has been measured-killed (see §3). The remaining move
is architectural: change **who does the per-tile work**.

Today the pipeline is *CPU-driven, one tile at a time*:

```
CPU walk SCB → per-record: cache hash lookup → rspq_write(4 words)
                                  ↓ (per tile, 565–4000+/frame)
RSP cmd_sprite_draw: palette check → build 9-quad RDP template → send
                                  ↓
RDP: SyncTile+SyncLoad → LoadBlock(128B → TMEM addr 0) → TexRect(1px/cyc)
```

Three independent inefficiencies, one per stage:

1. **CPU**: pays a hash lookup (~1 unavoidable dcache miss per *draw*, measured
   at its structural floor) plus an rspq command write **per tile**. In heavy
   buckets the sprite cache alone is 16–26% of the frame; this cost scales with
   draw count, not with unique content.
2. **RSP**: runs ~100+ instructions of template-copy + palette-search **per
   tile**, entered through the generic rspq dispatch loop per command.
3. **RDP**: every tile LoadBlocks into the **same TMEM address (0)**, so
   SyncTile/SyncLoad serialize each tile's texture load against the previous
   tile's draw — the RDP ping-pongs load→draw→load→draw with no overlap. And
   the RSP path draws everything in **1-cycle mode** (1 px/clock); the old CPU
   path used COPY mode (4 px/clock) for plain tiles — that 4x was lost in the
   RSP conversion (`video_n64.c:210-214` vs the old `rdpq_set_mode_copy` path
   at `video_n64.c:193-197`).

The redesign: **resolve tile pointers once per unique tile on the CPU, hand the
RSP a whole record list per frame (one command, not thousands), and emit an RDP
stream that pipelines** (rotating TMEM slots, sync elision, COPY mode for the
modal tile). Dense scenes then cost the CPU ~O(unique tiles + misses) instead
of O(draws), the RSP loops in a tight resident loop instead of re-dispatching,
and the RDP overlaps loads with draws.

This is NOT: frameskip, draw reordering (NeoGeo priority order must be
preserved — records must be drawn in walk order), offscreen composition /
static-layer caching (accuracy risk, rejected), or a revival of the killed
batch-arena (that failure was a *cached*-write sweep; see the law in §9).

---

## 2. Current architecture — annotated map (read these before coding)

| Piece | Where | What it does |
|---|---|---|
| Walk (produce) | `video.c` `sprite_walk_produce()` (~:125) | SCB walk → flat `SprWalkRec` list (8B: tnum/pal/flip in w0, x/y/sw/sh in w1). Bit-exact C reference; RSP twin exists (`cmd_sprite_walk`, opt-in `MVS64_WALK_RSP`, currently default-OFF — net-negative at modal sprite counts). |
| Consume | `video.c` `sprite_walk_consume()` (~:269) | Per record: `crom_tile_empty` bitmap skip → `draw_sprite()`. |
| Per-tile issue | `video_n64.c` `draw_sprite()` (:97) | `crom_get_sprite()` then `rsp_sprite_draw()` = one 4-word `rspq_write` per tile. DRAW_PERF buckets: `perf_dr_cache` / `perf_dr_rspq` / `perf_dr_tiles` / `perf_dr_empty`. |
| Tile cache | `roms.c` `crom_get_sprite()` (:148), `sprite_cache.c` | Robin-hood hash (31% load, ~1.2 probes, ~1 dcache miss/lookup). Miss = 128B PI DMA via `dfs_read` (tiles are pre-converted CI4 in the DFS). Eviction: `sprite_cache_pop` LCG scatter at frame tick; forced pop on cache-full inside `sprite_cache_insert`. `evict_gen` counts evictions (memo-invalidation hook, kept from the front-cache postmortem). |
| Ucode | `rsp_video.S` `cmd_sprite_draw` (:475) | Per command: vector palette-cache search (16 slots, TLUT at TMEM 0x800), template copy of `RDP_SPRITE_DRAW` (9 quads, :374), patch fields, `RDPQ_Send`. |
| RDP stream | `RDP_SPRITE_DRAW` (:374) | SyncTile, SyncLoad, SetTexImage, SetTile(TILE1 load @tmem 0), SetTile(TILE0 CI4 @tmem 0), SetTileSize, LoadBlock(128B), TexRect. 1-cycle standard mode + TLUT + alphacompare set once in `render_begin_sprites`. Per-tile SyncPipe was already removed (2026-07-08, item 5). |
| Fix layer | `rsp_fix_draw` + `srom_tile_empty` | Already skip-optimized (+6.4 fps win, 2026-07-07); not a target here. |

Scale facts: in-fight ~565 tiles/frame is the modal constant; dense
sprite-background stages issue up to ~8–10k tiles/frame *before* the empty-tile
bitmap skip (`roms.c:127` comment); `SPRWALK_MAX_RECS` = 4096 records after
culls. Records already live in a 16-byte-aligned static arena with an RSP
trailer convention (`video.c:84`).

## 3. Why incremental levers are closed (do not retread)

- **crom front-cache** (memo ahead of the hash): −0.6 dominant, killed 15dcba3.
- **Direct-mapped L1 before the hash**: cache bucket 565→815, killed.
- **Batched draw records via cached CPU arena**: dcache write-allocate sweep
  through the 8KB direct-mapped dcache evicted every other subsystem; WORSE
  with CACHE 0xD. Killed. (The uncached-write design is the lesson, not the
  batching idea itself — see §9 law 1.)
- **Walk-on-RSP as a standalone lever**: bit-exact + wedge-free since the rspq
  closed-loop-flush fix, but −1.3 modal / +0.1..0.2 heavy → default-OFF.
  Its economics change if Phase 2 lands (the kick/collect overhead amortizes
  over a much larger removed-CPU-cost block) — re-A/B it then, not before.
- **Sprite-cache lookup micro-opt**: measured at its structural floor.
  "Heavy-bucket cache% is irreducible **without fewer lookups**" — which is
  exactly what Phase 1 does.

---

## 4. Phase 0 — Instrumentation & decision data (Sonnet, 1 session, no behavior change)

Everything below gates on data we do not currently print. Extend [PERF2]
(DRAW_PERF ifdef'd, PERFCOUNT builds only; run `nm-perfsyms.sh` before
trusting any fps from an instrumented build):

1. **Tile-run statistics**: per frame — total records, unique tnums, longest
   and mean run-length of *consecutive identical* tnum, and count of records
   where `tnum == previous record's tnum`. Decides Phase 1's expected win
   (memo hit rate = 1 − uniques/records only if repeats are adjacent).
2. **Palette-switch count** per frame (consecutive records with different
   palnum) — sizes the TLUT-reload traffic and the copy-mode run lengths.
3. **Modal-tile share**: % of records with sw==16 && sh==16 && !flipx &&
   !flipy && no x-clip (the COPY-mode-eligible set).
4. **Cache miss count + PI DMA time** per frame (miss path already brackets
   `profile_dma_load`; split it per-frame into the bucket dump).
5. **RDP busy fraction**: sample DPC counters (DP_CLOCK / PIPE_BUSY /
   TMEM_BUSY) at frame boundaries and print per-frame. This is the Phase 3
   go/no-go number — today's evidence says draw cost is CPU-issue-side
   (dwait ~0), but that was measured at ~22–35 fps; at a 16.7ms budget the
   RDP's serialized load/draw ping-pong may surface as the next wall.
6. Capture a **480s bucket-matched baseline** (analyze-buckets.py, parent
   dir) on the current deliverable with the new counters, fights + at least
   one dense stage (round starts, death blows). Archive the log next to the
   ares baselines.

Gate: counters add zero cost to non-PERFCOUNT builds (ifdef'd); baseline
archived. **Decision output**: expected Phase 1 win (adjacent-repeat rate),
Phase 3 priority (RDP busy %), and the modal-tile share for copy mode.

## 5. Phase 1 — Fewer lookups: memo + resolve pass (Sonnet, small, land first)

**1a. Last-tile memo in the consume loop** (`video.c sprite_walk_consume`):
carry `{last_tnum, last_ptr}` across records; call `crom_get_sprite` only when
tnum changes. Adjacent cells in dense sprite-background strips repeat tiles —
this is the "per-strip reuse" shape flagged in the campaign notes. ~10 lines,
zero hazard (pointer stays valid within the frame: `sprite_cache_pop`'s forced
path cannot evict entries touched this tick — verify that invariant reading
`sprite_cache.c:118-125` + `:170-199` and document it in a comment; it is the
foundation Phase 2 stands on too).

**1b. (Measure-first, only if 0's data supports it) Flat resolve table**:
replace the hot-path hash with `u32 tile_ptr[crom_num_tiles]` (samsho2: 2^17
tiles × 4B = 512KB RDRAM) — resolve = one load; zero-entry = miss path (which
then does the existing insert + DMA and fills the table); eviction clears its
entry (`sprite_cache_pop` knows the key). Sparse reads don't trip the 8KB
streaming-sweep law, but 512KB must fit the RDRAM budget — audit free heap
first (`sys_get_heap_stats` or the malloc bookkeeping at boot). If it doesn't
fit, stop at 1a; do NOT build a small fronting table (that's the killed L1).

Gates: PC pixel gate (scripted run, screenshots byte-identical), 480s
bucket-matched A/B vs Phase 0 baseline, [PERF2] cache bucket delta reported
per bucket. Kill 1a if adjacent-repeat rate from Phase 0 is <10% (then 1b or
Phase 2 directly). Expected: cache% 16–26% shrinks toward the unique-tile
floor in heavy buckets; modal buckets ~flat (565 tiles, fewer repeats).

## 6. Phase 2 — One command per frame: record-driven batch draw on the RSP (Opus ucode + Sonnet harness)

Replace the per-tile `rspq_write` with: CPU resolves pointers, then issues
**one `cmd_sprite_batch` per chunk of records**; the RSP loops records
DMA'd into DMEM and emits the RDP stream itself.

Design constraints (learned the hard way — all four are load-bearing):

- **Uncached parallel array, not a cached arena.** Records (`sprwalk_recs`)
  stay as-is (produced cached by the C walk, or DMA'd by the RSP walk). The
  resolve pass writes pointers to a **separate parallel `u32 ptrs[]` through
  the uncached segment** (empty tiles → 0, RSP skips). CPU reads records
  cached, writes pointers uncached: no write-allocate sweep (the batch-arena
  killer), no cached/uncached aliasing on the same lines, and the RSP DMAs
  both arrays chunk-wise. Writeback of `sprwalk_recs` before the command when
  CPU-produced (walk already has this pattern, `video_n64.c:48-49`).
- **Chunked DMA loop in the ucode**: DMA N records (e.g. 64 recs = 512B) +
  N ptrs into DMEM, loop over them with the existing per-record body
  (palette-cache search, template patch, `RDPQ_Send` per record or small
  group — RDPQ_CMD_STAGING is small, don't batch sends past its capacity).
  The per-record body already exists and is proven; Phase 2's win is killing
  the per-command dispatch + CPU write overhead around it, so keep the body
  bit-identical first, optimize inside it later (Phase 3 edits the template).
- **Same-frame consumption only.** Kick the batch after the resolve pass of
  the same frame; pointers must not cross a `sprite_cache_tick`. The forced-
  evict-during-resolve hazard is closed by the current-tick protection
  (§5 1a); if that invariant can't be proven, add the syncpoint guard on the
  forced-evict path flagged in PLAN-OPTIMIZATION (2026-07-07 ⚠ note) instead.
- **DMEM budget**: rsp_video shares DMEM with the overlay machinery; audit
  free DMEM before sizing the record chunk (the walk ucode's DMEM map audit
  from Track D step 1 is the template).

**Verification rig (build it before the ucode, rsp_audio VERIFY pattern):**
`MVS64_BATCHDBG` dual-compute — in verify builds the ucode journals every
emitted RDP quad to an RDRAM ring; the CPU independently runs the OLD per-tile
issue logic into a shadow buffer and bit-compares streams per frame. Gate =
multi-thousand-frame ares run through dense fights, 0 mismatches (same bar as
the walk's 11.4k-frame gate). This catches palette-cache divergence, staging
overflows, and patch-offset bugs that a pixel eyeball never will.

Gates: BATCHDBG 0-mismatch soak ≥10k frames; PC pixel gate unaffected (PC
keeps the C consume path); 480s bucket-matched A/B; runtime twin knob
(`MVS64_BATCH_OFF=1` style, same-binary A/B per §9 law 4); rspq lost-wakeup
watchdog stays in place (vendored libdragon patch must be present — check
`patch --dry-run` after any libdragon reinstall). Kill criterion: if heavy
buckets are not clearly positive (≥ +1 fps) with modal ≥ flat after honest
bucket-matched A/B, revert to Phase 1 + keep the verify rig.

**Then re-open two settled questions with the new economics:**
- **Walk-on-RSP re-A/B**: with consume also on the RSP, walk→batch can fuse
  (the walk already produces the records in DMEM-adjacent RDRAM; the CPU only
  runs the resolve pass between kick and batch). The −1.3 modal verdict was
  priced against a CPU consume loop that no longer exists in this world.
- **Chunk splitting**: interleave resolve/batch chunks so the RSP draws chunk
  k while the CPU resolves k+1 (bounded by the audio offload's queue use —
  watch for the RSP contention pattern that sank walk coexistence pre-fix).

## 7. Phase 3 — RDP stream pipelining (Opus, hardware-facing)

Only after Phase 0's DPC data — and re-measure it after Phase 2, since a
faster CPU side raises RDP pressure. Three independent edits to the template
the ucode emits (`RDP_SPRITE_DRAW`):

1. **TMEM slot rotation**: lower TMEM half = 16 × 128B tile slots (TLUT owns
   0x800+). Rotate LoadBlock/SetTile target through 8+ slots per record;
   SyncLoad/SyncTile are then needed only when a slot is reused (every 8th
   record) or can rotate tile-descriptor pairs too ("descriptor/TMEM slot
   rotation", the shape already named in PLAN-OPTIMIZATION next-levers). The
   RDP then loads tile k+1's texels while drawing tile k.
2. **COPY mode for the modal tile**: records matching the modal predicate
   (Phase 0 counter 3) draw at 4px/cyc. Mode toggles need SyncPipe — emit
   them only on runs' edges (dense backgrounds are long modal runs; Phase 0
   counter 2/3 sizes the toggle count). Draw order is NEVER changed to
   lengthen runs.
3. **Palette-sorted TLUT preload**: if palette-switch count is small (≤16
   distinct/frame is guaranteed by hardware format), preload all frame
   palettes once in `render_begin_sprites` and drop the per-record palette
   cache search entirely (the fix layer already does exactly this,
   `video_n64.c:278-279`).

Gate reality check: **ares does not model RDP timing faithfully enough to
gate perf here** (already flagged in PLAN-OPTIMIZATION: "hardware-facing,
can't be gated in ares"). Correctness (pixel exactness) gates in ares +
BATCHDBG stream compare where applicable; **perf verdicts for Phase 3 come
from real-hardware flashcart runs** (user-in-the-loop: prepare a
DPC-counter-on-screen build so the user can read RDP busy % off the OSD) or
are recorded as "expected-on-hardware, unproven in ares". Do not let an ares
fps flatline kill Phase 3 items — but do let a DPC busy-% flatline.

## 8. Phase 4 — Integration & deliverable

- Re-run the full validation rig on the composed build: testsuite 125/126
  (nothing here touches m64k, run it anyway on deliverables), WAV byte-gate
  (nothing touches sound; run on deliverables), PC pixel gate, BATCHDBG soak,
  480s bucket-matched vs the Phase 0 baseline, ares full boot→menus→fight.
- Sonnet-driven BizHawk same-class boot check (boot-only — BizHawk cannot
  run libdragon-audio ROMs past early frames nor measure fps; ares is the
  perf gate). Minimize emulator windows; never steal focus.
- Decide default-ON knobs, update play-build tag (play8 → play9), record
  per-bucket table in BUILDS-<date>.md, update PLAN-OPTIMIZATION.md campaign
  header and the memory index.

---

## 9. Laws & guardrails (violating any of these has already cost a session)

1. **8KB dcache pollution law**: no new cached streaming writes or resident
   tables in the frame loop. Uncached writes pollute nothing and are
   near-optimal; sparse cached reads are fine; 7–12KB cached sweeps are not.
2. **16KB icache budget**: hot text ≤ ~13.7KB. New ucode costs IMEM not
   icache (good); new C hot-loop code must be size-audited (`nm-hotsets.sh`).
3. **Draw order is sacred**: records execute in walk order. No sorting, no
   reordering for batching/mode-run benefits. Priority overlap bugs are
   silent and content-dependent.
4. **Measurement methodology**: bucket-matched 480s+ A/Bs via
   analyze-buckets.py ONLY (never compare unbucketed fight medians);
   same-binary runtime knobs or single-initializer twin binaries (cross-binary
   deltas carry ±10-15 fps layout luck); `nm-perfsyms.sh` disjointness gate
   before trusting any PERFCOUNT fps; run-content divergence means
   content-matched windows only.
5. **Pointer-lifetime hazard**: any deferred consumption of cache pointers
   must prove the current-tick eviction protection or add the forced-evict
   syncpoint guard. A recycled pixel slot draws the wrong tile with no crash.
6. **rspq discipline**: the vendored libdragon closed-loop-flush patch
   (patches/libdragon-rspq-closed-loop-flush.patch) must be present in any
   toolchain reinstall; keep the lost-wakeup watchdog; audit DMEM maps before
   growing overlay state; sentinel-trailer polling over full `rspq_wait`
   where the walk already established the pattern.
7. **Commit discipline**: one gated increment per commit, pushed
   individually; no ROMs/BIOS in the repo; multi-line commit messages via
   `git commit -F`; pre-commit hook rewrites patch files — `patch --dry-run`
   after committing any patch.
8. **PC build parity**: the PC/SDL build keeps the C consume path and must
   stay -Werror-green (the walk split broke it once); run the WAV/pixel gates
   after any `video*.c` change — they build the PC side.

## 10. Expected value & honest bounds

- Phase 1: heavy-bucket cache% (16–26%) compresses toward the unique-tile
  floor; call it +1..+3 fps heavy, ~flat modal.
- Phase 2: removes per-record rspq/queue overhead and re-prices walk-on-RSP;
  +1..+3 fps heavy, up to +1 modal if walk fusion goes default-ON.
- Phase 3: invisible until the CPU side shrinks; on hardware it converts the
  per-tile serialized load/draw into a pipelined stream and 4x's modal-tile
  fill — this is what makes 60 *possible* on the RDP side, and is the piece
  ares cannot price.
- **(b) alone does not reach 60 fps.** It lifts the dense-scene floor (the
  dips the user actually sees) and removes draw from the critical path so
  that (a)'s dynarec gains translate into frame time. Plan for (a)+(b)
  jointly clearing the bar; keep expectations per-phase and kill fast.

## 11. Execution protocol for the implementing agents

- Read first: this file, PLAN-OPTIMIZATION.md campaign header + draw-tier
  sections, memory index entries (wpm2-full, draw-tier, walk-on-rsp,
  wave3-icache, campaign-60fps), `video.c` / `video_n64.c` / `rsp_video.S` /
  `sprite_cache.c` / `roms.c:100-170`.
- Build: WSL toolchain at /root/n64inst; ares runs via ps-ares-run.ps1
  (`-Out` must be an EXISTING directory — silent 0-byte log otherwise);
  bucket analysis via analyze-buckets.py in the repo parent dir.
- Model split: Opus owns §6 ucode + §7 template design and any hazard
  reasoning; Sonnet owns §4 counters, all measurement runs, A/B harnesses,
  BizHawk driving (per standing user preference), and mechanical C edits
  under Opus review. Emulator windows minimized, never steal focus.
- Per increment: implement → gates → bucket-matched A/B → commit+push →
  append one line to RESULTS below. If a kill criterion fires, record the
  measurement and the verdict here (a measured kill is a deliverable).

## RESULTS

(append per increment: date, commit, phase/step, gates, per-bucket delta, verdict)
