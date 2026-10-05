# Samurai Shodown II on Nintendo 64

This is a fork of [MVS64](https://github.com/rasky/mvs64), Giovanni Bajo's
NeoGeo emulator for the N64. It targets one game, Samurai Shodown II
(`samsho2`, NGH-063), played on a real console from a flash cart with full
sound.

Upstream MVS64 had no sound, and most games ran below full speed. This fork
adds the NeoGeo sound hardware (Z80 + YM2610), moves audio synthesis onto the
RSP, and tunes the 68000 core and the renderer for samsho2.

## Status

- **Playable on real hardware** from a flash cart, with music, sound effects
  and voices. Needs the 8 MB Expansion Pak. Audio runs at 11,025 Hz.
- **Framerate:** not a locked 60 fps. The current build (sndfix6) runs at a
  median 45.9 fps over scripted content windows in ares; busy fight scenes run
  lower.
- **Long sessions:** the latest builds fix the failures seen in long hardware
  sessions: sound dying after 17.9 minutes, sound lost mid-round, and an RSP
  crash after 30+ minutes. A long hardware soak of the latest build is in
  progress.
- **Other games:** only samsho2 is tested. Games that upstream MVS64 booted may
  still build, but they have not been tested with sound or the performance
  work. The 68000 idle-skip list is tuned for samsho2, so other games also lose
  that speedup until their own wait loops are added.

## What this fork adds

| Area | Changes |
| --- | --- |
| Sound | Z80 sound CPU and YM2610 (FM, SSG, ADPCM-A/B), integer-only. ADPCM samples stream from the cart. Audio reaches the N64 through an interrupt-fed ring, so it can never loop a stale buffer. |
| RSP audio | ADPCM decode and FM synthesis run on the RSP (`rsp_audio.S`, `rsp_fm.S`), with channel state kept on the RSP between chunks. |
| 68000 (m64k) | Inline fast paths for hot opcodes, fused DBF copy/fill loops, a working idle-skip, and an experimental dynarec (off by default). |
| Video | Empty-tile skipping for sprites and the fix layer, a direct CROM tile table, and RDP load pipelining with COPY mode for plain 16x16 tiles. |
| Reliability | Two libdragon rspq patches (`patches/`), wrap-safe timing, automatic restart of stalled RSP audio, and an on-screen audio-health overlay for hardware tests. |

## Building

[BUILDING.md](BUILDING.md) covers the WSL toolchain, the **required** libdragon
patches, the BIOS files and flashing. With the toolchain in place:

    make mvs64 ROM=/abs/path/to/samsho2.zip BIOS=/abs/path/to/uni-bios_4_0.rom EXTRA_DEFINES=-DMVS64_QUIET

This writes `mvs64-samsho2.z64`. ROMs and BIOS files are not included and must
never be committed; see BUILDING.md §3.

| Build | `EXTRA_DEFINES` | Use |
| --- | --- | --- |
| Release | `-DMVS64_QUIET` | Normal play; per-frame logging compiled out |
| Hardware test | `-DMVS64_QUIET -DMVS64_SNDOSD` | Release plus the audio-health overlay and a health log on the SD card (`sd:/mvs64log.txt`) |
| Debug | none | Per-frame debug logging; slower on hardware |

Make switches such as `WP_OFF=1`, `ADPCM_CPU=1`, `FP_OFF=1` and
`BLOCKOPS_OFF=1` turn single optimizations back off for A/B tests. The
experimental features below have their own switches.

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

### Other experiments

| Feature | Enable with | What it does | Result |
| --- | --- | --- | --- |
| Predecoded dispatch | `PD_ON=1` | Scaffold that dispatches through pre-decoded records per guest address instead of the opcode table | 5.5-5.9 fps slower: the record stream thrashes the 8 KB data cache. Closed |
| RSP sprite walk | `EXTRA_DEFINES=-DMVS64_WALK_RSP` | The RSP walks the sprite tables and builds the visible-tile list the CPU draws from | Bit-exact over 11,400 frames, but about 1.3 fps slower in typical scenes |
| Batched sprite draw | `EXTRA_DEFINES=-DMVS64_SPRBATCH` | One RSP command per 64 sprite tiles instead of one per tile | Pixel-identical, but 0.8-1.6 fps slower |
| Synchronous RSP FM | `WP_OFF=1 RSPFM=1` | FM synthesis on the RSP one chunk at a time, with the CPU waiting on each chunk | Bit-exact, but slower: 26.3 → 21.8 fps with two channels on the RSP, 12.6 with all four. The default whole-pump offload replaced it |

## Reading the audio-health overlay

Hardware-test builds draw five lines in the top-left corner, refreshed every
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
| [PLAN.md](PLAN.md) | The original port plan |
| [PLAN-OPTIMIZATION.md](PLAN-OPTIMIZATION.md) | Performance log; the top entry is the current state |
| [WHOLEPUMP-DESIGN.md](WHOLEPUMP-DESIGN.md) | Design of the RSP audio offload |
| [PLAN-DRAW-RDP.md](PLAN-DRAW-RDP.md) | Draw pipeline plan and results |
| [PLAN-BRINGUP.md](PLAN-BRINGUP.md) | Bring-up notes |
| [m64k/README.md](m64k/README.md) | The 68000 core |

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
