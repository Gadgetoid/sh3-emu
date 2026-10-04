#!/bin/sh
set -e
ROM=${1:-rom/odo-sh3.bin}
OUT=${TMPDIR:-/tmp}/sh3-boot
mkdir -p "$OUT"
check() {
    name=$1; expected=$2; shift 2
    ./headless "$ROM" --pgm="$OUT/$name.pgm" "$@" 2>/dev/null
    actual=$(shasum -a 256 "$OUT/$name.pgm" | cut -d' ' -f1)
    if [ -z "$expected" ]; then echo "$name $actual"; return; fi
    if [ "$actual" = "$expected" ]; then echo "ok   $name"; else echo "FAIL $name $actual"; exit 1; fi
}
CALIBRATE="--tap=4:240:120 --tap=6:48:24 --tap=8:48:216 --tap=10:432:216 --tap=12:432:24 --key=15:5A"
if ./headless "$ROM" --debug-output --seconds=1 2>&1 | grep -q "^debug: Windows CE Kernel for Hitachi SH"; then echo "ok   debug_output"; else echo "FAIL debug_output"; exit 1; fi
check calibration 32543f4dd30c39ce3e8685a55408a10d60095af60bedb3862314d5f836c54b90 --seconds=3
check desktop 644ad304df340ec56560db7229a83a2315bf3bc0a47eb74d813f0edb7e91db00 --seconds=22 $CALIBRATE
./headless "$ROM" --seconds=22 --save="$OUT/desktop.state" $CALIBRATE 2>/dev/null
check resumed 644ad304df340ec56560db7229a83a2315bf3bc0a47eb74d813f0edb7e91db00 --seconds=1 --load="$OUT/desktop.state"
check console 0c7192376347811dc03dee48e0d4b2a4943187bd492b37628a6ec1f97c2fa240 --seconds=12 --load="$OUT/desktop.state" --key=1:11+0D --tap=3:71:198:0.3 "--type=5:cmd\n" "--type=8:dir\n"
PORT=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
./headless "$ROM" --load="$OUT/desktop.state" --gdb="$PORT" > "$OUT/gdb.log" 2>&1 &
STUB=$!
if python3 tests/gdb_client.py "$PORT"; then echo "ok   gdb"; else echo "FAIL gdb"; kill $STUB; cat "$OUT/gdb.log"; exit 1; fi
wait $STUB
