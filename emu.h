#ifndef EMU_H
#define EMU_H

// Frameskipping mode:
//   0 - never frameskip, game might slowdown
//   1 - 30 FPS mode (draw one frame every two)
//   2 - auto mode. Game will frameskip as much as necessary to keep up with 60 FPS
#define CONFIG_FRAMESKIP_MODE            0

#include <stdint.h>
#include <stdbool.h>

// 68k core selection (defined via the Makefile CFLAGS so it is visible in all
// TUs incl. the Musashi headers): USE_M64K -> fast MIPS-asm m64k interpreter
// (N64 default). When unset on N64 (MUSASHI=1 build), the portable Musashi core
// is used instead (proven on the PC build; slower) as a fallback while m64k
// opcode bugs are investigated. USE_M64K gates only the 68k-core integration;
// the libdragon platform stays under N64.

// MVS64_QUIET: silence per-frame debugf tracing for release builds. On N64
// every debugf is an ISViewer/USB write (PI transactions) — several lines per
// frame cost real frame time on hardware and flood emulator logs.
#ifdef MVS64_QUIET
#define framef(...) ((void)0)
#else
#define framef(...) debugf(__VA_ARGS__)
#endif

#define MVS_CLOCK         24000000
#define M68K_CLOCK_DIV    2
#define FPS        		  60

// Audio output sample rate (Hz). The AI plays at this rate and the YM2610 is
// generated for it (sound_neogeo.c keys cyc_budget off this), so the two MUST
// match — define it in one place. On N64, generating YM2610 FM is slower than
// real time at 44.1kHz (the gngeo core is ~0.7x realtime here), so a lower rate
// trades audio bandwidth for the CPU headroom needed to sustain real-time,
// glitch-free playback alongside the 68k. Override via EXTRA_DEFINES=-DMVS64_AUDIO_RATE=N.
#ifndef MVS64_AUDIO_RATE
#ifdef N64
// Real-time YM2610 FM synthesis is the dominant audio cost on N64 and scales
// with the sample rate, so the N64 build defaults to a lower rate to keep the
// framerate up. Measured (samsho2, ares, with the Z80 idle-skip): 44100~5fps,
// 22050~6.5fps, 11025~13fps steady. 11025 keeps NeoGeo FM music clearly
// recognizable while preserving playable speed. Override per-build with
// EXTRA_DEFINES=-DMVS64_AUDIO_RATE=N.
#define MVS64_AUDIO_RATE  11025
#else
#define MVS64_AUDIO_RATE  44100
#endif
#endif
#define FRAME_CLOCK       (MVS_CLOCK / FPS)
#define LINE_CLOCK        (FRAME_CLOCK / 264)
#define WATCHDOG_PERIOD   3244030

#define MAX_EVENTS 8

typedef uint32_t (*EmuEventCb)(void *cbarg);

typedef struct {
    int64_t clock;
    EmuEventCb cb;
    void *cbarg;
    bool current;
} EmuEvent;

int emu_add_event(int64_t clock, EmuEventCb cb, void *cbarg);
void emu_change_event(int event_id, int64_t clock);
int64_t emu_clock(void);
int64_t emu_clock_frame(void);
uint32_t emu_pc(void);

void emu_cpu_reset(void);
void emu_cpu_irq(int level, bool state);

#endif
