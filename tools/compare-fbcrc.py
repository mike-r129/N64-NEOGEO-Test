#!/usr/bin/env python3
"""Pixel gate compare: [FBCRC] <frame> <crc> streams from two DET ares runs.

Frames are matched by index (DET content is deterministic from boot); the
verdict is over the common prefix. Any mismatch is a gate failure.
"""
import re, sys

def load(path):
    crcs = {}
    rx = re.compile(r"\[FBCRC\] (\d+) ([0-9a-fA-F]{8})")
    with open(path, errors="replace") as f:
        for line in f:
            m = rx.search(line)
            if m:
                crcs[int(m.group(1))] = m.group(2).lower()
    return crcs

a = load(sys.argv[1])
b = load(sys.argv[2])
common = sorted(set(a) & set(b))
if not common:
    print("FBCRC-GATE: VACUOUS (no common frames)")  # vacuous-gate law
    sys.exit(2)
bad = [f for f in common if a[f] != b[f]]
print(f"frames: A={len(a)} B={len(b)} common={len(common)} "
      f"range {common[0]}..{common[-1]}")
if bad:
    f0 = bad[0]
    print(f"FBCRC-GATE: FAIL — {len(bad)} differing frames, "
          f"first @{f0}: {a[f0]} vs {b[f0]}")
    sys.exit(1)
print(f"FBCRC-GATE: PASS — {len(common)} frames pixel-identical")
