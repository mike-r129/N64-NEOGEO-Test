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
`BLOCKOPS_OFF=1` turn single optimizations back off for A/B tests.
`DYNREC_ON=1` enables the experimental 68000 dynarec.

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
