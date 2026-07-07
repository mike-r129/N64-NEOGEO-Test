/***************************************************************************

 ym2610.c

 Software emulation for YAMAHA YM-2610 sound generator

 Copyright (C) 2001, 2002, 2003 Jarek Burczynski (bujar at mame dot net)
 Copyright (C) 1998 Tatsuyuki Satoh, MultiArcadeMachineEmulator development

 Version 1.4 (final beta)

 Comming from NJ pspmvs emu comming from Mame

 ***************************************************************************/

/*--------------------------------------------------------------------------

 History:

 03-08-2003 Jarek Burczynski:
 - fixed YM2608 initial values (after the reset)
 - fixed flag and irqmask handling (YM2608)
 - fixed BUFRDY flag handling (YM2608)

 14-06-2003 Jarek Burczynski:
 - implemented all of the YM2608 status register flags
 - implemented support for external memory read/write via YM2608
 - implemented support for deltat memory limit register in YM2608 emulation

 22-05-2003 Jarek Burczynski:
 - fixed LFO PM calculations (copy&paste bugfix)

 08-05-2003 Jarek Burczynski:
 - fixed SSG support

 22-04-2003 Jarek Burczynski:
 - implemented 100% correct LFO generator (verified on real YM2610 and YM2608)

 15-04-2003 Jarek Burczynski:
 - added support for YM2608's register 0x110 - status mask

 01-12-2002 Jarek Burczynski:
 - fixed register addressing in YM2608, YM2610, YM2610B chips. (verified on real YM2608)
 The addressing patch used for early Neo-Geo games can be removed now.

 26-11-2002 Jarek Burczynski, Nicola Salmoria:
 - recreated YM2608 ADPCM ROM using data from real YM2608's output which leads to:
 - added emulation of YM2608 drums.
 - output of YM2608 is two times lower now - same as YM2610 (verified on real YM2608)

 16-08-2002 Jarek Burczynski:
 - binary exact Envelope Generator (verified on real YM2203);
 identical to YM2151
 - corrected 'off by one' error in feedback calculations (when feedback is off)
 - corrected connection (algorithm) calculation (verified on real YM2203 and YM2610)

 18-12-2001 Jarek Burczynski:
 - added SSG-EG support (verified on real chip)

 12-08-2001 Jarek Burczynski:
 - corrected sin_tab and tl_tab data (verified on real chip)
 - corrected feedback calculations (verified on real chip)
 - corrected phase generator calculations (verified on real chip)
 - corrected envelope generator calculations (verified on real chip)
 - corrected volume level.
 - changed YM2610Update() function:
 this was needed to calculate YM2610 FM channels output correctly.
 (Each FM channel is calculated as in other chips, but the output of the
 channel gets shifted right by one *before* sending to accumulator.
 That was impossible to do with previous implementation).

 23-07-2001 Jarek Burczynski, Nicola Salmoria:
 - corrected ADPCM type A algorithm and tables (verified on real chip)

 11-06-2001 Jarek Burczynski:
 - corrected end of sample bug in OPNB_ADPCM_CALC_CH.
 Real YM2610 checks for equality between current and end addresses
 (only 20 LSB bits).

 08-12-1998 hiro-shi:
 - rename ADPCMA -> ADPCMB, ADPCMB -> DELTAT
 - move ROM limit check.(CALC_CH? -> 2610Write1/2)
 - test program (ADPCMB_TEST)
 - move ADPCM A/B end check.
 - ADPCMB repeat flag(no check)
 - change ADPCM volume rate (8->16) (32->48).

 09-12-1998 hiro-shi:
 - change ADPCM volume. (8->16, 48->64)
 - replace ym2610 ch0/3 (YM-2610B)
 - init cur_chip (restart bug fix)
 - change ADPCM_SHIFT (10->8) missing bank change 0x4000-0xffff.
 - add ADPCM_SHIFT_MASK
 - change ADPCMA_DECODE_MIN/MAX.

 ----------------------------------------------------------------------------

 comment of hiro-shi(Hiromitsu Shioya):
 YM2610 = OPN-B
 YM2610 : PSG:3ch FM:4ch ADPCM(18.5KHz):6ch DeltaT ADPCM:1ch

 --------------------------------------------------------------------------*/
// Vendored from gngeo (rofl0r/gngeo) src/ym2610/ym2610.c. MAME-derived YM2610
// core. License: MAME License (non-commercial) — see ym2610/LICENSE.mame.
// Local mvs64 changes are marked "MVS64:".

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#include "mvs.h"
// MVS64: dropped "../state.h" (save-state, #ifdef SAVE_STATE — unused) and
// "2610intf.h" (gngeo host interface — not vendored). YM2610UpdateRequest()
// is now a no-op in mvs.h.
#include "ym2610.h"

#ifndef PI
#define PI 3.14159265358979323846
#endif

/* MVS64: -DMVS64_YMPROF (N64) synthesis cost split, filled in
 * YM2610Update_stream, reported+reset by the sound module's [SNDRMS] print.
 * 0=sched(LFO+EG timer) 1=FM(EG replay+chan_calc) 2=SSG 3=ADPCM 4=mix+output. */
#if defined(MVS64_YMPROF) && defined(N64)
#include <libdragon.h>
uint32_t ym_prof[5];
#endif

/* select timer system internal or external */
#define FM_INTERNAL_TIMER 0

/* --- speedup optimize --- */
/* busy flag enulation , The definition of FM_GET_TIME_NOW() is necessary. */
#define FM_BUSY_FLAG_SUPPORT 1

/*------------------------------------------------------------------------*/

#define FREQ_SH			16  /* 16.16 fixed point (frequency calculations) */
#define EG_SH			16  /* 16.16 fixed point (envelope generator timing) */
#define LFO_SH			24  /*  8.24 fixed point (LFO calculations)       */
#define TIMER_SH		16  /* 16.16 fixed point (timers calculations)    */

#define FREQ_MASK		((1<<FREQ_SH)-1)

#define ENV_BITS		10
#define ENV_LEN			(1<<ENV_BITS)
#define ENV_STEP		(128.0/ENV_LEN)

#define MAX_ATT_INDEX	(ENV_LEN-1) /* 1023 */
#define MIN_ATT_INDEX	(0)			/* 0 */

#define EG_ATT			4
#define EG_DEC			3
#define EG_SUS			2
#define EG_REL			1
#define EG_OFF			0

#define SIN_BITS		10
#define SIN_LEN			(1<<SIN_BITS)
#define SIN_MASK		(SIN_LEN-1)

#define TL_RES_LEN		(256) /* 8 bits addressing (real chip) */

#define FINAL_SH	(0)
#define MAXOUT		(+32767)
#define MINOUT		(-32768)

/*  TL_TAB_LEN is calculated as:
 *   13 - sinus amplitude bits     (Y axis)
 *   2  - sinus sign bit           (Y axis)
 *   TL_RES_LEN - sinus resolution (X axis)
 */
#define TL_TAB_LEN (13*2*TL_RES_LEN)
/* MVS64: tl_tab (13*2*256 entries, 26KB as int) is mathematically
 * redundant: entry [x*2+s + i*2*256] == (s? -1:1) * (base[x] >> i). The
 * VR4300's dcache is 8KB direct-mapped, so the scattered 13-26KB table was
 * the synthesis loop's dominant miss source; a 512-byte base table stays
 * cache-resident and the >>/negate cost ~4 cycles vs ~50+ for a miss.
 * Values are bit-identical (base[x] is positive, so the arithmetic shift
 * and the sign applied after the shift reproduce the old entries exactly —
 * that is precisely how OPNInitTable built them). */
static s16 ALIGN_DATA tl_tab_base[TL_RES_LEN];

INLINE signed int tl_tab_lookup(unsigned int p) {
	int v = tl_tab_base[(p >> 1) & (TL_RES_LEN - 1)] >> (p >> 9);
	return (p & 1) ? -v : v;
}

#define ENV_QUIET		(TL_TAB_LEN>>3)

/* sin waveform table in 'decibel' scale */
static u16 ALIGN_DATA sin_tab[SIN_LEN];  /* MVS64: values < 2*TL_TAB_LEN, see above */

/* sustain level table (3dB per step) */
/* bit0, bit1, bit2, bit3, bit4, bit5, bit6 */
/* 1,    2,    4,    8,    16,   32,   64   (value)*/
/* 0.75, 1.5,  3,    6,    12,   24,   48   (dB)*/

/* 0 - 15: 0, 3, 6, 9,12,15,18,21,24,27,30,33,36,39,42,93 (dB)*/
#define SC(db) (u32) ( db * (4.0/ENV_STEP) )
static const u32 ALIGN_DATA sl_table[16] = { SC( 0), SC( 1), SC( 2), SC(3 ),
		SC(4 ), SC(5 ), SC(6 ), SC( 7), SC( 8), SC( 9), SC(10), SC(11),
		SC(12), SC(13), SC(14), SC(31) };
#undef SC

#define RATE_STEPS (8)
static const u8 ALIGN_DATA eg_inc[19 * RATE_STEPS] = {

/*cycle:0 1  2 3  4 5  6 7*/

/* 0 */0, 1, 0, 1, 0, 1, 0, 1, /* rates 00..11 0 (increment by 0 or 1) */
/* 1 */0, 1, 0, 1, 1, 1, 0, 1, /* rates 00..11 1 */
/* 2 */0, 1, 1, 1, 0, 1, 1, 1, /* rates 00..11 2 */
/* 3 */0, 1, 1, 1, 1, 1, 1, 1, /* rates 00..11 3 */

/* 4 */1, 1, 1, 1, 1, 1, 1, 1, /* rate 12 0 (increment by 1) */
/* 5 */1, 1, 1, 2, 1, 1, 1, 2, /* rate 12 1 */
/* 6 */1, 2, 1, 2, 1, 2, 1, 2, /* rate 12 2 */
/* 7 */1, 2, 2, 2, 1, 2, 2, 2, /* rate 12 3 */

/* 8 */2, 2, 2, 2, 2, 2, 2, 2, /* rate 13 0 (increment by 2) */
/* 9 */2, 2, 2, 4, 2, 2, 2, 4, /* rate 13 1 */
/*10 */2, 4, 2, 4, 2, 4, 2, 4, /* rate 13 2 */
/*11 */2, 4, 4, 4, 2, 4, 4, 4, /* rate 13 3 */

/*12 */4, 4, 4, 4, 4, 4, 4, 4, /* rate 14 0 (increment by 4) */
/*13 */4, 4, 4, 8, 4, 4, 4, 8, /* rate 14 1 */
/*14 */4, 8, 4, 8, 4, 8, 4, 8, /* rate 14 2 */
/*15 */4, 8, 8, 8, 4, 8, 8, 8, /* rate 14 3 */

/*16 */8, 8, 8, 8, 8, 8, 8, 8, /* rates 15 0, 15 1, 15 2, 15 3 (increment by 8) */
/*17 */16, 16, 16, 16, 16, 16, 16, 16, /* rates 15 2, 15 3 for attack */
/*18 */0, 0, 0, 0, 0, 0, 0, 0, /* infinity rates for attack and decay(s) */
};

#define O(a) (a*RATE_STEPS)

/*note that there is no O(17) in this table - it's directly in the code */
static const u8 ALIGN_DATA eg_rate_select[32+64+32]={	/* Envelope Generator rates (32 + 64 rates + 32 RKS) */
/* 32 infinite time rates */O(18), O(18), O(18), O(18), O(18), O(18), O(18),
		O(18), O(18), O(18), O(18), O(18), O(18), O(18), O(18), O(18),
		O(18), O(18), O(18), O(18), O(18), O(18), O(18), O(18), O(18),
		O(18), O(18), O(18), O(18), O(18), O(18), O(18),

		/* rates 00-11 */O( 0), O( 1), O( 2), O( 3), O( 0), O( 1), O( 2), O( 3),
		O( 0), O( 1), O( 2), O( 3), O( 0), O( 1), O( 2), O( 3), O( 0),
		O( 1), O( 2), O( 3), O( 0), O( 1), O( 2), O( 3), O( 0), O( 1),
		O( 2), O( 3), O( 0), O( 1), O( 2), O( 3), O( 0), O( 1), O( 2),
		O( 3), O( 0), O( 1), O( 2), O( 3), O( 0), O( 1), O( 2), O( 3),
		O( 0), O( 1), O( 2), O( 3),

		/* rate 12 */O( 4), O( 5), O( 6), O( 7),

		/* rate 13 */O( 8), O( 9), O(10), O(11),

		/* rate 14 */O(12), O(13), O(14), O(15),

		/* rate 15 */O(16), O(16), O(16), O(16),

		/* 32 dummy rates (same as 15 3) */O(16), O(16), O(16), O(16), O(16),
		O(16), O(16), O(16), O(16), O(16), O(16), O(16), O(16), O(16),
		O(16), O(16), O(16), O(16), O(16), O(16), O(16), O(16), O(16),
		O(16), O(16), O(16), O(16), O(16), O(16), O(16), O(16), O(16)

};
#undef O

/*rate  0,    1,    2,   3,   4,   5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15*/
/*shift 11,   10,   9,   8,   7,   6,  5,  4,  3,  2, 1,  0,  0,  0,  0,  0 */
/*mask  2047, 1023, 511, 255, 127, 63, 31, 15, 7,  3, 1,  0,  0,  0,  0,  0 */

#define O(a) (a*1)
static const u8 ALIGN_DATA eg_rate_shift[32+64+32]={	/* Envelope Generator counter shifts (32 + 64 rates + 32 RKS) */
/* 32 infinite time rates */O(0), O(0), O(0), O(0), O(0), O(0), O(0), O(0),
		O(0), O(0), O(0), O(0), O(0), O(0), O(0), O(0), O(0), O(0), O(0),
		O(0), O(0), O(0), O(0), O(0), O(0), O(0), O(0), O(0), O(0), O(0),
		O(0), O(0),

		/* rates 00-11 */O(11), O(11), O(11), O(11), O(10), O(10), O(10), O(10),
		O( 9), O( 9), O( 9), O( 9), O( 8), O( 8), O( 8), O( 8), O( 7),
		O( 7), O( 7), O( 7), O( 6), O( 6), O( 6), O( 6), O( 5), O( 5),
		O( 5), O( 5), O( 4), O( 4), O( 4), O( 4), O( 3), O( 3), O( 3),
		O( 3), O( 2), O( 2), O( 2), O( 2), O( 1), O( 1), O( 1), O( 1),
		O( 0), O( 0), O( 0), O( 0),

		/* rate 12 */O( 0), O( 0), O( 0), O( 0),

		/* rate 13 */O( 0), O( 0), O( 0), O( 0),

		/* rate 14 */O( 0), O( 0), O( 0), O( 0),

		/* rate 15 */O( 0), O( 0), O( 0), O( 0),

		/* 32 dummy rates (same as 15 3) */O( 0), O( 0), O( 0), O( 0), O( 0),
		O( 0), O( 0), O( 0), O( 0), O( 0), O( 0), O( 0), O( 0), O( 0),
		O( 0), O( 0), O( 0), O( 0), O( 0), O( 0), O( 0), O( 0), O( 0),
		O( 0), O( 0), O( 0), O( 0), O( 0), O( 0), O( 0), O( 0), O( 0)

};
#undef O

static const u8 ALIGN_DATA dt_tab[4 * 32] = {
/* this is YM2151 and YM2612 phase increment data (in 10.10 fixed point format)*/
/* FD=0 */
0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0,
		/* FD=1 */
		0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5,
		5, 6, 6, 7, 8, 8, 8, 8,
		/* FD=2 */
		1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 9, 10,
		11, 12, 13, 14, 16, 16, 16, 16,
		/* FD=3 */
		2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 9, 10, 11, 12, 13,
		14, 16, 17, 19, 20, 22, 22, 22, 22 };

/* OPN key frequency number -> key code follow table */
/* fnum higher 4bit -> keycode lower 2bit */
static const u8 ALIGN_DATA opn_fktable[16] = { 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 3,
		3, 3, 3, 3, 3 };

/* 8 LFO speed parameters */
/* each value represents number of samples that one LFO level will last for */
static const u32 ALIGN_DATA lfo_samples_per_step[8] = { 108, 77, 71, 67, 62, 44,
		8, 5 };

/*There are 4 different LFO AM depths available, they are:
 0 dB, 1.4 dB, 5.9 dB, 11.8 dB
 Here is how it is generated (in EG steps):

 11.8 dB = 0, 2, 4, 6, 8, 10,12,14,16...126,126,124,122,120,118,....4,2,0
 5.9 dB = 0, 1, 2, 3, 4, 5, 6, 7, 8....63, 63, 62, 61, 60, 59,.....2,1,0
 1.4 dB = 0, 0, 0, 0, 1, 1, 1, 1, 2,...15, 15, 15, 15, 14, 14,.....0,0,0

 (1.4 dB is loosing precision as you can see)

 It's implemented as generator from 0..126 with step 2 then a shift
 right N times, where N is:
 8 for 0 dB
 3 for 1.4 dB
 1 for 5.9 dB
 0 for 11.8 dB
 */
static const u8 lfo_ams_depth_shift[4] = { 8, 3, 1, 0 };

/*There are 8 different LFO PM depths available, they are:
 0, 3.4, 6.7, 10, 14, 20, 40, 80 (cents)

 Modulation level at each depth depends on F-NUMBER bits: 4,5,6,7,8,9,10
 (bits 8,9,10 = FNUM MSB from OCT/FNUM register)

 Here we store only first quarter (positive one) of full waveform.
 Full table (lfo_pm_table) containing all 128 waveforms is build
 at run (init) time.

 One value in table below represents 4 (four) basic LFO steps
 (1 PM step = 4 AM steps).

 For example:
 at LFO SPEED=0 (which is 108 samples per basic LFO step)
 one value from "lfo_pm_output" table lasts for 432 consecutive
 samples (4*108=432) and one full LFO waveform cycle lasts for 13824
 samples (32*432=13824; 32 because we store only a quarter of whole
 waveform in the table below)
 */
static const u8 ALIGN_DATA lfo_pm_output[7 * 8][8] = { /* 7 bits meaningful (of F-NUMBER), 8 LFO output levels per one depth (out of 32), 8 LFO depths */
/* FNUM BIT 4: 000 0001xxxx */
/* DEPTH 0 */{ 0, 0, 0, 0, 0, 0, 0, 0 },
/* DEPTH 1 */{ 0, 0, 0, 0, 0, 0, 0, 0 },
/* DEPTH 2 */{ 0, 0, 0, 0, 0, 0, 0, 0 },
/* DEPTH 3 */{ 0, 0, 0, 0, 0, 0, 0, 0 },
/* DEPTH 4 */{ 0, 0, 0, 0, 0, 0, 0, 0 },
/* DEPTH 5 */{ 0, 0, 0, 0, 0, 0, 0, 0 },
/* DEPTH 6 */{ 0, 0, 0, 0, 0, 0, 0, 0 },
/* DEPTH 7 */{ 0, 0, 0, 0, 1, 1, 1, 1 },

/* FNUM BIT 5: 000 0010xxxx */
/* DEPTH 0 */{ 0, 0, 0, 0, 0, 0, 0, 0 },
/* DEPTH 1 */{ 0, 0, 0, 0, 0, 0, 0, 0 },
/* DEPTH 2 */{ 0, 0, 0, 0, 0, 0, 0, 0 },
/* DEPTH 3 */{ 0, 0, 0, 0, 0, 0, 0, 0 },
/* DEPTH 4 */{ 0, 0, 0, 0, 0, 0, 0, 0 },
/* DEPTH 5 */{ 0, 0, 0, 0, 0, 0, 0, 0 },
/* DEPTH 6 */{ 0, 0, 0, 0, 1, 1, 1, 1 },
/* DEPTH 7 */{ 0, 0, 1, 1, 2, 2, 2, 3 },

/* FNUM BIT 6: 000 0100xxxx */
/* DEPTH 0 */{ 0, 0, 0, 0, 0, 0, 0, 0 },
/* DEPTH 1 */{ 0, 0, 0, 0, 0, 0, 0, 0 },
/* DEPTH 2 */{ 0, 0, 0, 0, 0, 0, 0, 0 },
/* DEPTH 3 */{ 0, 0, 0, 0, 0, 0, 0, 0 },
/* DEPTH 4 */{ 0, 0, 0, 0, 0, 0, 0, 1 },
/* DEPTH 5 */{ 0, 0, 0, 0, 1, 1, 1, 1 },
/* DEPTH 6 */{ 0, 0, 1, 1, 2, 2, 2, 3 },
/* DEPTH 7 */{ 0, 0, 2, 3, 4, 4, 5, 6 },

/* FNUM BIT 7: 000 1000xxxx */
/* DEPTH 0 */{ 0, 0, 0, 0, 0, 0, 0, 0 },
/* DEPTH 1 */{ 0, 0, 0, 0, 0, 0, 0, 0 },
/* DEPTH 2 */{ 0, 0, 0, 0, 0, 0, 1, 1 },
/* DEPTH 3 */{ 0, 0, 0, 0, 1, 1, 1, 1 },
/* DEPTH 4 */{ 0, 0, 0, 1, 1, 1, 1, 2 },
/* DEPTH 5 */{ 0, 0, 1, 1, 2, 2, 2, 3 },
/* DEPTH 6 */{ 0, 0, 2, 3, 4, 4, 5, 6 },
/* DEPTH 7 */{ 0, 0, 4, 6, 8, 8, 0xa, 0xc },

/* FNUM BIT 8: 001 0000xxxx */
/* DEPTH 0 */{ 0, 0, 0, 0, 0, 0, 0, 0 },
/* DEPTH 1 */{ 0, 0, 0, 0, 1, 1, 1, 1 },
/* DEPTH 2 */{ 0, 0, 0, 1, 1, 1, 2, 2 },
/* DEPTH 3 */{ 0, 0, 1, 1, 2, 2, 3, 3 },
/* DEPTH 4 */{ 0, 0, 1, 2, 2, 2, 3, 4 },
/* DEPTH 5 */{ 0, 0, 2, 3, 4, 4, 5, 6 },
/* DEPTH 6 */{ 0, 0, 4, 6, 8, 8, 0xa, 0xc },
/* DEPTH 7 */{ 0, 0, 8, 0xc, 0x10, 0x10, 0x14, 0x18 },

/* FNUM BIT 9: 010 0000xxxx */
/* DEPTH 0 */{ 0, 0, 0, 0, 0, 0, 0, 0 },
/* DEPTH 1 */{ 0, 0, 0, 0, 2, 2, 2, 2 },
/* DEPTH 2 */{ 0, 0, 0, 2, 2, 2, 4, 4 },
/* DEPTH 3 */{ 0, 0, 2, 2, 4, 4, 6, 6 },
/* DEPTH 4 */{ 0, 0, 2, 4, 4, 4, 6, 8 },
/* DEPTH 5 */{ 0, 0, 4, 6, 8, 8, 0xa, 0xc },
/* DEPTH 6 */{ 0, 0, 8, 0xc, 0x10, 0x10, 0x14, 0x18 },
/* DEPTH 7 */{ 0, 0, 0x10, 0x18, 0x20, 0x20, 0x28, 0x30 },

/* FNUM BIT10: 100 0000xxxx */
/* DEPTH 0 */{ 0, 0, 0, 0, 0, 0, 0, 0 },
/* DEPTH 1 */{ 0, 0, 0, 0, 4, 4, 4, 4 },
/* DEPTH 2 */{ 0, 0, 0, 4, 4, 4, 8, 8 },
/* DEPTH 3 */{ 0, 0, 4, 4, 8, 8, 0xc, 0xc },
/* DEPTH 4 */{ 0, 0, 4, 8, 8, 8, 0xc, 0x10 },
/* DEPTH 5 */{ 0, 0, 8, 0xc, 0x10, 0x10, 0x14, 0x18 },
/* DEPTH 6 */{ 0, 0, 0x10, 0x18, 0x20, 0x20, 0x28, 0x30 },
/* DEPTH 7 */{ 0, 0, 0x20, 0x30, 0x40, 0x40, 0x50, 0x60 },

};

/* all 128 LFO PM waveforms */
/* MVS64: narrowed from s32 (128KB!) — entries are ±(sum of u8s) so ±255
 * worst-case, well inside s16. Halves the table to 64KB to cut dcache/RDRAM
 * traffic when LFO PM is active. */
static s16 ALIGN_DATA lfo_pm_table[128 * 8 * 32]; /* 128 combinations of 7 bits meaningful (of F-NUMBER), 8 LFO depths, 32 LFO output levels per one depth */

/*----------------------------------
 for SSG emulator
 -----------------------------------*/

#define SSG_MAX_OUTPUT 0x7fff
#define SSG_STEP 0x8000

/* SSG register ID */
#define SSG_AFINE		(0)
#define SSG_ACOARSE		(1)
#define SSG_BFINE		(2)
#define SSG_BCOARSE		(3)
#define SSG_CFINE		(4)
#define SSG_CCOARSE		(5)
#define SSG_NOISEPER	(6)
#define SSG_ENABLE		(7)
#define SSG_AVOL		(8)
#define SSG_BVOL		(9)
#define SSG_CVOL		(10)
#define SSG_EFINE		(11)
#define SSG_ECOARSE		(12)
#define SSG_ESHAPE		(13)

#define SSG_PORTA		(14)
#define SSG_PORTB		(15)

/* register number to channel number , slot offset */
#define OPN_CHAN(N) (N&3)
#define OPN_SLOT(N) ((N>>2)&3)

/* slot number */
#define SLOT1 0
#define SLOT2 2
#define SLOT3 1
#define SLOT4 3

/* bit0 = Right enable , bit1 = Left enable */
#define OUTD_RIGHT  1
#define OUTD_LEFT   2
#define OUTD_CENTER 3

/* struct describing a single operator (SLOT) */
typedef struct {
	s32 *DT; /* detune          :dt_tab[DT] */
	u8 KSR; /* key scale rate  :3-KSR */
	u32 ar; /* attack rate  */
	u32 d1r; /* decay rate   */
	u32 d2r; /* sustain rate */
	u32 rr; /* release rate */
	u8 ksr; /* key scale rate  :kcode>>(3-KSR) */
	u32 mul; /* multiple        :ML_TABLE[ML] */
	/* Phase Generator */u32 phase; /* phase counter */
	u32 Incr; /* phase step */
	/* Envelope Generator */u8 state; /* phase type */
	u32 tl; /* total level: TL << 3 */
	s32 volume; /* envelope counter */
	u32 sl; /* sustain level:sl_table[SL] */
	u32 vol_out; /* current output from EG circuit (without AM from LFO) */
	/* MVS64: the four per-state EG rate shifts live in one array indexed by
	 * SLOT->state (EG_REL/SUS/DEC/ATT; [EG_OFF] unused), so the envelope
	 * advance can test "is this slot due at this eg_cnt" with one load
	 * before entering the state switch. The original field names are
	 * macro-aliased right below, so every existing reader/writer compiles
	 * unchanged and the array stays the single source of truth. */
	u8 eg_shv[5];
	u8 eg_sel_ar; /*  (attack state) */
	u8 eg_sel_d1r; /*  (decay state) */
	u8 eg_sel_d2r; /*  (sustain state) */
	u8 eg_sel_rr; /*  (release state) */
	u8 ssg; /* SSG-EG waveform */
	u8 ssgn; /* SSG-EG negated output */
	u32 key; /* 0=last key was KEY OFF, 1=KEY ON */
	/* LFO */u32 AMmask; /* AM enable flag */

} FM_SLOT;

/* MVS64: aliases for the eg_shv[] state-indexed array (see FM_SLOT). */
#define eg_sh_ar  eg_shv[EG_ATT]
#define eg_sh_d1r eg_shv[EG_DEC]
#define eg_sh_d2r eg_shv[EG_SUS]
#define eg_sh_rr  eg_shv[EG_REL]

typedef struct {
	FM_SLOT SLOT[4]; /* four SLOTs (operators) */
	u8 ALGO; /* algorithm */
	u8 FB; /* feedback shift */
	s32 op1_out[2]; /* op1 output for feedback */
	s32 *connect1; /* SLOT1 output pointer */
	s32 *connect3; /* SLOT3 output pointer */
	s32 *connect2; /* SLOT2 output pointer */
	s32 *connect4; /* SLOT4 output pointer */
	s32 *mem_connect;/* where to put the delayed sample (MEM) */
	s32 mem_value; /* delayed sample (MEM) value */
	s32 pms; /* channel PMS */
	u8 ams; /* channel AMS */
	u32 fc; /* fnum,blk:adjusted to sample rate */
	u8 kcode; /* key code:                        */
	u32 block_fnum; /* current blk/fnum value for this slot (can be different betweeen slots of one channel in 3slot mode) */
	/* MVS64: cache for the LFO-PM phase-delta stack in chan_calc. The four
	 * per-slot deltas only depend on (LFO_PM step, block_fnum) plus the
	 * slots' DT/mul (whose every change funnels through Incr=-1 ->
	 * refresh_fc_eg_chan, which invalidates this). 0xFFFFFFFF = invalid. */
	u32 pm_key;
	u32 pm_dp[4];
} FM_CH;

typedef struct {
	int clock; /* master clock  (Hz)   */
	int rate; /* sampling rate (Hz)   */
	double freqbase; /* frequency base       */
	double TimerBase; /* Timer base time      */
	/* MVS64: integer runtime equivalents (computed at init in OPNSetPres) so no
	 * FP executes on register writes (N64: those run in exception context). */
	u32 TimerBase_cyc8; /* Z80 cycles per timer count, 24.8 fixed point */
#if FM_BUSY_FLAG_SUPPORT
	u32 BusyExpire; /* MVS64: busy-clear deadline in Z80 cycles (0 = not busy) */
#endif
	u8 address; /* address register     */
	u8 irq; /* interrupt level      */
	u8 irqmask; /* irq mask             */
	u8 status; /* status flag          */
	u32 mode; /* mode  CSM / 3SLOT    */
	u8 prescaler_sel;/* prescaler selector */
	u8 fn_h; /* freq latch           */
	s32 TA; /* timer a              */
	s32 TAC; /* timer a counter      */
	u8 TB; /* timer b              */
	s32 TBC; /* timer b counter      */
	/* local time tables */s32 dt_tab[8][32];/* DeTune table       */
	/* Extention Timer and IRQ handler */
	FM_TIMERHANDLER Timer_Handler;
	FM_IRQHANDLER IRQ_Handler;
} FM_ST;

/* OPN 3slot struct */
typedef struct {
	u32 fc[3]; /* fnum3,blk3: calculated */
	u8 fn_h; /* freq3 latch */
	u8 kcode[3]; /* key code */
	u32 block_fnum[3]; /* current fnum value for this slot (can be different betweeen slots of one channel in 3slot mode) */
} FM_3SLOT;

/* OPN/A/B common state */
typedef struct {
	FM_ST ST; /* general state */
	FM_3SLOT SL3; /* 3 slot mode state */
	FM_CH *P_CH; /* pointer of CH */
	unsigned int pan[6 * 2]; /* fm channels output masks (0xffffffff = enable) */
	u32 eg_cnt; /* global envelope generator counter */
	u32 eg_timer; /* global envelope generator counter works at frequency = chipclock/64/3 */
	u32 eg_timer_add; /* step of eg_timer */
	u32 eg_timer_overflow;/* envelope generator timer overlfows every 3 samples (on real chip) */
	/* there are 2048 FNUMs that can be generated using FNUM/BLK registers
	 but LFO works with one more bit of a precision so we really need 4096 elements */u32 fn_table[4096]; /* fnumber->increment counter */
	/* LFO */u32 lfo_cnt;
	u32 lfo_inc;
	u32 lfo_freq[8]; /* LFO FREQ table */
} FM_OPN;

/* SSG struct */
static struct SSG_t {
	int lastEnable;
	u32 step;
	int period[3];
	int PeriodN;
	int PeriodE;
	int count[3];
	int CountN;
	int CountE;
	u32 vol[3];
	u32 VolE;
	u8 envelope[3];
	u8 output[3];
	u8 OutputN;
	s8 count_env;
	u8 hold;
	u8 alternate;
	u8 attack;
	u8 holding;
	int RNG;
	u32 vol_table[32];
} SSG;

/* ADPCM type A channel struct */
typedef struct {
	u8 flag; /* port state				*/
	u8 flagMask; /* arrived flag mask		*/
	u8 now_data; /* current ROM data			*/
	u32 now_addr; /* current ROM address		*/
	u32 now_step;
	u32 step;
	u32 start; /* sample data start address*/
	u32 end; /* sample data end address	*/
	u8 IL; /* Instrument Level			*/
	s32 adpcma_acc; /* accumulator				*/
	s32 adpcma_step; /* step						*/
	s32 adpcma_out; /* (speedup) hiro-shi!!		*/
	s8 vol_mul; /* volume in "0.75dB" steps	*/
	u8 vol_shift; /* volume in "-6dB" steps	*/
	s32 *pan; /* &out_adpcma[OPN_xxxx] 	*/
} ADPCMA;

/* ADPCM type B struct */
typedef struct adpcmb_state {
	s32 *pan; /* pan : &output_pointer[pan]   */
	u32 freqbase16; /* MVS64: 16.16 fixed (was double; delta writes are runtime) */
	int output_range;
	u32 now_addr; /* current address      */
	u32 now_step; /* currect step         */
	u32 step; /* step                 */
	u32 start; /* start address        */
	u32 limit; /* limit address        */
	u32 end; /* end address          */
	u32 delta; /* delta scale          */
	s32 volume; /* current volume       */
	s32 acc; /* shift Measurement value*/
	s32 adpcmd; /* next Forecast        */
	s32 adpcml; /* current value        */
	s32 prev_acc; /* leveling value       */
	u8 now_data; /* current rom data     */
	u8 CPU_data; /* current data from reg 08 */
	u8 portstate; /* port status          */
	u8 control2; /* control reg: SAMPLE, DA/AD, RAM TYPE (x8bit / x1bit), ROM/RAM */
	u8 portshift; /* address bits shift-left:
	 ** 8 for YM2610,
	 ** 5 for Y8950 and YM2608 */
	u8 DRAMportshift; /* address bits shift-right:
	 ** 0 for ROM and x8bit DRAMs,
	 ** 3 for x1 DRAMs */
	u8 memread; /* needed for reading/writing external memory */
	/* note that different chips have these flags on different
	 ** bits of the status register
	 */u8 status_change_EOS_bit; /* 1 on End Of Sample (record/playback/cycle time of AD/DA converting has passed)*/
	u8 status_change_BRDY_bit; /* 1 after recording 2 datas (2x4bits) or after reading/writing 1 data */
	/* neither Y8950 nor YM2608 can generate IRQ when PCMBSY bit changes, so instead of above,
	 ** the statusflag gets ORed with PCM_BSY (below) (on each read of statusflag of Y8950 and YM2608)
	 */u8 PCM_BSY; /* 1 when ADPCM is playing; Y8950/YM2608 only */

} ADPCMB;

/* here's the virtual YM2610 */
static struct ym2610_t {
	u8 regs[512]; /* registers            */
	FM_OPN OPN; /* OPN state            */
	FM_CH CH[6]; /* channel state        */
	u8 addr_A1; /* address line A1      */
	/* ADPCM-A unit */u8 adpcmaTL; /* adpcmA total level   */
	ADPCMA adpcma[6]; /* adpcm channels       */
	u8 adpcm_arrivedEndAddress;

	/* ADPCM-B unit */
	ADPCMB adpcmb; /* Delta-T ADPCM unit   */

}ALIGN_DATA YM2610;

/* current chip state */
static s32 m2, c1, c2; /* Phase Modulation input for operators 2,3,4 */
static s32 mem; /* one sample delay memory */

static s32 ALIGN_DATA out_fm[8]; /* outputs of working channels */
static s32 out_ssg; /* channel output CHENTER only for SSG */
static s32 ALIGN_DATA out_adpcma[4]; /* channel output NONE,LEFT,RIGHT or CENTER for YM2608/YM2610 ADPCM */
static s32 ALIGN_DATA out_delta[4]; /* channel output NONE,LEFT,RIGHT or CENTER for YM2608/YM2610 DELTAT*/

static u32 LFO_AM; /* runtime LFO calculations helper */
static s32 LFO_PM; /* runtime LFO calculations helper */

/* log output level */
#define LOG_ERR  3      /* ERROR       */
#define LOG_WAR  2      /* WARNING     */
#define LOG_INF  1      /* INFORMATION */
#define LOG_LEVEL LOG_INF

#define LOG(n,x) if( (n)>=LOG_LEVEL ) logerror x

/*********************************************************************************************/

/* status set and IRQ handling */INLINE void FM_STATUS_SET(FM_ST *ST, int flag) {
	/* set status flag */
	ST->status |= flag;
	if (!(ST->irq) && (ST->status & ST->irqmask)) {
		ST->irq = 1;
		/* callback user interrupt handler (IRQ is OFF to ON) */
		(ST->IRQ_Handler)(1);
	}
}

/* status reset and IRQ handling */INLINE void FM_STATUS_RESET(FM_ST *ST,
		int flag) {
	/* reset status flag */
	ST->status &= ~flag;
	if ((ST->irq) && !(ST->status & ST->irqmask)) {
		ST->irq = 0;
		/* callback user interrupt handler (IRQ is ON to OFF) */
		(ST->IRQ_Handler)(0);
	}
}

/* IRQ mask set */INLINE void FM_IRQMASK_SET(FM_ST *ST, int flag) {
	ST->irqmask = flag;
	/* IRQ handling check */
	FM_STATUS_SET(ST, 0);
	FM_STATUS_RESET(ST, 0);
}

/* OPN Mode Register Write */INLINE void set_timers(FM_ST *ST, int v) {
	/* b7 = CSM MODE */
	/* b6 = 3 slot mode */
	/* b5 = reset b */
	/* b4 = reset a */
	/* b3 = timer enable b */
	/* b2 = timer enable a */
	/* b1 = load b */
	/* b0 = load a */
	ST->mode = v;

	/* reset Timer b flag */
	if (v & 0x20)
		FM_STATUS_RESET(ST, 0x02);
	/* reset Timer a flag */
	if (v & 0x10)
		FM_STATUS_RESET(ST, 0x01);
	/* load b */
	if (v & 0x02) {
		if (ST->TBC == 0) {
			ST->TBC = (256 - ST->TB) << 4;
			/* External timer handler */
#if FM_INTERNAL_TIMER==0
			(ST->Timer_Handler)(1, ((u32)ST->TBC * ST->TimerBase_cyc8) >> 8);
#endif
		}
	} else { /* stop timer b */
		if (ST->TBC != 0) {
			ST->TBC = 0;
#if FM_INTERNAL_TIMER==0
			(ST->Timer_Handler)(1, 0);
#endif
		}
	}
	/* load a */
	if (v & 0x01) {
		if (ST->TAC == 0) {
			ST->TAC = (1024 - ST->TA);
			/* External timer handler */
#if FM_INTERNAL_TIMER==0
			(ST->Timer_Handler)(0, ((u32)ST->TAC * ST->TimerBase_cyc8) >> 8);
#endif
		}
	} else { /* stop timer a */
		if (ST->TAC != 0) {
			ST->TAC = 0;
#if FM_INTERNAL_TIMER==0
			(ST->Timer_Handler)(0, 0);
#endif
		}
	}
}

/* Timer A Overflow */INLINE void TimerAOver(FM_ST *ST) {
	/* set status (if enabled) */
	if (ST->mode & 0x04)
		FM_STATUS_SET(ST, 0x01);
	/* clear or reload the counter */
	ST->TAC = (1024 - ST->TA);
#if FM_INTERNAL_TIMER==0
	(ST->Timer_Handler)(0, ((u32)ST->TAC * ST->TimerBase_cyc8) >> 8);
#endif
}
/* Timer B Overflow */INLINE void TimerBOver(FM_ST *ST) {
	/* set status (if enabled) */
	if (ST->mode & 0x08)
		FM_STATUS_SET(ST, 0x02);
	/* clear or reload the counter */
	ST->TBC = (256 - ST->TB) << 4;
#if FM_INTERNAL_TIMER==0
	(ST->Timer_Handler)(1, ((u32)ST->TBC * ST->TimerBase_cyc8) >> 8);
#endif
}

#if FM_INTERNAL_TIMER
/* ----- internal timer mode , update timer */

/* ---------- calculate timer A ---------- */
#define INTERNAL_TIMER_A(ST,CSM_CH)					\
	{													\
		if( ST.TAC /*&&  (ST.Timer_Handler==0) */)		\
			/*if( (ST.TAC -= (int)(ST.freqbase*4096)) <= 0 )*/	\
			if( (ST.TAC -= (int)((1000.0/ST.rate)*4096)) <= 0 )	\
			{											\
				TimerAOver( &ST );						\
				/* CSM mode total level latch and auto key on */	\
				if( ST.mode & 0x80 )					\
					CSMKeyControll( CSM_CH );			\
			}											\
	}
/* ---------- calculate timer B ---------- */
#define INTERNAL_TIMER_B(ST,step)						\
	{														\
		if( ST.TBC /*&& (ST.Timer_Handler==0) */)				\
			/*if( (ST.TBC -= (int)(ST.freqbase*4096*step)) <= 0 )*/	\
			if( (ST.TBC -= (int)((1000.0/ST.rate)*4096*step)) <= 0 )	\
				TimerBOver( &ST );							\
	}
#else /* FM_INTERNAL_TIMER */
/* external timer mode */
#define INTERNAL_TIMER_A(ST,CSM_CH)
#define INTERNAL_TIMER_B(ST,step)
#endif /* FM_INTERNAL_TIMER */

#if FM_BUSY_FLAG_SUPPORT
/* MVS64: busy tracked in integer Z80 cycles (wrap-safe compare), not double
 * seconds — this runs on every YM status read/register write, including from
 * the N64 exception handler where FP is forbidden. */
INLINE u8 FM_STATUS_FLAG(FM_ST *ST) {
	if (ST->BusyExpire) {
		if ((s32)(ST->BusyExpire - FM_GET_TIME_NOW_CYC()) > 0)
			return ST->status | 0x80; /* with busy */
		/* expire */
		ST->BusyExpire = 0;
	}
	return ST->status;
}
INLINE void FM_BUSY_SET(FM_ST *ST, int busyclock) {
	u32 t = FM_GET_TIME_NOW_CYC() + (((u32)busyclock * ST->TimerBase_cyc8) >> 8);
	ST->BusyExpire = t ? t : 1; /* 0 means "not busy" */
}
#define FM_BUSY_CLEAR(ST) ((ST)->BusyExpire = 0)
#else
#define FM_STATUS_FLAG(ST) ((ST)->status)
#define FM_BUSY_SET(ST,bclock) {}
#define FM_BUSY_CLEAR(ST) {}
#endif

INLINE void FM_KEYON(FM_CH *CH, int s) {
	FM_SLOT *SLOT = &CH->SLOT[s];
	if (!SLOT->key) {
		SLOT->key = 1;
		SLOT->phase = 0; /* restart Phase Generator */
		SLOT->state = EG_ATT; /* phase -> Attack */
	}
}

INLINE void FM_KEYOFF(FM_CH *CH, int s) {
	FM_SLOT *SLOT = &CH->SLOT[s];
	if (SLOT->key) {
		SLOT->key = 0;
		if (SLOT->state > EG_REL)
			SLOT->state = EG_REL;/* phase -> Release */
	}
}

/* set algorithm connection */
static void setup_connection(FM_CH *CH, int ch) {
	s32 *carrier = &out_fm[ch];

	s32 **om1 = &CH->connect1;
	s32 **om2 = &CH->connect3;
	s32 **oc1 = &CH->connect2;

	s32 **memc = &CH->mem_connect;

	switch (CH->ALGO) {
	case 0:
		/* M1---C1---MEM---M2---C2---OUT */
		*om1 = &c1;
		*oc1 = &mem;
		*om2 = &c2;
		*memc = &m2;
		break;
	case 1:
		/* M1------+-MEM---M2---C2---OUT */
		/*      C1-+                     */
		*om1 = &mem;
		*oc1 = &mem;
		*om2 = &c2;
		*memc = &m2;
		break;
	case 2:
		/* M1-----------------+-C2---OUT */
		/*      C1---MEM---M2-+          */
		*om1 = &c2;
		*oc1 = &mem;
		*om2 = &c2;
		*memc = &m2;
		break;
	case 3:
		/* M1---C1---MEM------+-C2---OUT */
		/*                 M2-+          */
		*om1 = &c1;
		*oc1 = &mem;
		*om2 = &c2;
		*memc = &c2;
		break;
	case 4:
		/* M1---C1-+-OUT */
		/* M2---C2-+     */
		/* MEM: not used */
		*om1 = &c1;
		*oc1 = carrier;
		*om2 = &c2;
		*memc = &mem; /* store it anywhere where it will not be used */
		break;
	case 5:
		/*    +----C1----+     */
		/* M1-+-MEM---M2-+-OUT */
		/*    +----C2----+     */
		*om1 = 0; /* special mark */
		*oc1 = carrier;
		*om2 = carrier;
		*memc = &m2;
		break;
	case 6:
		/* M1---C1-+     */
		/*      M2-+-OUT */
		/*      C2-+     */
		/* MEM: not used */
		*om1 = &c1;
		*oc1 = carrier;
		*om2 = carrier;
		*memc = &mem; /* store it anywhere where it will not be used */
		break;
	case 7:
		/* M1-+     */
		/* C1-+-OUT */
		/* M2-+     */
		/* C2-+     */
		/* MEM: not used*/
		*om1 = carrier;
		*oc1 = carrier;
		*om2 = carrier;
		*memc = &mem; /* store it anywhere where it will not be used */
		break;
	}

	CH->connect4 = carrier;
}

/* set detune & multiple */INLINE void set_det_mul(FM_ST *ST, FM_CH *CH,
		FM_SLOT *SLOT, int v) {
	SLOT->mul = (v & 0x0f) ? (v & 0x0f) * 2 : 1;
	SLOT->DT = ST->dt_tab[(v >> 4) & 7];
	CH->SLOT[SLOT1].Incr = -1;
}

/* set total level */INLINE void set_tl(FM_CH *CH, FM_SLOT *SLOT, int v) {
	SLOT->tl = (v & 0x7f) << (ENV_BITS - 7); /* 7bit TL */
	/* MVS64: keep vol_out coherent immediately. advance_eg_channel skips slots
	 * parked in EG_OFF (speedup), so the per-EG-step recompute cannot be relied
	 * on to pick up a TL change for those slots. (This also applies TL at most
	 * one EG step earlier than before for active slots — the real chip applies
	 * TL immediately too.) */
	{
		unsigned int out = SLOT->tl + (u32) SLOT->volume;
		if ((SLOT->ssg & 0x08) && (SLOT->ssgn & 2))
			out ^= ((1 << ENV_BITS) - 1);
		SLOT->vol_out = out;
	}
}

/* set attack rate & key scale  */INLINE void set_ar_ksr(FM_CH *CH,
		FM_SLOT *SLOT, int v) {
	u8 old_KSR = SLOT->KSR;

	SLOT->ar = (v & 0x1f) ? 32 + ((v & 0x1f) << 1) : 0;

	SLOT->KSR = 3 - (v >> 6);
	if (SLOT->KSR != old_KSR) {
		CH->SLOT[SLOT1].Incr = -1;
	} else {
		/* refresh Attack rate */
		if ((SLOT->ar + SLOT->ksr) < 32 + 62) {
			SLOT->eg_sh_ar = eg_rate_shift[SLOT->ar + SLOT->ksr];
			SLOT->eg_sel_ar = eg_rate_select[SLOT->ar + SLOT->ksr];
		} else {
			SLOT->eg_sh_ar = 0;
			SLOT->eg_sel_ar = 17 * RATE_STEPS;
		}
	}
}

/* set decay rate */INLINE void set_dr(FM_SLOT *SLOT, int v) {
	SLOT->d1r = (v & 0x1f) ? 32 + ((v & 0x1f) << 1) : 0;

	SLOT->eg_sh_d1r = eg_rate_shift[SLOT->d1r + SLOT->ksr];
	SLOT->eg_sel_d1r = eg_rate_select[SLOT->d1r + SLOT->ksr];

}

/* set sustain rate */INLINE void set_sr(FM_SLOT *SLOT, int v) {
	SLOT->d2r = (v & 0x1f) ? 32 + ((v & 0x1f) << 1) : 0;

	SLOT->eg_sh_d2r = eg_rate_shift[SLOT->d2r + SLOT->ksr];
	SLOT->eg_sel_d2r = eg_rate_select[SLOT->d2r + SLOT->ksr];
}

/* set release rate */INLINE void set_sl_rr(FM_SLOT *SLOT, int v) {
	SLOT->sl = sl_table[v >> 4];

	SLOT->rr = 34 + ((v & 0x0f) << 2);

	SLOT->eg_sh_rr = eg_rate_shift[SLOT->rr + SLOT->ksr];
	SLOT->eg_sel_rr = eg_rate_select[SLOT->rr + SLOT->ksr];
}

INLINE signed int op_calc(u32 phase, unsigned int env, signed int pm) {
	u32 p;

	p = (env << 3)
			+ sin_tab[(((signed int) ((phase & ~FREQ_MASK) + (pm << 15)))
					>> FREQ_SH) & SIN_MASK];

	if (p >= TL_TAB_LEN)
		return 0;
	return tl_tab_lookup(p);
}

INLINE signed int op_calc1(u32 phase, unsigned int env, signed int pm) {
	u32 p;

	p = (env << 3)
			+ sin_tab[(((signed int) ((phase & ~FREQ_MASK) + pm)) >> FREQ_SH)
					& SIN_MASK];

	if (p >= TL_TAB_LEN)
		return 0;
	return tl_tab_lookup(p);
}

/* advance LFO to next sample */INLINE void advance_lfo(FM_OPN *OPN) {
	u8 pos;
	u8 prev_pos;

	if (OPN->lfo_inc) /* LFO enabled ? */
	{
		prev_pos = OPN->lfo_cnt >> LFO_SH & 127;

		OPN->lfo_cnt += OPN->lfo_inc;

		pos = (OPN->lfo_cnt >> LFO_SH) & 127;

		/* update AM when LFO output changes */

		/*if (prev_pos != pos)*/
		/* actually I can't optimize is this way without rewritting chan_calc()
		 to use chip->lfo_am instead of global lfo_am */
		{

			/* triangle */
			/* AM: 0 to 126 step +2, 126 to 0 step -2 */
			if (pos < 64)
				LFO_AM = (pos & 63) * 2;
			else
				LFO_AM = 126 - ((pos & 63) * 2);
		}

		/* PM works with 4 times slower clock */
		prev_pos >>= 2;
		pos >>= 2;
		/* update PM when LFO output changes */
		/*if (prev_pos != pos)*//* can't use global lfo_pm for this optimization, must be chip->lfo_pm instead*/
		{
			LFO_PM = pos;
		}

	} else {
		LFO_AM = 0;
		LFO_PM = 0;
	}
}

/* MVS64: takes the envelope counter by VALUE (not via OPN) so the channel-major
 * update loop can replay the same eg_cnt sequence per channel with everything
 * register-resident (an OPN->eg_cnt load per tick would also be reloaded after
 * every SLOT store because of type-based aliasing). */
INLINE void advance_eg_channel(u32 eg_cnt, FM_SLOT *SLOT) {
	unsigned int out;
	unsigned int swap_flag = 0;
	unsigned int i;

	i = 4; /* four operators per channel */
	do {
		/* MVS64 speedup: a slot parked in EG_OFF has constant volume, so its
		 * vol_out cannot change here (TL writes recompute it in set_tl; a
		 * pending SSG-EG swap_flag forces the full path so the ssgn xor quirk
		 * below behaves exactly as before). On samsho2's sparse FM usage this
		 * skips most of the EG work. */
		if (SLOT->state == EG_OFF && !swap_flag) {
			SLOT++;
			i--;
			continue;
		}

		/* MVS64 exact-skip: a slot's envelope only advances when eg_cnt
		 * lands on its current state's rate mask. Off-mask, neither volume
		 * nor state nor ssgn can change, and vol_out was already computed
		 * from these exact inputs the last time the slot WAS due (or at
		 * key-on/TL write), so skipping the whole body is bit-exact.
		 * A pending swap_flag forces the full path: the SSG swap must
		 * still be applied to the channel's remaining slots. */
		if (!swap_flag
		    && (eg_cnt & ((1u << SLOT->eg_shv[SLOT->state]) - 1))) {
			SLOT++;
			i--;
			continue;
		}

		switch (SLOT->state) {
		case EG_ATT: /* attack phase */
			if (!(eg_cnt & ((1 << SLOT->eg_sh_ar) - 1))) {
				SLOT->volume += (~SLOT->volume
						* (eg_inc[SLOT->eg_sel_ar
								+ ((eg_cnt >> SLOT->eg_sh_ar) & 7)])) >> 4;
				if (SLOT->volume <= MIN_ATT_INDEX) {
					SLOT->volume = MIN_ATT_INDEX;
					SLOT->state = EG_DEC;
				}
			}

			break;

		case EG_DEC: /* decay phase */
			if (SLOT->ssg & 0x08) /* SSG EG type envelope selected */
			{
				if (!(eg_cnt & ((1 << SLOT->eg_sh_d1r) - 1))) {
					SLOT->volume += (eg_inc[SLOT->eg_sel_d1r
							+ ((eg_cnt >> SLOT->eg_sh_d1r) & 7)] << 2);

					if (SLOT->volume >= SLOT->sl)
						SLOT->state = EG_SUS;
				}
			} else {
				if (!(eg_cnt & ((1 << SLOT->eg_sh_d1r) - 1))) {
					SLOT->volume += eg_inc[SLOT->eg_sel_d1r
							+ ((eg_cnt >> SLOT->eg_sh_d1r) & 7)];

					if (SLOT->volume >= SLOT->sl)
						SLOT->state = EG_SUS;
				}
			}
			break;

		case EG_SUS: /* sustain phase */
			if (SLOT->ssg & 0x08) /* SSG EG type envelope selected */
			{
				if (!(eg_cnt & ((1 << SLOT->eg_sh_d2r) - 1))) {
					SLOT->volume += (eg_inc[SLOT->eg_sel_d2r
							+ ((eg_cnt >> SLOT->eg_sh_d2r) & 7)] << 2);

					if (SLOT->volume >= MAX_ATT_INDEX) {
						SLOT->volume = MAX_ATT_INDEX;

						if (SLOT->ssg & 0x01) /* bit 0 = hold */
						{
							if (SLOT->ssgn & 1) /* have we swapped once ??? */
							{
								/* yes, so do nothing, just hold current level */
							} else
								swap_flag = (SLOT->ssg & 0x02) | 1; /* bit 1 = alternate */

						} else {
							/* same as KEY-ON operation */

							/* restart of the Phase Generator should be here,
							 only if AR is not maximum ??? */
							/*SLOT->phase = 0;*/

							/* phase -> Attack */
							SLOT->state = EG_ATT;

							swap_flag = (SLOT->ssg & 0x02); /* bit 1 = alternate */
						}
					}
				}
			} else {
				if (!(eg_cnt & ((1 << SLOT->eg_sh_d2r) - 1))) {
					SLOT->volume += eg_inc[SLOT->eg_sel_d2r
							+ ((eg_cnt >> SLOT->eg_sh_d2r) & 7)];

					if (SLOT->volume >= MAX_ATT_INDEX) {
						SLOT->volume = MAX_ATT_INDEX;
						/* do not change SLOT->state (verified on real chip) */
					}
				}

			}
			break;

		case EG_REL: /* release phase */
			if (!(eg_cnt & ((1 << SLOT->eg_sh_rr) - 1))) {
				SLOT->volume += eg_inc[SLOT->eg_sel_rr
						+ ((eg_cnt >> SLOT->eg_sh_rr) & 7)];

				if (SLOT->volume >= MAX_ATT_INDEX) {
					SLOT->volume = MAX_ATT_INDEX;
					SLOT->state = EG_OFF;
				}
			}
			break;

		}

		out = SLOT->tl + ((u32) SLOT->volume);

		if ((SLOT->ssg & 0x08) && (SLOT->ssgn & 2)) /* negate output (changes come from alternate bit, init comes from attack bit) */
			out ^= ((1 << ENV_BITS) - 1); /* 1023 */

		/* we need to store the result here because we are going to change ssgn
		 in next instruction */
		SLOT->vol_out = out;

		SLOT->ssgn ^= swap_flag;

		SLOT++;
		i--;
	} while (i);

}

#define volume_calc(OP) ((OP)->vol_out + (AM & (OP)->AMmask))

/* Advance the four phase counters of an LFO-PM'd channel by one sample.
 * MVS64: extracted from chan_calc's tail so the silent-channel fast path can
 * keep phases (and the pm_dp cache) exact without computing any output.
 *
 * The four PM'd phase deltas depend only on (LFO_PM, block_fnum) and slot
 * DT/mul (invalidated via refresh_fc_eg_chan). LFO_PM only steps every few
 * dozen samples and notes change far slower, so cache the delta stack instead
 * of redoing the table+multiply work every sample. Values are bit-identical. */
INLINE void update_phase_lfo(FM_OPN *OPN, FM_CH *CH, u32 lfo_pm) {
	/* add support for 3 slot mode */

	u32 block_fnum = CH->block_fnum;

	u32 pm_key = (lfo_pm << 17) | block_fnum;
	if (pm_key != CH->pm_key) {
		u32 fnum_lfo = ((block_fnum & 0x7f0) >> 4) * 32 * 8;
		s32 lfo_fn_table_index_offset =
				lfo_pm_table[fnum_lfo + CH->pms + lfo_pm];

		CH->pm_key = pm_key;
		if (lfo_fn_table_index_offset) /* LFO phase modulation active */
		{
			u8 blk;
			u32 fn;
			int kc, fc;

			block_fnum = block_fnum * 2 + lfo_fn_table_index_offset;

			blk = (block_fnum & 0x7000) >> 12;
			fn = block_fnum & 0xfff;

			/* keyscale code */
			kc = (blk << 2) | opn_fktable[fn >> 8];
			/* phase increment counter */
			fc = OPN->fn_table[fn] >> (7 - blk);

			CH->pm_dp[0] = ((fc + CH->SLOT[SLOT1].DT[kc])
					* CH->SLOT[SLOT1].mul) >> 1;
			CH->pm_dp[1] = ((fc + CH->SLOT[SLOT2].DT[kc])
					* CH->SLOT[SLOT2].mul) >> 1;
			CH->pm_dp[2] = ((fc + CH->SLOT[SLOT3].DT[kc])
					* CH->SLOT[SLOT3].mul) >> 1;
			CH->pm_dp[3] = ((fc + CH->SLOT[SLOT4].DT[kc])
					* CH->SLOT[SLOT4].mul) >> 1;
		} else /* LFO phase modulation  = zero */
		{
			CH->pm_dp[0] = CH->SLOT[SLOT1].Incr;
			CH->pm_dp[1] = CH->SLOT[SLOT2].Incr;
			CH->pm_dp[2] = CH->SLOT[SLOT3].Incr;
			CH->pm_dp[3] = CH->SLOT[SLOT4].Incr;
		}
	}
	CH->SLOT[SLOT1].phase += CH->pm_dp[0];
	CH->SLOT[SLOT2].phase += CH->pm_dp[1];
	CH->SLOT[SLOT3].phase += CH->pm_dp[2];
	CH->SLOT[SLOT4].phase += CH->pm_dp[3];
}

/* MVS64: the LFO position comes in by VALUE (precomputed per sample by the
 * channel-major update loop) instead of via the LFO_AM/LFO_PM globals — the
 * globals would be reloaded after every *CH->connect store (aliasing). */
INLINE void chan_calc(FM_OPN *OPN, FM_CH *CH, u32 lfo_am, u32 lfo_pm) {
	unsigned int eg_out;

	u32 AM = lfo_am >> CH->ams;

	m2 = c1 = c2 = mem = 0;

	*CH->mem_connect = CH->mem_value; /* restore delayed sample (MEM) value to m2 or c2 */

	eg_out = volume_calc(&CH->SLOT[SLOT1]);
	{
		s32 out = CH->op1_out[0] + CH->op1_out[1];
		CH->op1_out[0] = CH->op1_out[1];

		if (!CH->connect1) {
			/* algorithm 5  */
			mem = c1 = c2 = CH->op1_out[0];
		} else {
			/* other algorithms */
			*CH->connect1 += CH->op1_out[0];
		}

		CH->op1_out[1] = 0;
		if (eg_out < ENV_QUIET) /* SLOT 1 */
		{
			if (!CH->FB)
				out = 0;

			CH->op1_out[1] = op_calc1(CH->SLOT[SLOT1].phase, eg_out,
					(out << CH->FB));
		}
	}

	eg_out = volume_calc(&CH->SLOT[SLOT3]);
	if (eg_out < ENV_QUIET) /* SLOT 3 */
		*CH->connect3 += op_calc(CH->SLOT[SLOT3].phase, eg_out, m2);

	eg_out = volume_calc(&CH->SLOT[SLOT2]);
	if (eg_out < ENV_QUIET) /* SLOT 2 */
		*CH->connect2 += op_calc(CH->SLOT[SLOT2].phase, eg_out, c1);

	eg_out = volume_calc(&CH->SLOT[SLOT4]);
	if (eg_out < ENV_QUIET) /* SLOT 4 */
		*CH->connect4 += op_calc(CH->SLOT[SLOT4].phase, eg_out, c2);

	/* store current MEM */
	CH->mem_value = mem;

	/* update phase counters AFTER output calculations */
	if (CH->pms) {
		update_phase_lfo(OPN, CH, lfo_pm);
	} else /* no LFO phase modulation */
	{
		CH->SLOT[SLOT1].phase += CH->SLOT[SLOT1].Incr;
		CH->SLOT[SLOT2].phase += CH->SLOT[SLOT2].Incr;
		CH->SLOT[SLOT3].phase += CH->SLOT[SLOT3].Incr;
		CH->SLOT[SLOT4].phase += CH->SLOT[SLOT4].Incr;
	}
}

/* MVS64: algorithm-specialized chan_calc for the stream (chunked) path.
 * Identical dataflow to chan_calc(), but the operator connection targets —
 * which setup_connection() encodes as s32* pointers into the m2/c1/c2/mem
 * globals and out_fm[] — are resolved at compile time into LOCALS (algo is
 * a compile-time constant at every call site below). The pointer stores in
 * chan_calc() defeat type-based alias analysis: after every "*connect +="
 * the compiler must assume any s32 in the program may have changed and
 * reloads channel state per sample — measured as the bulk of FM cost
 * in-game (fmms ~722ms per 2.4s interval, ~2/3 of the whole YM2610).
 * Bit-exact: same operations on the same values in the same order; only
 * the storage location of the per-sample intermediate sums changes (they
 * were per-sample-zeroed globals). Returns the carrier sum, i.e. exactly
 * what chan_calc() left in out_fm[ch] for this sample.
 * Connection map per algorithm (from setup_connection):
 *   om1 (M1 out):  0,3,4,6->c1  1->mem  2->c2  5->special  7->carrier
 *   om2 (M2 out):  0..4->c2    5,6,7->carrier
 *   oc1 (C1 out):  0..3->mem   4..7->carrier
 *   memc(restore): 0,1,2,5->m2  3->c2   4,6,7->mem (dummy)
 *   SLOT4 always -> carrier. */
/* Per-sample intermediate sums live in a small LOCAL array indexed by the
 * precomputed routing below: locals cannot alias CH/global state, so the
 * compiler keeps channel state in registers across the sample. A SINGLE
 * shared body (algo is a runtime value hoisted per chunk) keeps the icache
 * footprint identical to chan_calc() — an earlier always_inline x8
 * specialization was WAV-exact but 40% SLOWER on N64 (icache blowout, the
 * -O3 lesson again). Index map: 0=m2 1=c1 2=c2 3=mem 4=carrier. */
#define CCS_M2   0
#define CCS_C1   1
#define CCS_C2   2
#define CCS_MEM  3
#define CCS_CAR  4
/* setup_connection() routing tables, per algorithm 0..7 */
static const u8 ccs_memc[8] = { CCS_M2, CCS_M2, CCS_M2, CCS_C2,
                                CCS_MEM, CCS_M2, CCS_MEM, CCS_MEM };
static const u8 ccs_om1[8]  = { CCS_C1, CCS_MEM, CCS_C2, CCS_C1,
                                CCS_C1, CCS_C1 /*unused: algo5 special*/,
                                CCS_C1, CCS_CAR };
static const u8 ccs_om2[8]  = { CCS_C2, CCS_C2, CCS_C2, CCS_C2,
                                CCS_C2, CCS_CAR, CCS_CAR, CCS_CAR };
static const u8 ccs_oc1[8]  = { CCS_MEM, CCS_MEM, CCS_MEM, CCS_MEM,
                                CCS_CAR, CCS_CAR, CCS_CAR, CCS_CAR };

INLINE s32 chan_calc_stream(FM_OPN *OPN, FM_CH *CH, u32 lfo_am, u32 lfo_pm,
		int algo, int i_memc, int i_om1, int i_om2, int i_oc1) {
	unsigned int eg_out;
	u32 AM = lfo_am >> CH->ams;
	s32 s[5];

	s[CCS_M2] = s[CCS_C1] = s[CCS_C2] = s[CCS_MEM] = s[CCS_CAR] = 0;

	/* *CH->mem_connect = CH->mem_value (restore delayed MEM sample) */
	s[i_memc] = CH->mem_value;

	/* SLOT 1 (M1, feedback) */
	{
		s32 out = CH->op1_out[0] + CH->op1_out[1];
		CH->op1_out[0] = CH->op1_out[1];

		if (algo == 5)   /* connect1 == 0 special mark */
			s[CCS_MEM] = s[CCS_C1] = s[CCS_C2] = CH->op1_out[0];
		else
			s[i_om1] += CH->op1_out[0];

		CH->op1_out[1] = 0;
		eg_out = volume_calc(&CH->SLOT[SLOT1]);
		if (eg_out < ENV_QUIET) {
			if (!CH->FB)
				out = 0;
			CH->op1_out[1] = op_calc1(CH->SLOT[SLOT1].phase, eg_out,
					(out << CH->FB));
		}
	}

	/* SLOT 3 (M2) */
	eg_out = volume_calc(&CH->SLOT[SLOT3]);
	if (eg_out < ENV_QUIET)
		s[i_om2] += op_calc(CH->SLOT[SLOT3].phase, eg_out, s[CCS_M2]);

	/* SLOT 2 (C1) */
	eg_out = volume_calc(&CH->SLOT[SLOT2]);
	if (eg_out < ENV_QUIET)
		s[i_oc1] += op_calc(CH->SLOT[SLOT2].phase, eg_out, s[CCS_C1]);

	/* SLOT 4 (C2) */
	eg_out = volume_calc(&CH->SLOT[SLOT4]);
	if (eg_out < ENV_QUIET)
		s[CCS_CAR] += op_calc(CH->SLOT[SLOT4].phase, eg_out, s[CCS_C2]);

	/* store current MEM */
	CH->mem_value = s[CCS_MEM];

	/* update phase counters AFTER output calculations */
	if (CH->pms) {
		update_phase_lfo(OPN, CH, lfo_pm);
	} else {
		CH->SLOT[SLOT1].phase += CH->SLOT[SLOT1].Incr;
		CH->SLOT[SLOT2].phase += CH->SLOT[SLOT2].Incr;
		CH->SLOT[SLOT3].phase += CH->SLOT[SLOT3].Incr;
		CH->SLOT[SLOT4].phase += CH->SLOT[SLOT4].Incr;
	}
	return s[CCS_CAR];
}

/* update phase increment and envelope generator */INLINE void refresh_fc_eg_slot(
		FM_SLOT *SLOT, int fc, int kc) {
	int ksr;

	/* (frequency) phase increment counter */
	SLOT->Incr = ((fc + SLOT->DT[kc]) * SLOT->mul) >> 1;

	ksr = kc >> SLOT->KSR;
	if (SLOT->ksr != ksr) {
		SLOT->ksr = ksr;

		/* calculate envelope generator rates */
		if ((SLOT->ar + SLOT->ksr) < 32 + 62) {
			SLOT->eg_sh_ar = eg_rate_shift[SLOT->ar + SLOT->ksr];
			SLOT->eg_sel_ar = eg_rate_select[SLOT->ar + SLOT->ksr];
		} else {
			SLOT->eg_sh_ar = 0;
			SLOT->eg_sel_ar = 17 * RATE_STEPS;
		}

		SLOT->eg_sh_d1r = eg_rate_shift[SLOT->d1r + SLOT->ksr];
		SLOT->eg_sel_d1r = eg_rate_select[SLOT->d1r + SLOT->ksr];

		SLOT->eg_sh_d2r = eg_rate_shift[SLOT->d2r + SLOT->ksr];
		SLOT->eg_sel_d2r = eg_rate_select[SLOT->d2r + SLOT->ksr];

		SLOT->eg_sh_rr = eg_rate_shift[SLOT->rr + SLOT->ksr];
		SLOT->eg_sel_rr = eg_rate_select[SLOT->rr + SLOT->ksr];
	}
}

/* update phase increment counters */INLINE void refresh_fc_eg_chan(FM_CH *CH) {
	if (CH->SLOT[SLOT1].Incr == -1) {
		int fc = CH->fc;
		int kc = CH->kcode;
		refresh_fc_eg_slot(&CH->SLOT[SLOT1], fc, kc);
		refresh_fc_eg_slot(&CH->SLOT[SLOT2], fc, kc);
		refresh_fc_eg_slot(&CH->SLOT[SLOT3], fc, kc);
		refresh_fc_eg_slot(&CH->SLOT[SLOT4], fc, kc);
		CH->pm_key = 0xFFFFFFFF;   /* DT/mul/fc changed: PM cache stale */
	}
}

/* initialize time tables */
static void init_timetables(FM_ST *ST, const u8 *dttable) {
	int i, d;
	double rate;

#if 0
	logerror("FM.C: samplerate=%8i chip clock=%8i  freqbase=%f  \n",
			ST->rate, ST->clock, ST->freqbase );
#endif

	/* DeTune table */
	for (d = 0; d <= 3; d++) {
		for (i = 0; i <= 31; i++) {
			rate = ((double) dttable[d * 32 + i]) * SIN_LEN * ST->freqbase
					* (1 << FREQ_SH) / ((double) (1 << 20));
			ST->dt_tab[d][i] = (s32) rate;
			ST->dt_tab[d + 4][i] = -ST->dt_tab[d][i];
#if 0
			logerror("FM.C: DT [%2i %2i] = %8x  \n", d, i, ST->dt_tab[d][i] );
#endif
		}
	}

}

static void reset_channels(FM_ST *ST, FM_CH *CH, int num) {
	int c, s;

	ST->mode = 0; /* normal mode */
	ST->TA = 0;
	ST->TAC = 0;
	ST->TB = 0;
	ST->TBC = 0;

	for (c = 0; c < num; c++) {
		CH[c].fc = 0;
		CH[c].pm_key = 0xFFFFFFFF;   /* MVS64: PM delta cache starts stale */
		for (s = 0; s < 4; s++) {
			CH[c].SLOT[s].ssg = 0;
			CH[c].SLOT[s].ssgn = 0;
			CH[c].SLOT[s].state = EG_OFF;
			CH[c].SLOT[s].volume = MAX_ATT_INDEX;
			CH[c].SLOT[s].vol_out = MAX_ATT_INDEX;
		}
	}
}

/* initialize generic tables */
static void OPNInitTable(void) {
	signed int i, x;
	signed int n;
	double o, m;

	for (x = 0; x < TL_RES_LEN; x++) {
		m = (1 << 16) / pow(2, (x + 1) * (ENV_STEP / 4.0) / 8.0);
		m = floor(m);

		/* we never reach (1<<16) here due to the (x+1) */
		/* result fits within 16 bits at maximum */

		n = (int) m; /* 16 bits here */
		n >>= 4; /* 12 bits here */
		if (n & 1) /* round to nearest */
			n = (n >> 1) + 1;
		else
			n = n >> 1;
		/* 11 bits here (rounded) */
		n <<= 2; /* 13 bits here (as in real chip) */
		/* MVS64: only the octave-0 positive value is stored; sign and the
		 * >>octave live in tl_tab_lookup() (bit-identical, see above). */
		tl_tab_base[x] = n;
	}
	/*logerror("FM.C: TL_TAB_LEN = %i elements (%i bytes)\n",TL_TAB_LEN, (int)sizeof(tl_tab));*/

	for (i = 0; i < SIN_LEN; i++) {
		/* non-standard sinus */
		m = sin(((i * 2) + 1) * M_PI / SIN_LEN); /* checked against the real chip */

		/* we never reach zero here due to ((i*2)+1) */

		if (m > 0.0)
			o = 8 * log(1.0 / m) / log(2); /* convert to 'decibels' */
		else
			o = 8 * log(-1.0 / m) / log(2); /* convert to 'decibels' */

		o = o / (ENV_STEP / 4);

		n = (int) (2.0 * o);
		if (n & 1) /* round to nearest */
			n = (n >> 1) + 1;
		else
			n = n >> 1;

		sin_tab[i] = n * 2 + (m >= 0.0 ? 0 : 1);
		/*logerror("FM.C: sin [%4i]= %4i (tl_tab value=%5i)\n", i, sin_tab[i],tl_tab[sin_tab[i]]);*/
	}

	/*logerror("FM.C: ENV_QUIET= %08x\n",ENV_QUIET );*/

	/* build LFO PM modulation table */
	for (i = 0; i < 8; i++) /* 8 PM depths */
	{
		u8 fnum;
		for (fnum = 0; fnum < 128; fnum++) /* 7 bits meaningful of F-NUMBER */
		{
			u8 value;
			u8 step;
			u32 offset_depth = i;
			u32 offset_fnum_bit;
			u32 bit_tmp;

			for (step = 0; step < 8; step++) {
				value = 0;
				for (bit_tmp = 0; bit_tmp < 7; bit_tmp++) /* 7 bits */
				{
					if (fnum & (1 << bit_tmp)) /* only if bit "bit_tmp" is set */
					{
						offset_fnum_bit = bit_tmp * 8;
						value +=
								lfo_pm_output[offset_fnum_bit + offset_depth][step];
					}
				}
				lfo_pm_table[(fnum * 32 * 8) + (i * 32) + step + 0] = value;
				lfo_pm_table[(fnum * 32 * 8) + (i * 32) + (step ^ 7) + 8] =
						value;
				lfo_pm_table[(fnum * 32 * 8) + (i * 32) + step + 16] = -value;
				lfo_pm_table[(fnum * 32 * 8) + (i * 32) + (step ^ 7) + 24] =
						-value;
			}
#if 0
			logerror("LFO depth=%1x FNUM=%04x (<<4=%4x): ", i, fnum, fnum<<4);
			for (step=0; step<16; step++) /* dump only positive part of waveforms */
			logerror("%02x ", lfo_pm_table[(fnum*32*8) + (i*32) + step] );
			logerror("\n");
#endif

		}
	}
}

/* CSM Key Controll */INLINE void CSMKeyControll(FM_CH *CH) {
	/* this is wrong, atm */

	/* all key on */
	FM_KEYON(CH, SLOT1);
	FM_KEYON(CH, SLOT2);
	FM_KEYON(CH, SLOT3);
	FM_KEYON(CH, SLOT4);
}

/* MVS64: precomputed ADPCM-A step (was float math at every key-on — key-on runs
 * in exception context on N64, so it must be integer). Set in OPNSetPres.
 * ADPCM_SHIFT moved up here from the ADPCM section so OPNSetPres can use it. */
#define ADPCM_SHIFT    (16)      /* frequency step rate   */
static u32 adpcma_step_base;

/* prescaler set (and make time tables) */
static void OPNSetPres(FM_OPN *OPN, int pres, int TimerPres, int SSGpres) {
	int i;

	/* frequency base */
	OPN->ST.freqbase =
			(OPN->ST.rate) ? ((double) OPN->ST.clock / OPN->ST.rate) / pres : 0;

#if 0
	OPN->ST.rate = (double)OPN->ST.clock / pres;
	OPN->ST.freqbase = 1.0;
#endif

	OPN->eg_timer_add = (1 << EG_SH) * OPN->ST.freqbase;
	OPN->eg_timer_overflow = (3) * (1 << EG_SH);

	/* Timer base time */
	OPN->ST.TimerBase = 1.0 / ((double) OPN->ST.clock / (double) TimerPres);

	/* MVS64: integer runtime equivalents. Double math is allowed HERE (init /
	 * normal context); the register-write paths then use only these integers.
	 * For the NeoGeo (8MHz chip, 4MHz Z80, TimerPres=144) TimerBase_cyc8 is
	 * exactly 72 cycles/count << 8. */
	OPN->ST.TimerBase_cyc8 = (u32) (OPN->ST.TimerBase
			* (double) FM_TIMEBASE_CYC_PER_SEC * 256.0 + 0.5);
	adpcma_step_base = (u32) ((float) (1 << ADPCM_SHIFT)
			* ((float) OPN->ST.freqbase) / 3.0);

	/* SSG part  prescaler set */
	if (SSGpres)
		SSG.step = ((double) SSG_STEP * OPN->ST.rate * 8)
				/ (OPN->ST.clock * 2 / SSGpres);

	/* make time tables */
	init_timetables(&OPN->ST, dt_tab);

	/* there are 2048 FNUMs that can be generated using FNUM/BLK registers
	 but LFO works with one more bit of a precision so we really need 4096 elements */
	/* calculate fnumber -> increment counter table */
	for (i = 0; i < 4096; i++) {
		/* freq table for octave 7 */
		/* OPN phase increment counter = 20bit */
		OPN->fn_table[i] = (u32) ((double) i * 32 * OPN->ST.freqbase
				* (1 << (FREQ_SH - 10))); /* -10 because chip works with 10.10 fixed point, while we use 16.16 */
#if 0
		logerror("FM.C: fn_table[%4i] = %08x (dec=%8i)\n",
				i, OPN->fn_table[i]>>6,OPN->fn_table[i]>>6 );
#endif
	}

	/* LFO freq. table */
	for (i = 0; i < 8; i++) {
		/* Amplitude modulation: 64 output levels (triangle waveform); 1 level lasts for one of "lfo_samples_per_step" samples */
		/* Phase modulation: one entry from lfo_pm_output lasts for one of 4 * "lfo_samples_per_step" samples  */
		OPN->lfo_freq[i] = (1.0 / lfo_samples_per_step[i]) * (1 << LFO_SH)
				* OPN->ST.freqbase;
#if 0
		logerror("FM.C: lfo_freq[%i] = %08x (dec=%8i)\n",
				i, OPN->lfo_freq[i],OPN->lfo_freq[i] );
#endif
	}
}

/* write a OPN mode register 0x20-0x2f */
static void OPNWriteMode(FM_OPN *OPN, int r, int v) {
	u8 c;
	FM_CH *CH;

	switch (r) {
	case 0x21: /* Test */
		break;
	case 0x22: /* LFO FREQ (YM2608/YM2610/YM2610B/YM2612) */
		if (v & 0x08) /* LFO enabled ? */
		{
			OPN->lfo_inc = OPN->lfo_freq[v & 7];
		} else {
			OPN->lfo_inc = 0;
		}
		break;
	case 0x24: /* timer A High 8*/
		OPN->ST.TA = (OPN->ST.TA & 0x03) | (((int) v) << 2);
		break;
	case 0x25: /* timer A Low 2*/
		OPN->ST.TA = (OPN->ST.TA & 0x3fc) | (v & 3);
		break;
	case 0x26: /* timer B */
		OPN->ST.TB = v;
		break;
	case 0x27: /* mode, timer control */
		set_timers(&(OPN->ST), v);
		break;
	case 0x28: /* key on / off */
		c = v & 0x03;
		if (c == 3)
			break;
		if (v & 0x04)
			c += 3;
		CH = OPN->P_CH;
		CH = &CH[c];
		if (v & 0x10)
			FM_KEYON(CH, SLOT1);
		else
			FM_KEYOFF(CH, SLOT1);
		if (v & 0x20)
			FM_KEYON(CH, SLOT2);
		else
			FM_KEYOFF(CH, SLOT2);
		if (v & 0x40)
			FM_KEYON(CH, SLOT3);
		else
			FM_KEYOFF(CH, SLOT3);
		if (v & 0x80)
			FM_KEYON(CH, SLOT4);
		else
			FM_KEYOFF(CH, SLOT4);
		break;
	}
}

/* write a OPN register (0x30-0xff) */
static void OPNWriteReg(FM_OPN *OPN, int r, int v) {
	FM_CH *CH;
	FM_SLOT *SLOT;

	u8 c = OPN_CHAN(r);

	if (c == 3)
		return; /* 0xX3,0xX7,0xXB,0xXF */

	if (r >= 0x100)
		c += 3;

	CH = OPN->P_CH;
	CH = &CH[c];

	SLOT = &(CH->SLOT[OPN_SLOT(r)]);

	switch (r & 0xf0) {
	case 0x30: /* DET , MUL */
		set_det_mul(&OPN->ST, CH, SLOT, v);
		break;

	case 0x40: /* TL */
		set_tl(CH, SLOT, v);
		break;

	case 0x50: /* KS, AR */
		set_ar_ksr(CH, SLOT, v);
		break;

	case 0x60: /* bit7 = AM ENABLE, DR */
		set_dr(SLOT, v);
		SLOT->AMmask = (v & 0x80) ? ~0 : 0;
		break;

	case 0x70: /*     SR */
		set_sr(SLOT, v);
		break;

	case 0x80: /* SL, RR */
		set_sl_rr(SLOT, v);
		break;

	case 0x90: /* SSG-EG */

		SLOT->ssg = v & 0x0f;
		SLOT->ssgn = (v & 0x04) >> 1; /* bit 1 in ssgn = attack */
		/* MVS64: keep vol_out coherent (EG_OFF slots are skipped in
		 * advance_eg_channel, so the negate state must be applied here). */
		{
			unsigned int out = SLOT->tl + (u32) SLOT->volume;
			if ((SLOT->ssg & 0x08) && (SLOT->ssgn & 2))
				out ^= ((1 << ENV_BITS) - 1);
			SLOT->vol_out = out;
		}

		/* SSG-EG envelope shapes :

		 E AtAlH
		 1 0 0 0  \\\\

        1 0 0 1  \___

		 1 0 1 0  \/\/
		 ___
		 1 0 1 1
		 1 1 0 0  ////
		 ___
		 1 1 0 1  /

		 1 1 1 0  /\/
		 1 1 1 1  /___


		 E = SSG-EG enable


		 The shapes are generated using Attack, Decay and Sustain phases.

		 Each single character in the diagrams above represents this whole
		 sequence:

		 - when KEY-ON = 1, normal Attack phase is generated (*without* any
		 difference when compared to normal mode),

		 - later, when envelope level reaches minimum level (max volume),
		 the EG switches to Decay phase (which works with bigger steps
		 when compared to normal mode - see below),

		 - later when envelope level passes the SL level,
		 the EG swithes to Sustain phase (which works with bigger steps
		 when compared to normal mode - see below),

		 - finally when envelope level reaches maximum level (min volume),
		 the EG switches to Attack phase again (depends on actual waveform).

		 Important is that when switch to Attack phase occurs, the phase counter
		 of that operator will be zeroed-out (as in normal KEY-ON) but not always.
		 (I havent found the rule for that - perhaps only when the output level is low)

		 The difference (when compared to normal Envelope Generator mode) is
		 that the resolution in Decay and Sustain phases is 4 times lower;
		 this results in only 256 steps instead of normal 1024.
		 In other words:
		 when SSG-EG is disabled, the step inside of the EG is one,
		 when SSG-EG is enabled, the step is four (in Decay and Sustain phases).

		 Times between the level changes are the same in both modes.


		 Important:
		 Decay 1 Level (so called SL) is compared to actual SSG-EG output, so
		 it is the same in both SSG and no-SSG modes, with this exception:

		 when the SSG-EG is enabled and is generating raising levels
		 (when the EG output is inverted) the SL will be found at wrong level !!!
		 For example, when SL=02:
		 0 -6 = -6dB in non-inverted EG output
		 96-6 = -90dB in inverted EG output
		 Which means that EG compares its level to SL as usual, and that the
		 output is simply inverted afterall.


		 The Yamaha's manuals say that AR should be set to 0x1f (max speed).
		 That is not necessary, but then EG will be generating Attack phase.

		 */

		break;

	case 0xa0:
		switch( OPN_SLOT(r)) {
			case 0: /* 0xa0-0xa2 : FNUM1 */
			{
				u32 fn = (((u32)( (OPN->ST.fn_h)&7))<<8) + v;
				u8 blk = OPN->ST.fn_h>>3;
				/* keyscale code */
				CH->kcode = (blk<<2) | opn_fktable[fn >> 7];
				/* phase increment counter */
				CH->fc = OPN->fn_table[fn*2]>>(7-blk);

				/* store fnum in clear form for LFO PM calculations */
				CH->block_fnum = (blk<<11) | fn;

				CH->SLOT[SLOT1].Incr=-1;
			}
			break;
			case 1: /* 0xa4-0xa6 : FNUM2,BLK */
			OPN->ST.fn_h = v&0x3f;
			break;
			case 2: /* 0xa8-0xaa : 3CH FNUM1 */
			if(r < 0x100)
			{
				u32 fn = (((u32)(OPN->SL3.fn_h&7))<<8) + v;
				u8 blk = OPN->SL3.fn_h>>3;
				/* keyscale code */
				OPN->SL3.kcode[c]= (blk<<2) | opn_fktable[fn >> 7];
				/* phase increment counter */
				OPN->SL3.fc[c] = OPN->fn_table[fn*2]>>(7-blk);
				OPN->SL3.block_fnum[c] = fn;
				(OPN->P_CH)[2].SLOT[SLOT1].Incr=-1;
			}
			break;
			case 3: /* 0xac-0xae : 3CH FNUM2,BLK */
			if(r < 0x100)
			OPN->SL3.fn_h = v&0x3f;
			break;
		}
		break;

		case 0xb0:
		switch( OPN_SLOT(r) ) {
			case 0: /* 0xb0-0xb2 : FB,ALGO */
			{
				int feedback = (v>>3)&7;
				CH->ALGO = v&7;
				CH->FB = feedback ? feedback+6 : 0;
				setup_connection( CH, c );
			}
			break;
			case 1: /* 0xb4-0xb6 : L , R , AMS , PMS (YM2612/YM2610B/YM2610/YM2608) */
			{
				/* b0-2 PMS */
				CH->pms = (v & 7) * 32; /* CH->pms = PM depth * 32 (index in lfo_pm_table) */

				/* b4-5 AMS */
				CH->ams = lfo_ams_depth_shift[(v>>4) & 0x03];

				/* PAN :  b7 = L, b6 = R */
				OPN->pan[ c*2 ] = (v & 0x80) ? ~0 : 0;
				OPN->pan[ c*2+1 ] = (v & 0x40) ? ~0 : 0;

			}
			break;
		}
		break;
	}
}

/*********************************************************************************************/

/* SSG */

static void SSGWriteReg(int r, int v) {
	int old;

	YM2610.regs[r] = v;

	switch (r) {
	case 0x00:
	case 0x02:
	case 0x04: /* Channel A/B/C Fine Tune */
	case 0x01:
	case 0x03:
	case 0x05: /* Channel A/B/C Coarse */
	{
		int ch = r >> 1;

		r &= ~1;
		YM2610.regs[r + 1] &= 0x0f;
		old = SSG.period[ch];
		SSG.period[ch] = (YM2610.regs[r] + 256 * YM2610.regs[r + 1]) * SSG.step;
		if (SSG.period[ch] == 0)
			SSG.period[ch] = SSG.step;
		SSG.count[ch] += SSG.period[ch] - old;
		if (SSG.count[ch] <= 0)
			SSG.count[ch] = 1;
	}
		break;

	case 0x06: /* Noise percent */
		YM2610.regs[SSG_NOISEPER] &= 0x1f;
		old = SSG.PeriodN;
		SSG.PeriodN = YM2610.regs[SSG_NOISEPER] * SSG.step;
		if (SSG.PeriodN == 0)
			SSG.PeriodN = SSG.step;
		SSG.CountN += SSG.PeriodN - old;
		if (SSG.CountN <= 0)
			SSG.CountN = 1;
		break;

	case 0x07: /* Enable */
		SSG.lastEnable = YM2610.regs[SSG_ENABLE];
		break;

	case 0x08:
	case 0x09:
	case 0x0a: /* Channel A/B/C Volume */
	{
		int ch = r & 3;

		YM2610.regs[r] &= 0x1f;
		SSG.envelope[ch] = YM2610.regs[r] & 0x10;
		SSG.vol[ch] =
				SSG.envelope[ch] ?
						SSG.VolE :
						SSG.vol_table[
								YM2610.regs[r] ? YM2610.regs[r] * 2 + 1 : 0];
	}
		break;

	case SSG_EFINE: // Envelope Fine
	case SSG_ECOARSE: // Envelope Coarse
		old = SSG.PeriodE;
		SSG.PeriodE = (YM2610.regs[SSG_EFINE] + 256 * YM2610.regs[SSG_ECOARSE])
				* SSG.step;
		if (SSG.PeriodE == 0)
			SSG.PeriodE = SSG.step / 2;
		SSG.CountE += SSG.PeriodE - old;
		if (SSG.CountE <= 0)
			SSG.CountE = 1;
		break;

	case SSG_ESHAPE: // Envelope Shapes
		YM2610.regs[SSG_ESHAPE] &= 0x0f;
		SSG.attack = (YM2610.regs[SSG_ESHAPE] & 0x04) ? 0x1f : 0x00;
		if ((YM2610.regs[SSG_ESHAPE] & 0x08) == 0) {
			/* if Continue = 0, map the shape to the equivalent one which has Continue = 1 */
			SSG.hold = 1;
			SSG.alternate = SSG.attack;
		} else {
			SSG.hold = YM2610.regs[SSG_ESHAPE] & 0x01;
			SSG.alternate = YM2610.regs[SSG_ESHAPE] & 0x02;
		}
		SSG.CountE = SSG.PeriodE;
		SSG.count_env = 0x1f;
		SSG.holding = 0;
		SSG.VolE = SSG.vol_table[SSG.count_env ^ SSG.attack];
		if (SSG.envelope[0])
			SSG.vol[0] = SSG.VolE;
		if (SSG.envelope[1])
			SSG.vol[1] = SSG.VolE;
		if (SSG.envelope[2])
			SSG.vol[2] = SSG.VolE;
		break;

	case SSG_PORTA: // Port A
	case SSG_PORTB: // Port B
		break;
	}
}

static int SSG_calc_count(int length) {
	int i;

	/* calc SSG count */
	for (i = 0; i < 3; i++) {
		if (YM2610.regs[SSG_ENABLE] & (0x01 << i)) {
			if (SSG.count[i] <= length * SSG_STEP)
				SSG.count[i] += length * SSG_STEP;
			SSG.output[i] = 1;
		} else if (YM2610.regs[0x08 + i] == 0) {
			if (SSG.count[i] <= length * SSG_STEP)
				SSG.count[i] += length * SSG_STEP;
		}
	}

	/* for the noise channel we must not touch OutputN - it's also not necessary */
	/* since we use outn. */
	if ((YM2610.regs[SSG_ENABLE] & 0x38) == 0x38) /* all off */
	{
		if (SSG.CountN <= length * SSG_STEP)
			SSG.CountN += length * SSG_STEP;
	}

	return (SSG.OutputN | YM2610.regs[SSG_ENABLE]);
}

static int SSG_CALC(int outn) {
	int ch;
	int vol[3];
	int left;

	/* vola, volb and volc keep track of how long each square wave stays */
	/* in the 1 position during the sample period. */
	vol[0] = vol[1] = vol[2] = 0;

	left = SSG_STEP;

	do {
		int nextevent;

		nextevent = (SSG.CountN < left) ? SSG.CountN : left;

		for (ch = 0; ch < 3; ch++) {
			if (outn & (0x08 << ch)) {
				if (SSG.output[ch])
					vol[ch] += SSG.count[ch];
				SSG.count[ch] -= nextevent;

				while (SSG.count[ch] <= 0) {
					SSG.count[ch] += SSG.period[ch];
					if (SSG.count[ch] > 0) {
						SSG.output[ch] ^= 1;
						if (SSG.output[ch])
							vol[ch] += SSG.period[ch];
						break;
					}
					SSG.count[ch] += SSG.period[ch];
					vol[ch] += SSG.period[ch];
				}
				if (SSG.output[ch])
					vol[ch] -= SSG.count[ch];
			} else {
				SSG.count[ch] -= nextevent;
				while (SSG.count[ch] <= 0) {
					SSG.count[ch] += SSG.period[ch];
					if (SSG.count[ch] > 0) {
						SSG.output[ch] ^= 1;
						break;
					}
					SSG.count[ch] += SSG.period[ch];
				}
			}
		}

		SSG.CountN -= nextevent;
		if (SSG.CountN <= 0) {
			/* Is noise output going to change? */
			if ((SSG.RNG + 1) & 2) /* (bit0^bit1)? */
			{
				SSG.OutputN = ~SSG.OutputN;
				outn = (SSG.OutputN | YM2610.regs[SSG_ENABLE]);
			}

			if (SSG.RNG & 1)
				SSG.RNG ^= 0x24000;
			SSG.RNG >>= 1;
			SSG.CountN += SSG.PeriodN;
		}

		left -= nextevent;
	} while (left > 0);

	/* update envelope */
	if (SSG.holding == 0) {
		SSG.CountE -= SSG_STEP;
		if (SSG.CountE <= 0) {
			do {
				SSG.count_env--;
				SSG.CountE += SSG.PeriodE;
			} while (SSG.CountE <= 0);

			/* check envelope current position */
			if (SSG.count_env < 0) {
				if (SSG.hold) {
					if (SSG.alternate)
						SSG.attack ^= 0x1f;
					SSG.holding = 1;
					SSG.count_env = 0;
				} else {
					/* if count_env has looped an odd number of times (usually 1), */
					/* invert the output. */
					if (SSG.alternate && (SSG.count_env & 0x20))
						SSG.attack ^= 0x1f;

					SSG.count_env &= 0x1f;
				}
			}

			SSG.VolE = SSG.vol_table[SSG.count_env ^ SSG.attack];
			/* reload volume */
			if (SSG.envelope[0])
				SSG.vol[0] = SSG.VolE;
			if (SSG.envelope[1])
				SSG.vol[1] = SSG.VolE;
			if (SSG.envelope[2])
				SSG.vol[2] = SSG.VolE;
		}
	}

	out_ssg = (((vol[0] * SSG.vol[0]) + (vol[1] * SSG.vol[1])
			+ (vol[2] * SSG.vol[2])) / SSG_STEP) / 3;

	return outn;
}

static void SSG_init_table(void) {
	int i;
	double out;

	/* calculate the volume->voltage conversion table */
	/* The AY-3-8910 has 16 levels, in a logarithmic scale (3dB per step) */
	/* The YM2149 still has 16 levels for the tone generators, but 32 for */
	/* the envelope generator (1.5dB per step). */
	out = SSG_MAX_OUTPUT;
	for (i = 31; i > 0; i--) {
		SSG.vol_table[i] = out + 0.5; /* round to nearest */

		out /= 1.188502227; /* = 10 ^ (1.5/20) = 1.5dB */
	}
	SSG.vol_table[0] = 0;
}

static void SSG_reset(void) {
	int i;

	SSG.RNG = 1;
	SSG.output[0] = 0;
	SSG.output[1] = 0;
	SSG.output[2] = 0;
	SSG.OutputN = 0xff;
	SSG.lastEnable = -1;
	for (i = 0; i < SSG_PORTA; i++) {
		YM2610.regs[i] = 0x00;
		SSGWriteReg(i, 0x00);
	}
}

static void SSG_write(int r, int v) {
	SSGWriteReg(r, v);
}

/*********************************************************************************************/

/**** YM2610 ADPCM-A defines ****/
/* MVS64: ADPCM_SHIFT define moved above OPNSetPres (adpcma_step_base). */
#define ADPCMA_ADDRESS_SHIFT 8   /* adpcm A address shift */

static u8 *pcmbufA;
static u32 pcmsizeA;

/* Algorithm and tables verified on real YM2610 */

/* usual ADPCM table (16 * 1.1^N) */
static int steps[49] = { 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55,
		60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
		253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876,
		963, 1060, 1166, 1282, 1411, 1552 };

/* different from the usual ADPCM table */
static int step_inc[8] = { -1 * 16, -1 * 16, -1 * 16, -1 * 16, 2 * 16, 5 * 16, 7
		* 16, 9 * 16 };

/* speedup purposes only */
/* MVS64: narrowed from int — max |value| = 15*steps[48]/8 = 2910.
 * 16-aligned so the RSP ADPCM offload can DMA it into DMEM. */
static s16 jedi_table[49 * 16] __attribute__((aligned(16)));

static void OPNB_ADPCMA_init_table(void) {
	int step, nib;

	for (step = 0; step < 49; step++) {
		/* loop over all nibbles and compute the difference */
		for (nib = 0; nib < 16; nib++) {
			int value = (2 * (nib & 0x07) + 1) * steps[step] / 8;
			jedi_table[step * 16 + nib] = (nib & 0x08) ? -value : value;
		}
	}

}

/* ADPCM A (Non control type) : calculate one channel output.
 * MVS64: RETURNS the sample contribution instead of accumulating it into the
 * *ch->pan bucket, so the channel-major update loop can route it into its
 * per-sample stereo accumulators (the pan pointer still identifies the bucket).
 * The end-of-sample path returns 0 — exactly the old early return that skipped
 * the *ch->pan += add. */
INLINE s32 OPNB_ADPCMA_calc_chan(
		ADPCMA *ch) {
	u32 step;
	u8 data;

	ch->now_step += ch->step;
	if (ch->now_step >= (1 << ADPCM_SHIFT)) {
		step = ch->now_step >> ADPCM_SHIFT;
		ch->now_step &= (1 << ADPCM_SHIFT) - 1;

		do {
			/* end check */
			/* 11-06-2001 JB: corrected comparison. Was > instead of == */
			/* YM2610 checks lower 20 bits only, the 4 MSB bits are sample bank */
			/* Here we use 1<<21 to compensate for nibble calculations */

			if ((ch->now_addr & ((1 << 21) - 1))
					== ((ch->end << 1) & ((1 << 21) - 1))) {
				ch->flag = 0;
				YM2610.adpcm_arrivedEndAddress |= ch->flagMask;
				return 0;
			}

			if (ch->now_addr & 1)
				data = ch->now_data & 0x0f;
			else {
				/* MVS64: resident buffer if present, else streamed window
				 * fetch (pcmsizeA still bounds the address). */
				u32 _a = ch->now_addr >> 1;
				ch->now_data = (_a < pcmsizeA)
						? (pcmbufA ? pcmbufA[_a]
						           : ym2610_vrom_fetch((int)(ch - YM2610.adpcma), _a))
						: 0;
				data = (ch->now_data >> 4) & 0x0f;
			}

			ch->now_addr++;

			ch->adpcma_acc += jedi_table[ch->adpcma_step + data];
			/* extend 12-bit signed int */

			if (ch->adpcma_acc & 0x800)
				ch->adpcma_acc |= ~0xfff;
			else
				ch->adpcma_acc &= 0xfff;

			ch->adpcma_step += step_inc[data & 7];
			Limit(ch->adpcma_step, 48*16, 0*16);

		} while (--step);

		/* calc pcm * volume data */
		ch->adpcma_out = (((Sint16) ch->adpcma_acc * ch->vol_mul)
				>> ch->vol_shift) & ~3; /* multiply, shift and mask out 2 LSB bits */
	}

	/* output for work of output channels */
	return ch->adpcma_out;
}

/* ADPCM type A Write */
static void OPNB_ADPCMA_write(int r, int v) {
	ADPCMA *adpcma = YM2610.adpcma;
	u8 c = r & 0x07;

	YM2610.regs[r] = v & 0xff; /* stock data */

	switch (r) {
	case 0x100: /* DM,--,C5,C4,C3,C2,C1,C0 */
		if (!(v & 0x80)) {
			/* KEY ON */
			for (c = 0; c < 6; c++) {
				if ((v >> c) & 1) {
					/**** start adpcm ****/
					/* MVS64: precomputed — no FP on key-on (exception context) */
					adpcma[c].step = adpcma_step_base;
					adpcma[c].now_addr = adpcma[c].start << 1;
					adpcma[c].now_step = 0;
					adpcma[c].adpcma_acc = 0;
					adpcma[c].adpcma_step = 0;
					adpcma[c].adpcma_out = 0;
					adpcma[c].flag = 1;

					/* MVS64: "mapped" = resident OR streamed (pcmsizeA > 0) */
					if (pcmbufA == NULL && pcmsizeA == 0) {
						/* Check ROM Mapped */
//						logerror("YM2610: ADPCM-A rom not mapped\n");
						adpcma[c].flag = 0;
					} else {
						if (adpcma[c].end >= pcmsizeA) {
							/* Check End in Range */
//							logerror("YM2610: ADPCM-A end out of range: $%08x\n", adpcma[c].end);
							/* adpcma[c].end = pcmsizeA - 1; *//* JB: DO NOT uncomment this, otherwise you will break the comparison in the ADPCM_CALC_CHA() */
						}
						if (adpcma[c].start >= pcmsizeA) /* Check Start in Range */
						{
//							logerror("YM2610: ADPCM-A start out of range: $%08x\n", adpcma[c].start);
							adpcma[c].flag = 0;
						}
					}
				}
			}
		} else {
			/* KEY OFF */
			for (c = 0; c < 6; c++)
				if ((v >> c) & 1)
					adpcma[c].flag = 0;
		}
		break;

	case 0x101: /* B0-5 = TL */
		YM2610.adpcmaTL = (v & 0x3f) ^ 0x3f;
		for (c = 0; c < 6; c++) {
			int volume = YM2610.adpcmaTL + adpcma[c].IL;

			if (volume >= 63) /* This is correct, 63 = quiet */
			{
				adpcma[c].vol_mul = 0;
				adpcma[c].vol_shift = 0;
			} else {
				adpcma[c].vol_mul = 15 - (volume & 7); /* so called 0.75 dB */
				adpcma[c].vol_shift = 1 + (volume >> 3); /* Yamaha engineers used the approximation: each -6 dB is close to divide by two (shift right) */
			}

			/* calc pcm * volume data */
			adpcma[c].adpcma_out = ((adpcma[c].adpcma_acc * adpcma[c].vol_mul)
					>> adpcma[c].vol_shift) & ~3; /* multiply, shift and mask out low 2 bits */
		}
		break;

	default:
		c = r & 0x07;
		if (c >= 0x06)
			return;
		switch (r & 0x138) {
		case 0x108: /* B7=L,B6=R,B4-0=IL */
		{
			int volume;

			adpcma[c].IL = (v & 0x1f) ^ 0x1f;

			volume = YM2610.adpcmaTL + adpcma[c].IL;

			if (volume >= 63) /* This is correct, 63 = quiet */
			{
				adpcma[c].vol_mul = 0;
				adpcma[c].vol_shift = 0;
			} else {
				adpcma[c].vol_mul = 15 - (volume & 7); /* so called 0.75 dB */
				adpcma[c].vol_shift = 1 + (volume >> 3); /* Yamaha engineers used the approximation: each -6 dB is close to divide by two (shift right) */
			}

			adpcma[c].pan = &out_adpcma[(v >> 6) & 0x03];

			/* calc pcm * volume data */
			adpcma[c].adpcma_out = ((adpcma[c].adpcma_acc * adpcma[c].vol_mul)
					>> adpcma[c].vol_shift) & ~3; /* multiply, shift and mask out low 2 bits */
		}
			break;

		case 0x110:
		case 0x118:
			adpcma[c].start = ((YM2610.regs[0x118 + c] << 8)
					| YM2610.regs[0x110 + c]) << ADPCMA_ADDRESS_SHIFT;
			break;

		case 0x120:
		case 0x128:
			adpcma[c].end = ((YM2610.regs[0x128 + c] << 8)
					| YM2610.regs[0x120 + c]) << ADPCMA_ADDRESS_SHIFT;
			adpcma[c].end += (1 << ADPCMA_ADDRESS_SHIFT) - 1;
			break;
		}
	}
}

/*********************************************************************************************/

/* DELTA-T particle adjuster */
#define ADPCMB_DELTA_MAX (24576)
#define ADPCMB_DELTA_MIN (127)
#define ADPCMB_DELTA_DEF (127)

#define ADPCMB_DECODE_RANGE 32768
#define ADPCMB_DECODE_MIN (-(ADPCMB_DECODE_RANGE))
#define ADPCMB_DECODE_MAX ((ADPCMB_DECODE_RANGE)-1)

static u8 *pcmbufB;
static u32 pcmsizeB;

/* Forecast to next Forecast (rate = *8) */
/* 1/8 , 3/8 , 5/8 , 7/8 , 9/8 , 11/8 , 13/8 , 15/8 */
static const s32 adpcmb_decode_table1[16] = { 1, 3, 5, 7, 9, 11, 13, 15, -1, -3,
		-5, -7, -9, -11, -13, -15, };
/* delta to next delta (rate= *64) */
/* 0.9 , 0.9 , 0.9 , 0.9 , 1.2 , 1.6 , 2.0 , 2.4 */
static const s32 adpcmb_decode_table2[16] = { 57, 57, 57, 57, 77, 102, 128, 153,
		57, 57, 57, 57, 77, 102, 128, 153 };

/* 0-DRAM x1, 1-ROM, 2-DRAM x8, 3-ROM (3 is bad setting - not allowed by the manual) */
static u8 dram_rightshift[4] = { 3, 0, 0, 0 };

/* DELTA-T-ADPCM write register */
static void OPNB_ADPCMB_write(ADPCMB *adpcmb, int r, int v) {
	if (r >= 0x20)
		return;

	YM2610.regs[r] = v; /* stock data */

	switch (r) {
	case 0x10:
		/*
		 START:
		 Accessing *external* memory is started when START bit (D7) is set to "1", so
		 you must set all conditions needed for recording/playback before starting.
		 If you access *CPU-managed* memory, recording/playback starts after
		 read/write of ADPCM data register $08.

		 REC:
		 0 = ADPCM synthesis (playback)
		 1 = ADPCM analysis (record)

		 MEMDATA:
		 0 = processor (*CPU-managed*) memory (means: using register $08)
		 1 = external memory (using start/end/limit registers to access memory: RAM or ROM)


		 SPOFF:
		 controls output pin that should disable the speaker while ADPCM analysis

		 RESET and REPEAT only work with external memory.


		 some examples:
		 value:   START, REC, MEMDAT, REPEAT, SPOFF, x,x,RESET   meaning:
		 C8     1      1    0       0       1      0 0 0       Analysis (recording) from AUDIO to CPU (to reg $08), sample rate in PRESCALER register
		 E8     1      1    1       0       1      0 0 0       Analysis (recording) from AUDIO to EXT.MEMORY,       sample rate in PRESCALER register
		 80     1      0    0       0       0      0 0 0       Synthesis (playing) from CPU (from reg $08) to AUDIO,sample rate in DELTA-N register
		 a0     1      0    1       0       0      0 0 0       Synthesis (playing) from EXT.MEMORY to AUDIO,        sample rate in DELTA-N register

		 60     0      1    1       0       0      0 0 0       External memory write via ADPCM data register $08
		 20     0      0    1       0       0      0 0 0       External memory read via ADPCM data register $08

		 */
		v |= 0x20; /*  YM2610 always uses external memory and doesn't even have memory flag bit. */
		adpcmb->portstate = v & (0x80 | 0x40 | 0x20 | 0x10 | 0x01); /* start, rec, memory mode, repeat flag copy, reset(bit0) */

		if (adpcmb->portstate & 0x80) /* START,REC,MEMDATA,REPEAT,SPOFF,--,--,RESET */
		{
			/* set PCM BUSY bit */
			adpcmb->PCM_BSY = 1;

			/* start ADPCM */
			adpcmb->now_step = 0;
			adpcmb->acc = 0;
			adpcmb->prev_acc = 0;
			adpcmb->adpcml = 0;
			adpcmb->adpcmd = ADPCMB_DELTA_DEF;
			adpcmb->now_data = 0;
		}

//		if (adpcmb->portstate & 0x20) /* do we access external memory? */
		{
			adpcmb->now_addr = adpcmb->start << 1;
			adpcmb->memread = 2; /* two dummy reads needed before accesing external memory via register $08*/

			/* if yes, then let's check if ADPCM memory is mapped and big enough */
			/* MVS64: "mapped" = resident OR streamed (pcmsizeB > 0) */
			if (!pcmbufB && pcmsizeB == 0) {
//				logerror("YM2610: Delta-T ADPCM rom not mapped\n");
				adpcmb->portstate = 0x00;
				adpcmb->PCM_BSY = 0;
			} else {
				if (adpcmb->end >= pcmsizeB) /* Check End in Range */
				{
//					logerror("YM2610: Delta-T ADPCM end out of range: $%08x\n", adpcmb->end);
					adpcmb->end = pcmsizeB - 1;
				}
				if (adpcmb->start >= pcmsizeB) /* Check Start in Range */
				{
//					logerror("YM2610: Delta-T ADPCM start out of range: $%08x\n", adpcmb->start);
					adpcmb->portstate = 0x00;
					adpcmb->PCM_BSY = 0;
				}
			}
		}
#if 0
		else /* we access CPU memory (ADPCM data register $08) so we only reset now_addr here */
		{
			adpcmb->now_addr = 0;
		}
#endif

		if (adpcmb->portstate & 0x01) {
			adpcmb->portstate = 0x00;

			/* clear PCM BUSY bit (in status register) */
			adpcmb->PCM_BSY = 0;

			/* set BRDY flag */
			if (adpcmb->status_change_BRDY_bit)
				YM2610.adpcm_arrivedEndAddress |=
						adpcmb->status_change_BRDY_bit;
		}
		break;

	case 0x11: /* L,R,-,-,SAMPLE,DA/AD,RAMTYPE,ROM */
		v |= 0x01; /*  YM2610 always uses ROM as an external memory and doesn't tave ROM/RAM memory flag bit. */
		adpcmb->pan = &out_delta[(v >> 6) & 0x03];
		if ((adpcmb->control2 & 3) != (v & 3)) {
			/*0-DRAM x1, 1-ROM, 2-DRAM x8, 3-ROM (3 is bad setting - not allowed by the manual) */
			if (adpcmb->DRAMportshift != dram_rightshift[v & 3]) {
				adpcmb->DRAMportshift = dram_rightshift[v & 3];

				/* final shift value depends on chip type and memory type selected:
				 8 for YM2610 (ROM only),
				 5 for ROM for Y8950 and YM2608,
				 5 for x8bit DRAMs for Y8950 and YM2608,
				 2 for x1bit DRAMs for Y8950 and YM2608.
				 */

				/* refresh addresses */
				adpcmb->start = ((YM2610.regs[0x13] << 8) | YM2610.regs[0x12])
						<< adpcmb->portshift;
				adpcmb->end = ((YM2610.regs[0x15] << 8) | YM2610.regs[0x14])
						<< adpcmb->portshift;
				adpcmb->end += (1 << adpcmb->portshift) - 1;
				adpcmb->limit = ((YM2610.regs[0x1d] << 8) | YM2610.regs[0x1c])
						<< adpcmb->portshift;
			}
		}
		adpcmb->control2 = v;
		break;

	case 0x12: /* Start Address L */
	case 0x13: /* Start Address H */
		adpcmb->start = ((YM2610.regs[0x13] << 8) | YM2610.regs[0x12])
				<< adpcmb->portshift;
		/*logerror("DELTAT start: 02=%2x 03=%2x addr=%8x\n", YM2610.regs[0x12], YM2610.regs[0x13], adpcmb->start);*/
		break;

	case 0x14: /* Stop Address L */
	case 0x15: /* Stop Address H */
		adpcmb->end = ((YM2610.regs[0x15] << 8) | YM2610.regs[0x14])
				<< adpcmb->portshift;
		adpcmb->end += (1 << adpcmb->portshift) - 1;
		/*logerror("DELTAT end  : 04=%2x 05=%2x addr=%8x\n", YM2610.regs[0x14], YM2610.reg[0x15], adpcmb->end);*/
		break;

	case 0x19: /* DELTA-N L (ADPCM Playback Prescaler) */
	case 0x1a: /* DELTA-N H */
		adpcmb->delta = (YM2610.regs[0x1a] << 8) | YM2610.regs[0x19];
		/* MVS64: integer 16.16 (was double) — register writes run in exception
		 * context on N64 where FP is forbidden. */
		adpcmb->step = (u32) (((unsigned long long) adpcmb->delta
						* adpcmb->freqbase16) >> 16);
		/*logerror("DELTAT deltan:09=%2x 0a=%2x\n", YM2610.regs[0x19], YM2610.regs[0x1a]);*/
		break;

	case 0x1b: /* Output level control (volume, linear) */
	{
		s32 oldvol = adpcmb->volume;
		adpcmb->volume = (v & 0xff)
				* (adpcmb->output_range / 256) / ADPCMB_DECODE_RANGE;
//								v	  *		((1<<16)>>8)		>>	15;
//						thus:	v	  *		(1<<8)				>>	15;
//						thus: output_range must be (1 << (15+8)) at least
//								v     *		((1<<23)>>8)		>>	15;
//								v	  *		(1<<15)				>>	15;
		/*logerror("DELTAT vol = %2x\n", v & 0xff);*/
		if (oldvol != 0) {
			/* MVS64: integer rescale (was double) — runtime register write */
			adpcmb->adpcml = (int) ((long long) adpcmb->adpcml
					* adpcmb->volume / oldvol);
		}
	}
		break;
	}
}

/* MVS64: like OPNB_ADPCMA_calc_chan, RETURNS the sample contribution instead
 * of accumulating into *adpcmb->pan; the EOS path returns 0 (the old early
 * return that skipped the add). */
INLINE s32 OPNB_ADPCMB_CALC(ADPCMB *adpcmb) {
	u32 step;
	int data;

	adpcmb->now_step += adpcmb->step;
	if (adpcmb->now_step >= (1 << ADPCM_SHIFT)) {
		step = adpcmb->now_step >> ADPCM_SHIFT;
		adpcmb->now_step &= (1 << ADPCM_SHIFT) - 1;
		do {
			if (adpcmb->now_addr == (adpcmb->limit << 1))
				adpcmb->now_addr = 0;

			if (adpcmb->now_addr == (adpcmb->end << 1)) {
				/* 12-06-2001 JB: corrected comparison. Was > instead of == */
				if (adpcmb->portstate & 0x10) {
					/* repeat start */
					adpcmb->now_addr = adpcmb->start << 1;
					adpcmb->acc = 0;
					adpcmb->adpcmd = ADPCMB_DELTA_DEF;
					adpcmb->prev_acc = 0;
				} else {
					/* set EOS bit in status register */
					if (adpcmb->status_change_EOS_bit)
						YM2610.adpcm_arrivedEndAddress |=
								adpcmb->status_change_EOS_bit;

					/* clear PCM BUSY bit (reflected in status register) */
					adpcmb->PCM_BSY = 0;

					adpcmb->portstate = 0;
					adpcmb->adpcml = 0;
					adpcmb->prev_acc = 0;
					return 0;
				}
			}
			if (adpcmb->now_addr & 1) {
				data = adpcmb->now_data & 0x0f;
			} else {
				/* MVS64: resident buffer if present, else streamed window 6 */
				u32 _b = adpcmb->now_addr >> 1;
				adpcmb->now_data = (_b < pcmsizeB)
						? (pcmbufB ? pcmbufB[_b] : ym2610_vrom_fetch(6, _b))
						: 0;
				data = adpcmb->now_data >> 4;
			}

			adpcmb->now_addr++;
			/* 12-06-2001 JB: */
			/* YM2610 address register is 24 bits wide.*/
			/* The "+1" is there because we use 1 bit more for nibble calculations.*/
			/* WARNING: */
			/* Side effect: we should take the size of the mapped ROM into account */
			adpcmb->now_addr &= ((1 << (24 + 1)) - 1);

			/* store accumulator value */
			adpcmb->prev_acc = adpcmb->acc;

			/* Forecast to next Forecast */
			adpcmb->acc += (adpcmb_decode_table1[data] * adpcmb->adpcmd / 8);
			Limit(adpcmb->acc, ADPCMB_DECODE_MAX, ADPCMB_DECODE_MIN);

			/* delta to next delta */
			adpcmb->adpcmd = (adpcmb->adpcmd * adpcmb_decode_table2[data]) / 64;
			Limit(adpcmb->adpcmd, ADPCMB_DELTA_MAX, ADPCMB_DELTA_MIN);

			/* ElSemi: Fix interpolator. */
			/*adpcmb->prev_acc = prev_acc + ((adpcmb->acc - prev_acc) / 2);*/

		} while (--step);

	}

	/* ElSemi: Fix interpolator. */
#if 1
	adpcmb->adpcml = adpcmb->prev_acc
			* (int) ((1 << ADPCM_SHIFT) - adpcmb->now_step);
	adpcmb->adpcml += (adpcmb->acc * (int) adpcmb->now_step);
	adpcmb->adpcml = (adpcmb->adpcml >> ADPCM_SHIFT) * (int) adpcmb->volume;
#else
	adpcmb->adpcml = ((adpcmb->acc * (int)adpcmb->now_step) >> ADPCM_SHIFT)* (int)adpcmb->volume;;
#endif

	/* output for work of output channels */
	return adpcmb->adpcml;
}

/* ============================================================================
 * MVS64: RSP ADPCM offload (-DMVS64_RSPADPCM, N64 only).
 *
 * The ADPCM source-address stream is deterministic (it does not depend on the
 * decoded data), so per chunk the CPU can stage exactly the source bytes each
 * channel will consume (through the existing streamed v.rom windows), hand the
 * RSP a parameter block, and read back per-sample L/R contribution arrays plus
 * the updated channel states. rsp_audio.S is a bit-exact port of the two
 * decoders above, restricted to the linear case; ADPCM-B chunks that could hit
 * the limit-wrap or repeat-restart paths fall back to the C decoder for that
 * chunk (rare), as does any channel whose staging would overflow.
 *
 * -DMVS64_RSPADPCM_VERIFY: dual-compute gate. The C decoders stay
 * authoritative; the RSP result is compared per sample and per state field
 * every chunk, and [RSPADPCM] telemetry reports the mismatch counters (the
 * gate is ZERO mismatches over a long ares run).
 * ==========================================================================*/
#if defined(N64) && defined(MVS64_RSPADPCM)
#include <libdragon.h>
#include <stddef.h>

extern uint32_t RSP_AUDIO_OVL_ID;

/* These mirror the PARAM/OUT layouts in rsp_audio.S exactly (natural field
 * alignment gives the byte offsets the ucode uses; asserts below verify). */
typedef struct {
	u32 now_addr, now_step, step, end_x2m;
	s32 acc, astep, aout;
	u8 now_data, flagMask, vol_mul, vol_shift;
	s32 maskL, maskR;
	u32 src_phys;
	u16 src_cur;
	u8 flag_out, pad0;
} rspa_cha_t;

typedef struct {
	u32 now_addr, now_step, step, end_x2;
	s32 acc, adpcmd, prev_acc, volume;
	s32 maskL, maskR;
	u32 src_phys;
	u16 src_cur;
	u8 now_data, flag_out;
	s32 adpcml;
	u8 eosbit;
	u8 pad[11];
} rspa_chb_t;

typedef struct __attribute__((aligned(16))) {
	u32 jedi_phys;
	u16 n;
	u8 amask, bflags;
	u8 pad0[8];
	rspa_cha_t a[6];
	rspa_chb_t b;
	u8 pad1[16];
} rspa_param_t;

typedef struct __attribute__((aligned(16))) {
	s32 l[128];
	s32 r[128];
	rspa_param_t echo;
	u32 arrived;
	u32 seq;
	u8 pad[8];
} rspa_out_t;

_Static_assert(sizeof(rspa_cha_t) == 48, "rspa_cha_t layout");
_Static_assert(sizeof(rspa_chb_t) == 64, "rspa_chb_t layout");
_Static_assert(sizeof(rspa_param_t) == 384, "rspa_param_t layout");
_Static_assert(offsetof(rspa_param_t, a) == 16, "rspa A offset");
_Static_assert(offsetof(rspa_param_t, b) == 304, "rspa B offset");
_Static_assert(offsetof(rspa_out_t, echo) == 1024, "rspa echo offset");
_Static_assert(offsetof(rspa_out_t, arrived) == 1408, "rspa arrived offset");
_Static_assert(offsetof(rspa_out_t, seq) == 1412, "rspa seq offset");
_Static_assert(sizeof(rspa_out_t) == 1424, "rspa_out_t size");

/* Source staging. The RSP DMAs fixed 176-byte slices from
 * src_phys + (src_cur & ~7), so a buffer must cover the worst-case cursor
 * (rounded down) + 176. At 11025Hz output: A consumes <= ~110 bytes per
 * 128-sample chunk (fixed 18.5kHz nibble rate), B <= ~325 (delta=0xFFFF). */
static u8 rspa_srcA[6][352] __attribute__((aligned(16)));
static u8 rspa_srcB[768] __attribute__((aligned(16)));
static rspa_param_t rspa_pb;
static rspa_out_t rspa_ob;
static u32 rspa_seqno;
static int rspa_dead;   /* poll timeout observed -> permanent C fallback */
#ifdef MVS64_RSPADPCM_VERIFY
static u32 rspa_chunks, rspa_badchunks, rspa_badsamp, rspa_badstate;
#endif

/* Stage the source bytes one channel will consume this chunk: the bytes at
 * the even addresses in [now_addr, now_addr+nib-1], i.e. byte addresses
 * starting at (now_addr+1)>>1. Returns the byte count. */
static u32 rspa_stage(int win, u32 now_addr, u32 nib, u8 *dst,
		const u8 *resident, u32 size) {
	u32 a0b = (now_addr + 1) >> 1;
	u32 cnt = nib ? ((now_addr + nib + 1) >> 1) - a0b : 0;
	u32 k;
	for (k = 0; k < cnt; k++) {
		u32 a = a0b + k;
		dst[k] = (a < size) ? (resident ? resident[a]
		                                : ym2610_vrom_fetch(win, a)) : 0;
	}
	if (cnt)
		data_cache_hit_writeback(dst, (cnt + 15) & ~(u32)15);
	return cnt;
}

/* Build the param block from live YM2610 state (called BEFORE the C decoders
 * touch anything this chunk). Returns nonzero if the RSP has work. */
static int rspa_build(int n, int dtl, int dtr) {
	int any = 0, c;
	ADPCMB * const dt = &YM2610.adpcmb;
	static int jedi_synced;
	if (!jedi_synced) {
		data_cache_hit_writeback(jedi_table, sizeof(jedi_table));
		jedi_synced = 1;
	}
	rspa_pb.jedi_phys = PhysicalAddr(jedi_table);
	rspa_pb.n = (u16) n;
	rspa_pb.amask = 0;
	rspa_pb.bflags = 0;
	for (c = 0; c < 6; c++) {
		ADPCMA * const ch = &YM2610.adpcma[c];
		rspa_cha_t * const p = &rspa_pb.a[c];
		u32 sched, to_end, nib, cnt;
		int pi;
		if (!ch->flag)
			continue;
		sched = (ch->now_step + (u32) n * ch->step) >> 16;
		to_end = ((ch->end << 1) - ch->now_addr) & ((1u << 21) - 1);
		nib = sched < to_end ? sched : to_end;
		cnt = nib ? ((ch->now_addr + nib + 1) >> 1)
		            - ((ch->now_addr + 1) >> 1) : 0;
		if (cnt > sizeof(rspa_srcA[0]) - 176)
			continue;   /* staging overflow (nonstandard rate) -> C fallback */
		rspa_stage(c, ch->now_addr, nib, rspa_srcA[c], pcmbufA, pcmsizeA);
		p->now_addr = ch->now_addr;
		p->now_step = ch->now_step;
		p->step = ch->step;
		p->end_x2m = (ch->end << 1) & ((1u << 21) - 1);
		p->acc = ch->adpcma_acc;
		p->astep = ch->adpcma_step;
		p->aout = ch->adpcma_out;
		p->now_data = ch->now_data;
		p->flagMask = ch->flagMask;
		p->vol_mul = (u8) ch->vol_mul;
		p->vol_shift = ch->vol_shift;
		pi = (int) (ch->pan - out_adpcma);
		p->maskL = (pi == OUTD_LEFT || pi == OUTD_CENTER) ? -1 : 0;
		p->maskR = (pi == OUTD_RIGHT || pi == OUTD_CENTER) ? -1 : 0;
		p->src_phys = PhysicalAddr(rspa_srcA[c]);
		p->src_cur = 0;
		p->flag_out = 1;
		rspa_pb.amask |= (u8) (1 << c);
		any = 1;
	}
	if (dt->portstate & 0x80) {
		u32 sched = (dt->now_step + (u32) n * dt->step) >> 16;
		u32 na = dt->now_addr;
		u32 lim = dt->limit << 1, end = dt->end << 1;
		u32 nib, cnt;
		int linear = 1;
		/* the RSP handles only the linear walk + end-stop; fall back for a
		 * chunk that could hit the limit-wrap or repeat-restart paths */
		if (lim >= na && lim <= na + sched)
			linear = 0;
		if ((dt->portstate & 0x10) && end >= na && end <= na + sched)
			linear = 0;
		if (na + sched + 2 >= (1u << 25))
			linear = 0;
		nib = sched;
		if (!(dt->portstate & 0x10) && end >= na && end - na < nib)
			nib = end - na;
		cnt = nib ? ((na + nib + 1) >> 1) - ((na + 1) >> 1) : 0;
		if (linear && cnt <= sizeof(rspa_srcB) - 176) {
			rspa_chb_t * const p = &rspa_pb.b;
			rspa_stage(6, na, nib, rspa_srcB, pcmbufB, pcmsizeB);
			p->now_addr = na;
			p->now_step = dt->now_step;
			p->step = dt->step;
			p->end_x2 = end;
			p->acc = dt->acc;
			p->adpcmd = dt->adpcmd;
			p->prev_acc = dt->prev_acc;
			p->volume = dt->volume;
			p->maskL = dtl ? -1 : 0;
			p->maskR = dtr ? -1 : 0;
			p->src_phys = PhysicalAddr(rspa_srcB);
			p->src_cur = 0;
			p->now_data = dt->now_data;
			p->flag_out = 1;
			p->adpcml = dt->adpcml;
			p->eosbit = dt->status_change_EOS_bit;
			rspa_pb.bflags = 1;
			any = 1;
		}
	}
	return any;
}

static void rspa_kick(void) {
	rspa_seqno++;
	data_cache_hit_writeback(&rspa_pb, sizeof(rspa_pb));
	/* drop any cached rspa_ob lines NOW so no dirty line writes back over the
	 * RSP's output later; the seq poll below goes through the uncached alias */
	data_cache_hit_invalidate(&rspa_ob, sizeof(rspa_ob));
	rspq_highpri_begin();
	rspq_write(RSP_AUDIO_OVL_ID, 0x1, PhysicalAddr(&rspa_pb),
			PhysicalAddr(&rspa_ob), rspa_seqno);
	rspq_highpri_end();
}

static int rspa_wait(void) {
	volatile u32 * const seqp = (volatile u32 *) UncachedAddr(&rspa_ob.seq);
	u32 t0 = TICKS_READ();
	while (*seqp != rspa_seqno) {
		if (TICKS_DISTANCE(t0, TICKS_READ()) > (s32) TICKS_FROM_MS(50)) {
			debugf("[RSPADPCM] TIMEOUT seq=%lu got=%lu - disabling RSP ADPCM\n",
					(unsigned long) rspa_seqno, (unsigned long) *seqp);
			rspa_dead = 1;
			return 0;
		}
	}
	return 1;
}

/* C decode of a channel subset, exactly like the classic pass 3 (used for the
 * per-chunk fallbacks, the timeout path, and the verify reference). */
static void rspa_c_decode(int n, u8 amask, int b_too,
		s32 *accl, s32 *accr, s32 *dtb_arr) {
	int i, j;
	ADPCMB * const dt = &YM2610.adpcmb;
	if (b_too) {
		for (i = 0; i < n; i++)
			dtb_arr[i] = (dt->portstate & 0x80) ? OPNB_ADPCMB_CALC(dt) : 0;
	}
	for (j = 0; j < 6; j++) {
		ADPCMA * const ch = &YM2610.adpcma[j];
		if (!(amask & (1 << j)) || !ch->flag)
			continue;
		{
			const int pi = (int) (ch->pan - out_adpcma);
			const int al = (pi == OUTD_LEFT || pi == OUTD_CENTER);
			const int ar = (pi == OUTD_RIGHT || pi == OUTD_CENTER);
			for (i = 0; i < n && ch->flag; i++) {
				const s32 o = OPNB_ADPCMA_calc_chan(ch);
				if (al)
					accl[i] += o;
				if (ar)
					accr[i] += o;
			}
		}
	}
}

/* Adopt the RSP results: fold the contribution arrays into the chunk
 * accumulators and write the echoed states back into the live structs. */
static void rspa_adopt(int n, s32 *accl, s32 *accr) {
	int c, i;
	for (i = 0; i < n; i++) {
		accl[i] += rspa_ob.l[i];
		accr[i] += rspa_ob.r[i];
	}
	for (c = 0; c < 6; c++) {
		const rspa_cha_t * const e = &rspa_ob.echo.a[c];
		ADPCMA * const ch = &YM2610.adpcma[c];
		if (!(rspa_pb.amask & (1 << c)))
			continue;
		ch->now_addr = e->now_addr;
		ch->now_step = e->now_step;
		ch->adpcma_acc = e->acc;
		ch->adpcma_step = e->astep;
		ch->adpcma_out = e->aout;
		ch->now_data = e->now_data;
		if (!e->flag_out)
			ch->flag = 0;
	}
	if (rspa_pb.bflags & 1) {
		const rspa_chb_t * const e = &rspa_ob.echo.b;
		ADPCMB * const dt = &YM2610.adpcmb;
		dt->now_addr = e->now_addr;
		dt->now_step = e->now_step;
		dt->acc = e->acc;
		dt->adpcmd = e->adpcmd;
		dt->prev_acc = e->prev_acc;
		dt->adpcml = e->adpcml;
		dt->now_data = e->now_data;
		if (!e->flag_out) {
			dt->portstate = 0;
			dt->PCM_BSY = 0;
		}
	}
	YM2610.adpcm_arrivedEndAddress |= (u8) rspa_ob.arrived;
}

#ifdef MVS64_RSPADPCM_VERIFY
/* Compare the RSP output against the (authoritative) C results.
 * refl/refr are the C per-sample contributions of the RSP-covered channels;
 * arr0/arr1 are adpcm_arrivedEndAddress before/after the C decode of those
 * channels. Counts mismatches; prints details for the first few. */
static void rspa_verify_cmp(int n, const s32 *refl, const s32 *refr,
		u8 arr0, u8 arr1) {
	int i, c, bad = 0;
	static int prints;
	for (i = 0; i < n; i++) {
		if (rspa_ob.l[i] != refl[i] || rspa_ob.r[i] != refr[i]) {
			bad++;
			if (prints < 8) {
				prints++;
				debugf("[RSPADPCM] SAMPDIFF chunk=%lu i=%d rsp=%ld/%ld c=%ld/%ld\n",
						(unsigned long) rspa_chunks, i,
						(long) rspa_ob.l[i], (long) rspa_ob.r[i],
						(long) refl[i], (long) refr[i]);
			}
		}
	}
	rspa_badsamp += (u32) bad;
	for (c = 0; c < 6; c++) {
		const rspa_cha_t * const e = &rspa_ob.echo.a[c];
		const ADPCMA * const ch = &YM2610.adpcma[c];
		if (!(rspa_pb.amask & (1 << c)))
			continue;
		if (e->now_addr != ch->now_addr || e->now_step != ch->now_step
				|| e->acc != ch->adpcma_acc || e->astep != ch->adpcma_step
				|| e->aout != ch->adpcma_out || e->now_data != ch->now_data
				|| (e->flag_out ? 1 : 0) != (ch->flag ? 1 : 0)) {
			bad++;
			rspa_badstate++;
			if (prints < 8) {
				prints++;
				debugf("[RSPADPCM] ASTATE ch%d rsp=%lx/%lx/%ld/%ld/%ld/%x/%d "
						"c=%lx/%lx/%ld/%ld/%ld/%x/%d\n", c,
						(unsigned long) e->now_addr, (unsigned long) e->now_step,
						(long) e->acc, (long) e->astep, (long) e->aout,
						e->now_data, e->flag_out,
						(unsigned long) ch->now_addr, (unsigned long) ch->now_step,
						(long) ch->adpcma_acc, (long) ch->adpcma_step,
						(long) ch->adpcma_out, ch->now_data, ch->flag ? 1 : 0);
			}
		}
	}
	if (rspa_pb.bflags & 1) {
		const rspa_chb_t * const e = &rspa_ob.echo.b;
		const ADPCMB * const dt = &YM2610.adpcmb;
		if (e->now_addr != dt->now_addr || e->now_step != dt->now_step
				|| e->acc != dt->acc || e->adpcmd != dt->adpcmd
				|| e->prev_acc != dt->prev_acc || e->adpcml != dt->adpcml
				|| e->now_data != dt->now_data
				|| (e->flag_out ? 1 : 0) != ((dt->portstate & 0x80) ? 1 : 0)) {
			bad++;
			rspa_badstate++;
			if (prints < 8) {
				prints++;
				debugf("[RSPADPCM] BSTATE rsp=%lx/%lx/%ld/%ld/%ld/%ld/%x/%d "
						"c=%lx/%lx/%ld/%ld/%ld/%ld/%x/%d\n",
						(unsigned long) e->now_addr, (unsigned long) e->now_step,
						(long) e->acc, (long) e->adpcmd, (long) e->prev_acc,
						(long) e->adpcml, e->now_data, e->flag_out,
						(unsigned long) dt->now_addr, (unsigned long) dt->now_step,
						(long) dt->acc, (long) dt->adpcmd, (long) dt->prev_acc,
						(long) dt->adpcml, dt->now_data,
						(dt->portstate & 0x80) ? 1 : 0);
			}
		}
	}
	if ((u8) (arr0 | (u8) rspa_ob.arrived) != arr1) {
		bad++;
		rspa_badstate++;
		if (prints < 8) {
			prints++;
			debugf("[RSPADPCM] ARRIVED rsp=%x arr0=%x arr1=%x\n",
					(unsigned) rspa_ob.arrived, arr0, arr1);
		}
	}
	if (bad)
		rspa_badchunks++;
}
#endif /* MVS64_RSPADPCM_VERIFY */
#endif /* N64 && MVS64_RSPADPCM */

/* ============================================================================
 * MVS64: RSP FM synthesis offload (-DMVS64_RSPFM, N64 only).
 *
 * rsp_fm.S replays pass 1 of YM2610Update_stream bit-exactly for the shipped
 * channels: the shared EG tick schedule, the per-slot envelope state machine,
 * the operator chain with feedback/MEM, and the phase generators. Per chunk
 * the CPU ships each eligible channel's state plus a per-sample dp index into
 * a precomputed phase-delta table (this is how LFO phase modulation works
 * without the 16KB fn_table: the deltas depend only on the <=8 distinct
 * lfo_pm values in a chunk). Channels fall back to the C path per chunk when
 * any slot uses SSG-EG, or when a fast LFO (48/72Hz) yields >8 distinct
 * lfo_pm values. The silent-channel fast path stays on the CPU as before.
 *
 * The RSP reconstructs sin_tab from its first 256 entries by quarter folding;
 * rspfm_init() verifies that fold against the real table once at boot and
 * permanently disables the offload if libm rounding ever breaks the symmetry.
 *
 * -DMVS64_RSPFM_VERIFY dual-computes every chunk (C authoritative) with
 * [RSPFM] mismatch telemetry — the gate is zero mismatches over a long run.
 * ==========================================================================*/
#if defined(N64) && defined(MVS64_RSPFM)
#include <libdragon.h>
#include <stddef.h>

extern uint32_t RSP_FM_OVL_ID;

/* Mirrors of the rsp_fm.S PARAM/OUT layouts (offsets asserted below). */
typedef struct {
	u32 phase;
	s32 volume;
	u32 vol_out;
	u32 pad0;
	u16 tl, sl;
	u8 sh[4];    /* EG rate shift, indexed state-1 (REL,SUS,DEC,ATT) */
	u8 sel[4];   /* eg_inc row offset, same indexing */
	u8 state, amflag;
	u16 pad1;
} rspfm_slot_t;

typedef struct {
	s32 op1_out[2];
	s32 mem_value;
	s32 maskL, maskR;
	u8 i_memc, i_om1, i_om2, i_oc1;
	u8 algo5, fb, ams, pms_mask;
	u32 pad0;
	u32 dp[8][4];    /* phase deltas per pm index, slot order S1,S3,S2,S4 */
	u8 pmidx[128];
	rspfm_slot_t slot[4];   /* memory order S1,S3,S2,S4 */
} rspfm_ch_t;

typedef struct __attribute__((aligned(16))) {
	u32 sinq_phys;
	u32 tlb_phys;
	u16 n;
	u8 chmask, pad0;
	u32 eg_base;
	u8 egt[128];
	u8 lfo_am[128];
	u8 pad1[16];
	rspfm_ch_t ch[4];
} rspfm_param_t;

typedef struct __attribute__((aligned(16))) {
	s32 l[128];
	s32 r[128];
	rspfm_ch_t echo[4];
	u32 seq;
	u8 pad[12];
} rspfm_out_t;

_Static_assert(sizeof(rspfm_slot_t) == 32, "rspfm slot layout");
_Static_assert(sizeof(rspfm_ch_t) == 416, "rspfm ch layout");
_Static_assert(offsetof(rspfm_ch_t, dp) == 32, "rspfm dp offset");
_Static_assert(offsetof(rspfm_ch_t, pmidx) == 160, "rspfm pmidx offset");
_Static_assert(offsetof(rspfm_ch_t, slot) == 288, "rspfm slot offset");
_Static_assert(offsetof(rspfm_param_t, egt) == 16, "rspfm egt offset");
_Static_assert(offsetof(rspfm_param_t, lfo_am) == 144, "rspfm lfoam offset");
_Static_assert(offsetof(rspfm_param_t, ch) == 288, "rspfm ch[] offset");
_Static_assert(sizeof(rspfm_param_t) == 288 + 4 * 416, "rspfm param size");
_Static_assert(offsetof(rspfm_out_t, echo) == 1024, "rspfm echo offset");
_Static_assert(offsetof(rspfm_out_t, seq) == 2688, "rspfm seq offset");
_Static_assert(sizeof(rspfm_out_t) == 2704, "rspfm out size");

static rspfm_param_t rspfm_pb;
static rspfm_out_t rspfm_ob;
static u32 rspfm_seqno;
static int rspfm_dead;      /* fold-check fail or poll timeout */
static int rspfm_checked;
/* last-sample pm cache values to restore into CH after adopting (pms only) */
static u32 rspfm_pmkey_last[4];
static u32 rspfm_pmdp_last[4][4];   /* C order: S1,S2,S3,S4 */
#ifdef MVS64_RSPFM_VERIFY
static u32 rspfm_chunks, rspfm_badchunks, rspfm_badsamp, rspfm_badstate;
#endif

/* One-time init: verify the RSP's quarter fold reproduces sin_tab exactly
 * (libm rounding could in principle break the mirror symmetry), and push the
 * tables to RDRAM for the RSP to DMA. */
static void rspfm_init(void) {
	int i;
	rspfm_checked = 1;
	for (i = 0; i < SIN_LEN; i++) {
		u32 j = i & 255;
		u32 rec;
		if (i & 256)
			j = 255 - j;
		rec = (u32) sin_tab[j] + ((u32) i >> 9 & 1);
		if (sin_tab[j] & 1)   /* quarter entries must carry sign 0 */
			rec = ~0u;
		if (rec != sin_tab[i]) {
			debugf("[RSPFM] sin fold check FAILED at %d (tab=%u rec=%lu)"
					" - FM offload disabled\n", i, sin_tab[i],
					(unsigned long) rec);
			rspfm_dead = 1;
			return;
		}
	}
	data_cache_hit_writeback(sin_tab, sizeof(sin_tab));
	data_cache_hit_writeback(tl_tab_base, sizeof(tl_tab_base));
	debugf("[RSPFM] sin fold check OK\n");
}

/* Compute the four phase deltas for one lfo_pm value, mirroring
 * update_phase_lfo() WITHOUT touching the CH pm cache. dp[] comes back in
 * C slot-name order S1,S2,S3,S4. */
static void rspfm_pm_dp(FM_OPN *OPN, FM_CH *CH, u32 lfo_pm, u32 dp[4]) {
	u32 block_fnum = CH->block_fnum;
	u32 fnum_lfo = ((block_fnum & 0x7f0) >> 4) * 32 * 8;
	s32 off = lfo_pm_table[fnum_lfo + CH->pms + lfo_pm];
	if (off) {
		u8 blk;
		u32 fn;
		int kc, fc;
		block_fnum = block_fnum * 2 + off;
		blk = (block_fnum & 0x7000) >> 12;
		fn = block_fnum & 0xfff;
		kc = (blk << 2) | opn_fktable[fn >> 8];
		fc = OPN->fn_table[fn] >> (7 - blk);
		dp[0] = (u32) ((fc + CH->SLOT[SLOT1].DT[kc]) * CH->SLOT[SLOT1].mul) >> 1;
		dp[1] = (u32) ((fc + CH->SLOT[SLOT2].DT[kc]) * CH->SLOT[SLOT2].mul) >> 1;
		dp[2] = (u32) ((fc + CH->SLOT[SLOT3].DT[kc]) * CH->SLOT[SLOT3].mul) >> 1;
		dp[3] = (u32) ((fc + CH->SLOT[SLOT4].DT[kc]) * CH->SLOT[SLOT4].mul) >> 1;
	} else {
		dp[0] = CH->SLOT[SLOT1].Incr;
		dp[1] = CH->SLOT[SLOT2].Incr;
		dp[2] = CH->SLOT[SLOT3].Incr;
		dp[3] = CH->SLOT[SLOT4].Incr;
	}
}

/* Pack one channel into param block slot j. Returns 0 if the channel must
 * stay on the CPU this chunk (SSG-EG in use, or >8 distinct lfo_pm values).
 * Must run BEFORE any C code mutates the channel this chunk. */
static int rspfm_pack_chan(FM_OPN *OPN, FM_CH *CH, int j, int n,
		const u8 *lfo_pm_arr, u32 panl, u32 panr,
		int i_memc, int i_om1, int i_om2, int i_oc1) {
	rspfm_ch_t * const p = &rspfm_pb.ch[j];
	static const u8 slot_names[4] = { SLOT1, SLOT3, SLOT2, SLOT4 };
	int s, i;

	for (s = 0; s < 4; s++)
		if (CH->SLOT[s].ssg & 0x08)
			return 0;

	if (CH->pms) {
		/* map each sample's lfo_pm to a dp table index (<= 8 distinct) */
		u8 vals[8];
		u32 dp_c[4];
		int nv = 0;
		for (i = 0; i < n; i++) {
			u8 v = lfo_pm_arr[i];
			int k;
			for (k = 0; k < nv; k++)
				if (vals[k] == v)
					break;
			if (k == nv) {
				if (nv == 8)
					return 0;   /* fast LFO: fall back to C this chunk */
				vals[nv++] = v;
			}
			p->pmidx[i] = (u8) k;
		}
		for (i = 0; i < nv; i++) {
			rspfm_pm_dp(OPN, CH, vals[i], dp_c);
			p->dp[i][0] = dp_c[0];   /* RSP slot order S1,S3,S2,S4 */
			p->dp[i][1] = dp_c[2];
			p->dp[i][2] = dp_c[1];
			p->dp[i][3] = dp_c[3];
		}
		p->pms_mask = 0xFF;
		/* pm cache state C would leave behind (adopted after the chunk) */
		rspfm_pmkey_last[j] = ((u32) lfo_pm_arr[n - 1] << 17) | CH->block_fnum;
		rspfm_pm_dp(OPN, CH, lfo_pm_arr[n - 1], rspfm_pmdp_last[j]);
	} else {
		p->dp[0][0] = CH->SLOT[SLOT1].Incr;
		p->dp[0][1] = CH->SLOT[SLOT3].Incr;
		p->dp[0][2] = CH->SLOT[SLOT2].Incr;
		p->dp[0][3] = CH->SLOT[SLOT4].Incr;
		p->pms_mask = 0;
	}

	p->op1_out[0] = CH->op1_out[0];
	p->op1_out[1] = CH->op1_out[1];
	p->mem_value = CH->mem_value;
	p->maskL = (s32) panl;
	p->maskR = (s32) panr;
	p->i_memc = (u8) i_memc;
	p->i_om1 = (u8) i_om1;
	p->i_om2 = (u8) i_om2;
	p->i_oc1 = (u8) i_oc1;
	p->algo5 = (CH->ALGO & 7) == 5;
	p->fb = CH->FB;
	p->ams = CH->ams;

	for (s = 0; s < 4; s++) {
		const FM_SLOT * const SL = &CH->SLOT[(int) slot_names[s]];
		rspfm_slot_t * const q = &p->slot[s];
		q->phase = SL->phase;
		q->volume = SL->volume;
		q->vol_out = SL->vol_out;
		q->tl = (u16) SL->tl;
		q->sl = (u16) SL->sl;
		q->sh[0] = SL->eg_shv[EG_REL];
		q->sh[1] = SL->eg_shv[EG_SUS];
		q->sh[2] = SL->eg_shv[EG_DEC];
		q->sh[3] = SL->eg_shv[EG_ATT];
		q->sel[0] = SL->eg_sel_rr;
		q->sel[1] = SL->eg_sel_d2r;
		q->sel[2] = SL->eg_sel_d1r;
		q->sel[3] = SL->eg_sel_ar;
		q->state = SL->state;
		q->amflag = SL->AMmask ? 1 : 0;
	}
	return 1;
}

static void rspfm_kick(int n, u32 eg_base, const u8 *egt_arr,
		const u8 *lfo_am_arr) {
	if (!rspfm_checked)
		rspfm_init();
	rspfm_pb.sinq_phys = PhysicalAddr(sin_tab);
	rspfm_pb.tlb_phys = PhysicalAddr(tl_tab_base);
	rspfm_pb.n = (u16) n;
	rspfm_pb.eg_base = eg_base;
	memcpy(rspfm_pb.egt, egt_arr, (size_t) n);
	memcpy(rspfm_pb.lfo_am, lfo_am_arr, (size_t) n);
	rspfm_seqno++;
	data_cache_hit_writeback(&rspfm_pb, sizeof(rspfm_pb));
	data_cache_hit_invalidate(&rspfm_ob, sizeof(rspfm_ob));
	rspq_highpri_begin();
	rspq_write(RSP_FM_OVL_ID, 0x0, PhysicalAddr(&rspfm_pb),
			PhysicalAddr(&rspfm_ob), rspfm_seqno);
	rspq_highpri_end();
}

static int rspfm_wait(void) {
	volatile u32 * const seqp = (volatile u32 *) UncachedAddr(&rspfm_ob.seq);
	u32 t0 = TICKS_READ();
	while (*seqp != rspfm_seqno) {
		if (TICKS_DISTANCE(t0, TICKS_READ()) > (s32) TICKS_FROM_MS(50)) {
			debugf("[RSPFM] TIMEOUT seq=%lu got=%lu - disabling FM offload\n",
					(unsigned long) rspfm_seqno, (unsigned long) *seqp);
			rspfm_dead = 1;
			return 0;
		}
	}
	return 1;
}

/* Write the echoed channel state back into the live structs (non-verify). */
static void rspfm_adopt_chan(FM_CH *CH, int j) {
	const rspfm_ch_t * const e = &rspfm_ob.echo[j];
	static const u8 slot_names[4] = { SLOT1, SLOT3, SLOT2, SLOT4 };
	int s;
	for (s = 0; s < 4; s++) {
		FM_SLOT * const SL = &CH->SLOT[(int) slot_names[s]];
		const rspfm_slot_t * const q = &e->slot[s];
		SL->phase = q->phase;
		SL->volume = q->volume;
		SL->vol_out = q->vol_out;
		SL->state = q->state;
	}
	CH->op1_out[0] = e->op1_out[0];
	CH->op1_out[1] = e->op1_out[1];
	CH->mem_value = e->mem_value;
	if (CH->pms) {
		CH->pm_key = rspfm_pmkey_last[j];
		CH->pm_dp[0] = rspfm_pmdp_last[j][0];
		CH->pm_dp[1] = rspfm_pmdp_last[j][1];
		CH->pm_dp[2] = rspfm_pmdp_last[j][2];
		CH->pm_dp[3] = rspfm_pmdp_last[j][3];
	}
}

#ifdef MVS64_RSPFM_VERIFY
/* Compare the RSP result against the (authoritative) C pass-1 outcome for
 * the shipped channels. refl/refr are the C contributions of exactly those
 * channels. */
static void rspfm_verify_cmp(int n, const s32 *refl, const s32 *refr,
		FM_CH **cch) {
	static const u8 fm_chix[4] = { 0, 1, 2, 3 };
	static const u8 slot_names[4] = { SLOT1, SLOT3, SLOT2, SLOT4 };
	static int prints;
	int i, j, s, bad = 0;
	(void) fm_chix;
	for (i = 0; i < n; i++) {
		if (rspfm_ob.l[i] != refl[i] || rspfm_ob.r[i] != refr[i]) {
			bad++;
			if (prints < 8) {
				prints++;
				debugf("[RSPFM] SAMPDIFF chunk=%lu i=%d rsp=%ld/%ld c=%ld/%ld\n",
						(unsigned long) rspfm_chunks, i,
						(long) rspfm_ob.l[i], (long) rspfm_ob.r[i],
						(long) refl[i], (long) refr[i]);
			}
		}
	}
	rspfm_badsamp += (u32) bad;
	for (j = 0; j < 4; j++) {
		const rspfm_ch_t * const e = &rspfm_ob.echo[j];
		const FM_CH *CH;
		if (!(rspfm_pb.chmask & (1 << j)))
			continue;
		CH = cch[j];
		for (s = 0; s < 4; s++) {
			const FM_SLOT * const SL = &CH->SLOT[(int) slot_names[s]];
			const rspfm_slot_t * const q = &e->slot[s];
			if (q->phase != SL->phase || q->volume != SL->volume
					|| q->vol_out != SL->vol_out || q->state != SL->state) {
				bad++;
				rspfm_badstate++;
				if (prints < 8) {
					prints++;
					debugf("[RSPFM] SLOTSTATE ch%d s%d rsp=%lx/%ld/%lu/%d "
							"c=%lx/%ld/%lu/%d\n", j, s,
							(unsigned long) q->phase, (long) q->volume,
							(unsigned long) q->vol_out, q->state,
							(unsigned long) SL->phase, (long) SL->volume,
							(unsigned long) SL->vol_out, SL->state);
				}
			}
		}
		if (e->op1_out[0] != CH->op1_out[0] || e->op1_out[1] != CH->op1_out[1]
				|| e->mem_value != CH->mem_value) {
			bad++;
			rspfm_badstate++;
			if (prints < 8) {
				prints++;
				debugf("[RSPFM] CHSTATE ch%d rsp=%ld/%ld/%ld c=%ld/%ld/%ld\n",
						j, (long) e->op1_out[0], (long) e->op1_out[1],
						(long) e->mem_value, (long) CH->op1_out[0],
						(long) CH->op1_out[1], (long) CH->mem_value);
			}
		}
	}
	if (bad)
		rspfm_badchunks++;
}
#endif /* MVS64_RSPFM_VERIFY */
#endif /* N64 && MVS64_RSPFM */

/*********************************************************************************************/

/* YM2610(OPNB) */

#ifdef SOUND_TEST
static int stream_pos;
static int samples_left;
#endif
static FM_TIMERHANDLER sav_TimerHandler;
static FM_IRQHANDLER sav_IRQHandler;

void YM2610Init(int clock, int rate, void *pcmroma, int pcmsizea, void *pcmromb,
		int pcmsizeb, FM_TIMERHANDLER TimerHandler, FM_IRQHANDLER IRQHandler) {
	/*
	 sound->stack    = 0x10000;
	 sound->stereo   = 1;
	 #ifdef SOUND_TEST
	 if (sound_test)
	 sound->callback = YM2610Update_SoundTest;
	 else
	 #endif
	 sound->callback = YM2610Update;
	 */

	/* clear */
	memset(&YM2610, 0, sizeof(YM2610));
	memset(&SSG, 0, sizeof(SSG));

	OPNInitTable();
	SSG_init_table();
	OPNB_ADPCMA_init_table();

	/* FM */
	YM2610.OPN.P_CH = YM2610.CH;
	YM2610.OPN.ST.clock = clock;
	YM2610.OPN.ST.rate = rate;
	/* Extend handler */
	YM2610.OPN.ST.Timer_Handler = TimerHandler;
	YM2610.OPN.ST.IRQ_Handler = IRQHandler;
	sav_TimerHandler = TimerHandler;
	sav_IRQHandler = IRQHandler;
	/* SSG */
	SSG.step = ((double) SSG_STEP * rate * 8) / clock;
	/* ADPCM-A */
	pcmbufA = (u8 *) pcmroma;
	pcmsizeA = pcmsizea;
	/* ADPCM-B */
	pcmbufB = (u8 *) pcmromb;
	pcmsizeB = pcmsizeb;

	YM2610.adpcmb.status_change_EOS_bit = 0x80; /* status flag: set bit7 on End Of Sample */

	YM2610Reset();
}

void YM2610ChangeSamplerate(int rate) {
	int i;
	YM2610.OPN.ST.rate = rate;
	SSG.step = ((double) SSG_STEP * rate * 8) / YM2610.OPN.ST.clock;
	OPNSetPres(&YM2610.OPN, 6 * 24, 6 * 24, 4 * 2); /* OPN 1/6, SSG 1/4 */
	for (i = 0; i < 6; i++) {
		YM2610.adpcma[i].step = adpcma_step_base;
	}
	YM2610.adpcmb.freqbase16 =
			(u32) (YM2610.OPN.ST.freqbase * 65536.0 + 0.5);
}
/* reset one of chip */
void YM2610Reset(void) {
	int i;
	FM_OPN *OPN = &YM2610.OPN;

	/* Reset Prescaler */
	OPNSetPres(OPN, 6 * 24, 6 * 24, 4 * 2); /* OPN 1/6, SSG 1/4 */
	/* reset SSG section */
	SSG_reset();
	/* status clear */
	FM_IRQMASK_SET(&OPN->ST, 0x03);
	FM_BUSY_CLEAR(&OPN->ST);
	OPNWriteMode(OPN, 0x27, 0x30); /* mode 0, timer reset */

	OPN->eg_timer = 0;
	OPN->eg_cnt = 0;

	FM_STATUS_RESET(&OPN->ST, 0xff);

	reset_channels(&OPN->ST, YM2610.CH, 6);
	/* reset OPerator paramater */
	for (i = 0xb6; i >= 0xb4; i--) {
		OPNWriteReg(OPN, i, 0xc0);
		OPNWriteReg(OPN, i | 0x100, 0xc0);
	}
	for (i = 0xb2; i >= 0x30; i--) {
		OPNWriteReg(OPN, i, 0x00);
		OPNWriteReg(OPN, i | 0x100, 0x00);
	}
	for (i = 0x26; i >= 0x20; i--) {
		OPNWriteReg(OPN, i, 0x00);
	}
	/**** ADPCM work initial ****/
	for (i = 0; i < 6; i++) {
		YM2610.adpcma[i].step = adpcma_step_base;
		YM2610.adpcma[i].now_addr = 0;
		YM2610.adpcma[i].now_step = 0;
		YM2610.adpcma[i].start = 0;
		YM2610.adpcma[i].end = 0;
		YM2610.adpcma[i].vol_mul = 0;
		YM2610.adpcma[i].pan = &out_adpcma[OUTD_CENTER]; /* default center */
		YM2610.adpcma[i].flagMask = 1 << i;
		YM2610.adpcma[i].flag = 0;
		YM2610.adpcma[i].adpcma_acc = 0;
		YM2610.adpcma[i].adpcma_step = 0;
		YM2610.adpcma[i].adpcma_out = 0;
	}
	YM2610.adpcmaTL = 0x3f;

	YM2610.adpcm_arrivedEndAddress = 0;

	/* ADPCM-B unit */
	YM2610.adpcmb.freqbase16 = (u32) (OPN->ST.freqbase * 65536.0 + 0.5);
	YM2610.adpcmb.portshift = 8; /* allways 8bits shift */
	YM2610.adpcmb.output_range = 1 << 23;

	YM2610.adpcmb.now_addr = 0;
	YM2610.adpcmb.now_step = 0;
	YM2610.adpcmb.step = 0;
	YM2610.adpcmb.start = 0;
	YM2610.adpcmb.end = 0;
	YM2610.adpcmb.limit = ~0; /* this way YM2610 and Y8950 (both of which don't have limit address reg) will still work */
	YM2610.adpcmb.volume = 0;
	YM2610.adpcmb.pan = &out_delta[OUTD_CENTER];
	YM2610.adpcmb.acc = 0;
	YM2610.adpcmb.prev_acc = 0;
	YM2610.adpcmb.adpcmd = 127;
	YM2610.adpcmb.adpcml = 0;
	YM2610.adpcmb.portstate = 0x20;
	YM2610.adpcmb.control2 = 0x01;

	/* The flag mask register disables the BRDY after the reset, however
	 ** as soon as the mask is enabled the flag needs to be set. */

	/* set BRDY bit in status register */
	if (YM2610.adpcmb.status_change_BRDY_bit)
		YM2610.adpcm_arrivedEndAddress |= YM2610.adpcmb.status_change_BRDY_bit;

#ifdef SOUND_TEST
	stream_pos = 0;
	samples_left = 0;
#endif
}

/* YM2610 write */
/* a = address */
/* v = value   */
int YM2610Write(int a, u8 v) {
	FM_OPN *OPN = &YM2610.OPN;
	int addr;
	int ch;

	v &= 0xff; /* adjust to 8 bit bus */

	switch (a & 3) {
	case 0: /* address port 0 */
		OPN->ST.address = v;
		YM2610.addr_A1 = 0;

		/* Write register to SSG emulator */
//		if (v < 16) SSG_write(0, v);
		break;

	case 1: /* data port 0    */
		if (YM2610.addr_A1 != 0)
			break; /* verified on real YM2608 */

		YM2610UpdateRequest();
		addr = OPN->ST.address;
		YM2610.regs[addr] = v;
		switch (addr & 0xf0) {
		case 0x00: /* SSG section */
			/* Write data to SSG emulator */
			SSG_write(addr, v);
			break;

		case 0x10: /* DeltaT ADPCM */
			switch (addr) {
			case 0x10: /* control 1 */
			case 0x11: /* control 2 */
			case 0x12: /* start address L */
			case 0x13: /* start address H */
			case 0x14: /* stop address L */
			case 0x15: /* stop address H */

			case 0x19: /* delta-n L */
			case 0x1a: /* delta-n H */
			case 0x1b: /* volume */
				OPNB_ADPCMB_write(&YM2610.adpcmb, addr, v);
				break;

			case 0x1c: /*  FLAG CONTROL : Extend Status Clear/Mask */
			{
				u8 statusmask = ~v;
				/* set arrived flag mask */
				for (ch = 0; ch < 6; ch++)
					YM2610.adpcma[ch].flagMask = statusmask & (1 << ch);

				YM2610.adpcmb.status_change_EOS_bit = statusmask & 0x80; /* status flag: set bit7 on End Of Sample */

				/* clear arrived flag */
				YM2610.adpcm_arrivedEndAddress &= statusmask;
			}
				break;
			}
			break;

		case 0x20: /* Mode Register */
			OPNWriteMode(OPN, addr, v);
			break;

		default: /* OPN section */
			/* write register */
			OPNWriteReg(OPN, addr, v);
			break;
		}
		break;

	case 2: /* address port 1 */
		OPN->ST.address = v;
		YM2610.addr_A1 = 1;
		break;

	case 3: /* data port 1    */
		if (YM2610.addr_A1 != 1)
			break; /* verified on real YM2608 */

		YM2610UpdateRequest();
		addr = YM2610.OPN.ST.address | 0x100;
		YM2610.regs[addr | 0x100] = v;
		if (addr < 0x130)
			/* 100-12f : ADPCM A section */
			OPNB_ADPCMA_write(addr, v);
		else
			OPNWriteReg(OPN, addr, v);
		break;
	}

	return OPN->ST.irq;
}

u8 YM2610Read(int a) {
	int addr = YM2610.OPN.ST.address;

	switch (a & 3) {
	case 0: /* status 0 : YM2203 compatible */
		return FM_STATUS_FLAG(&YM2610.OPN.ST) & 0x83;

	case 1: /* data 0 */
		if (addr < SSG_PORTA)
			return YM2610.regs[addr];
		if (addr == 0xff)
			return 0x01;
		break;

	case 2: /* status 1 : ADPCM status */
		/* ADPCM STATUS (arrived End Address) */
		/* B, --, A5, A4, A3, A2, A1, A0 */
		/* B     = ADPCM-B(DELTA-T) arrived end address */
		/* A0-A5 = ADPCM-A          arrived end address */
		return YM2610.adpcm_arrivedEndAddress;
	}
	return 0;
}

int YM2610TimerOver(int ch) {
	FM_ST *ST = &YM2610.OPN.ST;

	if (ch) {
		/* Timer B */
		TimerBOver(ST);
	} else {
		/* Timer A */

		YM2610UpdateRequest();

		/* timer update */
		TimerAOver(ST);

		/* CSM mode key, TL controll */
		if (ST->mode & 0x80) {
			/* CSM mode total level latch and auto key on */
			CSMKeyControll(&YM2610.CH[2]);
		}
	}

	return ST->irq;
}
s16 mixing_buffer[2][16384];
extern Uint16 play_buffer[16384];
//static Uint32 buf_pos;

/* MVS64: samples per channel-major batch. 64 keeps the per-chunk scratch
 * (~1KB) plus one channel's state inside the VR4300's 8KB dcache. */
#define YM_CHUNK 128

/* Generate samples for one of the YM2610s */
void YM2610Update_stream(int length) {
	FM_OPN *OPN = &YM2610.OPN;
	int i, j, outn;
	FMSAMPLE_MIX lt, rt;
	FM_CH *cch[6];
	Uint16 *pl = play_buffer;

	//printf("AAA %d\n",length);
	cch[0] = &YM2610.CH[1];
	cch[1] = &YM2610.CH[2];
	cch[2] = &YM2610.CH[4];
	cch[3] = &YM2610.CH[5];

	/* update frequency counter */
	refresh_fc_eg_chan(cch[0]);
	if (OPN->ST.mode & 0xc0) {
		/* 3SLOT MODE */
		if (cch[1]->SLOT[SLOT1].Incr == -1) {
			/* 3 slot mode */
			refresh_fc_eg_slot(&cch[1]->SLOT[SLOT1], OPN->SL3.fc[1],
					OPN->SL3.kcode[1]);
			refresh_fc_eg_slot(&cch[1]->SLOT[SLOT2], OPN->SL3.fc[2],
					OPN->SL3.kcode[2]);
			refresh_fc_eg_slot(&cch[1]->SLOT[SLOT3], OPN->SL3.fc[0],
					OPN->SL3.kcode[0]);
			refresh_fc_eg_slot(&cch[1]->SLOT[SLOT4], cch[1]->fc, cch[1]->kcode);
		}
	} else
		refresh_fc_eg_chan(cch[1]);
	refresh_fc_eg_chan(cch[2]);
	refresh_fc_eg_chan(cch[3]);

	/* calc SSG count */
	outn = SSG_calc_count(length);

/* MVS64: -DMVS64_YMPROF (N64) — per-pass TICKS split of the synthesis loop,
 * read+reset by the [SNDRMS] telemetry. 0=sched(LFO+EG timer) 1=FM(EG replay
 * + chan_calc) 2=SSG 3=ADPCM 4=mix+copy. */
#if defined(MVS64_YMPROF) && defined(N64)
#define YMPROF_T(x)   uint32_t _yp##x = TICKS_READ()
#define YMPROF_A(n,x) (ym_prof[n] += TICKS_DISTANCE(_yp##x, TICKS_READ()))
#else
#define YMPROF_T(x)   ((void)0)
#define YMPROF_A(n,x) ((void)0)
#endif

	/* MVS64: channel-major "batch" synthesis. The classic loop was sample-major
	 * (per sample: EG for all channels, chan_calc for all channels, SSG, ADPCM,
	 * mix), which reloads every channel's state from memory once per sample.
	 * Instead: precompute the per-sample shared schedules (LFO position, EG
	 * tick count) for a small chunk, run each FM/ADPCM channel over the whole
	 * chunk with its state register/cache-resident, and mix last. Bit-exact
	 * because channels never read each other's per-sample outputs, the shared
	 * LFO/EG sequences are replayed identically per channel, and the final mix
	 * is the same integer sum of the same terms (addition order irrelevant).
	 * Register writes (key-on, pan, ...) only happen BETWEEN update calls, so
	 * nothing can change channel routing or retrigger a channel mid-chunk.
	 * External timer mode is required: the old loop's INTERNAL_TIMER_A/B were
	 * empty macros here (no per-sample CSM key-on can occur). */
#if FM_INTERNAL_TIMER
#error "channel-major YM2610Update_stream requires FM_INTERNAL_TIMER == 0"
#endif
	{
		/* deltaT pan bucket -> stereo routing, constant within the call */
		ADPCMB * const dt = &YM2610.adpcmb;
		const int dti = (int)(dt->pan - out_delta);
		const int dtl = (dti == OUTD_LEFT || dti == OUTD_CENTER);
		const int dtr = (dti == OUTD_RIGHT || dti == OUTD_CENTER);
		/* out_fm index of the four active OPNB FM channels */
		static const u8 fmn[4] = { 1, 2, 4, 5 };

		while (length > 0) {
			const int n = length < YM_CHUNK ? length : YM_CHUNK;
			u8 lfo_am[YM_CHUNK], lfo_pm[YM_CHUNK], egt[YM_CHUNK];
			s32 acc_l[YM_CHUNK], acc_r[YM_CHUNK], dtb[YM_CHUNK];
			const u32 eg_base = OPN->eg_cnt;
#if defined(N64) && defined(MVS64_RSPADPCM) && !defined(MVS64_RSPADPCM_VERIFY)
			/* MVS64: kick the RSP ADPCM decode for this chunk NOW so it runs
			 * underneath the CPU's FM/SSG passes. Building the param block
			 * here is equivalent to building it at pass 3: passes 0-2 never
			 * touch ADPCM state. Results are collected in pass 3 below. */
			int rsp_any = 0;
			u8 ramask = 0, rb = 0;
			if (!rspa_dead) {
				rsp_any = rspa_build(n, dtl, dtr);
				if (rsp_any) {
					ramask = rspa_pb.amask;
					rb = (u8) (rspa_pb.bflags & 1);
					rspa_kick();
				}
			}
#endif

			/* pass 0: per-sample shared schedules (LFO position, EG ticks) */
			YMPROF_T(a);
			for (i = 0; i < n; i++) {
				u32 t = 0;
				advance_lfo(OPN);
				lfo_am[i] = (u8) LFO_AM;
				lfo_pm[i] = (u8) LFO_PM;
				OPN->eg_timer += OPN->eg_timer_add;
				while (OPN->eg_timer >= OPN->eg_timer_overflow) {
					OPN->eg_timer -= OPN->eg_timer_overflow;
					t++;
				}
				egt[i] = (u8) t;   /* <= 3 even at 8kHz output */
				OPN->eg_cnt += t;
				acc_l[i] = 0;
				acc_r[i] = 0;
			}
			YMPROF_A(0, a);

#if defined(N64) && defined(MVS64_RSPFM)
			/* MVS64: ship the eligible FM channels to the RSP now, so it
			 * synthesizes them underneath the CPU's remaining passes.
			 * Packing MUST happen before any C code mutates channel state
			 * this chunk; silent channels stay on the CPU fast path. */
			int fm_any = 0;
			if (!rspfm_checked)
				rspfm_init();
			if (!rspfm_dead) {
				rspfm_pb.chmask = 0;
				for (j = 0; j < 4; j++) {
					FM_CH * const CH = cch[j];
					const int algo = CH->ALGO & 7;
					if (CH->SLOT[SLOT1].state == EG_OFF
					    && CH->SLOT[SLOT2].state == EG_OFF
					    && CH->SLOT[SLOT3].state == EG_OFF
					    && CH->SLOT[SLOT4].state == EG_OFF
					    && CH->SLOT[SLOT1].vol_out >= ENV_QUIET
					    && CH->SLOT[SLOT2].vol_out >= ENV_QUIET
					    && CH->SLOT[SLOT3].vol_out >= ENV_QUIET
					    && CH->SLOT[SLOT4].vol_out >= ENV_QUIET
					    && CH->op1_out[0] == 0 && CH->op1_out[1] == 0
					    && CH->mem_value == 0)
						continue;   /* silent: CPU batch-advances phases */
					if (rspfm_pack_chan(OPN, CH, j, n, lfo_pm,
							OPN->pan[fmn[j] * 2 + 0],
							OPN->pan[fmn[j] * 2 + 1],
							ccs_memc[algo], ccs_om1[algo],
							ccs_om2[algo], ccs_oc1[algo]))
						rspfm_pb.chmask |= (u8) (1 << j);
				}
				if (rspfm_pb.chmask) {
					fm_any = 1;
					rspfm_kick(n, eg_base, egt, lfo_am);
				}
			}
#ifdef MVS64_RSPFM_VERIFY
			static s32 rspfm_refl[YM_CHUNK], rspfm_refr[YM_CHUNK];
			if (fm_any) {
				for (i = 0; i < n; i++) {
					rspfm_refl[i] = 0;
					rspfm_refr[i] = 0;
				}
			}
#endif
#endif

			/* pass 1: FM — each channel replays the same EG tick schedule
			 * with a private counter, then synthesizes its sample */
			YMPROF_T(b);
			for (j = 0; j < 4; j++) {
				FM_CH * const CH = cch[j];
				const u32 panl = OPN->pan[fmn[j] * 2 + 0];
				const u32 panr = OPN->pan[fmn[j] * 2 + 1];
				u32 cnt = eg_base;
				s32 *fm_al = acc_l, *fm_ar = acc_r;

#if defined(N64) && defined(MVS64_RSPFM)
				if (fm_any && (rspfm_pb.chmask & (1 << j))) {
#ifdef MVS64_RSPFM_VERIFY
					/* C stays authoritative but synthesizes into the ref
					 * arrays so the RSP result can be compared 1:1 */
					fm_al = rspfm_refl;
					fm_ar = rspfm_refr;
#else
					continue;   /* the RSP owns this channel this chunk */
#endif
				}
#endif

				/* MVS64: silent-channel fast path. If all four slots are
				 * parked in EG_OFF at >= ENV_QUIET attenuation, and the
				 * feedback/MEM history has already decayed to zero, the
				 * channel contributes exact zero for the whole chunk and no
				 * per-sample work can change its state: EG_OFF slots are
				 * no-ops in advance_eg_channel (vol_out frozen; key-on and
				 * register writes only happen between update calls), quiet
				 * ops produce no output for ANY AM (AM only attenuates
				 * further), and with op1_out/mem_value zero chan_calc would
				 * compute all-zero connections and leave them zero. Only the
				 * phase counters still advance — batch them. The vol_out
				 * check (not just EG_OFF) keeps the SSG-EG negate quirk
				 * exact: a negated EG_OFF slot can sit at LOW attenuation.
				 * samsho2 leaves most FM channels keyed off most of the
				 * time, so this skips the bulk of pass 1 in-game. */
				if (CH->SLOT[SLOT1].state == EG_OFF
				    && CH->SLOT[SLOT2].state == EG_OFF
				    && CH->SLOT[SLOT3].state == EG_OFF
				    && CH->SLOT[SLOT4].state == EG_OFF
				    && CH->SLOT[SLOT1].vol_out >= ENV_QUIET
				    && CH->SLOT[SLOT2].vol_out >= ENV_QUIET
				    && CH->SLOT[SLOT3].vol_out >= ENV_QUIET
				    && CH->SLOT[SLOT4].vol_out >= ENV_QUIET
				    && CH->op1_out[0] == 0 && CH->op1_out[1] == 0
				    && CH->mem_value == 0) {
					if (CH->pms) {
						/* per-sample deltas (LFO-PM), phases stay exact */
						for (i = 0; i < n; i++)
							update_phase_lfo(OPN, CH, lfo_pm[i]);
					} else {
						CH->SLOT[SLOT1].phase +=
								(u32) CH->SLOT[SLOT1].Incr * (u32) n;
						CH->SLOT[SLOT2].phase +=
								(u32) CH->SLOT[SLOT2].Incr * (u32) n;
						CH->SLOT[SLOT3].phase +=
								(u32) CH->SLOT[SLOT3].Incr * (u32) n;
						CH->SLOT[SLOT4].phase +=
								(u32) CH->SLOT[SLOT4].Incr * (u32) n;
					}
					continue;
				}

				/* Hoist the connection routing once per chunk (register
				 * writes cannot change CH->ALGO mid-chunk). */
				const int algo = CH->ALGO & 7;
				const int i_memc = ccs_memc[algo], i_om1 = ccs_om1[algo];
				const int i_om2 = ccs_om2[algo], i_oc1 = ccs_oc1[algo];
				for (i = 0; i < n; i++) {
					u32 t = egt[i];
					s32 o;
					while (t--) {
						cnt++;
						advance_eg_channel(cnt, &CH->SLOT[SLOT1]);
					}
					o = chan_calc_stream(OPN, CH, lfo_am[i], lfo_pm[i],
							algo, i_memc, i_om1, i_om2, i_oc1) >> 1;
					/* the shift right was verified on real chip */
					fm_al[i] += o & (s32) panl;
					fm_ar[i] += o & (s32) panr;
				}
			}
			YMPROF_A(1, b);

			/* pass 2: SSG */
			YMPROF_T(c);
			for (i = 0; i < n; i++) {
				outn = SSG_CALC(outn);
				acc_l[i] += out_ssg;
				acc_r[i] += out_ssg;
			}
			YMPROF_A(2, c);

			/* pass 3: deltaT ADPCM + ADPCM-A (flag/portstate can only go
			 * OFF mid-chunk — end of sample — never ON, so the per-sample
			 * guards match the old loop exactly) */
			YMPROF_T(d);
#if defined(N64) && defined(MVS64_RSPADPCM)
#ifdef MVS64_RSPADPCM_VERIFY
			{
				/* verify mode is synchronous: build+kick here, then shadow-
				 * compare the RSP result against the authoritative C decode */
				int rsp_any = 0;
				u8 ramask, rb;
				if (!rspa_dead)
					rsp_any = rspa_build(n, dtl, dtr);
				ramask = rsp_any ? rspa_pb.amask : 0;
				rb = rsp_any ? (u8) (rspa_pb.bflags & 1) : 0;
				if (rsp_any)
					rspa_kick();
				{
					/* C stays authoritative; the RSP result is shadow-compared */
					static s32 refl[YM_CHUNK], refr[YM_CHUNK];
					u8 arr0, arr1;
					for (i = 0; i < n; i++) {
						refl[i] = 0;
						refr[i] = 0;
					}
					/* channels NOT on the RSP decode straight into the chunk */
					rspa_c_decode(n, (u8) ~ramask, !rb, acc_l, acc_r, dtb);
					arr0 = YM2610.adpcm_arrivedEndAddress;
					/* RSP-covered channels decode into the reference arrays,
					 * deltaT folded exactly like the ucode folds it */
					if (rb) {
						for (i = 0; i < n; i++) {
							const s32 v = (dt->portstate & 0x80)
									? OPNB_ADPCMB_CALC(dt) : 0;
							dtb[i] = 0;
							if (dtl)
								refl[i] += v >> 9;
							if (dtr)
								refr[i] += v >> 9;
						}
					}
					rspa_c_decode(n, ramask, 0, refl, refr, NULL);
					arr1 = YM2610.adpcm_arrivedEndAddress;
					for (i = 0; i < n; i++) {
						acc_l[i] += refl[i];
						acc_r[i] += refr[i];
					}
					if (rsp_any) {
						rspa_chunks++;
						if (rspa_wait())
							rspa_verify_cmp(n, refl, refr, arr0, arr1);
						else
							rspa_badchunks++;
						if ((rspa_chunks & 1023) == 0)
							debugf("[RSPADPCM] chunks=%lu badchunks=%lu "
									"badsamp=%lu badstate=%lu\n",
									(unsigned long) rspa_chunks,
									(unsigned long) rspa_badchunks,
									(unsigned long) rspa_badsamp,
									(unsigned long) rspa_badstate);
					}
				}
			}
#else
			/* pipelined: the RSP was kicked at the top of the chunk and has
			 * been decoding underneath passes 0-2; do the C leftovers (chunk
			 * fallbacks), then collect */
			{
				if (rb) {
					for (i = 0; i < n; i++)
						dtb[i] = 0;   /* folded into l/r on the RSP */
				} else {
					for (i = 0; i < n; i++)
						dtb[i] = (dt->portstate & 0x80)
								? OPNB_ADPCMB_CALC(dt) : 0;
				}
				rspa_c_decode(n, (u8) ~ramask, 0, acc_l, acc_r, NULL);
				if (rsp_any) {
					if (rspa_wait()) {
						rspa_adopt(n, acc_l, acc_r);
					} else {
						/* timeout: no state was adopted, so the C decoders
						 * can still run this chunk from the pre-RSP state */
						rspa_c_decode(n, ramask, rb, acc_l, acc_r, dtb);
					}
				}
			}
#endif
#else
			for (i = 0; i < n; i++)
				dtb[i] = (dt->portstate & 0x80) ? OPNB_ADPCMB_CALC(dt) : 0;
			for (j = 0; j < 6; j++) {
				ADPCMA * const ch = &YM2610.adpcma[j];
				if (!ch->flag)
					continue;
				{
					const int pi = (int)(ch->pan - out_adpcma);
					const int al = (pi == OUTD_LEFT || pi == OUTD_CENTER);
					const int ar = (pi == OUTD_RIGHT || pi == OUTD_CENTER);
					for (i = 0; i < n && ch->flag; i++) {
						const s32 o = OPNB_ADPCMA_calc_chan(ch);
						if (al)
							acc_l[i] += o;
						if (ar)
							acc_r[i] += o;
					}
				}
			}
#endif
			YMPROF_A(3, d);

#if defined(N64) && defined(MVS64_RSPFM)
			/* collect the RSP FM result (kicked right after pass 0) */
			if (fm_any) {
#ifdef MVS64_RSPFM_VERIFY
				rspfm_chunks++;
				if (rspfm_wait())
					rspfm_verify_cmp(n, rspfm_refl, rspfm_refr, cch);
				else
					rspfm_badchunks++;
				for (i = 0; i < n; i++) {
					acc_l[i] += rspfm_refl[i];
					acc_r[i] += rspfm_refr[i];
				}
				if ((rspfm_chunks & 1023) == 0)
					debugf("[RSPFM] chunks=%lu badchunks=%lu badsamp=%lu "
							"badstate=%lu\n",
							(unsigned long) rspfm_chunks,
							(unsigned long) rspfm_badchunks,
							(unsigned long) rspfm_badsamp,
							(unsigned long) rspfm_badstate);
#else
				if (rspfm_wait()) {
					for (i = 0; i < n; i++) {
						acc_l[i] += rspfm_ob.l[i];
						acc_r[i] += rspfm_ob.r[i];
					}
					for (j = 0; j < 4; j++)
						if (rspfm_pb.chmask & (1 << j))
							rspfm_adopt_chan(cch[j], j);
				} else {
					/* timeout: no state was adopted — synthesize the shipped
					 * channels on the CPU from their untouched state (cold
					 * path; rspfm_dead now forces C for the session) */
					for (j = 0; j < 4; j++) {
						FM_CH * const CH = cch[j];
						const u32 panl = OPN->pan[fmn[j] * 2 + 0];
						const u32 panr = OPN->pan[fmn[j] * 2 + 1];
						u32 cnt = eg_base;
						if (!(rspfm_pb.chmask & (1 << j)))
							continue;
						{
							const int algo = CH->ALGO & 7;
							const int i_memc = ccs_memc[algo];
							const int i_om1 = ccs_om1[algo];
							const int i_om2 = ccs_om2[algo];
							const int i_oc1 = ccs_oc1[algo];
							for (i = 0; i < n; i++) {
								u32 t = egt[i];
								s32 o;
								while (t--) {
									cnt++;
									advance_eg_channel(cnt, &CH->SLOT[SLOT1]);
								}
								o = chan_calc_stream(OPN, CH, lfo_am[i],
										lfo_pm[i], algo, i_memc, i_om1,
										i_om2, i_oc1) >> 1;
								acc_l[i] += o & (s32) panl;
								acc_r[i] += o & (s32) panr;
							}
						}
					}
				}
#endif
			}
#endif

			/* pass 4: mix + output */
			YMPROF_T(e);
			for (i = 0; i < n; i++) {
				lt = acc_l[i];
				rt = acc_r[i];
				if (dtl)
					lt += dtb[i] >> 9;
				if (dtr)
					rt += dtb[i] >> 9;

				lt <<= 1;
				rt <<= 1;

				Limit(lt, MAXOUT, MINOUT);
				Limit(rt, MAXOUT, MINOUT);
				*pl++ = lt;
				*pl++ = rt;
			}
			YMPROF_A(4, e);

			length -= n;
		}
	}
}
#undef YMPROF_T
#undef YMPROF_A

void YM2610Update(int *p) {
	int i;
	s16 *buffer = (s16 *) p;
	s16 lt, rt;

	switch (/*option_samplerate*/0) {
	case 0:
		YM2610Update_stream(SOUND_SAMPLES >> 2);
		for (i = 0; i < SOUND_SAMPLES >> 2; i++) {
			lt = mixing_buffer[0][i];
			rt = mixing_buffer[1][i];
			*buffer++ = lt;
			*buffer++ = rt;
			*buffer++ = lt;
			*buffer++ = rt;
			*buffer++ = lt;
			*buffer++ = rt;
			*buffer++ = lt;
			*buffer++ = rt;
		}
		break;

	case 1:
		YM2610Update_stream(SOUND_SAMPLES >> 1);
		for (i = 0; i < SOUND_SAMPLES >> 1; i++) {
			lt = mixing_buffer[0][i];
			rt = mixing_buffer[1][i];
			*buffer++ = lt;
			*buffer++ = rt;
			*buffer++ = lt;
			*buffer++ = rt;
		}
		break;

	case 2:
		YM2610Update_stream(SOUND_SAMPLES);
		for (i = 0; i < SOUND_SAMPLES; i++) {
			*buffer++ = mixing_buffer[0][i];
			*buffer++ = mixing_buffer[1][i];
		}
		break;
	}
}

#ifdef SOUND_TEST
static int stream_pos;
static int samples_left;

void YM2610Update_SoundTest(int p)
{
	int i, length;
	s16 *buffer = (s16 *)p;

	length = SOUND_SAMPLES;

	if (samples_left)
	{
		for (i = 0; i < samples_left; i++)
		{
			*buffer++ = mixing_buffer[0][stream_pos];
			*buffer++ = mixing_buffer[1][stream_pos];
			stream_pos++;
			length--;
		}
	}

	next_frame:
	timer_update_subcpu();
	YM2610Update_stream(736);
	samples_left = 736;
	stream_pos = 0;

	for (i = 0; i < 736; i++)
	{
		*buffer++ = mixing_buffer[0][stream_pos];
		*buffer++ = mixing_buffer[1][stream_pos];
		stream_pos++;
		samples_left--;

		if (--length == 0) break;
	}
	if (length) goto next_frame;
}
#endif

#if 0  // MVS64: gngeo save-state (gzFile/mkstate_data) — not used
void ym2610_mkstate(gzFile *gzf, int mode) {
	int r;
	/* Old save state version was buggy, tried to load it anyway
	 * Thanks Robert for the fix
	 * */
	if (state_version == ST_VER2 && mode == STREAD) {
		struct ym2610_t ym2610_sav;
		memcpy(&ym2610_sav, &YM2610, sizeof(YM2610));
		mkstate_data(gzf, &YM2610, sizeof(YM2610), mode);
		/* restore some pointer */
		int fm, ch, slot;
		YM2610.OPN.ST.Timer_Handler = ym2610_sav.OPN.ST.Timer_Handler;
		YM2610.OPN.ST.IRQ_Handler = ym2610_sav.OPN.ST.IRQ_Handler;
		for (ch = 0; ch < 6; ch++) {
			YM2610.CH[ch].connect1 = ym2610_sav.CH[ch].connect1;
			YM2610.CH[ch].connect2 = ym2610_sav.CH[ch].connect2;
			YM2610.CH[ch].connect3 = ym2610_sav.CH[ch].connect3;
			YM2610.CH[ch].connect4 = ym2610_sav.CH[ch].connect4;
			YM2610.CH[ch].mem_connect = ym2610_sav.CH[ch].mem_connect;
			for (slot = 0; slot < 4; slot++) {
				YM2610.CH[ch].SLOT[slot].DT = ym2610_sav.CH[ch].SLOT[slot].DT;
			}
		}
		YM2610.OPN.P_CH = ym2610_sav.OPN.P_CH;
		for (ch = 0; ch < 6; ch++) {
			YM2610.adpcma[ch].pan = ym2610_sav.adpcma[ch].pan;
		}
		YM2610.adpcmb.pan = ym2610_sav.adpcmb.pan;
	} else {
		int fm, ch, slot;

		mkstate_data(gzf, &YM2610.regs, 512, mode);
		mkstate_data(gzf, &YM2610.OPN.ST.BusyExpire, 4, mode);
		mkstate_data(gzf, &YM2610.OPN.ST.address, 1, mode);
		mkstate_data(gzf, &YM2610.OPN.ST.irq, 1, mode);
		mkstate_data(gzf, &YM2610.OPN.ST.irqmask, 1, mode);
		mkstate_data(gzf, &YM2610.OPN.ST.status, 1, mode);
		mkstate_data(gzf, &YM2610.OPN.ST.mode, 4, mode);
		mkstate_data(gzf, &YM2610.OPN.ST.prescaler_sel, 1, mode);
		mkstate_data(gzf, &YM2610.OPN.ST.fn_h, 1, mode);
		mkstate_data(gzf, &YM2610.OPN.ST.TA, 4, mode);
		mkstate_data(gzf, &YM2610.OPN.ST.TAC, 4, mode);
		mkstate_data(gzf, &YM2610.OPN.ST.TB, 1, mode);
		mkstate_data(gzf, &YM2610.OPN.ST.TBC, 4, mode);

		for (ch = 0; ch < 6; ch++) {
			mkstate_data(gzf, YM2610.CH[ch].op1_out, 4 * 2, mode);
			mkstate_data(gzf, &YM2610.CH[ch].fc, 4, mode);

			for (slot = 0; slot < 4; slot++) {
				FM_SLOT *SLOT = &YM2610.CH[ch].SLOT[slot];

				mkstate_data(gzf, &SLOT->phase, 4, mode);
				mkstate_data(gzf, &SLOT->state, 1, mode);
				mkstate_data(gzf, &SLOT->volume, 4, mode);
			}
		}

		mkstate_data(gzf, YM2610.OPN.SL3.fc, 4 * 3, mode);
		mkstate_data(gzf, &YM2610.OPN.SL3.fn_h, 1, mode);
		mkstate_data(gzf, YM2610.OPN.SL3.kcode, 3, mode);

		mkstate_data(gzf, &YM2610.addr_A1, 1, mode);
		mkstate_data(gzf, &YM2610.adpcm_arrivedEndAddress, 1, mode);

		for (ch = 0; ch < 6; ch++) {
			mkstate_data(gzf, &YM2610.adpcma[ch].flag, 1, mode);
			mkstate_data(gzf, &YM2610.adpcma[ch].now_data, 1, mode);
			mkstate_data(gzf, &YM2610.adpcma[ch].now_addr, 4, mode);
			mkstate_data(gzf, &YM2610.adpcma[ch].now_step, 4, mode);
			mkstate_data(gzf, &YM2610.adpcma[ch].adpcma_acc, 4, mode);
			mkstate_data(gzf, &YM2610.adpcma[ch].adpcma_step, 4, mode);
			mkstate_data(gzf, &YM2610.adpcma[ch].adpcma_out, 4, mode);
		}

		mkstate_data(gzf, &YM2610.adpcmb.portstate, 1, mode);
		mkstate_data(gzf, &YM2610.adpcmb.now_addr, 4, mode);
		mkstate_data(gzf, &YM2610.adpcmb.now_step, 4, mode);
		mkstate_data(gzf, &YM2610.adpcmb.acc, 4, mode);
		mkstate_data(gzf, &YM2610.adpcmb.prev_acc, 4, mode);
		mkstate_data(gzf, &YM2610.adpcmb.adpcmd, 4, mode);
		mkstate_data(gzf, &YM2610.adpcmb.adpcml, 4, mode);

	}
	if (mode == STREAD) {

		for (r = 0; r < 16; r++) {
			SSG_write(0, r);
			SSG_write(1, YM2610.regs[r]);
		}

		for (r = 0x30; r < 0x9e; r++) {
			if ((r & 3) != 3) {
				OPNWriteReg(&YM2610.OPN, r, YM2610.regs[r]);
				OPNWriteReg(&YM2610.OPN, r | 0x100, YM2610.regs[r | 0x100]);
			}
		}

		for (r = 0xb0; r < 0xb6; r++) {
			if ((r & 3) != 3) {
				OPNWriteReg(&YM2610.OPN, r, YM2610.regs[r]);
				OPNWriteReg(&YM2610.OPN, r | 0x100, YM2610.regs[r | 0x100]);
			}
		}

		OPNB_ADPCMA_write(0x101, YM2610.regs[0x101]);
		for (r = 0; r < 6; r++) {
			OPNB_ADPCMA_write(r + 0x108, YM2610.regs[r + 0x108]);
			OPNB_ADPCMA_write(r + 0x110, YM2610.regs[r + 0x110]);
			OPNB_ADPCMA_write(r + 0x118, YM2610.regs[r + 0x118]);
			OPNB_ADPCMA_write(r + 0x120, YM2610.regs[r + 0x120]);
			OPNB_ADPCMA_write(r + 0x128, YM2610.regs[r + 0x128]);
		}

		YM2610.adpcmb.volume = 0;

		for (r = 1; r < 16; r++)
			OPNB_ADPCMB_write(&YM2610.adpcmb, r + 0x10, YM2610.regs[r + 0x10]);

		if (pcmbufB)
			YM2610.adpcmb.now_data = *(pcmbufB + (YM2610.adpcmb.now_addr >> 1));
	}
}
#endif  // MVS64: ym2610_mkstate disabled

#ifdef SAVE_STATE

STATE_SAVE( ym2610 )
{
	int slot, ch;

	state_save_byte(YM2610.regs, 512);

	state_save_double(&YM2610.OPN.ST.BusyExpire, 1);
	state_save_byte(&YM2610.OPN.ST.address, 1);
	state_save_byte(&YM2610.OPN.ST.irq, 1);
	state_save_byte(&YM2610.OPN.ST.irqmask, 1);
	state_save_byte(&YM2610.OPN.ST.status, 1);
	state_save_long(&YM2610.OPN.ST.mode, 1);
	state_save_byte(&YM2610.OPN.ST.prescaler_sel, 1);
	state_save_byte(&YM2610.OPN.ST.fn_h, 1);
	state_save_long(&YM2610.OPN.ST.TA, 1);
	state_save_long(&YM2610.OPN.ST.TAC, 1);
	state_save_byte(&YM2610.OPN.ST.TB, 1);
	state_save_long(&YM2610.OPN.ST.TBC, 1);

	for (ch = 0; ch < 6; ch++)
	{
		state_save_long(YM2610.CH[ch].op1_out, 2);
		state_save_long(&YM2610.CH[ch].fc, 1);

		for (slot = 0; slot < 4; slot++)
		{
			FM_SLOT *SLOT = &YM2610.CH[ch].SLOT[slot];

			state_save_long(&SLOT->phase, 1);
			state_save_byte(&SLOT->state, 1);
			state_save_long(&SLOT->volume, 1);
		}
	}

	state_save_long(YM2610.OPN.SL3.fc, 3);
	state_save_byte(&YM2610.OPN.SL3.fn_h, 1);
	state_save_byte(YM2610.OPN.SL3.kcode, 3);

	state_save_byte(&YM2610.addr_A1, 1);
	state_save_byte(&YM2610.adpcm_arrivedEndAddress, 1);

	for (ch = 0; ch < 6; ch++)
	{
		state_save_byte(&YM2610.adpcma[ch].flag, 1);
		state_save_byte(&YM2610.adpcma[ch].now_data, 1);
		state_save_long(&YM2610.adpcma[ch].now_addr, 1);
		state_save_long(&YM2610.adpcma[ch].now_step, 1);
		state_save_long(&YM2610.adpcma[ch].adpcma_acc, 1);
		state_save_long(&YM2610.adpcma[ch].adpcma_step, 1);
		state_save_long(&YM2610.adpcma[ch].adpcma_out, 1);
	}

	state_save_byte(&YM2610.adpcmb.portstate, 1);
	state_save_long(&YM2610.adpcmb.now_addr, 1);
	state_save_long(&YM2610.adpcmb.now_step, 1);
	state_save_long(&YM2610.adpcmb.acc, 1);
	state_save_long(&YM2610.adpcmb.prev_acc, 1);
	state_save_long(&YM2610.adpcmb.adpcmd, 1);
	state_save_long(&YM2610.adpcmb.adpcml, 1);

	state_save_long(&option_samplerate, 1);
}

STATE_LOAD( ym2610 )
{
	int slot, ch, r;

	state_load_byte(YM2610.regs, 512);

	state_load_double(&YM2610.OPN.ST.BusyExpire, 1);
	state_load_byte(&YM2610.OPN.ST.address, 1);
	state_load_byte(&YM2610.OPN.ST.irq, 1);
	state_load_byte(&YM2610.OPN.ST.irqmask, 1);
	state_load_byte(&YM2610.OPN.ST.status, 1);
	state_load_long(&YM2610.OPN.ST.mode, 1);
	state_load_byte(&YM2610.OPN.ST.prescaler_sel, 1);
	state_load_byte(&YM2610.OPN.ST.fn_h, 1);
	state_load_long(&YM2610.OPN.ST.TA, 1);
	state_load_long(&YM2610.OPN.ST.TAC, 1);
	state_load_byte(&YM2610.OPN.ST.TB, 1);
	state_load_long(&YM2610.OPN.ST.TBC, 1);

	for (ch = 0; ch < 6; ch++)
	{
		state_load_long(YM2610.CH[ch].op1_out, 2);
		state_load_long(&YM2610.CH[ch].fc, 1);

		for (slot = 0; slot < 4; slot++)
		{
			FM_SLOT *SLOT = &YM2610.CH[ch].SLOT[slot];

			state_load_long(&SLOT->phase, 1);
			state_load_byte(&SLOT->state, 1);
			state_load_long(&SLOT->volume, 1);
		}
	}

	state_load_long(YM2610.OPN.SL3.fc, 3);
	state_load_byte(&YM2610.OPN.SL3.fn_h, 1);
	state_load_byte(YM2610.OPN.SL3.kcode, 3);

	state_load_byte(&YM2610.addr_A1, 1);
	state_load_byte(&YM2610.adpcm_arrivedEndAddress, 1);

	for (ch = 0; ch < 6; ch++)
	{
		state_load_byte(&YM2610.adpcma[ch].flag, 1);
		state_load_byte(&YM2610.adpcma[ch].now_data, 1);
		state_load_long(&YM2610.adpcma[ch].now_addr, 1);
		state_load_long(&YM2610.adpcma[ch].now_step, 1);
		state_load_long(&YM2610.adpcma[ch].adpcma_acc, 1);
		state_load_long(&YM2610.adpcma[ch].adpcma_step, 1);
		state_load_long(&YM2610.adpcma[ch].adpcma_out, 1);
	}

	state_load_byte(&YM2610.adpcmb.portstate, 1);
	state_load_long(&YM2610.adpcmb.now_addr, 1);
	state_load_long(&YM2610.adpcmb.now_step, 1);
	state_load_long(&YM2610.adpcmb.acc, 1);
	state_load_long(&YM2610.adpcmb.prev_acc, 1);
	state_load_long(&YM2610.adpcmb.adpcmd, 1);
	state_load_long(&YM2610.adpcmb.adpcml, 1);

	state_load_long(&option_samplerate, 1);

	for (r = 0; r < 16; r++)
	{
		SSG_write(0, r);
		SSG_write(1, YM2610.regs[r]);
	}

	for (r = 0x30; r <0x9e; r++)
	{
		if ((r & 3) != 3)
		{
			OPNWriteReg(&YM2610.OPN, r, YM2610.regs[r]);
			OPNWriteReg(&YM2610.OPN, r | 0x100, YM2610.regs[r | 0x100]);
		}
	}

	for (r = 0xb0; r < 0xb6; r++)
	{
		if ((r & 3) != 3)
		{
			OPNWriteReg(&YM2610.OPN, r, YM2610.regs[r]);
			OPNWriteReg(&YM2610.OPN, r | 0x100, YM2610.regs[r | 0x100]);
		}
	}

	OPNB_ADPCMA_write(0x101, YM2610.regs[0x101]);
	for (r = 0; r < 6; r++)
	{
		OPNB_ADPCMA_write(r + 0x108, YM2610.regs[r + 0x108]);
		OPNB_ADPCMA_write(r + 0x110, YM2610.regs[r + 0x110]);
		OPNB_ADPCMA_write(r + 0x118, YM2610.regs[r + 0x118]);
		OPNB_ADPCMA_write(r + 0x120, YM2610.regs[r + 0x120]);
		OPNB_ADPCMA_write(r + 0x128, YM2610.regs[r + 0x128]);
	}

	YM2610.adpcmb.volume = 0;

	for (r = 1; r < 16; r++)
	OPNB_ADPCMB_write(&YM2610.adpcmb, r + 0x10, YM2610.regs[r + 0x10]);

	if (pcmbufB)
	YM2610.adpcmb.now_data = *(pcmbufB + (YM2610.adpcmb.now_addr >> 1));
}

#endif /* SAVE_STATE */

#if defined(MVS64_AUTOINPUT) || defined(MVS64_SNDHEALTH)
#include <stdio.h>
// MVS64 diagnostic: one-line snapshot of every state element that can hold a
// sustained tone, printed by sound_neogeo.c's [SNDRMS] telemetry (~1/s). Used
// to identify WHICH voice is latched during the "stuck boot beep" bug:
//   en       = SSG enable reg 0x07 (ACTIVE-LOW: bit clear = tone/noise ON)
//   v        = SSG volume regs 0x08-0x0A (bit4 = envelope mode)
//   p        = SSG tone periods (pitch of a stuck square wave)
//   fmkey    = FM key-on bits (ch*4+slot); a bit held for many seconds = stuck
//   fmhot    = FM slots NOT in EG_OFF whose vol_out is audible (<512)
//   offhot   = FM slots parked in EG_OFF whose vol_out is still audible —
//              nonzero means the EG_OFF-skip optimization left stale volume
//   ab       = ADPCM-B port state (bit7 busy; looping sample = stuck sound)
int ym2610_dbg_state(char *o, int n) {
	unsigned fmkey = 0, fmhot = 0, offhot = 0;
	for (int c = 0; c < 6; c++)
		for (int s = 0; s < 4; s++) {
			const FM_SLOT *sl = &YM2610.CH[c].SLOT[s];
			if (sl->key) fmkey |= 1u << (c * 4 + s);
			if (sl->vol_out < 512) {
				if (sl->state != EG_OFF) fmhot |= 1u << (c * 4 + s);
				else                     offhot |= 1u << (c * 4 + s);
			}
		}
	unsigned aa = 0;
	for (int c = 0; c < 6; c++)
		if (YM2610.adpcma[c].flag) aa |= 1u << c;
	return snprintf(o, n,
		"en=%02x v=%02x,%02x,%02x p=%03x,%03x,%03x fmkey=%06x fmhot=%06x offhot=%06x aa=%02x ab=%02x",
		YM2610.regs[0x07], YM2610.regs[0x08], YM2610.regs[0x09], YM2610.regs[0x0A],
		((YM2610.regs[1] & 0xf) << 8) | YM2610.regs[0],
		((YM2610.regs[3] & 0xf) << 8) | YM2610.regs[2],
		((YM2610.regs[5] & 0xf) << 8) | YM2610.regs[4],
		fmkey, fmhot, offhot, aa, YM2610.adpcmb.portstate);
}
#endif
