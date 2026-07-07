/***************************************************************************

  ym2610.h

  Header file for software emulation for YAMAHA YM-2610 sound generator

***************************************************************************/
// Vendored from gngeo (rofl0r/gngeo) src/ym2610/. YM2610 core is MAME-derived;
// see ym2610/LICENSE.mame (MAME License — non-commercial). Local mvs64 changes
// are marked "MVS64:".

#ifndef _YM2610_H_
#define _YM2610_H_

#include "mvs.h"

/* MVS64: busy-flag/timer time source — INTEGER Z80 cycles (4 MHz), supplied by
 * the sound module. The core must be FPU-FREE at runtime: on N64, YM register
 * writes execute inside the TLB/MMIO exception handler, where the FPU is
 * disabled (SR.CU1 clear) and no FP context is saved — any float/double math
 * there faults into a wild jump. All double math happens at init time only. */
u32 ym2610_time_now_cyc(void);
#define FM_GET_TIME_NOW_CYC() ym2610_time_now_cyc()
#define FM_TIMEBASE_CYC_PER_SEC 4000000 /* Z80 clock: the cycle domain above */

#if defined(MVS64_AUTOINPUT) || defined(MVS64_SNDHEALTH)
/* MVS64 diagnostic: snapshot of all tone-holding state ([YMSTATE] telemetry) */
int ym2610_dbg_state(char *o, int n);
#endif

/* MVS64: streamed ADPCM sample fetch, implemented by the sound module. Used
 * when the ADPCM sample ROM is not resident (pcmbuf NULL but pcmsize > 0): the
 * 7MB NeoGeo v.rom cannot live in RDRAM, so bytes come from small per-voice
 * window caches backed by cart streaming. win 0-5 = ADPCM-A ch, 6 = ADPCM-B.
 * The hit path is inlined here (one fetch per decoded byte, synthesis-hot);
 * only a window miss calls into the sound module to stream from cart. */
#define YM2610_VWIN_SIZE 2048
struct ym2610_vwin {
	u32 base;
	int valid;
	u8  buf[YM2610_VWIN_SIZE] __attribute__((aligned(16)));
};
extern struct ym2610_vwin ym2610_vwin[7];
u8 ym2610_vrom_fetch_slow(int win, u32 addr);
static inline u8 ym2610_vrom_fetch(int win, u32 addr) {
	struct ym2610_vwin *w = &ym2610_vwin[win];
	if (w->valid && w->base == (addr & ~(u32)(YM2610_VWIN_SIZE - 1)))
		return w->buf[addr & (YM2610_VWIN_SIZE - 1)];
	return ym2610_vrom_fetch_slow(win, addr);
}

typedef s16 FMSAMPLE;
typedef s32 FMSAMPLE_MIX;
#define TIMER_SH		16  /* 16.16 fixed point (timers calculations)    */

/* MVS64: was (channel, count, double stepTime); now passes the period as
 * integer Z80 cycles directly (0 = stop timer), so no FP crosses the boundary. */
typedef void (*FM_TIMERHANDLER)(int channel, u32 cycles);
typedef void (*FM_IRQHANDLER)(int irq);

void YM2610Init(int baseclock, int rate,
		void *pcmroma, int pcmsizea,
		void *pcmromb, int pcmsizeb,
		FM_TIMERHANDLER TimerHandler,
		FM_IRQHANDLER IRQHandler);
void YM2610ChangeSamplerate(int rate);
void YM2610Reset(void);
int  YM2610Write(int addr, u8 value);
u8   YM2610Read(int addr);
int  YM2610TimerOver(int channel);

void YM2610Update(int *p);
void YM2610Update_stream(int length);

#if defined(N64) && defined(MVS64_RSPWP)
/* Whole-pump deferred FM (WHOLEPUMP-DESIGN.md): the caller (emit) points
 * dest_base at the uncached AI position of the span BEFORE calling
 * YM2610Update_stream — deferred chunks write their final samples there at
 * collect time. YM2610_wp_finish() drains everything in flight and MUST run
 * before the pump returns its buffer to the AI. */
extern short *ym2610_wp_dest_base;
void YM2610_wp_finish(void);
void YM2610_wp_mark_emitted(void);
#endif

#ifdef SOUND_TEST
void YM2610Update_SoundTest(int p);
#endif

#ifdef SAVE_STATE
STATE_SAVE( ym2610 );
STATE_LOAD( ym2610 );
#endif

#endif /* _YM2610_H_ */
