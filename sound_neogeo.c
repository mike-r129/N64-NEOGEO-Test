// NeoGeo sound subsystem: Z80 audio CPU (+ YM2610 in WS3).
//
// Implements the sound.h seam with a real Z80 (superzazu core) running the
// m.rom driver. Memory map, bank switching and I/O port layout follow the
// NeoGeo hardware (cross-checked against gngeo). The YM2610 is not wired yet
// (WS3): its ports are stubbed, so this boots the driver and completes the
// 68k<->Z80 command handshake but still emits silence.
#include "sound.h"
#include "emu.h"
#include "roms.h"
#include "platform.h"
#include "z80.h"
#include "ym2610/ym2610.h"
#include <string.h>
#include <stdlib.h>

// Audio-health telemetry. MVS64_AUTOINPUT (headless validation) implies it, but
// MVS64_SNDHEALTH enables the [SNDRMS]/[AIPUMP] USB logs in a normal, human-
// driven build too (no scripted input), so real-hardware behaviour can be read
// off a flashcart USB capture while actually playing. MVS64_SNDOSD must imply
// it here too (2026-08-30: an OSD-only build wrote an SD log with no [SNDRMS]
// lines — platform_n64.c's gate included SNDOSD but this one didn't, so the
// one hardware post-mortem that needed z80pc/t=/steps didn't have them).
#if defined(MVS64_AUTOINPUT) || defined(MVS64_SNDHEALTH) || defined(MVS64_SNDOSD)
#define SND_HEALTH 1
#endif

// NeoGeo audio Z80 runs at 4 MHz; produce one video frame's worth per call.
#define Z80_CLOCK            4000000
#define Z80_CYCLES_PER_FRAME (Z80_CLOCK / 60)
#define YM_CLOCK             8000000
#define AUDIO_RATE           MVS64_AUDIO_RATE

static z80 cpu;
static int  z80_active;                 // false when there is no m.rom
static uint8_t z80_ram[0x800];          // 2KB work RAM at 0xF800-0xFFFF
static const uint8_t *z80_bank[4];      // window base pointers into M_ROM

static uint8_t sound_code;              // 68k -> Z80 command latch
static uint8_t result_code;             // Z80 -> 68k reply latch
static uint8_t pending_command;
static int snd_dbg;                     // MVS64_SNDDBG: trace Z80 PC
int sound_silent;                       // see sound.h: run Z80 but emit silence
#ifdef SND_HEALTH
static int g_timer_fires[2];            // YM2610TimerOver calls per SNDRMS interval
#endif

// NOTE (2026-07-01): the old "stuck-voice guard" that force-zeroed any SSG
// channel holding a constant nonzero volume for 3s was REMOVED. Telemetry
// proved it was killing LEGITIMATE sustained SSG content (stage ambience /
// music beds — samsho2 holds SSG voices for many seconds by design): every
// [STUCKGUARD] fire was followed by the in-game rms collapsing to 0, i.e. the
// guard itself was the "audio keeps cutting out" bug. The failure it guarded
// against (the AI replaying a stale buffer as an endless tone) is prevented
// structurally by the guest-clock audio pump + sound_silent underrun path.

// --- Sound event trace (MVS64_SNDTRACE) ------------------------------------
// Differential debug of the "press Start -> stuck long beep" bug: log every
// 68k->Z80 command and every YM2610 key-on/off (FM register 0x28) write, so we
// can see which FM channel is keyed on at Start and whether a key-OFF ever
// follows (a missing key-off = the stuck note). Events are pushed into a ring
// from ANY context — including the m64k MMIO exception handler that runs the Z80
// inline for sound_write_command, where SD/FatFs I/O is unsafe — and DRAINED to
// plat_log() from sound_gen_samples(), which only ever runs in safe pump context.
// Producers never overlap in time (the 68k frame step and the audio pump are
// sequential in the main loop), so the single-producer ring is race-free.
#ifdef MVS64_SNDTRACE
#define TRACE_N 2048                          // power of two
static volatile uint16_t trace_buf[TRACE_N];  // packed (type<<12)|payload12; 1=CMD 2=FMKEY 3=SSG
static volatile unsigned trace_head, trace_tail;
static inline void trace_push(int type, unsigned payload) {
	unsigned h = trace_head;
	trace_buf[h & (TRACE_N - 1)] = (uint16_t)((type << 12) | (payload & 0xFFF));
	trace_head = h + 1;
}
static void trace_drain(void) {
	while (trace_tail != trace_head) {
		uint16_t e = trace_buf[trace_tail & (TRACE_N - 1)]; trace_tail++;
		int type = e >> 12; unsigned p = e & 0xFFF; uint8_t v = (uint8_t)p;
		if (type == 1) plat_log("[CMD] %02x\n", v);
		else if (type == 2) plat_log("[YMKEY] %02x ch=%d %s\n", v, v & 7, ((v >> 4) ? "ON" : "off"));
		else if (type == 4) // ADPCM-A key on/off (reg 0x100): b7=0 on, b7=1 dump/off
			plat_log("[AKEY] %02x %s mask=%02x\n", v, (v & 0x80) ? "OFF" : "ON", v & 0x3f);
		else { // SSG: payload = (reg<<8)|val
			int reg = (p >> 8) & 0xF;
			if (reg == 0x07) plat_log("[SSG] mixer=%02x toneA=%d toneB=%d toneC=%d\n",
				v, !(v & 1), !(v & 2), !(v & 4));   // bit clear = tone ENABLED
			else plat_log("[SSG] reg%X(%s)=%02x\n", reg,
				reg==8?"volA":reg==9?"volB":reg==0xA?"volC":"tone", v);
		}
	}
}
#endif

// Last register selected on YM port A (control-A write). Needed by both the
// SNDTRACE event log and the N64 stuck-voice guard, so tracked unconditionally.
static uint8_t ym_addr_a;
#ifdef MVS64_SNDTRACE
static uint8_t ym_addr_b;                // last register selected on YM port B
#endif

// --- Z80 idle-skip ---------------------------------------------------------
// The NeoGeo sound driver spends most of its time in a tight interrupt-wait
// spin (poll a flag / branch back) between FM-timer ticks. Single-stepping that
// at the real 4 MHz rate dominates the N64 CPU (~70%), so without this the game
// drops to ~4 fps once audio is decoupled to real time. We detect a PURE spin —
// a backward branch that returns to the same PC with byte-identical registers
// and no port/RAM writes in the loop — and fast-forward cpu.cyc to the next
// timer deadline. emit() still synthesizes the FM audio for the skipped span, so
// music keeps sounding; only the redundant Z80 stepping is skipped. Provably
// equivalent: with identical state and no side effects, every iteration is the
// same until an external event (the timer IRQ), which fires at the deadline.
// Delay loops are NOT skipped — they mutate a register (e.g. DJNZ's B), so the
// snapshot compare fails. This is the sound-CPU analog of the 68k idle-skip.
static int z80_wrote;                    // set by z80_out/z80_write = real work
#ifdef SND_HEALTH
static unsigned long g_z80_steps, g_z80_skipcyc; static int g_z80_skips;
#endif
#ifdef MVS64_Z80HIST
// Diagnostic: where do the interpreted Z80 steps actually go? 16-byte PC
// buckets accumulated per step; top buckets reported per [SNDRMS] interval.
// Also counts stepping segments (timer-boundary re-entries) to price the
// per-segment cache-reentry cost. Diagnostic builds only (16KB table).
static uint32_t z80_hist[4096];
static uint32_t g_z80_segs;
#endif
// [SNDPROF] split cost telemetry (N64): where does audio wall-time actually go —
// stepping the Z80 vs synthesising the YM2610? Reported in the [SNDRMS] line as
// z80ms/ymms per 60-call interval (TICKS_PER_SECOND/1000 ticks per ms).
#if defined(SND_HEALTH) && defined(N64)
static uint32_t g_prof_z80t, g_prof_ymt;
#ifdef MVS64_Z80WARM
// Diagnostic: per-segment cold/warm split of Z80 stepping cost. The first
// Z80WARM_N steps after each emit() run against caches the YM synthesis just
// evicted; the rest run warm. [Z80WARM] reports ticks+steps for both halves.
#define Z80WARM_N 32
static uint32_t g_zw_ct, g_zw_cs, g_zw_wt, g_zw_ws, g_zw_seg;
#endif
static uint32_t g_prof_gen;   // whole sound_gen_samples body: genms - z80ms
                              // - ymms = the unaccounted seam (timer service,
                              // boundary math, RMS probe, wp ship/sweep)
#endif
// The snapshot runs at EVERY backward-branch edge of every Z80 loop (not
// just idle spins), so it is on the stepping hot path: pack the exact same
// fields as before into three u64s composed in registers and compare those
// directly — no memset/field stores/memcmp. Same captured state = identical
// skip decisions at identical cycle boundaries (the WAV-locked invariant).
struct z80snap { uint64_t q0, q1, q2; };
static inline void z80_snap(struct z80snap *s, const z80 *z) {
	s->q0 = (uint64_t)z->sp | ((uint64_t)z->ix << 16) | ((uint64_t)z->iy << 32) |
	        ((uint64_t)z->a << 48) | ((uint64_t)z->b << 56);
	s->q1 = (uint64_t)z->c | ((uint64_t)z->d << 8) | ((uint64_t)z->e << 16) |
	        ((uint64_t)z->h << 24) | ((uint64_t)z->l << 32) |
	        ((uint64_t)z->a_ << 40) | ((uint64_t)z->b_ << 48) |
	        ((uint64_t)z->c_ << 56);
	s->q2 = (uint64_t)z->d_ | ((uint64_t)z->e_ << 8) | ((uint64_t)z->h_ << 16) |
	        ((uint64_t)z->l_ << 24) | ((uint64_t)z->f_ << 32) |
	        ((uint64_t)z->i << 40) |
	        ((uint64_t)(uint8_t)((z->sf<<7)|(z->zf<<6)|(z->yf<<5)|(z->hf<<4)|
	                             (z->xf<<3)|(z->pf<<2)|(z->nf<<1)|(z->cf)) << 48) |
	        ((uint64_t)(uint8_t)((z->iff1?1:0)|(z->iff2?2:0)|
	                             (z->interrupt_mode<<2)) << 56);
}
static inline int z80_snap_eq(const struct z80snap *a, const struct z80snap *b) {
	return a->q0 == b->q0 && a->q1 == b->q1 && a->q2 == b->q2;
}

// YM2610 stream output (interleaved s16 L/R), filled by YM2610Update_stream().
uint16_t play_buffer[16384];

// Resident ADPCM sample ROM (v.rom) when RAM allows (PC build); NULL otherwise.
static uint8_t *vrom_resident;

// --- ADPCM sample streaming (v.rom window caches) ---------------------------
// The 7MB v.rom cannot live in N64 RDRAM, but the YM2610's ADPCM engines read
// it strictly sequentially per voice. Each of the 7 voices (6x ADPCM-A + the
// ADPCM-B) gets a small aligned window; a fetch outside the window reloads it
// via vrom_read (cart DFS/PI DMA on N64) — ~one 2KB read per 0.2s per active
// voice, a negligible PI load. Fetches happen only during synthesis (audio-pump
// context, normal C), never inside the 68k MMIO exception handler (key-on just
// arms the channel), so the PI DMA here is safe.
// Windows are defined in ym2610.h so the per-byte hit path can be inlined
// into the ADPCM decoders (it used to be a cross-TU call per nibble pair).
struct ym2610_vwin ym2610_vwin[7];

uint8_t ym2610_vrom_fetch_slow(int win, uint32_t addr) {
	struct ym2610_vwin *w = &ym2610_vwin[win];
	uint32_t base = addr & ~(uint32_t)(YM2610_VWIN_SIZE - 1);
	int len = YM2610_VWIN_SIZE;
	if (base + (uint32_t)len > v_rom_size) {
		len = (int)(v_rom_size - base);   // addr < v_rom_size is guaranteed
		if (len <= 0) return 0;           // by the pcmsize clamp upstream
	}
	vrom_read(base, w->buf, len);
	w->base = base; w->valid = 1;
	return w->buf[addr & (YM2610_VWIN_SIZE - 1)];
}

// --- YM2610 timer/IRQ glue (cycle-based) -----------------------------------
// The YM core hands us timer periods as integer Z80 cycles (0 = stop); we track
// deadlines in Z80 cycles and fire YM2610TimerOver (which raises the Z80 IRQ
// that ticks the music driver). This whole path is deliberately INTEGER-ONLY:
// on N64 it runs inside the TLB/MMIO exception handler (68k sound-latch write ->
// z80_run -> YM register write -> timer start), where the FPU is disabled and
// no FP context is saved — a single float op there nests into a wild jump.
static int           ym_timer_on[2];
static unsigned long ym_timer_deadline[2];   // absolute cpu.cyc

uint32_t ym2610_time_now_cyc(void) {        // FM_GET_TIME_NOW_CYC source
	return (uint32_t)cpu.cyc;
}

static void ym_timer_handler(int c, uint32_t cycles) {
	if (cycles == 0) {
		ym_timer_on[c] = 0;
	} else {
		ym_timer_on[c] = 1;
		ym_timer_deadline[c] = cpu.cyc + cycles;
	}
}

// The YM2610 /IRQ pin is LEVEL-triggered: as long as any unmasked timer flag
// is set, the line stays low and a real Z80 re-enters the interrupt the moment
// it executes EI/RETI. z80_gen_int models a one-shot edge (int_pending is
// consumed on acceptance), so when timers A and B fire close together — they
// run at ~130/s and ~107/s here and collide constantly — the second assert
// used to be swallowed (FM_STATUS_SET only calls the IRQ handler on the 0->1
// edge of ST->irq) and that music tick was silently LOST. A lost tick at the
// wrong moment leaves the driver's note-off step unexecuted = a latched voice.
// ym_irq_level mirrors the line so the Z80 run loops can re-assert while high.
static int ym_irq_level;
#ifdef SND_HEALTH
static int g_irq_redeliver;                 // level-triggered re-asserts (see above)
#endif
static void ym_irq_handler(int irq) {
	ym_irq_level = irq;
	if (irq) z80_gen_int(&cpu, 0xff);       // assert (IM1 -> RST 38h)
	else     cpu.int_pending = 0;           // deassert if not yet serviced
}

// Re-deliver a level-held IRQ the edge model dropped: the line is high, the
// CPU can take interrupts, but no interrupt is pending = a real Z80 would be
// entering the handler right now. Call before stepping in every Z80 run loop.
static inline void z80_service_level_irq(void) {
	if (ym_irq_level && cpu.iff1 && !cpu.int_pending) {
		z80_gen_int(&cpu, 0xff);
#ifdef SND_HEALTH
		g_irq_redeliver++;
#endif
	}
}

// Bank switch: window <bank> is remapped to M_ROM + size*(porthi & mask).
// Window/bank geometry (see gngeo cpu_z80_switchbank):
//   bank 0 -> 0x8000, 16KB, mask 0x0f   bank 1 -> 0xC000, 8KB,  mask 0x1f
//   bank 2 -> 0xE000, 4KB,  mask 0x3f   bank 3 -> 0xF000, 2KB,  mask 0x7f
static void switchbank(int bank, uint16_t port) {
	static const uint32_t bsize[4] = { 0x4000, 0x2000, 0x1000, 0x0800 };
	static const uint32_t bmask[4] = { 0x0f, 0x1f, 0x3f, 0x7f };
	uint32_t off = bsize[bank] * ((port >> 8) & bmask[bank]);
	if (off < m_rom_size)
		z80_bank[bank] = M_ROM + off;
}

// --- Z80 bus ---------------------------------------------------------------
// NOTE (2026-07-09): inlining this map into z80.c's rb/wb (killing the
// function-pointer call per byte access) was implemented, WAV-verified
// bit-exact, and MEASURED WORSE on N64: us/Z80-step 3.29 -> 3.62 (+10%),
// snd +2-4 points, in-fight fps -1..-1.8. The 6-branch decode inlined into
// hundreds of rb/wb sites across the opcode switch blew the icache, while
// the callback keeps it in one hot line — same lesson class as the -O3
// audio regression. The Z80's ~300 host cycles/step is cache behavior, not
// call overhead. Don't retry inline-bus; attack step count / locality.
static uint8_t z80_read(void *ud, uint16_t addr) {
	(void)ud;
	if (addr < 0x8000) return M_ROM[addr];               // fixed first 32KB
	if (addr < 0xC000) return z80_bank[0][addr - 0x8000];
	if (addr < 0xE000) return z80_bank[1][addr - 0xC000];
	if (addr < 0xF000) return z80_bank[2][addr - 0xE000];
	if (addr < 0xF800) return z80_bank[3][addr - 0xF000];
	return z80_ram[addr - 0xF800];
}

static void z80_write(void *ud, uint16_t addr, uint8_t val) {
	(void)ud;
	if (addr >= 0xF800) z80_ram[addr - 0xF800] = val;    // only work RAM is writable
	z80_wrote = 1;                                        // taints idle-skip window
}

static uint8_t z80_in(z80 *z, uint16_t port) {
	(void)z;
	switch (port & 0xff) {
	case 0x00: pending_command = 0; return sound_code;   // read command, ack
	case 0x04: return YM2610Read(0);                     // YM2610 status A
	case 0x05: return YM2610Read(1);                     // YM2610 read port
	case 0x06: return YM2610Read(2);                     // YM2610 status B
	case 0x08: switchbank(3, port); return 0;
	case 0x09: switchbank(2, port); return 0;
	case 0x0a: switchbank(1, port); return 0;
	case 0x0b: switchbank(0, port); return 0;
	}
	return 0;
}

static void z80_out(z80 *z, uint16_t port, uint8_t val) {
	(void)z;
	z80_wrote = 1;                                        // taints idle-skip window
	switch (port & 0xff) {
	case 0x04: YM2610Write(0, val);
		ym_addr_a = val;                                 // register select (bank A)
		break;                                           // control A
	case 0x05: YM2610Write(1, val);
#ifdef MVS64_SNDTRACE
		if (ym_addr_a == 0x28) trace_push(2, val);                   // FM key on/off
		else if (ym_addr_a == 0x07 ||                                // SSG mixer
		         (ym_addr_a >= 0x08 && ym_addr_a <= 0x0A))           // SSG volA/B/C
			trace_push(3, ((unsigned)ym_addr_a << 8) | val);
#endif
		break;                                           // data A
	case 0x06: YM2610Write(2, val);
#ifdef MVS64_SNDTRACE
		ym_addr_b = val;                                 // register select (bank B)
#endif
		break;                                           // control B
	case 0x07: YM2610Write(3, val);
#ifdef MVS64_SNDTRACE
		if (ym_addr_b == 0x00) trace_push(4, val);       // ADPCM-A key on/off/dump
#endif
		break;                                           // data B
	case 0x0c: result_code = val; break;                 // reply to 68k
	}
}

// --- sound.h seam ----------------------------------------------------------
void sound_init(void) {
	snd_dbg = getenv("MVS64_SNDDBG") != NULL;
	if (!M_ROM || m_rom_size == 0) {
		z80_active = 0;
		plat_log("[SND] no m.rom; sound disabled\n");
		return;
	}
	z80_init(&cpu);
	cpu.read_byte  = z80_read;
	cpu.write_byte = z80_write;
	cpu.port_in    = z80_in;
	cpu.port_out   = z80_out;
#ifdef MVS64_CYCWRAP_TEST
	// Wrap-gate rig — see the twin block in sound_reset().
	cpu.cyc = 0xFFFFFFFFul - 4000000ul * 120ul;
	plat_log("[CYCWRAP] cyc parked at %08lx (wrap in ~120s of audio)\n",
	         (unsigned long)cpu.cyc);
#endif

	// ADPCM sample source (v.rom, up to 7MB). samsho2 has no separate ADPCM-B
	// ROM, so A and B share the same data (as on real NeoGeo). Resident when RAM
	// allows (PC build); otherwise STREAMED per voice through the vwin window
	// caches above — a NULL buffer with adpcm_size > 0 selects streaming inside
	// the YM2610 core (ym2610_vrom_fetch). The old resident pull on N64 overran
	// the 8MB heap (7MB malloc) and ADPCM used to be disabled entirely there.
	unsigned adpcm_size = v_rom_size;
	if (v_rom_size) {
#ifdef N64
		plat_log("[SND] N64: v.rom streamed from cart (%u bytes); ADPCM enabled\n", v_rom_size);
#else
		if (getenv("MVS64_NO_ADPCM")) {
			// A/B: force the no-ADPCM condition (what N64 used to be).
			adpcm_size = 0;
			plat_log("[SND] MVS64_NO_ADPCM: ADPCM force-disabled\n");
		} else if (getenv("MVS64_STREAM_ADPCM")) {
			// A/B: exercise the N64 streaming path on the PC build; the WAV
			// must be byte-identical to the resident path.
			plat_log("[SND] MVS64_STREAM_ADPCM: v.rom streamed (%u bytes)\n", v_rom_size);
		} else {
			vrom_resident = malloc(v_rom_size);
			if (vrom_resident) {
				vrom_read(0, vrom_resident, v_rom_size);
				plat_log("[SND] v.rom resident: %u bytes\n", v_rom_size);
			} else {
				plat_log("[SND] v.rom alloc failed; streaming ADPCM\n");
			}
		}
#endif
	}
	YM2610Init(YM_CLOCK, AUDIO_RATE,
		vrom_resident, adpcm_size,   // ADPCM-A (NULL buf + size>0 = streamed)
		vrom_resident, adpcm_size,   // ADPCM-B (shared)
		ym_timer_handler, ym_irq_handler);

	sound_reset();
	z80_active = 1;
	plat_log("[SND] Z80 sound CPU init (m.rom %u bytes)\n", m_rom_size);
}

void sound_reset(void) {
	// Initial 1:1 window mapping (matches gngeo cpu_z80_init).
	z80_bank[0] = M_ROM + 0x8000;
	z80_bank[1] = M_ROM + 0xC000;
	z80_bank[2] = M_ROM + 0xE000;
	z80_bank[3] = M_ROM + 0xF000;
	memset(z80_ram, 0, sizeof(z80_ram));
	sound_code = result_code = pending_command = 0;
	ym_timer_on[0] = ym_timer_on[1] = 0;
	z80_init(&cpu);
	cpu.read_byte  = z80_read;
	cpu.write_byte = z80_write;
	cpu.port_in    = z80_in;
	cpu.port_out   = z80_out;
#ifdef MVS64_CYCWRAP_TEST
	// Gate rig for the 2^32 cycle-counter wrap (the 17.9-minute permanent
	// silence): park cyc ~2 minutes of audio time before the wrap so a short
	// ares run crosses it during attract music. Unfixed builds freeze the
	// Z80 forever at the crossing; fixed builds play straight through.
	cpu.cyc = 0xFFFFFFFFul - 4000000ul * 120ul;
	plat_log("[CYCWRAP] cyc parked at %08lx (wrap in ~120s of audio)\n",
	         (unsigned long)cpu.cyc);
#endif
	YM2610Reset();
}

#ifdef SND_HEALTH
static int g_cmd_lost;   // 68k overwrote a command the Z80 never consumed
static int g_nmi_precap; // command NMI injected while Z80 still mid-handler
static int g_nmi_postcap;// command NMI handler didn't finish within the cap
#endif

void sound_write_command(uint8_t cmd) {
#ifdef SND_HEALTH
	// The NeoGeo sound latch holds ONE byte. If the previous command is still
	// pending (Z80 hasn't read port 0x00 yet), this write destroys it — on real
	// hardware the Z80 consumes within microseconds, but if our Z80 lags the
	// 68k, commands vanish and the music driver desyncs (missing note-offs /
	// never-started songs). Count it to prove/disprove that mechanism.
	if (pending_command) g_cmd_lost++;
#endif
	sound_code = cmd;
	pending_command = 1;
#ifdef MVS64_SNDTRACE
	trace_push(1, cmd);
#endif
#ifdef MVS64_FASTBOOT
	// Diagnostic only: the one-time SNK boot voice/jingle (cmd 0x01) runs the Z80
	// flat-out (~957k steps, not idle-skippable) and drops the emulator to ~4fps,
	// so ares takes minutes just to clear boot. It is silenced anyway. Skip its
	// processing and fake the echo-ack reply so boot is fast and we can reach
	// combat quickly for tracing. NOT for release builds.
	if (cmd == 0x01) { result_code = cmd; pending_command = 0; return; }
#endif
	if (!z80_active) return;
	// On N64 this runs inside the TLB/MMIO exception handler (68k sound-latch
	// write). The NeoGeo BIOS busy-waits for the reply, so the run must stay
	// synchronous. That is safe ONLY because the whole Z80+YM2610 command path
	// is now integer-only (the YM core precomputes its double math at init:
	// TimerBase_cyc8 / adpcma_step_base / freqbase16) — the handler leaves the
	// FPU disabled (SR.CU1 clear, no FP context saved), and the previous
	// mfc0/mtc0 CU1-re-enable trick had an unhandled CP0 hazard window that
	// wedged Mupen (BizHawk) at the first sound command (~frame 537) and was
	// fragile on real hardware.
	//
	// Deliver the NMI the way real-hardware timing would: never into the middle
	// of another handler. The audio pump stops the Z80 wherever its cycle budget
	// runs out — routinely INSIDE the YM-timer IRQ tick handler (~5 music ticks
	// per audio buffer), which is exactly the code that maintains channel state
	// and issues SSG/FM note-offs. The old fixed z80_run(300) injected the
	// command NMI right there, and the command processing trampled the tick
	// handler's half-updated state: the note-off for the live SSG chord was
	// never issued and the tone latched on forever (the stuck boot beep /
	// character-select chord). On real hardware the Z80 runs continuously, so
	// an NMI landing mid-tick is a microsecond-window fluke the driver
	// tolerates; our chunked execution made it near-certain during command
	// bursts. So: (1) if the Z80 is inside a handler or DI section (IFF1
	// clear), first let it run back to EI/main-loop; (2) inject the NMI;
	// (3) run the NMI handler to COMPLETION (RETN restores IFF1), so the 68k's
	// busy-wait sees the real reply and never re-sends into a half-done
	// handler. Both runs are cycle-capped so a driver phase that parks with DI
	// (the boot jingle's RAM wait loop) cannot stall the 68k exception handler;
	// on cap we inject/return anyway, which is exactly the old behavior.
	// NOTE on every cyc loop bound in this file: cpu.cyc is 32-bit on N64 and
	// WRAPS after 2^32 Z80 cycles = 17.9 minutes of audio time. A magnitude
	// compare (cyc < bound) fails closed at the wrap — bound overflows small,
	// the loop never runs, and since stepping is the only thing that advances
	// cyc the Z80 freezes FOREVER (2026-08-30 permanent-silence postmortem:
	// both the hardware and ares deaths integrate to exactly 2^32 cycles).
	// All bounds therefore use the wrap-safe signed-distance form.
	if (!cpu.iff1 && !cpu.halted) {   // a DI+HALT park only an NMI can wake:
		unsigned long cap = cpu.cyc + 1500;   // don't burn the cap stepping it
		while ((long)(cap - cpu.cyc) > 0 && !cpu.iff1 && !cpu.halted)
			z80_step(&cpu);
#ifdef SND_HEALTH
		if (!cpu.iff1) g_nmi_precap++;
#endif
	}
	z80_gen_nmi(&cpu);
	{
		unsigned long cap = cpu.cyc + 8000;
		while ((long)(cap - cpu.cyc) > 0 && (cpu.nmi_pending || !cpu.iff1)) {
			z80_service_level_irq();
			z80_step(&cpu);
		}
#ifdef SND_HEALTH
		if (!cpu.iff1) g_nmi_postcap++;
#endif
	}
}

uint8_t sound_read_status(void) {
	return result_code;
}

static void emit(int16_t *out, int from, int count) {
#if defined(SND_HEALTH) && defined(N64)
	uint32_t _t0 = TICKS_READ();
#endif
#if defined(N64) && defined(MVS64_RSPWP)
	/* Whole-pump deferred FM: deferred chunks write their final samples
	 * straight into this span at collect time, and non-WP chunks pack
	 * theirs directly in pass 4 (track C step 4); everything is complete
	 * before sound_gen_samples returns the buffer — see YM2610_wp_finish. */
	ym2610_wp_dest_base = out + from * 2;
	YM2610Update_stream(count);
	ym2610_wp_dest_base = NULL;
#else
	YM2610Update_stream(count);
#endif
#ifdef N64
#ifdef MVS64_RSPWP
	/* Track C step 4 (emit-copy elision): Update_stream packed the non-WP
	 * chunks straight into this span (same u32 big-endian pack the collect
	 * uses); WP chunks land at collect. play_buffer is no longer read here
	 * (the snd_dbg [SND] s0 probe below goes stale on this path). */
	YM2610_wp_mark_emitted();
#else
	// `out` is an UNCACHED AI buffer: every store is a separate RDRAM
	// transaction, so pack each stereo frame into ONE 32-bit store (big-endian:
	// high half = left = out[0]) — halves the uncached traffic vs two 16-bit
	// stores. A stereo frame is 4 bytes, so out+from*2 is always 4-aligned.
	{
		uint32_t *dst = (uint32_t *)(out + from * 2);
		for (int i = 0; i < count; i++)
			dst[i] = ((uint32_t)play_buffer[i * 2 + 0] << 16) | play_buffer[i * 2 + 1];
	}
#endif
#else
	for (int i = 0; i < count; i++) {
		out[(from + i) * 2 + 0] = (int16_t)play_buffer[i * 2 + 0];
		out[(from + i) * 2 + 1] = (int16_t)play_buffer[i * 2 + 1];
	}
#endif
#if defined(SND_HEALTH) && defined(N64)
	g_prof_ymt += TICKS_DISTANCE(_t0, TICKS_READ());
#endif
}

int sound_gen_samples(int16_t *out, int nsamples) {
	if (!z80_active) {
		memset(out, 0, (size_t)nsamples * 2 * sizeof(int16_t));
		return nsamples;
	}
#if defined(SND_HEALTH) && defined(N64)
	uint32_t _gen_t0 = TICKS_READ();
#endif
#ifdef MVS64_SNDTRACE
	trace_drain();   // flush 68k->Z80 commands + YM key on/off to the log (SD/USB)
#endif

	// Rate-agnostic: "nsamples" always means exactly nsamples/AUDIO_RATE seconds
	// of Z80+YM2610 time, derived from the requested sample count rather than a
	// hardcoded 1/60s. This lets the N64 backend drive us at the true 44100 Hz AI
	// rate (any buffer length) so music plays at correct pitch AND tempo even when
	// the 68k frame loop is slow. At nsamples=735 cyc_budget == Z80_CYCLES_PER_FRAME
	// exactly, so the PC/SDL and headless paths are byte-identical to before.
	// Sustained-underrun silence: keep running the Z80 (handshake/timers) but skip
	// the YM2610 synthesis and output zeros. The buffer is pre-zeroed; every emit()
	// below is gated on !silent so nothing overwrites it.
	const int silent = sound_silent;
	if (silent) memset(out, 0, (size_t)nsamples * 2 * sizeof(int16_t));

	const unsigned long frame_start = cpu.cyc;
	const unsigned long cyc_budget  =
		(unsigned long)((unsigned long long)nsamples * Z80_CLOCK / AUDIO_RATE);
	const unsigned long frame_end   = cpu.cyc + cyc_budget;
	int produced = 0;

	uint16_t last_back = 0xFFFF;        // idle-skip: target of previous back-branch
	struct z80snap spin_snap = {0,0,0}; // registers at last_back last time we hit it
	                                    // (zero-init only quiets maybe-uninitialized:
	                                    // every compare is guarded by spin_armed)
	int spin_armed = 0;                 // snapshot valid + no writes since it taken

	while ((long)(frame_end - cpu.cyc) > 0) {   // wrap-safe (see NMI note)
		// Service any due FM timers first (re-arms them forward + raises IRQ).
		// Wrap-safe signed compare: cpu.cyc is 32-bit on N64 and wraps at ~17min
		// once it advances at the true ~4MHz rate (deltas here are tiny, <budget).
		for (int c = 0; c < 2; c++)
			if (ym_timer_on[c] && (long)(cpu.cyc - ym_timer_deadline[c]) >= 0) {
#ifdef SND_HEALTH
				g_timer_fires[c]++;
#endif
				YM2610TimerOver(c);
			}

		// Next event boundary = nearest future timer deadline, else end of budget.
		// We run the Z80 up to it, idle-skipping any interrupt-wait spin (so we
		// don't single-step 4 MHz of wait loop). The timer fires exactly at the
		// deadline, so tempo stays correct; emit() below renders the FM audio for
		// the whole span whether stepped or skipped.
		unsigned long next = frame_end;
		for (int c = 0; c < 2; c++)
			if (ym_timer_on[c] && (long)(ym_timer_deadline[c] - cpu.cyc) > 0 &&
			    (long)(ym_timer_deadline[c] - next) < 0)
				next = ym_timer_deadline[c];

#if defined(SND_HEALTH) && defined(N64)
		uint32_t _zt0 = TICKS_READ();
#endif
#ifdef MVS64_Z80HIST
		if ((long)(next - cpu.cyc) > 0) g_z80_segs++;
#endif
#ifdef MVS64_Z80WARM
		int _zw_si = 0; uint32_t _zw_t = TICKS_READ();
		if ((long)(next - cpu.cyc) > 0) g_zw_seg++;
#endif
		while ((long)(next - cpu.cyc) > 0) {   // wrap-safe (see NMI note)
			z80_service_level_irq();   // must precede the HALT check: a
			                           // re-delivered tick wakes a halted CPU
#ifndef MVS64_NOIDLESKIP
			// A HALTed Z80 with nothing deliverable pending is pure dead time,
			// but it defeats the back-branch idle-skip below (HALT never
			// branches: each z80_step burns a flat 4 cycles in place) — the
			// boot jingle's DI/HALT park cost ~1M single-steps per wall second,
			// most of the boot-window Z80 load. Fast-forwarding to the next
			// event boundary is EXACT, not heuristic: the only wake sources are
			// the YM timer IRQ (which bounds `next`) and the command NMI
			// (which arrives between pump calls). A pending-but-masked IRQ
			// (DI+HALT) cannot wake it either — only an NMI can.
			if (cpu.halted && !cpu.nmi_pending &&
			    (!cpu.int_pending || !cpu.iff1)) {
#ifdef SND_HEALTH
				g_z80_skipcyc += (next - cpu.cyc); g_z80_skips++;
#endif
				cpu.cyc = next;
				break;
			}
#endif
			uint16_t pc0 = cpu.pc;
			z80_wrote = 0;
#ifdef MVS64_Z80HIST
			z80_hist[cpu.pc >> 4]++;
#endif
			z80_step(&cpu);
#ifdef SND_HEALTH
			g_z80_steps++;
#endif
#ifdef MVS64_Z80WARM
			if (++_zw_si == Z80WARM_N) {
				uint32_t t = TICKS_READ();
				g_zw_ct += TICKS_DISTANCE(_zw_t, t); g_zw_cs += Z80WARM_N; _zw_t = t;
			}
#endif
			if (z80_wrote) spin_armed = 0;      // any write breaks the pure spin
#ifndef MVS64_NOIDLESKIP
			// Self-jump spin (JP $ / JR $): the back-branch detector below
			// never sees it (pc == pc0, not <), so the boot jingle's DI park
			// at JP $FFFD was single-stepped flat out — ~half of ALL Z80
			// steps in a boot+fight run. Fast-forward it BIT-EXACTLY: each
			// iteration is exactly {cyc += c, R += 1} (pc/mem_ptr already
			// fixed points), and nothing can interrupt it before `next` when
			// no NMI is pending and the maskable path can't fire (IFF1 clear,
			// or no pending/level-held IRQ) — so k iterations land cyc on the
			// same overshoot stepping would, with the same R.
			if (cpu.pc == pc0 && !z80_wrote && !cpu.halted && !cpu.iff_delay &&
			    !cpu.nmi_pending &&
			    !(cpu.iff1 && (cpu.int_pending || ym_irq_level))) {
				uint8_t op = z80_read(NULL, pc0);
				unsigned c = op == 0xC3 ? 10 : op == 0x18 ? 12 : 0;
				long rem = (long)(next - cpu.cyc);
				if (c && rem > 0) {
					unsigned long k = ((unsigned long)rem + c - 1) / c;
					cpu.cyc += k * c;
					cpu.r = (uint8_t)((cpu.r & 0x80) | ((cpu.r + k) & 0x7f));
#ifdef SND_HEALTH
					g_z80_skipcyc += k * c; g_z80_skips++;
#endif
					break;
				}
			}
#endif
			if (cpu.pc < pc0) {                 // backward branch = loop edge
				if (cpu.pc == last_back) {      // repeated target = candidate spin
					// NOTE: do not gate this behind a repeat threshold. Delaying
					// the skip changes where cpu.cyc lands when the budget/timer
					// boundary arrives mid-spin (a natural step OVERSHOOTS
					// `next` by instruction granularity; the skip lands exactly
					// on it), which shifts YM timer phase = audible divergence.
					// Measured: a >=8-repeat gate broke the byte-identical WAV.
					struct z80snap now; z80_snap(&now, &cpu);
					if (spin_armed && z80_snap_eq(&now, &spin_snap)) {
#ifdef SND_HEALTH
						g_z80_skipcyc += (next - cpu.cyc); g_z80_skips++;
#endif
#ifndef MVS64_NOIDLESKIP
						cpu.cyc = next;         // identical iteration -> jump to event
						break;
#endif
					}
					spin_snap = now; spin_armed = 1;   // seed/refresh for next compare
				} else {
					last_back = cpu.pc; spin_armed = 0;  // new target: cheap path
				}
			}
		}
#if defined(SND_HEALTH) && defined(N64)
		g_prof_z80t += TICKS_DISTANCE(_zt0, TICKS_READ());
#endif
#ifdef MVS64_Z80WARM
		if (_zw_si < Z80WARM_N) { g_zw_ct += TICKS_DISTANCE(_zw_t, TICKS_READ()); g_zw_cs += _zw_si; }
		else { g_zw_wt += TICKS_DISTANCE(_zw_t, TICKS_READ()); g_zw_ws += _zw_si - Z80WARM_N; }
#endif

		// Generate samples up to the cycle-proportional point in the budget.
		int target = (int)((unsigned long long)(cpu.cyc - frame_start) *
		                    nsamples / cyc_budget);
		if (target > nsamples) target = nsamples;
		if (target > produced) { if (!silent) emit(out, produced, target - produced); produced = target; }
	}
	if (produced < nsamples && !silent) emit(out, produced, nsamples - produced);

#if defined(N64) && defined(MVS64_RSPWP)
	/* Cross-pump deferral (full WP-M2): tail chunks may still be in flight
	 * when we return — `out` is the platform's staging buffer, and the
	 * platform pump blocks on YM2610_wp_finish() before publishing it to
	 * the pull ring (usually at its NEXT entry, after the inter-pump 68k
	 * window has drained the RSP for free). NOTE: the [SNDRMS] probe below
	 * reads the tail spans before they are final — diagnostic only. */
	YM2610_wp_finish_async();
#endif

	if (snd_dbg)
		plat_log("[SND] z80 pc=%04x code=%02x result=%02x timers=%d%d s0=%d\n",
			cpu.pc, sound_code, result_code, ym_timer_on[0], ym_timer_on[1],
			(int)play_buffer[0]);

#ifdef SND_HEALTH
	// Audio-activity probe: report the RMS amplitude of the generated frame so
	// "is sound actually being produced" is verifiable headless (non-zero,
	// varying RMS = the Z80 music driver is feeding the YM2610).
	{
		static int sc = 0;
		uint64_t acc = 0; int pk = 0;
		for (int i = 0; i < nsamples * 2; i++) {
			int v = out[i]; if (v < 0) v = -v;
			acc += (uint64_t)v * v; if (v > pk) pk = v;
		}
		int rms = 0; if (nsamples) { uint64_t m = acc / (nsamples * 2); while ((uint64_t)(rms+1)*(rms+1) <= m) rms++; }
		if ((sc++ % 60) == 0) {
			// steps = z80 instrs actually run; skipcyc = idle cycles fast-forwarded.
			// High skipcyc:steps ratio = idle-skip working (cheap waits).
#if defined(SND_HEALTH) && defined(N64)
			// z80ms/ymms: wall ms spent stepping the Z80 vs synthesising the
			// YM2610 over the 60-call interval — the audio cost split.
			// lost = commands the 68k overwrote before the Z80 consumed them
			// (cumulative); t = FM timer A/B armed (music engine tick source).
			// irqre = level-held YM IRQs the edge model had dropped and we
			// re-delivered (each one was a lost music tick before this fix);
			// nmiw  = command NMIs delivered mid-handler (pre-run cap hit) /
			//         handlers that outran the completion cap.
			// sp/ha/if: Z80 stack pointer + halted + iff1 — the 2026-08-30
			// silent-death forensics (a slow stack leak descending through
			// the driver's RAM would show as sp marching down over minutes).
			plat_log("[SNDRMS] rms=%d peak=%d z80pc=%04x sp=%04x hi=%d%d code=%02x steps=%lu skips=%d skipcyc=%lu z80ms=%lu ymms=%lu genms=%lu lost=%d t=%d%d irqre=%d nmiw=%d,%d\n",
				rms, pk, cpu.pc, cpu.sp, cpu.halted ? 1 : 0,
				cpu.iff1 ? 1 : 0, sound_code,
				g_z80_steps, g_z80_skips, g_z80_skipcyc,
				(unsigned long)(g_prof_z80t / (TICKS_PER_SECOND / 1000)),
				(unsigned long)(g_prof_ymt / (TICKS_PER_SECOND / 1000)),
				(unsigned long)(g_prof_gen / (TICKS_PER_SECOND / 1000)),
				g_cmd_lost, ym_timer_on[0], ym_timer_on[1],
				g_irq_redeliver, g_nmi_precap, g_nmi_postcap);
#ifdef MVS64_Z80WARM
			// ticks are COUNT (cpu/2): host cyc/step = 2*ticks/steps
			plat_log("[Z80WARM] seg=%lu cold=%lu/%lu warm=%lu/%lu cyc/step cold=%lu warm=%lu\n",
				(unsigned long)g_zw_seg, (unsigned long)g_zw_ct, (unsigned long)g_zw_cs,
				(unsigned long)g_zw_wt, (unsigned long)g_zw_ws,
				(unsigned long)(g_zw_cs ? 2ull * g_zw_ct / g_zw_cs : 0),
				(unsigned long)(g_zw_ws ? 2ull * g_zw_wt / g_zw_ws : 0));
			g_zw_ct = g_zw_cs = g_zw_wt = g_zw_ws = g_zw_seg = 0;
#endif
			plat_log("[SNDTMR] fires=%d,%d\n", g_timer_fires[0], g_timer_fires[1]);
			g_timer_fires[0] = g_timer_fires[1] = 0;
#ifdef MVS64_Z80HIST
			{
				// Top-12 16-byte PC buckets this interval + segment count.
				char hb[200]; int hn = 0;
				hn += snprintf(hb + hn, sizeof hb - (size_t)hn, "segs=%lu",
				               (unsigned long)g_z80_segs);
				for (int k = 0; k < 12 && hn < (int)sizeof hb - 16; k++) {
					uint32_t best = 0; int bi = -1;
					for (int j = 0; j < 4096; j++)
						if (z80_hist[j] > best) { best = z80_hist[j]; bi = j; }
					if (bi < 0 || !best) break;
					hn += snprintf(hb + hn, sizeof hb - (size_t)hn,
					               " %03x0=%lu", bi, (unsigned long)best);
					z80_hist[bi] = 0;   // consumed (rest cleared below)
				}
				plat_log("[Z80HIST] %s\n", hb);
				memset(z80_hist, 0, sizeof z80_hist);
				g_z80_segs = 0;
			}
#endif
			g_prof_z80t = 0; g_prof_ymt = 0; g_prof_gen = 0;
			{
				// Latched-voice hunt: snapshot every tone-holding state element
				// (SSG regs, FM key/EG state, ADPCM activity). A channel that
				// stays hot for many seconds while the stuck tone is audible
				// is the culprit; see ym2610_dbg_state for field meanings.
				char ymst[128];
				ym2610_dbg_state(ymst, sizeof ymst);
				plat_log("[YMSTATE] %s\n", ymst);
			}
#ifdef MVS64_YMPROF
			{
				extern uint32_t ym_prof[5];
				plat_log("[YMPROF] egms=%lu fmms=%lu ssgms=%lu adpcmms=%lu mixms=%lu\n",
					(unsigned long)(ym_prof[0] / (TICKS_PER_SECOND / 1000)),
					(unsigned long)(ym_prof[1] / (TICKS_PER_SECOND / 1000)),
					(unsigned long)(ym_prof[2] / (TICKS_PER_SECOND / 1000)),
					(unsigned long)(ym_prof[3] / (TICKS_PER_SECOND / 1000)),
					(unsigned long)(ym_prof[4] / (TICKS_PER_SECOND / 1000)));
				memset(ym_prof, 0, sizeof(uint32_t) * 5);
			}
#endif
#else
			plat_log("[SNDRMS] rms=%d peak=%d z80pc=%04x code=%02x steps=%lu skips=%d skipcyc=%lu lost=%d t=%d%d\n",
				rms, pk, cpu.pc, sound_code,
				g_z80_steps, g_z80_skips, g_z80_skipcyc,
				g_cmd_lost, ym_timer_on[0], ym_timer_on[1]);
#endif
			g_z80_steps = 0; g_z80_skipcyc = 0; g_z80_skips = 0;
		}
	}
#endif
#if defined(SND_HEALTH) && defined(N64)
	g_prof_gen += TICKS_DISTANCE(_gen_t0, TICKS_READ());
#endif
	return nsamples;
}
