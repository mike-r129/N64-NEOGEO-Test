#!/bin/bash
# Generate a compact input script (<256 events) that boots UniBIOS, inserts a
# coin, starts, confirms a character, and then mashes attacks + movement during
# the match. Kept economical so all events fit the emu's HL_MAX_EVENTS=256 cap.
OUT=/root/N64-NEOGEO/input_prof.txt
{
  echo "# boot -> coin -> start -> char select -> match (compact)"
  # two coins during title
  echo "540 560 coin"
  echo "580 600 coin"
  # press start several times to advance title/intro -> mode/char select
  for f in 660 780 900 1020 1140 1260 1380; do echo "$f $((f+8)) start"; done
  # character select: confirm with A a few times, wiggle right between
  for f in 1500 1620 1740 1860; do echo "$f $((f+8)) a"; echo "$((f+40)) $((f+55)) right"; done
  # extra start to begin the round
  echo "1980 1990 start"
  echo "2050 2060 start"
  # IN MATCH: alternate attacks + movement, ~48-frame period, 2200..5600
  k=0
  for f in $(seq 2200 48 5600); do
    case $((k % 3)) in
      0) atk=a;; 1) atk=b;; 2) atk=c;; esac
    echo "$f $((f+10)) $atk"
    case $((k % 2)) in
      0) echo "$((f+12)) $((f+30)) right";; 1) echo "$((f+12)) $((f+30)) left";; esac
    k=$((k+1))
  done
} > "$OUT"
echo "wrote $(wc -l < "$OUT") lines to $OUT"
head -20 "$OUT"
