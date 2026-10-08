# Samurai Shodown II on Nintendo 64

> **This project is now maintained in the [mvs64 fork](https://github.com/mike-r129/mvs64).**
> This repo is the original bulk of the performance work, which specifically
> targeted Samurai Shodown II: sound, RSP audio, the 68000 fast paths and the
> draw path were built and measured here against samsho2 alone. Once samsho2
> was stable, the changes were prepared for the fork of
> [MVS64](https://github.com/rasky/mvs64), where they are being generalized for
> other games and readied to offer upstream. This repo is kept as the
> samsho2-specific record (history, measurements, and the release27 build) and
> no longer receives new development. Please use the fork for current work.
>
> This work also predates
> [mike-r129/N64-Z80](https://github.com/mike-r129/N64-Z80), a hand-written
> MIPS assembly Z80 interpreter for the VR4300. The Z80 here is the C core
> (superzazu/z80), so the more efficient assembly core, and the extra
> performance it brings, is not part of this repo.
>
> The three libdragon rspq fixes found here (the lost-wakeup window in
> `rspq_flush_internal`, the highpri wedge from a stale `SIG_HIGHPRI_REQUESTED`,
> and application-sized lowpri buffers) live on in
> [mike-r129/libdragon](https://github.com/mike-r129/libdragon), a fork of
> upstream libdragon, as PRs #1-#3. The files in `patches/` are the originals.

This is a fork of [MVS64](https://github.com/rasky/mvs64), Giovanni Bajo's
NeoGeo emulator for the N64. It targets one game, Samurai Shodown II
(`samsho2`, NGH-063), played on a real console from a flash cart with full
sound.

Upstream MVS64 had no sound, and most games ran below full speed. This fork
adds the NeoGeo sound hardware (Z80 + YM2610), moves audio synthesis onto the
RSP, and tunes the 68000 core and the renderer for samsho2.

## Status

- **Playable on real hardware** from a flash cart, with music, sound effects
  and voices. Audio runs at 11,025 Hz. With the 8 MB Expansion Pak the C-ROM
  tile cache gets 4,096 slots (512 KB); without it, 1,280.
- **Framerate:** not a locked 60 fps. On a real console, fights usually run
  around 50 fps; the heaviest fight scenes measured so far drop to about 42
  (41.9 fps with ~785 sprite tiles on screen). See
  [Performance](#performance) below.
- **Current build:** release27 (2026-10-05), the first build after the
  cleanup. Hardware testing so far shows no problems.
- **Long sessions:** the failures seen in long hardware sessions (sound dying
  after 17.9 minutes, sound lost mid-round, an RSP crash after 30+ minutes)
  are fixed. Hardware sessions since then, including a full-day soak on
  2026-10-03, have run without sound loss or crashes.
- **Other games:** only samsho2 is tested. Games that upstream MVS64 booted may
  still build, but they have not been tested with sound or the performance
  work. The 68000 idle-skip list is tuned for samsho2, so other games also lose
  that speedup until their own wait loops are added. Making this work general
  for other games, and offering it back to upstream MVS64, happens in a
  separate fork ([mike-r129/mvs64](https://github.com/mike-r129/mvs64)), where
  this work is now maintained; this repo stays focused on samsho2.

## Performance

In-fight framerate across the project. ares figures are medians over
scripted fight windows; hardware figures are read from the performance
overlay on a real N64.

| Date | Milestone | In-fight fps |
| --- | --- | --- |
| 2026-07-02 | First build with full sound (Z80 + YM2610 all on the CPU) | 8.5 (ares) |
| 2026-07-02 | Smaller YM2610 tables, channel-major FM synthesis | 17.5 (ares) |
| 2026-07-06 | 68000 idle-skip fixed (it had never fired) | 21.4 (ares) |
| 2026-07-08 | FM and ADPCM synthesis moved to the RSP; audio pump reordered | 35.6 (ares) |
| 2026-09-23 | Z80 spin fast-forward, Z80 page map, C-ROM direct table | 45.9 (ares, all content) |
| 2026-10-03 | rspq buffer sizing, draw-path cuts, Z80 and 68000 trims | 51.8 (ares) |
| 2026-10-04 | Triple buffering, 2-word sprite command, 4,096-slot tile cache, cache-layout pinning | heaviest fights 36.7 -> 41.9 (hardware) |

Where a heavy hardware fight frame goes now (41.9 fps, 785 tiles, ms per
frame): 68000 9.8, sound 5.7, drawing on the CPU 5.4, waiting for a display
buffer 2.4. The RDP is busy 16.9 ms of the frame, close to the CPU's own
total, so the remaining gains have to come from both sides. The largest open
lever on the CPU is the 68000 dynarec, which is correct but needs wider
template coverage before it pays off; on the RDP, the time per sprite tile
is far above its fill cost and not yet explained.
[PLAN-OPTIMIZATION.md](PLAN-OPTIMIZATION.md) has every measurement.

The 2026-10-04 cleanup removed about 16,700 lines of dead experiments and
diagnostics. Each step was checked for identical memory layout or identical
pixels, and the release build kept its speed.

## What this fork adds

| Area | Changes |
| --- | --- |
| Sound | Z80 sound CPU and YM2610 (FM, SSG, ADPCM-A/B), integer-only. ADPCM samples stream from the cart. Audio reaches the N64 through an interrupt-fed ring, so it can never loop a stale buffer. |
| RSP audio | ADPCM decode and FM synthesis run on the RSP (`rsp_audio.S`, `rsp_fm.S`), with channel state kept on the RSP between chunks. |
| 68000 (m64k) | Inline fast paths for hot opcodes, fused DBF copy/fill loops, inline stores to the video RAM ports, a working idle-skip, and an experimental dynarec (off by default). The core's hot data and code are pinned to fixed cache sets. |
| Video | Empty-tile skipping for sprites and the fix layer, a direct C-ROM tile table, a 2-word RSP sprite command, RDP load pipelining with COPY mode for plain 16x16 tiles, and triple buffering. |
| Reliability | Three libdragon rspq patches (`patches/`), wrap-safe timing, automatic restart of stalled RSP audio, and on-screen overlays for hardware tests. |

## Building

[BUILDING.md](BUILDING.md) covers the WSL toolchain, the **required** libdragon
patches, the BIOS files and flashing. With the toolchain in place:

    make mvs64 ROM=/abs/path/to/samsho2.zip BIOS=/abs/path/to/uni-bios_4_0.rom EXTRA_DEFINES=-DMVS64_QUIET

This writes `mvs64-samsho2.z64`. ROMs and BIOS files are not included and must
never be committed; see BUILDING.md §3.

| Build | `EXTRA_DEFINES` | Use |
| --- | --- | --- |
| Release | `-DMVS64_QUIET` | Normal play; per-frame logging compiled out |
| Performance overlay | `-DMVS64_QUIET -DMVS64_PERFOSD` | Release plus the frame-time overlay described below |
| Audio-health test | `-DMVS64_QUIET -DMVS64_SNDOSD` | Release plus the audio-health overlay and a health log on the SD card (`sd:/mvs64log.txt`) |
| Debug | none | Per-frame debug logging; slower on hardware |

Make switches turn single optimizations back off for A/B tests:
`WP_OFF=1` (whole-pump RSP audio), `ADPCM_CPU=1 WP_OFF=1` (all ADPCM on the
CPU), `FP_OFF=1` (68000 fast paths), `BLOCKOPS_OFF=1` (fused DBF loops) and
`PORTSTORE_OFF=1` (inline video-port stores). `FRAMESKIP=n` lets the emulator
skip drawing up to n frames in a row when it falls behind, so game speed holds
instead of slowing down; it is off by default.

## Experimental features (off by default)

These are in the tree but switched off, because each one measured no faster
than the default path, or slower. They stay because they work, they're
tested, and some may pay off with more work or on other games. All figures are
from ares; [PLAN-OPTIMIZATION.md](PLAN-OPTIMIZATION.md) has the full
measurements.

### 68000 dynarec

    make mvs64 ROM=... BIOS=... DYNREC_ON=1

The dynarec translates hot 68000 code into native MIPS code instead of
interpreting it one instruction at a time.

- When the interpreter jumps to an address with no translated block, the
  address is queued. At the end of the time slice, the translator builds a
  block from per-instruction templates, up to the next branch, in a 256 KB code
  buffer.
- Blocks jump straight to each other once both ends are translated, and keep
  the guest's address registers in host registers while they run.
- Each block checks the remaining cycle budget before it starts, so interrupts
  land on the same instruction as in the interpreter. Memory-mapped I/O goes
  through the same trap handler the interpreter uses.
- Only ROM code is translated. samsho2 never runs code from RAM or the banked
  P-ROM window.

It matches the interpreter bit for bit: per-frame state hashes agree over
thousands of frames, and a differential test rig checks every template. But
only about 10% of guest instructions run translated, because many instructions
have no template yet, so fps is flat. Wider template coverage is the next step.
`DYNSTAT_ON=1` prints translation coverage, and `TRCRC_ON=1` prints the
per-frame state hash used to check bit-exactness. Template notes are in
[m64k/DYNREC-PHASE2-TEMPLATES.md](m64k/DYNREC-PHASE2-TEMPLATES.md).

### Removed experiments

Predecoded 68000 dispatch (5.5-5.9 fps slower), the wave-3 fast paths (2.9-3.9
fps slower), the RSP sprite walk (~1.3 fps slower), batched sprite drawing
(0.8-1.6 fps slower) and synchronous per-chunk FM on the RSP (slower than the
whole-pump offload) were measured, found slower, and removed from the tree.
Their code is in git history and their numbers are in
[PLAN-OPTIMIZATION.md](PLAN-OPTIMIZATION.md) and
[PLAN-DRAW-RDP.md](PLAN-DRAW-RDP.md).

## Reading the performance overlay

Performance-overlay builds draw ten lines below the HUD, refreshed every 60
frames. Times are milliseconds per emulated frame, averaged over the window;
a frame at 60 fps has 16.7 ms.

| Line | Meaning |
| --- | --- |
| `F d g` | Frames drawn per second, then emulated (game-speed) frames per second; they differ only with `FRAMESKIP` |
| `M m S s` | 68000 time (including I/O and Z80 catch-up), sound time (Z80 + YM2610) |
| `V v W w` | Drawing time on the CPU, then time spent waiting for a free display buffer (large when the RDP is the bottleneck) |
| `B b L l` | Inside V: palette conversion at frame start, fix layer |
| `R r` | Inside V: sprites |
| `Q q E e` | Inside R: C-ROM tile lookups, RSP command issue; the rest of R is the sprite walk |
| `C c N n` | Inside Q: tile-cache misses read from the cart, then sprite tiles drawn per frame |
| `G g H h` | Tiles drawn in RDP COPY mode, then tiles that would be COPY but are flipped |
| `A a X x` | Whole frame, then the part of it no other line accounts for |
| `P p T t` | RDP pipe-busy and TMEM-busy time per drawn frame (real hardware only; ares reads 0) |

## Reading the audio-health overlay

Audio-health builds draw five lines in the top-left corner, refreshed every
60 frames.

| Line | Meaning | Healthy |
| --- | --- | --- |
| `F` | Emulated fps | Varies by scene |
| `D wamh n` | RSP audio offloads marked dead (whole-pump, ADPCM, FM, fallback hatch), then the death count | `D 0000 0` |
| `K k R r W w` | Lost-wakeup watchdog kicks, offload revives, RSP queue wedges recovered by the libdragon patch | `K 0 R 0 W 0` |
| `S s n` | Silence governor engaged, then silence pads per second | `S 0 0` |
| `L n C n` | Buffered audio lead (samples), then samples played per second | `L` about two buffers (~80 ms), `C` about 11025 |

If sound dies but `C` stays near 11025, delivery still works and generation
stopped: check `D` and `S`. If `C` drops to 0, the audio interrupt chain died.
A nonzero `W` means the libdragon safety net caught an RSP queue wedge and play
continued.

## PC build

MVS64 includes a PC build of the same emulation core. This fork uses it as the
reference for bit-exact audio and pixel checks. A bug that shows up on N64 but
not on PC is in the N64 backend; otherwise it is in the NeoGeo emulation.

    make pctest
    ./emu /abs/path/to/samsho2.n64/

`samsho2.n64/` is the folder of preprocessed ROMs that `make mvs64` creates
next to `samsho2.zip`.

## Documentation

| File | Contents |
| --- | --- |
| [BUILDING.md](BUILDING.md) | Toolchain, libdragon patches, BIOS, flashing |
| [docs/archive/PLAN.md](docs/archive/PLAN.md) | The original port plan |
| [PLAN-OPTIMIZATION.md](PLAN-OPTIMIZATION.md) | Performance log; the top entry is the current state |
| [WHOLEPUMP-DESIGN.md](WHOLEPUMP-DESIGN.md) | Design of the RSP audio offload |
| [PLAN-DRAW-RDP.md](PLAN-DRAW-RDP.md) | Draw pipeline plan and results |
| [docs/archive/PLAN-BRINGUP.md](docs/archive/PLAN-BRINGUP.md) | Bring-up notes; the archive also holds the July build logs |
| [m64k/README.md](m64k/README.md) | The 68000 core |
| [tools/README.md](tools/README.md) | Measurement and correctness-gate scripts |

## Credits and licenses

- **MVS64** and the **m64k** 68000 core: Giovanni Bajo (rasky), MIT
  ([LICENSE](LICENSE)).
- **Z80 core:** [superzazu/z80](https://github.com/superzazu/z80) by Nicolas
  Allemand, MIT ([z80.LICENSE](z80.LICENSE)).
- **YM2610:** the MAME FM sound core by Jarek Burczynski and Tatsuyuki Satoh,
  by way of NJ's pspmvs ([ym2610/LICENSE.mame](ym2610/LICENSE.mame)).
- **Musashi** 68000 core (PC build, fallback core and test oracle): Karl
  Stenerud.
- **[libdragon](https://github.com/DragonMinded/libdragon):** the N64 SDK this
  is built on.
