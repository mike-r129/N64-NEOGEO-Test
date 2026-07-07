#include "platform.h"
#include "sound.h"
#include <memory.h>
#include <stdio.h>
#include <stdarg.h>

// Audio-health telemetry (see sound_neogeo.c). MVS64_SNDHEALTH enables the
// [AIPUMP] USB log in a normal human-driven build (no scripted MVS64_AUTOINPUT).
#if defined(MVS64_AUTOINPUT) || defined(MVS64_SNDHEALTH)
#define SND_HEALTH 1
#endif

// Durable on-SD telemetry sink for SND_HEALTH builds (NULL if unavailable, e.g.
// the cart's SD isn't supported or this isn't a health build). plat_log() writes
// here in addition to the debug channels; plat_audio_pump() periodically
// close+reopens it so the log survives a power-off (see notes there).
static FILE *g_sdlog = NULL;

// Telemetry logger — see platform.h. Mirrors to the SD log (if open) and to the
// libdragon debug channels (USB + emulator ISViewer, via stderr/debugf).
void plat_log(const char *fmt, ...) {
    va_list ap;
    if (g_sdlog) {
        va_start(ap, fmt);
        vfprintf(g_sdlog, fmt, ap);
        va_end(ap);
    }
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

volatile int N64_FRAME = 0;
uint32_t RSP_OVL_ID = 0;
uint32_t RSP_AUDIO_OVL_ID = 0;
uint32_t RSP_FM_OVL_ID = 0;

DEFINE_RSP_UCODE(rsp_video);
DEFINE_RSP_UCODE(rsp_audio);
DEFINE_RSP_UCODE(rsp_fm);

// Boot-time self-test of the audio RSP overlay: round-trip a small buffer
// through cmd_adpcm_test (DMA in, +1 every byte, DMA out). Proves overlay
// registration, command dispatch and both DMA directions before the ADPCM
// offload ever runs. One [RSPAUDIO] line either way.
static void rsp_audio_selftest(void) {
    static uint8_t src[64] __attribute__((aligned(16)));
    static uint8_t dst[64] __attribute__((aligned(16)));
    for (int i = 0; i < 64; i++) { src[i] = (uint8_t)(i * 3 + 7); dst[i] = 0; }
    data_cache_hit_writeback_invalidate(src, sizeof(src));
    data_cache_hit_writeback_invalidate(dst, sizeof(dst));
    rspq_write(RSP_AUDIO_OVL_ID, 0x0, PhysicalAddr(src), PhysicalAddr(dst),
               64 - 1 /* DMA_SIZE(64, 1) */);
    rspq_wait();
    data_cache_hit_invalidate(dst, sizeof(dst));
    int bad = -1;
    for (int i = 0; i < 64; i++) {
        if (dst[i] != (uint8_t)(src[i] + 1)) { bad = i; break; }
    }
    if (bad < 0)
        debugf("[RSPAUDIO] selftest OK\n");
    else
        debugf("[RSPAUDIO] selftest FAIL at %d: got %02x want %02x\n",
               bad, dst[bad], (uint8_t)(src[bad] + 1));
}

uint8_t keystate[256];

extern char end __attribute__((section (".data")));

// Audio (libdragon AI) state — see plat_audio_pump below.
static int audio_enabled = 0;
#define AI_NUM_BUFFERS 4          // AI back buffers handed to audio_init

// --- Interrupt-fed staging ring ---------------------------------------------
// The N64 AI hardware replays its last DMA buffer forever when its queue runs
// dry (there is no silence-on-underrun mode), and libdragon's AI interrupt
// fires on buffer LATCH, not on a timer: once the queue fully drains there is
// nothing left to latch, the interrupt chain dies, and nothing plays new data
// until the main loop's next audio_write_end() restarts it. A main-loop pump
// therefore cannot prevent replay: one sound_gen_samples() call can take
// 100-300ms when the Z80 is busy (the boot jingle), the queue holds ~160ms,
// and during the call nothing refills or even pushes queued buffers to the
// hardware. That drain-replay was the stuck high-pitch tone at boot/start.
//
// The fix is pull-based delivery: audio_set_buffer_callback() makes libdragon
// invoke audio_ring_cb() in AI-interrupt context every time the hardware
// needs a buffer. The callback drains this staging ring; if the ring is empty
// it pads clean silence. In callback mode libdragon refills the queue on
// every latch interrupt, so the chain is self-sustaining and the queue can
// never run dry — a stale-buffer replay is physically impossible. The main
// loop's only job is topping the ring up (plat_audio_pump below).
#define ARING_FRAMES 8192              // power of two; ~0.74s @11025Hz stereo
static int16_t aring[ARING_FRAMES * 2];        // stereo frames, cached memory
static volatile uint32_t aring_wr;             // frames produced (main loop)
static volatile uint32_t aring_rd;             // frames consumed (AI IRQ)
static volatile uint32_t aring_pad;            // frames the IRQ padded with silence

// Runs in AI-interrupt context. `buffer` is an uncached libdragon AI buffer
// (malloc_uncached) — write it with packed 32-bit stores (one stereo frame
// per store), same trick sound_neogeo.c's emit() uses, since 16-bit stores
// to uncached RDRAM are twice the transactions.
static void audio_ring_cb(short *buffer, size_t numsamples) {
    uint32_t *dst = (uint32_t *)buffer;
    uint32_t rd = aring_rd;
    uint32_t avail = aring_wr - rd;         // unsigned wraparound-safe
    uint32_t take = avail < (uint32_t)numsamples ? avail : (uint32_t)numsamples;
    for (uint32_t i = 0; i < take; i++) {
        uint32_t s = (rd + i) & (ARING_FRAMES - 1);
        dst[i] = ((uint32_t)(uint16_t)aring[s * 2 + 0] << 16) | (uint16_t)aring[s * 2 + 1];
    }
    for (uint32_t i = take; i < (uint32_t)numsamples; i++)
        dst[i] = 0;
    aring_rd = rd + take;
    aring_pad += (uint32_t)numsamples - take;
}

static void vblank_handler(void) {
    N64_FRAME++;
}

void plat_init(int audiofreq, int fps) {
#ifdef __LIBDRAGON_DEBUG_H
#ifndef MVS64_NOISVIEWER
    // Diagnostic gate (-DMVS64_NOISVIEWER skips this): some emulators
    // (BizHawk-Mupen) falsely pass ISViewer detection but never drain it, and a
    // full ISViewer buffer makes every later debugf block forever — with our
    // per-frame [EMU] print that wedges the machine at a deterministic frame.
    debug_init_isviewer();
#endif
#ifndef MVS64_NOUSBLOG
    // Same diagnostic reasoning as ISViewer above: the flashcart-USB probe can
    // false-positive under emulators, and usb_write DOES spin on cart status.
    debug_init_usblog();
#endif
#endif
#ifdef SND_HEALTH
    // Mount the flashcart's SD (FAT) and open a durable telemetry log the user can
    // read on a PC after reproducing on real hardware — no USB cable or host
    // capture tool needed. We own the FILE* (rather than libdragon's
    // debug_init_sdlog) so plat_audio_pump() can periodically close+reopen it:
    // FatFs commits a file's directory entry (its size) only on close, so without
    // that the log shows up empty/truncated on a PC after a power-off. Requires a
    // cart whose SD libdragon can drive (64drive / EverDrive-64 X-series, v3).
    if (debug_init_sdfs("sd:/", -1)) {
        g_sdlog = fopen("sd:/mvs64log.txt", "w");
        if (g_sdlog) {
            setvbuf(g_sdlog, NULL, _IOLBF, 0);   // line-buffered: flush each '\n'
            debugf("[SDLOG] writing to sd:/mvs64log.txt\n");
            plat_log("[SDLOG] mvs64 SND_HEALTH telemetry log start\n");
        } else {
            debugf("[SDLOG] fopen sd:/mvs64log.txt FAILED\n");
        }
    } else {
        debugf("[SDLOG] debug_init_sdfs FAILED (cart SD not supported?)\n");
    }
#endif
    debugf("MVS64\n");
    register_VI_handler(vblank_handler);

    char *heap_top = (char*)0x80000000 + get_memory_size() - 0x10000;
    char *heap_end = &end;
    debugf("heap [%p - %p = %d]\n", heap_top, heap_end, heap_top-heap_end);

	controller_init();
    // NOTE: there seems to be a bug in libdragon display library when ANTIALIAS_OFF
    // is used. Some RDP register is not configured correctly and the display is
    // corrupted on NTSC consoles.
	display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, ANTIALIAS_RESAMPLE);
    dfs_init(DFS_DEFAULT_LOCATION);
    rdpq_init();
    // rdpq_debug_start();

    // Register our custom RSP overlays into the RSP queue engine
    RSP_OVL_ID = rspq_overlay_register(&rsp_video);
    RSP_AUDIO_OVL_ID = rspq_overlay_register(&rsp_audio);
    RSP_FM_OVL_ID = rspq_overlay_register(&rsp_fm);
    rsp_audio_selftest();

    audio_init(audiofreq, AI_NUM_BUFFERS);
    // ORDER MATTERS: register the callback BEFORE the priming write. The AI
    // raises its interrupt when it LATCHES a buffer (start of DMA); whoever
    // services that interrupt must queue the next buffer right then or the
    // chain dies and the interrupt never fires again. The primer's own latch
    // IRQ can arrive within microseconds of audio_write_end(), so if the
    // callback isn't installed yet, that IRQ finds nothing to queue and the
    // whole audio path is dead from boot (total silence — this exact bug).
    audio_set_buffer_callback(audio_ring_cb);
    // One write kick-starts the chain: audio_write_end() runs libdragon's
    // audio_callback synchronously, which (in callback mode) immediately
    // pulls two buffers through audio_ring_cb — the ring is empty so they
    // are clean silence — and hands them to the AI. Every latch IRQ after
    // that refills through the callback: self-sustaining forever.
    audio_write_begin();
    audio_write_end();
    (void)fps;
}

// --- Audio (libdragon Audio Interface) -------------------------------------
// Delivery is handled entirely by audio_ring_cb() in AI-interrupt context —
// see the ring comment block above. plat_audio_pump() only keeps the ring
// topped up; it never touches the AI queue, so a slow sound_gen_samples()
// call can no longer let the hardware run dry and replay a stale buffer.
// sound_gen_samples() is rate-agnostic (N samples == N/AUDIO_RATE seconds of
// Z80+YM2610 time), so music plays at correct pitch AND tempo regardless of
// video fps. When the Z80 is inactive it writes clean silence.
void plat_enable_audio(int enable) {
    audio_enabled = enable;
}

void plat_audio_pump(void) {
    if (!audio_enabled) return;
#ifdef SND_HEALTH
    uint32_t _t0 = TICKS_READ();
#endif
    const int buflen = audio_get_buffer_length();
    const int n = buflen <= 2048 ? buflen : 2048;
    // Ring headroom kept staged ahead of the ISR. Two buffers (~80ms @11kHz)
    // rides out main-loop scheduling jitter at 28-30fps; combined with the
    // <=2 buffers libdragon keeps latched in the AI pipeline, total latency
    // matches the old push design (~160ms).
    const uint32_t TARGET_LEAD = 2u * (uint32_t)n;

    // Overload detection is read straight from the ring's silence-pad counter:
    // audio_ring_cb() only increments it when it truly had nothing to hand the
    // AI, so this is ground truth for "generation fell behind real time" —
    // unlike the old guest-clock debt heuristic, it also sees drains that
    // happen in the middle of one long sound_gen_samples() call. sound_silent
    // is set from the PREVIOUS pass's streak so one slow frame isn't muted.
    static uint32_t last_pad;
    static int underrun_streak;
    uint32_t pad_now = aring_pad;
    uint32_t starved = pad_now - last_pad;
    last_pad = pad_now;
    sound_silent = (underrun_streak >= 1);
    if (starved > 0) {
        if (underrun_streak < 1000) underrun_streak++;
    } else {
        underrun_streak = 0;
    }

    // Overload governor: in sustained starvation, generation is slower than
    // real time and the output is zeros anyway (sound_silent), so grinding
    // through more Z80/YM work per pass only steals frame budget from video —
    // the boot jingle's Z80-bound phase dropped the machine to ~4fps this way.
    // Process at most ONE buffer per pass while overloaded; the machine slows
    // uniformly instead of audio starving video. (The starved frames are never
    // "repaid": the ISR already padded that time with silence, so we just
    // resume generating from now.)
    int pass_budget = (underrun_streak >= 1) ? 1 : AI_NUM_BUFFERS;

    int filled = 0, discarded = 0;
    static int16_t stage[2048 * 2];

    // Top the ring up toward TARGET_LEAD. The ISR consumes exactly n frames
    // per callback, so lead is always a multiple of n.
    extern uint32_t profile_snd;
    while (filled < pass_budget) {
        uint32_t lead = aring_wr - aring_rd;
        if (lead + (uint32_t)n > TARGET_LEAD) break;   // topped up
        uint32_t snd_t0 = TICKS_READ();
        sound_gen_samples(stage, n);
        profile_snd += TICKS_DISTANCE(snd_t0, TICKS_READ());
        uint32_t wr = aring_wr;
        for (int i = 0; i < n; i++) {
            uint32_t s = (wr + i) & (ARING_FRAMES - 1);
            aring[s * 2 + 0] = stage[i * 2 + 0];
            aring[s * 2 + 1] = stage[i * 2 + 1];
        }
        MEMORY_BARRIER();      // publish samples before advancing the index
        aring_wr = wr + n;
        filled++;
    }

    // Safety valve for a broken emulated AI (BizHawk-Mupen class): if the ring
    // reader (the AI interrupt) stops advancing while the ring sits full, the
    // fill loop above never runs again and the 68k<->Z80 handshake starves —
    // the old frame-537 wedge. Detect it with a wall-clock accumulator that
    // resets whenever the reader moves; on real hardware and ares the reader
    // is IRQ-driven and always advances, so this is inert there. When it
    // trips, advance the sound machine into a discard buffer (handshake and
    // timers stay alive; the audio is lost, but so is the platform's AI).
    {
        static uint32_t last_t, last_rd;
        static uint64_t wall_acc;
        static int wall_due;
        uint32_t now = TICKS_READ();
        if (last_t == 0) last_t = now;
        uint32_t rd_now = aring_rd;
        if (rd_now != last_rd) {
            wall_due = 0;
            wall_acc = 0;
        } else {
            wall_acc += (uint64_t)TICKS_DISTANCE(last_t, now) * (uint32_t)audio_get_frequency();
            wall_due += wall_acc / TICKS_PER_SECOND;
            wall_acc %= TICKS_PER_SECOND;
            if (wall_due > 4 * n) wall_due = 4 * n;
        }
        last_t = now;
        last_rd = rd_now;
        if (filled == 0 && wall_due >= 2 * n) {
            uint32_t snd_t0 = TICKS_READ();
            sound_gen_samples(stage, n);
            profile_snd += TICKS_DISTANCE(snd_t0, TICKS_READ());
            wall_due -= n;
            discarded++;
        }
    }

#ifdef SND_HEALTH
    // Delivery-health probe (USB/ISViewer).
    //   buffers/60 = ring fills per 60 passes; ~= real-time rate at speed.
    //   starved = frames the ISR padded with silence this window. Boot-jingle
    //             and load-hitch windows may show bursts (audible as brief
    //             silence — never a stuck tone); must be ~0 in steady state.
    //   discard>0 = the AI interrupt stopped consuming (broken emulator AI —
    //             never on real HW or ares). If this fires, delivery is dead.
    //   lead    = ring fill in frames at print time (expect n..2n).
    //   sndms   = avg wall-clock ms synthesising audio per pump; >=16.7ms
    //             means real-time synthesis can't keep up (the perf wall).
    {
        static int pumps = 0, total = 0, deep = 0, maxf = 0, disc = 0;
        static uint32_t starvedsum = 0;
        static uint32_t tacc = 0;
        tacc += TICKS_DISTANCE(_t0, TICKS_READ());
        total += filled; if (filled > maxf) maxf = filled;
        disc += discarded;
        starvedsum += starved;
        if (underrun_streak) deep++;
        if ((pumps++ % 60) == 0) {
            plat_log("[AIPUMP] pass=%d buffers/60=%d maxfill=%d underruns=%d discard=%d starved=%u lead=%u sndms=%.2f silent=%d\n",
                     pumps, total, maxf, deep, disc, starvedsum,
                     (uint32_t)(aring_wr - aring_rd),
                     (float)tacc * 1000.f / (float)TICKS_PER_SECOND / 60.f, sound_silent);
            total = 0; maxf = 0; tacc = 0; disc = 0; deep = 0; starvedsum = 0;
            // Commit the SD log to the card so it survives a power-off. FatFs only
            // writes the directory entry (file size) on close, so we close+reopen
            // in append mode here; until this runs the file can appear empty on a
            // PC. Cheap at this cadence (~1/s at speed; ~1/15s during the slow boot).
            if (g_sdlog) {
                fclose(g_sdlog);
                g_sdlog = fopen("sd:/mvs64log.txt", "a");
                if (g_sdlog) setvbuf(g_sdlog, NULL, _IOLBF, 0);
            }
        }
    }
#endif
}

int plat_poll(void) {
	controller_scan();
    struct controller_data ckeys = get_keys_pressed();

    memset(keystate, 0, sizeof(keystate));

    if (ckeys.c[0].up)      { keystate[PLAT_KEY_P1_UP] = 1; }
    if (ckeys.c[0].down)    { keystate[PLAT_KEY_P1_DOWN] = 1; }
    if (ckeys.c[0].left)    { keystate[PLAT_KEY_P1_LEFT] = 1; }
    if (ckeys.c[0].right)   { keystate[PLAT_KEY_P1_RIGHT] = 1; }
    if (ckeys.c[0].A)       { keystate[PLAT_KEY_P1_A] = 1; }
    if (ckeys.c[0].B)       { keystate[PLAT_KEY_P1_B] = 1; }
    if (ckeys.c[0].C_down)  { keystate[PLAT_KEY_P1_C] = 1; }
    if (ckeys.c[0].C_right) { keystate[PLAT_KEY_P1_D] = 1; }
    if (ckeys.c[0].start)   { keystate[PLAT_KEY_P1_START] = 1; }
    // Z inserts a coin/credit (the MVS has no coin without this); C-up = select.
    if (ckeys.c[0].Z)       { keystate[PLAT_KEY_COIN_1] = 1; }
    if (ckeys.c[0].C_up)    { keystate[PLAT_KEY_P1_SELECT] = 1; }

#ifdef MVS64_AUTOINPUT
    // Deterministic input that REPLAYS input_play.txt (the PC headless script)
    // frame-for-frame, so the N64 (m64k) and PC (Musashi) sound traces are
    // directly comparable: identical inputs -> identical game state -> the 68k
    // command streams should match unless a 68k-core difference diverges them.
    // Frame-counted (not wall-clock), so it is identical regardless of N64 fps.
    {
        static int af = 0;
        int f = af++;
        #define AW(a,b)   (f >= (a) && f < (b))
        if (AW(540,560) || AW(580,600))                 keystate[PLAT_KEY_COIN_1]  = 1; // 2 coins
        if (AW(660,668) || AW(820,828) || AW(1000,1008) ||
            AW(1200,1208) || AW(1450,1458) ||
            AW(1700,1708) || AW(1950,1958))             keystate[PLAT_KEY_P1_START] = 1; // start
        // character select: wiggle + confirm
        if (f >= 1300 && f < 2000) {
            if ((f % 80) < 10)                          keystate[PLAT_KEY_P1_A]     = 1;
            if ((f % 80) >= 30 && (f % 80) < 40)        keystate[PLAT_KEY_P1_RIGHT] = 1;
        }
        // IN MATCH: mash attacks + movement continuously (reproduce combat SFX load)
        if (f >= 2200) {
            int m = f % 24;
            if (m < 8)                                  keystate[PLAT_KEY_P1_A]     = 1;
            else if (m < 14)                            keystate[PLAT_KEY_P1_B]     = 1;
            else if (m < 20)                            keystate[PLAT_KEY_P1_C]     = 1;
            if (m < 12)                                 keystate[PLAT_KEY_P1_RIGHT] = 1;
        }
        if ((f % 120) == 0) plat_log("[AUTOINPUT] frame=%d\n", f);
    }
#endif

    return 1;
}

void plat_enable_video(int enable) {

}

void plat_save_screenshot(const char *fn) {

}

uint8_t *g_screen_ptr;
int g_screen_pitch;

void plat_beginframe(void) {
    surface_t *rdp_disp = display_get();

	g_screen_ptr = rdp_disp->buffer;
	g_screen_pitch = 320*2;

    rdpq_attach(rdp_disp, NULL);
	rdpq_set_scissor(0, 0, 320, 224);
}

void plat_endframe(void) {
	rdpq_detach_show();
}
