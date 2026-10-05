# M64K Dynarec Phase-2 Emitter — Template Spec (2026-08-07)

Source-verified contracts for the per-form emitter, transcribed from
m64k_asm.S so the emitter session codes against this file instead of
re-deriving. Phase-1 infra: 4dd7c9e (arena/table/probes/EPC), 815a96a
(DYNSTAT; coverage: translate P_ROM minus page 0, plus BIOS 0xC0xxxx;
PBROM window has ZERO targets — stays untranslated).

## Block ABI (all verified against dispatch_body, m64k_asm.S ~:374)

Entry: from `dyn_probe_hit` (way-resolved `lw t0, host(t2); jr t0` — jr t0
is delay-slot-resolver-legal). At entry m_pc is the mapped host pointer of
the block head, pc_diff is current, and NO state has been modified by the
probe. Live per interpreter convention: ctx=a0, m_cycles=a1 (LIVE — the
TLB handler clamps/reads saved a1), mmap_mask=a2, rmw_cyctable=a3 (=0 at
TIMING_ACCURACY==0 outside RMW), lax_base=v0, zx64_mask=v1, flags s6/s7/s8
current, m_pc=t5. t0-t3, t6-t9, ra are scratch between instructions.

- **C_max gate** (first emitted insns): bail unless m_cycles > C_max
  (= sum of all per-insn charges in the block):
  `slti t0, a1, C_max+1; bnez t0, bail; nop` where bail: `j main_loop; nop`.
  Nothing is modified before the gate, so bail re-dispatches the same insn
  generically. The interpreter's per-insn model is `blez m_cycles ->
  main_loop_exit` BEFORE each insn (dispatch_body:377): a block whose every
  insn was pre-paid by the gate executes exactly the insns the interpreter
  would have; the (0, C_max] window falls to the interpreter. DBF back-edge
  re-entry re-tests the gate.
- **Charges**: `addiu a1, a1, -c` emitted at the template position where
  the fast path places use_cycles (BEFORE the faulting access — e.g.
  movew_fanp_dn charges 8 before lhu, movew_f_dst_d16 charges 12 before
  sh). A mid-block MMIO fault then observes the exact interpreter a1.
- **Exits**: every exit leaves m_pc = host pointer of the next insn to
  execute and jumps `j main_loop` (same 0x80xxxxxx 256MB segment as the
  arena — J-type reaches; bake main_loop's address at init). Straight-line
  tail: `addiu t5, t5, 2*ninsns` before the j (or fold into per-insn m_pc
  advances for PC-relative forms — pick ONE convention per form and bake
  displacement reads at translate time instead of runtime lh from code).
- **ADDRERR/IR rule**: dispatch stores `sw opcode, M64K_OFF_IR(ctx)` per
  insn on ADDRERR builds (dispatch_body:393). Emitted blocks do NOT store
  IR; instead EVERY address-error-capable access keeps the fast-path bail:
  `andi $1, addr_reg, 1; bnez $1, bail_here` with NOTHING mutated yet,
  where bail_here sets m_pc back to THIS insn's host pointer and
  `j main_loop` — the generic replay stores IR itself and raises the error
  bit-exactly (same contract as check_addr_bail, m64k_asm.S:433).
- **dptr (s2) law**: templates that skip a generic which would have set
  dptr must leave dptr a valid EVEN host pointer if any later generic can
  read it stale (the wave-3 stale-dptr crash class). Register-only and
  MOVE templates below don't touch dptr — safe.
- **MMIO canonical forms** (machine-enforced): loads MUST target t0
  (tlb_readhwio SAFE_MODE, hw_n64.S — RT==8 else crash screen; proven by
  DYNTEST), byte `lbu t0`, word `lhu t0` (`lh t0` allowed), long
  `lwl t0,0(p); lwr t0,3(p)`. Stores MUST come from result=t6: `sb/sh
  result` and long `swl result,0(p); swr result,3(p)`. map_m68k at
  MEMORY_BASE=0xFF000000 is ONE insn: `or dst, src, a2`.
- **Delay-slot law**: no memory access that can fault in any indirect-jump
  delay slot except after jr t0/t1/t3, jalr t7, jr ra (hw_n64.S resolver).
  Simplest: emitted delay slots are nop or non-faulting ALU.

## Flag encodings (bit-exact idioms lifted from handlers)

flag_nv (s6): bit31 = N, bit63 = N^V. flag_zc (s7): low 32 = !Z value
(zero iff Z), bit32 = C. flag_x (s8): bit0 = X.

- MOVE.w result r (V=C=0, X unchanged):
  `move s7, r` (r zero-extended 16-bit => bit32=0) ; `sll s6, r, 16`
  (32-bit sll sign-extends: bit31=bit63=N). [movew_fanp_dn:2970]
- MOVE.b: byte in r: `andi s7, r, 0xFF`; `sll s6, r, 24` — verify against
  the W3 MOVE.b bodies before use.
- MOVE.l result r: `move s7, r`?? — NO: bit32 must be 0, so use
  `zx: and s7, r, v1` if r may be sign-extended; check the W3 MOVE.l body.
- MOVEQ #i,Dn (op_moveq:3147, charge 4): value = (int32)(int8)i, baked:
  `addiu a1,-4; daddiu s6, zero, sx8(i); ori s7, zero, i&0xFF;
   sw s6, DREGS+4n(a0)` — flag_zc is the zero-extended BYTE (matches
  `andi flag_zc, opcode, 0xFF`), Dn gets the sign-extended long.

## First template set (from the debugged fast paths, preambles collapsed)

Registers/immediates baked at translate time; each entry lists source
label + charge placement:

1. MOVEQ — above. 5 insns, no fault possible.
2. MOVE.w (An)+,Dn [movew_fanp_dn:2955, 8 cycles]:
   `lw t1, AREGS+4s(a0); [addr bail t1]; addiu t2,t1,2;
    sw t2, AREGS+4s(a0); or t3,t1,a2; addiu a1,-8; lhu t0,0(t3);
    sh t0, DREGS+4d+2(a0); move s7,t0; sll s6,t0,16`
   NOTE: post-increment commits BEFORE the load (ea_011 order) — a fault
   on the load must see the incremented An. Bail BEFORE the sw.
3. MOVE.w (d16,An),Dn [movew_fsrc_d16:2974, 12 cycles]: d16 baked from
   code stream at translate time (`lh t2,0(m_pc)` collapses to an addiu
   constant); m_pc advance 4 total.
4. MOVE.w Dn,(d16,An) [movew_f_dst_d16:2873, 12]: charge 12 before sh;
   flags from the SOURCE value before the store.
5. MOVE.w #imm,(An) [movew_fsrc_other tail:2904, 12]: imm baked.
6. MOVE.w (An)+,(An) [movew_fsrc_anp:2925, 8+4]: same-register dst
   re-read AFTER src writeback — keep both AREGS reads in emitted order.
7. TST/CLR/LEA/ADDA/CMP/ADDQ/JSR/BSR: transcribe from the wave-3 W3_FULL
   bodies (removed from the tree in the cleanup branch; read them at
   c11ed5b:m64k/m64k_asm.S, #ifdef M64K_W3_FULL) — they were
   testsuite-covered and charge/flag-exact. Do NOT invent sequences.

Control transfers END a block in phase 2a (no emitted branches). A block
is: head + maximal run of supported straight-line forms; stop at the
first unsupported/control/RMW form; minimum profitable length ~3 insns
(else skip translation — probe overhead already paid).

## Translation trigger + forced testsuite mode

- Game (phase 2a): cold `dyn_miss_note` at the jmp_exec probe MISS edge
  (under M64K_DYNREC): write target PC to a 1-slot mailbox (sw only, 3
  insns). C side (m64k_run loop, slice boundary — reclamation law) reads
  mailbox, translates once (dedup via the table itself + a tried-set so
  untranslatable heads aren't retried), inserts via __m64k_dyn_publish
  THEN table-tag write (publish-ordering law). fps-gate the mailbox cost.
- Testsuite: call m64k_dyn_translate(ctx, pc) before each vector run
  (slice-entry probe hits it). Every vector then exercises gate/entry/
  exit/bail paths. Gate: 125/126 + cycle diff 2.67% unchanged.
- 2-way insert: prefer empty way (tag 0xFFFFFFFF), else evict way with
  the OLDER insertion stamp (u8 round-robin per set is fine phase-2a).

## Gates per increment (unchanged from blueprint)

Testsuite 125/126 both knob states; TRCRC stream identical vs interpreter
build same-inputs (this catches flag/charge drift the testsuite's isolated
vectors can't); [DYNTEST] PASS; ares 480s stability + bucket fps;
per-block [DYNSTAT]-style counters for translated-block hit rate.
