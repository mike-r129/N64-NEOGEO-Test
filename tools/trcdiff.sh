#!/bin/bash
# Compare two [TRCRC] streams frame-by-frame; print first divergence.
# Usage: trcdiff.sh <ref.txt> <test.txt>
a=$(mktemp); b=$(mktemp)
grep -o '\[TRCRC\] f=[0-9]* crc=[0-9a-f]*' "$1" > "$a"
grep -o '\[TRCRC\] f=[0-9]* crc=[0-9a-f]*' "$2" > "$b"
na=$(wc -l < "$a"); nb=$(wc -l < "$b")
n=$((na < nb ? na : nb))
head -n "$n" "$a" > "$a.t"; head -n "$n" "$b" > "$b.t"
if cmp -s "$a.t" "$b.t"; then
  echo "TRCRC IDENTICAL over $n frames (ref $na, test $nb)"
else
  echo "TRCRC DIVERGES (ref $na, test $nb frames):"
  diff "$a.t" "$b.t" | head -8
fi
rm -f "$a" "$b" "$a.t" "$b.t"
