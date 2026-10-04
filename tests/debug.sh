#!/bin/sh
set -e
ROM=${1:-rom/odo-sh3-debug.bin}
OUT=${TMPDIR:-/tmp}/sh3-debug
mkdir -p "$OUT"
if [ ! -f "$ROM" ]; then echo "skip debug tests: no $ROM (an Odo image with debugmgr.exe in \\Windows)"; exit 0; fi
CALIBRATE="--tap=4:240:120 --tap=6:48:24 --tap=8:48:216 --tap=10:432:216 --tap=12:432:24 --key=15:5A"
./headless "$ROM" --seconds=30 $CALIBRATE --key=20:11+0D --key=22:11+2D "--type=24:debugmgr\n" --save="$OUT/debugmgr.state" 2>/dev/null
PORT=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
./headless "$ROM" --load="$OUT/debugmgr.state" --gdb="$PORT" > "$OUT/gdb.log" 2>&1 &
STUB=$!
if python3 tests/gdb_client.py "$PORT" run; then echo "ok   gdb_run"; else echo "FAIL gdb_run"; kill $STUB; cat "$OUT/gdb.log"; exit 1; fi
kill $STUB 2>/dev/null || true
