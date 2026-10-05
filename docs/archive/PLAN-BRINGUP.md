# MVS64 samsho2 N64 Bring-Up Plan (Agent Swarm Execution)

> **BLOCKER CLOSED — 2026-07-01.** The BizHawk "frame-537 wedge" was **never a bug
> in this ROM**: systematic elimination (m64k vs Musashi cores, render on/off,
> old vs guest-clock audio pump, debug channels on/off, interpreter vs dynarec,
> unthrottled vs real-time) plus a control experiment with **stock libdragon
> examples** proved BizHawk-Mupen hangs on **any libdragon ROM that initializes
> the audio subsystem** (`audioplayer.z64` hangs at the same ~9s; `fontdemo.z64`
> runs fine). BizHawk-Mupen's N64 debug interfaces are also broken (memory reads
> return zeros, `read_registers` returns a stale IPL3-era snapshot), so the old
> "68k PC reaches 0x800008" evidence below-referenced was an artifact — disregard
> it. **BizHawk cannot gate this project; the automated pre-hardware gate is
> standalone ares** (full speed, correct rdpq video, real AI audio, ISViewer
> stdout telemetry; driver script `../ps-ares-run.ps1` + `-DMVS64_AUTOINPUT`
> builds). The boot crash that DID exist on ares/hardware was fixed earlier by
> the m64k exception-path + BCLR/BSET fixes (see memory `boot-crash-frame537`);
> boot + in-match (~25fps, audio + streamed ADPCM) is healthy on ares as of
> today. See memory `bizhawk-mupen-verdict` + `fpu-free-sound-core`.


Goal: take this NeoGeo "Samurai Shodown 2" (samsho2) N64 port from "boots NeoGeo
BIOS logo then black-screen crash" to **"boots into samsho2 with full playable
control, ready for usage"** on real N64 hardware (via flashcart) as a `.z64`.

Repo root: `<workspace>\mvs64`
Build flow: native WSL toolchain at `/root/n64inst` -> produces
`build/mvs64/mvs64-samsho2.z64` (ELF `build/mvs64/mvs64-samsho2.elf`, map
`build/mvs64/mvs64-samsho2.map`).
Working PC reference (boots fully in-game from the same converted data): target
`emu` via `platform_sdl.c`.

Read this whole file before touching code. Each workstream is written so an agent
with no prior context can execute it. Paths are absolute.

---

## 0. ORIENTATION FOR ALL AGENTS

- There are TWO 68k cores. N64 build uses the **m64k MIPS-asm core** (`emu.c`
  `m64k_run`); the PC/SDL build uses the Musashi-derived `m68kcpu.c`. **The bug is
  N64-only**, so it lives in N64-only code (`platform_n64.c`, `hw_n64.S`,
  `m64k/`, and the N64-specific allocation in `sound_neogeo.c`).
- On N64, **every NeoGeo MMIO/VRAM/palette access by the 68k is a deliberate MIPS
  TLB miss** dispatched through hand-written vectors in `hw_n64.S`
  (`mvs_tlbvector`/`mvs_intvector` installed over libdragon's at
  `0x80000080`/`0x80000180`). This fault path is the single hottest and most
  fragile thing in boot. Treat it with care.
- **BizHawk cannot render this ROM's video** (libdragon rdpq microcode) — the
  screen is black even when the CPU is alive. **Do NOT use screenshots as a video
  oracle.** The valid BizHawk signals are the **VI/frame counter** and **CPU
  PC/RA registers**, plus **capture_audio** for sound.
- The empirical crash signature is: emulated machine runs to ~frame 537 (~9s @
  60fps) then either freezes at 537, or the CPU jumps to **wild PC = 0x80800124**
  (just past the top of 8MB RDRAM; `__text_end=0x8003af30`) with **RA =
  0xA40000EC** (RSP register space). Intermittent run-to-run on the same profile.
- Key symbols (from the map): `end = 0x800f2e18` (heap start), `__text_end =
  0x8003af30`. `0x80800124 = 0x80000000 + 8MB + 0x124`.
- **Never commit ROMs/BIOS.** BIOS + sfix are user-supplied. Commit individual,
  scoped changes to `github.com/mike-r129/N64-NEOGEO-Test`. No `Co-Authored-By`
  trailers, no "Generated with" lines.

---

## 1. TOP BLOCKER — THE BOOT CRASH (blocks everything)

Nothing downstream can be validated until the machine reliably boots past frame
537 into attract/title. **All other workstreams depend on this gate passing.**

### 1.1 Synthesized root cause (primary)

**FPU is executed inside the N64 TLB/MMIO exception handler, where the FPU
coprocessor is disabled (SR.CU1 cleared) and no FP context is saved.**

Mechanism, code-verified end to end:

1. The 68k sound-latch write at address `0x320000` is NOT in the asm fast-path
   inline list in `hw_n64.S` (`mvs64_asm_io_write` only inlines pbrom / watchdog
   / lspc / palette, ~`hw_n64.S:562-569`), so it falls through
   `j tlbcallhandler_standard`.
2. The standard C handler **clears SR.CU1 (FPU disable) at `hw_n64.S:216**`
   (`and t1, ~(SR_IE | SR_EXL | SR_CU1)` — this is an ACTIVE instruction; the
   adjacent `# FIXME: disabled for now` comment is **stale** and does NOT comment
   it out). It also clears SR.EXL (re-entrancy enabled) and clobbers k0/k1.
3. It then calls `write_hwio` (`hw.c:158`, case `0x320000`) ->
   `sound_write_command(val)` (`sound_neogeo.c:178-184`) -> `z80_gen_nmi` +
   `z80_run(300)` **synchronously, inside the exception handler**.
4. The Z80 runs the music driver, issuing `YM2610Write` port-outs that execute
   **double/float math** with CU1 disabled: busy-flag math (`ym2610.c:821,829`,
   `FM_BUSY_FLAG_SUPPORT=1`), `set_timers`/`TimerBase` (`ym2610.c:737-784`),
   ADPCM key-on float step `(float)(1<<ADPCM_SHIFT)*(float)freqbase/3.0`
   (`ym2610.c:2168-2169`).
5. FP with CU1 off raises a **Coprocessor-Unusable exception**. Because EXL was
   already cleared (re-entrancy on) and k0/k1 are clobbered, the nested fault
   lands in `tlbfallback` -> libdragon `inthandler` with a poisoned EPC/RA ->
   the observed **wild PC = 0x80800124 / RA = 0xA40000EC**.

This explains every empirical fact: N64-only (PC build never uses this handler);
intermittent (depends on whether the Z80 reaches an FP-touching YM write within
the 300-cycle slice of a given latch write); ~frame 537 timing (when the
BIOS/attract music driver first programs the YM2610 through the sound latch); and
**why a stock 4MB N64 ALSO crashes** (there the 7MB v.rom malloc fails and ADPCM
is disabled, yet FM/timer writes still execute FP in-handler).

The normal per-frame `sound_gen_samples()` path (`emu.c:443`) is SAFE because it
runs in ordinary C context with the FPU owned by the frame loop — that asymmetry
is exactly why FP audio synthesis works elsewhere but the latch-write path
crashes.

Contributing latent bugs (real, but not the universal root cause — fix as
hardening, see workstreams): (a) 7MB v.rom pulled fully resident via
`malloc(v_rom_size)` in `sound_neogeo.c:142-150`, violating mvs64's streaming
design and overrunning the 8MB heap; (b) unbounded YM2610 ADPCM-A/B sample reads
(`ym2610.c:2126`, `~:2508`) with no clamp to `pcmsizeA`/`pcmsizeB`. Both can
produce OOB host reads at RDRAM top but require the FP-in-handler path to fire
first to program ADPCM, and neither explains the 4MB crash.

### 1.2 RECOMMENDED FIX (apply first)

Stop running the Z80/YM2610 (which uses the FPU) synchronously from inside the
TLB MMIO exception handler. On N64, `sound_write_command()` should ONLY latch the
command and mark an NMI pending; the actual NMI + Z80 run is serviced once per
frame in normal C context (FPU owned) at the start of `sound_gen_samples()` —
the proven-correct path on the PC build. Keep PC behavior unchanged via
`#ifdef N64`.

File: `<workspace>\mvs64\sound_neogeo.c`

Exact steps:

1. Read `sound_neogeo.c`; locate the sound statics block,
   `sound_write_command()` (lines 178-184), and `sound_gen_samples()` (line 198).
2. Add `static int nmi_pending = 0;` near the other sound statics.
3. **Edit 1 — `sound_write_command()` (178-184):** keep
   `sound_code = cmd; pending_command = 1;` unconditional. Wrap the synchronous
   NMI/run so it stays only on the PC build:

   ```c
   void sound_write_command(uint8_t cmd) {
       sound_code = cmd;
       pending_command = 1;
       if (!z80_active) return;
   #ifndef N64
       z80_gen_nmi(&cpu);
       z80_run(300);
   #else
       nmi_pending = 1;   // drained in sound_gen_samples (normal C context)
   #endif
   }
   ```

4. **Edit 2 — top of `sound_gen_samples()`** (after the `!z80_active`
   early-out, ~line 203), drain the pending NMI in normal context:

   ```c
   #ifdef N64
       if (nmi_pending) { nmi_pending = 0; z80_gen_nmi(&cpu); z80_run(300); }
   #endif
   ```

   The 68k busy-wait on `sound_read_status()` (`hw.c:126`) still observes
   `result_code`, now updated on the next sound pass instead of synchronously.

5. Rebuild the `.z64` with the native WSL toolchain at `/root/n64inst`.
6. Validate per section 1.4.

This removes ALL FPU execution from the exception-handler context, fixing the
crash on both 4MB and 8MB N64.

### 1.3 FALLBACKS (in priority order)

- **Fallback 1 (handler-side FP-safe fix; use if the BIOS handshake needs a
  same-frame ACK and the defer causes a boot STALL instead of a crash):** In
  `hw_n64.S`, change line 216 `and t1, ~(SR_IE | SR_EXL | SR_CU1)` to
  `and t1, ~(SR_IE | SR_EXL)` so the FPU stays usable in the handler. Because the
  handler saves no FP context, also save/restore caller-saved FP regs
  (`$f0-$f19`) around the C call, OR verify in `m64k_asm.S` that interpreter code
  between memory ops holds no live FP state. **Lower-risk variant:** keep CU1
  enabled AND keep EXL set (also remove `SR_EXL` from the clear) to forbid
  re-entrancy, eliminating the nested-fault wild-jump path entirely.
- **Fallback 2 (enforce defer in `hw.c`):** make `write_hwio` case `0x320000`
  only store `sound_code` + pending and never touch the Z80, so any caller is
  safe regardless of which path reaches it.
- **Fallback 3 (ADPCM OOB hardening — apply regardless; see Workstream B):** clamp
  the two ADPCM dereferences in `ym2610.c`. ADPCM-A ~line 2126:
  `{ u32 _a = ch->now_addr >> 1; ch->now_data = (_a < pcmsizeA) ? pcmbufA[_a] : 0; }`.
  ADPCM-B ~line 2508:
  `{ u32 _a = adpcmb->now_addr >> 1; adpcmb->now_data = (_a < pcmsizeB) ? pcmbufB[_a] : 0; }`.
  `pcmsizeA`/`pcmsizeB` are already set to `v_rom_size` in `YM2610Init`. In-range
  samples are byte-identical; OOB becomes silence instead of a host bus error.
- **Fallback 4 (heap-budget hardening — apply regardless; see Workstream C):** in
  `sound_neogeo.c:142`, under `#ifdef N64` only do the resident v.rom malloc when
  it comfortably fits: `headroom = (0x80000000 + get_memory_size()) -
  (uintptr)sbrk(0) - 512*1024`; refuse the resident pull (leave
  `vrom_resident=NULL`, ADPCM disabled, FM/SSG intact) when
  `v_rom_size > headroom`. The NULL path is already supported.
- **Fallback 5 (diagnostic if root cause is contested):** in `hw_n64.S` replace
  the silent `j tlbfallback` escapes (lines 262, 276, 292, 330, 478) and
  `tlbfallback` itself with a stub that records `C0_CAUSE` + `EPC` + opcode to a
  known RDRAM address before jumping to `inthandler`. A **CpU cause code
  (ExcCode 11)** at the sound-latch EPC confirms the primary root cause; a
  TLB/AddrErr code points elsewhere (e.g. the incomplete indirect-jump delay-slot
  resolver `tlb_done_delayslot_indirect`, `hw_n64.S:469-493`, which only handles
  `jr ra` / `jalr t7`).

### 1.4 VALIDATION GATE (must pass before any downstream workstream is accepted)

BizHawk MCP (mupen64plus + Angrylion + cxd4 LLE — same profile used for triage):

1. Build the patched `.z64`, load it, run **>= 10 COLD boots**.
2. **PASS criteria, every cold boot:** the VI/frame counter advances past **537**
   reliably and keeps ticking to **>= 1200 (~20s)** without freezing, AND the CPU
   PC stays within mapped code (`< __text_end = 0x8003af30`; **never 0x80800124**)
   with RA **never** in RSP space (`0xA4000xxx`). Poll frame counter + PC/RA via
   MCP memory/register reads each run. Do NOT use video.
3. **Before/after:** confirm the UNpatched build still intermittently
   freezes-at-537 or jumps to 0x80800124, and the patched build does neither
   across all runs.
4. Set `MVS64_SNDDBG` to confirm the Z80 still advances (`cpu.pc` changes,
   `result_code` updates) — proves the deferred handshake works.
5. Test on **both** a 4MB config and an 8MB Expansion Pak config in BizHawk (the
   FP-in-handler fix must hold on 4MB where ADPCM is disabled too).
6. **Hardware gate:** boot `samsho2.z64` via flashcart; confirm it advances past
   the NeoGeo BIOS logo to the title/attract sequence (previous failure was logo
   -> black -> never reaching title), and that FM/SSG audio plays without a hang
   ~9s in. Run several cold power-cycles to confirm the intermittency is gone.

If the defer fix alone works, it works on stock 4MB; no Expansion Pak is required
for **boot stability** (ADPCM playback of the 7MB v.rom still needs Workstreams
B/C, but that is a sound-quality issue, not a boot blocker).

---

## 2. PARALLELIZABLE WORKSTREAMS

All workstreams below have a hard dependency on **Workstream TOP-BLOCKER passing
its BizHawk + hardware gate (1.4)** before their own DONE gate can be accepted,
unless noted otherwise. Within that constraint they can be executed concurrently.

### Workstream A — Video validation on hardware / ares

**Goal:** prove the rdpq video pipeline actually renders samsho2 correctly
(playfield + sprites + fix layer + palette), since BizHawk cannot render this
ROM.

**Files:** `video.c`, `video_n64.c`, `platform_n64.c` (frame flip:
`display_get` -> `rdpq_attach` -> `rdpq_detach_show`), `emu.c`
(`emu_render`/`plat_beginframe`/`plat_endframe`). Reference: `platform_sdl.c`.

**Tasks:**
1. Stand up an **ares** N64 emulator (or cen64) that renders libdragon rdpq
   correctly as the primary automated video oracle (BizHawk is disqualified for
   video).
2. Boot the patched `.z64`; capture framebuffer screenshots at the BIOS logo,
   attract, and in-match.
3. Diff against PC reference (`emu` target) renders of the same frames from
   identical converted data. Verify sprite layering, fix-layer text, and palette
   are correct (no color/endianness swap, no missing layers).
4. Confirm `CONFIG_FRAMESKIP_MODE=0` paths and bounded sprite counts (381
   sprites, 40x28 fix) hold; verify the `assertf` sprite-size guards never trip.
5. **Hardware confirmation:** on real N64 via flashcart, photograph
   logo/attract/title/in-match and confirm visually correct output.

**Dependencies:** TOP-BLOCKER gate (need to reach attract/title to see anything).

**DONE gate:** ares (or cen64) renders correct attract + in-match frames matching
the PC reference, AND a real-hardware photo shows correct title/attract/in-match
with no layer/palette corruption.

### Workstream B — Audio: Z80 + YM2610 FM/SSG + ADPCM streaming + N64 AI backend

**Goal:** correct, glitch-free FM/SSG music and SFX through the N64 AI backend,
plus ADPCM playback that fits RAM limits.

**Files:** `sound_neogeo.c`, `z80.c`, `ym2610/ym2610.c`, `hw.c` (sound latch),
`platform_n64.c` (AI audio FIFO -> AI backend), `roms.c` (`vrom_read`/`vrom_open`
streaming), `emu.c` (`sound_gen_samples`).

**Tasks:**
1. After the TOP-BLOCKER defer fix, confirm Z80 + YM2610 FM/SSG synthesis runs in
   normal C context and the handshake completes (`result_code` updates).
2. **ADPCM OOB hardening (Fallback 3):** clamp the two ADPCM dereferences in
   `ym2610.c` (~2126 ADPCM-A, ~2508 ADPCM-B) against `pcmsizeA`/`pcmsizeB`.
3. **ADPCM v.rom streaming within RAM limits:** replace the flat 7MB resident
   `pcmbufA/pcmbufB` with a small windowed cache backed by `vrom_read()`
   (mirror the existing PBROM/crom streaming idiom in `roms.c`) so ADPCM plays
   without holding 7MB resident. Coordinate with Workstream C on the budget.
4. **N64 AI backend validation:** the AI FIFO -> AI path in `platform_n64.c`
   compiles but was never validated. Validate via **capture_audio WAV +
   autocorrelation** (the proven PC technique): capture N seconds of attract
   music, run autocorrelation to confirm the expected musical loop/period is
   present and stable (no dropouts, no FIFO underrun/overrun, correct sample
   rate, no channel swap).
5. Verify timer/IRQ glue stays bounded (`sound_neogeo.c:49-57` ym_timer_handler;
   `frame_end` + `cpu.cyc+1000` fallback) — no audio-loop hangs.

**Dependencies:** TOP-BLOCKER gate; Workstream C (RAM budget) for the streaming
cache sizing.

**DONE gate:** BizHawk `capture_audio` of attract music passes autocorrelation
(expected loop period present, stable, no dropouts) on both 4MB and 8MB configs;
FM/SSG + ADPCM SFX audibly correct on real hardware; no audio-path hang at ~9s.

### Workstream C — 4MB vs 8MB Expansion Pak strategy

**Goal:** a single `.z64` that boots and plays on **stock 4MB** N64, and uses the
8MB Expansion Pak (when present) only to improve audio (ADPCM), never as a boot
requirement.

**Files:** `platform_n64.c:33` (`heap_top`), `sound_neogeo.c:142-150` (resident
malloc), `roms.c` (PB_ROM/P_ROM allocation + vrom streaming), `emu.c` (alloc
ordering: `rom_load` then `hw_init`->`sound_init`).

**Tasks:**
1. Implement the heap-budget guard (Fallback 4) so the 7MB resident v.rom pull
   only happens with comfortable headroom; otherwise leave `vrom_resident=NULL`
   (ADPCM disabled, FM/SSG intact). Use
   `get_memory_size()` to detect 4MB vs 8MB at runtime.
2. Document the allocation ledger: static image ends at `end=0x800f2e18`
   (~994KB); P_ROM 1MB, PB_ROM up to `get_memory_size()-3MB`, sprite/srom caches,
   framebuffers, rdpq buffers, stack. Confirm the resident/streamed decision
   leaves >=512KB reserve on both configs.
3. Decide policy: **4MB = boot + FM/SSG + streamed-or-disabled ADPCM**; **8MB =
   same + resident/larger-window ADPCM**. The game must be fully playable on 4MB
   (boot stability does not require the Pak).
4. Coordinate the streaming window from Workstream B so ADPCM works on 4MB via the
   cart-streamed path rather than a resident pull.

**Dependencies:** TOP-BLOCKER gate. Tightly coupled with Workstream B.

**DONE gate:** patched `.z64` boots past frame 537 and is playable on BOTH a 4MB
BizHawk config and an 8MB config; no allocation overruns RDRAM top; documented
allocation ledger shows >=512KB reserve in both cases.

### Workstream D — Input (P1 + P2) and saves

**Goal:** full P1 + P2 controller input mapped to NeoGeo controls; backup RAM
saves persisted on the flashcart.

**Files:** `platform_n64.c` (controller polling -> NeoGeo input registers),
`hw.c` (input port reads, BACKUP_RAM mapping ~`hw.c:281-288`), `roms.c` (backup
RAM load/save), reference mapping in `platform_sdl.c`.

**Tasks:**
1. Map N64 controller buttons to NeoGeo A/B/C/D + Start/Coin/Select for P1; add
   P2 from a second controller port. Confirm directions (d-pad/stick) and that
   diagonals + simultaneous presses register (fighting-game inputs).
2. Verify the 68k input-port reads in `hw.c` see the polled state with correct
   active-low/bit-order conventions (cross-check against `platform_sdl.c`).
3. Implement/verify BACKUP_RAM persistence to flashcart SRAM/FlashRAM so settings
   and records survive power cycle.
4. Test entering service/soft-dip menus and confirm both players can join a match.

**Dependencies:** TOP-BLOCKER gate (need to reach a playable state).

**DONE gate:** on real hardware, P1 and P2 can both navigate menus and play a full
match with all attack buttons; backup data persists across a power cycle.

### Workstream E — Performance / frameskip / timing

**Goal:** stable, full-speed (or smoothly frameskipped) playback with correct
68k/Z80/video timing on real hardware.

**Files:** `emu.c` (frame loop, `CONFIG_FRAMESKIP_MODE`), `platform_n64.c` (main
loop + VI handler), `m64k/m64k_config.h` (timing knobs), `video_n64.c`.

**Tasks:**
1. Measure per-frame time on hardware (and ares) for attract + heavy in-match
   scenes; identify whether the m64k core + rdpq + audio fit in a 60fps (or 50/60
   NTSC) budget.
2. If needed, enable a frameskip mode in `emu.c` and verify it does not desync
   audio or input.
3. Review `m64k_config.h`: `TIMING_ACCURACY=0`, `ADDRERR=0`, `PRIVERR=0`,
   `DIVBYZERO=0`. Decide whether enabling `ADDRERR` (raise clean 68k bus/address
   errors instead of raw MIPS faults) improves robustness without a perf cost.
4. Confirm no audio FIFO underrun/overrun at sustained load (ties to Workstream
   B); confirm VI handler `N64_FRAME++` cadence is correct.

**Dependencies:** TOP-BLOCKER gate; benefits from Workstreams A/B/D being
functional to exercise real load.

**DONE gate:** sustained gameplay on real hardware holds target frame cadence with
no audio dropouts and no input lag regression; frameskip (if used) is
audio/input-synced.

### Workstream F — Closed-loop BizHawk-MCP regression harness

**Goal:** an automated, repeatable harness that cold-boots the `.z64`, asserts the
boot gate + audio gate, and flags regressions on every build.

**Files:** new harness scripts (place under a `tools/` or `scripts/` dir; do NOT
create stray docs). Reuses the BizHawk MCP `bizhawk` server and samsho2 profile.

**Tasks:**
1. Script: load `build/mvs64/mvs64-samsho2.z64`, cold-boot N times, poll the
   VI/frame counter and CPU PC/RA each run; assert frame > 537 and reaching
   >= 1200, PC < `0x8003af30`, RA not in `0xA4000xxx`. Fail loudly on
   freeze-at-537 or PC=0x80800124.
2. Add `capture_audio` -> WAV -> autocorrelation check for the attract music gate
   (shared with Workstream B).
3. Parameterize 4MB vs 8MB config so both run each pass.
4. Emit a pass/fail report per build; wire it as the acceptance check every
   workstream's DONE gate references.
5. Encode the "screenshots are NOT a valid video oracle for this ROM" rule — the
   harness must never assert on framebuffer pixels via BizHawk.

**Dependencies:** TOP-BLOCKER fix exists to test against (the harness itself can be
built in parallel; its first green run depends on the fix).

**DONE gate:** harness runs headless, executes >=10 cold boots across both RAM
configs, asserts boot + audio gates, and produces a deterministic pass/fail that
the team uses as the merge gate.

---

## 3. HOW TO VALIDATE VIA BizHawk MCP

- **Server:** the registered `bizhawk` MCP server (mupen64plus core + Angrylion
  RDP + cxd4 RSP LLE — the exact profile used for triage). Load the
  **samsho2 profile** (`build/mvs64/mvs64-samsho2.z64`).
- **Boot gate:** poll the **VI / frame counter**. The machine must advance **past
  frame 537** and keep ticking to **>= 1200 (~20s)** without freezing, across
  **>= 10 cold boots**. Also read the CPU **PC** (must stay `< 0x8003af30`, never
  `0x80800124`) and **RA** (never `0xA4000xxx`). These register/counter reads —
  not pixels — are the boot oracle.
- **Sound gate:** use **`capture_audio`** to record attract music to WAV, then run
  **autocorrelation** to confirm the expected musical loop period is present and
  stable (no dropouts). This validates the N64 AI backend end to end.
- **Video is NOT testable here:** BizHawk **cannot render this ROM's libdragon
  rdpq microcode** — the screen is black even when the CPU is alive. **Screenshots
  are NOT a valid video oracle for this ROM.** Validate video on **ares/cen64 and
  real hardware** (Workstream A) only.
- **Configs:** run every check on **both** a stock **4MB** config and an **8MB
  Expansion Pak** config.
