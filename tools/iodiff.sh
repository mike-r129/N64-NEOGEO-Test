#!/bin/bash
# Compare two [IO] MMIO-read streams by (addr,value) SEQUENCE; print the first
# divergence with context. The f= tag is display-only: even the guest-frame
# key can straddle log-start edges, so alignment is by stream index.
# Usage: iodiff.sh <ref.txt> <test.txt>
a=$(mktemp); b=$(mktemp); af=$(mktemp); bf=$(mktemp)
grep -o '\[IO\] f=[0-9]* a=[0-9a-f]* v=[0-9a-f]*' "$1" > "$af"
grep -o '\[IO\] f=[0-9]* a=[0-9a-f]* v=[0-9a-f]*' "$2" > "$bf"
sed 's/f=[0-9]* //' "$af" > "$a"
sed 's/f=[0-9]* //' "$bf" > "$b"
na=$(wc -l < "$a"); nb=$(wc -l < "$b")
n=$((na < nb ? na : nb))
echo "IO reads: ref=$na test=$nb (comparing first $n by addr+value)"
first=$(cmp <(head -n "$n" "$a") <(head -n "$n" "$b") 2>/dev/null | grep -o 'line [0-9]*' | grep -o '[0-9]*')
if [ -z "$first" ]; then
  echo "IO STREAMS IDENTICAL over $n reads"
else
  echo "FIRST DIVERGENT READ at stream index $first:"
  echo "--- ref (with frame tags) ---";  sed -n "$((first>6?first-6:1)),$((first+4))p" "$af"
  echo "--- test (with frame tags) ---"; sed -n "$((first>6?first-6:1)),$((first+4))p" "$bf"
fi
rm -f "$a" "$b" "$af" "$bf"
