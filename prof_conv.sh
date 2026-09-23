#!/bin/bash
cd /root/N64-NEOGEO/mvs64 || exit 1
for f in "$@"; do
  convert "shot_$f.bmp" "prof_s$f.png" && echo "conv $f ok" || echo "conv $f FAIL"
done
ls -la prof_s*.png
