# Building mvs64-samsho2.z64

This documents the **verified** native build (no Docker needed), as proven on
Windows 11 + WSL2 Ubuntu‑24.04. The ROM build pipeline works end‑to‑end today;
the only thing required to make the ROM **playable** is a real NeoGeo BIOS (§3).

## 1. One-time: build the libdragon toolchain (native, ~15 min)

You need a `mips64-elf` GCC toolchain + libdragon. The Docker route
(`libdragon-docker`) is the upstream-documented option; the native route below
is what this fork was validated with.

```sh
# In WSL (Ubuntu). Requires build deps:
sudo apt-get install -y build-essential texinfo wget curl flex bison \
    libpng-dev file xz-utils bzip2 libzstd-dev libsdl2-dev pkg-config

# Build + install the toolchain from a libdragon checkout (e.g. libdragon-trunk):
export N64_INST="$HOME/n64inst"
cd <libdragon>/tools && JOBS=$(nproc) ./build-toolchain.sh   # builds binutils+gcc+newlib
cd ..
# REQUIRED: the vendored rspq stability patches, in this order. Without them
# the RSP queue can wedge on real hardware (lost wakeup; highpri wedge after
# 30+ min of play). The first patch has scratch-path headers, hence the
# explicit target file.
patch src/rspq/rspq.c < <repo>/patches/libdragon-rspq-closed-loop-flush.patch
patch -p1 < <repo>/patches/libdragon-rspq-highpri-wedge.patch
# Lets the ROM size the lowpri command buffers (weak hooks; behaviour is
# upstream's unless the app defines them, as platform_n64.c does).
patch -p1 < <repo>/patches/libdragon-rspq-lowpri-size.patch
./build.sh                                                   # builds + installs libdragon
```

A ROM built against the patched library reports highpri-wedge recoveries as
`hpwedge=` in `[AIPUMP]` and `W` on the SNDOSD overlay (healthy: 0).

This installs `mips64-elf-gcc` (gcc 14.2 validated), `mkdfs`, `n64tool`,
`n64elfcompress`, and `include/n64.mk` under `$N64_INST`.

> **libdragon drift note:** current libdragon‑trunk no longer ships the
> `cycles.h` that `m64k.c` expects. This fork reconstructs it as
> `m64k/cycles.h` (standard M68000 exception‑cycle counts), so **no libdragon
> pinning is needed**. Remaining drift is only non‑fatal deprecation warnings
> (`get_keys_pressed`, `sprite->bitdepth`).

## 2. Build the ROM

```sh
export N64_INST="$HOME/n64inst"
export PATH="$PATH:$N64_INST/bin"
cd <repo>                       # this directory
make mvs64 ROM=/abs/path/to/samsho2.zip BIOS=/abs/path/to/<bios>
# -> produces mvs64-samsho2.z64 (valid .z64, big-endian magic 80371240)
```

`make`'s `all:` builds the host `mvsmakerom` first (so it isn't cross‑compiled),
runs it to convert the ROM set into `samsho2.n64/` (p/b/c/s/v/m.rom + p.bios/
s.bios), packs it with `mkdfs`, and links the final `.z64`.

Verified output with a placeholder BIOS: `mvs64-samsho2.z64`, 26.7 MB
(includes the 7 MB `v.rom` ADPCM data), magic `80371240`.

## 3. The BIOS (required for a *playable* ROM)

`mvsmakerom` needs a **NeoGeo system BIOS + `sfix.sfix`** in **one folder**:

- a BIOS program ROM whose name matches an `is_bios` prefix — `sp-`, `sp1.`,
  `uni-bios`, `asia-`, `japan-`, `sm1.` (e.g. `sp-s2.sp1` or `uni-bios.rom`);
- **`sfix.sfix`** (128 KB system‑fix ROM) in that same folder.

Pass the BIOS file as `BIOS=<that file>`; `sfix.sfix` is auto‑read beside it.

> These are **SNK‑copyrighted** and are NOT included here. Obtain them legally:
> **UniBIOS** (https://unibios.free.fr) for the program ROM, an `sfix.sfix` from
> a BIOS set you own, or a dump from your own NeoGeo/MVS/AES hardware.
> **Never commit ROMs/BIOS** (enforced by `.gitignore`).

## 4. Flashing

Copy `mvs64-samsho2.z64` to your flash cart (EverDrive‑64 X7 via SD, or 64drive)
and boot on a real N64. It is a native `.z64` (big‑endian) — **not** `.x64`
(which is a Commodore‑64/VICE format, not an N64 format). The 8 MB Expansion
Pak is recommended: with it the C-ROM tile cache gets 4,096 slots instead of
1,280.

## 5. PC reference build (for debugging / A‑B)

```sh
make pctest      # -> ./emu  (SDL; runs the same emulation core off samsho2.n64/)
./emu /abs/path/to/samsho2.n64/
```

Per upstream: a bug present in the PC build is an emulation‑layer bug; a bug
only on N64 is a backend bug. See `docs/archive/PLAN.md` §11 for the full validation strategy
(SDL + BizHawk MCP).
