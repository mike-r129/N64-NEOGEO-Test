#!/bin/bash
cd /mnt/c/Users/Mike/Desktop/N64-NEOGEO || exit 1
D=mvs64/disgame
echo "=== spin loop 0x1424-0x1440 ==="; $D 1424 1440
echo "=== fill loop 0x3358-0x3360 ==="; $D 3358 3360
echo "=== fill loop 0x32c0-0x32c8 ==="; $D 32c0 32c8
echo "=== fill loop 0x33ac-0x33b4 ==="; $D 33ac 33b4
echo "=== sprite loop 0x317f8-0x31810 ==="; $D 317f8 31810
