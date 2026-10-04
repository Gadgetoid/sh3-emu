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
card_read() {
    if [ "$(uname -s)" = Darwin ]; then
        mount=$(hdiutil attach -readonly -imagekey diskimage-class=CRawDiskImage "$1" | awk '/SH3CARD/ {sub(/^.*\t/, ""); print}')
        cat "$mount/$2"
        hdiutil detach -quiet "$mount"
    else
        mtype -i "$1@@512" "::$2"
    fi
}
rm -rf "$OUT/cardsrc" && mkdir -p "$OUT/cardsrc/data" && echo "written on the host, copied by CE" > "$OUT/cardsrc/data/hello.txt"
sh tools/mkcard.sh "$OUT/card.img" 16 "$OUT/cardsrc/data"
./headless "$ROM" --load="$OUT/desktop.state" --card="$OUT/card.img" --seconds=18 --key=2:11+0D --tap=4:71:198:0.3 "--type=6:cmd\n" \
    '--type=9:md "\Storage Card\fromce"\n' '--type=12:copy "\Storage Card\data\hello.txt" "\Storage Card\fromce\copy.txt"\n' > /dev/null 2>&1
if [ "$(card_read "$OUT/card.img" fromce/copy.txt)" = "written on the host, copied by CE" ]; then echo "ok   card"; else echo "FAIL card"; exit 1; fi
./headless "$ROM" --load="$OUT/desktop.state" --seconds=8 --key=2:11+0D --key=4:11+2D "--type=6:debugmgr\n" --save="$OUT/debugmgr.state" > /dev/null 2>&1
PORT=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
./headless "$ROM" --load="$OUT/debugmgr.state" --gdb="$PORT" > "$OUT/gdb_run.log" 2>&1 &
STUB=$!
if python3 tests/gdb_client.py "$PORT" run; then echo "ok   gdb_run"; else echo "FAIL gdb_run"; kill $STUB; cat "$OUT/gdb_run.log"; exit 1; fi
kill $STUB 2>/dev/null || true
PROGRAM=${PPFS_PROGRAM:-rom/mbtest.exe}
if [ -f "$PROGRAM" ]; then
    rm -rf "$OUT/folder" && mkdir -p "$OUT/folder" && cp "$PROGRAM" "$OUT/folder/"
    name=$(basename "$PROGRAM" .exe)
    if ./headless "$ROM" --load="$OUT/desktop.state" --folder="$OUT/folder" --debug-output --seconds=8 --key=2:11+0D --key=4:11+2D "--type=6:$name\n" 2>&1 | grep -q "^ppfs: opened .*$name.exe"; then echo "ok   ppfs"; else echo "FAIL ppfs"; exit 1; fi
else
    echo "skip ppfs: no $PROGRAM (an SH3 program that isn't in ROM)"
fi
