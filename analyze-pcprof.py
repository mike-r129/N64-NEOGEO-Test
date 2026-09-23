#!/usr/bin/env python3
"""Symbolize MVS64_PCPROF dumps ([PCPHDR]/[PCP]) against an nm listing.

usage: analyze-pcprof.py <ares-log> <nm-output.txt> [topN]
  nm-output.txt = `mips64-elf-nm -n -S <elf>` of the SAME build.
Prints: per-function sample share (in-fight windows summed), per-TU/object
grouping by symbol prefix, and icache-set (16KB direct, 32B lines) conflict
heat: for each set, the heat of the hottest two distinct functions mapping
there (min of the two = a crude conflict score)."""
import re, sys, bisect, collections

log, nmf = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 60

syms = []
for ln in open(nmf):
    p = ln.split()
    if len(p) < 3: continue
    try: a = int(p[0], 16)
    except ValueError: continue
    if p[-2].lower() not in ('t', 'w'): continue
    syms.append((a & 0xFFFFFFFF, p[-1]))
syms.sort()
addrs = [a for a, _ in syms]

def sym(a):
    i = bisect.bisect_right(addrs, a) - 1
    return syms[i][1] if i >= 0 else '?'

base = None
hist = collections.Counter()
total = other = 0
for ln in open(log, errors='replace'):
    m = re.search(r'\[PCPHDR\].*base=([0-9a-f]+) frames=(\d+) samples=(\d+) other=(\d+)', ln)
    if m:
        base = int(m.group(1), 16); total += int(m.group(3)); other += int(m.group(4))
        continue
    if ln.startswith('[PCP]') and base is not None:
        for tok in ln[5:].split():
            li, c = tok.split(':')
            hist[base + (int(li, 16) << 5)] += int(c, 16)

intext = sum(hist.values())
print(f'samples={total} in-text={intext} other(non-text EPC)={other}')
fn = collections.Counter()
for a, c in hist.items(): fn[sym(a)] += c
print(f'\n== top {top} functions ==')
cum = 0
for name, c in fn.most_common(top):
    cum += c
    print(f'{100*c/intext:6.2f}% {100*cum/intext:6.1f}%  {name}')

# icache set conflicts
sets = collections.defaultdict(collections.Counter)
for a, c in hist.items(): sets[(a >> 5) & 511][sym(a)] += c
conf = []
for s, cnt in sets.items():
    mc = cnt.most_common(2)
    if len(mc) == 2: conf.append((mc[1][1], s, mc))
conf.sort(reverse=True)
print('\n== top icache set conflicts (2nd-hottest fn heat) ==')
for sc, s, mc in conf[:40]:
    print(f'set {s:3d}: ' + '  '.join(f'{n}={c}' for n, c in mc))
tot_conf = sum(x[0] for x in conf)
print(f'\nconflict mass (sum 2nd-hottest per set) = {tot_conf} ({100*tot_conf/intext:.1f}% of in-text)')
