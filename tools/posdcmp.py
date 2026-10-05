#!/usr/bin/env python3
# posdcmp.py A.txt B.txt [minN] — join [PERFOSD] windows on f= and compare
# per-field means (ms*10 per frame) over windows with n >= minN (fights).
import re, sys, statistics as st
def load(p):
    d = {}
    for ln in open(p, errors='replace'):
        if '[PERFOSD]' not in ln: continue
        kv = dict(re.findall(r'(\w+)=(\d+)', ln))
        d[int(kv['f'])] = {k: int(v) for k, v in kv.items()}
    return d
A, B = load(sys.argv[1]), load(sys.argv[2])
minN = int(sys.argv[3]) if len(sys.argv) > 3 else 400
common = sorted(f for f in A if f in B and A[f].get('n', 0) >= minN and B[f].get('n', 0) >= minN
                and abs(A[f]['n'] - B[f]['n']) <= A[f]['n'] // 10)
print(f'{len(common)} matched windows (n>={minN}, tile counts within 10%)')
for k in ['f10', 'a', 'm', 's', 'v', 'r', 'q', 'e', 'c', 'l', 'b', 'n']:
    if k not in A[common[0]]: continue
    da = [A[f][k] for f in common]; db = [B[f][k] for f in common]
    dd = [b - a for a, b in zip(da, db)]
    print(f'{k:>4}: A {st.mean(da):7.1f}  B {st.mean(db):7.1f}  B-A mean {st.mean(dd):+6.1f} median {st.median(dd):+6.1f}')
