#!/bin/sh
# Build the Musashi-based ADDQ/SUBQ vector generator and emit gzipped
# TomHarte-schema vectors into this assets/ directory, alongside the
# downloaded TomHarte *.json.gz files. Run mktest.go afterwards to convert
# every *.json.gz (including these) into the *.btest files the ROM loads.
#
#   cd m64k/assets && ./mkquick.sh && go run mktest.go
#
# The generated ADDQ.json.gz / SUBQ.json.gz (and *.btest) are gitignored;
# only this script and mkquick.c are tracked.
set -e
cd "$(dirname "$0")"

CC=${CC:-gcc}
COUNT=${COUNT:-600}

echo "building mkquick..." >&2
$CC -O2 -std=c11 -Wall -Wno-unused-function \
    -o mkquick mkquick.c ../../m68kcpu.c ../../m68kops.c ../../m68kdasm.c -lm

echo "generating ADDQ.json.gz ($COUNT cases)..." >&2
./mkquick ADDQ "$COUNT" | gzip -9 > ADDQ.json.gz
echo "generating SUBQ.json.gz ($COUNT cases)..." >&2
./mkquick SUBQ "$COUNT" | gzip -9 > SUBQ.json.gz
echo "done." >&2
