#!/usr/bin/env python3
"""Aggregate [OPHIST]/[OPH] dumps from an MVS64_OPHIST ares run.

For each executed opcode: decode to a 68k mnemonic+form, classify whether it
hits an existing M64K_FASTPATHS inline path, and print a ranked table with
cumulative coverage. The point: size the headroom for a predecoded/threaded
dispatch (i.e. what fraction of executed instructions still take the generic
decode_ea/check_cc path, and which forms dominate it).

Usage: analyze-ophist.py ares-run.txt [--tail N]   (--tail: only last N intervals)
"""
import re, sys
from collections import defaultdict

EA_MODES = ["Dn", "An", "(An)", "(An)+", "-(An)", "(d16,An)", "(d8,An,Xn)", "ext"]
EA7 = ["(xxx).W", "(xxx).L", "(d16,PC)", "(d8,PC,Xn)", "#imm", "?", "?", "?"]
SIZES = {1: "b", 3: "w", 2: "l"}  # MOVE size field encoding
CONDS = ["T","F","HI","LS","CC","CS","NE","EQ","VC","VS","PL","MI","GE","LT","GT","LE"]

def ea_name(mode, reg):
    if mode == 7:
        return EA7[reg]
    return EA_MODES[mode]

def decode(op):
    """Return (mnemonic-with-form, fastpath_bool)."""
    top = op >> 12
    if top in (1, 2, 3):  # MOVE.b/.l/.w
        sz = SIZES[top]
        sm, sr = (op >> 3) & 7, op & 7
        dm, dr = (op >> 6) & 7, (op >> 9) & 7
        src, dst = ea_name(sm, sr), ea_name(dm, dr)
        name = f"MOVE.{sz} {src},{dst}"
        fast = False
        if sz == "w":
            if src == "Dn" and dst in ("Dn", "(An)", "(An)+", "(d16,An)"):
                fast = True
            if src == "(d16,An)" and dst == "Dn":
                fast = True
            if src == "#imm" and dst == "(An)":
                fast = True
        elif sz == "b":
            if src in ("(d16,An)", "(An)+") and dst == "Dn":
                fast = True
        elif sz == "l":
            if src in ("Dn", "An") and dst in ("Dn", "An"):
                fast = True
            if src == "(d16,An)" and dst in ("Dn", "An"):
                fast = True
        return name, fast
    if top == 6:
        cond = (op >> 8) & 0xF
        disp8 = op & 0xFF
        w = ".w" if disp8 == 0 else ".s"
        if cond == 1:
            return f"BSR{w}", False
        name = "BRA" if cond == 0 else "B" + CONDS[cond]
        return f"{name}{w}", True
    if top == 7:
        return "MOVEQ", False
    if top == 5:
        m, r = (op >> 3) & 7, op & 7
        if ((op >> 6) & 3) == 3:
            if m == 1:
                cond = (op >> 8) & 0xF
                # DBF/DBRA (cond=F) has the fast pre-check; other DBcc generic
                return f"DB{CONDS[cond]}", cond == 1
            return f"S{CONDS[(op>>8)&0xF]} {ea_name(m,r)}", False
        sz = ["b", "w", "l"][(op >> 6) & 3]
        kind = "SUBQ" if op & 0x100 else "ADDQ"
        fast = (m == 0)
        return f"{kind}.{sz} #,{ea_name(m,r)}", fast
    if top == 4:
        if (op & 0xFC0) == 0xC0 or (op & 0x1C0) == 0x1C0:  # LEA/CHK area
            if (op & 0x1C0) == 0x1C0:
                return f"LEA {ea_name((op>>3)&7, op&7)},An", False
        sub = (op >> 6) & 0xF
        m, r = (op >> 3) & 7, op & 7
        subnames = {0x0:"NEGX", 0x2:"CLR", 0x4:"NEG", 0x6:"NOT",
                    0x8:"misc48", 0xA:"TST", 0xC:"MOVEM"}
        if (op & 0xF00) == 0xA00:
            szf = (op >> 6) & 3
            if szf == 3:
                return f"TAS {ea_name(m,r)}", False
            sz = ["b", "w", "l"][szf]
            fast = sz in ("b", "w") and ea_name(m, r) == "(d16,An)"
            return f"TST.{sz} {ea_name(m,r)}", fast
        if (op & 0xFF8) == 0xE50:
            return "LINK", False
        if (op & 0xFF8) == 0xE58:
            return "UNLK", False
        if (op & 0xFC0) == 0xE80:
            return f"JSR {ea_name(m,r)}", False
        if (op & 0xFC0) == 0xEC0:
            return f"JMP {ea_name(m,r)}", False
        if op == 0x4E75:
            return "RTS", False
        if op == 0x4E73:
            return "RTE", False
        if (op & 0xFB80) == 0x4880 and ((op >> 6) & 1) == 0 or (op & 0xB80) == 0x880:
            pass  # fallthrough label soup; keep generic naming below
        name = subnames.get(sub & 0xE, f"grp4[{sub:x}]")
        return f"{name} {ea_name(m,r)}", False
    if top == 0:
        if op & 0x100 or (op & 0xF00) == 0x800:
            kind = ["BTST", "BCHG", "BCLR", "BSET"][(op >> 6) & 3]
            imm = "" if op & 0x100 else " #"
            return f"{kind}{imm} {ea_name((op>>3)&7, op&7)}", False
        kind = {0x0:"ORI", 0x2:"ANDI", 0x4:"SUBI", 0x6:"ADDI",
                0xA:"EORI", 0xC:"CMPI"}.get((op >> 9) & 0xF, "grp0")
        sz = ["b", "w", "l", "?"][(op >> 6) & 3]
        m, r = (op >> 3) & 7, op & 7
        fast = kind == "CMPI" and m == 0 and sz in ("b", "w", "l")
        return f"{kind}.{sz} #,{ea_name(m,r)}", fast
    if top in (0x8, 0x9, 0xB, 0xC, 0xD):
        names = {0x8: "OR", 0x9: "SUB", 0xB: "CMP", 0xC: "AND", 0xD: "ADD"}
        kind = names[top]
        opmode = (op >> 6) & 7
        m, r = (op >> 3) & 7, op & 7
        if opmode in (3, 7):
            return f"{kind}A.{'w' if opmode==3 else 'l'} {ea_name(m,r)},An", False
        sz = ["b", "w", "l"][opmode & 3]
        d = "ea,Dn" if opmode < 4 else "Dn,ea"
        src = ea_name(m, r)
        fast = kind == "ADD" and opmode < 4 and m == 0  # ADD.x Dn,Dm
        if top == 0xB and opmode >= 4 and m == 1:
            return f"CMPM.{sz}", False
        return f"{kind}.{sz} {src} {d}", fast
    if top == 0xE:
        szf = (op >> 6) & 3
        if szf == 3:
            return "SHIFTmem", False
        kind = ["AS", "LS", "ROX", "RO"][(op >> 3) & 3]
        d = "L" if op & 0x100 else "R"
        imr = "Dn" if op & 0x20 else "#"
        return f"{kind}{d}.{['b','w','l'][szf]} {imr},Dn", False
    if top == 0xA:
        return "LINE-A", False
    if top == 0xF:
        return "LINE-F", False
    return f"op{op:04x}", False

def main():
    path = sys.argv[1]
    tail = 0
    if "--tail" in sys.argv:
        tail = int(sys.argv[sys.argv.index("--tail") + 1])
    intervals = []       # list of dict opcode->count
    cur = None
    oph_re = re.compile(r"\[OPH\]((?: [0-9a-f]{4}:\d+)+)")
    hdr_re = re.compile(r"\[OPHIST\] total=(\d+) sel=(\d+) nsel=(\d+)")
    totals = []
    with open(path, errors="replace") as f:
        for line in f:
            m = hdr_re.search(line)
            if m:
                cur = {}
                intervals.append(cur)
                totals.append((int(m.group(1)), int(m.group(2))))
                continue
            m = oph_re.search(line)
            if m and cur is not None:
                for tok in m.group(1).split():
                    o, n = tok.split(":")
                    cur[int(o, 16)] = cur.get(int(o, 16), 0) + int(n)
    if tail:
        intervals = intervals[-tail:]
        totals = totals[-tail:]
    if not intervals:
        print("no [OPHIST] intervals found")
        return
    agg = defaultdict(int)
    for d in intervals:
        for o, n in d.items():
            agg[o] += n
    grand_total = sum(t for t, _ in totals)
    grand_sel = sum(s for _, s in totals)
    print(f"intervals={len(intervals)} total_insns={grand_total} "
          f"selected={grand_sel} ({grand_sel/grand_total*100:.1f}% of executed)")

    # per-form aggregation
    forms = defaultdict(lambda: [0, 0, set()])   # name -> [count, fastcount, opcodes]
    fast_total = 0
    for o, n in agg.items():
        name, fast = decode(o)
        forms[name][0] += n
        if fast:
            forms[name][1] += n
            fast_total += n
    print(f"fast-path coverage of selected: {fast_total/grand_sel*100:.1f}%  "
          f"(lower bound vs ALL executed: {fast_total/grand_total*100:.1f}%)")
    print(f"{'form':<34}{'%exec':>7}{'cum%':>7}  {'fast':>5}")
    ranked = sorted(forms.items(), key=lambda kv: -kv[1][0])
    cum = 0.0
    for name, (n, nf, _) in ranked[:60]:
        pct = n / grand_total * 100
        cum += pct
        tag = "FAST" if nf == n and n else ("part" if nf else "-")
        print(f"{name:<34}{pct:>6.2f}%{cum:>6.1f}%  {tag:>5}")

if __name__ == "__main__":
    main()
