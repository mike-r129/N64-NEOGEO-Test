#include "platform.h"
#include <memory.h>

volatile int N64_FRAME = 0;
uint32_t RSP_OVL_ID = 0;

DEFINE_RSP_UCODE(rsp_video);

uint8_t keystate[256];

extern char end __attribute__((section (".data")));

// Audio (libdragon AI) state — see plat_endaudio below.
static int     audio_enabled = 0;
static int     audio_spf = 735;           // stereo frames produced per video frame
static int16_t audio_scratch[2048 * 2];   // one video-frame of stereo samples
#define AFIFO_FRAMES 8192
static int16_t afifo[AFIFO_FRAMES * 2];
static int afifo_w = 0, afifo_r = 0;

static void vblank_handler(void) {
    N64_FRAME++;
}

void plat_init(int audiofreq, int fps) {
#ifdef __LIBDRAGON_DEBUG_H
    debug_init_isviewer();
    debug_init_usblog();
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

    audio_init(audiofreq, 4);
    audio_spf = audiofreq / fps;          // stereo frames produced per video frame
}

// --- Audio (libdragon Audio Interface) -------------------------------------
// The emulator produces ~audio_spf stereo frames per video frame, but the AI
// consumes fixed-length buffers, so we FIFO between the two.
void plat_enable_audio(int enable) {
    audio_enabled = enable;
}

void plat_beginaudio(int16_t **buf, int *nsamples) {
    *buf = audio_scratch;
    *nsamples = audio_spf;
}

void plat_endaudio(void) {
    if (!audio_enabled) return;

    // Enqueue this frame's samples.
    for (int i = 0; i < audio_spf; i++) {
        int nw = (afifo_w + 1) % AFIFO_FRAMES;
        if (nw == afifo_r) break;         // FIFO full: drop (overrun)
        afifo[afifo_w * 2 + 0] = audio_scratch[i * 2 + 0];
        afifo[afifo_w * 2 + 1] = audio_scratch[i * 2 + 1];
        afifo_w = nw;
    }

    // Drain into any free AI buffers (pad with silence on underrun).
    while (audio_can_write()) {
        short *out = audio_write_begin();
        int n = audio_get_buffer_length();
        for (int i = 0; i < n; i++) {
            if (afifo_r != afifo_w) {
                out[i * 2 + 0] = afifo[afifo_r * 2 + 0];
                out[i * 2 + 1] = afifo[afifo_r * 2 + 1];
                afifo_r = (afifo_r + 1) % AFIFO_FRAMES;
            } else {
                out[i * 2 + 0] = 0;
                out[i * 2 + 1] = 0;
            }
        }
        audio_write_end();
    }
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
    // Headless validation: deterministically drive coin -> start -> select ->
    // a match off the emulated frame counter (no human/controller needed), so
    // playable input + combat can be confirmed agentically. Enable with
    // EXTRA_DEFINES=-DMVS64_AUTOINPUT.
    {
        static int af = 0;
        int f = af++;
        #define AW(a,b)   (f >= (a) && f < (b))
        #define AD(p,on)  (((f) % (p)) < (on))
        if (AW( 500, 2600) && AD(40, 6))            keystate[PLAT_KEY_COIN_1]  = 1; // hammer coins
        if (AW(1400, 3600) && AD(70, 6) && !AD(40,6)) keystate[PLAT_KEY_P1_START] = 1; // start
        if (AW(2200, 4200) && AD(55, 6))            keystate[PLAT_KEY_P1_A]     = 1; // confirm/select
        if (AW(3200, 100000000) && AD(64, 30))      keystate[PLAT_KEY_P1_RIGHT] = 1; // approach
        if (AW(3200, 100000000) && AD(48, 10))      keystate[PLAT_KEY_P1_C]     = 1; // attack
        if ((f % 120) == 0) debugf("[AUTOINPUT] frame=%d\n", f);
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
