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

DEFINE_RSP_UCODE(rsp_video);

uint8_t keystate[256];

extern char end __attribute__((section (".data")));

// Audio (libdragon AI) state — see plat_audio_pump below.
static int audio_enabled = 0;
#define AI_NUM_BUFFERS 4          // AI back buffers; also bounds plat_audio_pump

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

    // Register our custom RSP overlay into the RSP queue engine
    RSP_OVL_ID = rspq_overlay_register(&rsp_video);

    audio_init(audiofreq, AI_NUM_BUFFERS);
    (void)fps;
}

// --- Audio (libdragon Audio Interface) -------------------------------------
// The AI DMA is the real-time clock master: it drains 44100 Hz continuously no
// matter how fast the (slow, ~22fps in-match) 68k frame loop runs. So we
// generate audio ON DEMAND — once per main-loop pass we fill EVERY currently
// free AI buffer, rendering exactly buffer-length samples straight into the AI
// buffer. sound_gen_samples() is rate-agnostic (N samples == N/44100 s of
// Z80+YM2610 time), so music plays at correct pitch AND tempo regardless of
// video fps, and there is never a mid-buffer silence splice — the old hand-
// rolled FIFO's zero-padding on underrun was the source of the periodic
// click/buzz. When the Z80 is inactive sound_gen_samples() writes a whole
// buffer of clean silence, so no special-casing is needed here.
void plat_enable_audio(int enable) {
    audio_enabled = enable;
}

// Audio generation is locked to the GUEST CLOCK, not to AI buffer availability:
// each pass we owe the DAC exactly (elapsed guest time) x (real AI frequency)
// samples, delivered in whole AI buffers. On real hardware the AI frees buffers
// at precisely the rate the debt accrues, so this behaves exactly like the old
// "fill every free buffer" pump. But it stays correct when the AI is emulated
// with wrong timing (BizHawk-Mupen): if the AI never frees a buffer, we still
// advance the Z80/YM in guest time (into a discard buffer) so 68k<->Z80
// handshakes keep completing — with an AI-availability-driven pump, the boot
// jingle's handshake starves and wedges the whole machine at ~frame 537; if the
// emulated AI drains too fast, we simply never generate more than guest time
// allows instead of spinning on audio_can_write(). Using audio_get_frequency()
// (the exact post-quantization DAC rate) keeps generation and drain in the same
// clock domain, so the debt has no long-term drift on real hardware.
void plat_audio_pump(void) {
    if (!audio_enabled) return;
#ifdef SND_HEALTH
    uint32_t _t0 = TICKS_READ();
#endif
    const int buflen = audio_get_buffer_length();
    static uint32_t last_t;
    static uint64_t acc;            // fractional sample debt (ticks x Hz)
    static int due;                 // whole samples owed to the DAC
    static int underrun_streak;
    uint32_t now = TICKS_READ();
    if (last_t == 0) last_t = now;  // first pass: start the clock, owe nothing
    acc += (uint64_t)TICKS_DISTANCE(last_t, now) * (uint32_t)audio_get_frequency();
    last_t = now;
    due += acc / TICKS_PER_SECOND;
    acc %= TICKS_PER_SECOND;

    // Sustained-underrun detection: owing more than the whole AI buffer set
    // means we can't generate real-time audio fast enough (the boot voice/jingle
    // is Z80-CPU-bound on N64). Drop the debt to ONE buffer and tell
    // sound_gen_samples to emit silence. Dropping to one buffer (not the full
    // set) matters: while overloaded the governor below processes only one
    // buffer per pass, so a 4-buffer debt could never be repaid below ~25fps and
    // a single stall (e.g. character-select loading hitch) trapped the pump in
    // silent+starved mode permanently — audible as a stuck replay tone. The Z80
    // still advances (handshake/timers intact). sound_silent is set from the
    // PREVIOUS pass's streak so a single slow frame isn't muted.
    sound_silent = (underrun_streak >= 1);
    if (due > AI_NUM_BUFFERS * buflen) {
        due = buflen;
        if (underrun_streak < 1000) underrun_streak++;
    } else {
        underrun_streak = 0;
    }

    // Overload governor: in sustained underrun (sound_silent set), generation
    // is slower than real time and the output is zeros anyway, so wall-locking
    // the Z80 only burns the frame budget stepping it — the boot jingle's
    // Z80-bound phase dropped the whole machine to ~4fps this way. Process at
    // most ONE buffer per pass while overloaded (the debt clamp above drops the
    // rest); the machine then slows uniformly instead of audio starving video.
    int pass_budget = (underrun_streak >= 1) ? 1 : AI_NUM_BUFFERS;

    int filled = 0, discarded = 0, silfill = 0;

    // ANTI-REPLAY, part 1 (pre-flood): while overloaded, generating even one
    // buffer can take 100-200ms (the Z80-bound jingle), during which the AI
    // drains the whole queue and the hardware starts REPLAYING its last buffer
    // (the stuck-tone bug). Top up every free slot but one with pure silence
    // BEFORE generating, so the ~160ms queue cushion covers the generation time
    // itself. The one slot left free receives this pass's generated buffer.
    if (underrun_streak >= 1) {
        for (int k = 0; k < AI_NUM_BUFFERS - 1 && audio_can_write(); k++) {
            uint32_t *o = (uint32_t *)audio_write_begin();
            for (int i = 0; i < buflen; i++)
                o[i] = 0;
            audio_write_end();
            silfill++;
        }
    }
    while (due >= buflen && filled < pass_budget && audio_can_write()) {
        short *out = audio_write_begin();
        sound_gen_samples((int16_t *)out, buflen);
        audio_write_end();
        due -= buflen;
        filled++;
    }
    // AI accepting nothing while we are >=2 buffers behind guest time: the AI is
    // stalled/mis-emulated. Advance the sound machine into a discard buffer so
    // the 68k<->Z80 handshake cannot starve. Never triggers on real hardware
    // (there the AI frees buffers at exactly the debt rate); the 2-buffer slack
    // absorbs scheduling jitter so a legit briefly-full queue is left alone.
    if (due >= 2 * buflen && filled < pass_budget) {
        static int16_t discard[2048 * 2];
        int n = buflen <= 2048 ? buflen : 2048;
        sound_gen_samples(discard, n);
        due -= n;
        discarded++;
    }

    // ANTI-REPLAY: when the AI DMA queue runs dry, the hardware REPLAYS its
    // last buffer forever — the "stuck high-pitch tone" heard at boot and in
    // character select. While overloaded we deliver few/no generated buffers
    // (the governor above), so top up EVERY remaining free AI buffer with pure
    // silence: no sound generation, no debt consumed, just packed zero stores
    // (~µs). Generated audio in this state is zeros anyway (sound_silent), so
    // the stream stays seamless; once the overload ends, real audio resumes
    // behind at most one queue's worth (~160ms) of clean silence.
    int silfill = 0;
    if (underrun_streak >= 1) {
        while (audio_can_write()) {
            uint32_t *out = (uint32_t *)audio_write_begin();
            for (int i = 0; i < buflen; i++)
                out[i] = 0;
            audio_write_end();
            silfill++;
        }
    }
#ifdef SND_HEALTH
    // Delivery-health probe (USB/ISViewer). buffers/60 ~= real-time delivery
    // rate; discard>0 = the AI is not draining (broken emulator AI — never on
    // real HW); underruns = passes where the guest-clock debt overflowed the
    // whole buffer set (generation can't keep up -> silence).
    //   sndms  = avg wall-clock ms spent synthesising audio per pump. If this
    //            approaches/exceeds the 16.7ms video-frame budget, real-time YM
    //            synthesis alone can't keep up -> the fundamental perf wall.
    {
        static int pumps = 0, total = 0, deep = 0, maxf = 0, disc = 0;
        static int sil = 0, dry = 0;
        static uint32_t tacc = 0;
        tacc += TICKS_DISTANCE(_t0, TICKS_READ());
        total += filled; if (filled > maxf) maxf = filled;
        disc += discarded;
        sil += silfill;
        // dry: the whole queue was free at pass start = the AI had fully
        // drained = the hardware was replaying its last buffer. Must stay 0
        // (except the very first pass) or a stuck tone was audible.
        if (filled + silfill >= AI_NUM_BUFFERS && pumps > 0) dry++;
        if (underrun_streak) deep++;
        if ((pumps++ % 60) == 0) {
            plat_log("[AIPUMP] pass=%d buffers/60=%d maxfill=%d underruns=%d discard=%d sil=%d dry=%d due=%d sndms=%.2f silent=%d\n",
                     pumps, total, maxf, deep, disc, sil, dry, due,
                     (float)tacc * 1000.f / (float)TICKS_PER_SECOND / 60.f, sound_silent);
            total = 0; maxf = 0; tacc = 0; disc = 0; deep = 0; sil = 0; dry = 0;
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
