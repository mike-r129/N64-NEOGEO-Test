#include "platform.h"
#include "sound.h"
#include <memory.h>
#include <stdio.h>
#include <stdarg.h>

// Audio-health telemetry (see sound_neogeo.c). MVS64_SNDHEALTH enables the
// [AIPUMP] USB log in a normal human-driven build (no scripted MVS64_AUTOINPUT).
// MVS64_SNDOSD additionally draws the audio-health numbers on screen (see
// plat_endframe) so a real console diagnoses sound loss with no cable or SD
// card pull — it implies the SD/USB telemetry too.
#if defined(MVS64_AUTOINPUT) || defined(MVS64_SNDHEALTH) || defined(MVS64_SNDOSD)
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

#ifdef MVS64_RSPWP
// rspq lost-wakeup watchdog kicks (see plat_audio_pump); file-scope so the
// SNDOSD overlay / [AIPUMP] telemetry can report it.
static uint32_t rspwp_wedge_kicks;
#endif
// Highpri-wedge recoveries performed inside libdragon's RSP wait loops by the
// vendored patch (patches/libdragon-rspq-highpri-wedge.patch). Weak so the
// ROM still links against an unpatched toolchain (then it always reads 0).
extern uint32_t __rspq_wedge_recoveries __attribute__((weak));
static inline uint32_t rspq_wedge_recoveries(void) {
    return &__rspq_wedge_recoveries ? __rspq_wedge_recoveries : 0;
}
// rspq lowpri command-buffer size, read by libdragon's rspq_init through the
// vendored patch (patches/libdragon-rspq-lowpri-size.patch); an unpatched
// toolchain ignores both and keeps upstream's 0x200 words. A fight frame
// issues ~10 KB of video commands while the RSP is often busy with a highpri
// audio burst, so with 2 x 2 KB the CPU blocks in rspq_next_buffer instead of
// returning to 68k emulation. The allocation is fixed at the max so A/B
// twins (MVS64_RSPQ_LOWPRI_WORDS) keep an identical heap; both pinned to
// .data so the twins' binaries differ only in the initializer.
#ifndef MVS64_RSPQ_LOWPRI_WORDS
#define MVS64_RSPQ_LOWPRI_WORDS 0x1000
#endif
int __rspq_lowpri_buffer_words __attribute__((section(".data"))) = MVS64_RSPQ_LOWPRI_WORDS;
int __rspq_lowpri_alloc_words  __attribute__((section(".data"))) = 0x1000;
// Consecutive pump passes that observed ISR silence-padding (the overload
// governor input, see plat_audio_pump); file-scope for SNDOSD.
static int underrun_streak;

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

static int det_fps = 60;   // guest fps for MVS64_DET_AUDIO's fixed quantum
void plat_init(int audiofreq, int fps) {
    det_fps = fps > 0 ? fps : 60;
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
	extern int mvs64_display_buffers;   // 3 (see plat_detach_show), or 2
	display_init(RESOLUTION_320x240, DEPTH_16_BPP, mvs64_display_buffers, GAMMA_NONE, ANTIALIAS_RESAMPLE);
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

// Copy one generated buffer into the staging ring (stereo frames).
static void aring_push(const int16_t *src, int n) {
    uint32_t wr = aring_wr;
    for (int i = 0; i < n; i++) {
        uint32_t s = (wr + i) & (ARING_FRAMES - 1);
        aring[s * 2 + 0] = src[i * 2 + 0];
        aring[s * 2 + 1] = src[i * 2 + 1];
    }
    MEMORY_BARRIER();      // publish samples before advancing the index
    aring_wr = wr + n;
}

static int16_t stage[2048 * 2];

#ifdef MVS64_PERFCOUNT
uint32_t perf_snd_pub;         // publish cost this frame (blocking finish +
                               // ring copy) = the profile_snd share that is
                               // NOT sound_gen_samples. Read+reset by emu.c.
                               // Stays defined without RSPWP (always 0 then).
#endif

#ifdef MVS64_RSPWP
// Cross-pump output deferral (full WP-M2, WHOLEPUMP-DESIGN.md addendum).
// sound_gen_samples() now returns with its tail RSP chunks still in flight;
// publishing `stage` to the ring is deferred to the NEXT pump entry, so the
// RSP burst deficit drains for free during the inter-pump 68k/draw window
// instead of being paid as a blocking wait at pump end. The fill policy
// counts the pending buffer as staged lead (it is published before the ISR
// could ever need it), and a safety valve publishes immediately whenever
// less than one AI callback of PUBLISHED lead remains.
void YM2610_wp_finish(void);
static int wp_pending_n;       // frames generated into stage, not yet published
static void wp_publish(void) {
    extern uint32_t profile_snd;
    uint32_t t0;
    if (!wp_pending_n) return;
    t0 = TICKS_READ();
    YM2610_wp_finish();        // usually instant: the RSP had the whole window
    profile_snd += TICKS_DISTANCE(t0, TICKS_READ());
#ifdef MVS64_DET_AUDIO
    // det-quantum generation can outrun the wall-rate reader: drop instead
    // of overwriting unread frames (host-side loss only; see pump comment).
    if (aring_wr - aring_rd + (uint32_t)wp_pending_n <= ARING_FRAMES)
        aring_push(stage, wp_pending_n);
#else
    aring_push(stage, wp_pending_n);
#endif
    wp_pending_n = 0;
#ifdef MVS64_PERFCOUNT
    perf_snd_pub += TICKS_DISTANCE(t0, TICKS_READ());
#endif
}
#endif

void plat_audio_pump(void) {
    if (!audio_enabled) return;

#if defined(MVS64_RSPWP) && defined(MVS64_WP_DEATHTEST)
    // Revive-cycle gate rig, thrash edition (2026-08-30 permanent-loss
    // postmortem): 8 forced dead-latches ~10s apart starting at pass 3600
    // (~60s at speed), i.e. every kill lands <30s after the previous
    // revive. The old bookkeeping exhausted its 6-attempt budget with no
    // restore path and stayed dead for the session; the fixed bookkeeping
    // must ride the thrash (parole retries) and, once the kills stop,
    // return to sustained health (off=0, budget restored) by run end.
    {
        extern void YM2610_offload_testkill(void);
        static int dt_passes;
        dt_passes++;
        if (dt_passes >= 3600 && dt_passes <= 7800
            && (dt_passes - 3600) % 600 == 0)
            YM2610_offload_testkill();
    }
#endif

#ifdef MVS64_RSPWP
    // rspq lost-wakeup watchdog. The whole-pump audio offload issues bursts
    // of commands separated by idle gaps (~500 halt/wake edges per second),
    // which hits a race in the rspq kernel's going-idle path: the RSP halts
    // just as the CPU sets SIG_MORE, and stays halted with work pending
    // (observed three times as "RSP CRASH ... display_get wait loop timed
    // out", RSP halted at kernel PC 0x18 with SIG_MORE set — on both the
    // highpri and lowpri queues). Halted+SIG_MORE is legal only for the
    // few-cycle window inside libdragon's own wake sequence, so if it
    // persists across two pump calls (~30-70ms, still well under
    // display_get's 200ms panic), clear the halt: that resumes the kernel's
    // idle loop, which re-checks SIG_MORE and proceeds. A spurious kick in
    // the benign window is a no-op (the CPU's own clear-halt follows).
    {
        volatile uint32_t * const SP_STATUS_REG =
            (volatile uint32_t *) 0xA4040010;
        static int wedged_seen;
        uint32_t st = *SP_STATUS_REG;
        if ((st & 1u /*HALTED*/) && (st & (1u << 14) /*SIG_MORE*/)) {
            if (wedged_seen++) {
                *SP_STATUS_REG = 1u /*SP_WSTATUS_CLEAR_HALT*/;
                wedged_seen = 0;
                rspwp_wedge_kicks++;
                debugf("[RSPWP] rspq lost-wakeup kicked (%lu)\n",
                       (unsigned long) rspwp_wedge_kicks);
            }
        } else {
            wedged_seen = 0;
        }
    }
#endif
#ifdef MVS64_RSPWP
    // Publish the previous pump's deferred buffer first: its chunks have had
    // the whole inter-pump window to complete, so the blocking finish inside
    // is normally a no-op poll.
    wp_publish();
#endif
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
    uint32_t pad_now = aring_pad;
    uint32_t starved = pad_now - last_pad;
    last_pad = pad_now;
    sound_silent = (underrun_streak >= 1);
    if (starved > 0) {
        if (underrun_streak < 1000) underrun_streak++;
    } else {
        underrun_streak = 0;
    }
#ifdef MVS64_DET_AUDIO
    // GATE-BUILD DETERMINISM: the stock fill loop below tops the ring up
    // toward TARGET_LEAD, but aring_rd advances in the AI interrupt at
    // REAL VR4300 rate — so the NUMBER of sound_gen_samples() passes per
    // guest frame depends on wall speed, and the Z80/YM phase the 68k
    // observes through its 0x320000 reply polls forks at marginal
    // handshake moments (the frame-377/3153 cross-profile TRCRC
    // attractors; also July's "run-content divergence" note). Under this
    // knob: generate EXACTLY one guest-frame's worth of samples per pump
    // (accumulator carries the remainder), ignore the ring level and the
    // wall-driven governor, and pin sound_silent. The ring may over/
    // underrun — host-side only; gate builds are not for listening.
    sound_silent = 0;
    underrun_streak = 0;
    static uint32_t det_carry;
    uint32_t det_want = det_carry + (uint32_t)audio_get_frequency();
    int det_samples = (int)(det_want / (uint32_t)det_fps);
    det_carry = det_want % (uint32_t)det_fps;
#endif

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
#ifdef MVS64_DET_AUDIO
    pass_budget = 1000000;              // the det quantum is the only limit
#endif

    // Top the ring up toward TARGET_LEAD. The ISR consumes exactly n frames
    // per callback, so lead is always a multiple of n.
    extern uint32_t profile_snd;
    while (filled < pass_budget) {
        uint32_t lead = aring_wr - aring_rd;
#ifdef MVS64_RSPWP
        lead += (uint32_t)wp_pending_n;   // deferred buffer counts as staged
#endif
#ifdef MVS64_DET_AUDIO
        if (filled * n >= det_samples) break;   // fixed guest quantum
        (void)lead;
#else
        if (lead + (uint32_t)n > TARGET_LEAD) break;   // topped up
#endif
#ifdef MVS64_RSPWP
        wp_publish();          // free the staging buffer before reusing it
#endif
        uint32_t snd_t0 = TICKS_READ();
        sound_gen_samples(stage, n);
        profile_snd += TICKS_DISTANCE(snd_t0, TICKS_READ());
#ifdef MVS64_RSPWP
        wp_pending_n = n;      // defer the publish to the next pump entry
#else
#ifdef MVS64_DET_AUDIO
        if (aring_wr - aring_rd + (uint32_t)n <= ARING_FRAMES)
            aring_push(stage, n);       // else drop: host-side loss only
#else
        aring_push(stage, n);
#endif
#endif
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
#ifdef MVS64_RSPWP
            wp_publish();      // free the staging buffer before reusing it
#endif
            uint32_t snd_t0 = TICKS_READ();
            sound_gen_samples(stage, n);
#ifdef MVS64_RSPWP
            // discard buffer: complete in-flight chunks before stage reuse,
            // but never publish (the platform's AI is broken here anyway)
            YM2610_wp_finish();
#endif
            profile_snd += TICKS_DISTANCE(snd_t0, TICKS_READ());
            wall_due -= n;
            discarded++;
        }
    }

#ifdef MVS64_RSPWP
    // Safety valve: with less than one full AI callback of PUBLISHED lead,
    // the deferred buffer cannot wait for the next pump entry (a slow frame
    // would starve the ISR into a silence pad). Publish now — this pays the
    // residual RSP deficit exactly when we're already behind, which is the
    // old (pre-deferral) behavior.
    if (wp_pending_n && aring_wr - aring_rd < (uint32_t)n)
        wp_publish();
#endif

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
#ifdef MVS64_RSPWP
            uint32_t kicks = rspwp_wedge_kicks;
#else
            uint32_t kicks = 0;
#endif
            plat_log("[AIPUMP] pass=%d buffers/60=%d maxfill=%d underruns=%d discard=%d starved=%u lead=%u sndms=%.2f silent=%d off=%lx deaths=%lu revives=%lu kicks=%lu hpwedge=%lu\n",
                     pumps, total, maxf, deep, disc, starvedsum,
                     (uint32_t)(aring_wr - aring_rd),
                     (float)tacc * 1000.f / (float)TICKS_PER_SECOND / 60.f, sound_silent,
                     (unsigned long)YM2610_offload_flags(),
                     (unsigned long)ym_off_deaths, (unsigned long)ym_off_revives,
                     (unsigned long)kicks, (unsigned long)rspq_wedge_recoveries());
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
#if defined(MVS64_RSPWP) && defined(MVS64_RSPQ_WEDGETEST)
    // Fault-injection rig for the rspq highpri wedge (the 2026-09-23 hardware
    // crash; see patches/libdragon-rspq-highpri-wedge.patch). Every 300th pump
    // from pass 900 on, let this pump's audio burst drain, then raise a stale
    // SIG_HIGHPRI_REQUESTED: exactly the state the upstream highpri_begin race
    // leaves behind. (Injected while segments are still queued, their own
    // WRITE_STATUS would consume it: the first rig, 2026-09-23, stuck only 1
    // of 5 times.) The kernel then re-enters highpri at the empty end of the
    // stream and sleeps there with SIG_HIGHPRI_RUNNING set, starving lowpri.
    // Unpatched toolchain: the next lowpri wait times out into the crash
    // screen (reproduced in ares: the exact hardware signature, rspq.c:951,
    // STATUS 0x1403). Patched: the wait-loop watchdog recovers it ([AIPUMP]
    // hpwedge= / SNDOSD W count up, play continues).
    {
        static uint32_t wt_passes, wt_injected;
        if (++wt_passes >= 900 && (wt_passes % 300) == 0) {
            rspq_highpri_sync();
            MEMORY_BARRIER();
            *SP_STATUS = SP_WSTATUS_SET_SIG4;   // = SET_SIG_HIGHPRI_REQUESTED
            MEMORY_BARRIER();
            wt_injected++;
            debugf("[WEDGETEST] stale HIGHPRI_REQUESTED injected (%lu)\n",
                   (unsigned long) wt_injected);
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

#if defined(MVS64_FBCRC) || defined(MVS64_DPCOSD) || defined(MVS64_SNDOSD)
static surface_t *fbcrc_disp;
#endif

#if defined(MVS64_DPCOSD) || defined(MVS64_SNDOSD) || defined(MVS64_PERFOSD)
// PLAN-DRAW-RDP Phase 3 hardware instrument: ares/paraLLEl-RDP does not
// model the DPC counters (they read 0 there), so the Phase 3 perf verdict
// comes from a real console. This build draws the RDP numbers on screen:
//   F ff.f   emulated fps (wall-clock, 30-frame window)
//   P pp.pp  RDP PIPE_BUSY ms per frame (60fps budget = 16.7ms)
//   T tt.tt  RDP TMEM_BUSY ms per frame (texture-load serialization)
//   B bb     PIPE_BUSY as % of DP_CLOCK (RDP duty cycle)
// Glyphs: 4x6 bitmap font drawn 2x through the uncached segment onto the
// finished frame (detach_wait first), top-left corner.
static const uint8_t dpcosd_font[27][6] = {
	{0x6,0x9,0x9,0x9,0x9,0x6}, {0x2,0x6,0x2,0x2,0x2,0x7}, // 0 1
	{0x6,0x9,0x1,0x2,0x4,0xF}, {0xE,0x1,0x6,0x1,0x1,0xE}, // 2 3
	{0x2,0x6,0xA,0xF,0x2,0x2}, {0xF,0x8,0xE,0x1,0x1,0xE}, // 4 5
	{0x6,0x8,0xE,0x9,0x9,0x6}, {0xF,0x1,0x2,0x2,0x4,0x4}, // 6 7
	{0x6,0x9,0x6,0x9,0x9,0x6}, {0x6,0x9,0x9,0x7,0x1,0x6}, // 8 9
	{0x0,0x0,0x0,0x0,0x0,0x2},                            // .
	{0xE,0x9,0xE,0x8,0x8,0x8}, {0xF,0x2,0x2,0x2,0x2,0x2}, // P T
	{0xE,0x9,0xE,0x9,0x9,0xE}, {0xF,0x8,0xE,0x8,0x8,0x8}, // B F
	{0x0,0x0,0x0,0x0,0x0,0x0},                            // space
	{0xE,0x9,0x9,0x9,0x9,0xE}, {0x9,0xA,0xC,0xC,0xA,0x9}, // D K
	{0xE,0x9,0x9,0xE,0xA,0x9}, {0x7,0x8,0x6,0x1,0x1,0xE}, // R S
	{0x8,0x8,0x8,0x8,0x8,0xF}, {0x6,0x9,0x8,0x8,0x9,0x6}, // L C
	{0x9,0x9,0x9,0xF,0xF,0x9},                            // W
	{0x9,0xF,0xF,0x9,0x9,0x9}, {0x9,0x9,0x9,0x9,0x6,0x6}, // M V
	{0x6,0x9,0x9,0xF,0x9,0x9}, {0x9,0x9,0x6,0x6,0x9,0x9}, // A X
};
static void dpcosd_text(uint16_t *fb, int stride_px, int x, int y, const char *s) {
	for (; *s; s++, x += 10) {
		int g;
		if (*s >= '0' && *s <= '9') g = *s - '0';
		else if (*s == '.') g = 10;
		else if (*s == 'P') g = 11;
		else if (*s == 'T') g = 12;
		else if (*s == 'B') g = 13;
		else if (*s == 'F') g = 14;
		else if (*s == 'D') g = 16;
		else if (*s == 'K') g = 17;
		else if (*s == 'R') g = 18;
		else if (*s == 'S') g = 19;
		else if (*s == 'L') g = 20;
		else if (*s == 'C') g = 21;
		else if (*s == 'W') g = 22;
		else if (*s == 'M') g = 23;
		else if (*s == 'V') g = 24;
		else if (*s == 'A') g = 25;
		else if (*s == 'X') g = 26;
		else continue;
		for (int r = 0; r < 6; r++) {
			uint8_t bits = dpcosd_font[g][r];
			for (int c = 0; c < 4; c++) {
				if (!(bits & (8 >> c))) continue;
				uint16_t *p = fb + (y + r*2) * stride_px + x + c*2;
				p[0] = p[1] = p[stride_px] = p[stride_px+1] = 0xFFFF;
			}
		}
	}
}
#endif

#ifdef MVS64_PERFOSD
// Hardware perf overlay that does NOT stall the pipeline: unlike SNDOSD/
// DPCOSD (detach_wait, then CPU-draw onto the finished frame, which
// serializes CPU and RDP every frame), the text is rendered into a small
// RGBA16 texture only when the numbers change (every 60 drawn frames) and
// blitted each frame with one copy-mode rectangle queued at the end of the
// frame's own RDP work. Two textures alternate, so the one being rewritten
// was last read 60 frames ago. Per guest frame, averaged over the window:
//   F dd.d gg.g   drawn fps, game-speed (emulated) fps
//   M mm.m S ss.s 68k ms (incl. MMIO and Z80 catch-up in sound commands),
//                 sound ms (Z80 + YM2610 pump)
//   V vv.v W ww.w draw CPU ms (video_render issue + frame end, without the
//                 wait), and ms spent in display_get waiting for a free
//                 buffer (= the RSP/RDP still finishing older frames: the
//                 frame is RDP-bound when W is large)
//   B bb.b L ll.l inside V: render_begin (palette snapshot/convert issue),
//                 fix layer
//   R rr.r K kk.k inside V: sprites total, and the part of it waiting for
//                 the RSP sprite walk
//   C cc.c        inside R: C-ROM tile-cache miss reads from the cart
//   A aa.a X xx.x whole guest frame ms (CPU wall time incl. all waits), and
//                 A minus M+S+V+W: the part no other line accounts for
//   P pp.p T tt.t RDP pipe-busy / TMEM-busy ms per drawn frame (DPC
//                 counters; real hardware only, ares reads 0)
#define POSD_W 160
#define POSD_H 120
static surface_t posd_surf[2];
static int posd_cur = -1;
static uint32_t posd_wait;          // ticks in display_get, current guest frame
static uint32_t posd_acc_all, posd_acc_m68k, posd_acc_snd, posd_acc_draw, posd_acc_wait;
static uint32_t posd_acc_n;
// Draw split (video.c DRAW_PERF_COARSE): render_begin, sprites (incl. the
// RSP walk wait and C-ROM miss reads), fix layer.
static uint32_t posd_acc_beg, posd_acc_spr, posd_acc_fix, posd_acc_walk, posd_acc_miss;
// emu.c main loop, once per guest frame.
void plat_perf_frame(uint32_t all, uint32_t m68k, uint32_t snd, uint32_t draw) {
	extern uint32_t perf_dr_begin, perf_dr_sprites, perf_dr_fix, perf_dr_wwait, perf_dr_missticks;
	posd_acc_all += all; posd_acc_m68k += m68k; posd_acc_snd += snd;
	posd_acc_draw += draw; posd_acc_wait += posd_wait;
	posd_acc_beg += perf_dr_begin; posd_acc_spr += perf_dr_sprites; posd_acc_fix += perf_dr_fix;
	posd_acc_walk += perf_dr_wwait; posd_acc_miss += perf_dr_missticks;
	perf_dr_begin = perf_dr_sprites = perf_dr_fix = perf_dr_wwait = perf_dr_missticks = 0;
	posd_wait = 0;
	posd_acc_n++;
}
static uint32_t posd_ms10(uint32_t ticks, uint32_t n) {   // ms*10 per frame
	return n ? (uint32_t)((uint64_t)ticks * 10000 / ((uint64_t)TICKS_PER_SECOND * n)) : 0;
}
static void posd_render(char lines[][32], int nlines) {
	int nxt = posd_cur < 0 ? 0 : posd_cur ^ 1;
	surface_t *s = &posd_surf[nxt];
	if (!s->buffer) *s = surface_alloc(FMT_RGBA16, POSD_W, POSD_H);
	{   // opaque black backdrop (RGBA5551 0x0001: alpha bit set) for legibility
		uint16_t *px = (uint16_t *)s->buffer;
		for (int i = 0; i < s->stride / 2 * POSD_H; i++) px[i] = 0x0001;
	}
	for (int i = 0; i < nlines; i++)
		dpcosd_text((uint16_t *)s->buffer, s->stride / 2, 4, 4 + i * 14, lines[i]);
	data_cache_hit_writeback(s->buffer, s->stride * POSD_H);
	posd_cur = nxt;
}
#endif

// Display buffering. With 2 buffers, display_get blocks until the buffer on
// screen is released at the NEXT vblank, so at 35-45 fps the CPU idled ~7.5
// ms per fight frame on hardware (PERFOSD W) - vsync quantization, not RDP
// load (RDP busy ~10 ms/frame). A third buffer removes that wait.
// Two buffers also fenced every frame: display_get could only return after
// the previous frame's RSP+RDP work was done, which is what made it safe for
// the next frame to reuse sprite-cache slots, the palette snapshot and
// PALETTE_RAM_EMU. With 3 buffers that fence is explicit: each frame ends
// with rdpq_detach_cb (show + count the frame done, under the DP interrupt)
// and plat_beginframe waits for that count before video_render touches any
// shared state. The CPU spends ~13 ms in the 68k/sound before it renders,
// so the previous frame's ~10 ms of RDP work is normally long finished.
// mvs64_display_buffers is a .data word: the 2-buffer twin is
// -DMVS64_DISPLAY_BUFFERS=2 (uses the plain rdpq_detach_show path).
#ifndef MVS64_DISPLAY_BUFFERS
#define MVS64_DISPLAY_BUFFERS 3
#endif
int mvs64_display_buffers __attribute__((section(".data"))) = MVS64_DISPLAY_BUFFERS;
static surface_t *cur_disp;
static uint32_t frames_issued;
static volatile uint32_t frames_done;
static void frame_done_cb(void *arg) {   // DP interrupt: RDP finished the frame
	display_show((surface_t *)arg);
	frames_done++;
}
static void plat_detach_show(void) {
	if (mvs64_display_buffers > 2) {
		frames_issued++;
		rdpq_detach_cb(frame_done_cb, cur_disp);
	} else {
		rdpq_detach_show();
	}
}

void plat_beginframe(void) {
#ifdef MVS64_PERFOSD
    uint32_t posd_t0 = TICKS_READ();
#endif
    surface_t *rdp_disp = display_get();
    // Previous frame fence (see plat_detach_show): its RDP work, and so all
    // of its RSP commands, must be done before this frame reuses cache slots
    // and palette buffers. Trivially true in 2-buffer and draining builds.
    while ((int32_t)(frames_issued - frames_done) > 0) {}
#ifdef MVS64_PERFOSD
    posd_wait += TICKS_DISTANCE(posd_t0, TICKS_READ());
#endif
#ifdef MVS64_FBCRC_PIPE
    // Pixel gate for the pipelined (3-buffer) path: after the fence, the
    // previous frame's buffer is final and not yet reused - hash it (same
    // FNV-1a over 320x224 as MVS64_FBCRC, keyed by that frame's g_frame) and
    // compare with a drained 2-buffer FBCRC baseline (both DET_AUDIO).
    {
        extern int g_frame;
        static surface_t *prev_disp;
        static int prev_key = -1;
        if (prev_disp && prev_key >= 0) {
            uint32_t crc = 0x811C9DC5u;
            const uint8_t *row = (const uint8_t *)UncachedAddr(prev_disp->buffer);
            for (int y = 0; y < 224; y++) {
                const uint32_t *p = (const uint32_t *)row;
                for (int x = 0; x < 320*2/4; x++) { crc ^= p[x]; crc *= 16777619u; }
                row += prev_disp->stride;
            }
            plat_log("[FBCRC] %lu %08lx\n", (unsigned long)prev_key, (unsigned long)crc);
        }
        prev_disp = rdp_disp;
        prev_key = g_frame;
    }
#endif
    cur_disp = rdp_disp;

	g_screen_ptr = rdp_disp->buffer;
	g_screen_pitch = 320*2;
#if defined(MVS64_FBCRC) || defined(MVS64_DPCOSD) || defined(MVS64_SNDOSD)
	fbcrc_disp = rdp_disp;
#endif

    rdpq_attach(rdp_disp, NULL);
	rdpq_set_scissor(0, 0, 320, 224);
}

void plat_endframe(void) {
#ifdef MVS64_RDPDBG
	// PLAN-DRAW-RDP Phase 3 recon rig: log the validated RDP stream for a
	// few frames so we can read the EXACT SetOtherModes words the rdpq
	// mode engine composes (render_begin_sprites standard mode, fix-layer
	// copy mode). The ucode's mid-pass mode switches must reproduce the
	// standard word bit-for-bit. Also doubles as a validator pass over the
	// raw ucode stream (0 errors expected). Frame windows: ~900 (title)
	// and ~3600 (attract demo, dense sprites).
	{
		static uint32_t rdpdbg_frame;
		rdpdbg_frame++;
		// Validation stays ON from frame 60 for the whole run (validator
		// errors print regardless of the log flag); the full command dump
		// is only windowed to keep the log readable.
		if (rdpdbg_frame == 60)
			rdpq_debug_start();
		if (rdpdbg_frame == 900 || rdpdbg_frame == 3600)
			rdpq_debug_log(true);
		if (rdpdbg_frame == 903 || rdpdbg_frame == 3603)
			rdpq_debug_log(false);
	}
#endif
#ifdef MVS64_FBCRC
	// Pixel-identity gate rig (PLAN-DRAW-RDP §6): drain the RDP, hash the
	// finished frame, then show. FNV-1a over the visible 320x224 region,
	// read uncached (the RDP wrote RDRAM behind the CPU cache). The drain
	// and the ~35k uncached reads change emulation speed, so this rig is
	// only meaningful in -DMVS64_DET_AUDIO builds (wall-channel law) and
	// its fps means nothing.
	rdpq_detach_wait();
	static uint32_t fbcrc_frame;
	uint32_t crc = 0x811C9DC5u;
	const uint8_t *row = (const uint8_t *)UncachedAddr(fbcrc_disp->buffer);
	for (int y = 0; y < 224; y++) {
		const uint32_t *p = (const uint32_t *)row;
		for (int x = 0; x < 320*2/4; x++) {
			crc ^= p[x];
			crc *= 16777619u;
		}
		row += fbcrc_disp->stride;
	}
	// Keyed by GUEST frame, so a frameskip build's drawn frames compare 1:1
	// against the same frames of a baseline (fbcrc_frame counts draws).
	extern int g_frame;
	fbcrc_frame++;
	plat_log("[FBCRC] %lu %08lx\n", (unsigned long)g_frame,
	         (unsigned long)crc);
	display_show(fbcrc_disp);
#elif defined(MVS64_DPCOSD)
	rdpq_detach_wait();
	{
		static uint32_t clk0, pipe0, tmem0, tick0, accn;
		static uint32_t acc_clk, acc_pipe, acc_tmem, acc_ticks;
		static char l1[28], l2[28], l3[28], l4[28];
		uint32_t clk  = *(volatile uint32_t*)0xA4100010 & 0xFFFFFF;
		uint32_t pipe = *(volatile uint32_t*)0xA4100018 & 0xFFFFFF;
		uint32_t tmem = *(volatile uint32_t*)0xA410001C & 0xFFFFFF;
		uint32_t now  = TICKS_READ();
		if (accn || clk0) {   // skip the bootstrap window
			acc_clk  += (clk  - clk0)  & 0xFFFFFF;
			acc_pipe += (pipe - pipe0) & 0xFFFFFF;
			acc_tmem += (tmem - tmem0) & 0xFFFFFF;
			acc_ticks += TICKS_DISTANCE(tick0, now);
			accn++;
		}
		clk0 = clk; pipe0 = pipe; tmem0 = tmem; tick0 = now;
		if (accn >= 30) {
			// RDP clock = 62.5MHz -> 62500 cycles/ms
			uint32_t pms = (uint32_t)((uint64_t)acc_pipe * 100 / (accn * 62500u));
			uint32_t tms = (uint32_t)((uint64_t)acc_tmem * 100 / (accn * 62500u));
			uint32_t bpc = acc_clk ? (uint32_t)((uint64_t)acc_pipe * 100 / acc_clk) : 0;
			uint32_t f10 = acc_ticks ? (uint32_t)((uint64_t)TICKS_PER_SECOND * accn * 10 / acc_ticks) : 0;
			sprintf(l1, "F %lu.%lu",  (unsigned long)(f10/10), (unsigned long)(f10%10));
			sprintf(l2, "P %lu.%02lu", (unsigned long)(pms/100), (unsigned long)(pms%100));
			sprintf(l3, "T %lu.%02lu", (unsigned long)(tms/100), (unsigned long)(tms%100));
			sprintf(l4, "B %lu",      (unsigned long)bpc);
			plat_log("[DPCOSD] f10=%lu pms=%lu tms=%lu busy=%lu\n",
			         (unsigned long)f10, (unsigned long)pms,
			         (unsigned long)tms, (unsigned long)bpc);
			acc_clk = acc_pipe = acc_tmem = acc_ticks = accn = 0;
		}
		uint16_t *fb = (uint16_t *)UncachedAddr(fbcrc_disp->buffer);
		int stride_px = fbcrc_disp->stride / 2;
		dpcosd_text(fb, stride_px, 8,  8, l1);
		dpcosd_text(fb, stride_px, 8, 22, l2);
		dpcosd_text(fb, stride_px, 8, 36, l3);
		dpcosd_text(fb, stride_px, 8, 50, l4);
	}
	display_show(fbcrc_disp);
#elif defined(MVS64_SNDOSD)
	rdpq_detach_wait();
	{
		// Audio-health OSD: which layer of the sound pipeline died, readable
		// on a real console with no cable (mirrors the [AIPUMP] telemetry).
		//   F dd.d gg.g  drawn fps, then game-speed (emulated) fps over a
		//             60-drawn-frame window; equal unless frameskip is on
		//   D wamh n  offload dead-latches (whole-pump, adpcm, fm, hatch)
		//             + death count
		//   K k R r W w  rspq lost-wakeup watchdog kicks + offload revives
		//             + rspq highpri-wedge recoveries (libdragon patch)
		//   S s n     silent-governor engaged + ISR silence-pad frames/sec
		//   L n C n   staging-ring lead (frames) + AI consumption frames/sec
		// Healthy @11kHz: D 0000 0, K 0 R 0 W 0, S 0 0, L ~2n, C ~11025.
		// Sound dead but C ~11025  -> delivery alive, generation muted/dead
		// (look at D/S). C 0 -> the AI interrupt chain itself died.
		extern int g_frame;
		static uint32_t tick0, rd0, pad0;
		static int accn, gf0;
		static char l1[48], l2[24], l3[24], l4[24], l5[24];
		if (accn == 0 && tick0 == 0) {   // bootstrap
			tick0 = TICKS_READ(); rd0 = aring_rd; pad0 = aring_pad; gf0 = g_frame;
		}
		if (++accn >= 60) {
			uint32_t now = TICKS_READ();
			uint32_t dt = TICKS_DISTANCE(tick0, now);
			uint32_t rd = aring_rd, pad = aring_pad;
			if (dt) {
				uint32_t f10  = (uint32_t)((uint64_t)TICKS_PER_SECOND * accn * 10 / dt);
				uint32_t g10  = (uint32_t)((uint64_t)TICKS_PER_SECOND * (uint32_t)(g_frame - gf0) * 10 / dt);
				uint32_t cons = (uint32_t)((uint64_t)(rd - rd0) * TICKS_PER_SECOND / dt);
				uint32_t strv = (uint32_t)((uint64_t)(pad - pad0) * TICKS_PER_SECOND / dt);
				uint32_t off  = YM2610_offload_flags();
#ifdef MVS64_RSPWP
				uint32_t kicks = rspwp_wedge_kicks;
#else
				uint32_t kicks = 0;
#endif
				sprintf(l1, "F %lu.%lu %lu.%lu", (unsigned long)(f10/10), (unsigned long)(f10%10),
				        (unsigned long)(g10/10), (unsigned long)(g10%10));
				sprintf(l2, "D %u%u%u%u %lu", (unsigned)!!(off & 2), (unsigned)!!(off & 1),
				        (unsigned)!!(off & 4), (unsigned)!!(off & 16),
				        (unsigned long)ym_off_deaths);
				snprintf(l3, sizeof l3, "K %lu R %lu W %lu", (unsigned long)kicks,
				        (unsigned long)ym_off_revives,
				        (unsigned long)rspq_wedge_recoveries());
				sprintf(l4, "S %d %lu", sound_silent ? 1 : 0, (unsigned long)strv);
				sprintf(l5, "L %lu C %lu", (unsigned long)(aring_wr - aring_rd),
				        (unsigned long)cons);
			}
			tick0 = now; rd0 = rd; pad0 = pad; accn = 0; gf0 = g_frame;
		}
		uint16_t *fb = (uint16_t *)UncachedAddr(fbcrc_disp->buffer);
		int stride_px = fbcrc_disp->stride / 2;
		dpcosd_text(fb, stride_px, 8,  8, l1);
		dpcosd_text(fb, stride_px, 8, 22, l2);
		dpcosd_text(fb, stride_px, 8, 36, l3);
		dpcosd_text(fb, stride_px, 8, 50, l4);
		dpcosd_text(fb, stride_px, 8, 64, l5);
	}
	display_show(fbcrc_disp);
#elif defined(MVS64_PERFOSD)
	{
		extern int g_frame;
		static uint32_t tick0, pipe0, tmem0, acc_pipe, acc_tmem;
		static int accn, gf0;
		static char lines[8][32];
		// DPC counters are 24-bit at 62.5 MHz (wrap every ~0.27 s):
		// accumulate per-frame deltas. Read without draining, so a delta
		// covers whatever the RDP finished since the last read.
		uint32_t pipe = *(volatile uint32_t*)0xA4100018 & 0xFFFFFF;
		uint32_t tmem = *(volatile uint32_t*)0xA410001C & 0xFFFFFF;
		if (tick0 == 0) {   // bootstrap
			tick0 = TICKS_READ(); gf0 = g_frame;
		} else {
			acc_pipe += (pipe - pipe0) & 0xFFFFFF;
			acc_tmem += (tmem - tmem0) & 0xFFFFFF;
		}
		pipe0 = pipe; tmem0 = tmem;
		if (++accn >= 60) {
			uint32_t now = TICKS_READ();
			uint32_t dt = TICKS_DISTANCE(tick0, now);
			uint32_t n = posd_acc_n;
			if (dt && n) {
				uint32_t f10 = (uint32_t)((uint64_t)TICKS_PER_SECOND * accn * 10 / dt);
				uint32_t g10 = (uint32_t)((uint64_t)TICKS_PER_SECOND * (uint32_t)(g_frame - gf0) * 10 / dt);
				uint32_t m = posd_ms10(posd_acc_m68k, n), s = posd_ms10(posd_acc_snd, n);
				uint32_t w = posd_ms10(posd_acc_wait, n);
				uint32_t v = posd_ms10(posd_acc_draw - posd_acc_wait, n);
				uint32_t a = posd_ms10(posd_acc_all, n);
				// RDP clock 62.5 MHz = 62500 cycles/ms; per drawn frame
				uint32_t p = (uint32_t)((uint64_t)acc_pipe * 10 / ((uint64_t)accn * 62500u));
				uint32_t t = (uint32_t)((uint64_t)acc_tmem * 10 / ((uint64_t)accn * 62500u));
				snprintf(lines[0], 32, "F %lu.%lu %lu.%lu", (unsigned long)(f10/10), (unsigned long)(f10%10),
				         (unsigned long)(g10/10), (unsigned long)(g10%10));
				snprintf(lines[1], 32, "M %lu.%lu S %lu.%lu", (unsigned long)(m/10), (unsigned long)(m%10),
				         (unsigned long)(s/10), (unsigned long)(s%10));
				snprintf(lines[2], 32, "V %lu.%lu W %lu.%lu", (unsigned long)(v/10), (unsigned long)(v%10),
				         (unsigned long)(w/10), (unsigned long)(w%10));
				// X = frame time not covered by M/S/V/W (events, input poll,
				// interrupt handlers, anything else in the loop)
				uint32_t mx = m + s + v + w, x = a > mx ? a - mx : 0;
				uint32_t b = posd_ms10(posd_acc_beg, n), r = posd_ms10(posd_acc_spr, n);
				uint32_t l = posd_ms10(posd_acc_fix, n), k = posd_ms10(posd_acc_walk, n);
				uint32_t c = posd_ms10(posd_acc_miss, n);
				#define POSD_MS(q) (unsigned long)((q)/10), (unsigned long)((q)%10)
				snprintf(lines[3], 32, "B %lu.%lu L %lu.%lu", POSD_MS(b), POSD_MS(l));
				snprintf(lines[4], 32, "R %lu.%lu K %lu.%lu", POSD_MS(r), POSD_MS(k));
				snprintf(lines[5], 32, "C %lu.%lu", POSD_MS(c));
				snprintf(lines[6], 32, "A %lu.%lu X %lu.%lu", POSD_MS(a), POSD_MS(x));
				snprintf(lines[7], 32, "P %lu.%lu T %lu.%lu", POSD_MS(p), POSD_MS(t));
				#undef POSD_MS
				posd_render(lines, 8);
				plat_log("[PERFOSD] f=%d f10=%lu g10=%lu m=%lu s=%lu v=%lu w=%lu a=%lu x=%lu p=%lu t=%lu b=%lu r=%lu l=%lu k=%lu c=%lu\n",
				         g_frame, (unsigned long)f10, (unsigned long)g10, (unsigned long)m,
				         (unsigned long)s, (unsigned long)v, (unsigned long)w, (unsigned long)a,
				         (unsigned long)x, (unsigned long)p, (unsigned long)t,
				         (unsigned long)b, (unsigned long)r, (unsigned long)l,
				         (unsigned long)k, (unsigned long)c);
			}
			tick0 = now; gf0 = g_frame; accn = 0;
			acc_pipe = acc_tmem = 0;
			posd_acc_all = posd_acc_m68k = posd_acc_snd = posd_acc_draw = posd_acc_wait = 0;
			posd_acc_beg = posd_acc_spr = posd_acc_fix = posd_acc_walk = posd_acc_miss = 0;
			posd_acc_n = 0;
		}
		if (posd_cur >= 0) {
			rdpq_set_mode_copy(true);
			rdpq_tex_blit(&posd_surf[posd_cur], 8, 64, NULL);   // below the HUD
		}
	}
	plat_detach_show();
#else
	plat_detach_show();
#endif
}
