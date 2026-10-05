// Per-frame diagnostic telemetry for instrumented N64 builds: [DYNSTAT],
// [BOSTAT], [TRCRC], [PERF]/[PERF2]/[PERF3], [OPHIST], [IDLEPROBE] and the
// PCPROF hook. emu.c's main loop calls emu_diag_frame() once per guest frame
// (EMU_DIAG, emu.h); in a release build the file compiles to nothing.
#include <stdio.h>
#include <string.h>
#include "emu.h"
#include "platform.h"
#ifdef EMU_DIAG
#ifdef USE_M64K
#include "m64k/m64k.h"
#endif

extern int g_frame;
extern uint32_t perf_m68k_slices;
#ifdef USE_M64K
extern m64k_t m64k;
#endif

#ifdef MVS64_OPHIST
// Exact per-opcode execution histogram; bumped per dispatched instruction
// in m64k_asm.S (m64k_ophist_ptr points here). 256KB, diagnostic only.
uint32_t m64k_ophist_tab[65536] __attribute__((aligned(16)));
#endif
#ifdef M64K_DYNSTAT
// Dynarec coverage rig (m64k_asm.S dynstat_count): per-slot counts of
// control-transfer targets seen at jmp_exec (= the dynarec probe's exact
// visibility) + last-writer PC per slot for collision detection. 512KB,
// diagnostic builds only. Dumped+reset every DYNSTAT_WINDOW frames.
uint32_t m64k_dynstat_cnt[65536] __attribute__((aligned(16)));
uint32_t m64k_dynstat_pc[65536] __attribute__((aligned(16)));
#endif

void emu_diag_frame(void) {
	#if defined(MVS64_PCPROF) && defined(USE_M64K)
	{
		extern void pcprof_frame(int frame, int in_fight);
		uint32_t fpc = m64k_get_pc(&m64k) & 0xFFFFFF;
		pcprof_frame(g_frame, fpc == 0x3200 || fpc == 0x31fe);
	}
	#endif
	#ifdef M64K_DYNSTAT
	#define DYNSTAT_WINDOW 600
	if (g_frame && (g_frame % DYNSTAT_WINDOW) == 0) {
		// One pass: total + PC-region aggregates (the blueprint's
		// P_ROM / WORK_RAM / PBROM-window split — decides whether
		// bank-keyed translation must be pulled forward) and the
		// top-16 hottest control-transfer targets.
		uint64_t total = 0;
		uint32_t reg_prom = 0, reg_ram = 0, reg_pbrom = 0, reg_bios = 0, reg_other = 0;
		enum { DYNH_N = 64 };   // wide list: cross-ref with [DYNTERM]
		int top[DYNH_N]; int ntop = 0;
		for (int i = 0; i < 65536; i++) {
			uint32_t n = m64k_dynstat_cnt[i];
			if (!n) continue;
			total += n;
			uint32_t pc = m64k_dynstat_pc[i] & 0xFFFFFF;
			if      (pc < 0x100000) reg_prom  += n;
			else if (pc < 0x200000) reg_ram   += n;
			else if (pc < 0x300000) reg_pbrom += n;
			else if (pc >= 0xC00000 && pc < 0xC20000) reg_bios += n;
			else reg_other += n;
			int j = ntop;
			while (j > 0 && m64k_dynstat_cnt[top[j-1]] < n) j--;
			if (j < DYNH_N) {
				if (ntop < DYNH_N) ntop++;
				for (int k = ntop - 1; k > j; k--) top[k] = top[k-1];
				top[j] = i;
			}
		}
		framef("[DYNSTAT] win=%d total=%llu prom=%lu ram=%lu pbrom=%lu bios=%lu other=%lu\n",
			DYNSTAT_WINDOW, (unsigned long long)total,
			(unsigned long)reg_prom, (unsigned long)reg_ram,
			(unsigned long)reg_pbrom, (unsigned long)reg_bios,
			(unsigned long)reg_other);
		for (int i = 0; i < ntop; i++)
			framef("[DYNH] pc=%06lx n=%lu\n",
				(unsigned long)(m64k_dynstat_pc[top[i]] & 0xFFFFFF),
				(unsigned long)m64k_dynstat_cnt[top[i]]);
		memset(m64k_dynstat_cnt, 0, sizeof(m64k_dynstat_cnt));
		#ifdef M64K_DYNREC
		{
			// Dynarec density: cumulative translations/chains — the
			// coverage levers phase-3 escalations are gated on.
			extern uint32_t __m64k_dyn_stat_blocks, __m64k_dyn_stat_insns;
			extern uint32_t __m64k_dyn_stat_chains, __m64k_dyn_stat_refused;
			// exec = guest insns EXECUTED inside blocks this window
			// (emitted counter). exec/DYNSTAT_WINDOW vs the ~8-11k
			// insns/frame the interpreter dispatches is the residency
			// fraction — the only coverage number that predicts fps.
			extern uint32_t __m64k_dyn_stat_exec;
			framef("[DYNSTAT2] blocks=%lu insns=%lu chains=%lu refused=%lu exec=%lu execpf=%lu\n",
				(unsigned long)__m64k_dyn_stat_blocks,
				(unsigned long)__m64k_dyn_stat_insns,
				(unsigned long)__m64k_dyn_stat_chains,
				(unsigned long)__m64k_dyn_stat_refused,
				(unsigned long)__m64k_dyn_stat_exec,
				(unsigned long)(__m64k_dyn_stat_exec / DYNSTAT_WINDOW));
			__m64k_dyn_stat_exec = 0;
		}
		#endif
	}
	#endif
	#if defined(M64K_BLOCKOPS) && (defined(M64K_DYNSTAT) || defined(MVS64_BOSTAT))
	// Own window: the DYNSTAT rig costs a jal per control transfer and
	// runs ~3x slower, which stops the autoinput harness reaching a
	// fight at all — so blockop accounting must be usable without it.
	if (g_frame && (g_frame % 600) == 0) {
		{
			// Why the fused VRAM-port copy declines: a rejected
			// blockop is silent everywhere else (the loop just runs
			// interpreted, one TLB exception per word).
			extern uint32_t blockop_fire, blockop_rej_dst;
			extern uint32_t blockop_rej_bud, blockop_rej_span;
			extern uint32_t blockop_rej_dstval, blockop_rej_shape;
			extern uint32_t blockop_seen, blockop_t3val, blockop_t9val;
			extern uint32_t blockop_rej_pc;
			framef("[BOSTAT] seen=%lu shape=%lu fire=%lu rej_dst=%lu rej_bud=%lu"
			       " rej_span=%lu t3=%ld body=%04lx dstval=%06lx rejpc=%06lx\n",
				(unsigned long)blockop_seen,
				(unsigned long)blockop_rej_shape,
				(unsigned long)blockop_fire,
				(unsigned long)blockop_rej_dst,
				(unsigned long)blockop_rej_bud,
				(unsigned long)blockop_rej_span,
				(long)(int32_t)blockop_t3val,
				(unsigned long)blockop_t9val,
				(unsigned long)blockop_rej_dstval,
				(unsigned long)(blockop_rej_pc & 0xFFFFFF));
			blockop_fire = blockop_rej_dst = 0;
			blockop_rej_bud = blockop_rej_span = 0;
			blockop_seen = blockop_rej_shape = 0;
		}
	}
	#endif
	#ifdef M64K_TRACECRC
	{
		// Per-frame 68k state-trace hash (dynarec bit-exactness rig,
		// m64k.c): two runs of the same build+inputs must emit identical
		// [TRCRC] streams; a dynarec build must match the interpreter's.
		extern uint32_t __m64k_tracecrc, __m64k_tracecrc_slices;
		framef("[TRCRC] f=%d crc=%08lx slices=%lu\n", g_frame,
			(unsigned long)__m64k_tracecrc,
			(unsigned long)__m64k_tracecrc_slices);
		__m64k_tracecrc = 2166136261u;
		__m64k_tracecrc_slices = 0;
		#ifdef M64K_TRCRC_SPLIT
		{
			// Content-only hash (regs/SR, no pc/cycles): separates
			// timing displacement from real state divergence.
			extern uint32_t __m64k_tracecrc_content;
			framef("[TRCCON] f=%d crc=%08lx\n", g_frame,
				(unsigned long)__m64k_tracecrc_content);
			__m64k_tracecrc_content = 2166136261u;
		}
		#endif
	}
	#endif
	#ifdef MVS64_PERFCOUNT
	{
		// Diagnostic counters (m64k_asm.S / hw_n64.S): executed 68k
		// instructions, idle-skip fires and TLB exceptions this frame,
		// plus the draw-bucket split from emu_render (in 0.01%-of-frame
		// units to stay integer: 100.00% == 10000).
		extern uint32_t perf_m68k_insns, perf_idle_skips, perf_tlb_faults;
		extern uint32_t perf_draw_wait, perf_draw_issue, perf_draw_end;
		extern uint32_t perf_snd_pub;
		const uint32_t fb = TICKS_PER_SECOND / 60 / 10000;  // ticks per 0.01%
		{
			// Trap histogram by 64KB page and direction (hw_n64.S):
			// zeroed at the fight-era frame 2400, dumped at 6600 as
			// traps per frame x100.
			extern uint32_t perf_trap_hist[512];
			extern uint32_t perf_trap_ring[2048], perf_trap_ring_n;
			if (g_frame == 2400) {
				memset(perf_trap_hist, 0, sizeof(uint32_t) * 512);
				perf_trap_ring_n = 0;
			}
			if (g_frame == 6600) {
				for (uint32_t i = 0; i < perf_trap_ring_n; i++)
					debugf("[TRAPS] epc=%08lx pc=%06lx\n",
					       (unsigned long)perf_trap_ring[2*i],
					       (unsigned long)(perf_trap_ring[2*i+1] & 0xFFFFFF));
				for (int i = 0; i < 512; i++)
					if (perf_trap_hist[i])
						debugf("[TRAPH] %s page=%02x per_frame_x100=%lu\n",
						       i >= 256 ? "W" : "R", i & 255,
						       (unsigned long)(perf_trap_hist[i] * 100UL / 4200));
			}
		}
		framef("[PERF] insns=%lu skips=%lu tlb=%lu slices=%lu dwait=%lu dissue=%lu dend=%lu pub=%lu\n",
			(unsigned long)perf_m68k_insns,
			(unsigned long)perf_idle_skips,
			(unsigned long)perf_tlb_faults,
			(unsigned long)perf_m68k_slices,
			(unsigned long)(perf_draw_wait / fb),
			(unsigned long)(perf_draw_issue / fb),
			(unsigned long)(perf_draw_end / fb),
			(unsigned long)(perf_snd_pub / fb));
		perf_m68k_insns = perf_idle_skips = perf_tlb_faults = 0;
		perf_m68k_slices = 0;
		perf_draw_wait = perf_draw_issue = perf_draw_end = 0;
		perf_snd_pub = 0;

		// DRAW-1 fine split of the draw-issue bucket (video.c): phase
		// ticks (same 0.01%-of-frame units) + per-frame draw counts.
		// walk = sprite pass minus cache lookups minus rspq issue.
		extern uint32_t perf_dr_begin, perf_dr_sprites, perf_dr_fix;
		extern uint32_t perf_dr_cache, perf_dr_rspq;
		extern uint32_t perf_dr_tiles, perf_dr_cells, perf_dr_empty;
		extern uint32_t perf_walk_spr, perf_walk_iter;
		framef("[PERF2] begin=%lu spr=%lu (cache=%lu rspq=%lu) fix=%lu tiles=%lu cells=%lu empty=%lu wspr=%lu witer=%lu\n",
			(unsigned long)(perf_dr_begin / fb),
			(unsigned long)(perf_dr_sprites / fb),
			(unsigned long)(perf_dr_cache / fb),
			(unsigned long)(perf_dr_rspq / fb),
			(unsigned long)(perf_dr_fix / fb),
			(unsigned long)perf_dr_tiles,
			(unsigned long)perf_dr_cells,
			(unsigned long)perf_dr_empty,
			(unsigned long)perf_walk_spr,
			(unsigned long)perf_walk_iter);
		perf_dr_begin = perf_dr_sprites = perf_dr_fix = 0;
		perf_dr_cache = perf_dr_rspq = 0;
		perf_dr_tiles = perf_dr_cells = perf_dr_empty = 0;
		perf_walk_spr = perf_walk_iter = 0;

		// [PERF3]: C-ROM cache-miss split (roms.c), RDP busy fractions
		// from the free-running 24-bit DPC counters (delta per window,
		// wrap-safe at >=4fps; dppipe/dpclk ~= RDP pipe busy fraction,
		// dptmem = TMEM loads) and ADPCM V-ROM refills.
		{
			extern uint32_t perf_dr_miss, perf_dr_missticks;
			static uint32_t dpc_clk0, dpc_pipe0, dpc_tmem0;
			uint32_t clk  = *(volatile uint32_t*)0xA4100010 & 0xFFFFFF;
			uint32_t pipe = *(volatile uint32_t*)0xA4100018 & 0xFFFFFF;
			uint32_t tmem = *(volatile uint32_t*)0xA410001C & 0xFFFFFF;
			extern uint32_t perf_vrom_reads, perf_vrom_ticks;
			framef("[PERF3] miss=%lu dmat=%lu dpclk=%lu dppipe=%lu dptmem=%lu vrom=%lu vromt=%lu\n",
				(unsigned long)perf_dr_miss,
				(unsigned long)(perf_dr_missticks / fb),
				(unsigned long)((clk  - dpc_clk0)  & 0xFFFFFF),
				(unsigned long)((pipe - dpc_pipe0) & 0xFFFFFF),
				(unsigned long)((tmem - dpc_tmem0) & 0xFFFFFF),
				(unsigned long)perf_vrom_reads,
				(unsigned long)(perf_vrom_ticks / fb));
			perf_vrom_reads = perf_vrom_ticks = 0;
			dpc_clk0 = clk; dpc_pipe0 = pipe; dpc_tmem0 = tmem;
			perf_dr_miss = perf_dr_missticks = 0;
		}
	}
	#endif
	#ifdef MVS64_OPHIST
	// Exact per-opcode execution histogram (bumped in m64k_asm.S's
	// dispatch). Every 300 frames: dump every opcode above ~0.05% of
	// the interval's executed instructions, then reset. Offline
	// analysis: tools/analyze-ophist.py.
	if ((g_frame % 300) == 299) {
		uint64_t total = 0;
		for (int i = 0; i < 65536; i++) total += m64k_ophist_tab[i];
		uint32_t thresh = (uint32_t)(total / 2000);
		if (thresh < 4) thresh = 4;
		enum { OPHIST_MAX = 384 };
		static uint16_t sel_op[OPHIST_MAX];
		static uint32_t sel_n[OPHIST_MAX];
		int nsel = 0;
		uint64_t selected = 0;
		for (int i = 0; i < 65536 && nsel < OPHIST_MAX; i++) {
			if (m64k_ophist_tab[i] >= thresh) {
				sel_op[nsel] = (uint16_t)i;
				sel_n[nsel] = m64k_ophist_tab[i];
				selected += m64k_ophist_tab[i];
				nsel++;
			}
		}
		// insertion sort, descending by count (nsel <= 384)
		for (int i = 1; i < nsel; i++) {
			uint16_t o = sel_op[i]; uint32_t n = sel_n[i]; int j = i - 1;
			while (j >= 0 && sel_n[j] < n) {
				sel_op[j+1] = sel_op[j]; sel_n[j+1] = sel_n[j]; j--;
			}
			sel_op[j+1] = o; sel_n[j+1] = n;
		}
		framef("[OPHIST] total=%llu sel=%llu nsel=%d\n",
			(unsigned long long)total, (unsigned long long)selected, nsel);
		for (int i = 0; i < nsel; i += 8) {
			char line[160]; int p = 0;
			for (int j = i; j < nsel && j < i + 8; j++)
				p += sprintf(line + p, " %04x:%lu",
					sel_op[j], (unsigned long)sel_n[j]);
			framef("[OPH]%s\n", line);
		}
		memset(m64k_ophist_tab, 0, sizeof(m64k_ophist_tab));
	}
	#endif
	#ifdef MVS64_IDLEPROBE
	{
		extern uint32_t idle_probe_found;
		if (idle_probe_found) {
			debugf("[IDLEPROBE] long spin at 68k pc=%06lx\n",
				(unsigned long)(idle_probe_found & 0xFFFFFF));
			idle_probe_found = 0;
		}
	}
	#endif
}
#endif
